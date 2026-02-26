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
#include <algorithm>

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

void KineticState::addVelocitySample(bool vertical, uint32_t timeMs, double delta, uint32_t minSampleMs, size_t maxSamples) {
    auto& lastSampleMs = vertical ? m_lastSampleMsV : m_lastSampleMsH;
    auto& samples      = vertical ? m_velocitySamplesV : m_velocitySamplesH;

    if (lastSampleMs == 0) {
        lastSampleMs = timeMs;
        return;
    }

    const uint32_t dt = timeMs - lastSampleMs;
    if (dt < minSampleMs)
        return;

    lastSampleMs = timeMs;
    if (dt == 0)
        return;

    const double velocity = delta / static_cast<double>(dt);
    samples.emplace_back(timeMs, velocity);

    while (samples.size() > maxSamples)
        samples.pop_front();
}

double KineticState::averagedVelocity(bool vertical, uint32_t nowMs, uint32_t relevanceMs) {
    auto& samples = vertical ? m_velocitySamplesV : m_velocitySamplesH;

    while (!samples.empty() && (nowMs - samples.front().first) > relevanceMs)
        samples.pop_front();

    if (samples.empty())
        return 0.0;

    double sum = 0.0;
    for (const auto& s : samples)
        sum += s.second;

    return sum / static_cast<double>(samples.size());
}

void KineticState::resetVelocityTrackers() {
    m_lastSampleMsV = 0;
    m_lastSampleMsH = 0;
    m_velocitySamplesV.clear();
    m_velocitySamplesH.clear();
}

