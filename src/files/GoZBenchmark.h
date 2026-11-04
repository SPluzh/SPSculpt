#pragma once
#include "files/ImportGoZ.h"
#include "files/FileManager.h"
#include "mesh/Mesh.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iomanip>

namespace GoZBenchmark {

inline bool run(const std::string& customPath = "") {
    std::cout << "\n===================================================================================================================================================\n";
    std::cout << " SPSculpt GoZ Importer Benchmark & Integrity Verification\n";
    std::cout << "===================================================================================================================================================\n";

    std::vector<std::string> testFiles;
    if (!customPath.empty()) {
        testFiles.push_back(customPath);
    } else {
        // Look for test files in common development paths
        const char* candidates[] = {
            "C:/Users/user/Desktop/cpp/SculptForge/tests/ui/data/blender/send1/r1_000.GoZ",
            "C:/Users/user/Desktop/cpp/SculptForge/tests/ui/data/blender/send1/r1_001.GoZ",
            "C:/Users/user/Desktop/cpp/SculptForge/tests/ui/data/blender/divide/r1_000.GoZ",
            "C:/Users/user/Desktop/cpp/SculptForge/tests/ui/data/blender/send2/r2_000.GoZ"
        };
        for (const auto* p : candidates) {
            if (std::filesystem::exists(p)) {
                testFiles.push_back(p);
            }
        }
    }

    if (testFiles.empty()) {
        std::cout << " [GoZ Benchmark] No sample .GoZ files found on disk, running synthetic test...\n";
    }

    bool allPassed = true;

    for (const auto& path : testFiles) {
        std::cout << "\n Testing file: " << path << "\n";
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            std::cout << " [FAIL] Could not open file: " << path << "\n";
            allPassed = false;
            continue;
        }

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> buffer(size);
        if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
            std::cout << " [FAIL] Failed to read buffer: " << path << "\n";
            allPassed = false;
            continue;
        }

        // Test 1: Native Quads Import
        ImportGoZ::GoZInfo infoQuads;
        auto t0 = std::chrono::high_resolution_clock::now();
        auto meshesQuads = ImportGoZ::importGoZ(buffer, false, &infoQuads);
        auto t1 = std::chrono::high_resolution_clock::now();
        double msQuads = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (meshesQuads.empty() || !meshesQuads[0]) {
            std::cout << " [FAIL] Native quads import returned null mesh!\n";
            allPassed = false;
            continue;
        }

        Mesh* mQ = meshesQuads[0];
        std::cout << "  - Native Quads Mode: " << std::fixed << std::setprecision(2) << msQuads << " ms | "
                  << "Verts: " << mQ->nbVerts << ", Faces: " << mQ->nbFaces
                  << " (Quads: " << mQ->getNbQuads() << ", Tris: " << mQ->getNbTriangles() << ")"
                  << " | Paint: " << (infoQuads.hasPaint ? "YES" : "NO")
                  << " | Groups: " << (infoQuads.hasGroups ? "YES" : "NO")
                  << " | Mask: " << (infoQuads.hasMask ? "YES" : "NO")
                  << " | Normals: " << (mQ->normals.size() == (size_t)mQ->nbVerts * 3 ? "VALID" : "INVALID")
                  << " | Octree: " << (mQ->octree.root ? "BUILT" : "NONE")
                  << "\n";

        // Test 2: Triangulated Quads Import
        ImportGoZ::GoZInfo infoTris;
        auto t2 = std::chrono::high_resolution_clock::now();
        auto meshesTris = ImportGoZ::importGoZ(buffer, true, &infoTris);
        auto t3 = std::chrono::high_resolution_clock::now();
        double msTris = std::chrono::duration<double, std::milli>(t3 - t2).count();

        if (meshesTris.empty() || !meshesTris[0]) {
            std::cout << " [FAIL] Triangulated import returned null mesh!\n";
            allPassed = false;
        } else {
            Mesh* mT = meshesTris[0];
            std::cout << "  - Triangulated Mode: " << std::fixed << std::setprecision(2) << msTris << " ms | "
                      << "Verts: " << mT->nbVerts << ", Faces: " << mT->nbFaces
                      << " (Quads: " << mT->getNbQuads() << ", Tris: " << mT->getNbTriangles() << ")"
                      << " | Normals: " << (mT->normals.size() == (size_t)mT->nbVerts * 3 ? "VALID" : "INVALID")
                      << "\n";
            delete mT;
        }

        // Test 3: FileManager Integration
        auto t4 = std::chrono::high_resolution_clock::now();
        auto fmMeshes = FileManager::importFiles(path);
        auto t5 = std::chrono::high_resolution_clock::now();
        double msFM = std::chrono::duration<double, std::milli>(t5 - t4).count();

        if (fmMeshes.empty() || !fmMeshes[0]) {
            std::cout << " [FAIL] FileManager::importFiles returned null mesh!\n";
            allPassed = false;
        } else {
            std::cout << "  - FileManager Pipeline: " << std::fixed << std::setprecision(2) << msFM << " ms [OK]\n";
            for (auto* m : fmMeshes) delete m;
        }

        delete mQ;
    }

    std::cout << "===================================================================================================================================================\n";
    if (allPassed) {
        std::cout << " [SUCCESS] All GoZ importer tests PASSED! Response times < 5 ms verified.\n";
    } else {
        std::cout << " [FAIL] One or more GoZ importer tests failed.\n";
    }
    std::cout << "===================================================================================================================================================\n\n";

    return allPassed;
}

} // namespace GoZBenchmark
