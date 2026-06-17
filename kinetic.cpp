#include "kinetic.hpp"
#include "globals.hpp"
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <chrono>
#include <cmath>
#include <string>
#include <fstream>
#include <cstdint>
#include <cctype>

static bool classLooksLikeBrowser(std::string cls) {
    for (auto& c : cls)
        c = std::tolower(static_cast<unsigned char>(c));

    return cls.find("firefox") != std::string::npos || cls.find("chrom") != std::string::npos || cls.find("brave") != std::string::npos ||
           cls.find("vivaldi") != std::string::npos || cls.find("opera") != std::string::npos || cls.find("librewolf") != std::string::npos ||
           cls.find("zen") != std::string::npos;
}

struct SScrollTargetKeys {
    uintptr_t windowKey  = 0;
    uintptr_t surfaceKey = 0;
};

static SScrollTargetKeys currentScrollTargetKeys() {
    SScrollTargetKeys out;

    if (g_pInputManager) {
        const auto PWIN = g_pInputManager->m_lastMouseFocus.lock();
        out.windowKey   = PWIN ? reinterpret_cast<uintptr_t>(PWIN.get()) : 0;
    }

    if (g_pSeatManager) {
        const auto PSURF = g_pSeatManager->m_state.pointerFocus.lock();
        out.surfaceKey   = PSURF ? reinterpret_cast<uintptr_t>(PSURF.get()) : 0;
    }

    return out;
}

KineticState::KineticState() {
    auto* loop = g_pCompositor->m_wlEventLoop;
    m_stopTimer  = wl_event_loop_add_timer(loop, onStopTimer, this);
    m_decayTimer = wl_event_loop_add_timer(loop, onDecayTimer, this);
}

KineticState::~KineticState() {
    if (m_stopTimer)
        wl_event_source_remove(m_stopTimer);
    if (m_decayTimer)
        wl_event_source_remove(m_decayTimer);
}

static void pushToWindow(std::deque<KineticState::DeltaSample>& window, double delta, uint32_t timeMs) {
    window.push_back({delta, timeMs});
    while (window.size() > KineticState::MAX_DELTA_WINDOW)
        window.pop_front();
}

