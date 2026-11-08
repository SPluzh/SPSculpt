#pragma once

#include "mesh/BVH.h"
#include "mesh/Mesh.h"

#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <cassert>
#include <algorithm>
#include <cmath>
#include <memory>
#include <omp.h>

namespace BVHValidation {

inline bool bruteRayTriangle(
    const glm::vec3& orig, const glm::vec3& dir,
    const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
    float& t) {
    const float EPSILON = 1e-7f;
    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;
    glm::vec3 h = glm::cross(dir, edge2);
    float a = glm::dot(edge1, h);
    if (std::abs(a) < EPSILON) return false;
    float f = 1.0f / a;
    glm::vec3 s = orig - v0;
    float u = f * glm::dot(s, h);
    if (u < 0.0f || u > 1.0f) return false;
    glm::vec3 q = glm::cross(s, edge1);
    float v = f * glm::dot(dir, q);
    if (v < 0.0f || u + v > 1.0f) return false;
    float hitT = f * glm::dot(edge2, q);
    if (hitT > EPSILON) {
        t = hitT;
        return true;
    }
    return false;
}

inline bool bruteForceRaycast(const Mesh& mesh, const glm::vec3& origin, const glm::vec3& dir, RayHit& hit) {
    float bestT = hit.t;
    bool found = false;

    for (int f = 0; f < mesh.nbFaces; ++f) {
        if (!mesh.faceVisible.empty() && f < (int)mesh.faceVisible.size() && !mesh.faceVisible[f]) {
            continue;
        }

        uint32_t idx = f * 4;
        uint32_t v0 = mesh.faces[idx + 0];
        uint32_t v1 = mesh.faces[idx + 1];
        uint32_t v2 = mesh.faces[idx + 2];
        uint32_t v3 = mesh.faces[idx + 3];

        if (!mesh.vertVisible.empty()) {
            if (v0 < mesh.vertVisible.size() && !mesh.vertVisible[v0]) continue;
            if (v1 < mesh.vertVisible.size() && !mesh.vertVisible[v1]) continue;
            if (v2 < mesh.vertVisible.size() && !mesh.vertVisible[v2]) continue;
            if (v3 != 0xFFFFFFFF && v3 < mesh.vertVisible.size() && !mesh.vertVisible[v3]) continue;
        }

        glm::vec3 p0(mesh.verts[v0 * 3], mesh.verts[v0 * 3 + 1], mesh.verts[v0 * 3 + 2]);
        glm::vec3 p1(mesh.verts[v1 * 3], mesh.verts[v1 * 3 + 1], mesh.verts[v1 * 3 + 2]);
        glm::vec3 p2(mesh.verts[v2 * 3], mesh.verts[v2 * 3 + 1], mesh.verts[v2 * 3 + 2]);

        float t = 0.0f;
        if (bruteRayTriangle(origin, dir, p0, p1, p2, t) && t < bestT) {
            bestT = t;
            hit.t = t;
            hit.faceId = f;
            hit.position = origin + dir * t;
            found = true;
        }

        if (v3 != 0xFFFFFFFF) {
            glm::vec3 p3(mesh.verts[v3 * 3], mesh.verts[v3 * 3 + 1], mesh.verts[v3 * 3 + 2]);
            if (bruteRayTriangle(origin, dir, p0, p2, p3, t) && t < bestT) {
                bestT = t;
                hit.t = t;
                hit.faceId = f;
                hit.position = origin + dir * t;
                found = true;
            }
        }
    }
    return found;
}

inline std::vector<uint32_t> bruteForcePickVertices(const Mesh& mesh, const glm::vec3& center, float radiusSquared, const uint8_t* vertVisible = nullptr) {
    std::vector<uint32_t> result;
    for (int v = 0; v < mesh.nbVerts; ++v) {
        if (vertVisible && !vertVisible[v]) continue;
        float dx = center.x - mesh.verts[v * 3 + 0];
        float dy = center.y - mesh.verts[v * 3 + 1];
        float dz = center.z - mesh.verts[v * 3 + 2];
        if (dx * dx + dy * dy + dz * dz <= radiusSquared) {
            result.push_back((uint32_t)v);
        }
    }
    return result;
}

inline std::unique_ptr<Mesh> createTestSphereMesh(int rings, int sectors) {
    auto mesh = std::make_unique<Mesh>();
    mesh->nbVerts = 0;
    mesh->nbFaces = 0;

    const float R = 2.0f;
    for (int r = 0; r <= rings; ++r) {
        float phi = 3.1415926535f * float(r) / float(rings);
        for (int s = 0; s <= sectors; ++s) {
            float theta = 2.0f * 3.1415926535f * float(s) / float(sectors);
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
    mesh->postInit(true);

    return mesh;
}

inline bool run() {
    std::cout << "\n======================================================\n";
    std::cout << "         SPSculpt BVH Acceleration Unit Tests         \n";
    std::cout << "======================================================\n\n";

    // 1. Procedural mesh generation
    std::cout << "[1/10] Generating procedural mesh (200 rings x 200 sectors = 40,000 faces)...\n";
    auto meshPtr = createTestSphereMesh(200, 200);
    Mesh& mesh = *meshPtr;
    std::cout << "       Vertices: " << mesh.nbVerts << ", Faces: " << mesh.nbFaces << "\n";

    // 2. Build BVH & Measure RAM
    std::cout << "[2/10] Building BVH structure & measuring memory footprint...\n";
    BVH bvh;
    auto tBuildStart = std::chrono::high_resolution_clock::now();
    bvh.build(mesh);
    auto tBuildEnd = std::chrono::high_resolution_clock::now();
    double buildMs = std::chrono::duration<double, std::milli>(tBuildEnd - tBuildStart).count();

    size_t memBytes = bvh.memoryBytes();
    std::cout << "       BVH Build: " << buildMs << " ms | Nodes: " << bvh.nodeCount()
              << " | Leaves: " << bvh.leafCount() << " | RAM: " << (memBytes / 1024.0 / 1024.0) << " MB\n";

    if (bvh.nodeCount() == 0 || bvh.leafCount() == 0) {
        std::cerr << "FAIL: BVH built empty nodes!\n";
        return false;
    }

    // 3. Test Raycast vs Ground Truth Brute-force
    std::cout << "[3/10] Testing raycast accuracy vs Brute-force (200 rays)...\n";
    std::mt19937 rng(1337);
    std::uniform_real_distribution<float> distCoord(-3.5f, 3.5f);

    int rayHits = 0, rayMisses = 0;
    for (int i = 0; i < 200; ++i) {
        glm::vec3 orig(distCoord(rng), distCoord(rng), distCoord(rng));
        glm::vec3 target(distCoord(rng) * 0.5f, distCoord(rng) * 0.5f, distCoord(rng) * 0.5f);
        glm::vec3 dir = glm::normalize(target - orig);

        RayHit bvhHit;
        bool bvhFound = bvh.raycast(mesh, orig, dir, bvhHit);

        RayHit bruteHit;
        bool bruteFound = bruteForceRaycast(mesh, orig, dir, bruteHit);

        if (bvhFound != bruteFound) {
            std::cerr << "FAIL: Mismatch in ray hit on ray #" << i << "! BVH=" << bvhFound << " Brute=" << bruteFound << "\n";
            return false;
        }

        if (bvhFound) {
            rayHits++;
            float diffT = std::abs(bvhHit.t - bruteHit.t);
            if (diffT > 1e-3f) {
                std::cerr << "FAIL: Distance discrepancy on ray #" << i << ": BVH=" << bvhHit.t << ", Brute=" << bruteHit.t << "\n";
                return false;
            }
        } else {
            rayMisses++;
        }
    }
    std::cout << "       Raycast Accuracy PASSED: " << rayHits << " hits, " << rayMisses << " misses matched exactly.\n";

    // 4. Test Sphere Vertex Picking vs Ground Truth Brute-force
    std::cout << "[4/10] Testing sphere vertex query vs Brute-force (50 random spheres)...\n";
    std::uniform_real_distribution<float> distPos(-2.0f, 2.0f);
    std::uniform_real_distribution<float> distRad(0.3f, 1.5f);

    for (int i = 0; i < 50; ++i) {
        glm::vec3 center(distPos(rng), distPos(rng), distPos(rng));
        float r = distRad(rng);
        float r2 = r * r;

        std::vector<uint32_t> bvhVerts;
        bvh.pickVerticesInSphere(mesh, center, r2, bvhVerts);
        std::sort(bvhVerts.begin(), bvhVerts.end());

        std::vector<uint32_t> bruteVerts = bruteForcePickVertices(mesh, center, r2);
        std::sort(bruteVerts.begin(), bruteVerts.end());

        if (bvhVerts != bruteVerts) {
            std::cerr << "Sphere #" << i << ": BVH=" << bvhVerts.size() << ", Brute=" << bruteVerts.size() << "\n";
            return false;
        }
    }
    std::cout << "       Sphere Query PASSED: 100% exact match with Brute-force.\n";

    // 5. Test Incremental Refit (+Y apex)
    std::cout << "[5/10] Testing O(K log N) incremental refit after sculpt deformation...\n";
    glm::vec3 sculptCenter(0.0f, 2.0f, 0.0f);
    float sculptRadius2 = 1.0f * 1.0f;
    std::vector<uint32_t> touchedVerts;
    bvh.pickVerticesInSphere(mesh, sculptCenter, sculptRadius2, touchedVerts);

    for (uint32_t v : touchedVerts) {
        mesh.verts[v * 3 + 1] += 0.75f;
    }

    std::vector<uint32_t> modifiedFaces;
    for (int f = 0; f < mesh.nbFaces; ++f) {
        uint32_t idx = f * 4;
        for (int k = 0; k < 4; ++k) {
            uint32_t v = mesh.faces[idx + k];
            if (std::find(touchedVerts.begin(), touchedVerts.end(), v) != touchedVerts.end()) {
                modifiedFaces.push_back((uint32_t)f);
                break;
            }
        }
    }

    auto tRefitStart = std::chrono::high_resolution_clock::now();
    bvh.refit(mesh, modifiedFaces.data(), modifiedFaces.size());
    auto tRefitEnd = std::chrono::high_resolution_clock::now();
    double refitMs = std::chrono::duration<double, std::milli>(tRefitEnd - tRefitStart).count();

    std::cout << "       Refit " << modifiedFaces.size() << " faces completed in " << refitMs << " ms.\n";

    glm::vec3 rayOrig(0.0f, 6.0f, 0.0f);
    glm::vec3 rayDir(0.0f, -1.0f, 0.0f);
    RayHit hit;
    bool hitApex = bvh.raycast(mesh, rayOrig, rayDir, hit);
    if (!hitApex || hit.position.y < 2.5f) {
        std::cerr << "FAIL: Deformed apex raycast did not hit displaced surface! Y=" << hit.position.y << "\n";
        return false;
    }
    std::cout << "       Apex raycast hit Y = " << hit.position.y << " (successfully deformed from 2.0 -> " << hit.position.y << ")\n";

    // 6. Test Refit WITHOUT updateFaceNormalsAndBoxes with offset in +X / +Z
    std::cout << "[6/10] Testing refit WITHOUT updateFaceNormalsAndBoxes (stale faceBoxes resilience)...\n";
    glm::vec3 sideCenter(2.0f, 0.0f, 0.0f);
    float sideRadius2 = 1.2f * 1.2f;
    std::vector<uint32_t> sideVerts;
    bvh.pickVerticesInSphere(mesh, sideCenter, sideRadius2, sideVerts);

    for (uint32_t v : sideVerts) {
        mesh.verts[v * 3 + 0] += 1.5f; // Displace sideways along +X
    }

    std::vector<uint32_t> sideFaces;
    for (int f = 0; f < mesh.nbFaces; ++f) {
        uint32_t idx = f * 4;
        for (int k = 0; k < 4; ++k) {
            uint32_t v = mesh.faces[idx + k];
            if (std::find(sideVerts.begin(), sideVerts.end(), v) != sideVerts.end()) {
                sideFaces.push_back((uint32_t)f);
                break;
            }
        }
    }

    // Call ONLY refit (mesh.faceBoxes are intentionally NOT updated!)
    bvh.refit(mesh, sideFaces.data(), sideFaces.size());

    // Old position should now be a miss or hit different point
    glm::vec3 sideRayOrig(6.0f, 0.0f, 0.0f);
    glm::vec3 sideRayDir(-1.0f, 0.0f, 0.0f);
    RayHit sideHit;
    bool hitSide = bvh.raycast(mesh, sideRayOrig, sideRayDir, sideHit);
    if (!hitSide || sideHit.position.x < 3.0f) {
        std::cerr << "FAIL: Displaced side raycast failed! X=" << sideHit.position.x << " (expected >= 3.0f)\n";
        return false;
    }

    // Sphere query in new deformed region must match brute force 100%
    glm::vec3 queryPos(3.5f, 0.0f, 0.0f);
    std::vector<uint32_t> bvhDeformed = bvh.pickVerticesInSphere(mesh, queryPos, 1.0f);
    std::sort(bvhDeformed.begin(), bvhDeformed.end());
    std::vector<uint32_t> bruteDeformed = bruteForcePickVertices(mesh, queryPos, 1.0f);
    std::sort(bruteDeformed.begin(), bruteDeformed.end());

    if (bvhDeformed != bruteDeformed || bvhDeformed.empty()) {
        std::cerr << "FAIL: Stale faceBoxes refit caused vertex query mismatch! BVH=" << bvhDeformed.size() << " Brute=" << bruteDeformed.size() << "\n";
        return false;
    }
    std::cout << "       Resilience to stale faceBoxes PASSED: 100% match with brute-force in displaced region.\n";

    // 7. Test Parallel Symmetry (scratchIndex = 0 and 1 in OpenMP parallel sections)
    std::cout << "[7/10] Testing parallel symmetry queries (scratchIndex 0 and 1 in parallel sections)...\n";
    bool symPassed = true;
    for (int i = 0; i < 100; ++i) {
        if (!symPassed) break;
        glm::vec3 center0(distPos(rng), distPos(rng), distPos(rng));
        glm::vec3 center1(-center0.x, center0.y, center0.z); // Mirror X
        float r2 = 0.8f * 0.8f;

        std::vector<uint32_t> v0, v1;
        #pragma omp parallel sections
        {
            #pragma omp section
            {
                bvh.pickVerticesInSphere(mesh, center0, r2, v0, nullptr, 0);
            }
            #pragma omp section
            {
                bvh.pickVerticesInSphere(mesh, center1, r2, v1, nullptr, 1);
            }
        }

        std::sort(v0.begin(), v0.end());
        std::sort(v1.begin(), v1.end());

        std::vector<uint32_t> brute0 = bruteForcePickVertices(mesh, center0, r2);
        std::vector<uint32_t> brute1 = bruteForcePickVertices(mesh, center1, r2);
        std::sort(brute0.begin(), brute0.end());
        std::sort(brute1.begin(), brute1.end());

        if (v0 != brute0 || v1 != brute1) {
            std::cerr << "FAIL: Parallel symmetry scratch collision on iter " << i << "! v0 size=" << v0.size() << " brute0=" << brute0.size() << " v1 size=" << v1.size() << " brute1=" << brute1.size() << "\n";
            symPassed = false;
        }
    }
    if (!symPassed) return false;
    std::cout << "       Parallel symmetry scratch isolation PASSED: 100/100 concurrent queries matched brute-force.\n";

    // 8. Test Visibility filtering (faceVisible and vertVisible)
    std::cout << "[8/10] Testing face & vertex visibility filtering...\n";
    // Hide a hemisphere
    for (int v = 0; v < mesh.nbVerts; ++v) {
        if (mesh.verts[v * 3 + 0] < 0.0f) {
            mesh.vertVisible[v] = 0;
        }
    }
    for (int f = 0; f < mesh.nbFaces; ++f) {
        uint32_t idx = f * 4;
        if (mesh.vertVisible[mesh.faces[idx]] == 0) {
            mesh.faceVisible[f] = 0;
        }
    }

    // Ray from negative X (+X direction) through the hidden hemisphere to the visible back wall
    glm::vec3 negRayOrig(-6.0f, 0.0f, 0.0f);
    glm::vec3 negRayDir(1.0f, 0.0f, 0.0f);
    RayHit visHit;
    bool visHitFound = bvh.raycast(mesh, negRayOrig, negRayDir, visHit);
    if (!visHitFound || visHit.position.x <= 0.0f) {
        std::cerr << "FAIL: Ray did not punch through hidden hemisphere to visible wall! X=" << visHit.position.x << "\n";
        return false;
    }

    // Sphere query in hidden region must return 0 vertices
    std::vector<uint32_t> hiddenVerts = bvh.pickVerticesInSphere(mesh, glm::vec3(-1.5f, 0.0f, 0.0f), 0.5f, mesh.vertVisible.data());
    if (!hiddenVerts.empty()) {
        std::cerr << "FAIL: Pick vertices returned " << hiddenVerts.size() << " hidden vertices!\n";
        return false;
    }
    // Restore visibility
    mesh.vertVisible.assign(mesh.nbVerts, 1);
    mesh.faceVisible.assign(mesh.nbFaces, 1);
    std::cout << "       Visibility filtering PASSED: hidden faces penetrated, hidden vertices culled.\n";

    // 9. Test Mixed topology (Triangles + Quads) and Empty/Degenerate mesh
    std::cout << "[9/10] Testing mixed topology (tris + quads) and empty mesh robustness...\n";
    Mesh mixedMesh;
    mixedMesh.nbVerts = 4;
    mixedMesh.verts = { 0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  1.0f, 1.0f, 0.0f,  0.0f, 1.0f, 0.0f };
    mixedMesh.nbFaces = 2;
    // Face 0: triangle (v3 = 0xFFFFFFFF)
    mixedMesh.faces.push_back(0); mixedMesh.faces.push_back(1); mixedMesh.faces.push_back(2); mixedMesh.faces.push_back(0xFFFFFFFF);
    // Face 1: quad
    mixedMesh.faces.push_back(0); mixedMesh.faces.push_back(2); mixedMesh.faces.push_back(3); mixedMesh.faces.push_back(0);
    mixedMesh.vertVisible.assign(4, 1);
    mixedMesh.faceVisible.assign(2, 1);
    mixedMesh.postInit(true);

    RayHit mixedHit;
    bool mHit = mixedMesh.bvh.raycast(mixedMesh, glm::vec3(0.5f, 0.2f, 2.0f), glm::vec3(0.0f, 0.0f, -1.0f), mixedHit);
    if (!mHit || mixedHit.faceId != 0) {
        std::cerr << "FAIL: Mixed topology raycast failed on triangle face!\n";
        return false;
    }

    // Empty mesh test: ensure no crash on any API call
    Mesh emptyMesh;
    emptyMesh.postInit(true);
    RayHit emptyHit;
    bool eHit = emptyMesh.bvh.raycast(emptyMesh, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), emptyHit);
    assert(!eHit);
    std::vector<uint32_t> emptyPick = emptyMesh.bvh.pickVerticesInSphere(emptyMesh, glm::vec3(0.0f), 1.0f);
    assert(emptyPick.empty());
    emptyMesh.bvh.refitAll(emptyMesh);
    emptyMesh.bvh.scale(2.0f, 2.0f, 2.0f);
    std::cout << "       Mixed topology & empty mesh robustness PASSED: zero crash on degenerate input.\n";

    // 10. Test Lazy GoZ Contract + Scale (including negative scale)
    std::cout << "[10/10] Testing lazy GoZ build contract and negative scaling...\n";
    auto lazyMesh = createTestSphereMesh(50, 50);
    lazyMesh->bvh.clear(); // Simulate GoZ uninitialized spatial state
    if (lazyMesh->bvh.isBuilt()) {
        std::cerr << "FAIL: Lazy mesh reports BVH built before requested!\n";
        return false;
    }

    lazyMesh->ensureBVHBuilt();
    if (!lazyMesh->bvh.isBuilt()) {
        std::cerr << "FAIL: ensureBVHBuilt() did not construct BVH!\n";
        return false;
    }

    // Scale mesh with negative factor (-1.0f on X, 2.0f on Y, 1.0f on Z)
    lazyMesh->bvh.scale(-1.0f, 2.0f, 1.0f);
    float aabb[6];
    lazyMesh->bvh.getRootAABB(aabb);
    if (aabb[0] > aabb[3] || aabb[1] > aabb[4] || aabb[2] > aabb[5]) {
        std::cerr << "FAIL: Negative scale inverted AABB min/max bounds!\n";
        return false;
    }
    std::cout << "       Lazy GoZ contract & negative scale PASSED: valid AABB ["
              << aabb[0] << ", " << aabb[3] << "].\n";

    std::cout << "\n======================================================\n";
    std::cout << "   SUCCESS: All 10 BVH tests passed without errors!  \n";
    std::cout << "======================================================\n\n";
    return true;
}

} // namespace BVHValidation

