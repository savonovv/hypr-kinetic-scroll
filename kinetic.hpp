#pragma once
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <wayland-server-core.h>
#include <cstdint>
#include <unordered_map>
#include <string>

class KineticState {
  public:
    KineticState();
    ~KineticState();

    void onAxis(IPointer::SAxisEvent& e);
    void onPointerFrame();
    void onTouchpadContact();
    void stopKinetic(const char* reason = nullptr);
    void setAppRule(const std::string& appClass, bool enabled);
    void setDefaultAppRule(bool enabled);
    void resetAppRules();

  private:
    static int onStopTimer(void* data);
    static int onDecayTimer(void* data);
    void       beginDecay(const char* reason);
    void       emitSyntheticScroll();
    bool       hasAppRule(const std::string& windowClass) const;
    bool       shouldProcessForWindow(const std::string& windowClass) const;

    double    m_velocityV             = 0.0;
    double    m_velocityH             = 0.0;
    uint32_t  m_lastEventMs           = 0;
    bool      m_tracking              = false;
    bool      m_decaying              = false;
    bool      m_axisEventInFrame      = false;
    bool      m_cancelOnStopTimer     = false;
    uintptr_t m_scrollTargetWindowKey = 0;
    uintptr_t m_scrollTargetSurfaceKey = 0;

    wl_event_source* m_stopTimer  = nullptr;
    wl_event_source* m_decayTimer = nullptr;

    // Bound in the ctor and destroyed in PLUGIN_EXIT with this object, so it
    // leaves Hyprland's CConfigValueBase registry before the .so is unmapped.
    CConfigValue<Hyprlang::FLOAT> m_scrollFactor;

    std::unordered_map<std::string, bool> m_perAppRules;
    bool                                  m_defaultAppRule = true;
};
