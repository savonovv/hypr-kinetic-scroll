#pragma once
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprutils/signal/Signal.hpp>

inline HANDLE PHANDLE = nullptr;

inline CHyprSignalListener g_pAxisListener;
inline CHyprSignalListener g_pButtonListener;
inline CHyprSignalListener g_pWindowListener;

class KineticState;
inline KineticState* g_pKineticState = nullptr;
