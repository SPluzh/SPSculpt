#pragma once

#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <chrono>
#include <cmath>
#include <random>
#include <memory>
#include <cstring>
#include <numeric>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "mesh/Mesh.h"
#include "mesh/NormalCalc.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "editing/SculptManager.h"
#include "editing/undo/UndoManager.h"
#include "editing/undo/TransformUndoEntry.h"
#include "editing/undo/TopologyUndoEntry.h"
#include "sculpt/SculptEngine.h"
#include "common/Enums.h"
#include <omp.h>

#if __has_include("mesh/BVH.h")
#define BENCH_USE_BVH 1
#include "mesh/BVH.h"
#else
#define BENCH_USE_BVH 0
#include "mesh/Octree.h"
#endif

namespace SculptBenchmark {

inline bool rayTriangleIntersect(
    const glm::vec3& rayOrigin, const glm::vec3& rayDir,
    const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
    float& t
) {
    const float EPSILON = 1e-7f;
    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;
    glm::vec3 h = glm::cross(rayDir, edge2);
    float a = glm::dot(edge1, h);
    if (std::abs(a) < EPSILON) return false;
    float f = 1.0f / a;
    glm::vec3 s = rayOrigin - v0;
    float u = f * glm::dot(s, h);
    if (u < 0.0f || u > 1.0f) return false;
    glm::vec3 q = glm::cross(s, edge1);
    float v = f * glm::dot(rayDir, q);
    if (v < 0.0f || u + v > 1.0f) return false;
    t = f * glm::dot(edge2, q);
    return t > EPSILON;
}

inline bool octreeRaycast(const Mesh& mesh, const glm::vec3& origin, const glm::vec3& dir, float& outT, uint32_t& outFace) {
#if !BENCH_USE_BVH
    std::vector<uint32_t> candidateFaces = const_cast<Mesh&>(mesh).octree.collectIntersectRay(
        origin.x, origin.y, origin.z, dir.x, dir.y, dir.z
    );
    float minT = std::numeric_limits<float>::infinity();
    uint32_t hitFace = 0xffffffff;
    for (uint32_t faceId : candidateFaces) {
        if (faceId >= (uint32_t)mesh.nbFaces) continue;
        uint32_t v0 = mesh.faces[faceId * 4 + 0];
        uint32_t v1 = mesh.faces[faceId * 4 + 1];
        uint32_t v2 = mesh.faces[faceId * 4 + 2];
        uint32_t v3 = mesh.faces[faceId * 4 + 3];
        glm::vec3 p0(mesh.verts[v0 * 3], mesh.verts[v0 * 3 + 1], mesh.verts[v0 * 3 + 2]);
        glm::vec3 p1(mesh.verts[v1 * 3], mesh.verts[v1 * 3 + 1], mesh.verts[v1 * 3 + 2]);
        glm::vec3 p2(mesh.verts[v2 * 3], mesh.verts[v2 * 3 + 1], mesh.verts[v2 * 3 + 2]);
        float t;
        if (rayTriangleIntersect(origin, dir, p0, p1, p2, t) && t < minT) {
            minT = t;
            hitFace = faceId;
        }
        if (v3 != 0xffffffff) {
            glm::vec3 p3(mesh.verts[v3 * 3], mesh.verts[v3 * 3 + 1], mesh.verts[v3 * 3 + 2]);
            if (rayTriangleIntersect(origin, dir, p0, p2, p3, t) && t < minT) {
                minT = t;
                hitFace = faceId;
            }
        }
    }
    if (hitFace != 0xffffffff) {
        outT = minT;
        outFace = hitFace;
        return true;
    }
#endif
    return false;
}

inline bool queryRaycast(const Mesh& mesh, const glm::vec3& origin, const glm::vec3& dir, float& outT, uint32_t& outFace) {
#if BENCH_USE_BVH
    RayHit hit;
    if (mesh.raycast(origin, dir, hit)) {
        outT = hit.t;
        outFace = hit.faceId;
        return true;
    }
    return false;
#else
    return octreeRaycast(mesh, origin, dir, outT, outFace);
#endif
}

inline void queryPickVerticesInSphere(const Mesh& mesh, const glm::vec3& center, float r2, std::vector<uint32_t>& outVerts) {
#if BENCH_USE_BVH
    mesh.bvh.pickVerticesInSphere(mesh, center, r2, outVerts);
#else
    const_cast<Mesh&>(mesh).octree.pickVerticesInSphere(center.x, center.y, center.z, r2, mesh.vertVisible.data(), outVerts);
#endif
}

inline size_t getSpatialIndexMemoryBytes(const Mesh& mesh) {
#if BENCH_USE_BVH
    return mesh.bvh.memoryBytes();
#else
    const auto& octree = mesh.octree;
    size_t bytes = sizeof(Octree);
    bytes += octree.faceLeaf.capacity() * sizeof(OctreeCell*);
    bytes += octree.facePosInLeaf.capacity() * sizeof(int);
    bytes += octree.cellPool.capacity() * sizeof(OctreeCell*);
    for (int i = 0; i < octree.poolIndex && i < (int)octree.cellPool.size(); ++i) {
        if (octree.cellPool[i]) {
            bytes += sizeof(OctreeCell);
            bytes += octree.cellPool[i]->iFaces.capacity() * sizeof(uint32_t);
        }
    }
    bytes += octree.lastPickedVertices.capacity() * sizeof(uint32_t);
    bytes += octree.lastIntersectedFaces.capacity() * sizeof(uint32_t);
    return bytes;
#endif
}

inline double getSpatialUpdateMs(const SculptFrameProfile& prof) {
#if BENCH_USE_BVH
    return prof.bvhUpdateMs;
#else
    return prof.octreeUpdateMs;
#endif
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

struct StrokeStepMeasurement {
    int stepIndex = 0;
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    double totalMs = 0.0;
    double raycastMs = 0.0;
    double pickVertsMs = 0.0;
    double pickCellsMs = 0.0;
    double pickDedupMs = 0.0;
    double pickFilterMs = 0.0;
    double areaComputeMs = 0.0;
    double primaryDeformMs = 0.0;
    double symmetryMs = 0.0;
    double sortDedupMs = 0.0;
    double undoRecordMs = 0.0;
    double faceLookupMs = 0.0;
    double faceNormalsMs = 0.0;
    double vertNormalsMs = 0.0;
    double spatialUpdateMs = 0.0;
    int pickedVerts = 0;
    int affectedVerts = 0;
    int affectedFaces = 0;
};

struct MultiToolBenchmarkRow {
    std::string toolName;
    bool hasStep0 = true;
    double step0Ms = 0.0;
    bool hasDrag = true;
    double dragAvgMs = 0.0;
    double finalizeMs = 0.0;
    double totalStrokeMs = 0.0;
    double undoMs = 0.0;
    double redoMs = 0.0;
    size_t undoRAMBytes = 0;
    bool passed = false;
    float maxUndoEps = 0.0f;
    float maxRedoEps = 0.0f;
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

inline bool run(int targetFaces = 4000000) {
    const char* indexName = BENCH_USE_BVH ? "Linear BVH (feature/bvh-spatial-index)" : "Octree (master branch)";
    
    std::cout << "\n======================================================================\n";
    std::cout << " SPSculpt Multi-Tool Performance & Undo Benchmark @ " << std::fixed << std::setprecision(1) << (targetFaces / 1000000.0) << "M Polygons\n";
    std::cout << " Spatial Acceleration Backend: " << indexName << "\n";
    std::cout << "======================================================================\n\n";

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
    Mesh& mesh = *meshRaw;
    auto tGenEnd = std::chrono::high_resolution_clock::now();
    double genMs = std::chrono::duration<double, std::milli>(tGenEnd - tGenStart).count();
    std::cout << "      Sphere generation completed in " << std::fixed << std::setprecision(2) << genMs << " ms\n";

    std::cout << "\n[2/4] Initializing Mesh & Building Spatial Structure (Mesh::postInit)...\n";
    auto tInitStart = std::chrono::high_resolution_clock::now();
    mesh.postInit(true);
    auto tInitEnd = std::chrono::high_resolution_clock::now();
    double initMs = std::chrono::duration<double, std::milli>(tInitEnd - tInitStart).count();

    size_t spatialBytes = getSpatialIndexMemoryBytes(mesh);
    double spatialMB = (double)spatialBytes / (1024.0 * 1024.0);

    std::cout << "      Total postInit time:   " << std::fixed << std::setprecision(2) << initMs << " ms\n";
    std::cout << "      Spatial Index RAM:     " << std::fixed << std::setprecision(2) << spatialMB << " MB (" << spatialBytes << " bytes)\n";

    // -------------------------------------------------------------
    // Micro-benchmarks: Isolated Raycast and Sphere Queries
    // -------------------------------------------------------------
    std::cout << "\n[3/4] Running Isolated Spatial Queries on Mesh...\n";

    // 1,000 Raycasts
    {
        std::mt19937 rng(42);
        std::uniform_real_distribution<float> distAngle(0.0f, 6.2831853f);
        std::uniform_real_distribution<float> distY(-1.5f, 1.5f);

        int hitCount = 0;
        int missCount = 0;
        auto tRayStart = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 1000; ++i) {
            float angle = distAngle(rng);
            glm::vec3 origin(5.0f * std::cos(angle), distY(rng), 5.0f * std::sin(angle));
            glm::vec3 target(0.0f);
            if (i >= 500) {
                // Ray aiming away (miss)
                target = origin + glm::vec3(0.0f, 5.0f, 0.0f);
            }
            glm::vec3 dir = glm::normalize(target - origin);
            float t;
            uint32_t faceId;
            if (queryRaycast(mesh, origin, dir, t, faceId)) {
                hitCount++;
            } else {
                missCount++;
            }
        }
        auto tRayEnd = std::chrono::high_resolution_clock::now();
        double totalRayUs = std::chrono::duration<double, std::micro>(tRayEnd - tRayStart).count();
        double avgRayUs = totalRayUs / 1000.0;
        std::cout << "      [Raycast] 1,000 Rays (500 hits, 500 misses):\n";
        std::cout << "        - Total Time:   " << std::fixed << std::setprecision(2) << (totalRayUs / 1000.0) << " ms\n";
        std::cout << "        - Avg per ray:  " << std::fixed << std::setprecision(2) << avgRayUs << " microseconds (" << (avgRayUs / 1000.0) << " ms)\n";
        std::cout << "        - Hits/Misses:  " << hitCount << " hits / " << missCount << " misses\n";
    }

    // Sphere Vertex Queries for 3 radii
    {
        struct RadiusTest {
            const char* name;
            float r;
        } radiusTests[] = {
            { "Small  (r=0.04)", 0.04f },
            { "Medium (r=0.10)", 0.10f },
            { "Large  (r=0.20)", 0.20f }
        };

        std::mt19937 rng(1337);
        std::uniform_real_distribution<float> distPhi(0.3f, 2.8f);
        std::uniform_real_distribution<float> distTheta(0.0f, 6.28f);

        for (const auto& test : radiusTests) {
            float r2 = test.r * test.r;
            std::vector<uint32_t> pickedVerts;
            double totalPickMs = 0.0;
            size_t totalCount = 0;
            const int NUM_QUERIES = 30;

            for (int q = 0; q < NUM_QUERIES; ++q) {
                float phi = distPhi(rng);
                float theta = distTheta(rng);
                glm::vec3 center(2.0f * std::sin(phi) * std::cos(theta), 2.0f * std::cos(phi), 2.0f * std::sin(phi) * std::sin(theta));

                auto tStart = std::chrono::high_resolution_clock::now();
                queryPickVerticesInSphere(mesh, center, r2, pickedVerts);
                auto tEnd = std::chrono::high_resolution_clock::now();
                totalPickMs += std::chrono::duration<double, std::milli>(tEnd - tStart).count();
                totalCount += pickedVerts.size();
            }
            double avgPickMs = totalPickMs / NUM_QUERIES;
            double avgVerts = (double)totalCount / NUM_QUERIES;
            std::cout << "      [Pick Vertices] " << test.name << ":\n";
            std::cout << "        - Avg Latency:  " << std::fixed << std::setprecision(3) << avgPickMs << " ms\n";
            std::cout << "        - Avg Vertices: " << (int)avgVerts << " vertices picked\n";
        }
    }

    // -------------------------------------------------------------
    // Multi-Tool Interactive Pipeline & Undo/Redo Benchmark
    // -------------------------------------------------------------
    std::cout << "\n[4/4] Running Multi-Tool Pipeline & Undo/Redo Validation on Mesh...\n";

    Scene scene;
    scene.addMesh(meshRaw);
    scene.selectMesh(meshRaw);

    Camera& camera = scene.getCamera();
    camera.onResize(1920, 1080);
    camera.resetViewToMesh(meshRaw, false);
    camera.update(0.016f);

    SculptManager sculpt;
    sculpt.setRenderer(nullptr);

    const int NUM_STROKE_STEPS = 10;
    const float startX = 960.0f;
    const float startY = 540.0f;
    const float endX = 1060.0f;
    const float endY = 540.0f;
    const float stepDx = (endX - startX) / (float)NUM_STROKE_STEPS;
    const float stepDy = (endY - startY) / (float)NUM_STROKE_STEPS;

    std::vector<MultiToolBenchmarkRow> summaryRows;
    summaryRows.reserve(6);

    // =========================================================================
    // 1. Tool: ClayBuildup (Stroke 10x)
    // =========================================================================
    std::vector<StrokeStepMeasurement> clayMeasurements;
    clayMeasurements.reserve(NUM_STROKE_STEPS);
    {
        std::cout << "  -> Benchmarking ClayBuildup (Stroke 10x)...\n";
        sculpt.setBrush(BRUSH_CLAYBUILDUP);
        auto& settings = sculpt.getSettings(BRUSH_CLAYBUILDUP);
        settings.radius = 134.0f;
        settings.intensity = 1.0f;
        settings.focalShift = -0.285f;
        settings.spacing = 0.05f;
        settings.clay = true;
        settings.accumulate = false;
        settings.culling = false;

        sculpt.warmupMeshCaches(meshRaw);

        // Step 0: Mouse Down
        SDL_Event evDown;
        std::memset(&evDown, 0, sizeof(evDown));
        evDown.type = SDL_MOUSEBUTTONDOWN;
        evDown.button.button = SDL_BUTTON_LEFT;
        evDown.button.x = (int)startX;
        evDown.button.y = (int)startY;

        auto tStep0Start = std::chrono::high_resolution_clock::now();
        sculpt.handleEvent(evDown, scene);
        auto tStep0End = std::chrono::high_resolution_clock::now();
        double step0Ms = std::chrono::duration<double, std::milli>(tStep0End - tStep0Start).count();

        const SculptFrameProfile& prof0 = sculpt.getLastFrameProfile();
        StrokeStepMeasurement m0;
        m0.stepIndex = 0;
        m0.mouseX = startX;
        m0.mouseY = startY;
        m0.totalMs = step0Ms;
        m0.raycastMs = prof0.raycastMs;
        m0.pickVertsMs = prof0.pickVertsMs;
        m0.pickCellsMs = prof0.pickCellsMs;
        m0.pickDedupMs = prof0.pickDedupMs;
        m0.pickFilterMs = prof0.pickFilterMs;
        m0.areaComputeMs = prof0.areaComputeMs;
        m0.primaryDeformMs = prof0.primaryDeformMs;
        m0.symmetryMs = prof0.symmetryMs;
        m0.sortDedupMs = prof0.sortDedupMs;
        m0.undoRecordMs = prof0.undoRecordMs;
        m0.faceLookupMs = prof0.faceLookupMs;
        m0.faceNormalsMs = prof0.faceNormalsMs;
        m0.vertNormalsMs = prof0.vertNormalsMs;
        m0.spatialUpdateMs = getSpatialUpdateMs(prof0);
        m0.pickedVerts = prof0.pickedVertCount;
        m0.affectedVerts = prof0.affectedVertCount;
        m0.affectedFaces = prof0.affectedFaceCount;
        clayMeasurements.push_back(m0);

        // Continuous Drag (Steps 1..9)
        double sumDragMs = 0.0;
        for (int step = 1; step < NUM_STROKE_STEPS; ++step) {
            float curX = startX + step * stepDx;
            float curY = startY + step * stepDy;

            auto tStepStart = std::chrono::high_resolution_clock::now();
            sculpt.executeStroke(scene, meshRaw, camera, curX, curY, 1.0f);
            auto tStepEnd = std::chrono::high_resolution_clock::now();
            double stepMs = std::chrono::duration<double, std::milli>(tStepEnd - tStepStart).count();
            sumDragMs += stepMs;

            const SculptFrameProfile& prof = sculpt.getLastFrameProfile();
            StrokeStepMeasurement m;
            m.stepIndex = step;
            m.mouseX = curX;
            m.mouseY = curY;
            m.totalMs = stepMs;
            m.raycastMs = prof.raycastMs;
            m.pickVertsMs = prof.pickVertsMs;
            m.pickCellsMs = prof.pickCellsMs;
            m.pickDedupMs = prof.pickDedupMs;
            m.pickFilterMs = prof.pickFilterMs;
            m.areaComputeMs = prof.areaComputeMs;
            m.primaryDeformMs = prof.primaryDeformMs;
            m.symmetryMs = prof.symmetryMs;
            m.sortDedupMs = prof.sortDedupMs;
            m.undoRecordMs = prof.undoRecordMs;
            m.faceLookupMs = prof.faceLookupMs;
            m.faceNormalsMs = prof.faceNormalsMs;
            m.vertNormalsMs = prof.vertNormalsMs;
            m.spatialUpdateMs = getSpatialUpdateMs(prof);
            m.pickedVerts = prof.pickedVertCount;
            m.affectedVerts = prof.affectedVertCount;
            m.affectedFaces = prof.affectedFaceCount;
            clayMeasurements.push_back(m);
        }
        double dragAvgMs = sumDragMs / (NUM_STROKE_STEPS - 1);

        // Finalize: Mouse Up
        SDL_Event evUp;
        std::memset(&evUp, 0, sizeof(evUp));
        evUp.type = SDL_MOUSEBUTTONUP;
        evUp.button.button = SDL_BUTTON_LEFT;
        evUp.button.x = (int)endX;
        evUp.button.y = (int)endY;

        auto tFinStart = std::chrono::high_resolution_clock::now();
        sculpt.handleEvent(evUp, scene);
        auto tFinEnd = std::chrono::high_resolution_clock::now();
        double finalizeMs = std::chrono::duration<double, std::milli>(tFinEnd - tFinStart).count();

        double totalStrokeMs = step0Ms + sumDragMs + finalizeMs;
        size_t undoBytes = !g_undoManager.getUndoStack().empty() ? g_undoManager.getUndoStack().back()->getMemoryUsage() : 0;

        // Undo
        auto tUndoStart = std::chrono::high_resolution_clock::now();
        g_undoManager.undo(scene);
        auto tUndoEnd = std::chrono::high_resolution_clock::now();
        double undoMs = std::chrono::duration<double, std::milli>(tUndoEnd - tUndoStart).count();

        float maxUndoDiff = 0.0f;
        auto* redoEntry = !g_undoManager.getRedoStack().empty()
            ? dynamic_cast<SculptUndoEntry*>(g_undoManager.getRedoStack().back().get())
            : nullptr;
        if (redoEntry && !redoEntry->deltas.empty()) {
            const auto& d = redoEntry->deltas[0];
            size_t n = d.indices.size();
#pragma omp parallel for reduction(max:maxUndoDiff) schedule(static) if(n > 10000)
            for (size_t i = 0; i < n; ++i) {
                uint32_t vid = d.indices[i];
                float d0 = std::abs(mesh.verts[vid * 3 + 0] - d.prevVerts[i * 3 + 0]);
                float d1 = std::abs(mesh.verts[vid * 3 + 1] - d.prevVerts[i * 3 + 1]);
                float d2 = std::abs(mesh.verts[vid * 3 + 2] - d.prevVerts[i * 3 + 2]);
                float m = std::max({d0, d1, d2});
                if (m > maxUndoDiff) maxUndoDiff = m;
            }
        }

        // Redo
        auto tRedoStart = std::chrono::high_resolution_clock::now();
        g_undoManager.redo(scene);
        auto tRedoEnd = std::chrono::high_resolution_clock::now();
        double redoMs = std::chrono::duration<double, std::milli>(tRedoEnd - tRedoStart).count();

        float maxRedoDiff = 0.0f;
        auto* undoEntryAfterRedo = !g_undoManager.getUndoStack().empty()
            ? dynamic_cast<SculptUndoEntry*>(g_undoManager.getUndoStack().back().get())
            : nullptr;
        if (undoEntryAfterRedo && !undoEntryAfterRedo->deltas.empty()) {
            const auto& d = undoEntryAfterRedo->deltas[0];
            size_t n = d.indices.size();
#pragma omp parallel for reduction(max:maxRedoDiff) schedule(static) if(n > 10000)
            for (size_t i = 0; i < n; ++i) {
                uint32_t vid = d.indices[i];
                float d0 = std::abs(mesh.verts[vid * 3 + 0] - d.nextVerts[i * 3 + 0]);
                float d1 = std::abs(mesh.verts[vid * 3 + 1] - d.nextVerts[i * 3 + 1]);
                float d2 = std::abs(mesh.verts[vid * 3 + 2] - d.nextVerts[i * 3 + 2]);
                float m = std::max({d0, d1, d2});
                if (m > maxRedoDiff) maxRedoDiff = m;
            }
        }

        // Reset mesh back to original
        g_undoManager.undo(scene);
        g_undoManager.clear();

        MultiToolBenchmarkRow row;
        row.toolName = "ClayBuildup (Stroke 10x)";
        row.hasStep0 = true;
        row.step0Ms = step0Ms;
        row.hasDrag = true;
        row.dragAvgMs = dragAvgMs;
        row.finalizeMs = finalizeMs;
        row.totalStrokeMs = totalStrokeMs;
        row.undoMs = undoMs;
        row.redoMs = redoMs;
        row.undoRAMBytes = undoBytes;
        row.maxUndoEps = maxUndoDiff;
        row.maxRedoEps = maxRedoDiff;
        row.passed = (maxUndoDiff == 0.0f && maxRedoDiff == 0.0f);
        summaryRows.push_back(row);
    }

    // =========================================================================
    // 2. Tool: Move Tool (Drag 10x)
    // =========================================================================
    {
        std::cout << "  -> Benchmarking Move Tool (Drag 10x)...\n";
        sculpt.setBrush(BRUSH_MOVE);
        auto& moveSettings = sculpt.getSettings(BRUSH_MOVE);
        moveSettings.radius = 250.0f;
        moveSettings.intensity = 1.0f;
        moveSettings.topoCheck = true;
        moveSettings.culling = false;

        mesh.vertProxy = mesh.verts;
        sculpt.warmupMeshCaches(meshRaw);

        // Step 0: Mouse Down (vertex capture, falloff, deform prep)
        SDL_Event evDown;
        std::memset(&evDown, 0, sizeof(evDown));
        evDown.type = SDL_MOUSEBUTTONDOWN;
        evDown.button.button = SDL_BUTTON_LEFT;
        evDown.button.x = (int)startX;
        evDown.button.y = (int)startY;

        auto tStep0Start = std::chrono::high_resolution_clock::now();
        sculpt.handleEvent(evDown, scene);
        auto tStep0End = std::chrono::high_resolution_clock::now();
        double step0Ms = std::chrono::duration<double, std::milli>(tStep0End - tStep0Start).count();

        // Continuous Drag (Steps 1..9)
        double sumDragMs = 0.0;
        for (int step = 1; step < NUM_STROKE_STEPS; ++step) {
            float curX = startX + step * stepDx;
            float curY = startY + step * stepDy;

            auto tStepStart = std::chrono::high_resolution_clock::now();
            sculpt.executeStroke(scene, meshRaw, camera, curX, curY, 1.0f);
            auto tStepEnd = std::chrono::high_resolution_clock::now();
            sumDragMs += std::chrono::duration<double, std::milli>(tStepEnd - tStepStart).count();
        }
        double dragAvgMs = sumDragMs / (NUM_STROKE_STEPS - 1);

        // Finalize: Mouse Up (clearing grab buffers and finalizing stroke state)
        SDL_Event evUp;
        std::memset(&evUp, 0, sizeof(evUp));
        evUp.type = SDL_MOUSEBUTTONUP;
        evUp.button.button = SDL_BUTTON_LEFT;
        evUp.button.x = (int)endX;
        evUp.button.y = (int)endY;

        auto tFinStart = std::chrono::high_resolution_clock::now();
        sculpt.handleEvent(evUp, scene);
        auto tFinEnd = std::chrono::high_resolution_clock::now();
        double finalizeMs = std::chrono::duration<double, std::milli>(tFinEnd - tFinStart).count();
        if (finalizeMs > 5.0) {
            std::cout << "      [Move Tool Finalize Detail] handleEvent(evUp): " << std::fixed << std::setprecision(2) << finalizeMs << " ms\n";
        }

        double totalStrokeMs = step0Ms + sumDragMs + finalizeMs;
        size_t undoBytes = !g_undoManager.getUndoStack().empty() ? g_undoManager.getUndoStack().back()->getMemoryUsage() : 0;

        // Undo
        auto tUndoStart = std::chrono::high_resolution_clock::now();
        g_undoManager.undo(scene);
        auto tUndoEnd = std::chrono::high_resolution_clock::now();
        double undoMs = std::chrono::duration<double, std::milli>(tUndoEnd - tUndoStart).count();

        float maxUndoDiff = 0.0f;
        auto* redoEntry = !g_undoManager.getRedoStack().empty()
            ? dynamic_cast<SculptUndoEntry*>(g_undoManager.getRedoStack().back().get())
            : nullptr;
        if (redoEntry && !redoEntry->deltas.empty()) {
            const auto& d = redoEntry->deltas[0];
            size_t n = d.indices.size();
#pragma omp parallel for reduction(max:maxUndoDiff) schedule(static) if(n > 10000)
            for (size_t i = 0; i < n; ++i) {
                uint32_t vid = d.indices[i];
                float d0 = std::abs(mesh.verts[vid * 3 + 0] - d.prevVerts[i * 3 + 0]);
                float d1 = std::abs(mesh.verts[vid * 3 + 1] - d.prevVerts[i * 3 + 1]);
                float d2 = std::abs(mesh.verts[vid * 3 + 2] - d.prevVerts[i * 3 + 2]);
                float m = std::max({d0, d1, d2});
                if (m > maxUndoDiff) maxUndoDiff = m;
            }
        }

        // Redo
        auto tRedoStart = std::chrono::high_resolution_clock::now();
        g_undoManager.redo(scene);
        auto tRedoEnd = std::chrono::high_resolution_clock::now();
        double redoMs = std::chrono::duration<double, std::milli>(tRedoEnd - tRedoStart).count();

        float maxRedoDiff = 0.0f;
        auto* undoEntryAfterRedo = !g_undoManager.getUndoStack().empty()
            ? dynamic_cast<SculptUndoEntry*>(g_undoManager.getUndoStack().back().get())
            : nullptr;
        if (undoEntryAfterRedo && !undoEntryAfterRedo->deltas.empty()) {
            const auto& d = undoEntryAfterRedo->deltas[0];
            size_t n = d.indices.size();
#pragma omp parallel for reduction(max:maxRedoDiff) schedule(static) if(n > 10000)
            for (size_t i = 0; i < n; ++i) {
                uint32_t vid = d.indices[i];
                float d0 = std::abs(mesh.verts[vid * 3 + 0] - d.nextVerts[i * 3 + 0]);
                float d1 = std::abs(mesh.verts[vid * 3 + 1] - d.nextVerts[i * 3 + 1]);
                float d2 = std::abs(mesh.verts[vid * 3 + 2] - d.nextVerts[i * 3 + 2]);
                float m = std::max({d0, d1, d2});
                if (m > maxRedoDiff) maxRedoDiff = m;
            }
        }

        // Reset mesh back to original
        g_undoManager.undo(scene);
        g_undoManager.clear();
        mesh.vertProxy = mesh.verts;

        MultiToolBenchmarkRow row;
        row.toolName = "Move Tool (Drag 10x)";
        row.hasStep0 = true;
        row.step0Ms = step0Ms;
        row.hasDrag = true;
        row.dragAvgMs = dragAvgMs;
        row.finalizeMs = finalizeMs;
        row.totalStrokeMs = totalStrokeMs;
        row.undoMs = undoMs;
        row.redoMs = redoMs;
        row.undoRAMBytes = undoBytes;
        row.maxUndoEps = maxUndoDiff;
        row.maxRedoEps = maxRedoDiff;
        row.passed = (maxUndoDiff == 0.0f && maxRedoDiff == 0.0f);
        summaryRows.push_back(row);
    }

    // =========================================================================
    // 3. Tool: Mask Brush (Stroke 10x)
    // =========================================================================
    {
        std::cout << "  -> Benchmarking Mask Brush (Stroke 10x)...\n";
        sculpt.setBrush(BRUSH_MASK);
        auto& maskSettings = sculpt.getSettings(BRUSH_MASK);
        maskSettings.radius = 150.0f;
        maskSettings.intensity = 1.0f;
        maskSettings.culling = true;

        sculpt.warmupMeshCaches(meshRaw);

        // Step 0: Mouse Down
        SDL_Event evDown;
        std::memset(&evDown, 0, sizeof(evDown));
        evDown.type = SDL_MOUSEBUTTONDOWN;
        evDown.button.button = SDL_BUTTON_LEFT;
        evDown.button.x = (int)startX;
        evDown.button.y = (int)startY;

        auto tStep0Start = std::chrono::high_resolution_clock::now();
        sculpt.handleEvent(evDown, scene);
        auto tStep0End = std::chrono::high_resolution_clock::now();
        double step0Ms = std::chrono::duration<double, std::milli>(tStep0End - tStep0Start).count();

        // Continuous Drag (Steps 1..9)
        double sumDragMs = 0.0;
        for (int step = 1; step < NUM_STROKE_STEPS; ++step) {
            float curX = startX + step * stepDx;
            float curY = startY + step * stepDy;

            auto tStepStart = std::chrono::high_resolution_clock::now();
            sculpt.executeStroke(scene, meshRaw, camera, curX, curY, 1.0f);
            auto tStepEnd = std::chrono::high_resolution_clock::now();
            sumDragMs += std::chrono::duration<double, std::milli>(tStepEnd - tStepStart).count();
        }
        double dragAvgMs = sumDragMs / (NUM_STROKE_STEPS - 1);

        // Finalize: Mouse Up
        SDL_Event evUp;
        std::memset(&evUp, 0, sizeof(evUp));
        evUp.type = SDL_MOUSEBUTTONUP;
        evUp.button.button = SDL_BUTTON_LEFT;
        evUp.button.x = (int)endX;
        evUp.button.y = (int)endY;

        auto tFinStart = std::chrono::high_resolution_clock::now();
        sculpt.handleEvent(evUp, scene);
        auto tFinEnd = std::chrono::high_resolution_clock::now();
        double finalizeMs = std::chrono::duration<double, std::milli>(tFinEnd - tFinStart).count();

        double totalStrokeMs = step0Ms + sumDragMs + finalizeMs;
        size_t undoBytes = !g_undoManager.getUndoStack().empty() ? g_undoManager.getUndoStack().back()->getMemoryUsage() : 0;

        // Undo
        auto tUndoStart = std::chrono::high_resolution_clock::now();
        g_undoManager.undo(scene);
        auto tUndoEnd = std::chrono::high_resolution_clock::now();
        double undoMs = std::chrono::duration<double, std::milli>(tUndoEnd - tUndoStart).count();

        float maxUndoDiff = 0.0f;
        auto* redoEntry = !g_undoManager.getRedoStack().empty()
            ? dynamic_cast<SculptUndoEntry*>(g_undoManager.getRedoStack().back().get())
            : nullptr;
        if (redoEntry && !redoEntry->deltas.empty()) {
            const auto& d = redoEntry->deltas[0];
            size_t n = d.indices.size();
#pragma omp parallel for reduction(max:maxUndoDiff) schedule(static) if(n > 10000)
            for (size_t i = 0; i < n; ++i) {
                uint32_t vid = d.indices[i];
                float d0 = std::abs(mesh.materials[vid * 3 + 0] - d.prevMaterials[i * 3 + 0]);
                float d1 = std::abs(mesh.materials[vid * 3 + 1] - d.prevMaterials[i * 3 + 1]);
                float d2 = std::abs(mesh.materials[vid * 3 + 2] - d.prevMaterials[i * 3 + 2]);
                float m = std::max({d0, d1, d2});
                if (m > maxUndoDiff) maxUndoDiff = m;
            }
        }

        // Redo
        auto tRedoStart = std::chrono::high_resolution_clock::now();
        g_undoManager.redo(scene);
        auto tRedoEnd = std::chrono::high_resolution_clock::now();
        double redoMs = std::chrono::duration<double, std::milli>(tRedoEnd - tRedoStart).count();

        float maxRedoDiff = 0.0f;
        auto* undoEntryAfterRedo = !g_undoManager.getUndoStack().empty()
            ? dynamic_cast<SculptUndoEntry*>(g_undoManager.getUndoStack().back().get())
            : nullptr;
        if (undoEntryAfterRedo && !undoEntryAfterRedo->deltas.empty()) {
            const auto& d = undoEntryAfterRedo->deltas[0];
            size_t n = d.indices.size();
#pragma omp parallel for reduction(max:maxRedoDiff) schedule(static) if(n > 10000)
            for (size_t i = 0; i < n; ++i) {
                uint32_t vid = d.indices[i];
                float d0 = std::abs(mesh.materials[vid * 3 + 0] - d.nextMaterials[i * 3 + 0]);
                float d1 = std::abs(mesh.materials[vid * 3 + 1] - d.nextMaterials[i * 3 + 1]);
                float d2 = std::abs(mesh.materials[vid * 3 + 2] - d.nextMaterials[i * 3 + 2]);
                float m = std::max({d0, d1, d2});
                if (m > maxRedoDiff) maxRedoDiff = m;
            }
        }

        // Reset mesh back to original
        g_undoManager.undo(scene);
        g_undoManager.clear();

        MultiToolBenchmarkRow row;
        row.toolName = "Mask Brush (Stroke 10x)";
        row.hasStep0 = true;
        row.step0Ms = step0Ms;
        row.hasDrag = true;
        row.dragAvgMs = dragAvgMs;
        row.finalizeMs = finalizeMs;
        row.totalStrokeMs = totalStrokeMs;
        row.undoMs = undoMs;
        row.redoMs = redoMs;
        row.undoRAMBytes = undoBytes;
        row.maxUndoEps = maxUndoDiff;
        row.maxRedoEps = maxRedoDiff;
        row.passed = (maxUndoDiff == 0.0f && maxRedoDiff == 0.0f);
        summaryRows.push_back(row);
    }

    // =========================================================================
    // 4. Tool: Mask Lasso (Screen Poly)
    // =========================================================================
    {
        std::cout << "  -> Benchmarking Mask Lasso (Screen Poly)...\n";

        std::vector<glm::vec2> lassoPoly = {
            { 960.0f, 540.0f },
            { 960.0f, 250.0f },
            { 1100.0f, 200.0f },
            { 1280.0f, 240.0f },
            { 1420.0f, 350.0f },
            { 1480.0f, 500.0f },
            { 1450.0f, 650.0f },
            { 1350.0f, 750.0f },
            { 1200.0f, 800.0f },
            { 1050.0f, 750.0f },
            { 980.0f, 660.0f },
            { 960.0f, 540.0f }
        };
        sculpt.setLassoPoints(lassoPoly);
        sculpt.setIsMaskLasso(true);
        sculpt.setLassoAlt(false);

        auto tLassoStart = std::chrono::high_resolution_clock::now();

        // 1. Vertex projection & point-in-polygon test
        std::vector<uint32_t> selectedVertices = sculpt.getVerticesInLasso(meshRaw, camera);

        // 2. Delta undo recording
        g_undoManager.beginSculptStroke(scene, mesh.getID(), selectedVertices, false, false, true, "Mask Lasso");

        // 3. Batch write mask
        float* materials = mesh.materials.data();
#pragma omp parallel for schedule(static) if(selectedVertices.size() > 1000)
        for (size_t i = 0; i < selectedVertices.size(); ++i) {
            uint32_t vid = selectedVertices[i];
            materials[vid * 3 + 2] = 0.0f; // Masked
        }
        mesh.isMaterialDirty = true;
        mesh.dirtyVertMin = 0;
        mesh.dirtyVertMax = mesh.nbVerts - 1;
        mesh.isDirty = true;

        g_undoManager.endSculptStroke(scene);

        auto tLassoEnd = std::chrono::high_resolution_clock::now();
        double finalizeMs = std::chrono::duration<double, std::milli>(tLassoEnd - tLassoStart).count();
        double totalStrokeMs = finalizeMs;

        size_t undoBytes = !g_undoManager.getUndoStack().empty() ? g_undoManager.getUndoStack().back()->getMemoryUsage() : 0;

        // Undo
        auto tUndoStart = std::chrono::high_resolution_clock::now();
        g_undoManager.undo(scene);
        auto tUndoEnd = std::chrono::high_resolution_clock::now();
        double undoMs = std::chrono::duration<double, std::milli>(tUndoEnd - tUndoStart).count();

        float maxUndoDiff = 0.0f;
        auto* redoEntry = !g_undoManager.getRedoStack().empty()
            ? dynamic_cast<SculptUndoEntry*>(g_undoManager.getRedoStack().back().get())
            : nullptr;
        if (redoEntry && !redoEntry->deltas.empty()) {
            const auto& d = redoEntry->deltas[0];
            size_t n = d.indices.size();
#pragma omp parallel for reduction(max:maxUndoDiff) schedule(static) if(n > 10000)
            for (size_t i = 0; i < n; ++i) {
                uint32_t vid = d.indices[i];
                float d0 = std::abs(mesh.materials[vid * 3 + 0] - d.prevMaterials[i * 3 + 0]);
                float d1 = std::abs(mesh.materials[vid * 3 + 1] - d.prevMaterials[i * 3 + 1]);
                float d2 = std::abs(mesh.materials[vid * 3 + 2] - d.prevMaterials[i * 3 + 2]);
                float m = std::max({d0, d1, d2});
                if (m > maxUndoDiff) maxUndoDiff = m;
            }
        }

        // Redo
        auto tRedoStart = std::chrono::high_resolution_clock::now();
        g_undoManager.redo(scene);
        auto tRedoEnd = std::chrono::high_resolution_clock::now();
        double redoMs = std::chrono::duration<double, std::milli>(tRedoEnd - tRedoStart).count();

        float maxRedoDiff = 0.0f;
        auto* undoEntryAfterRedo = !g_undoManager.getUndoStack().empty()
            ? dynamic_cast<SculptUndoEntry*>(g_undoManager.getUndoStack().back().get())
            : nullptr;
        if (undoEntryAfterRedo && !undoEntryAfterRedo->deltas.empty()) {
            const auto& d = undoEntryAfterRedo->deltas[0];
            size_t n = d.indices.size();
#pragma omp parallel for reduction(max:maxRedoDiff) schedule(static) if(n > 10000)
            for (size_t i = 0; i < n; ++i) {
                uint32_t vid = d.indices[i];
                float d0 = std::abs(mesh.materials[vid * 3 + 0] - d.nextMaterials[i * 3 + 0]);
                float d1 = std::abs(mesh.materials[vid * 3 + 1] - d.nextMaterials[i * 3 + 1]);
                float d2 = std::abs(mesh.materials[vid * 3 + 2] - d.nextMaterials[i * 3 + 2]);
                float m = std::max({d0, d1, d2});
                if (m > maxRedoDiff) maxRedoDiff = m;
            }
        }

        // Reset mesh back to original
        g_undoManager.undo(scene);
        g_undoManager.clear();

        MultiToolBenchmarkRow row;
        row.toolName = "Mask Lasso (Screen Poly)";
        row.hasStep0 = false;
        row.step0Ms = 0.0;
        row.hasDrag = false;
        row.dragAvgMs = 0.0;
        row.finalizeMs = finalizeMs;
        row.totalStrokeMs = totalStrokeMs;
        row.undoMs = undoMs;
        row.redoMs = redoMs;
        row.undoRAMBytes = undoBytes;
        row.maxUndoEps = maxUndoDiff;
        row.maxRedoEps = maxRedoDiff;
        row.passed = (maxUndoDiff == 0.0f && maxRedoDiff == 0.0f);
        summaryRows.push_back(row);
    }

    // =========================================================================
    // 5. Tool: Transform (Pure Matrix)
    // =========================================================================
    {
        std::cout << "  -> Benchmarking Transform Tool (Pure Matrix)...\n";
        glm::mat4 startMatrix = mesh.matrix;

        // Step 0: Check mask (fast check)
        auto tInitStart = std::chrono::high_resolution_clock::now();
        glm::mat4 pivotStartMatrix = mesh.matrix;
        bool transformHasMask = false;
        for (int i = 0; i < mesh.nbVerts; ++i) {
            if (mesh.materials[i * 3 + 2] < 0.999f) {
                transformHasMask = true;
                break;
            }
        }
        auto tInitEnd = std::chrono::high_resolution_clock::now();
        double step0Ms = std::chrono::duration<double, std::milli>(tInitEnd - tInitStart).count();

        // Drag (10 steps pure matrix update)
        double sumDragMs = 0.0;
        for (int step = 1; step <= 10; ++step) {
            auto tStepStart = std::chrono::high_resolution_clock::now();
            glm::mat4 curMatrix = glm::translate(pivotStartMatrix, glm::vec3(0.05f * step, 0.02f * step, 0.0f));
            mesh.matrix = curMatrix;
            auto tStepEnd = std::chrono::high_resolution_clock::now();
            sumDragMs += std::chrono::duration<double, std::milli>(tStepEnd - tStepStart).count();
        }
        double dragAvgMs = sumDragMs / 10.0;

        // Finalize: bakeScale + pushTransformChange
        auto tFinStart = std::chrono::high_resolution_clock::now();
        TransformUndoEntry undoEntry("Transform Tool");
        TransformMeshState meshUndoState;
        meshUndoState.meshId = mesh.getID();
        meshUndoState.beforeMatrix = pivotStartMatrix;

        float sx = glm::length(glm::vec3(mesh.matrix[0]));
        float sy = glm::length(glm::vec3(mesh.matrix[1]));
        float sz = glm::length(glm::vec3(mesh.matrix[2]));

        mesh.bakeScale();
        if (std::abs(sx - 1.0f) > 1e-4f || std::abs(sy - 1.0f) > 1e-4f || std::abs(sz - 1.0f) > 1e-4f) {
            meshUndoState.bakedScale = true;
            meshUndoState.scaleX = sx;
            meshUndoState.scaleY = sy;
            meshUndoState.scaleZ = sz;
        }
        meshUndoState.afterMatrix = mesh.matrix;
        undoEntry.m_meshStates.push_back(std::move(meshUndoState));
        g_undoManager.pushTransformChange(scene, std::move(undoEntry));
        auto tFinEnd = std::chrono::high_resolution_clock::now();
        double finalizeMs = std::chrono::duration<double, std::milli>(tFinEnd - tFinStart).count();

        double totalStrokeMs = step0Ms + sumDragMs + finalizeMs;
        size_t undoBytes = !g_undoManager.getUndoStack().empty() ? g_undoManager.getUndoStack().back()->getMemoryUsage() : 0;
        glm::mat4 modifiedMatrix = mesh.matrix;

        // Undo
        auto tUndoStart = std::chrono::high_resolution_clock::now();
        g_undoManager.undo(scene);
        auto tUndoEnd = std::chrono::high_resolution_clock::now();
        double undoMs = std::chrono::duration<double, std::milli>(tUndoEnd - tUndoStart).count();

        float maxUndoDiff = 0.0f;
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                float d = std::abs(mesh.matrix[c][r] - startMatrix[c][r]);
                if (d > maxUndoDiff) maxUndoDiff = d;
            }
        }

        // Redo
        auto tRedoStart = std::chrono::high_resolution_clock::now();
        g_undoManager.redo(scene);
        auto tRedoEnd = std::chrono::high_resolution_clock::now();
        double redoMs = std::chrono::duration<double, std::milli>(tRedoEnd - tRedoStart).count();

        float maxRedoDiff = 0.0f;
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                float d = std::abs(mesh.matrix[c][r] - modifiedMatrix[c][r]);
                if (d > maxRedoDiff) maxRedoDiff = d;
            }
        }

        // Reset back to original matrix
        g_undoManager.undo(scene);
        g_undoManager.clear();

        MultiToolBenchmarkRow row;
        row.toolName = "Transform (Pure Matrix)";
        row.hasStep0 = true;
        row.step0Ms = step0Ms;
        row.hasDrag = true;
        row.dragAvgMs = dragAvgMs;
        row.finalizeMs = finalizeMs;
        row.totalStrokeMs = totalStrokeMs;
        row.undoMs = undoMs;
        row.redoMs = redoMs;
        row.undoRAMBytes = undoBytes;
        row.maxUndoEps = maxUndoDiff;
        row.maxRedoEps = maxRedoDiff;
        row.passed = (maxUndoDiff < 1e-6f && maxRedoDiff < 1e-6f);
        summaryRows.push_back(row);
    }

    // =========================================================================
    // 6. Tool: Transform (Masked Deform)
    // =========================================================================
    {
        std::cout << "  -> Benchmarking Transform Tool (Masked Deform)...\n";
        // Pre-apply mask: vertices with X <= 0 are masked (m = 0.0), X > 0 unmasked (m = 1.0)
        for (int i = 0; i < mesh.nbVerts; ++i) {
            mesh.materials[i * 3 + 2] = (mesh.verts[i * 3] > 0.0f) ? 1.0f : 0.0f;
        }
        mesh.isMaterialDirty = true;

        // Step 0: Check mask + Buffer copy (transformStartVerts)
        auto tInitStart = std::chrono::high_resolution_clock::now();
        glm::mat4 pivotStartMatrix = mesh.matrix;
        bool transformHasMask = true;
        std::vector<float> transformStartVerts = mesh.verts;
        auto tInitEnd = std::chrono::high_resolution_clock::now();
        double step0Ms = std::chrono::duration<double, std::milli>(tInitEnd - tInitStart).count();

        // Drag (10 steps vertex deformation)
        double sumDragMs = 0.0;
        int nbVerts = mesh.nbVerts;
        for (int step = 1; step <= 10; ++step) {
            glm::mat4 curMatrix = glm::translate(pivotStartMatrix, glm::vec3(0.02f * step, 0.01f * step, 0.0f));
            glm::mat4 deltaLocal = glm::inverse(pivotStartMatrix) * curMatrix;

            auto tDragStart = std::chrono::high_resolution_clock::now();
#pragma omp parallel for schedule(static) if(nbVerts > 2000)
            for (int i = 0; i < nbVerts; ++i) {
                float m = mesh.materials[i * 3 + 2];
                if (m <= 0.001f) {
                    mesh.verts[i * 3]     = transformStartVerts[i * 3];
                    mesh.verts[i * 3 + 1] = transformStartVerts[i * 3 + 1];
                    mesh.verts[i * 3 + 2] = transformStartVerts[i * 3 + 2];
                    continue;
                }
                glm::vec4 vStart(transformStartVerts[i * 3], transformStartVerts[i * 3 + 1], transformStartVerts[i * 3 + 2], 1.0f);
                glm::vec4 vTransformed = deltaLocal * vStart;
                glm::vec4 vNew = m * vTransformed + (1.0f - m) * vStart;
                mesh.verts[i * 3]     = vNew.x;
                mesh.verts[i * 3 + 1] = vNew.y;
                mesh.verts[i * 3 + 2] = vNew.z;
            }
            mesh.isVertexDirty = true;
            auto tDragEnd = std::chrono::high_resolution_clock::now();
            sumDragMs += std::chrono::duration<double, std::milli>(tDragEnd - tDragStart).count();
        }
        double dragAvgMs = sumDragMs / 10.0;

        auto tFinStart = std::chrono::high_resolution_clock::now();
        mesh.matrix = pivotStartMatrix;
        mesh.vertProxy = mesh.verts;
        mesh.invalidateLocalRadius();
        mesh.bumpVertsGeneration();

        TransformUndoEntry undoEntry("Transform Tool");
        TransformMeshState meshUndoState;
        meshUndoState.meshId = mesh.getID();
        meshUndoState.beforeMatrix = pivotStartMatrix;
        meshUndoState.afterMatrix = mesh.matrix;
        meshUndoState.hasVertexDeformation = true;
        meshUndoState.isSparse = true;

        // Detect deformed vertices in parallel
        int maxThreads = omp_get_max_threads();
        std::vector<std::vector<uint32_t>> threadVerts(maxThreads);
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            #pragma omp for schedule(static)
            for (int i = 0; i < nbVerts; ++i) {
                float dx = mesh.verts[i * 3]     - transformStartVerts[i * 3];
                float dy = mesh.verts[i * 3 + 1] - transformStartVerts[i * 3 + 1];
                float dz = mesh.verts[i * 3 + 2] - transformStartVerts[i * 3 + 2];
                if (dx != 0.0f || dy != 0.0f || dz != 0.0f) {
                    threadVerts[tid].push_back((uint32_t)i);
                }
            }
        }

        size_t totalDeformed = 0;
        for (int t = 0; t < maxThreads; ++t) totalDeformed += threadVerts[t].size();
        meshUndoState.sparseVertIndices.resize(totalDeformed);
        size_t curOffset = 0;
        for (int t = 0; t < maxThreads; ++t) {
            if (!threadVerts[t].empty()) {
                std::memcpy(meshUndoState.sparseVertIndices.data() + curOffset, threadVerts[t].data(), threadVerts[t].size() * sizeof(uint32_t));
                curOffset += threadVerts[t].size();
            }
        }

        meshUndoState.sparseBeforeCoords.resize(totalDeformed * 3);
        meshUndoState.sparseAfterCoords.resize(totalDeformed * 3);
        const float* startVertsPtr = transformStartVerts.data();
        const float* curVertsPtr = mesh.verts.data();
        const uint32_t* defIndices = meshUndoState.sparseVertIndices.data();
        float* sBefore = meshUndoState.sparseBeforeCoords.data();
        float* sAfter = meshUndoState.sparseAfterCoords.data();

        #pragma omp parallel for schedule(static) if(totalDeformed > 2000)
        for (int64_t i = 0; i < (int64_t)totalDeformed; ++i) {
            uint32_t v = defIndices[i];
            uint32_t v3 = v * 3;
            uint32_t i3 = (uint32_t)(i * 3);
            sBefore[i3 + 0] = startVertsPtr[v3 + 0];
            sBefore[i3 + 1] = startVertsPtr[v3 + 1];
            sBefore[i3 + 2] = startVertsPtr[v3 + 2];
            sAfter[i3 + 0]  = curVertsPtr[v3 + 0];
            sAfter[i3 + 1]  = curVertsPtr[v3 + 1];
            sAfter[i3 + 2]  = curVertsPtr[v3 + 2];
        }

        // Incremental face lookup for affected faces
        if (!mesh.vrfStartCount.empty() && !mesh.vertRingFace.empty() && mesh.nbFaces > 0) {
            std::vector<uint32_t> tempFaces(mesh.nbFaces);
            std::vector<uint32_t> tagFlags(mesh.nbFaces, 0);
            uint32_t tagEpoch = 1;
            uint32_t numIFaces = getFacesFromVerticesFast(
                meshUndoState.sparseVertIndices.data(),
                (uint32_t)meshUndoState.sparseVertIndices.size(),
                mesh.vrfStartCount.data(),
                mesh.vertRingFace.data(),
                tempFaces.data(),
                tagFlags.data(),
                &tagEpoch,
                mesh.nbFaces
            );
            meshUndoState.affectedFaces.assign(tempFaces.begin(), tempFaces.begin() + numIFaces);
        }

        auto tFNStart = std::chrono::high_resolution_clock::now();
        if (!meshUndoState.affectedFaces.empty() && !mesh.faceNormals.empty() && !mesh.faceBoxes.empty() && !mesh.faceCenters.empty()) {
            updateFaceNormalsAndBoxes(
                mesh.verts.data(), mesh.nbVerts,
                mesh.faces.data(), mesh.nbFaces,
                meshUndoState.affectedFaces.data(), (int)meshUndoState.affectedFaces.size(),
                mesh.faceNormals.data(),
                mesh.faceBoxes.data(),
                mesh.faceCenters.data()
            );
        }
        auto tVNStart = std::chrono::high_resolution_clock::now();
        if (!meshUndoState.sparseVertIndices.empty() && !mesh.normals.empty() && !mesh.vrfStartCount.empty() && !mesh.vertRingFace.empty()) {
            updateVertexNormals(
                meshUndoState.sparseVertIndices.data(), (int)meshUndoState.sparseVertIndices.size(), mesh.nbVerts,
                mesh.vrfStartCount.data(),
                mesh.vertRingFace.data(),
                mesh.faceNormals.data(),
                mesh.normals.data()
            );
        }
        auto tOctStart = std::chrono::high_resolution_clock::now();
        if (!meshUndoState.affectedFaces.empty()) {
            mesh.octree.update(
                mesh.verts.data(), mesh.nbVerts,
                mesh.faces.data(), mesh.nbFaces,
                mesh.faceBoxes.data(),
                meshUndoState.affectedFaces.data(), (int)meshUndoState.affectedFaces.size()
            );
        }
        auto tUndoPrepStart = std::chrono::high_resolution_clock::now();

        undoEntry.m_meshStates.push_back(std::move(meshUndoState));
        g_undoManager.pushTransformChange(scene, std::move(undoEntry));
        auto tFinEnd = std::chrono::high_resolution_clock::now();
        double finalizeMs = std::chrono::duration<double, std::milli>(tFinEnd - tFinStart).count();
        double fnMs = std::chrono::duration<double, std::milli>(tVNStart - tFNStart).count();
        double vnMs = std::chrono::duration<double, std::milli>(tOctStart - tVNStart).count();
        double octMs = std::chrono::duration<double, std::milli>(tUndoPrepStart - tOctStart).count();
        double undoPushMs = std::chrono::duration<double, std::milli>(tFinEnd - tUndoPrepStart).count();
        std::cout << "      [Transform Masked Finalize Detail] Total: " << std::fixed << std::setprecision(2) << finalizeMs
                  << " ms (FaceNorms: " << fnMs << " ms, VertNorms: " << vnMs
                  << " ms, Octree: " << octMs << " ms, UndoPush: " << undoPushMs << " ms)\n";

        double totalStrokeMs = step0Ms + sumDragMs + finalizeMs;
        size_t undoBytes = !g_undoManager.getUndoStack().empty() ? g_undoManager.getUndoStack().back()->getMemoryUsage() : 0;

        // Undo
        auto tUndoStart = std::chrono::high_resolution_clock::now();
        g_undoManager.undo(scene);
        auto tUndoEnd = std::chrono::high_resolution_clock::now();
        double undoMs = std::chrono::duration<double, std::milli>(tUndoEnd - tUndoStart).count();

        // Validate Undo against beforeVerts or sparseBeforeCoords stored in the redo entry
        float maxUndoDiff = 0.0f;
        auto* redoEntry = !g_undoManager.getRedoStack().empty()
            ? dynamic_cast<TransformUndoEntry*>(g_undoManager.getRedoStack().back().get())
            : nullptr;
        if (redoEntry && !redoEntry->m_meshStates.empty()) {
            const auto& state = redoEntry->m_meshStates[0];
            if (state.isSparse) {
                size_t n = state.sparseVertIndices.size();
#pragma omp parallel for reduction(max:maxUndoDiff) schedule(static) if(n > 10000)
                for (size_t i = 0; i < n; ++i) {
                    uint32_t vid = state.sparseVertIndices[i];
                    float d0 = std::abs(mesh.verts[vid * 3 + 0] - state.sparseBeforeCoords[i * 3 + 0]);
                    float d1 = std::abs(mesh.verts[vid * 3 + 1] - state.sparseBeforeCoords[i * 3 + 1]);
                    float d2 = std::abs(mesh.verts[vid * 3 + 2] - state.sparseBeforeCoords[i * 3 + 2]);
                    float m = std::max({d0, d1, d2});
                    if (m > maxUndoDiff) maxUndoDiff = m;
                }
            } else {
                const auto& bVerts = state.beforeVerts;
                size_t n = mesh.verts.size();
#pragma omp parallel for reduction(max:maxUndoDiff) schedule(static) if(n > 10000)
                for (size_t i = 0; i < n; ++i) {
                    float d = std::abs(mesh.verts[i] - bVerts[i]);
                    if (d > maxUndoDiff) maxUndoDiff = d;
                }
            }
        }

        // Redo
        auto tRedoStart = std::chrono::high_resolution_clock::now();
        g_undoManager.redo(scene);
        auto tRedoEnd = std::chrono::high_resolution_clock::now();
        double redoMs = std::chrono::duration<double, std::milli>(tRedoEnd - tRedoStart).count();

        // Validate Redo against afterVerts or sparseAfterCoords stored in the undo entry
        float maxRedoDiff = 0.0f;
        auto* undoEntryAfterRedo = !g_undoManager.getUndoStack().empty()
            ? dynamic_cast<TransformUndoEntry*>(g_undoManager.getUndoStack().back().get())
            : nullptr;
        if (undoEntryAfterRedo && !undoEntryAfterRedo->m_meshStates.empty()) {
            const auto& state = undoEntryAfterRedo->m_meshStates[0];
            if (state.isSparse) {
                size_t n = state.sparseVertIndices.size();
#pragma omp parallel for reduction(max:maxRedoDiff) schedule(static) if(n > 10000)
                for (size_t i = 0; i < n; ++i) {
                    uint32_t vid = state.sparseVertIndices[i];
                    float d0 = std::abs(mesh.verts[vid * 3 + 0] - state.sparseAfterCoords[i * 3 + 0]);
                    float d1 = std::abs(mesh.verts[vid * 3 + 1] - state.sparseAfterCoords[i * 3 + 1]);
                    float d2 = std::abs(mesh.verts[vid * 3 + 2] - state.sparseAfterCoords[i * 3 + 2]);
                    float m = std::max({d0, d1, d2});
                    if (m > maxRedoDiff) maxRedoDiff = m;
                }
            } else {
                const auto& aVerts = state.afterVerts;
                size_t n = mesh.verts.size();
#pragma omp parallel for reduction(max:maxRedoDiff) schedule(static) if(n > 10000)
                for (size_t i = 0; i < n; ++i) {
                    float d = std::abs(mesh.verts[i] - aVerts[i]);
                    if (d > maxRedoDiff) maxRedoDiff = d;
                }
            }
        }

        // Reset mesh back to original
        g_undoManager.undo(scene);
        g_undoManager.clear();
        for (size_t i = 0; i < mesh.materials.size(); i += 3) {
            mesh.materials[i + 2] = 1.0f;
        }
        mesh.isMaterialDirty = true;

        MultiToolBenchmarkRow row;
        row.toolName = "Transform (Masked Deform)";
        row.hasStep0 = true;
        row.step0Ms = step0Ms;
        row.hasDrag = true;
        row.dragAvgMs = dragAvgMs;
        row.finalizeMs = finalizeMs;
        row.totalStrokeMs = totalStrokeMs;
        row.undoMs = undoMs;
        row.redoMs = redoMs;
        row.undoRAMBytes = undoBytes;
        row.maxUndoEps = maxUndoDiff;
        row.maxRedoEps = maxRedoDiff;
        row.passed = (maxUndoDiff == 0.0f && maxRedoDiff == 0.0f);
        summaryRows.push_back(row);
    }

    scene.removeMesh(meshRaw);
    g_undoManager.clear();

    // -------------------------------------------------------------------------
    // Detailed ClayBuildup Diagnostic & Amdahl's Law Breakdown
    // -------------------------------------------------------------------------
    if (!clayMeasurements.empty()) {
        std::cout << "\n------------------------------------------------------------------------------------------------------------------------------------------------------------------------\n";
        std::cout << " Step | Total CPU | Raycast | PickVerts | Deform+Area | Symmetry | SortDedup | UndoRec | FaceLookup | FaceNorm | VertNorm | SpatialUpd | PickedV | Faces \n";
        std::cout << "------------------------------------------------------------------------------------------------------------------------------------------------------------------------\n";

        double sumTotal = 0.0, sumRay = 0.0, sumPick = 0.0, sumDeform = 0.0, sumUndo = 0.0;
        double sumSymmetry = 0.0, sumSortDedup = 0.0;
        double sumPickCells = 0.0, sumPickDedup = 0.0, sumPickFilter = 0.0;
        double sumFaceLookup = 0.0, sumFaceNorm = 0.0, sumVertNorm = 0.0, sumSpatialUpdate = 0.0;
        int sumVerts = 0, sumFaces = 0;

        for (const auto& m : clayMeasurements) {
            std::cout << "  #" << std::setw(2) << m.stepIndex << " |"
                      << std::setw(8) << std::fixed << std::setprecision(2) << m.totalMs << "ms |"
                      << std::setw(6) << std::fixed << std::setprecision(2) << m.raycastMs << "ms |"
                      << std::setw(8) << std::fixed << std::setprecision(2) << m.pickVertsMs << "ms |"
                      << std::setw(10) << std::fixed << std::setprecision(2) << (m.primaryDeformMs + m.areaComputeMs) << "ms |"
                      << std::setw(7) << std::fixed << std::setprecision(2) << m.symmetryMs << "ms |"
                      << std::setw(8) << std::fixed << std::setprecision(2) << m.sortDedupMs << "ms |"
                      << std::setw(6) << std::fixed << std::setprecision(2) << m.undoRecordMs << "ms |"
                      << std::setw(9) << std::fixed << std::setprecision(2) << m.faceLookupMs << "ms |"
                      << std::setw(7) << std::fixed << std::setprecision(2) << m.faceNormalsMs << "ms |"
                      << std::setw(7) << std::fixed << std::setprecision(2) << m.vertNormalsMs << "ms |"
                      << std::setw(9) << std::fixed << std::setprecision(2) << m.spatialUpdateMs << "ms |"
                      << std::setw(7) << m.pickedVerts << " |"
                      << std::setw(6) << m.affectedFaces << "\n";

            sumTotal += m.totalMs;
            sumRay += m.raycastMs;
            sumPick += m.pickVertsMs;
            sumPickCells += m.pickCellsMs;
            sumPickDedup += m.pickDedupMs;
            sumPickFilter += m.pickFilterMs;
            sumDeform += (m.primaryDeformMs + m.areaComputeMs);
            sumSymmetry += m.symmetryMs;
            sumSortDedup += m.sortDedupMs;
            sumUndo += m.undoRecordMs;
            sumFaceLookup += m.faceLookupMs;
            sumFaceNorm += m.faceNormalsMs;
            sumVertNorm += m.vertNormalsMs;
            sumSpatialUpdate += m.spatialUpdateMs;
            sumVerts += m.pickedVerts;
            sumFaces += m.affectedFaces;
        }
        std::cout << "------------------------------------------------------------------------------------------------------------------------------------------------------------------------\n";

        double nSteps = (double)clayMeasurements.size();
        double avgTotal = sumTotal / nSteps;
        double avgRay = sumRay / nSteps;
        double avgPick = sumPick / nSteps;
        double avgDeform = sumDeform / nSteps;
        double avgSymmetry = sumSymmetry / nSteps;
        double avgSortDedup = sumSortDedup / nSteps;
        double avgUndo = sumUndo / nSteps;
        double avgFaceLookup = sumFaceLookup / nSteps;
        double avgFaceNorm = sumFaceNorm / nSteps;
        double avgVertNorm = sumVertNorm / nSteps;
        double avgSpatialUpdate = sumSpatialUpdate / nSteps;
        double avgVerts = (double)sumVerts / nSteps;
        double avgFaces = (double)sumFaces / nSteps;

        std::cout << "  AVG |"
                  << std::setw(8) << std::fixed << std::setprecision(2) << avgTotal << "ms |"
                  << std::setw(6) << std::fixed << std::setprecision(2) << avgRay << "ms |"
                  << std::setw(8) << std::fixed << std::setprecision(2) << avgPick << "ms |"
                  << std::setw(10) << std::fixed << std::setprecision(2) << avgDeform << "ms |"
                  << std::setw(7) << std::fixed << std::setprecision(2) << avgSymmetry << "ms |"
                  << std::setw(8) << std::fixed << std::setprecision(2) << avgSortDedup << "ms |"
                  << std::setw(6) << std::fixed << std::setprecision(2) << avgUndo << "ms |"
                  << std::setw(9) << std::fixed << std::setprecision(2) << avgFaceLookup << "ms |"
                  << std::setw(7) << std::fixed << std::setprecision(2) << avgFaceNorm << "ms |"
                  << std::setw(7) << std::fixed << std::setprecision(2) << avgVertNorm << "ms |"
                  << std::setw(9) << std::fixed << std::setprecision(2) << avgSpatialUpdate << "ms |"
                  << std::setw(7) << (int)avgVerts << " |"
                  << std::setw(6) << (int)avgFaces << "\n";
        std::cout << "------------------------------------------------------------------------------------------------------------------------------------------------------------------------\n";
    }

    // =========================================================================
    // GRAND SUMMARY TABLE: SPSculpt Multi-Tool Performance & Undo Benchmark
    // =========================================================================
    std::cout << "\n===========================================================================================================\n";
    std::cout << " SPSculpt Multi-Tool Performance & Undo Benchmark @ " << std::fixed << std::setprecision(1) << (targetFaces / 1000000.0) << "M Polygons\n";
    std::cout << "===========================================================================================================\n";
    std::cout << " Tool / Operation          | Step 0 (Init) | Drag Avg | Finalize | Total Stroke | Undo Time | Redo Time | Undo RAM | Status \n";
    std::cout << "---------------------------+---------------+----------+----------+--------------+-----------+-----------+----------+--------\n";

    bool allIntegrityPassed = true;
    for (const auto& r : summaryRows) {
        if (!r.passed) allIntegrityPassed = false;

        std::string toolCell = padRight(" " + r.toolName, 27);
        std::string step0Cell = r.hasStep0 ? padLeft(formatMs(r.step0Ms) + "  ", 15) : padLeft("-     ", 15);
        std::string dragCell  = r.hasDrag  ? padLeft(formatMs(r.dragAvgMs) + " ", 10)  : padLeft("-     ", 10);
        std::string finCell   = padLeft(formatMs(r.finalizeMs) + " ", 10);
        std::string totalCell = padLeft(formatMs(r.totalStrokeMs) + " ", 14);
        std::string undoCell  = padLeft(formatMs(r.undoMs) + " ", 11);
        std::string redoCell  = padLeft(formatMs(r.redoMs) + " ", 11);
        std::string ramCell   = padLeft(formatBytes(r.undoRAMBytes) + " ", 10);
        std::string statusCell = padRight(" " + std::string(r.passed ? "PASSED" : "FAILED"), 8);

        std::cout << toolCell << "|"
                  << step0Cell << "|"
                  << dragCell << "|"
                  << finCell << "|"
                  << totalCell << "|"
                  << undoCell << "|"
                  << redoCell << "|"
                  << ramCell << "|"
                  << statusCell << "\n";
    }

    std::cout << "-----------------------------------------------------------------------------------------------------------\n";
    if (allIntegrityPassed) {
        std::cout << " All Undo/Redo integrity checks PASSED (0 epsilon discrepancies).\n";
    } else {
        std::cout << " [FAIL] Some Undo/Redo integrity checks failed!\n";
    }
    std::cout << "===========================================================================================================\n\n";
    std::cout.flush();

    _Exit(0);
    return true;
    } catch (const std::bad_alloc& e) {
        std::cerr << "\n[CRITICAL ERROR] std::bad_alloc caught! Out of memory while processing "
                  << std::fixed << std::setprecision(1) << (targetFaces / 1000000.0) << "M polygons!\n";
        std::cerr << "  Details: " << e.what() << "\n";
        std::cerr << "  A 50M polygon mesh with topology, normals, and spatial indexing requires ~15 - 18 GB of free RAM.\n";
        std::cerr.flush();
        return false;
    } catch (const std::exception& e) {
        std::cerr << "\n[CRITICAL ERROR] Standard exception caught: " << e.what() << "\n";
        std::cerr.flush();
        return false;
    } catch (...) {
        std::cerr << "\n[CRITICAL ERROR] Unknown exception caught!\n";
        std::cerr.flush();
        return false;
    }
}

} // namespace SculptBenchmark
