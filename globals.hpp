#pragma once
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <string>

inline HANDLE PHANDLE = nullptr;

class KineticState;
inline KineticState* g_pKineticState = nullptr;

inline int getKineticConfigInt(const std::string& name, int fallback) {
    const auto OPTION = CConfigValue<Config::INTEGER>("plugin:kinetic-scroll:" + name);
    if (!OPTION.good())
        return fallback;

    return *OPTION;
}

inline double getKineticConfigFloat(const std::string& name, double fallback) {
    const auto OPTION = CConfigValue<Config::FLOAT>("plugin:kinetic-scroll:" + name);
    if (!OPTION.good())
        return fallback;

    return *OPTION;
}

inline std::string getKineticConfigString(const std::string& name, const std::string& fallback) {
    const auto OPTION = CConfigValue<Config::STRING>("plugin:kinetic-scroll:" + name);
    if (!OPTION.good())
        return fallback;

    return *OPTION;
}
