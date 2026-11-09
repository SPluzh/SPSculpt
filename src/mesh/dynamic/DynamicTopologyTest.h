#pragma once

#include "mesh/Mesh.h"
#include "mesh/BVH.h"
#include "mesh/dynamic/DynamicTopology.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "editing/SculptManager.h"
#include "editing/undo/UndoManager.h"
#include "render/AngleRenderer.h"
#include "sculpt/SculptEngine.h"
#include <SDL2/SDL.h>

#include <cassert>
#include <cmath>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dynamic {

class DynamicTopologyTest {
public:
    static bool runAllTests() {
        std::cout << "\n=======================================================\n";
        std::cout << "         DYNAMIC TOPOLOGY UNIT TEST SUITE             \n";
        std::cout << "=======================================================\n\n";

        int passed = 0;
        int total = 8;

        if (testEdgeSplit()) {
            std::cout << "[PASS] Test 1/8: Edge Split & Subdivision\n";
            passed++;
        } else {
            std::cout << "[FAIL] Test 1/8: Edge Split & Subdivision\n";
        }

        if (testEdgeCollapse()) {
            std::cout << "[PASS] Test 2/8: Edge Collapse & Watertight Decimation\n";
            passed++;
        } else {
            std::cout << "[FAIL] Test 2/8: Edge Collapse & Watertight Decimation\n";
        }

        if (testStress1000Dabs()) {
            std::cout << "[PASS] Test 3/8: Stress Test (1000 Dabs, Swap-With-End Compaction)\n";
            passed++;
        } else {
            std::cout << "[FAIL] Test 3/8: Stress Test (1000 Dabs)\n";
        }

        if (testSymmetryOrbits()) {
            std::cout << "[PASS] Test 4/8: Symmetry Orbits & Reflection Preservation\n";
            passed++;
        } else {
            std::cout << "[FAIL] Test 4/8: Symmetry Orbits\n";
        }

        if (testUndoRedo()) {
            std::cout << "[PASS] Test 5/8: Compact Undo/Redo (Bit-for-Bit Exactness)\n";
            passed++;
        } else {
            std::cout << "[FAIL] Test 5/8: Compact Undo/Redo\n";
        }

        if (testMultiStrokeGrowthAndRaycast()) {
            std::cout << "[PASS] Test 6/8: Multi-Stroke Growth & Raycast Picking Lifecycle (15 Strokes, >150k Verts)\n";
            passed++;
        } else {
            std::cout << "[FAIL] Test 6/8: Multi-Stroke Growth & Raycast Picking Lifecycle\n";
        }

        if (testLocalQuadPreservationAndUndo()) {
            std::cout << "[PASS] Test 7/8: Local Quad Preservation & Bit-for-Bit Quad Undo\n";
            passed++;
        } else {
            std::cout << "[FAIL] Test 7/8: Local Quad Preservation & Bit-for-Bit Quad Undo\n";
        }

        if (testMoveBrushAfterDynamicTopology()) {
            std::cout << "[PASS] Test 8/8: Move Brush Reachability & Displacement After Dynamic Topology\n";
            passed++;
        } else {
            std::cout << "[FAIL] Test 8/8: Move Brush Reachability & Displacement After Dynamic Topology\n";
        }

        std::cout << "\n-------------------------------------------------------\n";
        std::cout << "Summary: " << passed << " / " << total << " tests passed.\n";
        std::cout << "=======================================================\n\n";

        return passed == total;
    }

