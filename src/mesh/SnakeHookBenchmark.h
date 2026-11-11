#pragma once

#include <iostream>
#include <iomanip>
#include <vector>
#include <chrono>
#include <cmath>
#include <memory>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <SDL2/SDL.h>

#include "mesh/Mesh.h"
#include "mesh/BVH.h"
#include "mesh/dynamic/DynamicTopology.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "editing/SculptManager.h"
#include "editing/undo/UndoManager.h"
#include "sculpt/SculptEngine.h"
#include "common/OptFlags.h"

namespace SnakeHookBenchmark {

inline std::unique_ptr<Mesh> createBenchmarkQuadSphere(float R = 35.0f, int rings = 32, int sectors = 64) {
    auto mesh = std::make_unique<Mesh>();
    mesh->nbVerts = 0;
    mesh->nbFaces = 0;
    mesh->verts.reserve(((rings - 1) * sectors + 2) * 3);
    mesh->faces.reserve(((rings - 2) * sectors + 2 * sectors) * 4);

    // South pole (index 0)
    mesh->verts.push_back(0.0f); mesh->verts.push_back(-R); mesh->verts.push_back(0.0f);
    uint32_t southPole = mesh->nbVerts++;

    // Intermediate rings (r = 1 to rings - 1)
    for (int r = 1; r < rings; ++r) {
        float phi = -1.57079632679f + 3.14159265359f * float(r) / float(rings);
        float y = R * std::sin(phi);
        float cos_phi = std::cos(phi);
        for (int s = 0; s < sectors; ++s) {
            float theta = 6.28318530718f * float(s) / float(sectors);
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
        mesh->faces.push_back(0xFFFFFFFF);
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
        mesh->faces.push_back(0xFFFFFFFF);
        mesh->nbFaces++;
    }

    mesh->vertVisible.assign(mesh->nbVerts, 1);
    mesh->faceVisible.assign(mesh->nbFaces, 1);
    return mesh;
}

struct IntegrityReport {
    size_t boundaryEdges = 0;      // Edges with 1 face (HOLES / TEARS)
    size_t nonManifoldEdges = 0;   // Edges with >2 faces
    size_t nanInfVerts = 0;        // Invalid coordinates
    size_t posSideVerts = 0;       // Vertices on +X side
    size_t negSideVerts = 0;       // Vertices on -X side
    size_t zeroPlaneVerts = 0;     // Vertices near X=0 plane
    float minX = 0.0f;
    float maxX = 0.0f;
    float asymError = 0.0f;        // |maxX - (-minX)|
    bool isWatertight = true;
    bool isSymmetric = true;
};

inline IntegrityReport auditMeshIntegrity(const Mesh& mesh, float symTol = 0.1f) {
    IntegrityReport rep;
    if (mesh.nbVerts <= 0 || mesh.nbFaces <= 0) {
        rep.isWatertight = false;
        rep.isSymmetric = false;
        return rep;
    }

    // 1. Check coordinates for NaN / Inf and symmetry extents
    rep.minX = 1e30f;
    rep.maxX = -1e30f;
    const float EPS = 1e-4f;
    const float* vData = mesh.verts.data();
    for (int i = 0; i < mesh.nbVerts; ++i) {
        float x = vData[i * 3 + 0];
        float y = vData[i * 3 + 1];
        float z = vData[i * 3 + 2];
        if (std::isnan(x) || std::isnan(y) || std::isnan(z) ||
            std::isinf(x) || std::isinf(y) || std::isinf(z)) {
            rep.nanInfVerts++;
            continue;
        }
        if (x < rep.minX) rep.minX = x;
        if (x > rep.maxX) rep.maxX = x;

        if (x > EPS) {
            rep.posSideVerts++;
        } else if (x < -EPS) {
            rep.negSideVerts++;
        } else {
            rep.zeroPlaneVerts++;
        }
    }
    rep.asymError = std::abs(rep.maxX - (-rep.minX));

    // 2. Count edge incident faces to detect holes (boundary edges)
    // 64-bit key = (min(u, v) << 32) | max(u, v)
    std::unordered_map<uint64_t, uint16_t> edgeFaceCount;
    edgeFaceCount.reserve(mesh.nbFaces * 3);

    auto addEdge = [&](uint32_t a, uint32_t b) {
        if (a == b || a >= (uint32_t)mesh.nbVerts || b >= (uint32_t)mesh.nbVerts) return;
        uint64_t key = (uint64_t(std::min(a, b)) << 32) | uint64_t(std::max(a, b));
        edgeFaceCount[key]++;
    };

    const uint32_t* fData = mesh.faces.data();
    for (int t = 0; t < mesh.nbFaces; ++t) {
        uint32_t i0 = fData[t * 4 + 0];
        uint32_t i1 = fData[t * 4 + 1];
        uint32_t i2 = fData[t * 4 + 2];
        uint32_t i3 = fData[t * 4 + 3];

        if (i3 == 0xFFFFFFFF) {
            // Triangle
            addEdge(i0, i1);
            addEdge(i1, i2);
            addEdge(i2, i0);
        } else {
            // Quad
            addEdge(i0, i1);
            addEdge(i1, i2);
            addEdge(i2, i3);
            addEdge(i3, i0);
        }
    }

    for (const auto& [edge, count] : edgeFaceCount) {
        if (count == 1) {
            rep.boundaryEdges++;
        } else if (count > 2) {
            rep.nonManifoldEdges++;
        }
    }

    rep.isWatertight = (rep.boundaryEdges == 0 && rep.nanInfVerts == 0);
    rep.isSymmetric = (rep.posSideVerts == rep.negSideVerts) && (rep.asymError <= symTol);
    return rep;
}

inline bool run(int numStrokes = 10, bool withDyntopo = true, bool withSymmetry = true) {
    std::cout << "\n================================================================================\n";
    std::cout << "  SPSculpt SnakeHook Brush Benchmark & Hole / Symmetry Audit\n";
    std::cout << "  Configuration: " << numStrokes << " Pull Strokes | Dynamic Topology: "
              << (withDyntopo ? "ON (detail=0.50)" : "OFF")
              << " | Symmetry X: " << (withSymmetry ? "ON" : "OFF") << "\n";
    std::cout << "================================================================================\n\n";

    // Ensure immediate execution of drag steps in benchmark
    bool oldCoalesce = OptFlags::get().coalesceGrab;
    OptFlags::get().coalesceGrab = false;

    auto meshPtr = createBenchmarkQuadSphere(35.0f, 32, 64);
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
    sculpt.setBrush(BRUSH_SNAKEHOOK);

    auto& settings = sculpt.getSettings(BRUSH_SNAKEHOOK);
    settings.radius = 82.5f;
    settings.intensity = 1.0f;
    settings.spacing = 0.1f;
    settings.culling = false;
    settings.snakePinch = 0.0f;
    settings.snakeMagnify = 0.0f;
    settings.snakeTwist = 0.0f;
    settings.snakeRelax = 0.05f;

    if (withSymmetry) {
        sculpt.setUseSym(true);
        sculpt.setSymX(true);
        sculpt.setSymY(false);
        sculpt.setSymZ(false);
    } else {
        sculpt.setUseSym(false);
    }

    if (withDyntopo) {
        sculpt.setDyntopoEnabled(true);
        sculpt.getDyntopoSettings().enabled = true;
        sculpt.getDyntopoSettings().detail = 0.5f;
        sculpt.getDyntopoSettings().constantDetail = false;
        sculpt.getCurrentSettings().topologyEdgeSize = 0.6f;
    } else {
        sculpt.setDyntopoEnabled(false);
    }

    IntegrityReport initialRep = auditMeshIntegrity(*meshRaw);
    std::cout << "Initial Sphere: " << meshRaw->nbVerts << " vertices, " << meshRaw->nbFaces
              << " faces (QuadSphere R=35.0)\n";
    std::cout << "Initial Boundary Edges: " << initialRep.boundaryEdges
              << " (Watertight: " << (initialRep.isWatertight ? "YES" : "NO") << ")\n";
    std::cout << "Initial Symmetry: PosX=" << initialRep.posSideVerts << ", NegX=" << initialRep.negSideVerts
              << ", ZeroX=" << initialRep.zeroPlaneVerts
              << " | AsymError=" << std::fixed << std::setprecision(4) << initialRep.asymError << "\n\n";

    constexpr int DRAG_STEPS_PER_STROKE = 8;
    double totalBenchMs = 0.0;
    double maxStrokeMs = 0.0;
    int maxStrokeIdx = 0;
    size_t maxHolesEncountered = 0;
    bool allStrokesSymmetric = true;

    auto tBenchStart = std::chrono::high_resolution_clock::now();

    for (int s = 0; s < numStrokes; ++s) {
        auto tStrokeStart = std::chrono::high_resolution_clock::now();

        // 1. Pick a surface point on the +X hemisphere of the sphere
        float angleY = (float(s) - float(numStrokes) * 0.5f) * 0.15f;
        float cy = 18.0f * std::sin(angleY);
        float rCross = std::sqrt(std::max(0.0f, 35.0f * 35.0f - cy * cy));
        float angleX = 0.35f + 0.10f * std::sin(float(s) * 0.8f);
        float cx = rCross * std::cos(angleX); // Strongly on +X side
        float cz = rCross * std::sin(angleX); // Front-facing (+Z)

        glm::vec3 surfacePt(cx, cy, cz);
        glm::vec3 startScreen = camera.project(surfacePt);

        // Pull target: pull outward along +X by 25.0 to 45.0 units
        float pullDist = 30.0f + 10.0f * std::sin(float(s) * 0.5f);
        glm::vec3 pullTargetPt = surfacePt + glm::vec3(pullDist, 2.0f * std::sin(float(s)), 5.0f);
        glm::vec3 endScreen = camera.project(pullTargetPt);

        float dx = (endScreen.x - startScreen.x) / float(DRAG_STEPS_PER_STROKE);
        float dy = (endScreen.y - startScreen.y) / float(DRAG_STEPS_PER_STROKE);

        // 1. Mouse Down
        SDL_Event evDown;
        std::memset(&evDown, 0, sizeof(evDown));
        evDown.type = SDL_MOUSEBUTTONDOWN;
        evDown.button.button = SDL_BUTTON_LEFT;
        evDown.button.x = (int)std::round(startScreen.x);
        evDown.button.y = (int)std::round(startScreen.y);
        sculpt.handleEvent(evDown, scene);

        // 2. Drag steps (pulling the snake hook out)
        for (int step = 1; step <= DRAG_STEPS_PER_STROKE; ++step) {
            float curX = startScreen.x + float(step) * dx;
            float curY = startScreen.y + float(step) * dy;

            SDL_Event evMove;
            std::memset(&evMove, 0, sizeof(evMove));
            evMove.type = SDL_MOUSEMOTION;
            evMove.motion.state = SDL_BUTTON_LMASK;
            evMove.motion.x = (int)std::round(curX);
            evMove.motion.y = (int)std::round(curY);
            sculpt.handleEvent(evMove, scene);
        }

        // 3. Mouse Up
        SDL_Event evUp;
        std::memset(&evUp, 0, sizeof(evUp));
        evUp.type = SDL_MOUSEBUTTONUP;
        evUp.button.button = SDL_BUTTON_LEFT;
        evUp.button.x = (int)std::round(endScreen.x);
        evUp.button.y = (int)std::round(endScreen.y);
        sculpt.handleEvent(evUp, scene);

        auto tStrokeEnd = std::chrono::high_resolution_clock::now();
        double strokeMs = std::chrono::duration<double, std::milli>(tStrokeEnd - tStrokeStart).count();
        if (strokeMs > maxStrokeMs) {
            maxStrokeMs = strokeMs;
            maxStrokeIdx = s + 1;
        }

        // Audit integrity after stroke
        IntegrityReport rep = auditMeshIntegrity(*meshRaw);
        if (rep.boundaryEdges > maxHolesEncountered) {
            maxHolesEncountered = rep.boundaryEdges;
        }
        if (!rep.isSymmetric && withSymmetry) {
            allStrokesSymmetric = false;
        }

        std::cout << "  [Stroke " << std::setw(2) << (s + 1) << "/" << numStrokes << "]"
                  << " V=" << std::setw(6) << meshRaw->nbVerts
                  << " F=" << std::setw(6) << meshRaw->nbFaces
                  << " | Time=" << std::fixed << std::setprecision(1) << std::setw(5) << strokeMs << "ms"
                  << " | Holes(BoundaryEdges)=" << rep.boundaryEdges
                  << " | PosX=" << std::setw(5) << rep.posSideVerts
                  << " NegX=" << std::setw(5) << rep.negSideVerts
                  << " | ExtrusionX=[-" << std::fixed << std::setprecision(1) << (-rep.minX)
                  << ", +" << rep.maxX << "]"
                  << " | Status=" << (rep.boundaryEdges == 0 ? "OK" : "HOLES DETECTED!") << "\n";
        std::cout.flush();
    }

    auto tBenchEnd = std::chrono::high_resolution_clock::now();
    totalBenchMs = std::chrono::duration<double, std::milli>(tBenchEnd - tBenchStart).count();

    IntegrityReport finalRep = auditMeshIntegrity(*meshRaw);
    OptFlags::get().coalesceGrab = oldCoalesce;

    std::cout << "\n================================================================================\n";
    std::cout << "                 SNAKEHOOK BENCHMARK & AUDIT SUMMARY\n";
    std::cout << "================================================================================\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Configuration:             " << (withDyntopo ? "Dynamic Topology ON" : "Dyntopo OFF")
              << " + " << (withSymmetry ? "Symmetry X ON" : "Symmetry OFF") << "\n";
    std::cout << "Total Wall Time:           " << totalBenchMs << " ms (" << (totalBenchMs / 1000.0) << " s)\n";
    std::cout << "Average Stroke Time:       " << (totalBenchMs / numStrokes) << " ms\n";
    std::cout << "Max Stroke Time:           " << maxStrokeMs << " ms at stroke " << maxStrokeIdx << "\n";
    std::cout << "Final Mesh Complexity:     " << meshRaw->nbVerts << " verts, " << meshRaw->nbFaces << " faces\n";
    std::cout << "Holes (Boundary Edges):    " << finalRep.boundaryEdges << " (Expected: 0)\n";
    std::cout << "Non-Manifold Edges:        " << finalRep.nonManifoldEdges << " (Expected: 0)\n";
    std::cout << "NaN / Inf Vertices:        " << finalRep.nanInfVerts << " (Expected: 0)\n";
    std::cout << "Positive (+X) Vertices:    " << finalRep.posSideVerts << "\n";
    std::cout << "Negative (-X) Vertices:    " << finalRep.negSideVerts << "\n";
    std::cout << "Vertex Count Symmetry:     " << (finalRep.posSideVerts == finalRep.negSideVerts ? "EXACT MATCH (PASS)" : "MISMATCH (FAIL)") << "\n";
    std::cout << "Extrusion Extents:         MinX=" << finalRep.minX << ", MaxX=" << finalRep.maxX
              << " (Extrusion Delta Asymmetry: " << finalRep.asymError << ")\n";
    std::cout << "Symmetry Extent Integrity: " << (finalRep.asymError <= 0.1f ? "SYMMETRIC (PASS)" : "ASYMMETRIC (FAIL)") << "\n";
    std::cout << "Watertight Surface:        " << (finalRep.isWatertight ? "VERIFIED WATERTIGHT (PASS)" : "HOLES/DEGENERATE (FAIL)") << "\n";
    std::cout << "================================================================================\n\n";

    bool success = (finalRep.boundaryEdges == 0) &&
                   (finalRep.nonManifoldEdges == 0) &&
                   (finalRep.nanInfVerts == 0) &&
                   (!withSymmetry || finalRep.posSideVerts == finalRep.negSideVerts);

    if (success) {
        std::cout << ">>> BENCHMARK RESULT: ALL SNAKEHOOK INTEGRITY & SYMMETRY CHECKS PASSED <<<\n\n";
    } else {
        std::cerr << ">>> BENCHMARK RESULT: INTEGRITY / HOLE CHECK FAILED! <<<\n\n";
    }
    return success;
}

} // namespace SnakeHookBenchmark
