#include "globals.hpp"
#include "kinetic.hpp"
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <fstream>
#include <sstream>
#include <tuple>
#include <utility>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

// Bypass header-side CSignalT::listen adapter so plugins keep working when the
// running Hyprland was built against a different hyprutils minor version.
struct SignalBaseAccessor : Hyprutils::Signal::CSignalBase {
    using Hyprutils::Signal::CSignalBase::registerListenerInternal;
};

template <typename Signal, typename Handler>
static Hyprutils::Signal::CHyprSignalListener listenRaw(Signal& signal, Handler handler) {
    return reinterpret_cast<SignalBaseAccessor*>(&signal)->registerListenerInternal(std::move(handler));
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

static void onActiveWindow() {
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
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:stop_on_target_change", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:debug", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:stop_on_click", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:stop_on_focus", Hyprlang::INT{0});

    // Create kinetic state (must be before registering keyword so it's available during config parse)
    g_pKineticState = new KineticState();

    HyprlandAPI::addConfigKeyword(PHANDLE, "kinetic-scroll-rule", parseKineticScrollRule, {});
    registerLuaFunctions();

    // Register event callbacks
    g_pAxisCallback = listenRaw(Event::bus()->m_events.input.mouse.axis, [](void* data) {
        auto& args = *reinterpret_cast<std::tuple<const IPointer::SAxisEvent&, Event::SCallbackInfo&>*>(data);
        onMouseAxis(std::get<0>(args), std::get<1>(args));
    });
    g_pButtonCallback = listenRaw(Event::bus()->m_events.input.mouse.button, [](void* data) {
        auto& args = *reinterpret_cast<std::tuple<const IPointer::SButtonEvent&, Event::SCallbackInfo&>*>(data);
        onMouseButton(std::get<0>(args), std::get<1>(args));
    });
    g_pWindowCallback = listenRaw(Event::bus()->m_events.window.active, [](void* /*data*/) { onActiveWindow(); });
    g_pConfigReloadCallback = listenRaw(Event::bus()->m_events.config.preReload, [](void* /*data*/) { onConfigPreReload(); });

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
