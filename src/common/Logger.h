#ifndef COMMON_LOGGER_H
#define COMMON_LOGGER_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

struct LogEntry {
    LogLevel level;
    std::string timestamp;
    std::string message;
};

class Logger {
public:
    static constexpr size_t kMaxEntries = 1500;
    static constexpr size_t kMaxSessionLogFiles = 20;
    static constexpr size_t kCrashRingCapacity = 64;
    static constexpr size_t kCrashRingLineSize = 384;

    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    ~Logger() {
        if (m_logFile.is_open()) {
            m_logFile.close();
        }
        if (m_sessionLogFile.is_open()) {
            m_sessionLogFile.close();
        }
    }

    static inline void safeLocalTime(std::time_t t, struct std::tm& outTm) {
        outTm = {};
#ifdef _WIN32
        localtime_s(&outTm, &t);
#else
        localtime_r(&t, &outTm);
#endif
    }

    void init(bool headless = false) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_headless = headless;
        m_errorCount.store(0, std::memory_order_relaxed);
        m_warningCount.store(0, std::memory_order_relaxed);

        if (m_logFile.is_open()) {
            m_logFile.close();
        }
        if (m_sessionLogFile.is_open()) {
            m_sessionLogFile.close();
        }

        std::error_code ec;
        std::filesystem::path baseDir = std::filesystem::current_path(ec);
        if (ec || baseDir.empty()) {
            baseDir = ".";
        }
        m_logsDirPath = (baseDir / "logs").string();

        std::filesystem::create_directories(m_logsDirPath, ec);
        if (!ec) {
            rotateSessionLogsLocked(std::filesystem::path(m_logsDirPath));

            auto now = std::chrono::system_clock::now();
            auto timeT = std::chrono::system_clock::to_time_t(now);
            struct std::tm tmNow{};
            safeLocalTime(timeT, tmNow);

            char dateBuf[32];
            std::snprintf(dateBuf, sizeof(dateBuf), "%04d%02d%02d_%02d%02d%02d",
                          tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
                          tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);

            std::filesystem::path sessionPath = std::filesystem::path(m_logsDirPath) / (std::string("sculpt_") + dateBuf + ".log");
            int suffix = 1;
            while (std::filesystem::exists(sessionPath, ec) && suffix < 100) {
                sessionPath = std::filesystem::path(m_logsDirPath) /
                              (std::string("sculpt_") + dateBuf + "_" + std::to_string(suffix++) + ".log");
            }
            m_sessionLogPath = sessionPath.string();
            m_sessionLogFile.open(sessionPath, std::ios::out | std::ios::trunc);
            if (m_sessionLogFile.is_open()) {
                m_sessionLogFile << "[LOG INITIALIZED] Session: " << m_sessionLogPath << "\n";
                m_sessionLogFile.flush();
            }
        }

        // Legacy sculpt_log.txt kept for backwards compatibility
        m_logFile.open((baseDir / "sculpt_log.txt").string(), std::ios::out | std::ios::trunc);
        if (m_logFile.is_open()) {
            m_logFile << "[LOG INITIALIZED]\n";
            m_logFile.flush();
        }

