#include "common/CrashReporter.h"
#include "common/Logger.h"
#include "common/Version.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <csignal>
#include <exception>
#include <filesystem>
#include <fstream>
#include <new>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <shellapi.h>
#else
#include <unistd.h>
#include <execinfo.h>
#endif

namespace CrashReporter {

namespace {

constexpr size_t kMaxBreadcrumbs = 64;
constexpr size_t kMaxThreadSlots = 64;
constexpr size_t kLogRingCopyLines = 40;

struct BreadcrumbSlot {
    std::atomic<uint64_t> seq{0};
    uint64_t eventNumber = 0;
    double elapsedSec = 0.0;
    uint32_t threadId = 0;
    char category[24]{};
    char message[160]{};
};

struct ThreadSlot {
    std::atomic<uint32_t> threadId{0};
    char name[32]{};
    std::atomic<const char*> stage{"Idle"};
};

struct StrokeContextData {
    bool active = false;
    int brushId = -1;
    char brushName[48]{};
    uint64_t strokeSeq = 0;
    uint32_t passIndex = 0;
    uint32_t nbVerts = 0;
    uint32_t nbFaces = 0;
    uint32_t nbIVerts = 0;
    uint32_t nbIFaces = 0;
    bool dyntopo = false;
    bool symmetry = false;
    float radius = 0.0f;
    float intensity = 0.0f;
    float hitX = 0.0f;
    float hitY = 0.0f;
    float hitZ = 0.0f;
};

std::atomic<bool> g_initialized{false};
std::atomic<bool> g_headless{false};
std::atomic<uint32_t> g_mainThreadId{0};
std::atomic<const char*> g_mainThreadStage{"Startup"};
std::atomic<const char*> g_lastStage{"Startup"};
std::chrono::steady_clock::time_point g_startTime;

char g_logsDirUtf8[1024] = "logs";
#ifdef _WIN32
wchar_t g_logsDirW[1024] = L"logs";
#endif

BreadcrumbSlot g_breadcrumbs[kMaxBreadcrumbs];
std::atomic<uint64_t> g_breadcrumbWriteIdx{0};

ThreadSlot g_threadSlots[kMaxThreadSlots];

std::atomic<uint64_t> g_strokeSeqLock{0};
StrokeContextData g_strokeData;

std::atomic<int> g_crashGate{0}; // 0=idle, 1=writing
thread_local bool tl_inCrashWriter = false;
thread_local const char* tl_stage = "Startup";
thread_local int tl_threadSlot = -2; // -2=uninitialized, -1=table full

// Preallocated buffers used only inside g_crashGate == 1
char g_reportTextBuf[4096]{};
char g_recentLogBuf[kLogRingCopyLines][Logger::kCrashRingLineSize]{};
char g_reportPathOut[1024]{};
char g_dumpPathOut[1024]{};
#ifdef _WIN32
wchar_t g_reportPathW[1024]{};
wchar_t g_dumpPathW[1024]{};
wchar_t g_flagPathW[1024]{};
#endif

uint32_t currentThreadIdU32() {
#ifdef _WIN32
    return static_cast<uint32_t>(GetCurrentThreadId());
#else
    return static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

double elapsedSeconds() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - g_startTime).count();
}

void copyFixedStr(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    std::snprintf(dst, dstSize, "%s", src);
    dst[dstSize - 1] = '\0';
}

int findOrAssignThreadSlot(uint32_t tid) {
    if (tl_threadSlot >= 0 && static_cast<size_t>(tl_threadSlot) < kMaxThreadSlots) {
        if (g_threadSlots[tl_threadSlot].threadId.load(std::memory_order_relaxed) == tid) {
            return tl_threadSlot;
        }
    }
    for (size_t i = 0; i < kMaxThreadSlots; ++i) {
        if (g_threadSlots[i].threadId.load(std::memory_order_acquire) == tid) {
            tl_threadSlot = static_cast<int>(i);
            return tl_threadSlot;
        }
    }
    for (size_t i = 0; i < kMaxThreadSlots; ++i) {
        uint32_t expected = 0;
        if (g_threadSlots[i].threadId.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) {
            if (tid == g_mainThreadId.load(std::memory_order_relaxed)) {
                copyFixedStr(g_threadSlots[i].name, sizeof(g_threadSlots[i].name), "MainThread");
            } else {
                std::snprintf(g_threadSlots[i].name, sizeof(g_threadSlots[i].name), "Worker-%u", tid);
            }
            g_threadSlots[i].stage.store(tl_stage ? tl_stage : "Idle", std::memory_order_relaxed);
            tl_threadSlot = static_cast<int>(i);
            return tl_threadSlot;
        }
    }
    tl_threadSlot = -1;
    return -1;
}

StrokeContextData readStrokeSnapshotLockFree() {
    StrokeContextData copy{};
    for (int attempt = 0; attempt < 4; ++attempt) {
        uint64_t g1 = g_strokeSeqLock.load(std::memory_order_acquire);
        if ((g1 & 1ULL) != 0ULL) continue;
        std::memcpy(&copy, &g_strokeData, sizeof(StrokeContextData));
        uint64_t g2 = g_strokeSeqLock.load(std::memory_order_acquire);
        if (g1 == g2) {
            copy.brushName[sizeof(copy.brushName) - 1] = '\0';
            return copy;
        }
    }
    // Fallback dirty read if torn on active fault
    std::memcpy(&copy, &g_strokeData, sizeof(StrokeContextData));
    copy.brushName[sizeof(copy.brushName) - 1] = '\0';
    return copy;
}

#ifdef _WIN32

void writeAllToHandle(HANDLE hFile, const char* data, size_t len) {
    if (hFile == INVALID_HANDLE_VALUE || !data || len == 0) return;
    size_t offset = 0;
    while (offset < len) {
        DWORD chunk = static_cast<DWORD>((len - offset) > 65536 ? 65536 : (len - offset));
        DWORD written = 0;
        if (!WriteFile(hFile, data + offset, chunk, &written, nullptr) || written == 0) {
            break;
        }
        offset += written;
    }
}

void writeStrToHandle(HANDLE hFile, const char* str) {
    if (!str) return;
    writeAllToHandle(hFile, str, std::strlen(str));
}

void writeFmtToHandle(HANDLE hFile, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = std::vsnprintf(g_reportTextBuf, sizeof(g_reportTextBuf), fmt, args);
    va_end(args);
    if (n > 0) {
        size_t len = static_cast<size_t>(n);
        if (len >= sizeof(g_reportTextBuf)) len = sizeof(g_reportTextBuf) - 1;
        writeAllToHandle(hFile, g_reportTextBuf, len);
    }
}

const char* windowsExceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_BREAKPOINT:               return "EXCEPTION_BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DENORMAL_OPERAND:     return "EXCEPTION_FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INVALID_OPERATION:    return "EXCEPTION_FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW:             return "EXCEPTION_FLT_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:             return "EXCEPTION_INT_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION:      return "EXCEPTION_INVALID_DISPOSITION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
        case 0x20474343:                         return "GCC_UNHANDLED_CPP_EXCEPTION";
        case 0xE06D7363:                         return "MSVC_UNHANDLED_CPP_EXCEPTION";
        default:                                 return "UNKNOWN_EXCEPTION";
    }
}