void KineticState::onAxis(IPointer::SAxisEvent& e) {
    static const CConfigValue<Config::INTEGER> PENABLED("plugin:kinetic-scroll:enabled");
    static const CConfigValue<Config::INTEGER> PDISABLE_BROWSER("plugin:kinetic-scroll:disable_in_browser");
    static const CConfigValue<Config::INTEGER> PSTOPTARGET("plugin:kinetic-scroll:stop_on_target_change");
    static const CConfigValue<Config::FLOAT>   PDELTA_MUL("plugin:kinetic-scroll:delta_multiplier");
    static const CConfigValue<Config::INTEGER> PDEBUG("plugin:kinetic-scroll:debug");
    static uint64_t s_lastNotifyMs = 0;

    if (!*PENABLED)
        return;

    const auto targetKeys = currentScrollTargetKeys();

    if (*PSTOPTARGET && m_scrollTargetWindowKey != 0) {
        const bool windowChanged  = targetKeys.windowKey != 0 && targetKeys.windowKey != m_scrollTargetWindowKey;
        const bool surfaceChanged = targetKeys.surfaceKey != 0 && targetKeys.surfaceKey != m_scrollTargetSurfaceKey;
        if (windowChanged || surfaceChanged)
            stopKinetic("targetChanged");
    }

    if (*PDISABLE_BROWSER) {
        const auto PWIN = g_pInputManager ? g_pInputManager->m_lastMouseFocus.lock() : nullptr;
        if (PWIN && classLooksLikeBrowser(PWIN->m_class)) {
            if (m_decaying)
                stopKinetic("browserFocus");
            return;
        }
    }

    // Only handle touchpad scrolling (some devices report as mouse with smooth deltas)
    const bool touchpadSource = (e.source == WL_POINTER_AXIS_SOURCE_FINGER || e.source == WL_POINTER_AXIS_SOURCE_CONTINUOUS);
    const bool smoothMouse    = (e.mouse && e.deltaDiscrete == 0);
    if (!touchpadSource && !smoothMouse)
        return;

    if (e.delta == 0.0 && e.deltaDiscrete == 0)
        return;

    const double scaledDelta = e.delta * *PDELTA_MUL;

    // New finger scroll while decaying => continue from current momentum
    const bool resumedFromDecay = m_decaying;
    if (resumedFromDecay) {
        if (*PDEBUG) {
            std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
            if (log.is_open())
                log << "[hypr-kinetic-scroll] onAxis: decaying -> resume self=" << this << "\n";
        }
        m_decaying = false;
        wl_event_source_timer_update(m_decayTimer, 0);
    }

    m_tracking               = true;
    m_scrollTargetWindowKey  = targetKeys.windowKey;
    m_scrollTargetSurfaceKey = targetKeys.surfaceKey;

    uint32_t dt = e.timeMs - m_lastEventMs;

    if (m_lastEventMs > 0 && dt > 0 && dt < 200) {
        // Accumulate into sliding window for peak-aware velocity estimation
        double& vel    = (e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL) ? m_velocityV : m_velocityH;
        auto&   window = (e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL) ? m_recentDeltasV : m_recentDeltasH;

        pushToWindow(window, scaledDelta, e.timeMs);

        // Weighted average: more weight on recent samples, with a floor at
        // the last raw delta so kinetic never undershoots perceived speed.
        double weightedSum = 0.0;
        double weightTotal = 0.0;
        double lastRaw     = window.back().delta;
        for (size_t i = 0; i < window.size(); i++) {
            double w = static_cast<double>(i + 1); // linear ramp: newest = highest weight
            weightedSum += window[i].delta * w;
            weightTotal += w;
        }
        double avgVel = weightedSum / weightTotal;
        vel = std::copysign(std::max(std::abs(avgVel), std::abs(lastRaw)), avgVel);
    } else if (resumedFromDecay) {
        // Continue inertia: add new gesture impulse on top of remaining momentum
        if (e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
            m_velocityV += scaledDelta;
        else
            m_velocityH += scaledDelta;
    } else {
        // First event or large gap - seed velocity directly
        m_recentDeltasV.clear();
        m_recentDeltasH.clear();
        if (e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
            m_velocityV = scaledDelta;
        else
            m_velocityH = scaledDelta;
    }

    m_lastEventMs = e.timeMs;

    if (*PDEBUG) {
        auto     now   = std::chrono::steady_clock::now();
        uint64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        if (nowMs - s_lastNotifyMs > 200) {
            const char* axis = e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? "v" : "h";
            std::string msg = "[hypr-kinetic-scroll] axis=" + std::string(axis) + " delta=" + std::to_string(e.delta) +
                              " source=" + std::to_string((int)e.source) + " mouse=" + std::to_string(e.mouse ? 1 : 0) +
                              " discrete=" + std::to_string(e.deltaDiscrete) + " dt=" + std::to_string(dt) +
                              " v=" + std::to_string(m_velocityV) + " h=" + std::to_string(m_velocityH) +
                              " self=" + std::to_string(reinterpret_cast<uintptr_t>(this));
            HyprlandAPI::addNotification(PHANDLE, msg, CHyprColor{0.2, 0.6, 1.0, 1.0}, 1000);
            std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
            if (log.is_open())
                log << msg << "\n";
            s_lastNotifyMs = nowMs;
        }
    }

    // Reset stop detection: if no event arrives within the configured threshold,
    // the finger has lifted. Touchpad events arrive at ~8-16ms intervals.
    static const CConfigValue<Config::INTEGER> PSTOPDELAY("plugin:kinetic-scroll:stop_delay_ms");
    wl_event_source_timer_update(m_stopTimer, *PSTOPDELAY);
    // Ensure decay timer is off while actively tracking
    wl_event_source_timer_update(m_decayTimer, 0);
}

void KineticState::stopKinetic(const char* reason) {
    static const CConfigValue<Config::INTEGER> PDEBUG("plugin:kinetic-scroll:debug");
    if (*PDEBUG) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open())
            log << "[hypr-kinetic-scroll] stopKinetic reason=" << (reason ? reason : "(null)") << " self=" << this << "\n";
    }
    m_velocityV   = 0.0;
    m_velocityH   = 0.0;
    m_tracking    = false;
    m_decaying    = false;
    m_lastEventMs           = 0;
    m_scrollTargetWindowKey  = 0;
    m_scrollTargetSurfaceKey = 0;
    m_recentDeltasV.clear();
    m_recentDeltasH.clear();
    wl_event_source_timer_update(m_stopTimer, 0);
    wl_event_source_timer_update(m_decayTimer, 0);
}

