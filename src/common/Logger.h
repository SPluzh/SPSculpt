#ifndef COMMON_LOGGER_H
#define COMMON_LOGGER_H

#include <cstdio>
#include <cstdarg>
#include <fstream>
#include <string>

inline void sculpt_log_init() {
    std::ofstream logFile("sculpt_log.txt", std::ios::out | std::ios::trunc);
    if (logFile.is_open()) {
        logFile << "[LOG INITIALIZED]\n";
        logFile.flush();
    }
}

inline void sculpt_log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    fflush(stdout);
    va_end(args);

    // Also write to file
    std::ofstream logFile("sculpt_log.txt", std::ios::out | std::ios::app);
    if (logFile.is_open()) {
        va_start(args, format);
        char buf[4096];
        vsnprintf(buf, sizeof(buf), format, args);
        logFile << buf;
        logFile.flush();
        va_end(args);
    }
}

#endif // COMMON_LOGGER_H
