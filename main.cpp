#include "globals.hpp"
#include "kinetic.hpp"
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <fstream>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

static Hyprutils::Signal::CHyprSignalListener g_pAxisCallback;
static Hyprutils::Signal::CHyprSignalListener g_pButtonCallback;
static Hyprutils::Signal::CHyprSignalListener g_pWindowCallback;
static Hyprutils::Signal::CHyprSignalListener g_pConfigReloadCallback;
static std::vector<Hyprutils::Signal::CHyprSignalListener> g_pTouchpadCallbacks;

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

    if (!cfgInt(g_cfg.stopOnClick, 0))
        return;

    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED)
        return;
    if (cfgInt(g_cfg.debug, 0)) {
        std::ofstream log("/tmp/hypr-kinetic-scroll.log", std::ios::app);
        if (log.is_open())
            log << "[hypr-kinetic-scroll] mouseButton -> stopKinetic\n";
    }

    // Any mouse click stops kinetic scrolling
    g_pKineticState->stopKinetic("mouseButton");
}

static void onActiveWindow(PHLWINDOW /*window*/, Desktop::eFocusReason /*reason*/) {
    if (!g_pKineticState)
        return;

    if (!cfgInt(g_cfg.stopOnFocus, 0))
        return;

    if (cfgInt(g_cfg.debug, 0)) {
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
    // Keep the SPs alive. addConfigValueV2 only weakly tracks them; dropping
    // the last strong ref destroys the value and aborts Hyprland.
    g_cfg.enabled            = makeShared<CIntValue>("plugin:kinetic-scroll:enabled", "Enable kinetic scrolling", 1);
    g_cfg.decel              = makeShared<CFloatValue>("plugin:kinetic-scroll:decel", "Kinetic deceleration multiplier", 0.92F);
    g_cfg.minVelocity        = makeShared<CFloatValue>("plugin:kinetic-scroll:min_velocity", "Minimum velocity before stopping", 0.5F);
    g_cfg.intervalMs         = makeShared<CIntValue>("plugin:kinetic-scroll:interval_ms", "Kinetic timer interval", 16);
    g_cfg.deltaMultiplier    = makeShared<CFloatValue>("plugin:kinetic-scroll:delta_multiplier", "Scroll delta multiplier", 1.25F);
    g_cfg.disableInBrowser   = makeShared<CIntValue>("plugin:kinetic-scroll:disable_in_browser", "Disable kinetic scrolling in browsers", 1);
    g_cfg.stopOnTargetChange = makeShared<CIntValue>("plugin:kinetic-scroll:stop_on_target_change", "Stop inertia when scroll target changes", 1);
    g_cfg.disabledClasses    = makeShared<CStringValue>("plugin:kinetic-scroll:disabled_classes", "Comma or space separated classes with kinetic scrolling disabled", "");
    g_cfg.debug              = makeShared<CIntValue>("plugin:kinetic-scroll:debug", "Enable kinetic scroll debug logging", 0);
    g_cfg.stopOnClick        = makeShared<CIntValue>("plugin:kinetic-scroll:stop_on_click", "Stop inertia on mouse click", 0);
    g_cfg.stopOnFocus        = makeShared<CIntValue>("plugin:kinetic-scroll:stop_on_focus", "Stop inertia on focus change", 0);

    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfg.enabled);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfg.decel);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfg.minVelocity);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfg.intervalMs);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfg.deltaMultiplier);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfg.disableInBrowser);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfg.stopOnTargetChange);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfg.disabledClasses);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfg.debug);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfg.stopOnClick);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_cfg.stopOnFocus);
}

static void registerTouchpadCallbacks() {
    for (const auto& pointer : g_pInputManager->m_pointers) {
        if (!pointer || !pointer->m_isTouchpad)
            continue;

        g_pTouchpadCallbacks.emplace_back(pointer->m_pointerEvents.frame.listen([] {
            if (g_pKineticState)
                g_pKineticState->onPointerFrame();
        }));
        g_pTouchpadCallbacks.emplace_back(pointer->m_pointerEvents.motion.listen([](const IPointer::SMotionEvent&) {
            if (g_pKineticState)
                g_pKineticState->onTouchpadContact();
        }));
        g_pTouchpadCallbacks.emplace_back(pointer->m_pointerEvents.holdBegin.listen([](const IPointer::SHoldBeginEvent&) {
            if (g_pKineticState)
                g_pKineticState->onTouchpadContact();
        }));
    }
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    if (std::string{__hyprland_api_get_hash()} != std::string{__hyprland_api_get_client_hash()})
        throw std::runtime_error("Version mismatch");

    registerConfigValues();

    g_pKineticState = new KineticState();

    registerLuaFunctions();

    // Register event callbacks
    g_pAxisCallback         = Event::bus()->m_events.input.mouse.axis.listen(onMouseAxis);
    g_pButtonCallback       = Event::bus()->m_events.input.mouse.button.listen(onMouseButton);
    g_pWindowCallback       = Event::bus()->m_events.window.active.listen(onActiveWindow);
    g_pConfigReloadCallback = Event::bus()->m_events.config.preReload.listen(onConfigPreReload);
    registerTouchpadCallbacks();

    return {"hypr-kinetic-scroll", "Kinetic (inertial) scrolling for touchpads", "savonovv", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_pAxisCallback.reset();
    g_pButtonCallback.reset();
    g_pWindowCallback.reset();
    g_pConfigReloadCallback.reset();
    g_pTouchpadCallbacks.clear();

    delete g_pKineticState;
    g_pKineticState = nullptr;

    // Drop CIntValue/CConfigValue views while this .so is still mapped.
    // addConfigValueV2 copies defaults into Lua wrappers; those wrappers are
    // erased in onPluginUnload after PLUGIN_EXIT. If our CConfigValue objects
    // stay in Hyprland's global registry past that, the post-unload config
    // reload looks up deleted plugin keys and aborts (unload then load).
    g_cfg = {};
    PHANDLE = nullptr;
}
