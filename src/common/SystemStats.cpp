#include "common/SystemStats.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#elif defined(__linux__)
#include <fstream>
#include <sstream>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace SystemStats {

std::string formatMemoryBytes(uint64_t bytes) {
    char buf[48];
    const double kib = 1024.0;
    const double mib = 1024.0 * 1024.0;
    const double gib = 1024.0 * 1024.0 * 1024.0;

    if (bytes >= static_cast<uint64_t>(gib)) {
        std::snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(bytes) / gib);
    } else if (bytes >= static_cast<uint64_t>(mib)) {
        double mb = static_cast<double>(bytes) / mib;
        if (mb >= 100.0) {
            std::snprintf(buf, sizeof(buf), "%.0f MB", mb);
        } else {
            std::snprintf(buf, sizeof(buf), "%.1f MB", mb);
        }
    } else if (bytes >= static_cast<uint64_t>(kib)) {
        std::snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / kib);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return std::string(buf);
}

#if defined(_WIN32)

namespace {

std::string wideToUtf8Local(const WCHAR* wstr) {
    if (!wstr || !wstr[0]) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

std::string toLowerAscii(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

struct DxgiVramMonitor {
    std::mutex mutex;
    HMODULE dxgiModule = nullptr;
    IDXGIAdapter3* adapter3 = nullptr;
    std::string adapterName;
    uint64_t dedicatedTotalBytes = 0;
    uint64_t sharedTotalBytes = 0;
    bool initialized = false;
    bool boundViaEglD3D11 = false;

    ~DxgiVramMonitor() {
        releaseAdapterLocked();
    }

    void releaseAdapterLocked() {
        if (adapter3) {
            adapter3->Release();
            adapter3 = nullptr;
        }
        adapterName.clear();
        dedicatedTotalBytes = 0;
        sharedTotalBytes = 0;
        boundViaEglD3D11 = false;
        initialized = false;
    }

    bool tryInitFromEglD3D11Locked() {
        EGLDisplay dpy = eglGetCurrentDisplay();
        if (dpy == EGL_NO_DISPLAY || eglGetCurrentContext() == EGL_NO_CONTEXT) {
            return false;
        }

        const char* clientExts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
        if (!clientExts || std::strstr(clientExts, "EGL_EXT_device_query") == nullptr) {
            return false;
        }

        auto pfnQueryDisplayAttribEXT = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(
            eglGetProcAddress("eglQueryDisplayAttribEXT"));
        auto pfnQueryDeviceAttribEXT = reinterpret_cast<PFNEGLQUERYDEVICEATTRIBEXTPROC>(
            eglGetProcAddress("eglQueryDeviceAttribEXT"));
        if (!pfnQueryDisplayAttribEXT || !pfnQueryDeviceAttribEXT) {
            return false;
        }

        EGLAttrib eglDevAttr = 0;
        if (!pfnQueryDisplayAttribEXT(dpy, EGL_DEVICE_EXT, &eglDevAttr) || eglDevAttr == 0) {
            eglGetError();
            return false;
        }

        EGLAttrib d3d11DevAttr = 0;
        if (!pfnQueryDeviceAttribEXT(reinterpret_cast<EGLDeviceEXT>(eglDevAttr),
                                     EGL_D3D11_DEVICE_ANGLE,
                                     &d3d11DevAttr) || d3d11DevAttr == 0) {
            eglGetError();
            return false;
        }

        IUnknown* d3d11Unknown = reinterpret_cast<IUnknown*>(static_cast<uintptr_t>(d3d11DevAttr));
        IDXGIDevice* dxgiDevice = nullptr;
        HRESULT hr = d3d11Unknown->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
        if (FAILED(hr) || !dxgiDevice) {
            return false;
        }

        IDXGIAdapter* baseAdapter = nullptr;
        hr = dxgiDevice->GetAdapter(&baseAdapter);
        dxgiDevice->Release();
        if (FAILED(hr) || !baseAdapter) {
            return false;
        }

        IDXGIAdapter3* foundAdapter3 = nullptr;
        hr = baseAdapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&foundAdapter3));
        baseAdapter->Release();
        if (FAILED(hr) || !foundAdapter3) {
            return false;
        }

        releaseAdapterLocked();
        adapter3 = foundAdapter3;
        boundViaEglD3D11 = true;
        initialized = true;

        DXGI_ADAPTER_DESC2 desc{};
        if (SUCCEEDED(adapter3->GetDesc2(&desc))) {
            adapterName = wideToUtf8Local(desc.Description);
            dedicatedTotalBytes = static_cast<uint64_t>(desc.DedicatedVideoMemory);
            sharedTotalBytes = static_cast<uint64_t>(desc.SharedSystemMemory);
        }
        return true;
    }

    bool tryInitFromDxgiFactoryLocked() {
        if (!dxgiModule) {
            dxgiModule = LoadLibraryA("dxgi.dll");
        }
        if (!dxgiModule) {
            return false;
        }

        using CreateDXGIFactory1PFN = HRESULT(WINAPI*)(REFIID, void**);
        auto pfnCreateDXGIFactory1 = reinterpret_cast<CreateDXGIFactory1PFN>(
            GetProcAddress(dxgiModule, "CreateDXGIFactory1"));
        if (!pfnCreateDXGIFactory1) {
            return false;
        }

        IDXGIFactory1* factory = nullptr;
        if (FAILED(pfnCreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
            return false;
        }

        std::string glRendererLower;
        if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
            const char* glRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            if (glRenderer) {
                glRendererLower = toLowerAscii(glRenderer);
            }
        }

        IDXGIAdapter1* bestAdapter1 = nullptr;
        IDXGIAdapter3* bestAdapter3 = nullptr;
        DXGI_ADAPTER_DESC1 bestDesc{};
        int bestScore = -1;

        for (UINT idx = 0;; ++idx) {
            IDXGIAdapter1* adapter1 = nullptr;
            if (factory->EnumAdapters1(idx, &adapter1) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (!adapter1) {
                continue;
            }

            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter1->GetDesc1(&desc))) {
                IDXGIAdapter3* cand3 = nullptr;
                const bool hasAdapter3 = SUCCEEDED(adapter1->QueryInterface(__uuidof(IDXGIAdapter3),
                                                                            reinterpret_cast<void**>(&cand3))) && cand3;
                const bool isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
                const std::string nameUtf8 = wideToUtf8Local(desc.Description);
                const std::string nameLower = toLowerAscii(nameUtf8);

                int score = isSoftware ? 0 : 10;
                if (hasAdapter3) score += 5;
                if (!glRendererLower.empty() && !nameLower.empty()) {
                    if (glRendererLower.find(nameLower) != std::string::npos) {
                        score += 100;
                    } else if ((nameLower.find("nvidia") != std::string::npos && glRendererLower.find("nvidia") != std::string::npos) ||
                               (nameLower.find("amd") != std::string::npos && glRendererLower.find("amd") != std::string::npos) ||
                               (nameLower.find("radeon") != std::string::npos && glRendererLower.find("radeon") != std::string::npos) ||
                               (nameLower.find("intel") != std::string::npos && glRendererLower.find("intel") != std::string::npos) ||
                               (nameLower.find("vmware") != std::string::npos && glRendererLower.find("vmware") != std::string::npos)) {
                        score += 50;
                    }
                }

                bool better = false;
                if (!bestAdapter1 || score > bestScore) {
                    better = true;
                } else if (score == bestScore && desc.DedicatedVideoMemory > bestDesc.DedicatedVideoMemory) {
                    better = true;
                }

                if (better) {
                    if (bestAdapter3) bestAdapter3->Release();
                    if (bestAdapter1) bestAdapter1->Release();
                    bestAdapter1 = adapter1;
                    bestAdapter1->AddRef();
                    bestAdapter3 = cand3; // steals ownership of cand3
                    bestDesc = desc;
                    bestScore = score;
                } else if (cand3) {
                    cand3->Release();
                }
            }
            adapter1->Release();
        }

        factory->Release();

        if (!bestAdapter1) {
            return false;
        }

        releaseAdapterLocked();
        adapter3 = bestAdapter3;
        adapterName = wideToUtf8Local(bestDesc.Description);
        dedicatedTotalBytes = static_cast<uint64_t>(bestDesc.DedicatedVideoMemory);
        sharedTotalBytes = static_cast<uint64_t>(bestDesc.SharedSystemMemory);
        boundViaEglD3D11 = false;
        initialized = true;
        bestAdapter1->Release();
        return true;
    }

