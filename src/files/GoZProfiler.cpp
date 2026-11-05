#include "GoZProfiler.h"
#include "files/ImportGoZ.h"
#include "mesh/Mesh.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "render/AngleRenderer.h"
#include "common/Logger.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>
#include <thread>
#include <ctime>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#include <sys/resource.h>
#endif

#include <SDL2/SDL.h>
#include <GLES3/gl3.h>

namespace GoZProfiler {

int getCpuThreadCount() {
#ifdef _WIN32
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    if (sysInfo.dwNumberOfProcessors > 0) {
        return static_cast<int>(sysInfo.dwNumberOfProcessors);
    }
#endif
    unsigned int n = std::thread::hardware_concurrency();
    return n > 0 ? static_cast<int>(n) : 1;
}

size_t getAvailableRamMB() {
#ifdef _WIN32
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        return static_cast<size_t>(memInfo.ullAvailPhys / (1024 * 1024));
    }
#elif defined(_SC_AVPHYS_PAGES) && defined(_SC_PAGESIZE)
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && pageSize > 0) {
        return static_cast<size_t>((pages * pageSize) / (1024 * 1024));
    }
#endif
    return 0;
}

size_t getPeakRssMB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<size_t>(pmc.PeakWorkingSetSize / (1024 * 1024));
    }
#elif defined(RUSAGE_SELF)
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return static_cast<size_t>(usage.ru_maxrss / 1024);
    }
#endif
    return 0;
}

std::string getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf{};
#ifdef _WIN32
    localtime_s(&tmBuf, &tt);
#else
    localtime_r(&tt, &tmBuf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf);
    return std::string(buf);
}

std::string formatNumber(uint64_t n) {
    std::string s = std::to_string(n);
    int insertPosition = static_cast<int>(s.length()) - 3;
    while (insertPosition > 0) {
        s.insert(insertPosition, ",");
        insertPosition -= 3;
    }
    return s;
}

std::vector<StepTiming> ProfileRecord::getSteps() const {
    return {
        {"[I/O] Watcher File-Ready Wait",        fileReadyWaitMs},
        {"[I/O] Disk Read to RAM Buffer",        diskReadMs},
        {"[I/O] Thread Queue Latency",           queueTransferLatencyMs},
        {"[Parser] Header & Chunk Scanning",     parseHeaderMs},
        {"[Parser] Vertices Extraction",         parseVertsMs},
        {"[Parser] Faces Extraction",            parseFacesMs},
        {"[Parser] Polypaint & sRGB Conversion", parsePaintMs},
        {"[Parser] Polygroups Extraction",       parseGroupsMs},
        {"[Parser] Quad/Tri Mesh Build",         parseFacesConversionMs},
        {"[MeshInit] Normals Computation",       normalsComputationMs},
        {"[MeshInit] Bounding Box & Cleanup",    bboxAndCleanupMs},
        {"[Scene] Old Mesh Delete & Replace",    sceneReplaceMs},
        {"[GPU] Upload VBO (Pos, Norm, Col)",    gpuUploadVboMs},
        {"[GPU] Upload EBO (Indices)",           gpuUploadEboMs},
        {"[GPU] First Frame Render & Swap",      gpuFirstFrameRenderSwapMs}
    };
}

