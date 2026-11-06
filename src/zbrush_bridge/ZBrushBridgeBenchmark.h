#pragma once

#include "ZBrushBridgeConfig.h"
#include "ZBrushWatcher.h"
#include "ZBrushSender.h"
#include "files/ImportGoZ.h"
#include "files/ExportGoZ.h"
#include "mesh/Mesh.h"
#include "scene/Scene.h"

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
    std::cout << " SPSculpt ZBrush Full Scene Round-Trip Bridge Benchmark (Stage 5 Verification)\n";
    std::cout << "===================================================================================================================================================\n\n";

    bool allPassed = true;

    // --------------------------------------------------------------------------------------------------
    // Test 1: Config Hierarchy & Directory Resolution
    // --------------------------------------------------------------------------------------------------
    std::cout << "[Test 1/5] Verifying Configuration & Path Resolution...\n";
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
    std::string manifestPath = Config::getSceneManifestPath();
    std::string subtoolsDir = Config::getSubtoolsDir();
    std::string outSubtoolsDir = Config::getOutSubtoolsDir();
    std::string importScenePath = Config::getImportSceneScriptPath();

    if (gozPath.empty() || camPath.empty() || cmdPath.empty() ||
        manifestPath.empty() || subtoolsDir.empty() || outSubtoolsDir.empty() || importScenePath.empty()) {
        std::cout << "  [FAIL] Target paths resolution failed!\n";
        allPassed = false;
    } else {
        std::cout << "  - Manifest Path:        " << manifestPath << "\n";
        std::cout << "  - SubTools In Dir:      " << subtoolsDir << "\n";
        std::cout << "  - SubTools Out Dir:     " << outSubtoolsDir << "\n";
        std::cout << "  - Import Script Path:   " << importScenePath << "\n";
        std::cout << "  - Scene Debounce Delay: " << Config::getSceneDebounceDelayMs() << " ms\n";
        std::cout << "  - Auto-delete Removed:  " << (Config::isAutoDeleteRemovedSubTools() ? "TRUE" : "FALSE") << "\n";
        std::cout << "  [PASS] All 7 exchange path endpoints and settings resolved.\n";
    }

    // --------------------------------------------------------------------------------------------------
    // Test 2: In-Process Bridge DLL Verification (All 11 Exports & Function Execution)
    // --------------------------------------------------------------------------------------------------
    std::cout << "\n[Test 2/5] Verifying SPSculptBridge.dll ABI & Exports (11 Functions)...\n";
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
        "_build_/dist/SPSculptBridge.dll",
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
        // Standard ZScript FileExecute ABI:
        // float Func(char* pText, double optValue, char* pOptBuffer1, int* pOptBuffer1Size, char* pOptBuffer2, int* pOptBuffer2Size)
        typedef float (*ZScriptDllFunc)(char* pText, double optValue, char* b1, int* s1, char* b2, int* s2);

        auto pInstall      = (ZScriptDllFunc)GetProcAddress(hDll, "InstallHook");
        auto pRemove       = (ZScriptDllFunc)GetProcAddress(hDll, "RemoveHook");
        auto pSetDir       = (ZScriptDllFunc)GetProcAddress(hDll, "SetDataDir");
        auto pSetDelay     = (ZScriptDllFunc)GetProcAddress(hDll, "SetDebounceDelay");
        auto pTrigger      = (ZScriptDllFunc)GetProcAddress(hDll, "TriggerSync");
        auto pIsHook       = (ZScriptDllFunc)GetProcAddress(hDll, "IsHookInstalled");
        auto pBeginScene   = (ZScriptDllFunc)GetProcAddress(hDll, "BeginSceneManifest");
        auto pCheckDirty   = (ZScriptDllFunc)GetProcAddress(hDll, "CheckSubToolDirty");
        auto pAddEntry     = (ZScriptDllFunc)GetProcAddress(hDll, "AddManifestEntry");
        auto pSaveManifest = (ZScriptDllFunc)GetProcAddress(hDll, "SaveSceneManifest");
        auto pTriggerScene = (ZScriptDllFunc)GetProcAddress(hDll, "TriggerSceneImportScript");

        bool exportsValid = pInstall && pRemove && pSetDir && pSetDelay && pTrigger && pIsHook &&
                            pBeginScene && pCheckDirty && pAddEntry && pSaveManifest && pTriggerScene;

        std::cout << "  - InstallHook:              " << (pInstall ? "FOUND [OK]" : "MISSING [FAIL]") << "\n";
        std::cout << "  - RemoveHook:               " << (pRemove ? "FOUND [OK]" : "MISSING [FAIL]") << "\n";
        std::cout << "  - SetDataDir:               " << (pSetDir ? "FOUND [OK]" : "MISSING [FAIL]") << "\n";
        std::cout << "  - SetDebounceDelay:         " << (pSetDelay ? "FOUND [OK]" : "MISSING [FAIL]") << "\n";
        std::cout << "  - TriggerSync:              " << (pTrigger ? "FOUND [OK]" : "MISSING [FAIL]") << "\n";
        std::cout << "  - IsHookInstalled:          " << (pIsHook ? "FOUND [OK]" : "MISSING [FAIL]") << "\n";
        std::cout << "  - BeginSceneManifest:       " << (pBeginScene ? "FOUND [OK]" : "MISSING [FAIL]") << "\n";
        std::cout << "  - CheckSubToolDirty:        " << (pCheckDirty ? "FOUND [OK]" : "MISSING [FAIL]") << "\n";
        std::cout << "  - AddManifestEntry:         " << (pAddEntry ? "FOUND [OK]" : "MISSING [FAIL]") << "\n";
        std::cout << "  - SaveSceneManifest:        " << (pSaveManifest ? "FOUND [OK]" : "MISSING [FAIL]") << "\n";
        std::cout << "  - TriggerSceneImportScript: " << (pTriggerScene ? "FOUND [OK]" : "MISSING [FAIL]") << "\n";

        if (!exportsValid) {
            std::cout << "  [FAIL] Missing required ZScript/C-ABI exports in DLL!\n";
            allPassed = false;
        } else {
            std::cout << "  [PASS] All 11 ZScript/C-ABI functions verified.\n";

            // Test execution of CheckSubToolDirty logic
            char toolName[64] = "BenchTestSubtool";
            float d1 = pCheckDirty(toolName, 100.0, nullptr, nullptr, nullptr, nullptr); // initial call -> 1.0f (dirty)
            float d2 = pCheckDirty(toolName, 100.0, nullptr, nullptr, nullptr, nullptr); // same counter -> 0.0f (clean)
            float d3 = pCheckDirty(toolName, 101.0, nullptr, nullptr, nullptr, nullptr); // counter changed -> 1.0f (dirty)

            if (d1 == 1.0f && d2 == 0.0f && d3 == 1.0f) {
                std::cout << "  [PASS] Differential dirty-check cache verified (1.0 -> 0.0 -> 1.0).\n";
            } else {
                std::cout << "  [FAIL] Differential dirty check returned unexpected values ("
                          << d1 << ", " << d2 << ", " << d3 << ")!\n";
                allPassed = false;
            }

            // Test manifest generation via DLL
            fs::path tempDllDir = fs::temp_directory_path() / "SPSculpt_DLL_ManifestTest";
            fs::create_directories(tempDllDir);
            char dirBuf[512];
            strncpy(dirBuf, (tempDllDir.string() + "/").c_str(), sizeof(dirBuf));
            pSetDir(dirBuf, 0.0, nullptr, nullptr, nullptr, nullptr);

            char dummy[32] = "";
            pBeginScene(dummy, 2.0, nullptr, nullptr, nullptr, nullptr);
            char entry1[256] = "0|1|5|subtools/subtool_0.GoZ|Body";
            char entry2[256] = "1|0|10|subtools/subtool_1.GoZ|Armor";
            pAddEntry(entry1, 0.0, nullptr, nullptr, nullptr, nullptr);
            pAddEntry(entry2, 1.0, nullptr, nullptr, nullptr, nullptr);
            pSaveManifest(dummy, 0.0, nullptr, nullptr, nullptr, nullptr);

            fs::path genManifest = tempDllDir / "scene_manifest.json";
            if (fs::exists(genManifest) && fs::file_size(genManifest) > 20) {
                std::cout << "  [PASS] DLL scene manifest generation and atomic commit verified ("
                          << fs::file_size(genManifest) << " bytes).\n";
            } else {
                std::cout << "  [FAIL] DLL did not generate scene_manifest.json!\n";
                allPassed = false;
            }

            try { fs::remove_all(tempDllDir); } catch (...) {}
        }
        FreeLibrary(hDll);
    }
