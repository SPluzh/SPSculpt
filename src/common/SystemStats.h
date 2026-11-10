#pragma once

#include <cstdint>
#include <string>

enum class VramSourceKind {
    None = 0,
    DxgiDedicated,  // DXGI IDXGIAdapter3 LOCAL CurrentUsage (per-process dedicated VRAM)
    DxgiShared,     // DXGI IDXGIAdapter3 NON_LOCAL CurrentUsage (per-process shared/UMA GPU memory)
    OpenGLExt,      // GL_NVX_gpu_memory_info / GL_ATI_meminfo fallback
    AppTracked      // AngleRenderer tracked GPU buffers + RTT FBOs + textures
};

struct SystemMemoryStats {
    // Process RAM
    uint64_t processWorkingSetBytes = 0;      // Current physical RAM (Working Set / RSS)
    uint64_t processPeakWorkingSetBytes = 0;  // Peak physical RAM over process lifetime
    uint64_t processPrivateCommitBytes = 0;   // Private committed bytes (Windows)

    // System RAM
    uint64_t systemTotalPhysBytes = 0;
    uint64_t systemAvailPhysBytes = 0;
    uint64_t systemUsedPhysBytes = 0;
    uint32_t systemMemoryLoadPct = 0;

    // GPU / VRAM (OS/driver telemetry when available)
    std::string gpuAdapterName;
    bool dxgiAvailable = false;
    bool vramFromEglD3D11 = false;
    uint64_t vramDedicatedTotalBytes = 0;
    uint64_t vramSharedTotalBytes = 0;
    uint64_t vramProcessLocalBytes = 0;
    uint64_t vramProcessBudgetBytes = 0;
    uint64_t vramProcessSharedBytes = 0;
    uint64_t glDriverVramUsedBytes = 0;

    // Application-tracked GPU allocations (from AngleRenderer)
    uint64_t appMeshGpuBytes = 0;
    uint64_t appRenderTargetsGpuBytes = 0;
    uint64_t appTexturesGpuBytes = 0;
    uint64_t appTrackedGpuBytes = 0;

    // Selected VRAM usage for compact HUD display
    uint64_t effectiveVramUsedBytes = 0;
    VramSourceKind vramSource = VramSourceKind::None;
};

namespace SystemStats {

// Samples process RAM, system RAM, and GPU VRAM telemetry.
SystemMemoryStats sampleMemoryStats(uint64_t appMeshGpuBytes = 0,
                                    uint64_t appRenderTargetsGpuBytes = 0,
                                    uint64_t appTexturesGpuBytes = 0);

// Releases cached GPU adapter handles cleanly before context teardown.
void shutdownGpuMonitor();

// Formats byte counts compactly (e.g. "512 KB", "248 MB", "1.42 GB").
std::string formatMemoryBytes(uint64_t bytes);

} // namespace SystemStats

