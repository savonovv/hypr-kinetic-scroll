#include "globals.hpp"
#include "kinetic.hpp"
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <unordered_map>
#include <any>
#include <fstream>

static SP<HOOK_CALLBACK_FN> g_pAxisCallback;
static SP<HOOK_CALLBACK_FN> g_pButtonCallback;
static SP<HOOK_CALLBACK_FN> g_pWindowCallback;

static void onMouseAxis(void* /*self*/, SCallbackInfo& /*info*/, std::any data) {
    if (!g_pKineticState)
        return;

    auto eventData = std::any_cast<std::unordered_map<std::string, std::any>>(data);
    auto e         = std::any_cast<IPointer::SAxisEvent>(eventData["event"]);

    g_pKineticState->onAxis(e);
    // Don't cancel - let the original scroll event pass through to the app
}

static void onMouseButton(void* /*self*/, SCallbackInfo& /*info*/, std::any data) {
    if (!g_pKineticState)
        return;

    static auto const* PSTOPCLICK =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:stop_on_click")->getDataStaticPtr();
    static auto const* PDEBUG =
        (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:debug")->getDataStaticPtr();

    if (!**PSTOPCLICK)
        return;

    auto eventData = std::any_cast<std::unordered_map<std::string, std::any>>(data);
    auto e         = std::any_cast<IPointer::SButtonEvent>(eventData["event"]);

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

static void onActiveWindow(void* /*self*/, SCallbackInfo& /*info*/, std::any /*data*/) {
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
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:min_velocity", Hyprlang::FLOAT{0.5});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:interval_ms", Hyprlang::INT{16});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:delta_multiplier", Hyprlang::FLOAT{1.25});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:disable_in_browser", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:debug", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:stop_on_click", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:stop_on_focus", Hyprlang::INT{0});

    // Create kinetic state (must be after compositor is ready, which it is during PLUGIN_INIT)
    g_pKineticState = new KineticState();

    // Register event callbacks
    g_pAxisCallback   = HyprlandAPI::registerCallbackDynamic(PHANDLE, "mouseAxis", onMouseAxis);
    g_pButtonCallback = HyprlandAPI::registerCallbackDynamic(PHANDLE, "mouseButton", onMouseButton);
    g_pWindowCallback = HyprlandAPI::registerCallbackDynamic(PHANDLE, "activeWindow", onActiveWindow);

    HyprlandAPI::addNotification(PHANDLE, "[hypr-kinetic-scroll] Loaded!", CHyprColor{0.2, 0.8, 0.2, 1.0}, 3000);

    return {"hypr-kinetic-scroll", "Kinetic (inertial) scrolling for touchpads", "savonovv", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Release callback refs (Hyprland auto-cleans registered callbacks on plugin unload,
    // but resetting our SPs ensures deterministic ordering)
    g_pAxisCallback.reset();
    g_pButtonCallback.reset();
    g_pWindowCallback.reset();

    // Clean up kinetic state (removes wl timers)
    delete g_pKineticState;
    g_pKineticState = nullptr;
}