std::string ProfileRecord::formatReport() const {
    std::ostringstream ss;
    const std::string eqLine(80, '=');
    const std::string dashLine(80, '-');

    double totalTime = totalPipelineMs > 0.0 ? totalPipelineMs : 0.0001;

    ss << eqLine << "\n";
    ss << "           SPSCULPT GoZ PIPELINE DETAILED PROFILING REPORT\n";
    ss << eqLine << "\n";
    ss << std::left << std::setw(25) << "Timestamp:" << timestamp << "\n";
    ss << std::left << std::setw(25) << "File:" << filePath << "\n";
    double sizeMB = static_cast<double>(fileSizeBytes) / 1000000.0;
    ss << std::left << std::setw(25) << "File Size:"
       << std::fixed << std::setprecision(2) << sizeMB << " MB (" << formatNumber(fileSizeBytes) << " bytes)\n";
    ss << std::left << std::setw(25) << "System Hardware:"
       << "CPU Threads: " << cpuThreads << " | Available RAM: " << availableRamMB << " MB | Peak RSS: " << peakRssMB << " MB\n";
    ss << "\n";

    ss << dashLine << "\n";
    ss << "MESH SUMMARY\n";
    ss << dashLine << "\n";
    ss << std::left << std::setw(25) << "Vertices:" << formatNumber(nbVerts) << "\n";
    ss << std::left << std::setw(25) << "Faces:" << formatNumber(nbFaces)
       << " (Quads: " << formatNumber(nbQuads) << ", Triangles: " << formatNumber(nbTriangles) << ")\n";
    ss << std::left << std::setw(25) << "Polypaint:";
    if (hasPaint) ss << "YES (" << formatNumber(nbPaintColors) << " colors)\n";
    else ss << "NO\n";
    ss << std::left << std::setw(25) << "Polygroups:";
    if (hasGroups) ss << "YES (" << formatNumber(nbGroups) << " groups)\n";
    else ss << "NO\n";
    ss << std::left << std::setw(25) << "Mask:";
    if (hasMask) ss << "YES\n";
    else ss << "NO\n";
    ss << "\n";

    ss << dashLine << "\n";
    ss << "STEP-BY-STEP BREAKDOWN\n";
    ss << dashLine << "\n";
    ss << std::left << std::setw(37) << "Phase / Step Name" << " | "
       << std::left << std::setw(13) << "Time (ms)" << " | "
       << "% of Total\n";
    ss << dashLine << "\n";

    auto steps = getSteps();
    for (size_t i = 0; i < steps.size(); ++i) {
        // Divider after I/O (step 2), Parser (step 8), MeshInit (step 10)
        if (i == 3 || i == 9 || i == 11) {
            ss << dashLine << "\n";
        }
        double pct = (steps[i].timeMs / totalTime) * 100.0;
        std::ostringstream timeStr;
        timeStr << std::fixed << std::setprecision(2) << steps[i].timeMs << " ms";
        std::ostringstream pctStr;
        pctStr << std::fixed << std::setprecision(1) << pct << "%";

        ss << std::left << std::setw(37) << steps[i].phaseAndName << " | "
           << std::right << std::setw(11) << timeStr.str() << "   | "
           << std::right << std::setw(7) << pctStr.str() << "\n";
    }

    ss << dashLine << "\n";
    std::ostringstream totalStr;
    totalStr << std::fixed << std::setprecision(2) << totalPipelineMs << " ms";
    ss << std::left << std::setw(37) << "TOTAL END-TO-END PIPELINE TIME:" << " | "
       << std::right << std::setw(11) << totalStr.str() << "   | "
       << std::right << std::setw(7) << "100.0%\n";
    ss << eqLine << "\n\n";

    // Top 3 bottlenecks to optimize
    std::vector<StepTiming> sortedSteps = steps;
    std::sort(sortedSteps.begin(), sortedSteps.end(), [](const StepTiming& a, const StepTiming& b) {
        return a.timeMs > b.timeMs;
    });

    ss << "TOP 3 BOTTLENECKS TO OPTIMIZE:\n";
    int count = 0;
    for (size_t i = 0; i < sortedSteps.size() && count < 3; ++i) {
        if (sortedSteps[i].timeMs > 0.001) {
            double pct = (sortedSteps[i].timeMs / totalTime) * 100.0;
            ss << "  " << (count + 1) << ". " << sortedSteps[i].phaseAndName << " ("
               << std::fixed << std::setprecision(2) << sortedSteps[i].timeMs << " ms, "
               << std::fixed << std::setprecision(1) << pct << "%)\n";
            count++;
        }
    }
    if (count == 0) {
        ss << "  (All steps completed under 0.00 ms)\n";
    }
    ss << eqLine << "\n";

    return ss.str();
}