int KineticState::onStopTimer(void* data) {
    auto* self = static_cast<KineticState*>(data);

    static const CConfigValue<Config::INTEGER> PDEBUG("plugin:kinetic-scroll:debug");
    if (*PDEBUG) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open()) {
            log << "[hypr-kinetic-scroll] stopTimer tracking=" << (self->m_tracking ? 1 : 0) << " v=" << self->m_velocityV
                << " h=" << self->m_velocityH << " self=" << self << "\n";
        }
    }

    if (!self->m_tracking)
        return 0;

    static const CConfigValue<Config::FLOAT> PMINVEL("plugin:kinetic-scroll:min_velocity");

    // Only start kinetic if velocity is above threshold
    if (std::abs(self->m_velocityV) < *PMINVEL && std::abs(self->m_velocityH) < *PMINVEL) {
        self->m_tracking = false;
        return 0;
    }

    // Finger lifted - begin kinetic decay
    self->m_tracking = false;
    self->m_decaying = true;

    // Emit the first synthetic scroll immediately to bridge the gap between
    // the last real axis event and the first timer-driven decay tick.
    // Without this, there's a perceptible ~66ms stutter (50ms stop timeout +
    // 16ms interval) with zero scrolling.
    self->emitSyntheticScroll();

    // Now apply the first deceleration so the next timer tick starts decayed
    static const CConfigValue<Config::FLOAT> PDECEL("plugin:kinetic-scroll:decel");
    self->m_velocityV *= *PDECEL;
    self->m_velocityH *= *PDECEL;

    static const CConfigValue<Config::INTEGER> PINTERVAL("plugin:kinetic-scroll:interval_ms");

    wl_event_source_timer_update(self->m_decayTimer, *PINTERVAL);
    return 0;
}

int KineticState::onDecayTimer(void* data) {
    auto* self = static_cast<KineticState*>(data);

    static const CConfigValue<Config::INTEGER> PDEBUG("plugin:kinetic-scroll:debug");
    static const CConfigValue<Config::INTEGER> PDISABLE_BROWSER("plugin:kinetic-scroll:disable_in_browser");
    static const CConfigValue<Config::INTEGER> PSTOPTARGET("plugin:kinetic-scroll:stop_on_target_change");

    if (!self->m_decaying) {
        if (*PDEBUG) {
            std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
            if (log.is_open())
                log << "[hypr-kinetic-scroll] decayTimer skipped (not decaying)\n";
        }
        return 0;
    }

    const auto targetKeys = currentScrollTargetKeys();

    if (*PSTOPTARGET && self->m_scrollTargetWindowKey != 0) {
        const bool windowChanged  = targetKeys.windowKey != 0 && targetKeys.windowKey != self->m_scrollTargetWindowKey;
        const bool surfaceChanged = targetKeys.surfaceKey != 0 && targetKeys.surfaceKey != self->m_scrollTargetSurfaceKey;
        if (windowChanged || surfaceChanged) {
            self->stopKinetic("targetChangedDecay");
            return 0;
        }
    }

    if (*PDISABLE_BROWSER) {
        const auto PWIN = g_pInputManager ? g_pInputManager->m_lastMouseFocus.lock() : nullptr;
        if (PWIN && classLooksLikeBrowser(PWIN->m_class)) {
            self->stopKinetic("browserDecay");
            return 0;
        }
    }

    static const CConfigValue<Config::FLOAT>   PDECEL("plugin:kinetic-scroll:decel");
    static const CConfigValue<Config::FLOAT>   PMINVEL("plugin:kinetic-scroll:min_velocity");
    static const CConfigValue<Config::INTEGER> PINTERVAL("plugin:kinetic-scroll:interval_ms");

    // Apply deceleration
    self->m_velocityV *= *PDECEL;
    self->m_velocityH *= *PDECEL;

    bool activeV = std::abs(self->m_velocityV) >= *PMINVEL;
    bool activeH = std::abs(self->m_velocityH) >= *PMINVEL;

    if (!activeV && !activeH) {
        self->stopKinetic("decayDone");
        return 0;
    }

    self->emitSyntheticScroll();

    // Re-arm for next frame
    wl_event_source_timer_update(self->m_decayTimer, *PINTERVAL);
    return 0;
}

void KineticState::emitSyntheticScroll() {
    static const CConfigValue<Config::FLOAT> PSCROLLFACTOR("input:touchpad:scroll_factor");
    static const CConfigValue<Config::FLOAT> PMINVEL("plugin:kinetic-scroll:min_velocity");

    auto     now         = std::chrono::steady_clock::now();
    uint32_t timeMs      = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    double   scrollFactor = *PSCROLLFACTOR;

    if (std::abs(m_velocityV) >= *PMINVEL) {
        g_pSeatManager->sendPointerAxis(timeMs, WL_POINTER_AXIS_VERTICAL_SCROLL, m_velocityV * scrollFactor, 0, 0,
                                        WL_POINTER_AXIS_SOURCE_CONTINUOUS, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    }

    if (std::abs(m_velocityH) >= *PMINVEL) {
        g_pSeatManager->sendPointerAxis(timeMs, WL_POINTER_AXIS_HORIZONTAL_SCROLL, m_velocityH * scrollFactor, 0, 0,
                                        WL_POINTER_AXIS_SOURCE_CONTINUOUS, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    }

    g_pSeatManager->sendPointerFrame();
}
