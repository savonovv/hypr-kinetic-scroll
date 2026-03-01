#include "globals.hpp"
#include "kinetic.hpp"
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <unordered_map>
#include <any>
#include <fstream>

// Signal handlers - new Event bus API
static CHyprSignalListener g_pAxisListener;
static CHyprSignalListener g_pButtonListener;
static CHyprSignalListener g_pWindowListener;

static void onMouseAxis(IPointer::SAxisEvent e, Event::SCallbackInfo& info) {
    static auto const* PDEBUG =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:debug")->getDataStaticPtr();
    if (**PDEBUG) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open())
            log << "[hypr-kinetic-scroll] onMouseAxis called!\n";
    }
    
    if (!g_pKineticState)
        return;

    g_pKineticState->onAxis(e);
    // Don't cancel - let the original scroll event pass through to the app
}

static void onMouseButton(IPointer::SButtonEvent e, Event::SCallbackInfo& info) {
    if (!g_pKineticState)
        return;

    static auto const* PSTOPCLICK =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:stop_on_click")->getDataStaticPtr();
    static auto const* PDEBUG =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:debug")->getDataStaticPtr();

    if (!**PSTOPCLICK)
        return;

    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED)
        return;
    if (**PDEBUG) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open())
            log << "[hypr-kinetic-scroll] mouseButton -> stopKinetic\n";
    }

    // Any mouse click stops kinetic scrolling
    g_pKineticState->stopKinetic("mouseButton");
}

static void onActiveWindow(PHLWINDOW window, Desktop::eFocusReason reason) {
    (void)window;
    (void)reason;
    
    if (!g_pKineticState)
        return;

    static auto const* PSTOPFOCUS =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:stop_on_focus")->getDataStaticPtr();

    if (!**PSTOPFOCUS)
        return;

    static auto const* PDEBUG =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:debug")->getDataStaticPtr();
    if (**PDEBUG) {
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

    // Register config values
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:enabled", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:decel", Hyprlang::FLOAT{0.92});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:friction", Hyprlang::FLOAT{0.002});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:min_velocity", Hyprlang::FLOAT{0.6});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:interval_ms", Hyprlang::INT{8});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:delta_multiplier", Hyprlang::FLOAT{1.25});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:velocity_relevance_ms", Hyprlang::INT{100});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:min_sample_ms", Hyprlang::INT{5});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:max_velocity_samples", Hyprlang::INT{5});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:disable_in_browser", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:stop_on_target_change", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:debug", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:stop_on_click", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:stop_on_focus", Hyprlang::INT{0});

    // Create kinetic state (must be after compositor is ready, which it is during PLUGIN_INIT)
    g_pKineticState = new KineticState();

    // Register event callbacks using new Event bus API
    g_pAxisListener = Event::bus()->m_events.input.mouse.axis.listen(onMouseAxis);
    g_pButtonListener = Event::bus()->m_events.input.mouse.button.listen(onMouseButton);
    g_pWindowListener = Event::bus()->m_events.window.active.listen(onActiveWindow);

    HyprlandAPI::addNotification(PHANDLE, "[hypr-kinetic-scroll] Loaded!", CHyprColor{0.2, 0.8, 0.2, 1.0}, 3000);

    return {"hypr-kinetic-scroll", "Kinetic (inertial) scrolling for touchpads", "savonovv", "0.3.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Release signal listeners (when SP is reset, listener is unregistered)
    g_pAxisListener.reset();
    g_pButtonListener.reset();
    g_pWindowListener.reset();

    // Clean up kinetic state (removes wl timers)
    delete g_pKineticState;
    g_pKineticState = nullptr;
}