bool ProfileRecord::saveReportToFile(const std::string& outputPath, bool append) const {
    std::ios_base::openmode mode = std::ios::out;
    if (append) mode |= std::ios::app;
    else mode |= std::ios::trunc;

    std::ofstream outFile(outputPath, mode);
    if (!outFile.is_open()) {
        sculpt_log("[GoZ Profiler ERROR] Failed to open report file for writing: %s\n", outputPath.c_str());
        return false;
    }
    std::string report = formatReport();
    outFile << report << "\n";
    outFile.flush();
    outFile.close();

    std::error_code ec;
    std::string absPath = std::filesystem::absolute(outputPath, ec).string();
    sculpt_log("[GoZ Profiler] Profiling report saved to: %s\n", absPath.c_str());

    // Also write a copy to the project root if running from dist or build subdirectory
    const std::vector<std::string> altDirs = {"../", "../../", "../../../"};
    for (const auto& d : altDirs) {
        if (std::filesystem::exists(d + "CMakeLists.txt")) {
            std::string rootPath = d + outputPath;
            std::ofstream rootFile(rootPath, mode);
            if (rootFile.is_open()) {
                rootFile << report << "\n";
                rootFile.flush();
                rootFile.close();
                std::string rootAbs = std::filesystem::absolute(rootPath, ec).string();
                sculpt_log("[GoZ Profiler] Profiling report copy saved to: %s\n", rootAbs.c_str());
            }
            break;
        }
    }

    return true;
}

void ProfileRecord::printToConsole() const {
    std::string report = formatReport();
    std::cout << "\n" << report << std::endl;
    std::cout.flush();
}

