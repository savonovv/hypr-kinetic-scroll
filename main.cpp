#include "globals.hpp"
#include "kinetic.hpp"
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <fstream>

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:enabled", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:decel", Hyprlang::FLOAT{0.92});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:min_velocity", Hyprlang::FLOAT{0.5});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:interval_ms", Hyprlang::INT{16});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:delta_multiplier", Hyprlang::FLOAT{1.25});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:disable_in_browser", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:stop_on_target_change", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:debug", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:peak_boost", Hyprlang::FLOAT{0.35});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:stop_on_click", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:kinetic-scroll:stop_on_focus", Hyprlang::INT{0});

    g_pKineticState = new KineticState();

    g_pAxisListener = Event::bus()->m_events.input.mouse.axis.listen(
        [](IPointer::SAxisEvent e, Event::SCallbackInfo& info) {
            if (!g_pKineticState)
                return;
            g_pKineticState->onAxis(e);
        });

    g_pButtonListener = Event::bus()->m_events.input.mouse.button.listen(
        [](IPointer::SButtonEvent e, Event::SCallbackInfo& info) {
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

            g_pKineticState->stopKinetic("mouseButton");
        });

    g_pWindowListener = Event::bus()->m_events.window.active.listen(
        [](PHLWINDOW w, Desktop::eFocusReason reason) {
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

            g_pKineticState->stopKinetic("activeWindow");
        });

    HyprlandAPI::addNotification(PHANDLE, "[hypr-kinetic-scroll] Loaded!", CHyprColor{0.2, 0.8, 0.2, 1.0}, 3000);

    return {"hypr-kinetic-scroll", "Kinetic (inertial) scrolling for touchpads", "savonovv", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_pAxisListener.reset();
    g_pButtonListener.reset();
    g_pWindowListener.reset();

    delete g_pKineticState;
    g_pKineticState = nullptr;
}