#else
    std::cout << "  - Skipped Win32 DLL verification on non-Windows platform.\n";
#endif

    // --------------------------------------------------------------------------------------------------
    // Test 3: ZBrushWatcher Live Single-Mesh Filesystem Monitoring
    // --------------------------------------------------------------------------------------------------
    std::cout << "\n[Test 3/5] Testing ZBrushWatcher with Asynchronous ReadDirectoryChangesW...\n";
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
            auto tWriteStart = std::chrono::high_resolution_clock::now();

            {
                std::ofstream dst(targetMeshFile, std::ios::binary);
                dst.write(reinterpret_cast<const char*>(sampleData.data()), sampleData.size());
                dst.close();
            }

            bool detected = false;
            std::vector<uint8_t> receivedBuf;
            for (int i = 0; i < 100; ++i) {
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
                          << msLatency << " ms | Buffer: " << receivedBuf.size() << " bytes\n";

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
                    std::cout << "  [PASS] Async GoZ file monitoring verified.\n";
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
    }

    try { fs::remove_all(tempWatchDir); } catch (...) {}

    // --------------------------------------------------------------------------------------------------
    // Test 4: ZScript Plugin File Completeness (Multi-SubTool & Full Scene)
    // --------------------------------------------------------------------------------------------------
    std::cout << "\n[Test 4/5] Verifying ZBrush ZScript Plugin Template Completeness...\n";
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
        bool hasSyncActive = content.find("Sync Active") != std::string::npos;
        bool hasSyncVisible = content.find("Sync Visible") != std::string::npos;
        bool hasSyncScene = content.find("Sync Scene") != std::string::npos;
        bool hasExportRoutine = content.find("ExportFullScene") != std::string::npos;
        bool hasFreeze = content.find("[IFreeze,") != std::string::npos;

        std::cout << "  - SubPalette:                  " << (hasSubPalette ? "YES [OK]" : "NO [FAIL]") << "\n";
        std::cout << "  - 'Sync Active' Button:        " << (hasSyncActive ? "YES [OK]" : "NO [FAIL]") << "\n";
        std::cout << "  - 'Sync Visible' Button:       " << (hasSyncVisible ? "YES [OK]" : "NO [FAIL]") << "\n";
        std::cout << "  - 'Sync Scene' Button:         " << (hasSyncScene ? "YES [OK]" : "NO [FAIL]") << "\n";
        std::cout << "  - 'ExportFullScene' Routine:   " << (hasExportRoutine ? "YES [OK]" : "NO [FAIL]") << "\n";
        std::cout << "  - Viewport Freeze Protection:  " << (hasFreeze ? "YES [OK]" : "NO [FAIL]") << "\n";

        if (hasSubPalette && hasSyncActive && hasSyncVisible && hasSyncScene && hasExportRoutine && hasFreeze) {
            std::cout << "  [PASS] Full scene ZScript plugin template verified.\n";
        } else {
            std::cout << "  [FAIL] ZBrush plugin template incomplete!\n";
            allPassed = false;
        }
    }

    // --------------------------------------------------------------------------------------------------
    // Test 5: Multi-SubTool Scene Synchronization & Round-Trip End-to-End
    // --------------------------------------------------------------------------------------------------
    std::cout << "\n[Test 5/5] Multi-SubTool Scene Synchronization & Round-Trip End-to-End...\n";
    fs::path tempSceneDir = fs::temp_directory_path() / "SPSculpt_Scene_Benchmark";
    try {
        fs::remove_all(tempSceneDir);
    } catch (...) {}
    fs::create_directories(tempSceneDir);
    fs::create_directories(tempSceneDir / "subtools");

    std::string origDir = Config::getDataDir();
    Config::setDataDir(tempSceneDir.string() + "/");

    // 1. Create synthetic meshes
    Mesh* m0 = new Mesh();
    m0->outlinerName = "SubTool_Torso";
    m0->verts = {
        -1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f
    };
    m0->nbVerts = 8;
    m0->faces = {
        0, 1, 2, 3,
        4, 7, 6, 5,
        0, 4, 5, 1,
        2, 6, 7, 3,
        0, 3, 7, 4,
        1, 5, 6, 2
    };
    m0->nbFaces = 6;
    m0->faceGroups = { 1, 1, 2, 2, 3, 3 };

    Mesh* m1 = new Mesh();
    m1->outlinerName = "SubTool_Head";
    m1->verts = {
         0.0f,  2.0f,  0.0f,
        -1.0f,  0.0f, -1.0f,
         1.0f,  0.0f, -1.0f,
         0.0f,  0.0f,  1.0f
    };
    m1->nbVerts = 4;
    m1->faces = {
        0, 1, 2, 0xFFFFFFFFu,
        0, 2, 3, 0xFFFFFFFFu,
        0, 3, 1, 0xFFFFFFFFu,
        1, 3, 2, 0xFFFFFFFFu
    };
    m1->nbFaces = 4;
    m1->faceGroups = { 10, 10, 20, 20 };

    Mesh* m2 = new Mesh();
    m2->outlinerName = "SubTool_Armor";
    m2->verts = {
        -2.0f, 0.0f, 0.0f,
         2.0f, 0.0f, 0.0f,
         0.0f, 2.0f, 0.0f,
         0.0f,-2.0f, 0.0f,
         0.0f, 0.0f, 2.0f
    };
    m2->nbVerts = 5;
    m2->faces = {
        0, 1, 2, 0xFFFFFFFFu,
        0, 3, 1, 4
    };
    m2->nbFaces = 2;
    m2->faceGroups = { 30, 40 };

    // 2. Export meshes to subtools/
    fs::path f0 = tempSceneDir / "subtools" / "subtool_0.GoZ";
    fs::path f1 = tempSceneDir / "subtools" / "subtool_1.GoZ";
    fs::path f2 = tempSceneDir / "subtools" / "subtool_2.GoZ";

    bool exp0 = ExportGoZ::exportGoZ(f0.string(), m0);
    bool exp1 = ExportGoZ::exportGoZ(f1.string(), m1);
    bool exp2 = ExportGoZ::exportGoZ(f2.string(), m2);

    if (!exp0 || !exp1 || !exp2) {
        std::cout << "  [FAIL] Failed to export synthetic GoZ subtools!\n";
        allPassed = false;
    } else {
        std::cout << "  - Exported 3 GoZ SubTools ("
                  << fs::file_size(f0) << ", " << fs::file_size(f1) << ", " << fs::file_size(f2) << " bytes)\n";

        // 3. Start ZBrushWatcher on tempSceneDir
        ZBrushWatcher sceneWatcher;
        sceneWatcher.setWatchDirectory(tempSceneDir.string() + "/");
        sceneWatcher.start();

        // 4. Write scene_manifest.json (atomic tmp rename)
        fs::path tmpManifest = tempSceneDir / "scene_manifest.json.tmp";
        fs::path finalManifest = tempSceneDir / "scene_manifest.json";
        {
            std::ofstream man(tmpManifest);
            man << "{\n";
            man << "  \"version\": 1,\n";
            man << "  \"activeIndex\": 1,\n";
            man << "  \"timestamp\": 1000,\n";
            man << "  \"subtools\": [\n";
            man << "    { \"id\": 0, \"name\": \"SubTool_Torso\", \"visible\": true, \"undoCounter\": 10, \"file\": \"subtools/subtool_0.GoZ\" },\n";
            man << "    { \"id\": 1, \"name\": \"SubTool_Head\", \"visible\": true, \"undoCounter\": 20, \"file\": \"subtools/subtool_1.GoZ\" },\n";
            man << "    { \"id\": 2, \"name\": \"SubTool_Armor\", \"visible\": false, \"undoCounter\": 30, \"file\": \"subtools/subtool_2.GoZ\" }\n";
            man << "  ]\n";
            man << "}\n";
            man.close();
        }
        std::error_code ec;
        fs::rename(tmpManifest, finalManifest, ec);

        // 5. Wait for watcher to detect and consume scene snapshot
        bool sceneDetected = false;
        SceneSnapshot sceneSnap;
        auto tWaitStart = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 300; ++i) { // up to 1500 ms (accounting for debounce)
            if (sceneWatcher.hasPendingScene()) {
                if (sceneWatcher.consumePendingScene(sceneSnap)) {
                    sceneDetected = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        auto tWaitEnd = std::chrono::high_resolution_clock::now();
        double msSceneLatency = std::chrono::duration<double, std::milli>(tWaitEnd - tWaitStart).count();

        if (!sceneDetected || sceneSnap.subtools.size() != 3) {
            std::cout << "  [FAIL] Watcher failed to detect scene_manifest.json or pre-read 3 SubTools! (Detected: "
                      << sceneSnap.subtools.size() << ")\n";
            allPassed = false;
        } else {
            std::cout << "  - Scene Manifest Detected: " << std::fixed << std::setprecision(2)
                      << msSceneLatency << " ms | SubTools: " << sceneSnap.subtools.size()
                      << " | Active Index: " << sceneSnap.activeIndex << "\n";

            bool snapPropsValid = (sceneSnap.subtools[0].name == "SubTool_Torso" && sceneSnap.subtools[0].visible == true) &&
                                  (sceneSnap.subtools[1].name == "SubTool_Head"  && sceneSnap.subtools[1].visible == true) &&
                                  (sceneSnap.subtools[2].name == "SubTool_Armor" && sceneSnap.subtools[2].visible == false);

            if (!snapPropsValid) {
                std::cout << "  [FAIL] Snapshot subtool properties (name/visibility) mismatch!\n";
                allPassed = false;
            } else {
                std::cout << "  [PASS] SubTool names, visibility, and hierarchy preserved.\n";
            }

            // Verify preloaded in-memory buffers
            bool buffersValid = true;
            for (size_t i = 0; i < sceneSnap.subtools.size(); ++i) {
                const auto& st = sceneSnap.subtools[i];
                if (st.buffer.empty()) {
                    buffersValid = false;
                    break;
                }
                auto parsed = ImportGoZ::importGoZ(st.buffer, false);
                if (parsed.empty() || !parsed[0]) {
                    buffersValid = false;
                    break;
                }
                delete parsed[0];
            }

            if (buffersValid) {
                std::cout << "  [PASS] All 3 SubTool GoZ buffers pre-read into RAM and parsed successfully.\n";
            } else {
                std::cout << "  [FAIL] In-memory GoZ buffer verification failed!\n";
                allPassed = false;
            }
        }
        sceneWatcher.stop();

        // 6. Test round-trip export from SPSculpt to ZBrush (sendSceneToZBrush)
        Scene testScene;
        testScene.addMesh(m0);
        testScene.addMesh(m1);

        auto sendRes = sendSceneToZBrush(testScene, false);
        std::cout << "  - sendSceneToZBrush: Meshes=" << sendRes.meshesSent
                  << ", Elapsed=" << std::fixed << std::setprecision(2) << sendRes.elapsedMs << " ms | " << sendRes.message << "\n";

        fs::path out0 = tempSceneDir / "out_subtools" / "subtool_0.GoZ";
        fs::path out1 = tempSceneDir / "out_subtools" / "subtool_1.GoZ";
        fs::path cmdFile = tempSceneDir / "cmd_import_scene.txt";

        if (fs::exists(out0) && fs::exists(out1) && fs::exists(cmdFile)) {
            std::ifstream cmdIn(cmdFile);
            std::string cmdContent((std::istreambuf_iterator<char>(cmdIn)),
                                   std::istreambuf_iterator<char>());
            bool cmdHasFreeze = cmdContent.find("[IFreeze,") != std::string::npos;
            bool cmdHasImport = cmdContent.find("Tool:Import") != std::string::npos;
            bool cmdHasAppend = cmdContent.find("Tool:SubTool:Append") != std::string::npos;
            bool cmdHasMesh0 = cmdContent.find("SubTool_Torso") != std::string::npos;
            bool cmdHasMesh1 = cmdContent.find("SubTool_Head") != std::string::npos;

            if (cmdHasFreeze && cmdHasImport && cmdHasAppend && cmdHasMesh0 && cmdHasMesh1) {
                std::cout << "  [PASS] Round-trip scene export generated out_subtools and valid ZScript import command.\n";
            } else {
                std::cout << "  [FAIL] Generated cmd_import_scene.txt missing expected ZScript commands!\n";
                allPassed = false;
            }
        } else {
            std::cout << "  [FAIL] Round-trip files (out_subtools or cmd_import_scene.txt) not created!\n";
            allPassed = false;
        }

        // Restore custom data dir
        Config::setDataDir(origDir);
    }

    delete m2;

    try {
        fs::remove_all(tempSceneDir);
    } catch (...) {}

    // --------------------------------------------------------------------------------------------------
    // Summary
    // --------------------------------------------------------------------------------------------------
    std::cout << "\n===================================================================================================================================================\n";
    if (allPassed) {
        std::cout << " [SUCCESS] All Stage 5 ZBrush Full Scene Round-Trip Bridge tests PASSED!\n";
    } else {
        std::cout << " [FAIL] Some Stage 5 tests FAILED!\n";
    }
    std::cout << "===================================================================================================================================================\n\n";

    return allPassed;
}

} // namespace ZBrushBridge
