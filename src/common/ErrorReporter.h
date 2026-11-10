#ifndef COMMON_ERROR_REPORTER_H
#define COMMON_ERROR_REPORTER_H

#include "common/Logger.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct AppErrorEvent {
    uint64_t id = 0;
    LogLevel level = LogLevel::Error;
    std::string title;
    std::string message;
    std::string detail;
    std::string timestamp;
    bool showBanner = true;
};

class ErrorReporter {
public:
    static constexpr size_t kMaxPendingQueue = 64;
    static constexpr int64_t kDedupeWindowMs = 3000;

    static ErrorReporter& instance() {
        static ErrorReporter inst;
        return inst;
    }

    void reportError(const std::string& title,
                     const std::string& message,
                     const std::string& detail = "",
                     bool showBanner = true) {
        reportInternal(LogLevel::Error, title, message, detail, showBanner);
    }

    void reportWarning(const std::string& title,
                       const std::string& message,
                       const std::string& detail = "",
                       bool showBanner = false) {
        reportInternal(LogLevel::Warning, title, message, detail, showBanner);
    }

    std::vector<AppErrorEvent> pollNewErrors() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<AppErrorEvent> out(m_pending.begin(), m_pending.end());
        m_pending.clear();
        return out;
    }

    size_t getTotalErrorCount() const {
        return m_totalErrors.load(std::memory_order_relaxed);
    }

    void clearPending() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.clear();
    }

private:
    void reportInternal(LogLevel level,
                        const std::string& title,
                        const std::string& message,
                        const std::string& detail,
                        bool showBanner) {
        const auto nowSteady = std::chrono::steady_clock::now();
        const std::string key = title + "\x1f" + message;

        uint64_t eventId = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // Evict expired dedupe entries occasionally
            if (m_recentKeys.size() > 64) {
                for (auto it = m_recentKeys.begin(); it != m_recentKeys.end();) {
                    auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(nowSteady - it->second.lastTime).count();
                    if (ageMs > kDedupeWindowMs * 4) {
                        it = m_recentKeys.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            auto it = m_recentKeys.find(key);
            if (it != m_recentKeys.end()) {
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(nowSteady - it->second.lastTime).count();
                if (elapsedMs < kDedupeWindowMs) {
                    it->second.suppressed++;
                    return;
                }
                it->second.lastTime = nowSteady;
                it->second.suppressed = 0;
            } else {
                m_recentKeys.emplace(key, DedupeEntry{nowSteady, 0});
            }

            if (level == LogLevel::Error) {
                m_totalErrors.fetch_add(1, std::memory_order_relaxed);
            }
            eventId = ++m_nextId;

            auto nowWall = std::chrono::system_clock::now();
            auto timeT = std::chrono::system_clock::to_time_t(nowWall);
            struct std::tm tmNow{};
            Logger::safeLocalTime(timeT, tmNow);
            char timeBuf[16];
            std::snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);

            AppErrorEvent ev;
            ev.id = eventId;
            ev.level = level;
            ev.title = title;
            ev.message = message;
            ev.detail = detail;
            ev.timestamp = timeBuf;
            ev.showBanner = showBanner;

            m_pending.push_back(std::move(ev));
            if (m_pending.size() > kMaxPendingQueue) {
                m_pending.pop_front();
            }
        }

        if (detail.empty()) {
            sculpt_log_lvl(level, "[%s] %s\n", title.c_str(), message.c_str());
        } else {
            sculpt_log_lvl(level, "[%s] %s | Detail: %s\n", title.c_str(), message.c_str(), detail.c_str());
        }
    }

    struct DedupeEntry {
        std::chrono::steady_clock::time_point lastTime;
        size_t suppressed = 0;
    };

    mutable std::mutex m_mutex;
    std::deque<AppErrorEvent> m_pending;
    std::unordered_map<std::string, DedupeEntry> m_recentKeys;
    std::atomic<size_t> m_totalErrors{0};
    uint64_t m_nextId = 0;
};

inline void report_app_error(const std::string& title,
                             const std::string& message,
                             const std::string& detail = "",
                             bool showBanner = true) {
    ErrorReporter::instance().reportError(title, message, detail, showBanner);
}

#endif // COMMON_ERROR_REPORTER_H
