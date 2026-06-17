#include "globals.hpp"
#include "kinetic.hpp"
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <fstream>
#include <stdexcept>
#include <vector>

static CHyprSignalListener              g_axisListener;
static CHyprSignalListener              g_buttonListener;
static CHyprSignalListener              g_windowListener;
static CHyprSignalListener              g_swipeBeginListener;
static CHyprSignalListener              g_pinchBeginListener;
static std::vector<CHyprSignalListener> g_holdBeginListeners;

static void onMouseAxis(IPointer::SAxisEvent e, Event::SCallbackInfo& /*info*/) {
    if (!g_pKineticState)
        return;

    g_pKineticState->onAxis(e);
    // Don't cancel - let the original scroll event pass through to the app
}

static void onMouseButton(IPointer::SButtonEvent e, Event::SCallbackInfo& /*info*/) {
    if (!g_pKineticState)
        return;

    static const CConfigValue<Config::INTEGER> PSTOPCLICK("plugin:kinetic-scroll:stop_on_click");
    static const CConfigValue<Config::INTEGER> PDEBUG("plugin:kinetic-scroll:debug");

    if (!*PSTOPCLICK)
        return;

    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED)
        return;
    if (*PDEBUG) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open())
            log << "[hypr-kinetic-scroll] mouseButton -> stopKinetic\n";
    }

    // Any mouse click stops kinetic scrolling
    g_pKineticState->stopKinetic("mouseButton");
}

static void stopOnTwoFingerTouchpadGesture(uint32_t fingers, const char* reason) {
    if (!g_pKineticState || fingers != 2)
        return;

    static const CConfigValue<Config::INTEGER> PSTOPGESTURE("plugin:kinetic-scroll:stop_on_touchpad_gesture");
    static const CConfigValue<Config::INTEGER> PDEBUG("plugin:kinetic-scroll:debug");

    if (!*PSTOPGESTURE)
        return;

    if (*PDEBUG) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open())
            log << "[hypr-kinetic-scroll] " << reason << " fingers=" << fingers << " -> stopKinetic\n";
    }

    g_pKineticState->stopKinetic(reason);
}

static void onSwipeBegin(IPointer::SSwipeBeginEvent e, Event::SCallbackInfo& /*info*/) {
    stopOnTwoFingerTouchpadGesture(e.fingers, "swipeBegin");
}

static void onPinchBegin(IPointer::SPinchBeginEvent e, Event::SCallbackInfo& /*info*/) {
    stopOnTwoFingerTouchpadGesture(e.fingers, "pinchBegin");
}

static void onHoldBegin(IPointer::SHoldBeginEvent e) {
    stopOnTwoFingerTouchpadGesture(e.fingers, "holdBegin");
}

static void onActiveWindow(PHLWINDOW /*window*/, Desktop::eFocusReason /*reason*/) {
    if (!g_pKineticState)
        return;

    static const CConfigValue<Config::INTEGER> PSTOPFOCUS("plugin:kinetic-scroll:stop_on_focus");

    if (!*PSTOPFOCUS)
        return;

    static const CConfigValue<Config::INTEGER> PDEBUG("plugin:kinetic-scroll:debug");
    if (*PDEBUG) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open())
            log << "[hypr-kinetic-scroll] activeWindow -> stopKinetic\n";
    }

    // Window focus change stops kinetic scrolling
    g_pKineticState->stopKinetic("activeWindow");
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // NOTE: version check skipped for local dev (headers v0.53.3, running v0.53.1).
    // Re-enable for distribution:
    // if (__hyprland_api_get_hash() != __hyprland_api_get_client_hash())
    //     throw std::runtime_error("Version mismatch");

    // Register config values through the Lua-compatible V2 API.
    auto addIntConfig = [](const char* name, const char* description, Config::INTEGER value) {
        if (!HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<Config::Values::CIntValue>(name, description, value)))
            throw std::runtime_error(std::string{"Failed to register config value "} + name);
    };
    auto addFloatConfig = [](const char* name, const char* description, Config::FLOAT value) {
        if (!HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<Config::Values::CFloatValue>(name, description, value)))
            throw std::runtime_error(std::string{"Failed to register config value "} + name);
    };

    addIntConfig("plugin:kinetic-scroll:enabled", "Enable kinetic scrolling", 1);
    addFloatConfig("plugin:kinetic-scroll:decel", "Kinetic scroll deceleration multiplier", 0.92F);
    addFloatConfig("plugin:kinetic-scroll:min_velocity", "Minimum velocity before stopping kinetic scrolling", 0.5F);
    addIntConfig("plugin:kinetic-scroll:interval_ms", "Kinetic scroll timer interval in milliseconds", 16);
    addFloatConfig("plugin:kinetic-scroll:delta_multiplier", "Input delta multiplier for kinetic scrolling", 1.25F);
    addIntConfig("plugin:kinetic-scroll:disable_in_browser", "Disable plugin inertia in browsers", 1);
    addIntConfig("plugin:kinetic-scroll:stop_on_target_change", "Stop inertia when the scroll target changes", 1);
    addIntConfig("plugin:kinetic-scroll:debug", "Enable kinetic scroll debug logging", 0);
    addIntConfig("plugin:kinetic-scroll:stop_on_click", "Stop inertia on mouse click", 0);
    addIntConfig("plugin:kinetic-scroll:stop_on_focus", "Stop inertia on focus change", 0);
    addIntConfig("plugin:kinetic-scroll:stop_on_touchpad_gesture", "Stop inertia on two-finger touchpad gestures", 1);
    addIntConfig("plugin:kinetic-scroll:stop_delay_ms", "Delay before treating scroll input as stopped", 20);

    // Create kinetic state (must be after compositor is ready, which it is during PLUGIN_INIT)
    g_pKineticState = new KineticState();

    // Register event callbacks via the Event::bus() signal system
    g_axisListener       = Event::bus()->m_events.input.mouse.axis.listen(onMouseAxis);
    g_buttonListener     = Event::bus()->m_events.input.mouse.button.listen(onMouseButton);
    g_windowListener     = Event::bus()->m_events.window.active.listen(onActiveWindow);
    g_swipeBeginListener = Event::bus()->m_events.gesture.swipe.begin.listen(onSwipeBegin);
    g_pinchBeginListener = Event::bus()->m_events.gesture.pinch.begin.listen(onPinchBegin);

    // Hyprland's global Event::bus() currently exposes swipe/pinch, but not hold.
    // Listen to each existing pointer directly so a two-finger touchpad hold/tap can
    // stop kinetic motion as soon as Hyprland/libinput reports it.
    if (g_pInputManager) {
        for (const auto& pointer : g_pInputManager->m_pointers) {
            if (pointer)
                g_holdBeginListeners.emplace_back(pointer->m_pointerEvents.holdBegin.listen(onHoldBegin));
        }
    }

    HyprlandAPI::addNotification(PHANDLE, "[hypr-kinetic-scroll] Loaded!", CHyprColor{0.2, 0.8, 0.2, 1.0}, 3000);

    return {"hypr-kinetic-scroll", "Kinetic (inertial) scrolling for touchpads", "savonovv", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Release listeners (auto-unregister when SPs are reset)
    g_axisListener.reset();
    g_buttonListener.reset();
    g_windowListener.reset();
    g_swipeBeginListener.reset();
    g_pinchBeginListener.reset();
    g_holdBeginListeners.clear();

    // Clean up kinetic state (removes wl timers)
    delete g_pKineticState;
    g_pKineticState = nullptr;
}
