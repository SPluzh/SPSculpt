#include "common/WorkTimer.h"
#include <cstdio>
#include <cmath>

void WorkTimer::startSession() {
    m_idleTimer = 0.0f;
    m_isRunning = true;
}

void WorkTimer::stopSession() {
    m_isRunning = false;
}

void WorkTimer::reset() {
    m_savedTotalSeconds = 0;
    m_sessionAccumulator = 0.0;
    m_idleTimer = 0.0f;
    m_isRunning = false;
}

void WorkTimer::tick(float deltaTime, bool appHasFocus, bool viewportHadActivity) {
    if (!appHasFocus) {
        m_isRunning = false;
        return;
    }

    if (viewportHadActivity) {
        m_idleTimer = 0.0f;
    } else {
        m_idleTimer += deltaTime;
    }

    if (m_idleTimer < IDLE_THRESHOLD) {
        m_isRunning = true;
        if (deltaTime > 0.0f && deltaTime < 1.0f) {
            m_sessionAccumulator += static_cast<double>(deltaTime);
        }
    } else {
        m_isRunning = false;
    }
}

void WorkTimer::setTotalSeconds(uint64_t seconds) {
    m_savedTotalSeconds = seconds;
}

uint64_t WorkTimer::getTotalSeconds() const {
    return m_savedTotalSeconds + static_cast<uint64_t>(m_sessionAccumulator);
}

uint64_t WorkTimer::getSessionSeconds() const {
    return static_cast<uint64_t>(m_sessionAccumulator);
}

std::string WorkTimer::format(uint64_t seconds, bool shortFormat) {
    uint64_t hours = seconds / 3600;
    uint64_t minutes = (seconds % 3600) / 60;
    uint64_t secs = seconds % 60;

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu",
                  static_cast<unsigned long long>(hours),
                  static_cast<unsigned long long>(minutes),
                  static_cast<unsigned long long>(secs));
    return std::string(buf);
}