    void query(SystemMemoryStats& stats) {
        std::lock_guard<std::mutex> lock(mutex);

        // If we haven't initialized or we initialized before EGL context was ready,
        // prefer binding directly to ANGLE's active D3D11 adapter.
        if (!boundViaEglD3D11 && eglGetCurrentContext() != EGL_NO_CONTEXT) {
            tryInitFromEglD3D11Locked();
        }
        if (!initialized) {
            tryInitFromDxgiFactoryLocked();
            initialized = true;
        }

        if (!adapterName.empty()) {
            stats.gpuAdapterName = adapterName;
            stats.vramFromEglD3D11 = boundViaEglD3D11;
            stats.vramDedicatedTotalBytes = dedicatedTotalBytes;
            stats.vramSharedTotalBytes = sharedTotalBytes;
        }

        if (!adapter3) {
            return;
        }

        DXGI_QUERY_VIDEO_MEMORY_INFO localInfo{};
        DXGI_QUERY_VIDEO_MEMORY_INFO nonLocalInfo{};
        HRESULT hrLocal = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localInfo);
        HRESULT hrNonLocal = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocalInfo);

        if (SUCCEEDED(hrLocal) || SUCCEEDED(hrNonLocal)) {
            stats.dxgiAvailable = true;
            if (SUCCEEDED(hrLocal)) {
                stats.vramProcessLocalBytes = static_cast<uint64_t>(localInfo.CurrentUsage);
                stats.vramProcessBudgetBytes = static_cast<uint64_t>(localInfo.Budget);
            }
            if (SUCCEEDED(hrNonLocal)) {
                stats.vramProcessSharedBytes = static_cast<uint64_t>(nonLocalInfo.CurrentUsage);
            }
        }
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex);
        releaseAdapterLocked();
    }
};

