#include "globals.hpp"
#include "kinetic.hpp"
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <fstream>
#include <sstream>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

static Hyprutils::Signal::CHyprSignalListener g_pAxisCallback;
static Hyprutils::Signal::CHyprSignalListener g_pButtonCallback;
static Hyprutils::Signal::CHyprSignalListener g_pWindowCallback;
static Hyprutils::Signal::CHyprSignalListener g_pConfigReloadCallback;

static void onMouseAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& /*info*/) {
    if (!g_pKineticState)
        return;

    auto event = e;
    g_pKineticState->onAxis(event);
    // Don't cancel - let the original scroll event pass through to the app
}

static void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& /*info*/) {
    if (!g_pKineticState)
        return;

    if (!getKineticConfigInt("stop_on_click", 0))
        return;

    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED)
        return;
    if (getKineticConfigInt("debug", 0)) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open())
            log << "[hypr-kinetic-scroll] mouseButton -> stopKinetic\n";
    }

    // Any mouse click stops kinetic scrolling
    g_pKineticState->stopKinetic("mouseButton");
}

static void onActiveWindow() {
    if (!g_pKineticState)
        return;

    if (!getKineticConfigInt("stop_on_focus", 0))
        return;

    if (getKineticConfigInt("debug", 0)) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open())
            log << "[hypr-kinetic-scroll] activeWindow -> stopKinetic\n";
    }

    // Window focus change stops kinetic scrolling
    g_pKineticState->stopKinetic("activeWindow");
}

static void onConfigPreReload() {
    if (g_pKineticState)
        g_pKineticState->resetAppRules();
}

static Hyprlang::CParseResult parseKineticScrollRule(const char* /*command*/, const char* value) {
    Hyprlang::CParseResult result;
    std::istringstream     iss(value ? value : "");
    std::string            mode, appClass;

    if (!(iss >> mode)) {
        result.setError("Invalid format: expected 'enable [class]' or 'disable [class]'");
        return result;
    }

    bool enable = true;
    if (mode == "disable")
        enable = false;
    else if (mode != "enable") {
        result.setError("Invalid mode: expected 'enable' or 'disable'");
        return result;
    }

    if (!(iss >> appClass)) {
        g_pKineticState->setDefaultAppRule(enable);
        return result;
    }

    std::string extra;
    if (iss >> extra) {
        result.setError("Invalid format: expected 'enable [class]' or 'disable [class]'");
        return result;
    }

    g_pKineticState->setAppRule(appClass, enable);
    return result;
}

static int luaSetAppRule(lua_State* L, bool enabled) {
    const char* appClass = luaL_checkstring(L, 1);
    if (g_pKineticState)
        g_pKineticState->setAppRule(appClass, enabled);
    return 0;
}

static int luaEnable(lua_State* L) {
    return luaSetAppRule(L, true);
}

static int luaDisable(lua_State* L) {
    return luaSetAppRule(L, false);
}

static int luaEnableDefault(lua_State* /*L*/) {
    if (g_pKineticState)
        g_pKineticState->setDefaultAppRule(true);
    return 0;
}

static int luaDisableDefault(lua_State* /*L*/) {
    if (g_pKineticState)
        g_pKineticState->setDefaultAppRule(false);
    return 0;
}

static int luaResetRules(lua_State* /*L*/) {
    if (g_pKineticState)
        g_pKineticState->resetAppRules();
    return 0;
}

static void registerLuaFunctions() {
    HyprlandAPI::addLuaFunction(PHANDLE, "kinetic_scroll", "enable", luaEnable);
    HyprlandAPI::addLuaFunction(PHANDLE, "kinetic_scroll", "disable", luaDisable);
    HyprlandAPI::addLuaFunction(PHANDLE, "kinetic_scroll", "enable_default", luaEnableDefault);
    HyprlandAPI::addLuaFunction(PHANDLE, "kinetic_scroll", "disable_default", luaDisableDefault);
    HyprlandAPI::addLuaFunction(PHANDLE, "kinetic_scroll", "reset_rules", luaResetRules);
}

static void registerConfigValues() {
    using namespace Config::Values;
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CIntValue>("plugin:kinetic-scroll:enabled", "Enable kinetic scrolling", 1));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CFloatValue>("plugin:kinetic-scroll:decel", "Kinetic deceleration multiplier", 0.92F));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CFloatValue>("plugin:kinetic-scroll:min_velocity", "Minimum velocity before stopping", 0.5F));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CIntValue>("plugin:kinetic-scroll:interval_ms", "Kinetic timer interval", 16));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CFloatValue>("plugin:kinetic-scroll:delta_multiplier", "Scroll delta multiplier", 1.25F));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CIntValue>("plugin:kinetic-scroll:disable_in_browser", "Disable kinetic scrolling in browsers", 1));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CIntValue>("plugin:kinetic-scroll:stop_on_target_change", "Stop inertia when scroll target changes", 1));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CIntValue>("plugin:kinetic-scroll:debug", "Enable kinetic scroll debug logging", 0));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CIntValue>("plugin:kinetic-scroll:stop_on_click", "Stop inertia on mouse click", 0));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CIntValue>("plugin:kinetic-scroll:stop_on_focus", "Stop inertia on focus change", 0));
    HyprlandAPI::addConfigValueV2(PHANDLE, makeShared<CStringValue>("plugin:kinetic-scroll:disabled_classes", "Comma or space separated classes with kinetic scrolling disabled", ""));
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

    registerConfigValues();

    // Create kinetic state (must be before registering keyword so it's available during config parse)
    g_pKineticState = new KineticState();

    HyprlandAPI::addConfigKeyword(PHANDLE, "kinetic-scroll-rule", parseKineticScrollRule, {});
    registerLuaFunctions();

    // Register event callbacks
    g_pAxisCallback = Event::bus()->m_events.input.mouse.axis.listen(onMouseAxis);
    g_pButtonCallback = Event::bus()->m_events.input.mouse.button.listen(onMouseButton);
    g_pWindowCallback = Event::bus()->m_events.window.active.listen(onActiveWindow);
    g_pConfigReloadCallback = Event::bus()->m_events.config.preReload.listen(onConfigPreReload);

    HyprlandAPI::addNotification(PHANDLE, "[hypr-kinetic-scroll] Loaded!", CHyprColor{0.2, 0.8, 0.2, 1.0}, 3000);

    return {"hypr-kinetic-scroll", "Kinetic (inertial) scrolling for touchpads", "savonovv", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Release callback refs (Hyprland auto-cleans registered callbacks on plugin unload,
    // but resetting our SPs ensures deterministic ordering)
    g_pAxisCallback.reset();
    g_pButtonCallback.reset();
    g_pWindowCallback.reset();
    g_pConfigReloadCallback.reset();

    // Clean up kinetic state (removes wl timers)
    delete g_pKineticState;
    g_pKineticState = nullptr;
}
