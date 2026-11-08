#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <cstdint>

class Scene;
class Mesh;
class AngleRenderer;
struct SDL_Window;

namespace GoZProfiler {

struct StepTiming {
    std::string phaseAndName;
    double timeMs = 0.0;
};

struct ProfileRecord {
    // File & Hardware metadata
    std::string filePath;
    size_t fileSizeBytes = 0;
    std::string timestamp;
    int cpuThreads = 0;
    size_t availableRamMB = 0;
    size_t peakRssMB = 0;

    // Mesh Summary
    size_t nbVerts = 0;
    size_t nbFaces = 0;
    size_t nbQuads = 0;
    size_t nbTriangles = 0;
    bool hasPaint = false;
    size_t nbPaintColors = 0;
    bool hasGroups = false;
    size_t nbGroups = 0;
    bool hasMask = false;

    // Phase I: Watcher & I/O
    double watcherDetectionLatencyMs = 0.0;
    double fileReadyWaitMs = 0.0;
    double diskReadMs = 0.0;
    double queueTransferLatencyMs = 0.0;

    // Phase II: Binary GoZ Parser
    double parseHeaderMs = 0.0;
    double parseVertsMs = 0.0;
    double parseFacesMs = 0.0;
    double parsePaintMs = 0.0;
    double parseGroupsMs = 0.0;
    double parseMaskMs = 0.0;
    double parseFacesConversionMs = 0.0;

    // Phase III: MeshInit
    double meshAllocMoveMs = 0.0;
    double normalsComputationMs = 0.0;
    double bboxAndCleanupMs = 0.0;
    double topologyBVHCheckMs = 0.0;

    // Phase IV: Scene
    double sceneReplaceMs = 0.0;

    // Phase V: GPU
    double gpuUploadVboMs = 0.0;
    double gpuUploadEboMs = 0.0;
    double gpuFirstFrameRenderSwapMs = 0.0;

    // End-to-end total
    double totalPipelineMs = 0.0;

    std::vector<StepTiming> getSteps() const;
    std::string formatReport() const;
    bool saveReportToFile(const std::string& outputPath = "goz_profile_report.txt", bool append = true) const;
    void printToConsole() const;
};

// Hardware info
int getCpuThreadCount();
size_t getAvailableRamMB();
size_t getPeakRssMB();
std::string getCurrentTimestamp();
std::string formatNumber(uint64_t n);

// Standalone CLI benchmark runner
bool runCliBenchmark(const std::string& customPath, Scene& scene, AngleRenderer& renderer, SDL_Window* window);

} // namespace GoZProfiler

