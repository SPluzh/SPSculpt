#pragma once

#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <algorithm>
#include <numeric>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "mesh/Mesh.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "editing/TrimTool.h"
#include "editing/ClipCurveTool.h"
#include "editing/undo/UndoManager.h"
#include "editing/undo/TopologyUndoEntry.h"
#include "common/Enums.h"
#include "common/Constants.h"
#include "editing/manifold/ManifoldBridge.h"
#include <omp.h>

namespace TrimClipBenchmark {

struct BenchmarkRow {
    std::string toolName;
    double executionMs = 0.0;
    double undoMs = 0.0;
    double redoMs = 0.0;
    size_t undoRAMBytes = 0;
    int inVerts = 0;
    int outVerts = 0;
    int flyingVerts = -1; // -1 = N/A, >= 0 for flying vertices
    float maxUndoEps = 0.0f;
    float maxRedoEps = 0.0f;
    bool passed = false;
    std::string notes;

    // Quad Cap validation
    bool isTrim = false;
    bool quadCapChecked = false;
    bool quadCapPassed = false;    // Whether cut was closed with a regular quad grid
    int capQuads = 0;              // Quads on cap
    int capTris = 0;               // Triangles on cap
    float capQuadRatio = 0.0f;     // Quad ratio [0.0..1.0]
    int capPatches = 0;
    int capPatchesSuccess = 0;
    int capPatchesFallback = 0;
    std::string capFallbackReason;
};

inline std::string padRight(const std::string& str, size_t totalWidth) {
    if (str.length() >= totalWidth) return str;
    return str + std::string(totalWidth - str.length(), ' ');
}

inline std::string padLeft(const std::string& str, size_t totalWidth) {
    if (str.length() >= totalWidth) return str;
    return std::string(totalWidth - str.length(), ' ') + str;
}

inline std::string formatMs(double ms, int decimals = 1) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(decimals) << ms << " ms";
    return oss.str();
}

inline std::string formatBytes(size_t bytes) {
    std::ostringstream oss;
    if (bytes >= 1024 * 1024) {
        oss << std::fixed << std::setprecision(1) << ((double)bytes / (1024.0 * 1024.0)) << " MB";
    } else if (bytes >= 1024) {
        oss << std::fixed << std::setprecision(1) << ((double)bytes / 1024.0) << " KB";
    } else {
        oss << bytes << " B";
    }
    return oss.str();
}

inline std::unique_ptr<Mesh> createSphereMesh(int rings, int sectors) {
    auto mesh = std::make_unique<Mesh>();
    mesh->verts.reserve((rings + 1) * (sectors + 1) * 3);
    mesh->faces.reserve(rings * sectors * 4);

    float R = 2.0f;
    for (int r = 0; r <= rings; ++r) {
        float phi = float(M_PI) * float(r) / float(rings);
        for (int s = 0; s <= sectors; ++s) {
            float theta = 2.0f * float(M_PI) * float(s) / float(sectors);
            float x = R * std::sin(phi) * std::cos(theta);
            float y = R * std::cos(phi);
            float z = R * std::sin(phi) * std::sin(theta);
            mesh->verts.push_back(x);
            mesh->verts.push_back(y);
            mesh->verts.push_back(z);
            mesh->nbVerts++;
        }
    }

    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < sectors; ++s) {
            uint32_t v0 = r * (sectors + 1) + s;
            uint32_t v1 = v0 + 1;
            uint32_t v2 = (r + 1) * (sectors + 1) + s + 1;
            uint32_t v3 = (r + 1) * (sectors + 1) + s;

            mesh->faces.push_back(v0);
            mesh->faces.push_back(v1);
            mesh->faces.push_back(v2);
            mesh->faces.push_back(v3);
            mesh->nbFaces++;
        }
    }

    mesh->vertVisible.assign(mesh->nbVerts, 1);
    mesh->faceVisible.assign(mesh->nbFaces, 1);
    return mesh;
}

