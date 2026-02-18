#pragma once
#include <hyprland/src/devices/IPointer.hpp>
#include <wayland-server-core.h>
#include <cstdint>

class KineticState {
  public:
    KineticState();
    ~KineticState();

    void onAxis(IPointer::SAxisEvent& e);
    void stopKinetic(const char* reason = nullptr);

  private:
    static int onStopTimer(void* data);
    static int onDecayTimer(void* data);
    void       emitSyntheticScroll();

    double    m_velocityV             = 0.0;
    double    m_velocityH             = 0.0;
    uint32_t  m_lastEventMs           = 0;
    bool      m_tracking              = false;
    bool      m_decaying              = false;
    uintptr_t m_scrollTargetWindowKey = 0;
    uintptr_t m_scrollTargetSurfaceKey = 0;

    wl_event_source* m_stopTimer  = nullptr;
    wl_event_source* m_decayTimer = nullptr;
};