const char* baseNameOfPath(const char* path) {
    if (!path) return "";
    const char* lastSlash = std::strrchr(path, '\\');
    const char* lastFwd = std::strrchr(path, '/');
    const char* best = (lastSlash > lastFwd) ? lastSlash : lastFwd;
    return best ? (best + 1) : path;
}

static uint64_t g_peDiskImageBase = 0;

uint64_t readImagePreferredBase(HMODULE mod) {
    if (!mod) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const uint8_t*>(mod) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return static_cast<uint64_t>(nt->OptionalHeader.ImageBase);
    }
    return 0;
}

uint64_t readDiskPreferredImageBase() {
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        HANDLE hExe = CreateFileW(exePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hExe != INVALID_HANDLE_VALUE) {
            uint8_t hdr[4096]{};
            DWORD bytesRead = 0;
            BOOL ok = ReadFile(hExe, hdr, sizeof(hdr), &bytesRead, nullptr);
            CloseHandle(hExe);
            if (ok && bytesRead >= sizeof(IMAGE_DOS_HEADER)) {
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hdr);
                if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew >= 0 &&
                    static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) <= bytesRead) {
                    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(hdr + dos->e_lfanew);
                    if (nt->Signature == IMAGE_NT_SIGNATURE &&
                        nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
                        return static_cast<uint64_t>(nt->OptionalHeader.ImageBase);
                    }
                }
            }
        }
    }
    return readImagePreferredBase(GetModuleHandleA(nullptr));
}

void writeStackTraceToHandle(HANDLE hFile, CONTEXT* pCtx) {
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    CONTEXT ctx{};
    if (pCtx) {
        ctx = *pCtx;
    } else {
        RtlCaptureContext(&ctx);
    }

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    const BOOL symReady = SymInitialize(process, nullptr, TRUE);

    STACKFRAME64 sf{};
#if defined(_M_X64) || defined(__x86_64__)
    const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    sf.AddrPC.Offset = ctx.Rip;
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp;
    sf.AddrStack.Mode = AddrModeFlat;
#else
    const DWORD machine = IMAGE_FILE_MACHINE_I386;
    sf.AddrPC.Offset = ctx.Eip;
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Ebp;
    sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Esp;
    sf.AddrStack.Mode = AddrModeFlat;
#endif

    alignas(IMAGEHLP_SYMBOL64) char symBuf[sizeof(IMAGEHLP_SYMBOL64) + 256]{};
    auto* symbol = reinterpret_cast<PIMAGEHLP_SYMBOL64>(symBuf);
    symbol->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
    symbol->MaxNameLength = 255;

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    HMODULE mainMod = GetModuleHandleA(nullptr);
    const uint64_t diskBase = g_peDiskImageBase ? g_peDiskImageBase : readImagePreferredBase(mainMod);

    writeStrToHandle(hFile, "\n--- Call Stack (use addr2line -e SPSculpt.exe.debug -f -C <dbg>) ---\n");
    for (int frame = 0; frame < 48; ++frame) {
        if (!StackWalk64(machine, process, thread, &sf, &ctx,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
        if (sf.AddrPC.Offset == 0) break;

        HMODULE mod = nullptr;
        char modName[128] = "unknown";
        uintptr_t rva = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(sf.AddrPC.Offset)),
                               &mod) && mod) {
            char modPath[MAX_PATH]{};
            if (GetModuleFileNameA(mod, modPath, MAX_PATH) > 0) {
                copyFixedStr(modName, sizeof(modName), baseNameOfPath(modPath));
            }
            rva = static_cast<uintptr_t>(sf.AddrPC.Offset) - reinterpret_cast<uintptr_t>(mod);
        }

        const bool isMainMod = (mod != nullptr && mod == mainMod && diskBase != 0);
        const uint64_t dbgAddr = isMainMod ? (diskBase + static_cast<uint64_t>(rva)) : 0ULL;

        const char* symName = nullptr;
        DWORD64 symDisp = 0;
        if (symReady && SymGetSymFromAddr64(process, sf.AddrPC.Offset, &symDisp, symbol)) {
            symName = symbol->Name;
        }

        DWORD lineDisp = 0;
        if (symReady && SymGetLineFromAddr64(process, sf.AddrPC.Offset, &lineDisp, &line)) {
            if (isMainMod) {
                writeFmtToHandle(hFile, "  [%02d] %s+0x%llX  pc=0x%016llX  dbg=0x%016llX  [%s+0x%llX @ %s:%lu]\n",
                                 frame, modName, static_cast<unsigned long long>(rva),
                                 static_cast<unsigned long long>(sf.AddrPC.Offset),
                                 static_cast<unsigned long long>(dbgAddr),
                                 symName ? symName : "?", static_cast<unsigned long long>(symDisp),
                                 line.FileName, static_cast<unsigned long>(line.LineNumber));
            } else {
                writeFmtToHandle(hFile, "  [%02d] %s+0x%llX  pc=0x%016llX  [%s+0x%llX @ %s:%lu]\n",
                                 frame, modName, static_cast<unsigned long long>(rva),
                                 static_cast<unsigned long long>(sf.AddrPC.Offset),
                                 symName ? symName : "?", static_cast<unsigned long long>(symDisp),
                                 line.FileName, static_cast<unsigned long>(line.LineNumber));
            }
        } else if (symName) {
            if (isMainMod) {
                writeFmtToHandle(hFile, "  [%02d] %s+0x%llX  pc=0x%016llX  dbg=0x%016llX  [%s+0x%llX]\n",
                                 frame, modName, static_cast<unsigned long long>(rva),
                                 static_cast<unsigned long long>(sf.AddrPC.Offset),
                                 static_cast<unsigned long long>(dbgAddr),
                                 symName, static_cast<unsigned long long>(symDisp));
            } else {
                writeFmtToHandle(hFile, "  [%02d] %s+0x%llX  pc=0x%016llX  [%s+0x%llX]\n",
                                 frame, modName, static_cast<unsigned long long>(rva),
                                 static_cast<unsigned long long>(sf.AddrPC.Offset),
                                 symName, static_cast<unsigned long long>(symDisp));
            }
        } else {
            if (isMainMod) {
                writeFmtToHandle(hFile, "  [%02d] %s+0x%llX  pc=0x%016llX  dbg=0x%016llX\n",
                                 frame, modName, static_cast<unsigned long long>(rva),
                                 static_cast<unsigned long long>(sf.AddrPC.Offset),
                                 static_cast<unsigned long long>(dbgAddr));
            } else {
                writeFmtToHandle(hFile, "  [%02d] %s+0x%llX  pc=0x%016llX\n",
                                 frame, modName, static_cast<unsigned long long>(rva),
                                 static_cast<unsigned long long>(sf.AddrPC.Offset));
            }
        }
    }
    FlushFileBuffers(hFile);
}

