#pragma once
#include <cstddef>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#include <unistd.h>
#endif

struct ProcessMemorySnapshot {
    size_t currentWorkingSetBytes = 0; // Working Set / RSS
    size_t peakWorkingSetBytes = 0;    // Peak Working Set over process lifetime
    size_t privateCommitBytes = 0;     // Private committed memory
};

inline ProcessMemorySnapshot getProcessMemorySnapshot() {
    ProcessMemorySnapshot snap;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc;
    std::memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        snap.currentWorkingSetBytes = pmc.WorkingSetSize;
        snap.peakWorkingSetBytes = pmc.PeakWorkingSetSize;
        snap.privateCommitBytes = pmc.PrivateUsage;
    }
#elif defined(__linux__)
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        snap.currentWorkingSetBytes = (size_t)usage.ru_maxrss * 1024;
        snap.peakWorkingSetBytes = snap.currentWorkingSetBytes;
    }
#elif defined(__APPLE__)
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        snap.currentWorkingSetBytes = (size_t)usage.ru_maxrss; // On macOS ru_maxrss is bytes
        snap.peakWorkingSetBytes = snap.currentWorkingSetBytes;
    }
#endif
    return snap;
}

inline size_t getAvailablePhysicalMemoryBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        return (size_t)memInfo.ullAvailPhys;
    }
#elif defined(__linux__)
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0) {
        return (size_t)pages * (size_t)pageSize;
    }
#endif
    return (size_t)16ULL * 1024ULL * 1024ULL * 1024ULL; // Fallback 16 GB
}

inline size_t getTotalPhysicalMemoryBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        return (size_t)memInfo.ullTotalPhys;
    }
#elif defined(__linux__)
    long pages = sysconf(_SC_PHYS_PAGES);
    long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0) {
        return (size_t)pages * (size_t)pageSize;
    }
#endif
    return (size_t)16ULL * 1024ULL * 1024ULL * 1024ULL; // Fallback 16 GB
}