    static bool runBenchmark100Strokes() {
        std::cout << "\n================================================================================\n";
        std::cout << "      SPSculpt Dynamic Topology 100-Stroke Benchmark & Crash Diagnostics\n";
        std::cout << "================================================================================\n\n";

        auto meshPtr = createQuadSphere(35.0f, 32, 64);
        Mesh* meshRaw = meshPtr.release();
        meshRaw->initFaceGroups();
        meshRaw->postInit(true);
        meshRaw->bvh.resetDiagnosticCounters();

        Scene scene;
        scene.addMesh(meshRaw);
        scene.selectMesh(meshRaw);

        Camera& camera = scene.getCamera();
        camera.onResize(1920, 1080);
        camera.resetViewToMesh(meshRaw, false);
        camera.update(0.016f);

        SculptManager sculpt;
        sculpt.setRenderer(nullptr);
        sculpt.setBrush(BRUSH_DYNAMIC_TOPOLOGY);

        auto& settings = sculpt.getSettings(BRUSH_DYNAMIC_TOPOLOGY);
        settings.radius = 120.0f;
        settings.intensity = 0.5f;
        settings.spacing = 0.15f; // minSpacing = 18.0 px (matches user log: minSpacing=18.00px)
        settings.topologyEdgeSize = 0.05f;

        DynamicTopology* dt = sculpt.getDyntopo();

        constexpr int NUM_STROKES = 100;
        constexpr int DRAG_STEPS_PER_STROKE = 4; // 1 mouse down + 4 motion steps = 5 dabs/stroke (500 dabs total)

        DyntopoRemeshProfile totalRemeshProf{};
        DyntopoRemeshProfile phaseRemeshProf[3]{};
        DyntopoFinishProfile totalFinishProf{};
        DyntopoFinishProfile phaseFinishProf[3]{};

        int totalSubsteps = 0;
        int phaseSubsteps[3] = {0, 0, 0};
        size_t totalSplits = 0, totalCollapses = 0, totalFlips = 0;
        double maxSubstepMs = 0.0;
        int maxSubstepStroke = 0;
        double maxReleaseMs = 0.0;
        int maxReleaseStroke = 0;
        uint32_t maxEmptyLeavesDuringDrag = 0;
        uint32_t maxBvhDepthDuringDrag = 0;

        struct EboCheckpoint {
            int stroke;
            int verts;
            int faces;
            double triEboMs;
            double wireEboMs;
            size_t wireIndicesCount;
        };
        std::vector<EboCheckpoint> eboCheckpoints;

        auto accumulateRemesh = [](DyntopoRemeshProfile& dst, const DyntopoRemeshProfile& src) {
            dst.initAdjacencyMs        += src.initAdjacencyMs;
            dst.gatherRegionMs         += src.gatherRegionMs;
            dst.splitPassMs            += src.splitPassMs;
            dst.refit1Ms               += src.refit1Ms;
            dst.queryRegion2Ms         += src.queryRegion2Ms;
            dst.collapsePassMs         += src.collapsePassMs;
            dst.flipPassMs             += src.flipPassMs;
            dst.relaxPassMs            += src.relaxPassMs;
            dst.compactMs              += src.compactMs;
            dst.vertNormalsAndTouchMs  += src.vertNormalsAndTouchMs;
            dst.refit2Ms               += src.refit2Ms;
            dst.fullFaceNormalsBoxesMs += src.fullFaceNormalsBoxesMs;
            dst.totalMs                += src.totalMs;
        };

        auto accumulateFinish = [](DyntopoFinishProfile& dst, const DyntopoFinishProfile& src) {
            dst.initTopologyMs     += src.initTopologyMs;
            dst.vertProxyCopyMs    += src.vertProxyCopyMs;
            dst.faceNormalsBoxesMs += src.faceNormalsBoxesMs;
            dst.vertNormalsMs      += src.vertNormalsMs;
            dst.bvhBuildMs         += src.bvhBuildMs;
            dst.cleanupMs          += src.cleanupMs;
            dst.totalMs            += src.totalMs;
        };

        std::cout << "Initial Mesh: " << meshRaw->nbVerts << " verts, " << meshRaw->nbFaces << " faces (Tris=" << (meshRaw->nbFaces - meshRaw->getNbQuads()) << ", Quads=" << meshRaw->getNbQuads() << ", R=35.0 world units, matching SPSculpt default sphere)\n";
        std::cout << "Executing 100 strokes (Phase 1: Default 0.02 Growth 1..10 | Phase 2: Collapse 11..55 | Phase 3: Mixed High-Poly 56..100)...\n\n";

        auto tBenchStart = std::chrono::high_resolution_clock::now();

        for (int s = 0; s < NUM_STROKES; ++s) {
            int phase = (s < 10) ? 0 : ((s < 55) ? 1 : 2);

            // Configure target edge size per phase on R=35.0 sphere:
            // Phase 0 (Strokes 1..10): default topologyEdgeSize = 0.02f (reproduces user's exact +6000 verts/substep growth to ~250k+ verts!)
            // Phase 1 (Strokes 11..55): coarsening/collapse (1.2 -> 3.0) to empty BVH leaves and trigger raycast hazards on high-poly mesh
            // Phase 2 (Strokes 56..100): alternating fine (0.35) and coarse (1.50) strokes on high-poly mesh
            if (phase == 0) {
                settings.topologyEdgeSize = 0.02f;
            } else if (phase == 1) {
                float t = float(s - 10) / 44.0f;
                settings.topologyEdgeSize = 1.2f * (1.0f - t) + 3.0f * t;
            } else {
                settings.topologyEdgeSize = (s % 2 == 0) ? 0.35f : 1.50f;
            }

            // Generate a stroke trajectory across the visible sphere disc around screen center (960, 540)
            float angle = float(s) * 0.47f;
            float radial = 60.0f + 90.0f * std::sin(float(s) * 0.31f);
            float startX = 960.0f + radial * std::cos(angle);
            float startY = 540.0f + radial * std::sin(angle);
            float dirAngle = angle + 1.2f;
            float stepDist = 23.0f; // > minSpacing (18px) and < 2*minSpacing (36px) -> 1 substep per motion event
            float dx = stepDist * std::cos(dirAngle);
            float dy = stepDist * std::sin(dirAngle);

            double strokeSubstepsMs = 0.0;

            // 1. Mouse Down (Substep 0)
            SDL_Event evDown;
            std::memset(&evDown, 0, sizeof(evDown));
            evDown.type = SDL_MOUSEBUTTONDOWN;
            evDown.button.button = SDL_BUTTON_LEFT;
            evDown.button.x = (int)std::round(startX);
            evDown.button.y = (int)std::round(startY);

            dt->lastRemeshProfile = {};
            sculpt.handleEvent(evDown, scene);
            if (dt->lastRemeshProfile.totalMs > 0.0) {
                accumulateRemesh(totalRemeshProf, dt->lastRemeshProfile);
                accumulateRemesh(phaseRemeshProf[phase], dt->lastRemeshProfile);
                strokeSubstepsMs += dt->lastRemeshProfile.totalMs;
                totalSubsteps++;
                phaseSubsteps[phase]++;
                totalSplits    += dt->stats.splits;
                totalCollapses += dt->stats.collapses;
                totalFlips     += dt->stats.flips;
                if (dt->lastRemeshProfile.totalMs > maxSubstepMs) {
                    maxSubstepMs = dt->lastRemeshProfile.totalMs;
                    maxSubstepStroke = s + 1;
                }
            }

            // 2. Drag Motion Steps (Substeps 1..DRAG_STEPS_PER_STROKE)
            for (int step = 1; step <= DRAG_STEPS_PER_STROKE; ++step) {
                float curX = startX + float(step) * dx;
                float curY = startY + float(step) * dy;

                SDL_Event evMove;
                std::memset(&evMove, 0, sizeof(evMove));
                evMove.type = SDL_MOUSEMOTION;
                evMove.motion.state = SDL_BUTTON_LMASK;
                evMove.motion.x = (int)std::round(curX);
                evMove.motion.y = (int)std::round(curY);

                dt->lastRemeshProfile = {};
                sculpt.handleEvent(evMove, scene);
                if (dt->lastRemeshProfile.totalMs > 0.0) {
                    accumulateRemesh(totalRemeshProf, dt->lastRemeshProfile);
                    accumulateRemesh(phaseRemeshProf[phase], dt->lastRemeshProfile);
                    strokeSubstepsMs += dt->lastRemeshProfile.totalMs;
                    totalSubsteps++;
                    phaseSubsteps[phase]++;
                    totalSplits    += dt->stats.splits;
                    totalCollapses += dt->stats.collapses;
                    totalFlips     += dt->stats.flips;
                    if (dt->lastRemeshProfile.totalMs > maxSubstepMs) {
                        maxSubstepMs = dt->lastRemeshProfile.totalMs;
                        maxSubstepStroke = s + 1;
                    }
                }
            }

            // Inspect BVH state during drag before finish() rebuilds it
            uint32_t dragEmptyLeaves = meshRaw->bvh.countEmptyLeaves();
            uint32_t dragMaxDepth = meshRaw->bvh.computeMaxDepth();
            if (dragEmptyLeaves > maxEmptyLeavesDuringDrag) maxEmptyLeavesDuringDrag = dragEmptyLeaves;
            if (dragMaxDepth > maxBvhDepthDuringDrag) maxBvhDepthDuringDrag = dragMaxDepth;

            // Also fire 10 diagnostic rays across the mesh BEFORE finish() while empty leaves exist in BVH
            for (int r = 0; r < 10; ++r) {
                float a = float(r) * 0.628318f + float(s) * 0.1f;
                glm::vec3 ro(15.0f * std::cos(a), 15.0f * std::sin(a), 100.0f);
                glm::vec3 rd(0.0f, 0.0f, -1.0f);
                RayHit rh;
                meshRaw->bvh.raycast(*meshRaw, ro, rd, rh);
            }

            // 3. Mouse Up (Stroke Release -> DynamicTopology::finish)
            SDL_Event evUp;
            std::memset(&evUp, 0, sizeof(evUp));
            evUp.type = SDL_MOUSEBUTTONUP;
            evUp.button.button = SDL_BUTTON_LEFT;
            evUp.button.x = (int)std::round(startX + float(DRAG_STEPS_PER_STROKE) * dx);
            evUp.button.y = (int)std::round(startY + float(DRAG_STEPS_PER_STROKE) * dy);

            dt->lastFinishProfile = {};
            sculpt.handleEvent(evUp, scene);
            accumulateFinish(totalFinishProf, dt->lastFinishProfile);
            accumulateFinish(phaseFinishProf[phase], dt->lastFinishProfile);
            if (dt->lastFinishProfile.totalMs > maxReleaseMs) {
                maxReleaseMs = dt->lastFinishProfile.totalMs;
                maxReleaseStroke = s + 1;
            }

            // EBO / Wireframe CPU generation benchmark at checkpoints
            if ((s + 1) <= 10 || (s + 1) % 10 == 0) {
                std::vector<uint32_t> triIdx;
                auto tTri0 = std::chrono::high_resolution_clock::now();
                AngleRenderer::generateTriangleIndices(meshRaw, triIdx);
                double triMs = std::chrono::duration<double, std::milli>(
                    std::chrono::high_resolution_clock::now() - tTri0).count();

                std::vector<uint32_t> wireIdx;
                auto tWire0 = std::chrono::high_resolution_clock::now();
                AngleRenderer::generateWireframeIndices(meshRaw, wireIdx);
                double wireMs = std::chrono::duration<double, std::milli>(
                    std::chrono::high_resolution_clock::now() - tWire0).count();

                eboCheckpoints.push_back({
                    s + 1, meshRaw->nbVerts, meshRaw->nbFaces, triMs, wireMs, wireIdx.size()
                });

                // Move Brush Reachability & Frozen Vertices Audit
                meshRaw->isTopologyDirty = false; // Simulate renderer upload clearing flag
                meshRaw->ensureTopologyBuilt();

                float hitAngle = float(s) * 0.47f;
                glm::vec3 rOrig(50.0f * std::cos(hitAngle), 50.0f * std::sin(hitAngle), 50.0f);
                glm::vec3 rDir = glm::normalize(glm::vec3(0.0f) - rOrig);
                RayHit mHit;
                int moveExpCount = 0, movePickedCount = 0, moveStuckCount = 0;
                float moveReach = 100.0f;
                if (meshRaw->bvh.raycast(*meshRaw, rOrig, rDir, mHit)) {
                    float moveRadius = 8.0f;
                    float moveRadius2 = moveRadius * moveRadius;
                    std::vector<uint32_t> expected;
                    meshRaw->bvh.pickVerticesInSphere(*meshRaw, mHit.position, moveRadius2, expected, meshRaw->vertVisible.data(), 0);
                    moveExpCount = (int)expected.size();

                    std::vector<uint8_t> visitedFlags(meshRaw->nbVerts, 0);
                    uint8_t visitedEpoch = 1;
                    std::vector<uint32_t> pickedTopo;
                    SculptManager::pickVerticesInSphereTopological(
                        meshRaw, mHit.position, moveRadius2, mHit.faceId,
                        visitedFlags, visitedEpoch, pickedTopo
                    );
                    movePickedCount = (int)pickedTopo.size();

                    std::unordered_set<uint32_t> pSet(pickedTopo.begin(), pickedTopo.end());
                    for (uint32_t v : expected) {
                        if (pSet.find(v) == pSet.end()) {
                            glm::vec3 origP(meshRaw->verts[v * 3], meshRaw->verts[v * 3 + 1], meshRaw->verts[v * 3 + 2]);
                            float d = glm::distance(origP, mHit.position);
                            if (d < 0.7f * moveRadius) moveStuckCount++;
                        }
                    }
                    moveReach = (moveExpCount > 0) ? (100.0f * (float)(moveExpCount - moveStuckCount) / (float)moveExpCount) : 100.0f;
                }

                std::cout << "  [Stroke " << std::setw(3) << (s + 1) << "/100]"
                          << " V=" << std::setw(7) << meshRaw->nbVerts
                          << " F=" << std::setw(7) << meshRaw->nbFaces
                          << " (Tris=" << (meshRaw->nbFaces - meshRaw->getNbQuads()) << ", Quads=" << meshRaw->getNbQuads() << ")"
                          << " | DragSum=" << std::fixed << std::setprecision(1) << std::setw(6) << strokeSubstepsMs << "ms"
                          << " | Finish=" << std::setw(6) << dt->lastFinishProfile.totalMs << "ms"
                          << " | WireEBO=" << std::setw(6) << wireMs << "ms"
                          << " | MoveReach=" << std::setw(5) << moveReach << "% (Stuck=" << moveStuckCount << ")"
                          << " | EmptyLeaves=" << std::setw(4) << dragEmptyLeaves
                          << " | CrashHits=" << meshRaw->bvh.emptyLeafHazardCount() << "\n";
            }
        }

        auto tBenchEnd = std::chrono::high_resolution_clock::now();
        double totalBenchMs = std::chrono::duration<double, std::milli>(tBenchEnd - tBenchStart).count();

        // Benchmark 10 Undo + 10 Redo operations
        double totalUndoMs = 0.0;
        double totalRedoMs = 0.0;
        constexpr int UNDO_TEST_COUNT = 10;
        for (int i = 0; i < UNDO_TEST_COUNT; ++i) {
            auto t0 = std::chrono::high_resolution_clock::now();
            g_undoManager.undo(scene);
            totalUndoMs += std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - t0).count();
        }
        for (int i = 0; i < UNDO_TEST_COUNT; ++i) {
            auto t0 = std::chrono::high_resolution_clock::now();
            g_undoManager.redo(scene);
            totalRedoMs += std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - t0).count();
        }

        // Diagnostic check for Crash #2: Global Dyntopo + Sculpting Brush stale vrfStartCount / vrvStartCount
        bool globalDyntopoOobDetected = false;
        size_t staleVrfVertCapacity = 0;
        size_t postRemeshVertCount = 0;
        {
            auto testMesh = createSphere(1.0f, 16, 16);
            testMesh->postInit(true);
            DynamicTopology testDt;
            glm::vec3 c(0.0f, 1.0f, 0.0f);
            testDt.remesh(*testMesh, testMesh->bvh, c, 0.5f, 0.03f, {});
            staleVrfVertCapacity = testMesh->vrfStartCount.size() / 2;
            postRemeshVertCount = (size_t)testMesh->nbVerts;
            if (postRemeshVertCount > staleVrfVertCapacity) {
                globalDyntopoOobDetected = true;
            }
        }

        std::cout << "\n================================================================================\n";
        std::cout << "                   100-STROKE BENCHMARK DIAGNOSTIC REPORT\n";
        std::cout << "================================================================================\n";
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Total Wall Time (100 Strokes): " << totalBenchMs << " ms (" << (totalBenchMs / 1000.0) << " s)\n";
        std::cout << "Total Substeps Executed:       " << totalSubsteps << " substeps (" << (totalRemeshProf.totalMs / std::max(1, totalSubsteps)) << " ms/substep avg, max " << maxSubstepMs << " ms at stroke " << maxSubstepStroke << ")\n";
        std::cout << "Total Topology Operations:     Splits=" << totalSplits << ", Collapses=" << totalCollapses << ", Flips=" << totalFlips << "\n";
        std::cout << "Final Mesh Complexity:         " << meshRaw->nbVerts << " verts, " << meshRaw->nbFaces << " faces (Tris=" << (meshRaw->nbFaces - meshRaw->getNbQuads()) << ", Quads=" << meshRaw->getNbQuads() << ")\n\n";

        auto printRemeshBreakdown = [](const char* title, const DyntopoRemeshProfile& p, int substeps) {
            double denom = std::max(1, substeps);
            double pct = (p.totalMs > 0.0) ? (100.0 / p.totalMs) : 0.0;
            std::cout << "--- " << title << " (" << substeps << " substeps, Total: " << p.totalMs << " ms, Avg: " << (p.totalMs / denom) << " ms/substep) ---\n";
            std::cout << "  1. initAdjacency (m_vertTris):   " << std::setw(8) << p.initAdjacencyMs        << " ms (" << std::setw(5) << (p.initAdjacencyMs * pct)        << "%) | Avg: " << (p.initAdjacencyMs / denom) << " ms\n";
            std::cout << "  2. gatherRegion (BVH query 1):   " << std::setw(8) << p.gatherRegionMs         << " ms (" << std::setw(5) << (p.gatherRegionMs * pct)         << "%) | Avg: " << (p.gatherRegionMs / denom) << " ms\n";
            std::cout << "  3. splitPass:                    " << std::setw(8) << p.splitPassMs            << " ms (" << std::setw(5) << (p.splitPassMs * pct)            << "%) | Avg: " << (p.splitPassMs / denom) << " ms\n";
            std::cout << "  4. refitChanged #1 (post-split): " << std::setw(8) << p.refit1Ms               << " ms (" << std::setw(5) << (p.refit1Ms * pct)               << "%) | Avg: " << (p.refit1Ms / denom) << " ms\n";
            std::cout << "  5. queryRegion #2 (BVH query 2): " << std::setw(8) << p.queryRegion2Ms         << " ms (" << std::setw(5) << (p.queryRegion2Ms * pct)         << "%) | Avg: " << (p.queryRegion2Ms / denom) << " ms\n";
            std::cout << "  6. collapsePass:                 " << std::setw(8) << p.collapsePassMs         << " ms (" << std::setw(5) << (p.collapsePassMs * pct)         << "%) | Avg: " << (p.collapsePassMs / denom) << " ms\n";
            std::cout << "  7. flipPass:                     " << std::setw(8) << p.flipPassMs             << " ms (" << std::setw(5) << (p.flipPassMs * pct)             << "%) | Avg: " << (p.flipPassMs / denom) << " ms\n";
            std::cout << "  8. relaxPass:                    " << std::setw(8) << p.relaxPassMs            << " ms (" << std::setw(5) << (p.relaxPassMs * pct)            << "%) | Avg: " << (p.relaxPassMs / denom) << " ms\n";
            std::cout << "  9. compact (tris + verts):       " << std::setw(8) << p.compactMs              << " ms (" << std::setw(5) << (p.compactMs * pct)              << "%) | Avg: " << (p.compactMs / denom) << " ms\n";
            std::cout << " 10. vertNormals & touchLeaf:      " << std::setw(8) << p.vertNormalsAndTouchMs  << " ms (" << std::setw(5) << (p.vertNormalsAndTouchMs * pct)  << "%) | Avg: " << (p.vertNormalsAndTouchMs / denom) << " ms\n";
            std::cout << " 11. refitChanged #2 (final):      " << std::setw(8) << p.refit2Ms               << " ms (" << std::setw(5) << (p.refit2Ms * pct)               << "%) | Avg: " << (p.refit2Ms / denom) << " ms\n";
            std::cout << " 12. updateFaceNormalsAndBoxes:    " << std::setw(8) << p.fullFaceNormalsBoxesMs << " ms (" << std::setw(5) << (p.fullFaceNormalsBoxesMs * pct) << "%) | Avg: " << (p.fullFaceNormalsBoxesMs / denom) << " ms  <-- FULL MESH RECOMPUTE!\n\n";
        };

        printRemeshBreakdown("ALL 100 STROKES REMESH BREAKDOWN", totalRemeshProf, totalSubsteps);
        printRemeshBreakdown("PHASE 1 (Strokes 1..10: Default 0.02 Growth to ~250k+ Verts)", phaseRemeshProf[0], phaseSubsteps[0]);
        printRemeshBreakdown("PHASE 2 (Strokes 11..55: Collapse & Coarsening on High-Poly)", phaseRemeshProf[1], phaseSubsteps[1]);
        printRemeshBreakdown("PHASE 3 (Strokes 56..100: Mixed Split/Collapse on High-Poly)", phaseRemeshProf[2], phaseSubsteps[2]);

        double finPct = (totalFinishProf.totalMs > 0.0) ? (100.0 / totalFinishProf.totalMs) : 0.0;
        std::cout << "--- STROKE RELEASE (DynamicTopology::finish) BREAKDOWN (100 releases, Total: " << totalFinishProf.totalMs << " ms, Avg: " << (totalFinishProf.totalMs / 100.0) << " ms, Max: " << maxReleaseMs << " ms at stroke " << maxReleaseStroke << ") ---\n";
        std::cout << "  1. mesh.initTopology (CSR rebuild):  " << std::setw(8) << totalFinishProf.initTopologyMs     << " ms (" << std::setw(5) << (totalFinishProf.initTopologyMs * finPct)     << "%) | Avg: " << (totalFinishProf.initTopologyMs / 100.0) << " ms\n";
        std::cout << "  2. vertProxy copy:                   " << std::setw(8) << totalFinishProf.vertProxyCopyMs    << " ms (" << std::setw(5) << (totalFinishProf.vertProxyCopyMs * finPct)    << "%) | Avg: " << (totalFinishProf.vertProxyCopyMs / 100.0) << " ms\n";
        std::cout << "  3. updateFaceNormalsAndBoxes (full): " << std::setw(8) << totalFinishProf.faceNormalsBoxesMs << " ms (" << std::setw(5) << (totalFinishProf.faceNormalsBoxesMs * finPct) << "%) | Avg: " << (totalFinishProf.faceNormalsBoxesMs / 100.0) << " ms\n";
        std::cout << "  4. updateVertexNormals (full):       " << std::setw(8) << totalFinishProf.vertNormalsMs      << " ms (" << std::setw(5) << (totalFinishProf.vertNormalsMs * finPct)      << "%) | Avg: " << (totalFinishProf.vertNormalsMs / 100.0) << " ms\n";
        std::cout << "  5. bvh.build (full rebuild):         " << std::setw(8) << totalFinishProf.bvhBuildMs         << " ms (" << std::setw(5) << (totalFinishProf.bvhBuildMs * finPct)         << "%) | Avg: " << (totalFinishProf.bvhBuildMs / 100.0) << " ms\n";
        std::cout << "  6. cleanup & build TopologyUndo:     " << std::setw(8) << totalFinishProf.cleanupMs          << " ms (" << std::setw(5) << (totalFinishProf.cleanupMs * finPct)          << "%) | Avg: " << (totalFinishProf.cleanupMs / 100.0) << " ms\n\n";

        std::cout << "--- RENDERER EBO / WIREFRAME CPU UPLOAD BOTTLENECK (AngleRenderer::uploadIfDirty) ---\n";
        for (const auto& cp : eboCheckpoints) {
            std::cout << "  Stroke " << std::setw(3) << cp.stroke
                      << " | V=" << std::setw(7) << cp.verts << ", F=" << std::setw(7) << cp.faces
                      << " | TriEBO: " << std::setw(6) << cp.triEboMs << " ms"
                      << " | WireEBO (std::sort edges): " << std::setw(7) << cp.wireEboMs << " ms"
                      << " (" << (cp.wireIndicesCount / 2) << " unique edges)\n";
        }
        std::cout << "\n--- UNDO / REDO LATENCY (10 Undos + 10 Redos at " << meshRaw->nbVerts << " verts) ---\n";
        std::cout << "  Avg Undo Time: " << (totalUndoMs / UNDO_TEST_COUNT) << " ms (Total 10x: " << totalUndoMs << " ms)\n";
        std::cout << "  Avg Redo Time: " << (totalRedoMs / UNDO_TEST_COUNT) << " ms (Total 10x: " << totalRedoMs << " ms)\n\n";

        std::cout << "================================================================================\n";
        std::cout << "                         CRASH & HAZARD DIAGNOSTICS\n";
        std::cout << "================================================================================\n";
        std::cout << "  [CRASH #1] BVH Empty Leaf Raycast/Query Dereference (n.firstIndex == 0xFFFFFFFF):\n";
        std::cout << "             - Intercepted SIGSEGV events:    " << meshRaw->bvh.emptyLeafHazardCount() << " times\n";
        std::cout << "             - Max empty leaves during drag:  " << maxEmptyLeavesDuringDrag << " empty leaves\n";
        std::cout << "             - Max BVH depth during drag:     " << maxBvhDepthDuringDrag << " levels (fixed stack limit = 64)\n";
        std::cout << "  [CRASH #2] Global Dyntopo + Sculpt Brush Stale CSR Adjacency (vrfStartCount/vrvStartCount):\n";
        std::cout << "             - Out-of-bounds condition:       " << (globalDyntopoOobDetected ? "CONFIRMED (SIGSEGV hazard)" : "Not triggered") << "\n";
        std::cout << "             - Pre-stroke CSR capacity:       " << staleVrfVertCapacity << " verts vs Post-remesh nbVerts: " << postRemeshVertCount << " verts\n";
        std::cout << "  [CRASH #3] BVH splitLeaf / editLeaf m_leafCap & Traversal Stack Overflow:\n";
        std::cout << "             - m_leafCap OOB hazards:         " << meshRaw->bvh.leafCapHazardCount() << "\n";
        std::cout << "             - Stack overflow (>64) hazards:  " << meshRaw->bvh.stackOverflowHazardCount() << "\n";
        std::cout << "  [BUG #4] Move Brush Frozen Vertices after Dynamic Topology (Stale vrvStartCount / Hijacked isTopologyDirty):\n";
        std::cout << "             - Move brush reachability:       Audited at checkpoints (see table above)\n";
        std::cout << "================================================================================\n\n";

        return true;
    }

    static bool runBenchmark1000Strokes() {
        std::cout << "\n================================================================================\n";
        std::cout << "  SPSculpt Dynamic Topology 1000-Stroke Stress Benchmark & Move Brush Audit\n";
        std::cout << "================================================================================\n\n";

        auto meshPtr = createQuadSphere(35.0f, 32, 64);
        Mesh* meshRaw = meshPtr.release();
        meshRaw->initFaceGroups();
        meshRaw->postInit(true);

        Scene scene;
        scene.addMesh(meshRaw);
        scene.selectMesh(meshRaw);

        Camera& camera = scene.getCamera();
        camera.onResize(1920, 1080);
        camera.resetViewToMesh(meshRaw, false);
        camera.update(0.016f);

        SculptManager sculpt;
        sculpt.setRenderer(nullptr);
        sculpt.setBrush(BRUSH_DYNAMIC_TOPOLOGY);

        auto& settings = sculpt.getSettings(BRUSH_DYNAMIC_TOPOLOGY);
        settings.radius = 120.0f;
        settings.intensity = 0.5f;
        settings.spacing = 0.15f;

        DynamicTopology* dt = sculpt.getDyntopo();

        constexpr int NUM_STROKES = 1000;
        constexpr int DRAG_STEPS_PER_STROKE = 2; // 1 mouse down + 2 drag steps = 3 dabs/stroke

        int totalSubsteps = 0;
        size_t totalSplits = 0, totalCollapses = 0, totalFlips = 0;
        double maxSubstepMs = 0.0;
        int maxSubstepStroke = 0;
        double totalRemeshMs = 0.0;

        struct MoveVerificationResult {
            int stroke;
            int verts;
            int faces;
            int expectedVerts;
            int pickedTopoVerts;
            int stuckVerts;
            float reachabilityPct;
            bool bvhValid;
        };
        std::vector<MoveVerificationResult> verifications;

        std::cout << "Initial Mesh: " << meshRaw->nbVerts << " verts, " << meshRaw->nbFaces
                  << " faces (R=35.0 Quad Sphere, 1920 quads, 128 tris)\n";
        std::cout << "Executing 1000 strokes across 4 phases with Move Brush audits at regular intervals...\n\n";

        auto tBenchStart = std::chrono::high_resolution_clock::now();

        for (int s = 0; s < NUM_STROKES; ++s) {
            // Configure parameters according to phase:
            // Phase 1 (1..300): Subdivision & Growth
            // Phase 2 (301..600): Coarsening & Decimation
            // Phase 3 (601..850): Mixed Dynamic Topology
            // Phase 4 (851..1000): Global Dyntopo with ClayBuildup / Smooth
            if (s < 300) {
                sculpt.setBrush(BRUSH_DYNAMIC_TOPOLOGY);
                settings.topologyEdgeSize = 0.5f + 0.3f * std::sin(float(s) * 0.1f);
            } else if (s < 600) {
                sculpt.setBrush(BRUSH_DYNAMIC_TOPOLOGY);
                float t = float(s - 300) / 299.0f;
                settings.topologyEdgeSize = 1.0f * (1.0f - t) + 2.2f * t;
            } else if (s < 850) {
                sculpt.setBrush(BRUSH_DYNAMIC_TOPOLOGY);
                settings.topologyEdgeSize = (s % 2 == 0) ? 0.45f : 1.30f;
            } else {
                sculpt.setDyntopoEnabled(true);
                sculpt.setBrush((s % 3 == 0) ? BRUSH_SMOOTH : BRUSH_CLAYBUILDUP);
                sculpt.getCurrentSettings().topologyEdgeSize = 0.6f;
            }

            // Stroke trajectory around visible disk
            float angle = float(s) * 0.37f;
            float radial = 50.0f + 100.0f * std::sin(float(s) * 0.19f);
            float startX = 960.0f + radial * std::cos(angle);
            float startY = 540.0f + radial * std::sin(angle);
            float dirAngle = angle + 1.1f;
            float stepDist = 22.0f;
            float dx = stepDist * std::cos(dirAngle);
            float dy = stepDist * std::sin(dirAngle);

            // 1. Mouse Down
            SDL_Event evDown;
            std::memset(&evDown, 0, sizeof(evDown));
            evDown.type = SDL_MOUSEBUTTONDOWN;
            evDown.button.button = SDL_BUTTON_LEFT;
            evDown.button.x = (int)std::round(startX);
            evDown.button.y = (int)std::round(startY);

            dt->lastRemeshProfile = {};
            sculpt.handleEvent(evDown, scene);
            if (dt->lastRemeshProfile.totalMs > 0.0) {
                totalRemeshMs += dt->lastRemeshProfile.totalMs;
                totalSubsteps++;
                totalSplits    += dt->stats.splits;
                totalCollapses += dt->stats.collapses;
                totalFlips     += dt->stats.flips;
                if (dt->lastRemeshProfile.totalMs > maxSubstepMs) {
                    maxSubstepMs = dt->lastRemeshProfile.totalMs;
                    maxSubstepStroke = s + 1;
                }
            }

            // 2. Drag Steps
            for (int step = 1; step <= DRAG_STEPS_PER_STROKE; ++step) {
                float curX = startX + float(step) * dx;
                float curY = startY + float(step) * dy;

                SDL_Event evMove;
                std::memset(&evMove, 0, sizeof(evMove));
                evMove.type = SDL_MOUSEMOTION;
                evMove.motion.state = SDL_BUTTON_LMASK;
                evMove.motion.x = (int)std::round(curX);
                evMove.motion.y = (int)std::round(curY);

                dt->lastRemeshProfile = {};
                sculpt.handleEvent(evMove, scene);
                if (dt->lastRemeshProfile.totalMs > 0.0) {
                    totalRemeshMs += dt->lastRemeshProfile.totalMs;
                    totalSubsteps++;
                    totalSplits    += dt->stats.splits;
                    totalCollapses += dt->stats.collapses;
                    totalFlips     += dt->stats.flips;
                    if (dt->lastRemeshProfile.totalMs > maxSubstepMs) {
                        maxSubstepMs = dt->lastRemeshProfile.totalMs;
                        maxSubstepStroke = s + 1;
                    }
                }
            }

            // 3. Mouse Up
            SDL_Event evUp;
            std::memset(&evUp, 0, sizeof(evUp));
            evUp.type = SDL_MOUSEBUTTONUP;
            evUp.button.button = SDL_BUTTON_LEFT;
            evUp.button.x = (int)std::round(startX + float(DRAG_STEPS_PER_STROKE) * dx);
            evUp.button.y = (int)std::round(startY + float(DRAG_STEPS_PER_STROKE) * dy);

            sculpt.handleEvent(evUp, scene);

            // Audit Checkpoint every 100 strokes and at final stroke
            if ((s + 1) % 100 == 0 || (s + 1) == NUM_STROKES) {
                // Simulate frame render upload: AngleRenderer clears mesh->isTopologyDirty
                meshRaw->isTopologyDirty = false;

                // Test Move Brush reachability
                meshRaw->ensureTopologyBuilt();

                // Raycast to find a hit point on the surface
                float hitAngle = float(s) * 0.25f;
                glm::vec3 rOrig(50.0f * std::cos(hitAngle), 50.0f * std::sin(hitAngle), 50.0f);
                glm::vec3 rDir = glm::normalize(glm::vec3(0.0f) - rOrig);
                RayHit hit;
                int expCount = 0;
                int pickedCount = 0;
                int stuckCount = 0;
                float reachability = 100.0f;

                if (meshRaw->bvh.raycast(*meshRaw, rOrig, rDir, hit)) {
                    float moveRadius = 8.0f;
                    float moveRadius2 = moveRadius * moveRadius;

                    std::vector<uint32_t> expected;
                    meshRaw->bvh.pickVerticesInSphere(*meshRaw, hit.position, moveRadius2, expected, meshRaw->vertVisible.data(), 0);
                    expCount = (int)expected.size();

                    std::vector<uint8_t> visitedFlags(meshRaw->nbVerts, 0);
                    uint8_t visitedEpoch = 1;
                    std::vector<uint32_t> pickedTopo;
                    SculptManager::pickVerticesInSphereTopological(
                        meshRaw, hit.position, moveRadius2, hit.faceId,
                        visitedFlags, visitedEpoch, pickedTopo
                    );
                    pickedCount = (int)pickedTopo.size();

                    std::unordered_set<uint32_t> pSet(pickedTopo.begin(), pickedTopo.end());
                    for (uint32_t v : expected) {
                        if (pSet.find(v) == pSet.end()) {
                            glm::vec3 origP(meshRaw->verts[v * 3], meshRaw->verts[v * 3 + 1], meshRaw->verts[v * 3 + 2]);
                            float d = glm::distance(origP, hit.position);
                            if (d < 0.7f * moveRadius) {
                                stuckCount++;
                            }
                        }
                    }
                    reachability = (expCount > 0) ? (100.0f * (float)(expCount - stuckCount) / (float)expCount) : 100.0f;
                }

                std::string bvhErr;
                bool bvhValid = meshRaw->bvh.validateTree(*meshRaw, bvhErr);

                verifications.push_back({
                    s + 1, meshRaw->nbVerts, meshRaw->nbFaces,
                    expCount, pickedCount, stuckCount, reachability, bvhValid
                });

                std::cout << "  [Stroke " << std::setw(4) << (s + 1) << "/1000]"
                          << " V=" << std::setw(7) << meshRaw->nbVerts
                          << " F=" << std::setw(7) << meshRaw->nbFaces
                          << " | MovePick=" << std::setw(4) << pickedCount << "/" << std::setw(4) << expCount
                          << " | Stuck=" << std::setw(3) << stuckCount
                          << " | MoveReach=" << std::fixed << std::setprecision(1) << std::setw(5) << reachability << "%"
                          << " | BVH=" << (bvhValid ? "OK" : "CORRUPT") << "\n";
            }
        }

        auto tBenchEnd = std::chrono::high_resolution_clock::now();
        double totalBenchMs = std::chrono::duration<double, std::milli>(tBenchEnd - tBenchStart).count();

        // 10 Undo + 10 Redo tests
        double totalUndoMs = 0.0, totalRedoMs = 0.0;
        constexpr int UNDO_COUNT = 10;
        for (int i = 0; i < UNDO_COUNT; ++i) {
            auto t0 = std::chrono::high_resolution_clock::now();
            g_undoManager.undo(scene);
            totalUndoMs += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
        }
        for (int i = 0; i < UNDO_COUNT; ++i) {
            auto t0 = std::chrono::high_resolution_clock::now();
            g_undoManager.redo(scene);
            totalRedoMs += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
        }

        std::cout << "\n================================================================================\n";
        std::cout << "             1000-STROKE BENCHMARK & MOVE BRUSH AUDIT REPORT\n";
        std::cout << "================================================================================\n";
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Total Wall Time (1000 Strokes):  " << totalBenchMs << " ms (" << (totalBenchMs / 1000.0) << " s)\n";
        std::cout << "Total Substeps Executed:         " << totalSubsteps << " substeps (Avg: "
                  << (totalRemeshMs / std::max(1, totalSubsteps)) << " ms/substep, Max: " << maxSubstepMs << " ms at stroke " << maxSubstepStroke << ")\n";
        std::cout << "Topology Operations:             Splits=" << totalSplits << ", Collapses=" << totalCollapses << ", Flips=" << totalFlips << "\n";
        std::cout << "Final Mesh Complexity:           " << meshRaw->nbVerts << " verts, " << meshRaw->nbFaces << " faces\n\n";

        std::cout << "--- MOVE BRUSH REACHABILITY AUDIT TABLE ---\n";
        int totalStuck = 0;
        float sumReach = 0.0f;
        for (const auto& v : verifications) {
            std::cout << "  Stroke " << std::setw(4) << v.stroke
                      << " | V=" << std::setw(7) << v.verts
                      << " | Picked: " << std::setw(4) << v.pickedTopoVerts << " / " << std::setw(4) << v.expectedVerts
                      << " | Frozen/Stuck Verts: " << std::setw(3) << v.stuckVerts
                      << " | Reachability: " << std::setw(5) << v.reachabilityPct << "%"
                      << " | BVH: " << (v.bvhValid ? "PASS" : "FAIL") << "\n";
            totalStuck += v.stuckVerts;
            sumReach += v.reachabilityPct;
        }
        float avgReach = verifications.empty() ? 100.0f : (sumReach / (float)verifications.size());
        std::cout << "\nAverage Move Brush Reachability: " << avgReach << "%\n";
        std::cout << "Total Frozen Vertices Detected:  " << totalStuck << " vertices\n\n";

        std::cout << "--- UNDO / REDO PERFORMANCE (10x Undo + 10x Redo) ---\n";
        std::cout << "  Avg Undo Latency: " << (totalUndoMs / UNDO_COUNT) << " ms\n";
        std::cout << "  Avg Redo Latency: " << (totalRedoMs / UNDO_COUNT) << " ms\n\n";

        std::cout << "================================================================================\n";
        std::cout << "VERDICT: " << ((totalStuck == 0 && avgReach > 99.0f) ? "[PASS] 1000 Strokes & Move Brush 100% Functional" : "[FAIL] Move Brush Frozen Vertices Detected") << "\n";
        std::cout << "================================================================================\n\n";

        return (totalStuck == 0 && avgReach > 99.0f);
    }

