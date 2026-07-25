#ifndef COMMON_LOGGER_H
#define COMMON_LOGGER_H

#include <cstdio>
#include <cstdarg>
#include <fstream>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <sstream>

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
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void init() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ofstream logFile("sculpt_log.txt", std::ios::out | std::ios::trunc);
        if (logFile.is_open()) {
            logFile << "[LOG INITIALIZED]\n";
            logFile.flush();
        }
        m_entries.clear();
    }

    void log(LogLevel level, const char* format, va_list args) {
        char buf[4096];
        vsnprintf(buf, sizeof(buf), format, args);
        std::string msg(buf);

        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t_now), "%H:%M:%S")
           << '.' << std::setfill('0') << std::setw(3) << ms.count();
        std::string timeStr = ss.str();

        // Output to stdout
        printf("[%s] %s", timeStr.c_str(), msg.c_str());
        fflush(stdout);

        // Write to log file
        std::ofstream logFile("sculpt_log.txt", std::ios::out | std::ios::app);
        if (logFile.is_open()) {
            logFile << "[" << timeStr << "] " << msg;
            logFile.flush();
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.push_back({level, timeStr, msg});
        if (m_entries.size() > 1000) {
            m_entries.erase(m_entries.begin());
        }
    }

    const std::vector<LogEntry>& getEntries() const {
        return m_entries;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
    }

private:
    std::vector<LogEntry> m_entries;
    mutable std::mutex m_mutex;
};

inline void sculpt_log_init() {
    Logger::instance().init();
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

#endif // COMMON_LOGGER_H