void writeMinidumpToPath(const wchar_t* dumpPath, PEXCEPTION_POINTERS exPtrs, DWORD faultingThreadId) {
    HANDLE hDump = CreateFileW(dumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hDump == INVALID_HANDLE_VALUE) {
        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    PMINIDUMP_EXCEPTION_INFORMATION pMei = nullptr;
    if (exPtrs) {
        mei.ThreadId = faultingThreadId;
        mei.ExceptionPointers = exPtrs;
        mei.ClientPointers = FALSE;
        pMei = &mei;
    }

    const MINIDUMP_TYPE richFlags = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs |
        MiniDumpWithHandleData |
        MiniDumpWithThreadInfo |
        MiniDumpWithProcessThreadData
    );

    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDump,
                                richFlags, pMei, nullptr, nullptr);
    if (!ok) {
        SetFilePointer(hDump, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(hDump);
        ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDump,
                               MiniDumpNormal, pMei, nullptr, nullptr);
    }

    FlushFileBuffers(hDump);
    CloseHandle(hDump);
    if (!ok) {
        DeleteFileW(dumpPath);
    }
}

void wideToUtf8Fixed(const wchar_t* src, char* dst, size_t dstSize) {
    if (!dst || dstSize == 0) return;
    dst[0] = '\0';
    if (!src) return;
    int len = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, static_cast<int>(dstSize), nullptr, nullptr);
    if (len <= 0) dst[0] = '\0';
    dst[dstSize - 1] = '\0';
}

struct CrashWriteRequest {
    const char* reason;
    const char* detail;
    PEXCEPTION_POINTERS exPtrs;
    DWORD faultingThreadId;
    bool writeLastCrashFlag;
    bool showModalIfInteractive;
};

