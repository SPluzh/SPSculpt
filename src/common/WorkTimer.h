#pragma once

#include <cstdint>
#include <string>

class WorkTimer {
public:
    WorkTimer() = default;

    void startSession();
    void stopSession();
    void reset();

    // Call every frame from main loop
    void tick(float deltaTime, bool appHasFocus, bool viewportHadActivity);

    void setTotalSeconds(uint64_t seconds);
    uint64_t getTotalSeconds() const;
    uint64_t getSessionSeconds() const;

    bool isRunning() const { return m_isRunning; }

    // Formats seconds into human-readable string: e.g. "3ч 42м" or "47м 12с" or "12с"
    static std::string format(uint64_t seconds, bool shortFormat = false);

private:
    uint64_t m_savedTotalSeconds = 0;
    double m_sessionAccumulator = 0.0;
    float m_idleTimer = 0.0f; // time elapsed since last activity
    bool m_isRunning = false;

    static constexpr float IDLE_THRESHOLD = 5.0f; // seconds of inactivity before timer pauses
};