        m_entries.clear();
        m_ringWriteIdx.store(0, std::memory_order_release);
        for (size_t i = 0; i < kCrashRingCapacity; ++i) {
            m_ringSlotSeq[i].store(0, std::memory_order_release);
            m_crashRing[i][0] = '\0';
        }
    }

    void log(LogLevel level, const char* format, va_list args) {
        if (!format) return;

        char buf[4096];
        vsnprintf(buf, sizeof(buf), format, args);
        buf[sizeof(buf) - 1] = '\0';

        std::string rawMsg(buf);
        std::string uiMsg = rawMsg;
        while (!uiMsg.empty() && (uiMsg.back() == '\n' || uiMsg.back() == '\r')) {
            uiMsg.pop_back();
        }
        std::string fileMsg = uiMsg;
        fileMsg.push_back('\n');

        auto now = std::chrono::system_clock::now();
        auto timeT = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        struct std::tm tmNow{};
        safeLocalTime(timeT, tmNow);

        char timeStr[24];
        std::snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d.%03d",
                      tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec, static_cast<int>(ms.count()));

        const char* lvlTag = "";
        switch (level) {
            case LogLevel::Debug:   lvlTag = "[DEBUG] "; break;
            case LogLevel::Info:    lvlTag = ""; break;
            case LogLevel::Warning: lvlTag = "[WARN] "; break;
            case LogLevel::Error:   lvlTag = "[ERROR] "; break;
        }

        if (level == LogLevel::Error) {
            m_errorCount.fetch_add(1, std::memory_order_relaxed);
        } else if (level == LogLevel::Warning) {
            m_warningCount.fetch_add(1, std::memory_order_relaxed);
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        // Update lock-free crash ring buffer for allocation-free post-mortem dumps
        {
            uint64_t seqIdx = m_ringWriteIdx.load(std::memory_order_relaxed);
            size_t slot = static_cast<size_t>(seqIdx % kCrashRingCapacity);
            m_ringSlotSeq[slot].store(seqIdx * 2 + 1, std::memory_order_release);
            std::snprintf(m_crashRing[slot], kCrashRingLineSize, "[%s] %s%s", timeStr, lvlTag, uiMsg.c_str());
            m_crashRing[slot][kCrashRingLineSize - 1] = '\0';
            m_ringSlotSeq[slot].store(seqIdx * 2 + 2, std::memory_order_release);
            m_ringWriteIdx.store(seqIdx + 1, std::memory_order_release);
        }

        // Output to stdout
        std::printf("[%s] %s%s", timeStr, lvlTag, fileMsg.c_str());
        std::fflush(stdout);

        // Write to session log and legacy log
        if (m_sessionLogFile.is_open()) {
            m_sessionLogFile << "[" << timeStr << "] " << lvlTag << fileMsg;
            m_sessionLogFile.flush();
        }
        if (m_logFile.is_open()) {
            m_logFile << "[" << timeStr << "] " << lvlTag << fileMsg;
            m_logFile.flush();
        }

        m_entries.push_back({level, timeStr, uiMsg});
        if (m_entries.size() > kMaxEntries) {
            m_entries.pop_front();
        }
    }

    // Thread-safe copy of log entries for UI rendering
    std::vector<LogEntry> getEntriesSnapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return std::vector<LogEntry>(m_entries.begin(), m_entries.end());
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
        m_errorCount.store(0, std::memory_order_relaxed);
        m_warningCount.store(0, std::memory_order_relaxed);
    }

    size_t getErrorCount() const {
        return m_errorCount.load(std::memory_order_relaxed);
    }

    size_t getWarningCount() const {
        return m_warningCount.load(std::memory_order_relaxed);
    }

    std::string getSessionLogPath() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_sessionLogPath;
    }

    std::string getLogsDirectory() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_logsDirPath.empty() ? std::string("logs") : m_logsDirPath;
    }

    // Wait-free / allocation-free copy of recent log lines for crash dump generation.
    // Safe to invoke from SEH/signal handlers without acquiring m_mutex.
    size_t copyRecentCrashLines(char outLines[][kCrashRingLineSize], size_t maxOut) const {
        if (!outLines || maxOut == 0) return 0;
        const uint64_t total = m_ringWriteIdx.load(std::memory_order_acquire);
        const uint64_t available = (total < kCrashRingCapacity) ? total : kCrashRingCapacity;
        const uint64_t startIdx = (available > maxOut) ? (total - maxOut) : (total - available);

        size_t written = 0;
        for (uint64_t idx = startIdx; idx < total && written < maxOut; ++idx) {
            size_t slot = static_cast<size_t>(idx % kCrashRingCapacity);
            uint64_t s1 = m_ringSlotSeq[slot].load(std::memory_order_acquire);
            if ((s1 & 1ULL) != 0ULL) continue; // currently being written by another thread
            std::memcpy(outLines[written], m_crashRing[slot], kCrashRingLineSize);
            outLines[written][kCrashRingLineSize - 1] = '\0';
            uint64_t s2 = m_ringSlotSeq[slot].load(std::memory_order_acquire);
            if (s1 == s2 && s2 != 0 && outLines[written][0] != '\0') {
                ++written;
            }
        }
        return written;
    }

private:
    static void rotateSessionLogsLocked(const std::filesystem::path& logsDir) {
        std::error_code ec;
        if (!std::filesystem::exists(logsDir, ec) || !std::filesystem::is_directory(logsDir, ec)) {
            return;
        }
        std::vector<std::filesystem::path> logFiles;
        for (const auto& entry : std::filesystem::directory_iterator(logsDir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            std::string name = entry.path().filename().string();
            if (name.rfind("sculpt_", 0) == 0 && name.size() > 4 &&
                name.compare(name.size() - 4, 4, ".log") == 0) {
                logFiles.push_back(entry.path());
            }
        }
        if (logFiles.size() < kMaxSessionLogFiles) {
            return;
        }
        std::sort(logFiles.begin(), logFiles.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
            return a.filename().string() < b.filename().string();
        });
        size_t toDelete = logFiles.size() - (kMaxSessionLogFiles - 1);
        for (size_t i = 0; i < toDelete; ++i) {
            std::filesystem::remove(logFiles[i], ec);
        }
    }

    std::deque<LogEntry> m_entries;
    std::ofstream m_logFile;
    std::ofstream m_sessionLogFile;
    std::string m_sessionLogPath;
    std::string m_logsDirPath;
    mutable std::mutex m_mutex;
    bool m_headless = false;

    std::atomic<size_t> m_errorCount{0};
    std::atomic<size_t> m_warningCount{0};

    alignas(64) char m_crashRing[kCrashRingCapacity][kCrashRingLineSize]{};
    std::atomic<uint64_t> m_ringWriteIdx{0};
    std::atomic<uint64_t> m_ringSlotSeq[kCrashRingCapacity]{};
};

inline void sculpt_log_init(bool headless = false) {
    Logger::instance().init(headless);
}

inline void sculpt_log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    Logger::instance().log(LogLevel::Info, format, args);
    va_end(args);
}

inline void sculpt_log_lvl(LogLevel level, const char* format, ...) {
    va_list args;
    va_start(args, format);
    Logger::instance().log(level, format, args);
    va_end(args);
}

inline void sculpt_log_warn(const char* format, ...) {
    va_list args;
    va_start(args, format);
    Logger::instance().log(LogLevel::Warning, format, args);
    va_end(args);
}

inline void sculpt_log_error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    Logger::instance().log(LogLevel::Error, format, args);
    va_end(args);
}

#endif // COMMON_LOGGER_H