void executeCrashWriteWindows(const CrashWriteRequest& req) {
    CreateDirectoryW(g_logsDirW, nullptr);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    const DWORD pid = GetCurrentProcessId();

    static std::atomic<uint32_t> s_seq{0};
    uint32_t seq = ++s_seq;

    std::swprintf(g_reportPathW, 1024, L"%ls\\crash_%04u%02u%02u_%02u%02u%02u_p%lu_%u.txt",
                  g_logsDirW, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  static_cast<unsigned long>(pid), static_cast<unsigned int>(seq));
    std::swprintf(g_dumpPathW, 1024, L"%ls\\crash_%04u%02u%02u_%02u%02u%02u_p%lu_%u.dmp",
                  g_logsDirW, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  static_cast<unsigned long>(pid), static_cast<unsigned int>(seq));
    std::swprintf(g_flagPathW, 1024, L"%ls\\last_crash.flag", g_logsDirW);

    wideToUtf8Fixed(g_reportPathW, g_reportPathOut, sizeof(g_reportPathOut));
    wideToUtf8Fixed(g_dumpPathW, g_dumpPathOut, sizeof(g_dumpPathOut));

    char tsStr[40];
    std::snprintf(tsStr, sizeof(tsStr), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    const DWORD faultTid = req.faultingThreadId ? req.faultingThreadId : GetCurrentThreadId();
    HMODULE mainMod = GetModuleHandleA(nullptr);
    uintptr_t mainBase = reinterpret_cast<uintptr_t>(mainMod);
    uint64_t preferredBase = g_peDiskImageBase ? g_peDiskImageBase : readImagePreferredBase(mainMod);

    HANDLE hReport = CreateFileW(g_reportPathW, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (hReport != INVALID_HANDLE_VALUE) {
        writeStrToHandle(hReport, "========================================================================\n");
        writeFmtToHandle(hReport, "SPSculpt Crash / Diagnostic Report (%s%s)\n",
                         Version::STRING, Version::SUFFIX);
        writeStrToHandle(hReport, "========================================================================\n");
        writeFmtToHandle(hReport, "Timestamp      : %s\n", tsStr);
        writeFmtToHandle(hReport, "Uptime         : %.3f s\n", elapsedSeconds());
        writeFmtToHandle(hReport, "PID / FaultTID : %lu / %lu\n",
                         static_cast<unsigned long>(pid), static_cast<unsigned long>(faultTid));
        writeFmtToHandle(hReport, "Reason         : %s\n", req.reason ? req.reason : "Unknown");
        if (req.detail && req.detail[0] != '\0') {
            writeFmtToHandle(hReport, "Detail         : %s\n", req.detail);
        }
        writeFmtToHandle(hReport, "Module Base    : 0x%016llX (Disk ImageBase=0x%016llX)\n",
                         static_cast<unsigned long long>(mainBase),
                         static_cast<unsigned long long>(preferredBase));
        writeFmtToHandle(hReport, "Main Stage     : %s\n", g_mainThreadStage.load(std::memory_order_relaxed));
        writeFmtToHandle(hReport, "Fault Stage    : %s\n", tl_stage ? tl_stage : "Unknown");

        // Memory status
        MEMORYSTATUSEX memStatus{};
        memStatus.dwLength = sizeof(memStatus);
        if (GlobalMemoryStatusEx(&memStatus)) {
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            pmc.cb = sizeof(pmc);
            double procWorkingMb = 0.0;
            double procPrivateMb = 0.0;
            if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&pmc), sizeof(pmc))) {
                procWorkingMb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
                procPrivateMb = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
            }
            writeFmtToHandle(hReport, "Process RAM    : WorkingSet=%.1f MB, Private=%.1f MB | SysMemLoad=%lu%% (Free=%.1f MB)\n",
                             procWorkingMb, procPrivateMb,
                             static_cast<unsigned long>(memStatus.dwMemoryLoad),
                             static_cast<double>(memStatus.ullAvailPhys) / (1024.0 * 1024.0));
        }

        if (req.exPtrs && req.exPtrs->ExceptionRecord) {
            const EXCEPTION_RECORD* er = req.exPtrs->ExceptionRecord;
            uintptr_t exAddr = reinterpret_cast<uintptr_t>(er->ExceptionAddress);
            HMODULE exMod = nullptr;
            char exModName[128] = "unknown";
            uintptr_t exRva = 0;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(exAddr), &exMod) && exMod) {
                char p[MAX_PATH]{};
                if (GetModuleFileNameA(exMod, p, MAX_PATH) > 0) {
                    copyFixedStr(exModName, sizeof(exModName), baseNameOfPath(p));
                }
                exRva = exAddr - reinterpret_cast<uintptr_t>(exMod);
            }
            writeStrToHandle(hReport, "\n--- Exception ---\n");
            writeFmtToHandle(hReport, "Code           : 0x%08lX (%s)\n",
                             static_cast<unsigned long>(er->ExceptionCode),
                             windowsExceptionName(er->ExceptionCode));
            if (exMod != nullptr && exMod == mainMod && preferredBase != 0) {
                writeFmtToHandle(hReport, "Address        : 0x%016llX (%s+0x%llX, dbg=0x%016llX)\n",
                                 static_cast<unsigned long long>(exAddr),
                                 exModName,
                                 static_cast<unsigned long long>(exRva),
                                 static_cast<unsigned long long>(preferredBase + static_cast<uint64_t>(exRva)));
            } else {
                writeFmtToHandle(hReport, "Address        : 0x%016llX (%s+0x%llX)\n",
                                 static_cast<unsigned long long>(exAddr),
                                 exModName,
                                 static_cast<unsigned long long>(exRva));
            }
            if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
                ULONG_PTR op = er->ExceptionInformation[0];
                ULONG_PTR target = er->ExceptionInformation[1];
                const char* opStr = (op == 0) ? "Read" : (op == 1 ? "Write" : (op == 8 ? "Execute" : "Unknown"));
                writeFmtToHandle(hReport, "AccessViolation: %s at address 0x%016llX\n",
                                 opStr, static_cast<unsigned long long>(target));
            }
            if (req.exPtrs->ContextRecord) {
#if defined(_M_X64) || defined(__x86_64__)
                const CONTEXT& c = *req.exPtrs->ContextRecord;
                writeFmtToHandle(hReport, "Registers x64  : RIP=0x%016llX RSP=0x%016llX RBP=0x%016llX\n",
                                 c.Rip, c.Rsp, c.Rbp);
                writeFmtToHandle(hReport, "                 RAX=0x%016llX RBX=0x%016llX RCX=0x%016llX RDX=0x%016llX\n",
                                 c.Rax, c.Rbx, c.Rcx, c.Rdx);
                writeFmtToHandle(hReport, "                 RSI=0x%016llX RDI=0x%016llX R8 =0x%016llX R9 =0x%016llX\n",
                                 c.Rsi, c.Rdi, c.R8, c.R9);
#endif
            }
        }

        // Stroke context
        StrokeContextData sc = readStrokeSnapshotLockFree();
        writeStrToHandle(hReport, "\n--- Active Stroke Context ---\n");
        writeFmtToHandle(hReport, "Active         : %s | Brush=%s (id=%d) | Seq=%llu Pass=%u\n",
                         sc.active ? "YES" : "NO",
                         sc.brushName[0] ? sc.brushName : "None",
                         sc.brushId,
                         static_cast<unsigned long long>(sc.strokeSeq),
                         sc.passIndex);
        writeFmtToHandle(hReport, "Mesh           : nbVerts=%u, nbFaces=%u | Affected: nbIVerts=%u, nbIFaces=%u\n",
                         sc.nbVerts, sc.nbFaces, sc.nbIVerts, sc.nbIFaces);
        writeFmtToHandle(hReport, "Flags/Params   : Dyntopo=%s, Sym=%s, Radius=%.4f, Intensity=%.4f, Hit=(%.3f, %.3f, %.3f)\n",
                         sc.dyntopo ? "ON" : "OFF",
                         sc.symmetry ? "ON" : "OFF",
                         sc.radius, sc.intensity, sc.hitX, sc.hitY, sc.hitZ);

        // Registered threads
        writeStrToHandle(hReport, "\n--- Tracked Threads ---\n");
        for (size_t i = 0; i < kMaxThreadSlots; ++i) {
            uint32_t tid = g_threadSlots[i].threadId.load(std::memory_order_acquire);
            if (tid != 0) {
                const char* stg = g_threadSlots[i].stage.load(std::memory_order_acquire);
                writeFmtToHandle(hReport, "  [TID %u] %-16s stage=%s\n",
                                 tid, g_threadSlots[i].name, stg ? stg : "?");
            }
        }

        // Breadcrumbs
        writeStrToHandle(hReport, "\n--- Recent Breadcrumbs (oldest -> newest) ---\n");
        const uint64_t totalBc = g_breadcrumbWriteIdx.load(std::memory_order_acquire);
        const uint64_t countBc = (totalBc < kMaxBreadcrumbs) ? totalBc : kMaxBreadcrumbs;
        const uint64_t startBc = totalBc - countBc;
        for (uint64_t i = startBc; i < totalBc; ++i) {
            const BreadcrumbSlot& slot = g_breadcrumbs[static_cast<size_t>(i % kMaxBreadcrumbs)];
            uint64_t s1 = slot.seq.load(std::memory_order_acquire);
            if ((s1 & 1ULL) != 0ULL || s1 == 0) continue;
            BreadcrumbSlot localCopy{};
            localCopy.eventNumber = slot.eventNumber;
            localCopy.elapsedSec = slot.elapsedSec;
            localCopy.threadId = slot.threadId;
            std::memcpy(localCopy.category, slot.category, sizeof(localCopy.category));
            std::memcpy(localCopy.message, slot.message, sizeof(localCopy.message));
            uint64_t s2 = slot.seq.load(std::memory_order_acquire);
            if (s1 == s2) {
                localCopy.category[sizeof(localCopy.category) - 1] = '\0';
                localCopy.message[sizeof(localCopy.message) - 1] = '\0';
                writeFmtToHandle(hReport, "  #%llu [+%.3fs] [T%u] [%s] %s\n",
                                 static_cast<unsigned long long>(localCopy.eventNumber),
                                 localCopy.elapsedSec,
                                 localCopy.threadId,
                                 localCopy.category,
                                 localCopy.message);
            }
        }

        // Recent log lines from Logger (lock-free)
        writeStrToHandle(hReport, "\n--- Recent Log Lines ---\n");
        size_t logCount = Logger::instance().copyRecentCrashLines(g_recentLogBuf, kLogRingCopyLines);
        for (size_t i = 0; i < logCount; ++i) {
            writeStrToHandle(hReport, "  ");
            writeStrToHandle(hReport, g_recentLogBuf[i]);
            writeStrToHandle(hReport, "\n");
        }

        FlushFileBuffers(hReport);

        // Write Call Stack (done after flushing context in case DbgHelp fails)
        writeStackTraceToHandle(hReport, req.exPtrs ? req.exPtrs->ContextRecord : nullptr);

        CloseHandle(hReport);
    }

    // Write binary minidump
    writeMinidumpToPath(g_dumpPathW, req.exPtrs, faultTid);

    if (req.writeLastCrashFlag) {
        HANDLE hFlag = CreateFileW(g_flagPathW, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (hFlag != INVALID_HANDLE_VALUE) {
            StrokeContextData sc = readStrokeSnapshotLockFree();
            writeFmtToHandle(hFlag, "timestamp=%s\n", tsStr);
            writeFmtToHandle(hFlag, "summary=%s%s%s\n",
                             req.reason ? req.reason : "Crash",
                             (req.detail && req.detail[0]) ? ": " : "",
                             (req.detail && req.detail[0]) ? req.detail : "");
            writeFmtToHandle(hFlag, "stage=%s\n", tl_stage ? tl_stage : g_mainThreadStage.load());
            writeFmtToHandle(hFlag, "brush=%s\n", sc.active ? sc.brushName : "None");
            writeFmtToHandle(hFlag, "report=%s\n", g_reportPathOut);
            writeFmtToHandle(hFlag, "dump=%s\n", g_dumpPathOut);
            FlushFileBuffers(hFlag);
            CloseHandle(hFlag);
        }
    }

    if (req.showModalIfInteractive && !g_headless.load(std::memory_order_acquire)) {
        wchar_t msgW[2048]{};
        wchar_t reasonW[256]{};
        wchar_t stageW[128]{};
        MultiByteToWideChar(CP_UTF8, 0, req.reason ? req.reason : "Unhandled Crash", -1, reasonW, 256);
        MultiByteToWideChar(CP_UTF8, 0, tl_stage ? tl_stage : "Unknown", -1, stageW, 128);
        std::swprintf(msgW, 2048,
                      L"SPSculpt столкнулся с критической ошибкой и завершает работу.\n\n"
                      L"Причина: %ls\n"
                      L"Этап: %ls\n\n"
                      L"Отчёт и дамп памяти сохранены:\n"
                      L"  %ls\n"
                      L"  %ls\n\n"
                      L"Отправьте оба файла разработчику для точной диагностики.",
                      reasonW, stageW, g_reportPathW, g_dumpPathW);
        MessageBoxW(nullptr, msgW, L"SPSculpt — Crash Report Saved", MB_OK | MB_ICONERROR | MB_TOPMOST);
    }
}

