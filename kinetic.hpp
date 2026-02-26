#pragma once
#include <hyprland/src/devices/IPointer.hpp>
#include <wayland-server-core.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

class KineticState {
  public:
    KineticState();
    ~KineticState();

    void onAxis(IPointer::SAxisEvent& e);
    void stopKinetic(const char* reason = nullptr);

  private:
    static int onStopTimer(void* data);
    static int onDecayTimer(void* data);
    void       emitSyntheticScroll(double dtMs);
    void       addVelocitySample(bool vertical, uint32_t timeMs, double delta, uint32_t minSampleMs, size_t maxSamples);
    double     averagedVelocity(bool vertical, uint32_t nowMs, uint32_t relevanceMs);
    void       resetVelocityTrackers();

    double    m_velocityV             = 0.0;
    double    m_velocityH             = 0.0;
    uint32_t  m_lastEventMs           = 0;
    uint32_t  m_lastSampleMsV         = 0;
    uint32_t  m_lastSampleMsH         = 0;
    uint32_t  m_lastDecaySampleMs     = 0;
    bool      m_tracking              = false;
    bool      m_decaying              = false;
    uintptr_t m_scrollTargetWindowKey = 0;
    uintptr_t m_scrollTargetSurfaceKey = 0;

    std::deque<std::pair<uint32_t, double>> m_velocitySamplesV;
    std::deque<std::pair<uint32_t, double>> m_velocitySamplesH;

    wl_event_source* m_stopTimer  = nullptr;
    wl_event_source* m_decayTimer = nullptr;
};