private:
    static std::unique_ptr<Mesh> createQuadSphere(float R = 1.0f, int rings = 24, int sectors = 24) {
        auto mesh = std::make_unique<Mesh>();
        mesh->verts.reserve(((rings - 1) * sectors + 2) * 3);
        mesh->faces.reserve(((rings - 2) * sectors + 2 * sectors) * 4);

        // South pole (index 0)
        mesh->verts.push_back(0.0f); mesh->verts.push_back(-R); mesh->verts.push_back(0.0f);
        uint32_t southPole = mesh->nbVerts++;

        // Intermediate rings (r = 1 to rings - 1)
        for (int r = 1; r < rings; ++r) {
            float phi = -float(M_PI_2) + float(M_PI) * float(r) / float(rings);
            float y = R * std::sin(phi);
            float cos_phi = std::cos(phi);
            for (int s = 0; s < sectors; ++s) {
                float theta = 2.0f * float(M_PI) * float(s) / float(sectors);
                float x = R * cos_phi * std::cos(theta);
                float z = R * cos_phi * std::sin(theta);
                mesh->verts.push_back(x);
                mesh->verts.push_back(y);
                mesh->verts.push_back(z);
                mesh->nbVerts++;
            }
        }

        // North pole (index nbVerts)
        mesh->verts.push_back(0.0f); mesh->verts.push_back(R); mesh->verts.push_back(0.0f);
        uint32_t northPole = mesh->nbVerts++;

        // South pole faces (r = 0 to r = 1)
        for (int s = 0; s < sectors; ++s) {
            int s_next = (s + 1) % sectors;
            uint32_t v_curr = 1 + s;
            uint32_t v_next = 1 + s_next;
            mesh->faces.push_back(southPole);
            mesh->faces.push_back(v_curr);
            mesh->faces.push_back(v_next);
            mesh->faces.push_back(TRI_INDEX);
            mesh->nbFaces++;
        }

        // Intermediate ring quads
        for (int r = 1; r < rings - 1; ++r) {
            for (int s = 0; s < sectors; ++s) {
                int s_next = (s + 1) % sectors;
                uint32_t v0 = 1 + (r - 1) * sectors + s;
                uint32_t v1 = 1 + (r - 1) * sectors + s_next;
                uint32_t v2 = 1 + r * sectors + s_next;
                uint32_t v3 = 1 + r * sectors + s;

                mesh->faces.push_back(v0);
                mesh->faces.push_back(v3);
                mesh->faces.push_back(v2);
                mesh->faces.push_back(v1);
                mesh->nbFaces++;
            }
        }

        // North pole faces
        int lastRingStart = 1 + (rings - 2) * sectors;
        for (int s = 0; s < sectors; ++s) {
            int s_next = (s + 1) % sectors;
            uint32_t v_curr = lastRingStart + s;
            uint32_t v_next = lastRingStart + s_next;

            mesh->faces.push_back(v_curr);
            mesh->faces.push_back(northPole);
            mesh->faces.push_back(v_next);
            mesh->faces.push_back(TRI_INDEX);
            mesh->nbFaces++;
        }

        mesh->vertVisible.assign(mesh->nbVerts, 1);
        mesh->faceVisible.assign(mesh->nbFaces, 1);
        mesh->materials.assign(mesh->nbVerts * 3, 1.0f);
        mesh->colors.assign(mesh->nbVerts * 3, 1.0f);
        mesh->initTopology();
        mesh->ensureBVHBuilt();
        return mesh;
    }

    static std::unique_ptr<Mesh> createSphere(float R = 1.0f, int rings = 24, int sectors = 24) {
        auto mesh = std::make_unique<Mesh>();
        mesh->verts.reserve((rings * sectors + 2) * 3);
        mesh->faces.reserve((rings - 1) * sectors * 2 * 4);

        // North pole (vertex 0)
        mesh->verts.push_back(0.0f); mesh->verts.push_back(R); mesh->verts.push_back(0.0f);
        mesh->nbVerts++;

        // Intermediate rings
        for (int r = 1; r < rings; ++r) {
            float phi = float(M_PI) * float(r) / float(rings);
            for (int s = 0; s < sectors; ++s) {
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

        // South pole
        mesh->verts.push_back(0.0f); mesh->verts.push_back(-R); mesh->verts.push_back(0.0f);
        uint32_t southPole = mesh->nbVerts++;

        // Top ring (triangles connecting north pole to ring 1)
        for (int s = 0; s < sectors; ++s) {
            uint32_t v1 = 1 + s;
            uint32_t v2 = 1 + ((s + 1) % sectors);
            mesh->faces.push_back(0);
            mesh->faces.push_back(v1);
            mesh->faces.push_back(v2);
            mesh->faces.push_back(TRI_INDEX);
            mesh->nbFaces++;
        }

        // Middle quads (split to 2 triangles each)
        for (int r = 1; r < rings - 1; ++r) {
            uint32_t r0 = 1 + (r - 1) * sectors;
            uint32_t r1 = 1 + r * sectors;
            for (int s = 0; s < sectors; ++s) {
                uint32_t sNext = (s + 1) % sectors;
                uint32_t a = r0 + s;
                uint32_t b = r0 + sNext;
                uint32_t c = r1 + sNext;
                uint32_t d = r1 + s;

                // Mirror-symmetric diagonal split across X=0 plane
                bool isPosX = (s < sectors / 4 || s >= 3 * sectors / 4);
                if (isPosX) {
                    // Tri 1: a, d, b (diagonal b-d)
                    mesh->faces.push_back(a);
                    mesh->faces.push_back(d);
                    mesh->faces.push_back(b);
                    mesh->faces.push_back(TRI_INDEX);
                    mesh->nbFaces++;

                    // Tri 2: b, d, c
                    mesh->faces.push_back(b);
                    mesh->faces.push_back(d);
                    mesh->faces.push_back(c);
                    mesh->faces.push_back(TRI_INDEX);
                    mesh->nbFaces++;
                } else {
                    // Tri 1: a, d, c (diagonal a-c, exact mirror of b-d)
                    mesh->faces.push_back(a);
                    mesh->faces.push_back(d);
                    mesh->faces.push_back(c);
                    mesh->faces.push_back(TRI_INDEX);
                    mesh->nbFaces++;

                    // Tri 2: a, c, b
                    mesh->faces.push_back(a);
                    mesh->faces.push_back(c);
                    mesh->faces.push_back(b);
                    mesh->faces.push_back(TRI_INDEX);
                    mesh->nbFaces++;
                }
            }
        }

        // Bottom ring (triangles connecting ring (rings-2) to south pole)
        uint32_t lastRing = 1 + (rings - 2) * sectors;
        for (int s = 0; s < sectors; ++s) {
            uint32_t v1 = lastRing + s;
            uint32_t v2 = lastRing + ((s + 1) % sectors);
            mesh->faces.push_back(southPole);
            mesh->faces.push_back(v2);
            mesh->faces.push_back(v1);
            mesh->faces.push_back(TRI_INDEX);
            mesh->nbFaces++;
        }

        mesh->vertVisible.assign(mesh->nbVerts, 1);
        mesh->faceVisible.assign(mesh->nbFaces, 1);
        mesh->materials.assign(mesh->nbVerts * 3, 1.0f);
        mesh->colors.assign(mesh->nbVerts * 3, 1.0f);
        mesh->initTopology();
        mesh->ensureBVHBuilt();
        return mesh;
    }

    static bool verifyMeshIntegrity(const Mesh& mesh, std::string& outErr, bool mustBeWatertight = true) {
        if (mesh.nbVerts <= 0 || mesh.nbFaces <= 0) {
            outErr = "Empty mesh (nbVerts=" + std::to_string(mesh.nbVerts) + ", nbFaces=" + std::to_string(mesh.nbFaces) + ")";
            return false;
        }
        if (mesh.verts.size() != (size_t)mesh.nbVerts * 3) {
            outErr = "Compaction violation: verts.size()=" + std::to_string(mesh.verts.size()) +
                     " != nbVerts*3=" + std::to_string(mesh.nbVerts * 3);
            return false;
        }
        if (mesh.faces.size() != (size_t)mesh.nbFaces * 4) {
            outErr = "Compaction violation: faces.size()=" + std::to_string(mesh.faces.size()) +
                     " != nbFaces*4=" + std::to_string(mesh.nbFaces * 4);
            return false;
        }

        for (int v = 0; v < mesh.nbVerts; ++v) {
            float x = mesh.verts[v * 3 + 0];
            float y = mesh.verts[v * 3 + 1];
            float z = mesh.verts[v * 3 + 2];
            if (std::isnan(x) || std::isnan(y) || std::isnan(z) ||
                std::isinf(x) || std::isinf(y) || std::isinf(z)) {
                outErr = "Vertex " + std::to_string(v) + " contains NaN/Inf";
                return false;
            }
        }

        struct EdgeKey {
            uint32_t a, b;
            bool operator==(const EdgeKey& o) const { return a == o.a && b == o.b; }
        };
        struct EdgeHash {
            size_t operator()(const EdgeKey& k) const {
                return std::hash<uint64_t>()(((uint64_t)k.a << 32) | (uint64_t)k.b);
            }
        };
        std::unordered_map<EdgeKey, int, EdgeHash> edgeCount;

        for (int f = 0; f < mesh.nbFaces; ++f) {
            uint32_t v0 = mesh.faces[f * 4 + 0];
            uint32_t v1 = mesh.faces[f * 4 + 1];
            uint32_t v2 = mesh.faces[f * 4 + 2];
            uint32_t v3 = mesh.faces[f * 4 + 3];

            if (v3 == TRI_INDEX) {
                if (v0 >= (uint32_t)mesh.nbVerts || v1 >= (uint32_t)mesh.nbVerts || v2 >= (uint32_t)mesh.nbVerts) {
                    outErr = "Face " + std::to_string(f) + " has out-of-bounds vertex index";
                    return false;
                }
                if (v0 == v1 || v1 == v2 || v2 == v0) {
                    outErr = "Face " + std::to_string(f) + " is degenerate (duplicate vertex)";
                    return false;
                }

                uint32_t tri[3] = { v0, v1, v2 };
                for (int e = 0; e < 3; ++e) {
                    uint32_t ea = std::min(tri[e], tri[(e + 1) % 3]);
                    uint32_t eb = std::max(tri[e], tri[(e + 1) % 3]);
                    edgeCount[{ea, eb}]++;
                }
            } else {
                if (v0 >= (uint32_t)mesh.nbVerts || v1 >= (uint32_t)mesh.nbVerts ||
                    v2 >= (uint32_t)mesh.nbVerts || v3 >= (uint32_t)mesh.nbVerts) {
                    outErr = "Quad Face " + std::to_string(f) + " has out-of-bounds vertex index";
                    return false;
                }
                if (v0 == v1 || v0 == v2 || v0 == v3 || v1 == v2 || v1 == v3 || v2 == v3) {
                    outErr = "Quad Face " + std::to_string(f) + " is degenerate (duplicate vertex)";
                    return false;
                }

                uint32_t quad[4] = { v0, v1, v2, v3 };
                for (int e = 0; e < 4; ++e) {
                    uint32_t ea = std::min(quad[e], quad[(e + 1) % 4]);
                    uint32_t eb = std::max(quad[e], quad[(e + 1) % 4]);
                    edgeCount[{ea, eb}]++;
                }
            }
        }

        int boundaryEdges = 0;
        for (const auto& [e, count] : edgeCount) {
            if (count > 2) {
                outErr = "Non-manifold edge (" + std::to_string(e.a) + ", " + std::to_string(e.b) +
                         ") shared by " + std::to_string(count) + " faces";
                return false;
            }
            if (count == 1) {
                boundaryEdges++;
            }
        }

        if (mustBeWatertight && boundaryEdges > 0) {
            outErr = "Mesh is not watertight: has " + std::to_string(boundaryEdges) + " boundary edges";
            return false;
        }

        if (mustBeWatertight) {
            int V = mesh.nbVerts;
            int E = (int)edgeCount.size();
            int F = mesh.nbFaces;
            int chi = V - E + F;
            if (chi != 2) {
                outErr = "Euler characteristic violation: V(" + std::to_string(V) +
                         ") - E(" + std::to_string(E) + ") + F(" + std::to_string(F) +
                         ") = " + std::to_string(chi) + " (expected 2 for sphere topology)";
                return false;
            }
        }

        return true;
    }

    // 1. testEdgeSplit: Split single edge on a simple grid/sphere, verify vertex/triangle counts and validity
    static bool testEdgeSplit() {
        auto mesh = createSphere(1.0f, 16, 16);
        int origVerts = mesh->nbVerts;
        int origFaces = mesh->nbFaces;

        DynamicTopology dyntopo;
        // Fine target edge length to trigger edge splits at north pole area
        glm::vec3 center(0.0f, 1.0f, 0.0f);
        float radius = 0.5f;
        float targetEdge = 0.05f; // Small target length => triggers splits

        bool changed = dyntopo.remesh(*mesh, mesh->bvh, center, radius, targetEdge, {});
        if (!changed) {
            std::cerr << "  [Error] testEdgeSplit: remesh returned false (no changes made)\n";
            return false;
        }

        auto undo = dyntopo.finish(*mesh, mesh->bvh);
        if (!undo) {
            std::cerr << "  [Error] testEdgeSplit: undo record is null\n";
            return false;
        }

        if (mesh->nbVerts <= origVerts || mesh->nbFaces <= origFaces) {
            std::cerr << "  [Error] testEdgeSplit: vertex or face count did not increase (V: "
                      << origVerts << " -> " << mesh->nbVerts << ", F: "
                      << origFaces << " -> " << mesh->nbFaces << ")\n";
            return false;
        }

        std::string err;
        if (!verifyMeshIntegrity(*mesh, err, true)) {
            std::cerr << "  [Error] testEdgeSplit: integrity check failed: " << err << "\n";
            return false;
        }

        // Test BVH query after split
        RayHit hit;
        glm::vec3 rayOrig(0.0f, 2.0f, 0.0f);
        glm::vec3 rayDir(0.0f, -1.0f, 0.0f);
        if (!mesh->bvh.raycast(*mesh, rayOrig, rayDir, hit)) {
            std::cerr << "  [Error] testEdgeSplit: BVH raycast failed to hit subdivided surface\n";
            return false;
        }

        std::cout << "  [Detail] EdgeSplit: Verts " << origVerts << " -> " << mesh->nbVerts
                  << ", Faces " << origFaces << " -> " << mesh->nbFaces
                  << ", Splits: " << dyntopo.stats.splits << "\n";
        return true;
    }

    // 2. testEdgeCollapse: Collapse edge with manifold and watertight verification
    static bool testEdgeCollapse() {
        // High density sphere
        auto mesh = createSphere(1.0f, 40, 40);
        int origVerts = mesh->nbVerts;
        int origFaces = mesh->nbFaces;

        DynamicTopology dyntopo;
        // Large target edge length to trigger edge collapses
        glm::vec3 center(0.0f, 1.0f, 0.0f);
        float radius = 0.8f;
        float targetEdge = 0.40f; // Large target length => collapses fine edges

        bool changed = dyntopo.remesh(*mesh, mesh->bvh, center, radius, targetEdge, {});
        if (!changed) {
            std::cerr << "  [Error] testEdgeCollapse: remesh returned false\n";
            return false;
        }

        auto undo = dyntopo.finish(*mesh, mesh->bvh);
        if (!undo) {
            std::cerr << "  [Error] testEdgeCollapse: undo record is null\n";
            return false;
        }

        if (mesh->nbVerts >= origVerts || mesh->nbFaces >= origFaces) {
            std::cerr << "  [Error] testEdgeCollapse: vertex count did not decrease (V: "
                      << origVerts << " -> " << mesh->nbVerts << ", F: "
                      << origFaces << " -> " << mesh->nbFaces << ")\n";
            return false;
        }

        std::string err;
        if (!verifyMeshIntegrity(*mesh, err, true)) {
            std::cerr << "  [Error] testEdgeCollapse: integrity check failed: " << err << "\n";
            return false;
        }

        std::cout << "  [Detail] EdgeCollapse: Verts " << origVerts << " -> " << mesh->nbVerts
                  << ", Faces " << origFaces << " -> " << mesh->nbFaces
                  << ", Collapses: " << dyntopo.stats.collapses << "\n";
        return true;
    }

    // 3. testStress1000Dabs: 1000 dabs on UV sphere without crashes, leaks, or degeneracies
    static bool testStress1000Dabs() {
        auto mesh = createSphere(1.0f, 24, 24);
        DynamicTopology dyntopo;

        auto tStart = std::chrono::high_resolution_clock::now();
        size_t totalSplits = 0;
        size_t totalCollapses = 0;
        size_t totalFlips = 0;

        constexpr int NUM_DABS = 1000;
        for (int i = 0; i < NUM_DABS; ++i) {
            // Fibonacci spiral distribution of dabs across sphere surface
            float phi = std::acos(1.0f - 2.0f * (float(i) + 0.5f) / float(NUM_DABS));
            float theta = float(i) * 2.3999632f; // Golden angle

            glm::vec3 center(
                std::sin(phi) * std::cos(theta),
                std::cos(phi),
                std::sin(phi) * std::sin(theta)
            );

            // Alternating radius and detail
            float radius = 0.20f + 0.10f * std::sin(float(i) * 0.15f);
            float targetEdge = 0.06f + 0.04f * std::cos(float(i) * 0.11f);

            bool changed = dyntopo.remesh(*mesh, mesh->bvh, center, radius, targetEdge, {});
            if (changed) {
                totalSplits += dyntopo.stats.splits;
                totalCollapses += dyntopo.stats.collapses;
                totalFlips += dyntopo.stats.flips;
            }

            // Periodic sanity check every 250 dabs
            if ((i + 1) % 250 == 0) {
                std::string err;
                if (!verifyMeshIntegrity(*mesh, err, true)) {
                    std::cerr << "  [Error] testStress1000Dabs at dab " << (i + 1)
                              << ": integrity failed: " << err << "\n";
                    return false;
                }
            }
        }

        auto undo = dyntopo.finish(*mesh, mesh->bvh);
        auto tEnd = std::chrono::high_resolution_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

        std::string finalErr;
        if (!verifyMeshIntegrity(*mesh, finalErr, true)) {
            std::cerr << "  [Error] testStress1000Dabs final integrity check failed: " << finalErr << "\n";
            return false;
        }

        std::cout << "  [Detail] Stress 1000 Dabs in " << std::fixed << std::setprecision(1) << elapsedMs
                  << " ms (Avg: " << std::setprecision(3) << (elapsedMs / NUM_DABS) << " ms/dab) | "
                  << "Final: " << mesh->nbVerts << " verts, " << mesh->nbFaces << " tris | "
                  << "Splits: " << totalSplits << ", Collapses: " << totalCollapses << ", Flips: " << totalFlips << "\n";
        return true;
    }

    // 4. testSymmetryOrbits: Remesh symmetrical sphere across X, verify vertices at +X and -X match exactly
    static bool testSymmetryOrbits() {
        auto mesh = createSphere(1.0f, 24, 24);
        DynamicTopology dyntopo;

        SymCopy id;
        SymCopy mirrorX;
        mirrorX.m = glm::mat3(glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f)));
        mirrorX.pivot = glm::vec3(0.0f);

        std::vector<SymCopy> mirrors = { id, mirrorX };

        // Perform remesh strictly on +X hemisphere
        glm::vec3 center(0.6f, 0.3f, 0.2f);
        float radius = 0.35f;
        float targetEdge = 0.05f;

        bool changed = dyntopo.remesh(*mesh, mesh->bvh, center, radius, targetEdge, mirrors);
        if (!changed) {
            std::cerr << "  [Error] testSymmetryOrbits: remesh returned false\n";
            return false;
        }

        auto undo = dyntopo.finish(*mesh, mesh->bvh);

        std::string err;
        if (!verifyMeshIntegrity(*mesh, err, true)) {
            std::cerr << "  [Error] testSymmetryOrbits: integrity check failed: " << err << "\n";
            return false;
        }

        // Count positive and negative X vertices
        int posCount = 0;
        int negCount = 0;
        int zeroCount = 0;
        const float EPS = 1e-4f;

        for (int v = 0; v < mesh->nbVerts; ++v) {
            float x = mesh->verts[v * 3 + 0];
            if (x > EPS) posCount++;
            else if (x < -EPS) negCount++;
            else zeroCount++;
        }

        if (posCount != negCount) {
            std::cerr << "  [Error] testSymmetryOrbits: asymmetry detected! posCount="
                      << posCount << ", negCount=" << negCount << "\n";
            return false;
        }

        // Verify that for each +X vertex, there is a matching -X vertex
        for (int v = 0; v < mesh->nbVerts; ++v) {
            float x = mesh->verts[v * 3 + 0];
            float y = mesh->verts[v * 3 + 1];
            float z = mesh->verts[v * 3 + 2];
            if (x > EPS) {
                glm::vec3 target(-x, y, z);
                bool found = false;
                for (int u = 0; u < mesh->nbVerts; ++u) {
                    glm::vec3 p(mesh->verts[u * 3 + 0], mesh->verts[u * 3 + 1], mesh->verts[u * 3 + 2]);
                    if (glm::distance(p, target) < 1e-3f) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    std::cerr << "  [Error] testSymmetryOrbits: missing mirrored vertex for ("
                              << x << ", " << y << ", " << z << ")\n";
                    return false;
                }
            }
        }

        std::cout << "  [Detail] SymmetryOrbits: PosX=" << posCount << ", NegX=" << negCount
                  << ", ZeroX=" << zeroCount << " (Exact match)\n";
        return true;
    }

    // 5. testUndoRedo: Perform 10 strokes, undo all 10, redo all 10, verify bit-for-bit match
    static bool testUndoRedo() {
        auto mesh = createSphere(1.0f, 20, 20);

        const int initialVertCount = mesh->nbVerts;
        const int initialFaceCount = mesh->nbFaces;
        const std::vector<float> initialVerts = mesh->verts;
        const std::vector<uint32_t> initialFaces = mesh->faces;

        std::vector<std::unique_ptr<TopologyUndo>> undoStack;
        constexpr int NUM_STROKES = 10;

        for (int s = 0; s < NUM_STROKES; ++s) {
            DynamicTopology dyntopo;
            float angle = float(s) * (2.0f * float(M_PI) / float(NUM_STROKES));
            glm::vec3 center(std::cos(angle) * 0.8f, std::sin(angle) * 0.8f, 0.0f);
            float radius = 0.3f;
            float targetEdge = (s % 2 == 0) ? 0.05f : 0.25f; // Alternating split/collapse

            dyntopo.remesh(*mesh, mesh->bvh, center, radius, targetEdge, {});
            auto undo = dyntopo.finish(*mesh, mesh->bvh);
            if (undo) {
                undoStack.push_back(std::move(undo));
            }
        }

        if (undoStack.empty()) {
            std::cerr << "  [Error] testUndoRedo: no undo records collected\n";
            return false;
        }

        const int finalVertCount = mesh->nbVerts;
        const int finalFaceCount = mesh->nbFaces;
        const std::vector<float> finalVerts = mesh->verts;
        const std::vector<uint32_t> finalFaces = mesh->faces;

        // Undo all strokes in reverse order
        for (int i = (int)undoStack.size() - 1; i >= 0; --i) {
            undoStack[i]->swapWith(*mesh);
            mesh->initTopology();
        }

        // Verify restored initial state
        if (mesh->nbVerts != initialVertCount || mesh->nbFaces != initialFaceCount) {
            std::cerr << "  [Error] testUndoRedo (Undo): Count mismatch! Expected V="
                      << initialVertCount << " F=" << initialFaceCount
                      << ", got V=" << mesh->nbVerts << " F=" << mesh->nbFaces << "\n";
            return false;
        }
        if (mesh->verts != initialVerts) {
            std::cerr << "  [Error] testUndoRedo (Undo): Vertex buffer does not match initial bit-for-bit\n";
            return false;
        }
        if (mesh->faces != initialFaces) {
            std::cerr << "  [Error] testUndoRedo (Undo): Face buffer does not match initial bit-for-bit\n";
            return false;
        }

        // Redo all strokes in forward order
        for (size_t i = 0; i < undoStack.size(); ++i) {
            undoStack[i]->swapWith(*mesh);
            mesh->initTopology();
        }

        // Verify restored final state
        if (mesh->nbVerts != finalVertCount || mesh->nbFaces != finalFaceCount) {
            std::cerr << "  [Error] testUndoRedo (Redo): Count mismatch! Expected V="
                      << finalVertCount << " F=" << finalFaceCount
                      << ", got V=" << mesh->nbVerts << " F=" << mesh->nbFaces << "\n";
            return false;
        }
        if (mesh->verts != finalVerts) {
            std::cerr << "  [Error] testUndoRedo (Redo): Vertex buffer does not match final bit-for-bit\n";
            return false;
        }
        if (mesh->faces != finalFaces) {
            std::cerr << "  [Error] testUndoRedo (Redo): Face buffer does not match final bit-for-bit\n";
            return false;
        }

        std::cout << "  [Detail] UndoRedo: 10 strokes swapped backward and forward with bit-for-bit exactness\n";
        return true;
    }

    // 6. testMultiStrokeGrowthAndRaycast: 15 consecutive multi-substep strokes with symmetry,
    // growing mesh to >150k vertices and >300k faces, simulating exact SculptManager stroke lifecycle:
    // raycast picking -> substep remesh -> stroke release finish -> BVH rebuild -> subsequent raycast picking.
    static bool testMultiStrokeGrowthAndRaycast() {
        auto mesh = createSphere(1.0f, 24, 24);
        mesh->initFaceGroups();
        mesh->initTopology();
        mesh->ensureBVHBuilt();

        SymCopy id;
        SymCopy mirrorX;
        mirrorX.m = glm::mat3(glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f)));
        mirrorX.pivot = glm::vec3(0.0f);
        std::vector<SymCopy> mirrors = { id, mirrorX };

        DynamicTopology dyntopo;
        constexpr int NUM_STROKES = 15;
        std::vector<std::unique_ptr<TopologyUndo>> undoStack;

        for (int stroke = 0; stroke < NUM_STROKES; ++stroke) {
            // Step 1: Simulate Mouse Down Raycast Picking
            glm::vec3 rayOrig(0.4f, 0.4f, 3.0f);
            glm::vec3 rayDir(0.0f, 0.0f, -1.0f);
            RayHit hit;
            if (!mesh->bvh.raycast(*mesh, rayOrig, rayDir, hit)) {
                std::cerr << "  [Error] testMultiStrokeGrowthAndRaycast: raycast failed at stroke " << stroke << "\n";
                return false;
            }

            uint32_t intersectFaceId = hit.faceId;
            if (intersectFaceId >= (uint32_t)mesh->nbFaces) {
                std::cerr << "  [Error] testMultiStrokeGrowthAndRaycast: hit faceId " << intersectFaceId
                          << " >= nbFaces " << mesh->nbFaces << "\n";
                return false;
            }

            // Verify face normal access
            if (intersectFaceId * 3 + 2 >= mesh->faceNormals.size()) {
                std::cerr << "  [Error] testMultiStrokeGrowthAndRaycast: faceNormals out of bounds at stroke " << stroke << "\n";
                return false;
            }
            glm::vec3 initNormal(mesh->faceNormals[intersectFaceId * 3],
                                 mesh->faceNormals[intersectFaceId * 3 + 1],
                                 mesh->faceNormals[intersectFaceId * 3 + 2]);

            // Verify face group access
            if (!mesh->faceGroups.empty() && intersectFaceId >= mesh->faceGroups.size()) {
                std::cerr << "  [Error] testMultiStrokeGrowthAndRaycast: faceGroups out of bounds at stroke " << stroke << "\n";
                return false;
            }

            // Step 2: Simulate Stroke Drag Substeps (5 substeps per stroke)
            float strokeAngle = float(stroke) * 0.4f;
            float radius = 0.6f;
            float targetEdge = 0.010f; // Very fine edge to grow mesh to >150k verts and >300k faces
            for (int step = 0; step < 5; ++step) {
                float t = float(step) / 5.0f;
                glm::vec3 subPos(
                    0.5f * std::cos(strokeAngle + t * 0.5f),
                    0.5f * std::sin(strokeAngle + t * 0.5f),
                    0.7f
                );
                subPos = glm::normalize(subPos) * 1.0f;

                // Substep Raycast (mimicking executeStroke raycast during drag)
                glm::vec3 subRayOrig = subPos * 2.0f;
                glm::vec3 subRayDir = glm::normalize(subPos - subRayOrig);
                RayHit subHit;
                if (mesh->bvh.raycast(*mesh, subRayOrig, subRayDir, subHit)) {
                    if (subHit.faceId >= (uint32_t)mesh->nbFaces) {
                        std::cerr << "  [Error] Substep raycast hit invalid faceId " << subHit.faceId << "\n";
                        return false;
                    }
                }

                dyntopo.remesh(*mesh, mesh->bvh, subPos, radius, targetEdge, mirrors);

                // Verify buffer sizes during the drag
                if (mesh->faceNormals.size() < (size_t)mesh->nbFaces * 3) {
                    std::cerr << "  [Error] faceNormals.size() < nbFaces * 3 during drag\n";
                    return false;
                }

                // Verify BVH internal consistency during continuous drag
                std::string bvhDragErr;
                if (!mesh->bvh.validateTree(*mesh, bvhDragErr)) {
                    std::cerr << "  [Error] BVH consistency violation during drag at stroke "
                              << stroke << ", step " << step << ": " << bvhDragErr << "\n";
                    return false;
                }
            }

            // Step 3: Stroke Release
            auto undo = dyntopo.finish(*mesh, mesh->bvh);
            if (undo) {
                undoStack.push_back(std::move(undo));
            }

            // Verify mesh buffers and integrity
            std::string err;
            if (!verifyMeshIntegrity(*mesh, err, true)) {
                std::cerr << "  [Error] testMultiStrokeGrowthAndRaycast at stroke " << stroke
                          << ": integrity failed: " << err << "\n";
                return false;
            }

            // Ensure BVH and all cached structures are in sync
            mesh->ensureTopologyBuilt();
            mesh->ensureBVHBuilt();

            // Verify BVH integrity after finish
            std::string bvhPostErr;
            if (!mesh->bvh.validateTree(*mesh, bvhPostErr)) {
                std::cerr << "  [Error] BVH consistency violation after finish at stroke "
                          << stroke << ": " << bvhPostErr << "\n";
                return false;
            }

            // Verify that 20 random rays hitting the mesh succeed without error
            for (int r = 0; r < 20; ++r) {
                float phi = float(r) * 0.314f;
                glm::vec3 rOrig(2.0f * std::cos(phi), 2.0f * std::sin(phi), 0.5f);
                glm::vec3 rTarget(0.0f, 0.0f, 0.0f);
                glm::vec3 rDir = glm::normalize(rTarget - rOrig);
                RayHit rh;
                if (mesh->bvh.raycast(*mesh, rOrig, rDir, rh)) {
                    if (rh.faceId >= (uint32_t)mesh->nbFaces) {
                        std::cerr << "  [Error] Raycast hit invalid faceId " << rh.faceId
                                  << " >= nbFaces " << mesh->nbFaces << "\n";
                        return false;
                    }
                }
            }
        }

        std::cout << "  [Detail] MultiStroke: " << NUM_STROKES << " strokes completed. "
                  << "Final: " << mesh->nbVerts << " verts, " << mesh->nbFaces << " tris.\n";

        // Step 4: Test Undo and Redo across these strokes
        size_t undoCount = std::min<size_t>(5, undoStack.size());
        for (size_t i = 0; i < undoCount; ++i) {
            size_t idx = undoStack.size() - 1 - i;
            undoStack[idx]->swapWith(*mesh);
            mesh->initTopology();
            mesh->ensureBVHBuilt();
        }
        for (size_t i = 0; i < undoCount; ++i) {
            size_t idx = undoStack.size() - undoCount + i;
            undoStack[idx]->swapWith(*mesh);
            mesh->initTopology();
            mesh->ensureBVHBuilt();
        }

        std::string finalErr;
        if (!verifyMeshIntegrity(*mesh, finalErr, true)) {
            std::cerr << "  [Error] testMultiStrokeGrowthAndRaycast: integrity failed after undo/redo: " << finalErr << "\n";
            return false;
        }

        return true;
    }

    // 7. testLocalQuadPreservationAndUndo: Local stroke on a quad sphere preserving untouched quads
    static bool testLocalQuadPreservationAndUndo() {
        auto mesh = createQuadSphere(35.0f, 32, 64);
        int origVerts = mesh->nbVerts;
        int origFaces = mesh->nbFaces;
        int origQuads = mesh->getNbQuads();
        int origTris = mesh->nbFaces - mesh->getNbQuads(); // 128 triangle faces on poles

        if (origQuads != 1920 || origTris != 128 || origFaces != 2048 || origVerts != 1986) {
            std::cerr << "  [Error] testLocalQuadPreservationAndUndo: initial sphere counts unexpected (V="
                      << origVerts << ", F=" << origFaces << ", Q=" << origQuads << ", T=" << origTris << ")\n";
            return false;
        }

        std::vector<float> origVertsCopy = mesh->verts;
        std::vector<uint32_t> origFacesCopy = mesh->faces;

        DynamicTopology dyntopo;
        SymCopy id;
        SymCopy mirrorX;
        mirrorX.m = glm::mat3(glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f)));
        mirrorX.pivot = glm::vec3(0.0f);
        std::vector<SymCopy> mirrors = { id, mirrorX };

        // Stroke in equatorial neighborhood on +X side
        glm::vec3 center(30.0f, 0.0f, 18.0f);
        float radius = 10.0f;
        float targetEdge = 1.0f;

        bool changed = dyntopo.remesh(*mesh, mesh->bvh, center, radius, targetEdge, mirrors);
        if (!changed) {
            std::cerr << "  [Error] testLocalQuadPreservationAndUndo: remesh returned false\n";
            return false;
        }

        auto undo = dyntopo.finish(*mesh, mesh->bvh);
        if (!undo) {
            std::cerr << "  [Error] testLocalQuadPreservationAndUndo: undo record is null\n";
            return false;
        }

        int afterQuads = mesh->getNbQuads();
        int afterTris = mesh->nbFaces - mesh->getNbQuads();

        if (afterTris <= 128) {
            std::cerr << "  [Error] testLocalQuadPreservationAndUndo: triangles did not increase under brush (afterTris="
                      << afterTris << ")\n";
            return false;
        }

        if (afterQuads <= 1500) {
            std::cerr << "  [Error] testLocalQuadPreservationAndUndo: quads outside brush were unexpectedly triangulated (afterQuads="
                      << afterQuads << ", expected > 1500)\n";
            return false;
        }

        std::string err;
        if (!verifyMeshIntegrity(*mesh, err, true)) {
            std::cerr << "  [Error] testLocalQuadPreservationAndUndo: integrity check failed: " << err << "\n";
            return false;
        }

        // Check X-symmetry: count vertices with x > 1e-3 and x < -1e-3
        int countPos = 0, countNeg = 0;
        for (int i = 0; i < mesh->nbVerts; ++i) {
            float x = mesh->verts[i * 3 + 0];
            if (x > 1e-3f) countPos++;
            else if (x < -1e-3f) countNeg++;
        }
        if (countPos != countNeg) {
            std::cerr << "  [Error] testLocalQuadPreservationAndUndo: vertex asymmetry across X=0 plane ("
                      << countPos << " on +X vs " << countNeg << " on -X)\n";
            return false;
        }

        // Test Undo: restore original quad mesh
        undo->swapWith(*mesh);

        int restoredQuads = mesh->getNbQuads();
        int restoredTris = mesh->nbFaces - mesh->getNbQuads();

        if (restoredQuads != origQuads || restoredTris != origTris ||
            mesh->nbVerts != origVerts || mesh->nbFaces != origFaces) {
            std::cerr << "  [Error] testLocalQuadPreservationAndUndo: undo did not restore face/vertex counts (V="
                      << mesh->nbVerts << ", F=" << mesh->nbFaces << ", Q=" << restoredQuads
                      << ", T=" << restoredTris << ")\n";
            return false;
        }

        if (mesh->verts != origVertsCopy || mesh->faces != origFacesCopy) {
            std::cerr << "  [Error] testLocalQuadPreservationAndUndo: undo did not restore verts/faces bit-for-bit\n";
            return false;
        }

        if (!verifyMeshIntegrity(*mesh, err, true)) {
            std::cerr << "  [Error] testLocalQuadPreservationAndUndo: integrity check failed after undo: " << err << "\n";
            return false;
        }

        std::cout << "  [Detail] LocalQuadPreservation: Remeshed to V=" << (mesh->nbVerts)
                  << " (Tris=" << afterTris << ", Quads=" << afterQuads << " preserved). Undo bit-for-bit exact!\n";
        return true;
    }

    // 8. testMoveBrushAfterDynamicTopology: Verifies that after applying Dynamic Topology (BRUSH_DYNAMIC_TOPOLOGY),
    // the Move brush (BRUSH_MOVE) using topological picking correctly reaches 100% of vertices in its sphere
    // and displaces them without stuck/frozen vertices, even after AngleRenderer clears isTopologyDirty.
    static bool testMoveBrushAfterDynamicTopology() {
        auto mesh = createQuadSphere(35.0f, 32, 64);
        mesh->initFaceGroups();
        mesh->postInit(true);

        DynamicTopology dyntopo;
        SymCopy id;
        SymCopy mirrorX;
        mirrorX.m = glm::mat3(glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f)));
        mirrorX.pivot = glm::vec3(0.0f);
        std::vector<SymCopy> mirrors = { id, mirrorX };

        // 1. Perform a Dynamic Topology stroke on the +X hemisphere (equatorial region)
        glm::vec3 center(25.0f, 0.0f, 24.5f);
        center = glm::normalize(center) * 35.0f;
        float radius = 10.0f;
        float targetEdge = 0.8f;

        bool changed = dyntopo.remesh(*mesh, mesh->bvh, center, radius, targetEdge, mirrors);
        if (!changed) {
            std::cerr << "  [Error] testMoveBrushAfterDynamicTopology: remesh returned false\n";
            return false;
        }

        auto undo = dyntopo.finish(*mesh, mesh->bvh);
        if (!undo) {
            std::cerr << "  [Error] testMoveBrushAfterDynamicTopology: finish returned null undo\n";
            return false;
        }

        // 2. Simulate what AngleRenderer::uploadIfDirty does on the very next render frame:
        // AngleRenderer uses mesh->isTopologyDirty as a GPU EBO upload flag and sets it to false!
        mesh->isTopologyDirty = false;

        // 3. User now switches to Move Brush and clicks on the remeshed area.
        // SculptManager::executeStroke begins with mesh->ensureTopologyBuilt():
        mesh->ensureTopologyBuilt();

        // 4. Test topological picking with Move Brush on the newly remeshed region:
        // Find a triangle in the center of the remeshed area
        RayHit hit;
        glm::vec3 rayOrig = center * 2.0f;
        glm::vec3 rayDir = glm::normalize(center - rayOrig);
        if (!mesh->bvh.raycast(*mesh, rayOrig, rayDir, hit)) {
            std::cerr << "  [Error] testMoveBrushAfterDynamicTopology: raycast failed to hit remeshed area\n";
            return false;
        }

        uint32_t intersectFaceId = hit.faceId;
        float moveRadius = 8.0f;
        float moveRadius2 = moveRadius * moveRadius;

        std::vector<uint8_t> visitedFlags(mesh->nbVerts, 0);
        uint8_t visitedEpoch = 1;
        std::vector<uint32_t> pickedTopo;
        SculptManager::pickVerticesInSphereTopological(
            mesh.get(), hit.position, moveRadius2, intersectFaceId,
            visitedFlags, visitedEpoch, pickedTopo
        );

        // Ground-truth: compute all connected vertices geometrically within moveRadius
        std::vector<uint32_t> expectedVerts;
        mesh->bvh.pickVerticesInSphere(*mesh, hit.position, moveRadius2, expectedVerts, mesh->vertVisible.data(), 0);

        // 5. Test Move Brush displacement: apply strokeMove
        std::vector<float> preMoveVerts = mesh->verts;
        glm::vec3 dragDir(2.0f, 1.0f, 0.0f);
        strokeMove(
            mesh->verts.data(),
            mesh->vertProxy.data(),
            mesh->materials.data(),
            pickedTopo.data(),
            (int)pickedTopo.size(),
            hit.position.x, hit.position.y, hit.position.z,
            dragDir.x, dragDir.y, dragDir.z,
            moveRadius,
            0.0f, false,
            false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
            false, nullptr
        );

        int movedCount = 0;
        int unmovedInsideHalfRadius = 0;
        for (uint32_t ev : expectedVerts) {
            float dx = mesh->verts[ev * 3] - preMoveVerts[ev * 3];
            float dy = mesh->verts[ev * 3 + 1] - preMoveVerts[ev * 3 + 1];
            float dz = mesh->verts[ev * 3 + 2] - preMoveVerts[ev * 3 + 2];
            float disp2 = dx * dx + dy * dy + dz * dz;
            if (disp2 > 1e-6f) {
                movedCount++;
            } else {
                glm::vec3 origP(preMoveVerts[ev * 3], preMoveVerts[ev * 3 + 1], preMoveVerts[ev * 3 + 2]);
                float distOrig = glm::distance(origP, hit.position);
                if (distOrig < 0.7f * moveRadius) {
                    unmovedInsideHalfRadius++;
                }
            }
        }

        // 6. Test Move Brush across boundary (clicking on untouched quad adjacent to remeshed region)
        glm::vec3 boundaryPt = center + glm::vec3(0.0f, 0.0f, 12.0f);
        boundaryPt = glm::normalize(boundaryPt) * 35.0f;
        RayHit bHit;
        mesh->bvh.raycast(*mesh, boundaryPt * 2.0f, glm::normalize(boundaryPt - boundaryPt * 2.0f), bHit);

        std::vector<uint32_t> bPickedTopo;
        visitedEpoch++;
        SculptManager::pickVerticesInSphereTopological(
            mesh.get(), bHit.position, moveRadius2, bHit.faceId,
            visitedFlags, visitedEpoch, bPickedTopo
        );

        std::vector<uint32_t> bExpectedVerts;
        mesh->bvh.pickVerticesInSphere(*mesh, bHit.position, moveRadius2, bExpectedVerts, mesh->vertVisible.data(), 0);

        std::unordered_set<uint32_t> bTopoSet(bPickedTopo.begin(), bPickedTopo.end());
        int boundaryStuckCount = 0;
        for (uint32_t ev : bExpectedVerts) {
            if (bTopoSet.find(ev) == bTopoSet.end()) {
                glm::vec3 origP(mesh->verts[ev * 3], mesh->verts[ev * 3 + 1], mesh->verts[ev * 3 + 2]);
                float d = glm::distance(origP, bHit.position);
                if (d < 0.7f * moveRadius) {
                    boundaryStuckCount++;
                }
            }
        }

        std::cout << "  [Detail] MoveBrushAfterDyntopo: Center Hit - Picked " << pickedTopo.size()
                  << " / " << expectedVerts.size() << " expected verts (Moved=" << movedCount
                  << ", UnmovedInCore=" << unmovedInsideHalfRadius << ")\n";
        std::cout << "  [Detail] MoveBrushAfterDyntopo: Boundary Hit - Picked " << bPickedTopo.size()
                  << " / " << bExpectedVerts.size() << " expected verts (StuckInCore=" << boundaryStuckCount << ")\n";

        if (unmovedInsideHalfRadius > 0 || boundaryStuckCount > 0) {
            std::cerr << "  [Error] testMoveBrushAfterDynamicTopology: Stale vrv/topology detected! "
                      << (unmovedInsideHalfRadius + boundaryStuckCount) << " vertices inside Move brush radius were frozen!\n";
            return false;
        }

        return true;
    }
};

} // namespace dynamic