DWORD WINAPI stackOverflowDumpThreadProc(LPVOID param) {
    const auto* req = static_cast<const CrashWriteRequest*>(param);
    if (req) {
        executeCrashWriteWindows(*req);
    }
    return 0;
}

void writeFatalWithGate(const CrashWriteRequest& req) {
    if (tl_inCrashWriter) return;
    int expected = 0;
    if (!g_crashGate.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        // Another thread is already writing the crash dump; give it time to finish
        for (int i = 0; i < 100 && g_crashGate.load(std::memory_order_acquire) != 2; ++i) {
            Sleep(50);
        }
        return;
    }
    tl_inCrashWriter = true;

    const bool isStackOverflow = req.exPtrs && req.exPtrs->ExceptionRecord &&
                                 req.exPtrs->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW;
    if (isStackOverflow) {
        HANDLE hThread = CreateThread(nullptr, 1024 * 1024, stackOverflowDumpThreadProc,
                                      const_cast<CrashWriteRequest*>(&req), 0, nullptr);
        if (hThread) {
            WaitForSingleObject(hThread, 20000);
            CloseHandle(hThread);
        } else {
            executeCrashWriteWindows(req);
        }
    } else {
        executeCrashWriteWindows(req);
    }

    tl_inCrashWriter = false;
    g_crashGate.store(2, std::memory_order_release);
}

