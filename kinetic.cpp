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

bool KineticState::hasAppRule(const std::string& windowClass) const {
    return m_perAppRules.contains(windowClass);
}

bool KineticState::shouldProcessForWindow(const std::string& windowClass) const {
    const auto it = m_perAppRules.find(windowClass);
    if (it != m_perAppRules.end())
        return it->second;
    return m_defaultAppRule;
}

void KineticState::setAppRule(const std::string& appClass, bool enabled) {
    m_perAppRules[appClass] = enabled;
}

void KineticState::setDefaultAppRule(bool enabled) {
    m_defaultAppRule = enabled;
}

void KineticState::resetAppRules() {
    m_perAppRules.clear();
    m_defaultAppRule = true;
}

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

void KineticState::onAxis(IPointer::SAxisEvent& e) {
    static uint64_t s_lastNotifyMs = 0;

    if (!getKineticConfigInt("enabled", 1))
        return;

    const auto targetKeys = currentScrollTargetKeys();

    if (getKineticConfigInt("stop_on_target_change", 1) && m_scrollTargetWindowKey != 0) {
        const bool windowChanged = targetKeys.windowKey != 0 && targetKeys.windowKey != m_scrollTargetWindowKey;
        const bool surfaceChanged = targetKeys.surfaceKey != 0 && targetKeys.surfaceKey != m_scrollTargetSurfaceKey;
        if (windowChanged || surfaceChanged)
            stopKinetic("targetChanged");
    }

    const auto PWIN = g_pInputManager ? g_pInputManager->m_lastMouseFocus.lock() : nullptr;
    if (PWIN) {
        const bool hasRule = hasAppRule(PWIN->m_class);
        if (!shouldProcessForWindow(PWIN->m_class)) {
            if (m_decaying)
                stopKinetic("appRule");
            return;
        }

        if (getKineticConfigInt("disable_in_browser", 1) && !hasRule && classLooksLikeBrowser(PWIN->m_class)) {
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

    const double scaledDelta = e.delta * getKineticConfigFloat("delta_multiplier", 1.25);

    // New finger scroll while decaying => continue from current momentum
    const bool resumedFromDecay = m_decaying;
    if (resumedFromDecay) {
        if (getKineticConfigInt("debug", 0)) {
            std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
            if (log.is_open())
                log << "[hypr-kinetic-scroll] onAxis: decaying -> resume self=" << this << "\n";
        }
        m_decaying = false;
        wl_event_source_timer_update(m_decayTimer, 0);
    }

    m_tracking              = true;
    m_scrollTargetWindowKey = targetKeys.windowKey;
    m_scrollTargetSurfaceKey = targetKeys.surfaceKey;

    constexpr double alpha = 0.3;
    uint32_t         dt    = e.timeMs - m_lastEventMs;

    if (m_lastEventMs > 0 && dt > 0 && dt < 200) {
        // Exponential smoothing of deltas
        if (e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
            m_velocityV = alpha * scaledDelta + (1.0 - alpha) * m_velocityV;
        else
            m_velocityH = alpha * scaledDelta + (1.0 - alpha) * m_velocityH;
    } else if (resumedFromDecay) {
        // Continue inertia: add new gesture impulse on top of remaining momentum
        if (e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
            m_velocityV += scaledDelta;
        else
            m_velocityH += scaledDelta;
    } else {
        // First event or large gap - seed velocity directly
        if (e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
            m_velocityV = scaledDelta;
        else
            m_velocityH = scaledDelta;
    }

    m_lastEventMs = e.timeMs;

    if (getKineticConfigInt("debug", 0)) {
        auto     now    = std::chrono::steady_clock::now();
        uint64_t nowMs  = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        if (nowMs - s_lastNotifyMs > 200) {
            const char* axis = e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? "v" : "h";
            std::string msg = "[hypr-kinetic-scroll] axis=" + std::string(axis) +
                              " delta=" + std::to_string(e.delta) +
                              " source=" + std::to_string((int)e.source) +
                              " mouse=" + std::to_string(e.mouse ? 1 : 0) +
                              " discrete=" + std::to_string(e.deltaDiscrete) +
                              " dt=" + std::to_string(dt) +
                              " v=" + std::to_string(m_velocityV) +
                              " h=" + std::to_string(m_velocityH) +
                              " self=" + std::to_string(reinterpret_cast<uintptr_t>(this));
            HyprlandAPI::addNotification(PHANDLE, msg, CHyprColor{0.2, 0.6, 1.0, 1.0}, 1000);
            std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
            if (log.is_open())
                log << msg << "\n";
            s_lastNotifyMs = nowMs;
        }
    }

    // Reset stop detection: if no event arrives within 50ms, finger has lifted
    wl_event_source_timer_update(m_stopTimer, 50);
    // Ensure decay timer is off while actively tracking
    wl_event_source_timer_update(m_decayTimer, 0);
}

void KineticState::stopKinetic(const char* reason) {
    if (getKineticConfigInt("debug", 0)) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open())
            log << "[hypr-kinetic-scroll] stopKinetic reason=" << (reason ? reason : "(null)") << " self=" << this << "\n";
    }
    m_velocityV   = 0.0;
    m_velocityH   = 0.0;
    m_tracking    = false;
    m_decaying    = false;
    m_lastEventMs          = 0;
    m_scrollTargetWindowKey = 0;
    m_scrollTargetSurfaceKey = 0;
    wl_event_source_timer_update(m_stopTimer, 0);
    wl_event_source_timer_update(m_decayTimer, 0);
}

int KineticState::onStopTimer(void* data) {
    auto* self = static_cast<KineticState*>(data);

    if (getKineticConfigInt("debug", 0)) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open()) {
            log << "[hypr-kinetic-scroll] stopTimer tracking=" << (self->m_tracking ? 1 : 0)
                << " v=" << self->m_velocityV << " h=" << self->m_velocityH << " self=" << self << "\n";
        }
    }

    if (!self->m_tracking)
        return 0;

    // Only start kinetic if velocity is above threshold
    const double minVelocity = getKineticConfigFloat("min_velocity", 0.5);
    if (std::abs(self->m_velocityV) < minVelocity && std::abs(self->m_velocityH) < minVelocity) {
        self->m_tracking = false;
        return 0;
    }

    // Finger lifted - begin kinetic decay
    self->m_tracking = false;
    self->m_decaying = true;

    wl_event_source_timer_update(self->m_decayTimer, getKineticConfigInt("interval_ms", 16));
    return 0;
}

int KineticState::onDecayTimer(void* data) {
    auto* self = static_cast<KineticState*>(data);

    if (!self->m_decaying) {
        if (getKineticConfigInt("debug", 0)) {
            std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
            if (log.is_open())
                log << "[hypr-kinetic-scroll] decayTimer skipped (not decaying)\n";
        }
        return 0;
    }

    const auto targetKeys = currentScrollTargetKeys();

    if (getKineticConfigInt("stop_on_target_change", 1) && self->m_scrollTargetWindowKey != 0) {
        const bool windowChanged  = targetKeys.windowKey != 0 && targetKeys.windowKey != self->m_scrollTargetWindowKey;
        const bool surfaceChanged = targetKeys.surfaceKey != 0 && targetKeys.surfaceKey != self->m_scrollTargetSurfaceKey;
        if (windowChanged || surfaceChanged) {
            self->stopKinetic("targetChangedDecay");
            return 0;
        }
    }

    const auto PWIN = g_pInputManager ? g_pInputManager->m_lastMouseFocus.lock() : nullptr;
    if (PWIN) {
        const bool hasRule = self->hasAppRule(PWIN->m_class);
        if (!self->shouldProcessForWindow(PWIN->m_class)) {
            self->stopKinetic("appRuleDecay");
            return 0;
        }

        if (getKineticConfigInt("disable_in_browser", 1) && !hasRule && classLooksLikeBrowser(PWIN->m_class)) {
            self->stopKinetic("browserDecay");
            return 0;
        }
    }

    // Apply deceleration
    const double decel = getKineticConfigFloat("decel", 0.92);
    self->m_velocityV *= decel;
    self->m_velocityH *= decel;

    const double minVelocity = getKineticConfigFloat("min_velocity", 0.5);
    bool activeV = std::abs(self->m_velocityV) >= minVelocity;
    bool activeH = std::abs(self->m_velocityH) >= minVelocity;

    if (!activeV && !activeH) {
        self->stopKinetic("decayDone");
        return 0;
    }

    self->emitSyntheticScroll();

    // Re-arm for next frame
    wl_event_source_timer_update(self->m_decayTimer, getKineticConfigInt("interval_ms", 16));
    return 0;
}

void KineticState::emitSyntheticScroll() {
    static auto PSCROLLFACTOR = CConfigValue<Hyprlang::FLOAT>("input:touchpad:scroll_factor");
    auto     now         = std::chrono::steady_clock::now();
    uint32_t timeMs      = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    double   scrollFactor = *PSCROLLFACTOR;

    const double minVelocity = getKineticConfigFloat("min_velocity", 0.5);

    if (std::abs(m_velocityV) >= minVelocity) {
        g_pSeatManager->sendPointerAxis(
            timeMs,
            WL_POINTER_AXIS_VERTICAL_SCROLL,
            m_velocityV * scrollFactor,
            0,   // discrete
            0,   // v120
            WL_POINTER_AXIS_SOURCE_CONTINUOUS,
            WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    }

    if (std::abs(m_velocityH) >= minVelocity) {
        g_pSeatManager->sendPointerAxis(
            timeMs,
            WL_POINTER_AXIS_HORIZONTAL_SCROLL,
            m_velocityH * scrollFactor,
            0,
            0,
            WL_POINTER_AXIS_SOURCE_CONTINUOUS,
            WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    }

    g_pSeatManager->sendPointerFrame();
}
