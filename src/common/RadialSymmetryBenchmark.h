#pragma once

#include "common/Symmetry.h"
#include "common/OptFlags.h"
#include "common/Enums.h"
#include "mesh/Mesh.h"
#include "mesh/BVH.h"
#include "mesh/dynamic/DynamicTopology.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "editing/SculptManager.h"
#include "editing/BrushCursor.h"
#include "editing/undo/UndoManager.h"

#include <SDL2/SDL.h>
#include <glm/glm.hpp>
#include <omp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_set>
#include <vector>

namespace RadialSymmetryBenchmark {

struct MeshSnapshot {
    std::vector<float> verts;
    std::vector<float> normals;
    std::vector<float> faceNormals;
    std::vector<float> faceBoxes;
    std::vector<float> faceCenters;
};

inline std::unique_ptr<Mesh> createUVSphere(float radius, int rings, int sectors) {
    auto mesh = std::make_unique<Mesh>();
    mesh->verts.reserve((size_t)(rings + 1) * (sectors + 1) * 3);
    mesh->faces.reserve((size_t)rings * sectors * 4);

    for (int r = 0; r <= rings; ++r) {
        float phi = float(M_PI) * float(r) / float(rings);
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);
        for (int s = 0; s <= sectors; ++s) {
            float theta = 2.0f * float(M_PI) * float(s) / float(sectors);
            mesh->verts.push_back(radius * sinPhi * std::cos(theta));
            mesh->verts.push_back(radius * cosPhi);
            mesh->verts.push_back(radius * sinPhi * std::sin(theta));
            mesh->nbVerts++;
        }
    }

    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < sectors; ++s) {
            uint32_t v0 = (uint32_t)(r * (sectors + 1) + s);
            uint32_t v1 = v0 + 1;
            uint32_t v2 = (uint32_t)((r + 1) * (sectors + 1) + s + 1);
            uint32_t v3 = (uint32_t)((r + 1) * (sectors + 1) + s);
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

inline MeshSnapshot captureSnapshot(const Mesh& mesh) {
    MeshSnapshot snap;
    snap.verts = mesh.verts;
    snap.normals = mesh.normals;
    snap.faceNormals = mesh.faceNormals;
    snap.faceBoxes = mesh.faceBoxes;
    snap.faceCenters = mesh.faceCenters;
    return snap;
}

inline void restoreSnapshot(Mesh& mesh, const MeshSnapshot& snap) {
    mesh.verts = snap.verts;
    mesh.vertProxy = snap.verts;
    mesh.normals = snap.normals;
    mesh.faceNormals = snap.faceNormals;
    mesh.faceBoxes = snap.faceBoxes;
    mesh.faceCenters = snap.faceCenters;
    mesh.bvh.build(mesh);
    mesh.clearDirtyBlockMasks();
    mesh.isVertexDirty = false;
    mesh.isDirty = false;
}

inline SDL_Event makeMouseDown(float x, float y) {
    SDL_Event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = (int)std::round(x);
    ev.button.y = (int)std::round(y);
    return ev;
}

inline SDL_Event makeMouseMove(float x, float y, bool pressed = true) {
    SDL_Event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_MOUSEMOTION;
    ev.motion.state = pressed ? SDL_BUTTON_LMASK : 0;
    ev.motion.x = (int)std::round(x);
    ev.motion.y = (int)std::round(y);
    return ev;
}

inline SDL_Event makeMouseUp(float x, float y) {
    SDL_Event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_MOUSEBUTTONUP;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = (int)std::round(x);
    ev.button.y = (int)std::round(y);
    return ev;
}

struct StrokeBenchConfig {
    std::string label;
    bool useSym = false;
    bool mirrorX = false;
    bool radialEnabled = false;
    int radialCount = 2;
};

struct StrokeBenchResult {
    double step0WallMs = 0.0;
    double step0CpuMs = 0.0;
    double drag10WallMs = 0.0;
    double drag10AvgMs = 0.0;
    double fastSwipeWallMs = 0.0;
    SculptFrameProfile step0Prof;
    double accumSymMs = 0.0;
    double accumSymPickMs = 0.0;
    double accumSymDeformMs = 0.0;
    double accumSymUndoMs = 0.0;
    double accumUndoMs = 0.0;
    double accumSortDedupMs = 0.0;
    double accumPostMs = 0.0;
};

inline StrokeBenchResult measureBrushScenario(
    Scene& scene,
    Mesh* mesh,
    const MeshSnapshot& snap,
    BrushType brush,
    float brushRadius,
    const StrokeBenchConfig& cfg,
    int dragSteps = 10,
    bool doFastSwipe = true
) {
    restoreSnapshot(*mesh, snap);
    g_undoManager.clear();

    SculptManager sculpt;
    sculpt.setRenderer(nullptr);
    sculpt.setBrush(brush);
    auto& st = sculpt.getSettings(brush);
    st.radius = brushRadius;
    st.intensity = 0.5f;
    st.spacing = 0.05f;
    st.culling = false;

    sculpt.setUseSym(cfg.useSym);
    sculpt.setSymX(cfg.mirrorX);
    sculpt.setSymY(false);
    sculpt.setSymZ(false);
    sculpt.setSymRadialEnabled(cfg.radialEnabled);
    sculpt.setSymRadialCount(cfg.radialCount);
    sculpt.setSymRadialAxis(1);
    sculpt.setSymmetryMode(SymmetryMode::Local);
    sculpt.warmupMeshCaches(mesh);

    const Camera& cam = scene.getCamera();
    const glm::vec3 s0 = cam.project(glm::vec3(0.0f, 5.0f, 35.0f));

    StrokeBenchResult res;

    // 1) Step 0 + N gradual drag steps (3px X + 1px Y = 3.16px -> exactly 1 substep per motion event)
    const bool isGrab = (brush == BRUSH_MOVE || brush == BRUSH_SNAKEHOOK);
    {
        auto evDown = makeMouseDown(s0.x, s0.y);
        auto t0 = std::chrono::high_resolution_clock::now();
        sculpt.handleEvent(evDown, scene);
        auto t1 = std::chrono::high_resolution_clock::now();
        res.step0WallMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        res.step0Prof = sculpt.getLastFrameProfile();
        res.step0CpuMs = res.step0Prof.getTotalMs();

        double dragSum = 0.0;
        for (int i = 1; i <= dragSteps; ++i) {
            float sx = std::round(s0.x) + float(i * 3);
            float sy = std::round(s0.y) + float(i * 1);
            auto evMove = makeMouseMove(sx, sy, true);
            auto m0 = std::chrono::high_resolution_clock::now();
            sculpt.handleEvent(evMove, scene);
            if (isGrab) {
                sculpt.processFrame(scene);
            }
            auto m1 = std::chrono::high_resolution_clock::now();
            dragSum += std::chrono::duration<double, std::milli>(m1 - m0).count();

            const SculptFrameProfile& p = sculpt.getLastFrameProfile();
            res.accumSymMs += p.symmetryMs;
            res.accumSymPickMs += p.symPickMs;
            res.accumSymDeformMs += p.symDeformMs;
            res.accumSymUndoMs += p.symUndoMs;
            res.accumUndoMs += p.undoRecordMs;
            res.accumSortDedupMs += p.sortDedupMs;
            res.accumPostMs += (p.faceLookupMs + p.faceNormalsMs + p.vertNormalsMs + p.bvhUpdateMs);
        }
        auto evUp = makeMouseUp(std::round(s0.x) + float(dragSteps * 3), std::round(s0.y) + float(dragSteps * 1));
        sculpt.handleEvent(evUp, scene);

        res.drag10WallMs = dragSum;
        res.drag10AvgMs = (dragSteps > 0) ? (dragSum / dragSteps) : 0.0;
    }

    // 2) Fast mouse swipe (60px single SDL_MOUSEMOTION -> up to 16 substeps)
    if (doFastSwipe) {
        restoreSnapshot(*mesh, snap);
        g_undoManager.clear();
        sculpt.warmupMeshCaches(mesh);

        auto evDown = makeMouseDown(s0.x, s0.y);
        sculpt.handleEvent(evDown, scene);

        auto evSwipe = makeMouseMove(s0.x + 55.0f, s0.y + 15.0f, true);
        auto sw0 = std::chrono::high_resolution_clock::now();
        sculpt.handleEvent(evSwipe, scene);
        auto sw1 = std::chrono::high_resolution_clock::now();
        res.fastSwipeWallMs = std::chrono::duration<double, std::milli>(sw1 - sw0).count();

        auto evUp = makeMouseUp(s0.x + 55.0f, s0.y + 15.0f);
        sculpt.handleEvent(evUp, scene);
    }

    g_undoManager.clear();
    return res;
}

inline void runRadialCountScalingBench(Scene& scene, Mesh* mesh, const MeshSnapshot& snap) {
    std::cout << "--------------------------------------------------------------------------------\n";
    std::cout << "[1] RADIAL COUNT SCALING (" << mesh->nbVerts << " verts, ClayBuildup, Radius=50px)\n";
    std::cout << "    (Drag10 = 10 x 3.16px events [1 substep/event] | FastSwipe = 1 x 57px event -> 16 substeps)\n";
    std::cout << "--------------------------------------------------------------------------------\n";
    std::cout << std::left << std::setw(18) << "Config"
              << std::right
              << std::setw(9) << "Step0"
              << std::setw(9) << "Drag10"
              << std::setw(9) << "AvgStep"
              << std::setw(11) << "FastSwipe"
              << std::setw(9) << "Picked"
              << std::setw(10) << "Affected"
              << " | " << "SymTotal (Pick/Def/Undo) | SortDed | PostBVH\n";

    std::vector<StrokeBenchConfig> configs = {
        { "Sym OFF (1x)",      false, false, false, 2  },
        { "Mirror X (2x)",     true,  true,  false, 2  },
        { "Radial 4 (4x)",     true,  false, true,  4  },
        { "Radial 8 (8x)",     true,  false, true,  8  },
        { "Radial 16 (16x)",   true,  false, true,  16 },
        { "Radial 32 (32x)",   true,  false, true,  32 },
        { "Radial 64 (64x)",   true,  false, true,  64 },
        { "MirrorX+Rad32(64x)",true,  true,  true,  32 }
    };

    double baselineDragAvg = 0.0;
    std::vector<StrokeBenchResult> results;
    results.reserve(configs.size());

    for (size_t idx = 0; idx < configs.size(); ++idx) {
        auto r = measureBrushScenario(scene, mesh, snap, BRUSH_CLAYBUILDUP, 50.0f, configs[idx], 10, true);
        if (idx == 0) baselineDragAvg = std::max(r.drag10AvgMs, 0.001);
        const double postStep0 = r.step0Prof.faceLookupMs + r.step0Prof.faceNormalsMs +
                                 r.step0Prof.vertNormalsMs + r.step0Prof.bvhUpdateMs;
        std::cout << std::fixed << std::setprecision(2)
                  << std::left << std::setw(18) << configs[idx].label
                  << std::right
                  << std::setw(8) << r.step0WallMs << "m"
                  << std::setw(8) << r.drag10WallMs << "m"
                  << std::setw(8) << r.drag10AvgMs << "m"
                  << std::setw(10) << r.fastSwipeWallMs << "m"
                  << std::setw(9) << r.step0Prof.pickedVertCount
                  << std::setw(10) << r.step0Prof.affectedVertCount
                  << " | " << std::setw(6) << r.step0Prof.symmetryMs << "m ("
                  << std::setw(5) << r.step0Prof.symPickMs << "/"
                  << std::setw(5) << r.step0Prof.symDeformMs << "/"
                  << std::setw(5) << r.step0Prof.symUndoMs << ")"
                  << " | " << std::setw(6) << r.step0Prof.sortDedupMs << "m"
                  << " | " << std::setw(6) << postStep0 << "m"
                  << "  [" << std::setprecision(1) << (r.drag10AvgMs / baselineDragAvg) << "x]\n";
        results.push_back(r);
    }

    // Print cumulative breakdown for Drag10 on key configs (1 substep per drag event)
    std::cout << "\n  [Drag10 Stage Breakdown across 10 substeps (excluding Step0)]\n";
    for (size_t idx : { 0u, 3u, 5u, 6u }) {
        if (idx >= results.size()) continue;
        const auto& r = results[idx];
        double primaryUndo = std::max(0.0, r.accumUndoMs - r.accumSymUndoMs);
        double primaryPickDef = std::max(0.0, r.drag10WallMs - r.accumSymMs - r.accumSortDedupMs - r.accumPostMs - primaryUndo);
        std::cout << std::fixed << std::setprecision(2)
                  << "    " << std::left << std::setw(18) << configs[idx].label
                  << " Total=" << std::setw(7) << r.drag10WallMs << "ms"
                  << " | Prim(Pick+Def+Ray)=" << std::setw(5) << primaryPickDef << "m"
                  << " PrimUndo=" << std::setw(5) << primaryUndo << "m"
                  << " | SymTotal=" << std::setw(7) << r.accumSymMs << "ms"
                  << " (Pick=" << std::setw(5) << r.accumSymPickMs
                  << ", Def=" << std::setw(6) << r.accumSymDeformMs
                  << ", Undo=" << std::setw(6) << r.accumSymUndoMs << ")"
                  << " | SortDed=" << std::setw(5) << r.accumSortDedupMs << "m"
                  << " | Post+BVH=" << std::setw(5) << r.accumPostMs << "m\n";
    }
    std::cout << "\n";
}

inline void runLargeRadiusAndMeshDensityBench() {
    std::cout << "--------------------------------------------------------------------------------\n";
    std::cout << "[2] MESH DENSITY & BRUSH RADIUS SCALING (ClayBuildup: Sym OFF vs Rad 16 vs Rad 32)\n";
    std::cout << "--------------------------------------------------------------------------------\n";

    struct DensityCase { int rings; int sectors; float radius; const char* name; };
    std::vector<DensityCase> cases = {
        { 500,  500,  50.0f, "250K verts (R=50)" },
        { 1000, 1000, 50.0f, "1.0M verts (R=50)" },
        { 1000, 1000, 120.0f,"1.0M verts (R=120)"},
        { 1500, 1500, 50.0f, "2.25M verts(R=50)" }
    };

    for (const auto& dc : cases) {
        auto ptr = createUVSphere(35.0f, dc.rings, dc.sectors);
        Mesh* mesh = ptr.release();
        mesh->postInit(true);
        MeshSnapshot snap = captureSnapshot(*mesh);

        Scene scene;
        scene.addMesh(mesh);
        scene.selectMesh(mesh);
        Camera& cam = scene.getCamera();
        cam.onResize(1280, 720);
        cam.resetViewToMesh(mesh, false);
        cam.update(0.016f);

        StrokeBenchConfig c1{ "SymOFF", false, false, false, 2 };
        StrokeBenchConfig c16{ "Rad16", true, false, true, 16 };
        StrokeBenchConfig c32{ "Rad32", true, false, true, 32 };

        auto r1 = measureBrushScenario(scene, mesh, snap, BRUSH_CLAYBUILDUP, dc.radius, c1, 6, false);
        auto r16 = measureBrushScenario(scene, mesh, snap, BRUSH_CLAYBUILDUP, dc.radius, c16, 6, false);
        auto r32 = measureBrushScenario(scene, mesh, snap, BRUSH_CLAYBUILDUP, dc.radius, c32, 6, false);

        std::cout << std::fixed << std::setprecision(2)
                  << "  " << std::left << std::setw(18) << dc.name
                  << " (" << std::setw(7) << mesh->nbVerts << "v)"
                  << " | SymOFF: S0=" << std::setw(5) << r1.step0WallMs << "m AvgDrag=" << std::setw(5) << r1.drag10AvgMs << "m (Pick=" << r1.step0Prof.pickedVertCount << ")"
                  << " | Rad16: S0=" << std::setw(6) << r16.step0WallMs << "m AvgDrag=" << std::setw(6) << r16.drag10AvgMs << "m (SymUndo=" << r16.step0Prof.symUndoMs << "m)"
                  << " | Rad32: S0=" << std::setw(6) << r32.step0WallMs << "m AvgDrag=" << std::setw(6) << r32.drag10AvgMs << "m (SymUndo=" << r32.step0Prof.symUndoMs << "m, SymDef=" << r32.step0Prof.symDeformMs << "m)\n";
    }
    std::cout << "\n";
}

inline void runBrushComparisonBench(Scene& scene, Mesh* mesh, const MeshSnapshot& snap) {
    std::cout << "--------------------------------------------------------------------------------\n";
    std::cout << "[3] BRUSH COMPARISON (" << mesh->nbVerts << " verts, Radius=50, Sym OFF vs Radial 32)\n";
    std::cout << "--------------------------------------------------------------------------------\n";

    struct BrushItem { BrushType type; const char* name; };
    std::vector<BrushItem> brushes = {
        { BRUSH_CLAYBUILDUP, "ClayBuildup" },
        { BRUSH_FLATTEN,     "Flatten" },
        { BRUSH_BRUSH,       "Brush (Standard)" },
        { BRUSH_SQUAREBRUSH, "SquareBrush" },
        { BRUSH_SMOOTH,      "Smooth" },
        { BRUSH_MOVE,        "Move (Grab)" },
        { BRUSH_SNAKEHOOK,   "SnakeHook" }
    };

    StrokeBenchConfig cOff{ "SymOFF", false, false, false, 2 };
    StrokeBenchConfig cR32{ "Rad32",  true,  false, true,  32 };

    std::cout << std::left << std::setw(18) << "Brush"
              << " | " << std::setw(19) << "SymOFF (Step0/Avg)"
              << " | " << std::setw(21) << "Rad32 (Step0/Avg)"
              << " | Slowdown | Rad32 Step0 SymPick/SymDef/SymUndo/SortDed\n";

    for (const auto& b : brushes) {
        auto r1 = measureBrushScenario(scene, mesh, snap, b.type, 50.0f, cOff, 6, false);
        auto r32 = measureBrushScenario(scene, mesh, snap, b.type, 50.0f, cR32, 6, false);
        double ratio = r32.step0WallMs / std::max(r1.step0WallMs, 0.001);
        std::cout << std::fixed << std::setprecision(2)
                  << std::left << std::setw(18) << b.name
                  << " | " << std::right << std::setw(7) << r1.step0WallMs << "m / " << std::setw(6) << r1.drag10AvgMs << "m"
                  << "  | " << std::setw(7) << r32.step0WallMs << "m / " << std::setw(7) << r32.drag10AvgMs << "m"
                  << "  | " << std::setw(6) << std::setprecision(1) << ratio << "x  | "
                  << std::setprecision(2)
                  << r32.step0Prof.symPickMs << " / "
                  << r32.step0Prof.symDeformMs << " / "
                  << r32.step0Prof.symUndoMs << " / "
                  << r32.step0Prof.sortDedupMs << " ms\n";
    }

    // Also test Dynamic Topology + Radial Symmetry
    std::cout << "\n  [Dynamic Topology + Radial Symmetry (90K mesh, ClayBuildup + Dyntopo ON)]\n";
    auto dyPtr = createUVSphere(35.0f, 300, 300);
    Mesh* dyMesh = dyPtr.release();
    dyMesh->postInit(true);
    MeshSnapshot dySnap = captureSnapshot(*dyMesh);

    Scene dyScene;
    dyScene.addMesh(dyMesh);
    dyScene.selectMesh(dyMesh);
    Camera& dyCam = dyScene.getCamera();
    dyCam.onResize(1280, 720);
    dyCam.resetViewToMesh(dyMesh, false);
    dyCam.update(0.016f);

    std::vector<StrokeBenchConfig> dyConfigs = {
        { "Dyn SymOFF (1x)",   false, false, false, 2  },
        { "Dyn MirrorX (2x)",  true,  true,  false, 2  },
        { "Dyn Radial 8 (8x)", true,  false, true,  8  },
        { "Dyn Radial 16",     true,  false, true,  16 },
        { "Dyn Radial 32",     true,  false, true,  32 }
    };

    const glm::vec3 dyS0 = dyCam.project(glm::vec3(0.0f, 5.0f, 35.0f));
    for (const auto& dc : dyConfigs) {
        restoreSnapshot(*dyMesh, dySnap);
        g_undoManager.clear();

        SculptManager dySculpt;
        dySculpt.setRenderer(nullptr);
        dySculpt.setBrush(BRUSH_CLAYBUILDUP);
        dySculpt.getSettings(BRUSH_CLAYBUILDUP).radius = 50.0f;
        dySculpt.getSettings(BRUSH_CLAYBUILDUP).intensity = 0.5f;
        dySculpt.setDyntopoEnabled(true);
        dySculpt.getDyntopoSettings().enabled = true;
        dySculpt.getDyntopoSettings().detail = 0.5f;
        dySculpt.setUseSym(dc.useSym);
        dySculpt.setSymX(dc.mirrorX);
        dySculpt.setSymRadialEnabled(dc.radialEnabled);
        dySculpt.setSymRadialCount(dc.radialCount);
        dySculpt.setSymRadialAxis(1);
        dySculpt.warmupMeshCaches(dyMesh);

        auto evD = makeMouseDown(dyS0.x, dyS0.y);
        auto t0 = std::chrono::high_resolution_clock::now();
        dySculpt.handleEvent(evD, dyScene);
        auto t1 = std::chrono::high_resolution_clock::now();

        auto evM = makeMouseMove(dyS0.x + 20.0f, dyS0.y + 8.0f, true);
        auto t2 = std::chrono::high_resolution_clock::now();
        dySculpt.handleEvent(evM, dyScene);
        auto t3 = std::chrono::high_resolution_clock::now();

        auto evU = makeMouseUp(dyS0.x + 20.0f, dyS0.y + 8.0f);
        dySculpt.handleEvent(evU, dyScene);

        double s0ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double mms = std::chrono::duration<double, std::milli>(t3 - t2).count();
        std::cout << std::fixed << std::setprecision(2)
                  << "    " << std::left << std::setw(18) << dc.label
                  << " | Step0=" << std::setw(7) << s0ms << "ms"
                  << " | Move=" << std::setw(7) << mms << "ms"
                  << " | FinalVerts=" << dyMesh->nbVerts << "\n";
    }
    g_undoManager.clear();
    std::cout << "\n";
}

inline void runMicroBenchmarks(Scene& scene, Mesh* mesh, const MeshSnapshot& snap) {
    restoreSnapshot(*mesh, snap);

    std::cout << "--------------------------------------------------------------------------------\n";
    std::cout << "[4] ISOLATED BOTTLENECK MICRO-BENCHMARKS (" << mesh->nbVerts << " verts)\n";
    std::cout << "--------------------------------------------------------------------------------\n";

    // 4a. UndoManager::recordAffectedVertices: 1 large batch vs N small per-copy calls + drag growth
    {
        std::cout << "  [4a] UndoManager::recordAffectedVertices() - Batched vs Per-Copy calls (3 rounds of 10-step growth):\n";
        std::cout << "       (Exposes 3x OMP barriers/copy + exact-fit std::vector::resize() realloc/memcpy)\n";

        // Build 64 disjoint vertex slices (~2048 verts each -> ~131K unique verts total) to cross the >=2048 OMP threshold
        constexpr int NUM_SLICES_64 = 64;
        constexpr size_t SLICE_SIZE_64 = 2048;
        std::vector<std::vector<uint32_t>> slices64(NUM_SLICES_64);
        std::vector<std::vector<uint32_t>> slices32(32);
        std::vector<uint32_t> batched128k;
        const size_t totalVertsNeeded = (size_t)NUM_SLICES_64 * SLICE_SIZE_64;
        batched128k.reserve(totalVertsNeeded);

        uint32_t cursor = 0;
        for (int s = 0; s < NUM_SLICES_64; ++s) {
            slices64[s].reserve(SLICE_SIZE_64);
            for (size_t k = 0; k < SLICE_SIZE_64; ++k) {
                uint32_t v = (cursor++) % (uint32_t)mesh->nbVerts;
                slices64[s].push_back(v);
                batched128k.push_back(v);
            }
        }
        for (int s = 0; s < 32; ++s) {
            slices32[s].reserve(SLICE_SIZE_64 * 2);
            slices32[s].insert(slices32[s].end(), slices64[s * 2].begin(), slices64[s * 2].end());
            slices32[s].insert(slices32[s].end(), slices64[s * 2 + 1].begin(), slices64[s * 2 + 1].end());
        }

        // Warmup stamp table
        g_undoManager.clear();
        g_undoManager.warmupForMesh(mesh->m_id, mesh->nbVerts, mesh->nbFaces);

        auto benchUndoPattern = [&](const char* label, const std::vector<std::vector<uint32_t>>& batches, int dragDabs) {
            double firstDabMs = 0.0;
            double dragMs = 0.0;

            g_undoManager.clear();
            g_undoManager.warmupForMesh(mesh->m_id, mesh->nbVerts, mesh->nbFaces);

            std::vector<uint32_t> emptySeed;
            g_undoManager.beginSculptStroke(scene, mesh->m_id, emptySeed, true, false, false, "UndoBench");

            // Dab 0 (new vertices across all slices)
            {
                auto t0 = std::chrono::high_resolution_clock::now();
                for (const auto& b : batches) {
                    g_undoManager.recordAffectedVertices(scene, mesh->m_id, b, true, false, false);
                }
                auto t1 = std::chrono::high_resolution_clock::now();
                firstDabMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            }

            // Subsequent dabs shifted by stride offset so delta grows each substep (simulating real drag)
            std::vector<std::vector<uint32_t>> shifted = batches;
            for (int d = 1; d <= dragDabs; ++d) {
                for (auto& b : shifted) {
                    for (uint32_t& v : b) {
                        v = (v + 3000) % (uint32_t)mesh->nbVerts;
                    }
                }
                auto t0 = std::chrono::high_resolution_clock::now();
                for (const auto& b : shifted) {
                    g_undoManager.recordAffectedVertices(scene, mesh->m_id, b, true, false, false);
                }
                auto t1 = std::chrono::high_resolution_clock::now();
                dragMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
            }

            g_undoManager.endSculptStroke(scene);
            g_undoManager.clear();

            std::cout << std::fixed << std::setprecision(2)
                      << "       " << std::left << std::setw(28) << label
                      << " | Dab0=" << std::setw(6) << firstDabMs << "ms"
                      << " | Drag(" << dragDabs << " dabs)=" << std::setw(7) << dragMs << "ms"
                      << " (avg=" << std::setw(5) << (dragMs / dragDabs) << "ms/dab)\n";
        };

        std::vector<std::vector<uint32_t>> oneBatch = { batched128k };
        benchUndoPattern("1 call  x 131,072 verts", oneBatch, 8);
        benchUndoPattern("32 calls x   4,096 verts", slices32, 8);
        benchUndoPattern("64 calls x   2,048 verts", slices64, 8);
    }

    // 4b. BVH::pickVerticesInSphere scaling & m_lastQueriedLeaves duplication
    {
        std::cout << "\n  [4b] BVH::pickVerticesInSphere() - 1 primary call vs 31 sequential radial copy calls:\n";
        SymmetryParams sp;
        sp.useSym = true;
        sp.radialEnabled = true;
        sp.radialCount = 32;
        sp.radialAxis = 1;
        auto sec = buildSymmetryCopies(sp, mesh, false);

        glm::vec3 c0(0.0f, 5.0f, 35.0f);
        float r2 = 2.0f * 2.0f; // approx world radius matching 50px screen brush
        {
            const Camera& cam = scene.getCamera();
            float fov = cam.getFovDegrees() * float(M_PI) / 180.0f;
            float dist = glm::distance(cam.computePosition(), c0);
            float wr = 50.0f * dist * std::tan(fov * 0.5f) * 2.0f / float(cam.getHeight());
            r2 = wr * wr;
        }

        std::vector<uint32_t> tmp;
        mesh->bvh.clearLastQueriedLeaves();
        auto t0 = std::chrono::high_resolution_clock::now();
        mesh->bvh.pickVerticesInSphere(*mesh, c0, r2, tmp, mesh->vertVisible.data(), 0);
        auto t1 = std::chrono::high_resolution_clock::now();
        size_t leavesPrimary = mesh->bvh.getLastQueriedLeaves().size();
        const BVHPickStats st0 = mesh->bvh.getLastPickStats(0);

        auto t2 = std::chrono::high_resolution_clock::now();
        size_t totalPickedSym = 0;
        for (const auto& sc : sec) {
            glm::vec3 cs = sc.point(c0);
            mesh->bvh.pickVerticesInSphere(*mesh, cs, r2, tmp, mesh->vertVisible.data(), 1);
            totalPickedSym += tmp.size();
        }
        auto t3 = std::chrono::high_resolution_clock::now();
        size_t leavesAfterAll = mesh->bvh.getLastQueriedLeaves().size();

        double ms1 = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double ms31 = std::chrono::duration<double, std::milli>(t3 - t2).count();
        std::cout << std::fixed << std::setprecision(2)
                  << "       1 primary pick : " << ms1 << " ms | candFaces=" << st0.candFaces
                  << " usedMT=" << (st0.usedMT ? 1 : 0) << " | queriedLeaves=" << leavesPrimary << "\n"
                  << "       31 sym picks   : " << ms31 << " ms (" << (ms31 / 31.0) << " ms/copy)"
                  << " | totalVerts=" << totalPickedSym << " | totalQueriedLeaves=" << leavesAfterAll << "\n";
    }

    // 4c. BrushCursor::update Occlusion Raycast Overhead across 50 cursor moves (>2px)
    {
        std::cout << "\n  [4c] BrushCursor::update() Occlusion Raycast Cost (50 cursor moves >2px):\n";
        BrushCursor cursor;
        const Camera& cam = scene.getCamera();
        const glm::vec3 s0 = cam.project(glm::vec3(0.0f, 5.0f, 35.0f));

        auto testCursor = [&](const char* label, bool useSym, bool mirrorX, bool radEn, int radCount, bool splitRight) {
            if (splitRight) {
                scene.setSplitMode(Scene::SplitMode::INDEPENDENT);
                if (auto cr = scene.getCameraRight()) {
                    *cr = cam;
                }
            } else {
                scene.setSplitMode(Scene::SplitMode::OFF);
            }

            SymmetryParams sp;
            sp.useSym = useSym;
            sp.symX = mirrorX;
            sp.radialEnabled = radEn;
            sp.radialCount = radCount;
            sp.radialAxis = 1;
            auto copies = useSym ? buildSymmetryCopies(sp, mesh, false) : std::vector<SymCopy>{};

            cursor.resetOcclusionQueryCount();
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int m = 0; m < 50; ++m) {
                int mx = (int)std::round(s0.x + float((m % 10) * 4));
                int my = (int)std::round(s0.y + float((m / 10) * 4));
                cursor.update(mx, my, scene, 50.0f, copies, false, BRUSH_CLAYBUILDUP, false);
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            std::cout << std::fixed << std::setprecision(2)
                      << "       " << std::left << std::setw(24) << label
                      << " | Total50=" << std::setw(6) << totalMs << "ms"
                      << " | Avg/move=" << std::setw(5) << (totalMs / 50.0) << "ms"
                      << " | BVH OccRays=" << cursor.getOcclusionQueryCount() << "\n";
        };

        testCursor("Sym OFF (1 view)",        false, false, false, 2,  false);
        testCursor("Mirror X (1 view)",       true,  true,  false, 2,  false);
        testCursor("Radial 8 (1 view)",       true,  false, true,  8,  false);
        testCursor("Radial 32 (1 view)",      true,  false, true,  32, false);
        testCursor("Radial 64 (1 view)",      true,  false, true,  64, false);
        testCursor("Radial 32 (Split 2x)",    true,  false, true,  32, true);

        scene.setSplitMode(Scene::SplitMode::OFF);
    }

    std::cout << "\n";
}

inline bool run() {
    std::cout << "\n================================================================================\n";
    std::cout << "             SPSCULPT RADIAL SYMMETRY PERFORMANCE BENCHMARK\n";
    std::cout << "================================================================================\n";
    std::cout << "OpenMP Max Threads : " << omp_get_max_threads() << "\n";
    std::cout << "Active OptFlags    : " << OptFlags::formatSummary() << "\n";
    std::cout << "  (batchSubsteps=" << (OptFlags::get().batchSubsteps ? "ON" : "OFF")
              << ", bvhRefitLeaves=" << (OptFlags::get().bvhRefitLeaves ? "ON" : "OFF")
              << ", cursorThrottle=" << (OptFlags::get().cursorThrottle ? "ON" : "OFF")
              << ", analyticNormals=" << (OptFlags::get().analyticNormals ? "ON" : "OFF") << ")\n\n";

    auto tBuild0 = std::chrono::high_resolution_clock::now();
    auto meshPtr = createUVSphere(35.0f, 1000, 1000);
    Mesh* mesh = meshPtr.release();
    mesh->postInit(true);
    MeshSnapshot snap = captureSnapshot(*mesh);
    auto tBuild1 = std::chrono::high_resolution_clock::now();
    std::cout << "1M Test Sphere ready (" << mesh->nbVerts << " verts, " << mesh->nbFaces << " faces) in "
              << std::fixed << std::setprecision(1)
              << std::chrono::duration<double, std::milli>(tBuild1 - tBuild0).count() << " ms\n\n";

    Scene scene;
    scene.addMesh(mesh);
    scene.selectMesh(mesh);
    Camera& cam = scene.getCamera();
    cam.onResize(1280, 720);
    cam.resetViewToMesh(mesh, false);
    cam.update(0.016f);

    // Warmup OpenMP pool & SculptManager buffers so Step0 doesn't pay cold-cache jitter
    {
        StrokeBenchConfig warmCfg{ "Warmup", false, false, false, 2 };
        (void)measureBrushScenario(scene, mesh, snap, BRUSH_CLAYBUILDUP, 50.0f, warmCfg, 2, false);
    }

    runRadialCountScalingBench(scene, mesh, snap);
    runLargeRadiusAndMeshDensityBench();
    runBrushComparisonBench(scene, mesh, snap);
    runMicroBenchmarks(scene, mesh, snap);

    std::cout << "================================================================================\n";
    std::cout << "                    RADIAL SYMMETRY BENCHMARK COMPLETE\n";
    std::cout << "================================================================================\n\n";
    return true;
}

} // namespace RadialSymmetryBenchmark