LONG WINAPI unhandledExceptionFilter(PEXCEPTION_POINTERS ExceptionInfo) {
    if (tl_inCrashWriter) return EXCEPTION_CONTINUE_SEARCH;

    char detailBuf[256]{};
    DWORD code = 0;
    if (ExceptionInfo && ExceptionInfo->ExceptionRecord) {
        code = ExceptionInfo->ExceptionRecord->ExceptionCode;
        std::snprintf(detailBuf, sizeof(detailBuf), "Win32 Exception 0x%08lX (%s)",
                      static_cast<unsigned long>(code), windowsExceptionName(code));
    } else {
        std::snprintf(detailBuf, sizeof(detailBuf), "Win32 Unhandled Exception");
    }

    CrashWriteRequest req{};
    req.reason = "Unhandled Hardware/OS Exception";
    req.detail = detailBuf;
    req.exPtrs = ExceptionInfo;
    req.faultingThreadId = GetCurrentThreadId();
    req.writeLastCrashFlag = true;
    req.showModalIfInteractive = true;
    writeFatalWithGate(req);

    return EXCEPTION_EXECUTE_HANDLER;
}

void purecallHandler() {
    CrashWriteRequest req{};
    req.reason = "Pure Virtual Function Call (_purecall)";
    req.detail = "Invoked pure virtual method during execution or destruction";
    req.exPtrs = nullptr;
    req.faultingThreadId = GetCurrentThreadId();
    req.writeLastCrashFlag = true;
    req.showModalIfInteractive = true;
    writeFatalWithGate(req);
    std::_Exit(3);
}

void invalidParameterHandler(const wchar_t* /*expression*/,
                             const wchar_t* /*function*/,
                             const wchar_t* /*file*/,
                             unsigned int /*line*/,
                             uintptr_t /*pReserved*/) {
    CrashWriteRequest req{};
    req.reason = "CRT Invalid Parameter Handler";
    req.detail = "C Runtime received an invalid parameter";
    req.exPtrs = nullptr;
    req.faultingThreadId = GetCurrentThreadId();
    req.writeLastCrashFlag = true;
    req.showModalIfInteractive = true;
    writeFatalWithGate(req);
    std::_Exit(3);
}

#else // POSIX fallback

void writeCrashFilePosix(const char* reason, const char* detail, bool writeFlag) {
    std::error_code ec;
    std::filesystem::create_directories(g_logsDirUtf8, ec);

    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    struct std::tm tmNow{};
    Logger::safeLocalTime(tt, tmNow);

    char baseName[128];
    std::snprintf(baseName, sizeof(baseName), "crash_%04d%02d%02d_%02d%02d%02d_p%ld.txt",
                  tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
                  tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec, static_cast<long>(getpid()));
    std::snprintf(g_reportPathOut, sizeof(g_reportPathOut), "%s/%s", g_logsDirUtf8, baseName);
    g_dumpPathOut[0] = '\0';

    std::ofstream out(g_reportPathOut, std::ios::out | std::ios::trunc);
    if (out.is_open()) {
        out << "SPSculpt Crash Report (" << Version::STRING << Version::SUFFIX << ")\n";
        out << "Reason: " << (reason ? reason : "Unknown") << "\n";
        if (detail && detail[0]) out << "Detail: " << detail << "\n";
        out << "Main Stage: " << g_mainThreadStage.load() << "\n";
        out << "Fault Stage: " << (tl_stage ? tl_stage : "Unknown") << "\n";

        StrokeContextData sc = readStrokeSnapshotLockFree();
        out << "Active Stroke: " << (sc.active ? "YES" : "NO")
            << " Brush=" << sc.brushName
            << " nbVerts=" << sc.nbVerts << " nbFaces=" << sc.nbFaces
            << " nbIVerts=" << sc.nbIVerts << " nbIFaces=" << sc.nbIFaces << "\n";

        void* frames[48]{};
        int count = backtrace(frames, 48);
        char** syms = backtrace_symbols(frames, count);
        out << "\n--- Backtrace ---\n";
        if (syms) {
            for (int i = 0; i < count; ++i) {
                out << "  [" << i << "] " << syms[i] << "\n";
            }
            std::free(syms);
        }
        out.flush();
    }

    if (writeFlag) {
        std::string flagPath = std::string(g_logsDirUtf8) + "/last_crash.flag";
        std::ofstream flag(flagPath, std::ios::out | std::ios::trunc);
        if (flag.is_open()) {
            flag << "summary=" << (reason ? reason : "Crash") << "\n";
            flag << "stage=" << (tl_stage ? tl_stage : "Unknown") << "\n";
            flag << "report=" << g_reportPathOut << "\n";
            flag.flush();
        }
    }
}

#endif // _WIN32