DxgiVramMonitor& getDxgiMonitor() {
    static DxgiVramMonitor instance;
    return instance;
}

} // namespace

void shutdownGpuMonitor() {
    getDxgiMonitor().shutdown();
}

#else

void shutdownGpuMonitor() {}

#endif // _WIN32

SystemMemoryStats sampleMemoryStats(uint64_t appMeshGpuBytes,
                                    uint64_t appRenderTargetsGpuBytes,
                                    uint64_t appTexturesGpuBytes) {
    SystemMemoryStats stats;
    stats.appMeshGpuBytes = appMeshGpuBytes;
    stats.appRenderTargetsGpuBytes = appRenderTargetsGpuBytes;
    stats.appTexturesGpuBytes = appTexturesGpuBytes;
    stats.appTrackedGpuBytes = appMeshGpuBytes + appRenderTargetsGpuBytes + appTexturesGpuBytes;

#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        stats.processWorkingSetBytes = static_cast<uint64_t>(pmc.WorkingSetSize);
        stats.processPeakWorkingSetBytes = static_cast<uint64_t>(pmc.PeakWorkingSetSize);
        stats.processPrivateCommitBytes = static_cast<uint64_t>(pmc.PrivateUsage);
    }

    MEMORYSTATUSEX memStatus{};
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        stats.systemTotalPhysBytes = static_cast<uint64_t>(memStatus.ullTotalPhys);
        stats.systemAvailPhysBytes = static_cast<uint64_t>(memStatus.ullAvailPhys);
        if (stats.systemTotalPhysBytes >= stats.systemAvailPhysBytes) {
            stats.systemUsedPhysBytes = stats.systemTotalPhysBytes - stats.systemAvailPhysBytes;
        }
        stats.systemMemoryLoadPct = static_cast<uint32_t>(memStatus.dwMemoryLoad);
    }

    getDxgiMonitor().query(stats);