void KineticState::onAxis(IPointer::SAxisEvent& e) {
    static auto const* PENABLED =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:enabled")->getDataStaticPtr();
    static auto const* PDISABLE_BROWSER =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:disable_in_browser")->getDataStaticPtr();
    static auto const* PSTOPTARGET =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:stop_on_target_change")->getDataStaticPtr();
    static auto const* PDELTA_MUL =
        (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:delta_multiplier")->getDataStaticPtr();
    static auto const* PVEL_RELEVANCE_MS =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:velocity_relevance_ms")->getDataStaticPtr();
    static auto const* PMIN_SAMPLE_MS =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:min_sample_ms")->getDataStaticPtr();
    static auto const* PMAX_VELOCITY_SAMPLES =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:max_velocity_samples")->getDataStaticPtr();
    static auto const* PDEBUG =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:debug")->getDataStaticPtr();
    static uint64_t s_lastNotifyMs = 0;

    if (!**PENABLED)
        return;

    const auto targetKeys = currentScrollTargetKeys();

    if (**PSTOPTARGET && m_scrollTargetWindowKey != 0) {
        const bool windowChanged = targetKeys.windowKey != 0 && targetKeys.windowKey != m_scrollTargetWindowKey;
        const bool surfaceChanged = targetKeys.surfaceKey != 0 && targetKeys.surfaceKey != m_scrollTargetSurfaceKey;
        if (windowChanged || surfaceChanged)
            stopKinetic("targetChanged");
    }

    if (**PDISABLE_BROWSER) {
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

    const double scaledDelta = e.delta * **PDELTA_MUL;

    // New finger scroll while decaying => continue from current momentum
    const bool resumedFromDecay = m_decaying;
    if (resumedFromDecay) {
        if (**PDEBUG) {
            std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
            if (log.is_open())
                log << "[hypr-kinetic-scroll] onAxis: decaying -> resume self=" << this << "\n";
        }
        m_decaying = false;
        m_lastDecaySampleMs = 0;
        wl_event_source_timer_update(m_decayTimer, 0);
    }

    m_tracking              = true;
    m_scrollTargetWindowKey = targetKeys.windowKey;
    m_scrollTargetSurfaceKey = targetKeys.surfaceKey;

    const uint32_t minSampleMs = std::max(1, static_cast<int>(**PMIN_SAMPLE_MS));
    const uint32_t relevanceMs = std::max(1, static_cast<int>(**PVEL_RELEVANCE_MS));
    const size_t   maxSamples  = static_cast<size_t>(std::max(1, static_cast<int>(**PMAX_VELOCITY_SAMPLES)));

    const bool verticalAxis = e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL;
    addVelocitySample(verticalAxis, e.timeMs, scaledDelta, minSampleMs, maxSamples);

    double trackedVelocity = averagedVelocity(verticalAxis, e.timeMs, relevanceMs);
    if (trackedVelocity == 0.0) {
        uint32_t seedDt = 16;
        if (m_lastEventMs > 0 && e.timeMs > m_lastEventMs)
            seedDt = std::max<uint32_t>(1, e.timeMs - m_lastEventMs);
        trackedVelocity = scaledDelta / static_cast<double>(seedDt);
    }

    if (verticalAxis) {
        if (resumedFromDecay)
            m_velocityV += trackedVelocity;
        else
            m_velocityV = trackedVelocity;
    } else {
        if (resumedFromDecay)
            m_velocityH += trackedVelocity;
        else
            m_velocityH = trackedVelocity;
    }

    m_lastEventMs = e.timeMs;

    if (**PDEBUG) {
        auto     now    = std::chrono::steady_clock::now();
        uint64_t nowMs  = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        if (nowMs - s_lastNotifyMs > 200) {
            const char* axis = e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? "v" : "h";
            std::string msg = "[hypr-kinetic-scroll] axis=" + std::string(axis) +
                              " delta=" + std::to_string(e.delta) +
                               " source=" + std::to_string((int)e.source) +
                               " mouse=" + std::to_string(e.mouse ? 1 : 0) +
                               " discrete=" + std::to_string(e.deltaDiscrete) +
                               " trackedV=" + std::to_string(trackedVelocity) +
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
    static auto const* PDEBUG =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:debug")->getDataStaticPtr();
    if (**PDEBUG) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open())
            log << "[hypr-kinetic-scroll] stopKinetic reason=" << (reason ? reason : "(null)") << " self=" << this << "\n";
    }
    m_velocityV   = 0.0;
    m_velocityH   = 0.0;
    m_tracking    = false;
    m_decaying    = false;
    m_lastEventMs          = 0;
    m_lastDecaySampleMs    = 0;
    m_scrollTargetWindowKey = 0;
    m_scrollTargetSurfaceKey = 0;
    resetVelocityTrackers();
    wl_event_source_timer_update(m_stopTimer, 0);
    wl_event_source_timer_update(m_decayTimer, 0);
}

int KineticState::onStopTimer(void* data) {
    auto* self = static_cast<KineticState*>(data);

    static auto const* PDEBUG =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:debug")->getDataStaticPtr();
    if (**PDEBUG) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open()) {
            log << "[hypr-kinetic-scroll] stopTimer tracking=" << (self->m_tracking ? 1 : 0)
                << " v=" << self->m_velocityV << " h=" << self->m_velocityH << " self=" << self << "\n";
        }
    }

    if (!self->m_tracking)
        return 0;

    static auto const* PMINVEL =
        (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:min_velocity")->getDataStaticPtr();
    static auto const* PINTERVAL =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:interval_ms")->getDataStaticPtr();

    const double intervalMs = static_cast<double>(std::max(1, static_cast<int>(**PINTERVAL)));
    const double projectedV = std::abs(self->m_velocityV) * intervalMs;
    const double projectedH = std::abs(self->m_velocityH) * intervalMs;

    // Only start kinetic if velocity is above threshold
    if (projectedV < **PMINVEL && projectedH < **PMINVEL) {
        self->m_tracking = false;
        return 0;
    }

    // Finger lifted - begin kinetic decay
    self->m_tracking = false;
    self->m_decaying = true;

    wl_event_source_timer_update(self->m_decayTimer, **PINTERVAL);
    return 0;
}

int KineticState::onDecayTimer(void* data) {
    auto* self = static_cast<KineticState*>(data);

    static auto const* PDEBUG =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:debug")->getDataStaticPtr();
    static auto const* PDISABLE_BROWSER =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:disable_in_browser")->getDataStaticPtr();
    static auto const* PSTOPTARGET =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:stop_on_target_change")->getDataStaticPtr();

    if (!self->m_decaying) {
        if (**PDEBUG) {
            std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
            if (log.is_open())
                log << "[hypr-kinetic-scroll] decayTimer skipped (not decaying)\n";
        }
        return 0;
    }

    const auto targetKeys = currentScrollTargetKeys();

    if (**PSTOPTARGET && self->m_scrollTargetWindowKey != 0) {
        const bool windowChanged  = targetKeys.windowKey != 0 && targetKeys.windowKey != self->m_scrollTargetWindowKey;
        const bool surfaceChanged = targetKeys.surfaceKey != 0 && targetKeys.surfaceKey != self->m_scrollTargetSurfaceKey;
        if (windowChanged || surfaceChanged) {
            self->stopKinetic("targetChangedDecay");
            return 0;
        }
    }

    if (**PDISABLE_BROWSER) {
        const auto PWIN = g_pInputManager ? g_pInputManager->m_lastMouseFocus.lock() : nullptr;
        if (PWIN && classLooksLikeBrowser(PWIN->m_class)) {
            self->stopKinetic("browserDecay");
            return 0;
        }
    }

    static auto const* PDECEL =
        (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:decel")->getDataStaticPtr();
    static auto const* PFRICTION =
        (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:friction")->getDataStaticPtr();
    static auto const* PMINVEL =
        (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:min_velocity")->getDataStaticPtr();
    static auto const* PINTERVAL =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:interval_ms")->getDataStaticPtr();

    const auto now = std::chrono::steady_clock::now();
    const auto nowMs = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    const double intervalMs = static_cast<double>(std::max(1, static_cast<int>(**PINTERVAL)));

    double dtMs = intervalMs;
    if (self->m_lastDecaySampleMs > 0 && nowMs > self->m_lastDecaySampleMs)
        dtMs = static_cast<double>(nowMs - self->m_lastDecaySampleMs);
    self->m_lastDecaySampleMs = nowMs;

    const double friction = static_cast<double>(**PFRICTION);
    double       decayFactor;
    if (friction > 0.0 && friction < 1.0) {
        decayFactor = std::pow(1.0 - friction, dtMs);
    } else {
        const double decel = std::clamp(static_cast<double>(**PDECEL), 0.0, 1.0);
        decayFactor        = std::pow(decel, dtMs / intervalMs);
    }

    self->m_velocityV *= decayFactor;
    self->m_velocityH *= decayFactor;

    const bool activeV = std::abs(self->m_velocityV) * dtMs >= **PMINVEL;
    const bool activeH = std::abs(self->m_velocityH) * dtMs >= **PMINVEL;

    if (!activeV && !activeH) {
        self->stopKinetic("decayDone");
        return 0;
    }

    self->emitSyntheticScroll(dtMs);

    // Re-arm for next frame
    wl_event_source_timer_update(self->m_decayTimer, **PINTERVAL);
    return 0;
}

void KineticState::emitSyntheticScroll(double dtMs) {
    static auto PSCROLLFACTOR = CConfigValue<Hyprlang::FLOAT>("input:touchpad:scroll_factor");
    static auto const* PMINVEL =
        (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:min_velocity")->getDataStaticPtr();

    auto     now         = std::chrono::steady_clock::now();
    uint32_t timeMs      = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    double   scrollFactor = *PSCROLLFACTOR;

    if (std::abs(m_velocityV) * dtMs >= **PMINVEL) {
        g_pSeatManager->sendPointerAxis(
            timeMs,
            WL_POINTER_AXIS_VERTICAL_SCROLL,
            m_velocityV * dtMs * scrollFactor,
            0,   // discrete
            0,   // v120
            WL_POINTER_AXIS_SOURCE_CONTINUOUS,
            WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    }

    if (std::abs(m_velocityH) * dtMs >= **PMINVEL) {
        g_pSeatManager->sendPointerAxis(
            timeMs,
            WL_POINTER_AXIS_HORIZONTAL_SCROLL,
            m_velocityH * dtMs * scrollFactor,
            0,
            0,
            WL_POINTER_AXIS_SOURCE_CONTINUOUS,
            WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    }

    g_pSeatManager->sendPointerFrame();
}