void terminateHandler() {
    char detail[512] = "std::terminate called";
    if (auto exPtr = std::current_exception()) {
        try {
            std::rethrow_exception(exPtr);
        } catch (const std::exception& e) {
            std::snprintf(detail, sizeof(detail), "Uncaught std::exception: %s", e.what());
        } catch (...) {
            std::snprintf(detail, sizeof(detail), "Uncaught unknown C++ exception");
        }
    }
#ifdef _WIN32
    CrashWriteRequest req{};
    req.reason = "C++ std::terminate()";
    req.detail = detail;
    req.exPtrs = nullptr;
    req.faultingThreadId = GetCurrentThreadId();
    req.writeLastCrashFlag = true;
    req.showModalIfInteractive = true;
    writeFatalWithGate(req);
#else
    writeCrashFilePosix("C++ std::terminate()", detail, true);
#endif
    std::_Exit(3);
}

void newHandler() {
#ifdef _WIN32
    CrashWriteRequest req{};
    req.reason = "Out of Memory (std::new_handler)";
    req.detail = "operator new failed to allocate memory";
    req.exPtrs = nullptr;
    req.faultingThreadId = GetCurrentThreadId();
    req.writeLastCrashFlag = false;
    req.showModalIfInteractive = false;
    writeFatalWithGate(req);
#else
    writeCrashFilePosix("Out of Memory (std::new_handler)", "operator new failed", false);
#endif
    // Throw std::bad_alloc so local error boundaries can attempt recovery cleanly
    std::set_new_handler(nullptr);
    throw std::bad_alloc();
}

void signalHandler(int sig) {
    const char* sigName = "Unknown Signal";
    switch (sig) {
        case SIGSEGV: sigName = "SIGSEGV (Segmentation Fault)"; break;
        case SIGABRT: sigName = "SIGABRT (Abort)"; break;
        case SIGFPE:  sigName = "SIGFPE (Floating Point Exception)"; break;
        case SIGILL:  sigName = "SIGILL (Illegal Instruction)"; break;
        case SIGINT:  sigName = "SIGINT (Interrupt)"; break;
        case SIGTERM: sigName = "SIGTERM (Terminate)"; break;
        default: break;
    }

    if (sig == SIGINT || sig == SIGTERM) {
        std::_Exit(1);
    }

#ifdef _WIN32
    CrashWriteRequest req{};
    req.reason = sigName;
    req.detail = "Fatal C runtime signal caught";
    req.exPtrs = nullptr;
    req.faultingThreadId = GetCurrentThreadId();
    req.writeLastCrashFlag = true;
    req.showModalIfInteractive = true;
    writeFatalWithGate(req);
#else
    writeCrashFilePosix(sigName, "Fatal POSIX signal caught", true);
#endif
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

} // namespace

void init(bool headless) {
    g_headless.store(headless, std::memory_order_release);
    if (!g_initialized.exchange(true, std::memory_order_acq_rel)) {
        g_startTime = std::chrono::steady_clock::now();
        g_mainThreadId.store(currentThreadIdU32(), std::memory_order_release);

        std::error_code ec;
        std::filesystem::path baseDir = std::filesystem::current_path(ec);
        if (ec || baseDir.empty()) baseDir = ".";
        std::filesystem::path logsPath = baseDir / "logs";
        std::filesystem::create_directories(logsPath, ec);

        copyFixedStr(g_logsDirUtf8, sizeof(g_logsDirUtf8), logsPath.string().c_str());
#ifdef _WIN32
        std::wstring logsW = logsPath.wstring();
        std::wcsncpy(g_logsDirW, logsW.c_str(), 1023);
        g_logsDirW[1023] = L'\0';

        ULONG stackGuarantee = 64 * 1024;
        SetThreadStackGuarantee(&stackGuarantee);
        g_peDiskImageBase = readDiskPreferredImageBase();
#endif
        registerCurrentThread("MainThread");
        setStage("Startup");
        recordBreadcrumb("SYSTEM", "CrashReporter initialized (v%s%s)", Version::STRING, Version::SUFFIX);
    }

    reinstallHandlers();
}

void reinstallHandlers() {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
    _set_purecall_handler(purecallHandler);
    _set_invalid_parameter_handler(invalidParameterHandler);
#endif
    std::set_terminate(terminateHandler);
    std::set_new_handler(newHandler);
    std::signal(SIGSEGV, signalHandler);
    std::signal(SIGABRT, signalHandler);
    std::signal(SIGFPE,  signalHandler);
    std::signal(SIGILL,  signalHandler);
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);
}

void setHeadless(bool headless) {
    g_headless.store(headless, std::memory_order_release);
}

bool isHeadless() {
    return g_headless.load(std::memory_order_acquire);
}

void registerCurrentThread(const char* name) {
    const uint32_t tid = currentThreadIdU32();
    const int slot = findOrAssignThreadSlot(tid);
    if (slot >= 0 && name) {
        copyFixedStr(g_threadSlots[slot].name, sizeof(g_threadSlots[slot].name), name);
    }
}

void setStage(const char* stage) {
    const char* safeStage = stage ? stage : "Unknown";
    tl_stage = safeStage;
    g_lastStage.store(safeStage, std::memory_order_relaxed);
    const uint32_t tid = currentThreadIdU32();
    if (tid == g_mainThreadId.load(std::memory_order_relaxed)) {
        g_mainThreadStage.store(safeStage, std::memory_order_relaxed);
    }
    const int slot = findOrAssignThreadSlot(tid);
    if (slot >= 0) {
        g_threadSlots[slot].stage.store(safeStage, std::memory_order_relaxed);
    }
}

const char* getStage() {
    return tl_stage ? tl_stage : "Unknown";
}