#elif defined(__linux__)
    {
        std::ifstream statm("/proc/self/statm");
        long totalPages = 0, residentPages = 0;
        if (statm >> totalPages >> residentPages) {
            long pageSize = sysconf(_SC_PAGESIZE);
            if (residentPages > 0 && pageSize > 0) {
                stats.processWorkingSetBytes = static_cast<uint64_t>(residentPages) * static_cast<uint64_t>(pageSize);
            }
        }
        struct rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss > 0) {
            stats.processPeakWorkingSetBytes = static_cast<uint64_t>(usage.ru_maxrss) * 1024ULL;
        }
        if (stats.processWorkingSetBytes == 0) {
            stats.processWorkingSetBytes = stats.processPeakWorkingSetBytes;
        }

        long physPages = sysconf(_SC_PHYS_PAGES);
        long availPages = sysconf(_SC_AVPHYS_PAGES);
        long pageSize = sysconf(_SC_PAGESIZE);
        if (physPages > 0 && pageSize > 0) {
            stats.systemTotalPhysBytes = static_cast<uint64_t>(physPages) * static_cast<uint64_t>(pageSize);
            if (availPages >= 0) {
                stats.systemAvailPhysBytes = static_cast<uint64_t>(availPages) * static_cast<uint64_t>(pageSize);
                if (stats.systemTotalPhysBytes >= stats.systemAvailPhysBytes) {
                    stats.systemUsedPhysBytes = stats.systemTotalPhysBytes - stats.systemAvailPhysBytes;
                }
            }
            if (stats.systemTotalPhysBytes > 0) {
                stats.systemMemoryLoadPct = static_cast<uint32_t>((stats.systemUsedPhysBytes * 100ULL) / stats.systemTotalPhysBytes);
            }
        }
    }
#endif

    // Select primary VRAM number cleanly:
    // 1) DXGI dedicated local segment usage (real per-process GPU VRAM on Windows WDDM 2.0+)
    // 2) DXGI non-local shared GPU segment usage (for UMA/iGPUs without dedicated carveout)
    // 3) App-tracked GPU memory (meshes + RTTs + textures) if DXGI returns 0 (e.g. vGPU/VMware/non-DXGI)
    if (stats.dxgiAvailable && stats.vramProcessLocalBytes > 0) {
        stats.effectiveVramUsedBytes = stats.vramProcessLocalBytes;
        stats.vramSource = VramSourceKind::DxgiDedicated;
    } else if (stats.dxgiAvailable && stats.vramProcessSharedBytes > 0) {
        stats.effectiveVramUsedBytes = stats.vramProcessSharedBytes;
        stats.vramSource = VramSourceKind::DxgiShared;
    } else if (stats.appTrackedGpuBytes > 0) {
        stats.effectiveVramUsedBytes = stats.appTrackedGpuBytes;
        stats.vramSource = VramSourceKind::AppTracked;
    } else if (stats.glDriverVramUsedBytes > 0) {
        stats.effectiveVramUsedBytes = stats.glDriverVramUsedBytes;
        stats.vramSource = VramSourceKind::OpenGLExt;
    } else {
        stats.effectiveVramUsedBytes = 0;
        stats.vramSource = VramSourceKind::None;
    }

    return stats;
}

} // namespace SystemStats

