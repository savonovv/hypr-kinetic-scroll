#pragma once
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <string>

inline HANDLE PHANDLE = nullptr;

class KineticState;
inline KineticState* g_pKineticState = nullptr;

struct KineticConfig {
    SP<Config::Values::CIntValue>    enabled;
    SP<Config::Values::CFloatValue>  decel;
    SP<Config::Values::CFloatValue>  minVelocity;
    SP<Config::Values::CIntValue>    intervalMs;
    SP<Config::Values::CFloatValue>  deltaMultiplier;
    SP<Config::Values::CIntValue>    disableInBrowser;
    SP<Config::Values::CIntValue>    stopOnTargetChange;
    SP<Config::Values::CStringValue> disabledClasses;
    SP<Config::Values::CIntValue>    debug;
    SP<Config::Values::CIntValue>    stopOnClick;
    SP<Config::Values::CIntValue>    stopOnFocus;
};

inline KineticConfig g_cfg;

inline int cfgInt(const SP<Config::Values::CIntValue>& v, int fallback) {
    return v ? static_cast<int>(v->value()) : fallback;
}

inline double cfgFloat(const SP<Config::Values::CFloatValue>& v, double fallback) {
    return v ? static_cast<double>(v->value()) : fallback;
}

inline std::string cfgString(const SP<Config::Values::CStringValue>& v, const std::string& fallback) {
    return v ? v->value() : fallback;
}