inline bool run(int targetFaces = 100000) {
    std::cout << "\n======================================================================================================\n";
    std::cout << " SPSculpt Trim Tool & Clip Curve Performance, Undo & Integrity Benchmark @ "
              << std::fixed << std::setprecision(1) << (targetFaces / 1000000.0) << "M Polygons\n";
    std::cout << "======================================================================================================\n\n";

    try {
        int rings = (int)std::sqrt(targetFaces);
        int sectors = targetFaces / rings;
        int actualFaces = rings * sectors;
        int actualVerts = (rings + 1) * (sectors + 1);

        std::cout << "[1/4] Generating Procedural UV Sphere (" << rings << " rings x " << sectors << " sectors)...\n";
        std::cout << "      Faces:    " << actualFaces << " quads (" << (actualFaces / 1000000.0) << " M polygons)\n";
        std::cout << "      Vertices: " << actualVerts << " vertices (" << (actualVerts / 1000000.0) << " M vertices)\n";

        auto tGenStart = std::chrono::high_resolution_clock::now();
        auto meshPtr = createSphereMesh(rings, sectors);
        Mesh* meshRaw = meshPtr.release();
        auto tGenEnd = std::chrono::high_resolution_clock::now();
        double genMs = std::chrono::duration<double, std::milli>(tGenEnd - tGenStart).count();
        std::cout << "      Sphere generation completed in " << std::fixed << std::setprecision(2) << genMs << " ms\n";

        std::cout << "\n[2/4] Initializing Mesh & Building Spatial Structure (Mesh::postInit)...\n";
        auto tInitStart = std::chrono::high_resolution_clock::now();
        meshRaw->postInit(true);
        auto tInitEnd = std::chrono::high_resolution_clock::now();
        double initMs = std::chrono::duration<double, std::milli>(tInitEnd - tInitStart).count();
        std::cout << "      Mesh postInit completed in " << std::fixed << std::setprecision(2) << initMs << " ms\n";

        std::cout << "\n[3/4] Initializing Scene, Camera Viewport & Undo Manager...\n";
        Scene scene;
        scene.addMesh(meshRaw);
        scene.selectMesh(meshRaw);

        Camera& camera = scene.getCamera();
        camera.onResize(1920, 1080);
        camera.resetViewToMesh(meshRaw, false);
        camera.update(0.016f);

        g_undoManager.clear();

        auto getActiveMesh = [&scene]() -> Mesh* {
            if (scene.getSelected()) return scene.getSelected();
            if (!scene.getMeshes().empty()) return scene.getMeshes()[0];
            return nullptr;
        };

        std::vector<BenchmarkRow> rows;
        rows.reserve(6);

        // Keep snapshot of the pristine sphere
        Mesh* pristineMesh = getActiveMesh();
        const int origVertCount = pristineMesh->nbVerts;
        const int origFaceCount = pristineMesh->nbFaces;
        const std::vector<float> pristineVerts = pristineMesh->verts;
        const std::vector<uint32_t> pristineFaces = pristineMesh->faces;

        std::cout << "\n[4/4] Executing Trim Tool & Clip Curve Operations...\n";

        // =========================================================================
        // Helper to run, analyze and verify Trim Tool operations
        // =========================================================================
        auto runTrimBenchmark = [&](const std::string& name,
                                    const std::vector<glm::vec2>& lasso,
                                    bool isAlt,
                                    int stepIdx,
                                    int totalSteps) -> BenchmarkRow {
            std::cout << "  -> [" << stepIdx << "/" << totalSteps << "] Benchmarking " << name << "...\n";
            Mesh* curMesh = getActiveMesh();

            HistoryState beforeState = scene.saveCurrentState(true);

            TrimConfig config;
            config.fillHole = true;
            config.remeshFilled = true;
            sculpt::QuadCapResult capRes;
            uint32_t capGid = 0;
            config.outCapResult = &capRes;
            config.outCapGid = &capGid;

            auto tStart = std::chrono::high_resolution_clock::now();
            bool modified = TrimTool::execute(
                curMesh,
                camera,
                lasso,
                isAlt,
                false, // useSym = false
                SymmetryMode::Local,
                {},
                config
            );
            auto tEnd = std::chrono::high_resolution_clock::now();
            double execMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

            curMesh = getActiveMesh();
            int trimmedVerts = curMesh ? curMesh->nbVerts : 0;
            int trimmedFaces = curMesh ? curMesh->nbFaces : 0;

            // Direct topological inspection of cap faces on mesh
            int capQuads = 0;
            int capTris = 0;
            if (curMesh && capGid > 0) {
                for (int f = 0; f < curMesh->nbFaces; ++f) {
                    if (f < (int)curMesh->faceGroups.size() && curMesh->faceGroups[f] == capGid) {
                        if (curMesh->faces[f * 4 + 3] != TRI_INDEX) {
                            capQuads++;
                        } else {
                            capTris++;
                        }
                    }
                }
            }
            int totalCapFaces = capQuads + capTris;
            float quadRatio = totalCapFaces > 0 ? (float)capQuads / (float)totalCapFaces : 0.0f;
            // Quad cap passed if generator succeeded, created quads, and quad ratio >= 50%
            bool quadCapPassed = capRes.success && (capQuads > 0) && (quadRatio >= 0.50f);

            // Verify whether the trimmed mesh can be cleanly converted to a valid 2-manifold
            manifold::Manifold checkM = curMesh ? sculpt::ManifoldBridge::fromMesh(*curMesh) : manifold::Manifold();
            bool isManifold = (checkM.Status() == manifold::Manifold::Error::NoError);

            int openEdges = 0, nonManifoldEdges = 0;
            if (curMesh) {
                std::unordered_map<uint64_t, int> halfEdges;
                for (int f = 0; f < curMesh->nbFaces; ++f) {
                    uint32_t v0 = curMesh->faces[f * 4 + 0];
                    uint32_t v1 = curMesh->faces[f * 4 + 1];
                    uint32_t v2 = curMesh->faces[f * 4 + 2];
                    uint32_t v3 = curMesh->faces[f * 4 + 3];
                    auto addHE = [&](uint32_t a, uint32_t b) {
                        uint64_t key = ((uint64_t)a << 32) | b;
                        halfEdges[key]++;
                    };
                    addHE(v0, v1);
                    addHE(v1, v2);
                    if (v3 != TRI_INDEX) {
                        addHE(v2, v3);
                        addHE(v3, v0);
                    } else {
                        addHE(v2, v0);
                    }
                }
                for (const auto& [he, cnt] : halfEdges) {
                    uint32_t a = (uint32_t)(he >> 32);
                    uint32_t b = (uint32_t)(he & 0xFFFFFFFF);
                    uint64_t rev = ((uint64_t)b << 32) | a;
                    auto it = halfEdges.find(rev);
                    int revCnt = (it != halfEdges.end()) ? it->second : 0;
                    if (cnt > 1) nonManifoldEdges++;
                    if (revCnt == 0) openEdges++;
                }
            }

            std::cout << "      [Quad Cap Check] Cap Faces: " << totalCapFaces
                      << " (" << capQuads << " quads, " << capTris << " tris -> "
                      << std::fixed << std::setprecision(1) << (quadRatio * 100.0f) << "% quads) | "
                      << (quadCapPassed ? "CLOSED WITH QUAD GRID [OK]" : "NOT CLOSED WITH QUAD GRID [NO QUAD]") << "\n";
            std::cout << "      [Manifold Check] Mesh -> Manifold: Status " << (int)checkM.Status()
                      << " (" << (isManifold ? "VALID 2-MANIFOLD [OK]" : "NOT MANIFOLD [ERROR]") << ") | Open HE: "
                      << openEdges << ", Non-Manifold HE: " << nonManifoldEdges << "\n";
            if (!quadCapPassed) {
                std::cout << "      [Quad Cap Diagnostic] "
                          << (!capRes.fallbackReason.empty() ? capRes.fallbackReason : "No quads on cap / fell back to raw triangles") << "\n";
            }

            HistoryState afterState = scene.saveCurrentState(true);
            g_undoManager.pushTopologyChange(scene, name, std::move(beforeState), std::move(afterState));
            size_t undoBytes = !g_undoManager.getUndoStack().empty() ? g_undoManager.getUndoStack().back()->getMemoryUsage() : 0;

            // Undo
            auto tUndoStart = std::chrono::high_resolution_clock::now();
            g_undoManager.undo(scene);
            auto tUndoEnd = std::chrono::high_resolution_clock::now();
            double undoMs = std::chrono::duration<double, std::milli>(tUndoEnd - tUndoStart).count();

            curMesh = getActiveMesh();
            float maxUndoDiff = 0.0f;
            if (curMesh && curMesh->nbVerts == origVertCount) {
                for (size_t i = 0; i < pristineVerts.size(); ++i) {
                    float diff = std::abs(curMesh->verts[i] - pristineVerts[i]);
                    if (diff > maxUndoDiff) maxUndoDiff = diff;
                }
            } else {
                maxUndoDiff = 999.0f;
            }

            // Redo
            auto tRedoStart = std::chrono::high_resolution_clock::now();
            g_undoManager.redo(scene);
            auto tRedoEnd = std::chrono::high_resolution_clock::now();
            double redoMs = std::chrono::duration<double, std::milli>(tRedoEnd - tRedoStart).count();

            curMesh = getActiveMesh();
            float maxRedoDiff = 0.0f;
            if (!curMesh || curMesh->nbVerts != trimmedVerts || curMesh->nbFaces != trimmedFaces) {
                maxRedoDiff = 999.0f;
            }

            // Revert back to pristine mesh
            g_undoManager.undo(scene);
            g_undoManager.clear();

            BenchmarkRow row;
            row.toolName = name;
            row.executionMs = execMs;
            row.undoMs = undoMs;
            row.redoMs = redoMs;
            row.undoRAMBytes = undoBytes;
            row.inVerts = origVertCount;
            row.outVerts = trimmedVerts;
            row.flyingVerts = -1;
            row.maxUndoEps = maxUndoDiff;
            row.maxRedoEps = maxRedoDiff;
            row.isTrim = true;
            row.quadCapChecked = true;
            row.quadCapPassed = quadCapPassed;
            row.capQuads = capQuads;
            row.capTris = capTris;
            row.capQuadRatio = quadRatio;
            row.capPatches = capRes.numPatches;
            row.capPatchesSuccess = capRes.patchesQuadCapped;
            row.capPatchesFallback = capRes.patchesFellBack;
            row.capFallbackReason = capRes.fallbackReason;
            row.passed = modified && (maxUndoDiff == 0.0f) && (maxRedoDiff == 0.0f);
            return row;
        };

        // 1. Tool: Trim Tool (Default: Cut & Fill Hole with QuadCap)
        {
            std::vector<glm::vec2> trimCornerLasso = {
                { 960.0f, 540.0f },
                { 960.0f, 260.0f },
                { 1240.0f, 260.0f },
                { 1240.0f, 540.0f }
            };
            rows.push_back(runTrimBenchmark("Trim Tool (Default Cut & Cap)", trimCornerLasso, false, 1, 8));
        }

        // 2. Tool: Trim Tool (Alt Mode: Keep Inside Volume with QuadCap)
        {
            std::vector<glm::vec2> trimAltLasso = {
                { 860.0f, 440.0f },
                { 1060.0f, 440.0f },
                { 1060.0f, 640.0f },
                { 860.0f, 640.0f }
            };
            rows.push_back(runTrimBenchmark("Trim Tool (Alt: Keep Inside)", trimAltLasso, true, 2, 8));
        }

        // 3. Tool: Trim Tool (Hole in Sphere Center - Through-Hole Cutout)
        {
            std::vector<glm::vec2> trimHoleLasso = {
                { 880.0f, 460.0f },
                { 1040.0f, 460.0f },
                { 1040.0f, 620.0f },
                { 880.0f, 620.0f }
            };
            rows.push_back(runTrimBenchmark("Trim Tool (Hole in Center)", trimHoleLasso, false, 3, 8));
        }

        // 4. Tool: Trim Tool (Elongated Cutout / Narrow Slot Notch)
        {
            std::vector<glm::vec2> trimSlotLasso = {
                { 740.0f, 520.0f },
                { 1180.0f, 520.0f },
                { 1180.0f, 560.0f },
                { 740.0f, 560.0f }
            };
            rows.push_back(runTrimBenchmark("Trim Tool (Elongated Cutout)", trimSlotLasso, false, 4, 8));
        }

        // 5. Tool: Trim Tool (Consecutive Cuts - 2 In Sequence)
        auto runConsecutiveTrimBenchmark = [&](const std::string& name,
                                               const std::vector<glm::vec2>& lasso1,
                                               const std::vector<glm::vec2>& lasso2,
                                               int stepIdx, int totalSteps) -> BenchmarkRow {
            std::cout << "  -> [" << stepIdx << "/" << totalSteps << "] Benchmarking " << name << "...\n";
            Mesh* curMesh = getActiveMesh();
            int origVertCount = curMesh ? curMesh->nbVerts : 0;
            const std::vector<float> pristineVerts = curMesh ? curMesh->verts : std::vector<float>();

            // --- CUT 1 ---
            HistoryState beforeCut1 = scene.saveCurrentState(true);
            TrimConfig config1;
            config1.fillHole = true;
            config1.remeshFilled = true;
            sculpt::QuadCapResult capRes1;
            uint32_t capGid1 = 0;
            config1.outCapResult = &capRes1;
            config1.outCapGid = &capGid1;

            auto tStart1 = std::chrono::high_resolution_clock::now();
            bool mod1 = TrimTool::execute(curMesh, camera, lasso1, false, false, SymmetryMode::Local, {}, config1);
            auto tEnd1 = std::chrono::high_resolution_clock::now();
            double exec1Ms = std::chrono::duration<double, std::milli>(tEnd1 - tStart1).count();

            curMesh = getActiveMesh();
            manifold::Manifold checkM1 = curMesh ? sculpt::ManifoldBridge::fromMesh(*curMesh) : manifold::Manifold();
            bool isManifold1 = (checkM1.Status() == manifold::Manifold::Error::NoError);
            std::cout << "      [Cut 1/2 Check] Mesh -> Manifold: Status " << (int)checkM1.Status()
                      << " (" << (isManifold1 ? "VALID 2-MANIFOLD [OK]" : "NOT MANIFOLD [ERROR]") << ")\n";

            HistoryState afterCut1 = scene.saveCurrentState(true);
            g_undoManager.pushTopologyChange(scene, name + " (Cut 1)", std::move(beforeCut1), std::move(afterCut1));

            // --- CUT 2 (ON RESULT OF CUT 1) ---
            HistoryState beforeCut2 = scene.saveCurrentState(true);
            TrimConfig config2;
            config2.fillHole = true;
            config2.remeshFilled = true;
            sculpt::QuadCapResult capRes2;
            uint32_t capGid2 = 0;
            config2.outCapResult = &capRes2;
            config2.outCapGid = &capGid2;

            auto tStart2 = std::chrono::high_resolution_clock::now();
            bool mod2 = TrimTool::execute(curMesh, camera, lasso2, false, false, SymmetryMode::Local, {}, config2);
            auto tEnd2 = std::chrono::high_resolution_clock::now();
            double exec2Ms = std::chrono::duration<double, std::milli>(tEnd2 - tStart2).count();

            curMesh = getActiveMesh();
            int finalVerts = curMesh ? curMesh->nbVerts : 0;
            int finalFaces = curMesh ? curMesh->nbFaces : 0;

            manifold::Manifold checkM2 = curMesh ? sculpt::ManifoldBridge::fromMesh(*curMesh) : manifold::Manifold();
            bool isManifold2 = (checkM2.Status() == manifold::Manifold::Error::NoError);

            int openEdges = 0, nonManifoldEdges = 0;
            if (curMesh) {
                std::unordered_map<uint64_t, int> halfEdges;
                for (int f = 0; f < curMesh->nbFaces; ++f) {
                    uint32_t v0 = curMesh->faces[f * 4 + 0];
                    uint32_t v1 = curMesh->faces[f * 4 + 1];
                    uint32_t v2 = curMesh->faces[f * 4 + 2];
                    uint32_t v3 = curMesh->faces[f * 4 + 3];
                    auto addHE = [&](uint32_t a, uint32_t b) {
                        uint64_t key = ((uint64_t)a << 32) | b;
                        halfEdges[key]++;
                    };
                    addHE(v0, v1);
                    addHE(v1, v2);
                    if (v3 != TRI_INDEX) {
                        addHE(v2, v3);
                        addHE(v3, v0);
                    } else {
                        addHE(v2, v0);
                    }
                }
                for (const auto& [he, cnt] : halfEdges) {
                    uint32_t a = (uint32_t)(he >> 32);
                    uint32_t b = (uint32_t)(he & 0xFFFFFFFF);
                    uint64_t rev = ((uint64_t)b << 32) | a;
                    auto it = halfEdges.find(rev);
                    int revCnt = (it != halfEdges.end()) ? it->second : 0;
                    if (cnt > 1) nonManifoldEdges++;
                    if (revCnt == 0) openEdges++;
                }
            }

            int capQuads2 = 0, capTris2 = 0;
            if (curMesh && capGid2 > 0) {
                for (int f = 0; f < curMesh->nbFaces; ++f) {
                    if (f < (int)curMesh->faceGroups.size() && curMesh->faceGroups[f] == capGid2) {
                        if (curMesh->faces[f * 4 + 3] != TRI_INDEX) capQuads2++;
                        else capTris2++;
                    }
                }
            }
            int totalCap2 = capQuads2 + capTris2;
            float quadRatio2 = totalCap2 > 0 ? (float)capQuads2 / (float)totalCap2 : 0.0f;
            bool quadCapPassed2 = capRes2.success && (capQuads2 > 0) && (quadRatio2 >= 0.50f);

            std::cout << "      [Cut 2/2 Check] Cap Faces: " << totalCap2
                      << " (" << capQuads2 << " quads, " << capTris2 << " tris -> "
                      << std::fixed << std::setprecision(1) << (quadRatio2 * 100.0f) << "% quads) | "
                      << (quadCapPassed2 ? "CLOSED WITH QUAD GRID [OK]" : "NOT CLOSED WITH QUAD GRID [NO QUAD]") << "\n";
            std::cout << "      [Cut 2/2 Check] Mesh -> Manifold: Status " << (int)checkM2.Status()
                      << " (" << (isManifold2 ? "VALID 2-MANIFOLD [OK]" : "NOT MANIFOLD [ERROR]") << ") | Open HE: "
                      << openEdges << ", Non-Manifold HE: " << nonManifoldEdges << "\n";

            HistoryState afterCut2 = scene.saveCurrentState(true);
            g_undoManager.pushTopologyChange(scene, name + " (Cut 2)", std::move(beforeCut2), std::move(afterCut2));
            size_t undoBytes = 0;
            for (const auto& entry : g_undoManager.getUndoStack()) {
                undoBytes += entry->getMemoryUsage();
            }

            // Undo Cut 2 then Cut 1
            auto tUndoStart = std::chrono::high_resolution_clock::now();
            g_undoManager.undo(scene); // Undoes Cut 2
            g_undoManager.undo(scene); // Undoes Cut 1
            auto tUndoEnd = std::chrono::high_resolution_clock::now();
            double undoMs = std::chrono::duration<double, std::milli>(tUndoEnd - tUndoStart).count();

            curMesh = getActiveMesh();
            float maxUndoDiff = 0.0f;
            if (curMesh && curMesh->nbVerts == origVertCount) {
                for (size_t i = 0; i < pristineVerts.size(); ++i) {
                    float diff = std::abs(curMesh->verts[i] - pristineVerts[i]);
                    if (diff > maxUndoDiff) maxUndoDiff = diff;
                }
            } else {
                maxUndoDiff = 999.0f;
            }

            // Redo Cut 1 then Cut 2
            auto tRedoStart = std::chrono::high_resolution_clock::now();
            g_undoManager.redo(scene); // Redoes Cut 1
            g_undoManager.redo(scene); // Redoes Cut 2
            auto tRedoEnd = std::chrono::high_resolution_clock::now();
            double redoMs = std::chrono::duration<double, std::milli>(tRedoEnd - tRedoStart).count();

            curMesh = getActiveMesh();
            float maxRedoDiff = 0.0f;
            if (!curMesh || curMesh->nbVerts != finalVerts || curMesh->nbFaces != finalFaces) {
                maxRedoDiff = 999.0f;
            }

            // Undo both back to pristine sphere
            g_undoManager.undo(scene);
            g_undoManager.undo(scene);
            g_undoManager.clear();

            BenchmarkRow row;
            row.toolName = name;
            row.executionMs = exec1Ms + exec2Ms;
            row.undoMs = undoMs;
            row.redoMs = redoMs;
            row.undoRAMBytes = undoBytes;
            row.inVerts = origVertCount;
            row.outVerts = finalVerts;
            row.flyingVerts = -1;
            row.maxUndoEps = maxUndoDiff;
            row.maxRedoEps = maxRedoDiff;
            row.isTrim = true;
            row.quadCapChecked = true;
            row.quadCapPassed = isManifold1 && isManifold2 && quadCapPassed2;
            row.capQuads = capQuads2;
            row.capTris = capTris2;
            row.capQuadRatio = quadRatio2;
            row.capPatches = capRes2.numPatches;
            row.capPatchesSuccess = capRes2.patchesQuadCapped;
            row.capPatchesFallback = capRes2.patchesFellBack;
            row.capFallbackReason = capRes2.fallbackReason;
            row.passed = mod1 && mod2 && isManifold1 && isManifold2 && (maxUndoDiff == 0.0f) && (maxRedoDiff == 0.0f);
            return row;
        };

        {
            std::vector<glm::vec2> cut1Lasso = {
                { 960.0f, 540.0f },
                { 960.0f, 260.0f },
                { 1240.0f, 260.0f },
                { 1240.0f, 540.0f }
            };
            std::vector<glm::vec2> cut2Lasso = {
                { 680.0f, 540.0f },
                { 680.0f, 820.0f },
                { 1040.0f, 820.0f },
                { 1040.0f, 540.0f }
            };
            rows.push_back(runConsecutiveTrimBenchmark("Trim Tool (Consecutive Cuts)", cut1Lasso, cut2Lasso, 5, 8));
        }

        // =========================================================================
        // 6. Tool: Clip Curve (Straight Line Across Sphere)
        // =========================================================================
        {
            std::cout << "  -> [6/8] Benchmarking Clip Curve (Straight Line)...\n";
            Mesh* curMesh = getActiveMesh();

            std::vector<glm::vec2> clipLine = {
                { 700.0f, 540.0f },
                { 1220.0f, 540.0f }
            };

            HistoryState beforeState = scene.saveCurrentState(false);
            const std::vector<float> preVerts = curMesh->verts;

            auto tStart = std::chrono::high_resolution_clock::now();
            bool modified = ClipCurveTool::execute(
                curMesh,
                camera,
                clipLine,
                false, // altMode
                false, // useSym
                SymmetryMode::Local,
                {}
            );
            auto tEnd = std::chrono::high_resolution_clock::now();
            double execMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

            curMesh = getActiveMesh();

            // Flying points check:
            // Pristine sphere radius is 2.0f. Any vertex with length > 2.05f or NaN/Inf is a flying vertex
            int flyingCount = 0;
            for (int i = 0; i < curMesh->nbVerts; ++i) {
                float x = curMesh->verts[i * 3 + 0];
                float y = curMesh->verts[i * 3 + 1];
                float z = curMesh->verts[i * 3 + 2];
                if (std::isnan(x) || std::isnan(y) || std::isnan(z) ||
                    std::isinf(x) || std::isinf(y) || std::isinf(z)) {
                    flyingCount++;
                } else {
                    float r = std::sqrt(x * x + y * y + z * z);
                    if (r > 2.05f) {
                        flyingCount++;
                    }
                }
            }

            const std::vector<float> postVerts = curMesh->verts;
            HistoryState afterState = scene.saveCurrentState(false);
            g_undoManager.pushTopologyChange(scene, "Clip Curve (Straight Line)", std::move(beforeState), std::move(afterState));
            size_t undoBytes = !g_undoManager.getUndoStack().empty() ? g_undoManager.getUndoStack().back()->getMemoryUsage() : 0;

            // Undo
            auto tUndoStart = std::chrono::high_resolution_clock::now();
            g_undoManager.undo(scene);
            auto tUndoEnd = std::chrono::high_resolution_clock::now();
            double undoMs = std::chrono::duration<double, std::milli>(tUndoEnd - tUndoStart).count();

            curMesh = getActiveMesh();
            float maxUndoDiff = 0.0f;
            for (size_t i = 0; i < preVerts.size(); ++i) {
                float diff = std::abs(curMesh->verts[i] - preVerts[i]);
                if (diff > maxUndoDiff) maxUndoDiff = diff;
            }

            // Redo
            auto tRedoStart = std::chrono::high_resolution_clock::now();
            g_undoManager.redo(scene);
            auto tRedoEnd = std::chrono::high_resolution_clock::now();
            double redoMs = std::chrono::duration<double, std::milli>(tRedoEnd - tRedoStart).count();

            curMesh = getActiveMesh();
            float maxRedoDiff = 0.0f;
            for (size_t i = 0; i < postVerts.size(); ++i) {
                float diff = std::abs(curMesh->verts[i] - postVerts[i]);
                if (diff > maxRedoDiff) maxRedoDiff = diff;
            }

            // Revert back to pristine mesh
            g_undoManager.undo(scene);
            g_undoManager.clear();

            BenchmarkRow row;
            row.toolName = "Clip Curve (Straight Line)";
            row.executionMs = execMs;
            row.undoMs = undoMs;
            row.redoMs = redoMs;
            row.undoRAMBytes = undoBytes;
            row.inVerts = origVertCount;
            row.outVerts = curMesh->nbVerts;
            row.flyingVerts = flyingCount;
            row.maxUndoEps = maxUndoDiff;
            row.maxRedoEps = maxRedoDiff;
            row.passed = modified && (flyingCount == 0) && (maxUndoDiff == 0.0f) && (maxRedoDiff == 0.0f);
            rows.push_back(row);
        }

        // =========================================================================
        // 7. Tool: Clip Curve (Polyline with Angle / Bend)
        // =========================================================================
        {
            std::cout << "  -> [7/8] Benchmarking Clip Curve (Polyline Angle / Bend)...\n";
            Mesh* curMesh = getActiveMesh();

            std::vector<glm::vec2> clipPoly = {
                { 700.0f, 540.0f },
                { 960.0f, 540.0f },
                { 960.0f, 800.0f }
            };

            HistoryState beforeState = scene.saveCurrentState(false);
            const std::vector<float> preVerts = curMesh->verts;

            auto tStart = std::chrono::high_resolution_clock::now();
            bool modified = ClipCurveTool::execute(
                curMesh,
                camera,
                clipPoly,
                false, // altMode
                false, // useSym
                SymmetryMode::Local,
                {}
            );
            auto tEnd = std::chrono::high_resolution_clock::now();
            double execMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

            curMesh = getActiveMesh();

            // Flying points check:
            int flyingCount = 0;
            for (int i = 0; i < curMesh->nbVerts; ++i) {
                float x = curMesh->verts[i * 3 + 0];
                float y = curMesh->verts[i * 3 + 1];
                float z = curMesh->verts[i * 3 + 2];
                if (std::isnan(x) || std::isnan(y) || std::isnan(z) ||
                    std::isinf(x) || std::isinf(y) || std::isinf(z)) {
                    flyingCount++;
                } else {
                    float r = std::sqrt(x * x + y * y + z * z);
                    if (r > 2.05f) {
                        flyingCount++;
                    }
                }
            }

            const std::vector<float> postVerts = curMesh->verts;
            HistoryState afterState = scene.saveCurrentState(false);
            g_undoManager.pushTopologyChange(scene, "Clip Curve (Polyline Angle)", std::move(beforeState), std::move(afterState));
            size_t undoBytes = !g_undoManager.getUndoStack().empty() ? g_undoManager.getUndoStack().back()->getMemoryUsage() : 0;

            // Undo
            auto tUndoStart = std::chrono::high_resolution_clock::now();
            g_undoManager.undo(scene);
            auto tUndoEnd = std::chrono::high_resolution_clock::now();
            double undoMs = std::chrono::duration<double, std::milli>(tUndoEnd - tUndoStart).count();

            curMesh = getActiveMesh();
            float maxUndoDiff = 0.0f;
            for (size_t i = 0; i < preVerts.size(); ++i) {
                float diff = std::abs(curMesh->verts[i] - preVerts[i]);
                if (diff > maxUndoDiff) maxUndoDiff = diff;
            }

            // Redo
            auto tRedoStart = std::chrono::high_resolution_clock::now();
            g_undoManager.redo(scene);
            auto tRedoEnd = std::chrono::high_resolution_clock::now();
            double redoMs = std::chrono::duration<double, std::milli>(tRedoEnd - tRedoStart).count();

            curMesh = getActiveMesh();
            float maxRedoDiff = 0.0f;
            for (size_t i = 0; i < postVerts.size(); ++i) {
                float diff = std::abs(curMesh->verts[i] - postVerts[i]);
                if (diff > maxRedoDiff) maxRedoDiff = diff;
            }

            // Revert back to pristine mesh
            g_undoManager.undo(scene);
            g_undoManager.clear();

            BenchmarkRow row;
            row.toolName = "Clip Curve (Polyline Angle)";
            row.executionMs = execMs;
            row.undoMs = undoMs;
            row.redoMs = redoMs;
            row.undoRAMBytes = undoBytes;
            row.inVerts = origVertCount;
            row.outVerts = curMesh->nbVerts;
            row.flyingVerts = flyingCount;
            row.maxUndoEps = maxUndoDiff;
            row.maxRedoEps = maxRedoDiff;
            row.passed = modified && (flyingCount == 0) && (maxUndoDiff == 0.0f) && (maxRedoDiff == 0.0f);
            rows.push_back(row);
        }

        // =========================================================================
        // 8. Tool: Clip Curve (with Symmetry)
        // =========================================================================
        {
            std::cout << "  -> [8/8] Benchmarking Clip Curve (with X Symmetry)...\n";
            Mesh* curMesh = getActiveMesh();

            std::vector<glm::vec2> clipLine = {
                { 960.0f, 400.0f },
                { 1300.0f, 400.0f }
            };

            std::vector<glm::vec3> symScales = { { -1.0f, 1.0f, 1.0f } };

            HistoryState beforeState = scene.saveCurrentState(false);
            const std::vector<float> preVerts = curMesh->verts;

            auto tStart = std::chrono::high_resolution_clock::now();
            bool modified = ClipCurveTool::execute(
                curMesh,
                camera,
                clipLine,
                false, // altMode
                true,  // useSym = true
                SymmetryMode::Local,
                symScales
            );
            auto tEnd = std::chrono::high_resolution_clock::now();
            double execMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

            curMesh = getActiveMesh();

            // Flying points check:
            int flyingCount = 0;
            for (int i = 0; i < curMesh->nbVerts; ++i) {
                float x = curMesh->verts[i * 3 + 0];
                float y = curMesh->verts[i * 3 + 1];
                float z = curMesh->verts[i * 3 + 2];
                if (std::isnan(x) || std::isnan(y) || std::isnan(z) ||
                    std::isinf(x) || std::isinf(y) || std::isinf(z)) {
                    flyingCount++;
                } else {
                    float r = std::sqrt(x * x + y * y + z * z);
                    if (r > 2.05f) {
                        flyingCount++;
                    }
                }
            }

            const std::vector<float> postVerts = curMesh->verts;
            HistoryState afterState = scene.saveCurrentState(false);
            g_undoManager.pushTopologyChange(scene, "Clip Curve (Symmetry)", std::move(beforeState), std::move(afterState));
            size_t undoBytes = !g_undoManager.getUndoStack().empty() ? g_undoManager.getUndoStack().back()->getMemoryUsage() : 0;

            // Undo
            auto tUndoStart = std::chrono::high_resolution_clock::now();
            g_undoManager.undo(scene);
            auto tUndoEnd = std::chrono::high_resolution_clock::now();
            double undoMs = std::chrono::duration<double, std::milli>(tUndoEnd - tUndoStart).count();

            curMesh = getActiveMesh();
            float maxUndoDiff = 0.0f;
            for (size_t i = 0; i < preVerts.size(); ++i) {
                float diff = std::abs(curMesh->verts[i] - preVerts[i]);
                if (diff > maxUndoDiff) maxUndoDiff = diff;
            }

            // Redo
            auto tRedoStart = std::chrono::high_resolution_clock::now();
            g_undoManager.redo(scene);
            auto tRedoEnd = std::chrono::high_resolution_clock::now();
            double redoMs = std::chrono::duration<double, std::milli>(tRedoEnd - tRedoStart).count();

            curMesh = getActiveMesh();
            float maxRedoDiff = 0.0f;
            for (size_t i = 0; i < postVerts.size(); ++i) {
                float diff = std::abs(curMesh->verts[i] - postVerts[i]);
                if (diff > maxRedoDiff) maxRedoDiff = diff;
            }

            // Revert back to pristine mesh
            g_undoManager.undo(scene);
            g_undoManager.clear();

            BenchmarkRow row;
            row.toolName = "Clip Curve (with X Symmetry)";
            row.executionMs = execMs;
            row.undoMs = undoMs;
            row.redoMs = redoMs;
            row.undoRAMBytes = undoBytes;
            row.inVerts = origVertCount;
            row.outVerts = curMesh->nbVerts;
            row.flyingVerts = flyingCount;
            row.maxUndoEps = maxUndoDiff;
            row.maxRedoEps = maxRedoDiff;
            row.passed = modified && (flyingCount == 0) && (maxUndoDiff == 0.0f) && (maxRedoDiff == 0.0f);
            rows.push_back(row);
        }

        // =========================================================================
        // Print Summary Table
        // =========================================================================
        std::cout << "\n==================================================================================================================================\n";
        std::cout << " SPSculpt Trim Tool & Clip Curve Benchmark Summary @ "
                  << std::fixed << std::setprecision(1) << (targetFaces / 1000000.0) << "M Polygons\n";
        std::cout << "==================================================================================================================================\n";
        std::cout << " " << padRight("Tool / Operation", 32) << " | "
                  << padLeft("Execution", 11) << " | "
                  << padLeft("Undo Time", 10) << " | "
                  << padLeft("Redo Time", 10) << " | "
                  << padLeft("Undo RAM", 10) << " | "
                  << padLeft("Quad Cap Grid", 20) << " | "
                  << padLeft("Flying", 8) << " | "
                  << padLeft("Integrity", 11) << " | "
                  << "Status \n";
        std::cout << "---------------------------------+-------------+------------+------------+------------+----------------------+----------+-------------+--------\n";

        bool allIntegrityPassed = true;
        bool allQuadCapsPassed = true;

        for (const auto& r : rows) {
            std::string flyStr = (r.flyingVerts < 0) ? "    -   " : std::to_string(r.flyingVerts);
            std::string epsStr = (r.maxUndoEps == 0.0f && r.maxRedoEps == 0.0f) ? "0 eps diff" : "MISMATCH";

            std::string quadCapStr;
            if (r.isTrim) {
                if (r.quadCapPassed) {
                    std::ostringstream ss;
                    ss << std::fixed << std::setprecision(1) << (r.capQuadRatio * 100.0f) << "% Q (" << r.capQuads << "/" << r.capTris << ")";
                    quadCapStr = ss.str();
                } else {
                    std::ostringstream ss;
                    ss << "NO (" << std::fixed << std::setprecision(1) << (r.capQuadRatio * 100.0f) << "% Q)";
                    quadCapStr = ss.str();
                }
            } else {
                quadCapStr = "         -          ";
            }

            bool integrityPassed = r.passed && (r.flyingVerts <= 0);
            std::string statusStr;
            if (integrityPassed && (!r.isTrim || r.quadCapPassed)) {
                statusStr = "PASSED";
            } else if (integrityPassed && r.isTrim && !r.quadCapPassed) {
                statusStr = "NO QUAD";
                allQuadCapsPassed = false;
            } else {
                statusStr = "FAILED";
                allIntegrityPassed = false;
            }

            std::cout << " " << padRight(r.toolName, 32) << " | "
                      << padLeft(formatMs(r.executionMs), 11) << " | "
                      << padLeft(formatMs(r.undoMs), 10) << " | "
                      << padLeft(formatMs(r.redoMs), 10) << " | "
                      << padLeft(formatBytes(r.undoRAMBytes), 10) << " | "
                      << padLeft(quadCapStr, 20) << " | "
                      << padLeft(flyStr, 8) << " | "
                      << padLeft(epsStr, 11) << " | "
                      << statusStr << " \n";
        }
        std::cout << "----------------------------------------------------------------------------------------------------------------------------------\n";

        // =========================================================================
        // Print Detailed Quad Cap Diagnostics Breakdown
        // =========================================================================
        std::cout << "\n==================================================================================================================================\n";
        std::cout << " SPSculpt Quad Cap Verification & Diagnostic Breakdown:\n";
        std::cout << "==================================================================================================================================\n";
        for (const auto& r : rows) {
            if (!r.isTrim) continue;
            std::cout << " * " << r.toolName << ":\n";
            std::cout << "   - Quad Grid Status:   " << (r.quadCapPassed ? "CLOSED WITH QUAD GRID [OK]" : "NOT CLOSED WITH QUAD GRID [NO QUAD]") << "\n";
            std::cout << "   - Cap Face Analysis:  " << (r.capQuads + r.capTris) << " total cap faces ("
                      << r.capQuads << " quads, " << r.capTris << " triangles -> "
                      << std::fixed << std::setprecision(1) << (r.capQuadRatio * 100.0f) << "% quad ratio)\n";
            std::cout << "   - Patch Components:   " << r.capPatches << " patch(es) detected ("
                      << r.capPatchesSuccess << " quad-capped, " << r.capPatchesFallback << " fell back)\n";
            if (!r.quadCapPassed || !r.capFallbackReason.empty()) {
                std::cout << "   - Fallback Reason:    "
                          << (!r.capFallbackReason.empty() ? r.capFallbackReason : "Insufficient quad ratio / no regular quads generated") << "\n";
            }
            std::cout << "\n";
        }
        std::cout << "==================================================================================================================================\n";

        if (allIntegrityPassed && allQuadCapsPassed) {
            std::cout << " [SUCCESS] All Trim Tool & Clip Curve operations, Undo/Redo & Quad Cap checks PASSED!\n";
        } else if (allIntegrityPassed && !allQuadCapsPassed) {
            std::cout << " [ATTENTION] Undo/Redo & geometry integrity PASSED, but one or more cuts failed to close with a quad grid!\n";
            std::cout << "             (See Quad Cap Diagnostic Breakdown above for details to guide quad meshing fixes)\n";
        } else {
            std::cout << " [FAILURE] One or more benchmark operations or integrity checks failed!\n";
        }
        std::cout << "==================================================================================================================================\n\n";

        return allIntegrityPassed;
    } catch (const std::exception& e) {
        std::cerr << "EXCEPTION during Trim & Clip Curve benchmark: " << e.what() << std::endl;
        return false;
    }
}

} // namespace TrimClipBenchmark
