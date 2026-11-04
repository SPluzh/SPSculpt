#pragma once

#include "ZBrushBridgeConfig.h"
#include "ZBrushWatcher.h"
#include "files/ImportGoZ.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace ZBrushBridge {

namespace fs = std::filesystem;

inline bool runZBrushBridgeBenchmark() {
    std::cout << "\n===================================================================================================================================================\n";
    std::cout << " SPSculpt ZBrush Bridge & Auto-Sync Benchmark (Stage 2)\n";
    std::cout << "===================================================================================================================================================\n\n";

    bool allPassed = true;

    // Test 1: Config Hierarchy & Directory Resolution
    std::cout << "[Test 1/4] Verifying Configuration & Path Resolution...\n";
    std::string defaultDir = Config::getDefaultDataDir();
    std::cout << "  - Default Exchange Directory: " << defaultDir << "\n";
    if (defaultDir.empty() || defaultDir.find("SPSculptData") == std::string::npos) {
        std::cout << "  [FAIL] Invalid default data dir!\n";
        allPassed = false;
    } else {
        std::cout << "  [PASS] Default exchange directory resolved correctly.\n";
    }

    std::string gozPath = Config::getMeshGoZPath();
    std::string camPath = Config::getCameraPath();
    std::string cmdPath = Config::getCommandScriptPath();
    if (gozPath.empty() || camPath.empty() || cmdPath.empty()) {
        std::cout << "  [FAIL] Target paths resolution failed!\n";
        allPassed = false;
    } else {
        std::cout << "  [PASS] Mesh, camera, and command script paths resolved.\n";
    }

    // Test 2: In-Process Bridge DLL Verification
    std::cout << "\n[Test 2/4] Verifying SPSculptBridge.dll ABI & Exports...\n";
#ifdef _WIN32
    const std::vector<std::string> candidateDllPaths = {
        "SPSculptBridge.dll",
        "libSPSculptBridge.dll",
        "zbrush_plugin/SPSculptBridgeData/SPSculptBridge.dll",
        "../../zbrush_plugin/SPSculptBridgeData/SPSculptBridge.dll",
        "zbrush_plugin/SPSculptBridge/SPSculptBridge.dll",
        "../../zbrush_plugin/SPSculptBridge/SPSculptBridge.dll",
        "_build_/build/SPSculptBridge.dll",
        "_build_/build/libSPSculptBridge.dll",
        "C:/Users/user/Desktop/cpp/sculptsp-native/zbrush_plugin/SPSculptBridgeData/SPSculptBridge.dll",
        "C:/Users/user/Desktop/cpp/sculptsp-native/_build_/build/SPSculptBridge.dll"
    };

    HMODULE hDll = NULL;
    std::string foundPath;
    for (const auto& path : candidateDllPaths) {
        hDll = LoadLibraryA(path.c_str());
        if (hDll) {
            foundPath = path;
            break;
        }
    }

    if (!hDll) {
        std::cout << "  [FAIL] Could not load SPSculptBridge.dll from any candidate location!\n";
        allPassed = false;
    } else {
        std::cout << "  - Loaded DLL: " << foundPath << "\n";
        auto pInstall = (FARPROC)GetProcAddress(hDll, "InstallHook");
        auto pRemove = (FARPROC)GetProcAddress(hDll, "RemoveHook");
        auto pSetDir = (FARPROC)GetProcAddress(hDll, "SetDataDir");
        auto pSetDelay = (FARPROC)GetProcAddress(hDll, "SetDebounceDelay");
        auto pTrigger = (FARPROC)GetProcAddress(hDll, "TriggerSync");
        auto pIsHook = (FARPROC)GetProcAddress(hDll, "IsHookInstalled");

        bool exportsValid = pInstall && pRemove && pSetDir && pSetDelay && pTrigger && pIsHook;
        std::cout << "  - InstallHook:       " << (pInstall ? "FOUND [OK]" : "MISSING [FAIL]") << "\n";
        std::cout << "  - RemoveHook:        " << (pRemove ? "FOUND [OK]" : "MISSING [FAIL]") << "\n";
        std::cout << "  - SetDataDir:        " << (pSetDir ? "FOUND [OK]" : "MISSING [FAIL]") << "\n";
        std::cout << "  - SetDebounceDelay:  " << (pSetDelay ? "FOUND [OK]" : "MISSING [FAIL]") << "\n";
        std::cout << "  - TriggerSync:       " << (pTrigger ? "FOUND [OK]" : "MISSING [FAIL]") << "\n";
        std::cout << "  - IsHookInstalled:   " << (pIsHook ? "FOUND [OK]" : "MISSING [FAIL]") << "\n";

        if (!exportsValid) {
            std::cout << "  [FAIL] Missing required ZScript/C-ABI exports in DLL!\n";
            allPassed = false;
        } else {
            std::cout << "  [PASS] All 6 ZScript/C-ABI functions verified.\n";
        }
        FreeLibrary(hDll);
    }
#else
    std::cout << "  - Skipped Win32 DLL verification on non-Windows platform.\n";
#endif

    // Test 3: ZBrushWatcher Live Filesystem Monitoring
    std::cout << "\n[Test 3/4] Testing ZBrushWatcher with Asynchronous ReadDirectoryChangesW...\n";
    fs::path tempWatchDir = fs::temp_directory_path() / "SPSculpt_Bridge_Test";
    fs::create_directories(tempWatchDir);

    ZBrushWatcher watcher;
    watcher.setWatchDirectory(tempWatchDir.string() + "/");
    watcher.start();

    if (!watcher.isRunning()) {
        std::cout << "  [FAIL] Watcher thread failed to start!\n";
        allPassed = false;
    } else {
        std::cout << "  - Watcher thread running on: " << watcher.getWatchDirectory() << "\n";

        // Find sample GoZ file
        std::string sampleGozPath = "C:/Users/user/Desktop/cpp/SculptForge/tests/ui/data/blender/send1/r1_000.GoZ";
        if (!fs::exists(sampleGozPath)) {
            sampleGozPath = "tests/ui/data/blender/send1/r1_000.GoZ";
        }

        if (fs::exists(sampleGozPath)) {
            std::ifstream src(sampleGozPath, std::ios::binary);
            std::vector<uint8_t> sampleData((std::istreambuf_iterator<char>(src)),
                                            std::istreambuf_iterator<char>());
            src.close();

            fs::path targetMeshFile = tempWatchDir / "mesh.GoZ";

            // Measure detection latency
            auto tWriteStart = std::chrono::high_resolution_clock::now();

            {
                std::ofstream dst(targetMeshFile, std::ios::binary);
                dst.write(reinterpret_cast<const char*>(sampleData.data()), sampleData.size());
                dst.close(); // Closed on write, triggers ReadDirectoryChangesW
            }

            // Wait for watcher to detect and read buffer
            bool detected = false;
            std::vector<uint8_t> receivedBuf;
            for (int i = 0; i < 100; ++i) { // up to 500 ms
                if (watcher.hasPendingMesh()) {
                    if (watcher.consumePendingMesh(receivedBuf)) {
                        detected = true;
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            auto tDetected = std::chrono::high_resolution_clock::now();
            double msLatency = std::chrono::duration<double, std::milli>(tDetected - tWriteStart).count();

            if (!detected || receivedBuf.empty()) {
                std::cout << "  [FAIL] Watcher did not detect mesh.GoZ update within timeout!\n";
                allPassed = false;
            } else {
                std::cout << "  - Update Detected & Read: " << std::fixed << std::setprecision(2)
                          << msLatency << " ms (Target: < 20 ms) | Buffer: " << receivedBuf.size() << " bytes\n";

                // Parse received buffer
                ImportGoZ::GoZInfo info;
                auto tParseStart = std::chrono::high_resolution_clock::now();
                auto meshes = ImportGoZ::importGoZ(receivedBuf, false, &info);
                auto tParseEnd = std::chrono::high_resolution_clock::now();
                double msParse = std::chrono::duration<double, std::milli>(tParseEnd - tParseStart).count();

                if (meshes.empty() || !meshes[0]) {
                    std::cout << "  [FAIL] Failed to parse received GoZ buffer!\n";
                    allPassed = false;
                } else {
                    Mesh* m = meshes[0];
                    std::cout << "  - In-Memory Parse Time:  " << std::fixed << std::setprecision(2)
                              << msParse << " ms (Verts: " << m->nbVerts << ", Faces: " << m->nbFaces << ")\n";
                    std::cout << "  [PASS] Async GoZ file monitoring and zero-disk UI pipeline verified.\n";
                    delete m;
                }
            }
        } else {
            std::cout << "  [WARN] Sample GoZ file not found for watcher test.\n";
        }

        // Test Camera snapshot detection
        fs::path targetCamFile = tempWatchDir / "camera.txt";
        {
            std::ofstream camOut(targetCamFile);
            camOut << "10.5,20.0,30.2,45.0,90.0,0.0,1.5\n";
            camOut.close();
        }

        bool camDetected = false;
        CameraSnapshot camSnap;
        for (int i = 0; i < 100; ++i) {
            if (watcher.hasPendingCamera()) {
                if (watcher.consumePendingCamera(camSnap)) {
                    camDetected = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (!camDetected || !camSnap.valid) {
            std::cout << "  [FAIL] Camera snapshot not detected by watcher!\n";
            allPassed = false;
        } else {
            std::cout << "  - Camera Snapshot: Pos(" << camSnap.xP << ", " << camSnap.yP << ", " << camSnap.zP
                      << ") Rot(" << camSnap.xR << ", " << camSnap.yR << ", " << camSnap.zR << ") Sc(" << camSnap.sc << ")\n";
            std::cout << "  [PASS] Camera snapshot parsing verified.\n";
        }

        watcher.stop();
        std::cout << "  - Watcher stopped cleanly.\n";
    }

    try {
        fs::remove_all(tempWatchDir);
    } catch (...) {}

    // Test 4: ZScript Plugin File Completeness
    std::cout << "\n[Test 4/4] Verifying ZBrush ZScript Plugin Template...\n";
    const std::vector<std::string> candidatePluginPaths = {
        "zbrush_plugin/SPSculptBridge.txt",
        "../../zbrush_plugin/SPSculptBridge.txt",
        "C:/Users/user/Desktop/cpp/sculptsp-native/zbrush_plugin/SPSculptBridge.txt"
    };

    std::string pluginPath;
    for (const auto& path : candidatePluginPaths) {
        if (fs::exists(path)) {
            pluginPath = path;
            break;
        }
    }

    if (pluginPath.empty()) {
        std::cout << "  [FAIL] 'zbrush_plugin/SPSculptBridge.txt' does not exist!\n";
        allPassed = false;
    } else {
        std::ifstream plg(pluginPath);
        std::string content((std::istreambuf_iterator<char>(plg)),
                            std::istreambuf_iterator<char>());
        bool hasSubPalette = content.find("ZPlugin:SPSculpt") != std::string::npos;
        bool hasSync = content.find("Sync Active") != std::string::npos;
        bool hasHook = content.find("Install Auto-Sync Hook") != std::string::npos || content.find("SPSculptBridge") != std::string::npos;

        std::cout << "  - SubPalette: " << (hasSubPalette ? "YES" : "NO") << "\n";
        std::cout << "  - Sync Active Button: " << (hasSync ? "YES" : "NO") << "\n";
        std::cout << "  - Auto-Sync / Hook Integration: " << (hasHook ? "YES" : "NO") << "\n";

        if (hasSubPalette && hasSync) {
            std::cout << "  [PASS] ZBrush plugin template verified.\n";
        } else {
            std::cout << "  [FAIL] ZBrush plugin template incomplete!\n";
            allPassed = false;
        }
    }

    std::cout << "===================================================================================================================================================\n";
    if (allPassed) {
        std::cout << " [SUCCESS] All Stage 2 ZBrush Bridge & Auto-Sync tests PASSED!\n";
    } else {
        std::cout << " [FAIL] Some Stage 2 tests FAILED!\n";
    }
    std::cout << "===================================================================================================================================================\n\n";

    return allPassed;
}

} // namespace ZBrushBridge