void recordBreadcrumb(const char* category, const char* fmt, ...) {
    char msgBuf[160]{};
    if (fmt) {
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
        va_end(args);
        msgBuf[sizeof(msgBuf) - 1] = '\0';
    }

    const uint64_t idx = g_breadcrumbWriteIdx.fetch_add(1, std::memory_order_acq_rel);
    BreadcrumbSlot& slot = g_breadcrumbs[static_cast<size_t>(idx % kMaxBreadcrumbs)];
    const uint64_t baseSeq = idx * 2 + 1;
    slot.seq.store(baseSeq, std::memory_order_release);
    slot.eventNumber = idx + 1;
    slot.elapsedSec = elapsedSeconds();
    slot.threadId = currentThreadIdU32();
    copyFixedStr(slot.category, sizeof(slot.category), category ? category : "APP");
    copyFixedStr(slot.message, sizeof(slot.message), msgBuf);
    slot.seq.store(baseSeq + 1, std::memory_order_release);
}

void setStrokeActive(bool active) {
    const uint64_t seq = g_strokeSeqLock.load(std::memory_order_relaxed);
    g_strokeSeqLock.store(seq + 1, std::memory_order_release);
    g_strokeData.active = active;
    g_strokeSeqLock.store(seq + 2, std::memory_order_release);
}

void updateStrokeContext(int brushId,
                         const char* brushName,
                         uint64_t strokeSeq,
                         uint32_t passIndex,
                         uint32_t nbVerts,
                         uint32_t nbFaces,
                         uint32_t nbIVerts,
                         uint32_t nbIFaces,
                         bool dyntopo,
                         bool symmetry,
                         float radius,
                         float intensity,
                         float hitX,
                         float hitY,
                         float hitZ) {
    const uint64_t seq = g_strokeSeqLock.load(std::memory_order_relaxed);
    g_strokeSeqLock.store(seq + 1, std::memory_order_release);
    g_strokeData.active = true;
    g_strokeData.brushId = brushId;
    copyFixedStr(g_strokeData.brushName, sizeof(g_strokeData.brushName), brushName ? brushName : "Unknown");
    g_strokeData.strokeSeq = strokeSeq;
    g_strokeData.passIndex = passIndex;
    g_strokeData.nbVerts = nbVerts;
    g_strokeData.nbFaces = nbFaces;
    g_strokeData.nbIVerts = nbIVerts;
    g_strokeData.nbIFaces = nbIFaces;
    g_strokeData.dyntopo = dyntopo;
    g_strokeData.symmetry = symmetry;
    g_strokeData.radius = radius;
    g_strokeData.intensity = intensity;
    g_strokeData.hitX = hitX;
    g_strokeData.hitY = hitY;
    g_strokeData.hitZ = hitZ;
    g_strokeSeqLock.store(seq + 2, std::memory_order_release);
}

void updateStrokeCounts(uint32_t nbIVerts, uint32_t nbIFaces) {
    const uint64_t seq = g_strokeSeqLock.load(std::memory_order_relaxed);
    g_strokeSeqLock.store(seq + 1, std::memory_order_release);
    g_strokeData.nbIVerts = nbIVerts;
    g_strokeData.nbIFaces = nbIFaces;
    g_strokeSeqLock.store(seq + 2, std::memory_order_release);
}

void clearStrokeContext() {
    const uint64_t seq = g_strokeSeqLock.load(std::memory_order_relaxed);
    g_strokeSeqLock.store(seq + 1, std::memory_order_release);
    g_strokeData.active = false;
    g_strokeData.passIndex = 0;
    g_strokeData.nbIVerts = 0;
    g_strokeData.nbIFaces = 0;
    g_strokeSeqLock.store(seq + 2, std::memory_order_release);
}

DiagnosticPaths writeDiagnosticSnapshot(const char* reason,
                                          const char* detail,
                                          bool writeLastCrashFlag,
                                          bool showModalIfInteractive) {
    DiagnosticPaths paths{};
    int expected = 0;
    if (!g_crashGate.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        return paths;
    }
    tl_inCrashWriter = true;

#ifdef _WIN32
    CrashWriteRequest req{};
    req.reason = reason ? reason : "Diagnostic Snapshot";
    req.detail = detail ? detail : "";
    req.exPtrs = nullptr;
    req.faultingThreadId = GetCurrentThreadId();
    req.writeLastCrashFlag = writeLastCrashFlag;
    req.showModalIfInteractive = showModalIfInteractive;
    executeCrashWriteWindows(req);
#else
    (void)showModalIfInteractive;
    writeCrashFilePosix(reason, detail, writeLastCrashFlag);
#endif

    paths.reportPath = g_reportPathOut;
    paths.dumpPath = g_dumpPathOut;

    tl_inCrashWriter = false;
    // Reset to 0 so repeated non-fatal diagnostic snapshots also succeed cleanly!
    g_crashGate.store(0, std::memory_order_release);
    return paths;
}

bool consumePreviousCrash(PreviousCrashInfo& outInfo) {
    outInfo = PreviousCrashInfo{};
    std::error_code ec;
    std::filesystem::path flagPath = std::filesystem::path(g_logsDirUtf8) / "last_crash.flag";
    if (!std::filesystem::exists(flagPath, ec)) {
        return false;
    }

    std::ifstream in(flagPath);
    if (in.is_open()) {
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                line.pop_back();
            }
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            if (k == "timestamp") outInfo.timestamp = v;
            else if (k == "summary") outInfo.summary = v;
            else if (k == "stage") outInfo.stage = v;
            else if (k == "brush") outInfo.brushName = v;
            else if (k == "report") outInfo.reportPath = v;
            else if (k == "dump") outInfo.dumpPath = v;
        }
        in.close();
        outInfo.found = true;
    }

    std::filesystem::remove(flagPath, ec);
    return outInfo.found;
}

std::string getLogsDirectory() {
    return g_logsDirUtf8;
}

bool openLogsDirectory() {
    std::error_code ec;
    std::filesystem::create_directories(g_logsDirUtf8, ec);
#ifdef _WIN32
    HINSTANCE res = ShellExecuteW(nullptr, L"open", g_logsDirW, nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<uintptr_t>(res) > 32;
#else
    std::string cmd = "xdg-open \"" + std::string(g_logsDirUtf8) + "\" >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0;
#endif
}

} // namespace CrashReporter
