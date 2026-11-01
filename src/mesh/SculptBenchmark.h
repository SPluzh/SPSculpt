#pragma once

#include <iostream>
#include <iomanip>
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
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "editing/SculptManager.h"
#include "editing/undo/UndoManager.h"
#include "common/Enums.h"

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

inline bool run(int targetFaces = 4000000) {
    const char* indexName = BENCH_USE_BVH ? "Linear BVH (feature/bvh-spatial-index)" : "Octree (master branch)";
    
    std::cout << "\n======================================================================\n";
    std::cout << " SPSculpt ClayBuildup Sculpt Benchmark @ " << std::fixed << std::setprecision(1) << (targetFaces / 1000000.0) << "M Polygons\n";
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
    Mesh& mesh = *meshPtr;
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
    std::cout << "\n[3/4] Running Isolated Spatial Queries on 4M Mesh...\n";

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
    // Full End-to-End Sculpt Stroke: BRUSH_CLAYBUILDUP
    // -------------------------------------------------------------
    std::cout << "\n[4/4] Simulating End-to-End ClayBuildup Sculpt Stroke on 4M Mesh...\n";

    Scene scene;
    scene.addMesh(meshPtr.get());
    scene.selectMesh(meshPtr.get());

    Camera& camera = scene.getCamera();
    camera.onResize(1920, 1080);
    camera.resetViewToMesh(meshPtr.get(), false);
    camera.update(0.016f);

    SculptManager sculpt;
    sculpt.setBrush(BRUSH_CLAYBUILDUP);
    // Explicitly set baseline parameters matching user session
    auto& settings = sculpt.getSettings(BRUSH_CLAYBUILDUP);
    settings.radius = 134.0f;
    settings.intensity = 1.0f;
    settings.focalShift = -0.285f;
    settings.spacing = 0.05f;
    settings.clay = true;
    settings.accumulate = false;
    settings.culling = false;

    // Pre-warm tag buffers and Undo stamps for the active mesh (Recommendation 1)
    sculpt.warmupMeshCaches(meshPtr.get());

    // Stroke trajectory across screen center (960, 540) to (1060, 540)
    const int NUM_STROKE_STEPS = 10;
    float startX = 960.0f;
    float startY = 540.0f;
    float endX = 1060.0f;
    float endY = 540.0f;
    float stepDx = (endX - startX) / (float)NUM_STROKE_STEPS;
    float stepDy = (endY - startY) / (float)NUM_STROKE_STEPS;

    std::vector<StrokeStepMeasurement> measurements;
    measurements.reserve(NUM_STROKE_STEPS);

    // Mouse Down: initiates the stroke
    SDL_Event evDown;
    std::memset(&evDown, 0, sizeof(evDown));
    evDown.type = SDL_MOUSEBUTTONDOWN;
    evDown.button.button = SDL_BUTTON_LEFT;
    evDown.button.x = (int)startX;
    evDown.button.y = (int)startY;
    
    auto tStep0Start = std::chrono::high_resolution_clock::now();
    sculpt.handleEvent(evDown, scene);
    auto tStep0End = std::chrono::high_resolution_clock::now();
    double step0TotalMs = std::chrono::duration<double, std::milli>(tStep0End - tStep0Start).count();

    const SculptFrameProfile& prof0 = sculpt.getLastFrameProfile();
    StrokeStepMeasurement m0;
    m0.stepIndex = 0;
    m0.mouseX = startX;
    m0.mouseY = startY;
    m0.totalMs = step0TotalMs;
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
    measurements.push_back(m0);

    // Subsequent continuous stroke steps
    for (int step = 1; step < NUM_STROKE_STEPS; ++step) {
        float curX = startX + step * stepDx;
        float curY = startY + step * stepDy;

        auto tStepStart = std::chrono::high_resolution_clock::now();
        sculpt.executeStroke(scene, meshPtr.get(), camera, curX, curY, 1.0f);
        auto tStepEnd = std::chrono::high_resolution_clock::now();
        double stepTotalMs = std::chrono::duration<double, std::milli>(tStepEnd - tStepStart).count();

        const SculptFrameProfile& prof = sculpt.getLastFrameProfile();
        StrokeStepMeasurement m;
        m.stepIndex = step;
        m.mouseX = curX;
        m.mouseY = curY;
        m.totalMs = stepTotalMs;
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
        measurements.push_back(m);
    }

    // Mouse Up: ends the stroke
    SDL_Event evUp;
    std::memset(&evUp, 0, sizeof(evUp));
    evUp.type = SDL_MOUSEBUTTONUP;
    evUp.button.button = SDL_BUTTON_LEFT;
    evUp.button.x = (int)endX;
    evUp.button.y = (int)endY;
    sculpt.handleEvent(evUp, scene);

    scene.removeMesh(meshPtr.get());
    g_undoManager.clear();

    // Compute averages across steps
    double sumTotal = 0.0, sumRay = 0.0, sumPick = 0.0, sumDeform = 0.0, sumUndo = 0.0;
    double sumSymmetry = 0.0, sumSortDedup = 0.0;
    double sumPickCells = 0.0, sumPickDedup = 0.0, sumPickFilter = 0.0;
    double sumFaceLookup = 0.0, sumFaceNorm = 0.0, sumVertNorm = 0.0, sumSpatialUpdate = 0.0;
    int sumVerts = 0, sumFaces = 0;

    std::cout << "\n------------------------------------------------------------------------------------------------------------------------------------------------------------------------\n";
    std::cout << " Step | Total CPU | Raycast | PickVerts | Deform+Area | Symmetry | SortDedup | UndoRec | FaceLookup | FaceNorm | VertNorm | SpatialUpd | PickedV | Faces \n";
    std::cout << "------------------------------------------------------------------------------------------------------------------------------------------------------------------------\n";

    for (const auto& m : measurements) {
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

        if (m.totalMs > 30.0 || m.stepIndex == 0) {
            std::cout << "      (Step #" << m.stepIndex << " Detail: Pick[Cells: " << std::fixed << std::setprecision(2) << m.pickCellsMs
                      << "ms, Dedup: " << m.pickDedupMs << "ms, Radius: " << m.pickFilterMs
                      << "ms] | Undo: " << m.undoRecordMs << "ms | Deform: " << m.primaryDeformMs
                      << "ms | Sym: " << m.symmetryMs << "ms | FaceNorm: " << m.faceNormalsMs << "ms)\n";
        }

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

    double nSteps = (double)measurements.size();
    double avgTotal = sumTotal / nSteps;
    double avgRay = sumRay / nSteps;
    double avgPick = sumPick / nSteps;
    double avgPickCells = sumPickCells / nSteps;
    double avgPickDedup = sumPickDedup / nSteps;
    double avgPickFilter = sumPickFilter / nSteps;
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

    double steadySumTotal = 0.0;
    int steadyCount = 0;
    for (size_t i = 1; i < measurements.size(); ++i) {
        steadySumTotal += measurements[i].totalMs;
        steadyCount++;
    }
    double steadyAvgTotal = steadyCount > 0 ? (steadySumTotal / steadyCount) : avgTotal;

    double spatialTotalMs = avgRay + avgPick + avgSpatialUpdate;
    double spatialPct = (spatialTotalMs / avgTotal) * 100.0;
    double normalsTotalMs = avgFaceNorm + avgVertNorm;
    double normalsPct = (normalsTotalMs / avgTotal) * 100.0;
    double pipelineOtherMs = avgTotal - spatialTotalMs;
    double pipelineOtherPct = (pipelineOtherMs / avgTotal) * 100.0;

    std::cout << "\n======================================================================\n";
    std::cout << " BENCHMARK SUMMARY & AMDAHL'S LAW BREAKDOWN\n";
    std::cout << "======================================================================\n";
    std::cout << " Average Stroke Step Time:    " << std::fixed << std::setprecision(2) << avgTotal << " ms\n";
    std::cout << " Effective Brush Response FPS:" << std::fixed << std::setprecision(1) << (1000.0 / avgTotal) << " FPS\n";
    std::cout << " Steady-State Avg (Step 1-9): " << std::fixed << std::setprecision(2) << steadyAvgTotal << " ms ("
              << std::fixed << std::setprecision(1) << (1000.0 / steadyAvgTotal) << " FPS)\n";
    std::cout << " Step 0 Warmup (First Click): " << std::fixed << std::setprecision(2) << measurements[0].totalMs << " ms\n";
    std::cout << "\n Pipeline Breakdown:\n";
    std::cout << "   [Spatial Acceleration (Raycast + Pick + Update)]: "
              << std::fixed << std::setprecision(2) << spatialTotalMs << " ms ("
              << std::fixed << std::setprecision(1) << spatialPct << "% of total stroke time)\n";
    std::cout << "     |- Raycast:                 " << std::fixed << std::setprecision(2) << avgRay << " ms (" << std::setprecision(1) << (avgRay / avgTotal * 100.0) << "%)\n";
    std::cout << "     |- Pick Vertices in Sphere: " << std::fixed << std::setprecision(2) << avgPick << " ms (" << std::setprecision(1) << (avgPick / avgTotal * 100.0) << "%)\n";
    std::cout << "     |    |- Collect Cells:      " << std::fixed << std::setprecision(2) << avgPickCells << " ms\n";
    std::cout << "     |    |- Candidate Verts:    " << std::fixed << std::setprecision(2) << avgPickDedup << " ms\n";
    std::cout << "     |    \\- Filter Sphere:      " << std::fixed << std::setprecision(2) << avgPickFilter << " ms\n";
    std::cout << "     \\- Spatial Index Update:    " << std::fixed << std::setprecision(2) << avgSpatialUpdate << " ms (" << std::setprecision(1) << (avgSpatialUpdate / avgTotal * 100.0) << "%)\n";
    std::cout << "\n   [Non-Spatial Pipeline (Mesh Deformation & Topology)]:\n";
    std::cout << "     |- Primary Deform + Area:   " << std::fixed << std::setprecision(2) << avgDeform << " ms (" << std::setprecision(1) << (avgDeform / avgTotal * 100.0) << "%)\n";
    std::cout << "     |- Symmetry Pass:           " << std::fixed << std::setprecision(2) << avgSymmetry << " ms (" << std::setprecision(1) << (avgSymmetry / avgTotal * 100.0) << "%)\n";
    std::cout << "     |- Sort & Dedup (Radix):    " << std::fixed << std::setprecision(2) << avgSortDedup << " ms (" << std::setprecision(1) << (avgSortDedup / avgTotal * 100.0) << "%)\n";
    std::cout << "     |- Undo Record:             " << std::fixed << std::setprecision(2) << avgUndo << " ms (" << std::setprecision(1) << (avgUndo / avgTotal * 100.0) << "%)\n";
    std::cout << "     |- Face Lookup (RingFaces): " << std::fixed << std::setprecision(2) << avgFaceLookup << " ms (" << std::setprecision(1) << (avgFaceLookup / avgTotal * 100.0) << "%)\n";
    std::cout << "     |- Face Normals Calc:       " << std::fixed << std::setprecision(2) << avgFaceNorm << " ms (" << std::setprecision(1) << (avgFaceNorm / avgTotal * 100.0) << "%)\n";
    std::cout << "     \\- Vert Normals Calc:       " << std::fixed << std::setprecision(2) << avgVertNorm << " ms (" << std::setprecision(1) << (avgVertNorm / avgTotal * 100.0) << "%)\n";
    std::cout << "   Total Non-Spatial Overhead:   " << std::fixed << std::setprecision(2) << pipelineOtherMs << " ms (" << std::setprecision(1) << pipelineOtherPct << "% of total stroke time)\n";
    std::cout << "======================================================================\n\n";
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
