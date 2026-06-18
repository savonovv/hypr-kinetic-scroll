#pragma once
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <string>

inline HANDLE PHANDLE = nullptr;

class KineticState;
inline KineticState* g_pKineticState = nullptr;

inline int getKineticConfigInt(const std::string& name, int fallback) {
    const auto VALUE = HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:" + name);
    if (!VALUE)
        return fallback;

    const auto DATA = (Hyprlang::INT* const*)VALUE->getDataStaticPtr();
    return DATA && *DATA ? **DATA : fallback;
}

inline double getKineticConfigFloat(const std::string& name, double fallback) {
    const auto VALUE = HyprlandAPI::getConfigValue(PHANDLE, "plugin:kinetic-scroll:" + name);
    if (!VALUE)
        return fallback;

    const auto DATA = (Hyprlang::FLOAT* const*)VALUE->getDataStaticPtr();
    return DATA && *DATA ? **DATA : fallback;
}