bool runCliBenchmark(const std::string& customPath, Scene& scene, AngleRenderer& renderer, SDL_Window* window) {
    std::string targetPath = customPath;
    if (targetPath.empty()) {
        const std::vector<std::string> candidates = {
            "models/mesh.GoZ",
            "../models/mesh.GoZ",
            "../../models/mesh.GoZ",
            "../../../models/mesh.GoZ",
            "C:/Users/Public/Documents/SPSculptData/mesh.GoZ",
            "C:/Users/user/Desktop/cpp/sculptsp-native/models/mesh.GoZ"
        };
        for (const auto& c : candidates) {
            if (std::filesystem::exists(c)) {
                targetPath = c;
                break;
            }
        }
    }

    if (targetPath.empty() || !std::filesystem::exists(targetPath)) {
        std::cerr << "[GoZ Profiler ERROR] File not found: " << targetPath << "\n";
        std::cerr << "Please provide a valid GoZ file path: SPSculpt.exe --profile-goz <path/to/mesh.GoZ>\n";
        return false;
    }

    std::error_code ec;
    std::string absPath = std::filesystem::absolute(targetPath, ec).string();
    size_t fileSizeBytes = static_cast<size_t>(std::filesystem::file_size(targetPath, ec));

    std::cout << "[GoZ Profiler] Starting end-to-end GoZ pipeline profiling for:\n  " << absPath
              << " (" << formatNumber(fileSizeBytes) << " bytes)\n";

    ProfileRecord record;
    record.filePath = absPath;
    record.fileSizeBytes = fileSizeBytes;
    record.timestamp = getCurrentTimestamp();
    record.cpuThreads = getCpuThreadCount();
    record.availableRamMB = getAvailableRamMB();

    auto tPipelineStart = std::chrono::high_resolution_clock::now();

    // Phase I: Disk Read
    auto tDiskStart = std::chrono::high_resolution_clock::now();
    std::ifstream file(targetPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[GoZ Profiler ERROR] Could not open file: " << targetPath << "\n";
        return false;
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        std::cerr << "[GoZ Profiler ERROR] Failed to read file data: " << targetPath << "\n";
        return false;
    }
    auto tDiskEnd = std::chrono::high_resolution_clock::now();
    record.diskReadMs = std::chrono::duration<double, std::milli>(tDiskEnd - tDiskStart).count();
    record.watcherDetectionLatencyMs = 0.0;
    record.fileReadyWaitMs = 0.0;
    record.queueTransferLatencyMs = 0.0;

    // Phase II: Parser
    ImportGoZ::GoZInfo info;
    ImportGoZ::GoZParserStats parserStats;
    auto meshes = ImportGoZ::importGoZ(buffer, false, &info, &parserStats);
    buffer.clear();
    buffer.shrink_to_fit();

    if (meshes.empty() || !meshes[0]) {
        std::cerr << "[GoZ Profiler ERROR] GoZ parser returned null mesh!\n";
        return false;
    }

    Mesh* mesh = meshes[0];
    record.nbVerts = mesh->nbVerts;
    record.nbFaces = mesh->nbFaces;
    record.nbQuads = info.nbQuads;
    record.nbTriangles = info.nbTriangles;
    record.hasPaint = info.hasPaint;
    record.nbPaintColors = info.hasPaint ? mesh->nbVerts : 0;
    record.hasGroups = info.hasGroups;
    record.nbGroups = info.hasGroups ? mesh->nbFaces : 0;
    record.hasMask = info.hasMask;

    record.parseHeaderMs = parserStats.headerScanMs;
    record.parseVertsMs = parserStats.vertsExtractionMs;
    record.parseFacesMs = parserStats.facesExtractionMs;
    record.parsePaintMs = parserStats.paintExtractionMs;
    record.parseGroupsMs = parserStats.groupsExtractionMs;
    record.parseMaskMs = parserStats.maskExtractionMs;
    record.parseFacesConversionMs = parserStats.facesConversionMs;

    // Phase III: MeshInit
    record.meshAllocMoveMs = parserStats.meshAllocMoveMs;
    record.normalsComputationMs = mesh->lastPostInitStats.msFaceNormals + mesh->lastPostInitStats.msVertexNormals;
    record.bboxAndCleanupMs = mesh->lastPostInitStats.msBbox;
    record.topologyOctreeCheckMs = mesh->lastPostInitStats.msTopology + mesh->lastPostInitStats.msOctree;

    // Phase IV: Scene Replace
    auto tSceneStart = std::chrono::high_resolution_clock::now();
    mesh->outlinerName = "GoZ Profile Target";
    if (!scene.getMeshes().empty()) {
        renderer.notifyMeshReplaced(scene.getMeshes()[0], mesh);
        scene.replaceMesh(scene.getMeshes()[0], mesh);
    } else {
        scene.addMesh(mesh);
    }
    scene.selectMesh(mesh);
    renderer.notifyMeshReplaced(nullptr, mesh);
    renderer.setEnableSelectionFlash(false);
    scene.getCamera().resetViewToMeshes({mesh}, false);
    auto tSceneEnd = std::chrono::high_resolution_clock::now();
    record.sceneReplaceMs = std::chrono::duration<double, std::milli>(tSceneEnd - tSceneStart).count();

    // Phase V: GPU Upload & First Frame Render & Swap
    renderer.resetLastUploadTiming();
    auto tGpuRenderStart = std::chrono::high_resolution_clock::now();

    renderer.render(scene);
    glFinish();

    auto tSwapStart = std::chrono::high_resolution_clock::now();
    if (window) {
        SDL_GL_SwapWindow(window);
    }
    auto tGpuEnd = std::chrono::high_resolution_clock::now();

    const auto& uploadTiming = renderer.getLastUploadTiming();
    record.gpuUploadVboMs = uploadTiming.msVbo;
    record.gpuUploadEboMs = uploadTiming.msEbo;
    double totalGpuFrameMs = std::chrono::duration<double, std::milli>(tGpuEnd - tGpuRenderStart).count();
    record.gpuFirstFrameRenderSwapMs = std::max(0.0, totalGpuFrameMs - uploadTiming.msTotal);

    auto tPipelineEnd = std::chrono::high_resolution_clock::now();
    record.totalPipelineMs = std::chrono::duration<double, std::milli>(tPipelineEnd - tPipelineStart).count();
    record.peakRssMB = getPeakRssMB();

    // Save report to file (append = false for fresh CLI run, or true if keeping log)
    // We save both to goz_profile_report.txt and print to console
    record.saveReportToFile("goz_profile_report.txt", false);
    record.printToConsole();

    return true;
}

} // namespace GoZProfiler
