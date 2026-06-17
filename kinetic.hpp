#pragma once
#include <hyprland/src/devices/IPointer.hpp>
#include <wayland-server-core.h>
#include <hyprland/src/event/EventBus.hpp>
#include <cstdint>
#include <deque>

class KineticState {
  public:
    KineticState();
    ~KineticState();

    void onAxis(IPointer::SAxisEvent& e);
    void stopKinetic(const char* reason = nullptr);

    // Exposed for the free function pushToWindow
    struct DeltaSample {
        double   delta;
        uint32_t timeMs;
    };
    static constexpr size_t MAX_DELTA_WINDOW = 5;

  private:
    static int onStopTimer(void* data);
    static int onDecayTimer(void* data);
    void       emitSyntheticScroll();

    double    m_velocityV              = 0.0;
    double    m_velocityH              = 0.0;
    uint32_t  m_lastEventMs            = 0;
    bool      m_tracking               = false;
    bool      m_decaying               = false;
    uintptr_t m_scrollTargetWindowKey  = 0;
    uintptr_t m_scrollTargetSurfaceKey = 0;

    std::deque<DeltaSample> m_recentDeltasV;
    std::deque<DeltaSample> m_recentDeltasH;

    wl_event_source* m_stopTimer  = nullptr;
    wl_event_source* m_decayTimer = nullptr;
};
