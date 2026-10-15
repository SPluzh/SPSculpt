#include "editing/manifold/QuadCapGenerator.h"
#include "mesh/Mesh.h"
#include "mesh/Topology.h"
#include "common/Logger.h"
#include "common/Constants.h"
#include "CDT.h"

#include <chrono>
#include <cmath>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

namespace sculpt {

// 2D Point-in-polygon test (winding number)
static bool pointInPoly2D(const glm::vec2& p, const std::vector<glm::vec2>& poly) {
    int wn = 0;
    int n = static_cast<int>(poly.size());
    for (int i = 0; i < n; ++i) {
        glm::vec2 p1 = poly[i];
        glm::vec2 p2 = poly[(i + 1) % n];
        if (p1.y <= p.y) {
            if (p2.y > p.y) {
                float vt = (p.y - p1.y) / (p2.y - p1.y);
                if (p.x < p1.x + vt * (p2.x - p1.x)) ++wn;
            }
        } else {
            if (p2.y <= p.y) {
                float vt = (p.y - p1.y) / (p2.y - p1.y);
                if (p.x < p1.x + vt * (p2.x - p1.x)) --wn;
            }
        }
    }
    return wn != 0;
}

// Distance from point to polygon boundary
static float distToPolyBoundary(const glm::vec2& p, const std::vector<glm::vec2>& poly) {
    float minD2 = 1e18f;
    int n = static_cast<int>(poly.size());
    for (int i = 0; i < n; ++i) {
        glm::vec2 a = poly[i];
        glm::vec2 b = poly[(i + 1) % n];
        glm::vec2 ab = b - a;
        float len2 = glm::dot(ab, ab);
        float t = (len2 > 1e-8f) ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        glm::vec2 proj = a + t * ab;
        glm::vec2 diff = p - proj;
        minD2 = std::min(minD2, glm::dot(diff, diff));
    }
    return std::sqrt(minD2);
}

// Returns the closest point on triangle (a, b, c) to point p (Christer Ericson, RTCD)
static glm::vec3 closestPointOnTriangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    glm::vec3 ab = b - a;
    glm::vec3 ac = c - a;
    glm::vec3 ap = p - a;
    float d1 = glm::dot(ab, ap);
    float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    glm::vec3 bp = p - b;
    float d3 = glm::dot(ab, bp);
    float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        return a + v * ab;
    }

    glm::vec3 cp = p - c;
    float d5 = glm::dot(ab, cp);
    float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        return a + w * ac;
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + w * (c - b);
    }

    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    return a + ab * v + ac * w;
}

// Möller–Trumbore ray-triangle intersection
static bool intersectRayTriangle(const glm::vec3& orig, const glm::vec3& dir,
                                 const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
                                 float& t, float& u, float& v) {
    const float EPSILON = 1e-7f;
    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;
    glm::vec3 h = glm::cross(dir, edge2);
    float a = glm::dot(edge1, h);
    if (a > -EPSILON && a < EPSILON) return false;
    float f = 1.0f / a;
    glm::vec3 s = orig - v0;
    u = f * glm::dot(s, h);
    if (u < -0.001f || u > 1.001f) return false;
    glm::vec3 q = glm::cross(s, edge1);
    v = f * glm::dot(dir, q);
    if (v < -0.001f || u + v > 1.001f) return false;
    t = f * glm::dot(edge2, q);
    return true;
}

// Check if 2D line segments (p1, p2) and (p3, p4) intersect or overlap
static bool segmentsIntersect2D(const glm::vec2& p1, const glm::vec2& p2,
                                const glm::vec2& p3, const glm::vec2& p4) {
    auto cross2D = [](const glm::vec2& a, const glm::vec2& b) -> float {
        return a.x * b.y - a.y * b.x;
    };

    glm::vec2 d1 = p2 - p1;
    glm::vec2 d2 = p4 - p3;
    float denom = cross2D(d1, d2);
    glm::vec2 dp = p3 - p1;

    const float eps = 1e-5f;

    if (std::abs(denom) < 1e-8f) {
        // Collinear or parallel
        if (std::abs(cross2D(dp, d1)) > 1e-5f) {
            return false; // Parallel, disjoint lines
        }
        float d1Len2 = glm::dot(d1, d1);
        if (d1Len2 > 1e-10f) {
            float t0 = glm::dot(p3 - p1, d1) / d1Len2;
            float t1 = glm::dot(p4 - p1, d1) / d1Len2;
            float minT = std::min(t0, t1);
            float maxT = std::max(t0, t1);
            if (maxT > eps && minT < 1.0f - eps) {
                return true;
            }
        }
        return false;
    }

    float t = cross2D(dp, d2) / denom;
    float u = cross2D(dp, d1) / denom;

    // For non-adjacent segments: any overlap in [0, 1] is invalid for CDT constraint graph
    return (t >= -eps && t <= (1.0f + eps) && u >= -eps && u <= (1.0f + eps));
}

// Checks if a 2D boundary polygon is simple (no self-intersections, no 180-deg foldbacks, no zero-length edges)
static bool polygonHasSelfIntersections(const std::vector<glm::vec2>& poly, int& outSegA, int& outSegB) {
    outSegA = -1;
    outSegB = -1;
    const size_t N = poly.size();
    if (N < 3) return true;

    // Check adjacent edges for zero length or 180-degree foldbacks
    for (size_t i = 0; i < N; ++i) {
        glm::vec2 segI = poly[(i + 1) % N] - poly[i];
        float lenI = glm::length(segI);
        if (lenI < 1e-5f) {
            outSegA = static_cast<int>(i);
            outSegB = static_cast<int>((i + 1) % N);
            return true; // Coincident adjacent vertices in 2D
        }
        glm::vec2 segNext = poly[(i + 2) % N] - poly[(i + 1) % N];
        float lenNext = glm::length(segNext);
        if (lenNext > 1e-5f) {
            float dotDir = glm::dot(segI / lenI, segNext / lenNext);
            if (dotDir < -0.999f) {
                outSegA = static_cast<int>(i);
                outSegB = static_cast<int>((i + 1) % N);
                return true; // 180-degree foldback directly onto previous edge
            }
        }
    }

    // Precompute AABBs for each segment
    struct SegAABB {
        float minX, maxX, minY, maxY;
    };
    std::vector<SegAABB> segBoxes(N);
    for (size_t i = 0; i < N; ++i) {
        const glm::vec2& a = poly[i];
        const glm::vec2& b = poly[(i + 1) % N];
        segBoxes[i] = {
            std::min(a.x, b.x),
            std::max(a.x, b.x),
            std::min(a.y, b.y),
            std::max(a.y, b.y)
        };
    }

    const float boxTol = 1e-5f;

    // Check all non-adjacent pairs
    for (size_t i = 0; i < N; ++i) {
        for (size_t j = i + 2; j < N; ++j) {
            if (i == 0 && j == N - 1) continue; // Adjacent (wrap-around endpoint)

            // Fast AABB rejection
            if (segBoxes[i].maxX + boxTol < segBoxes[j].minX ||
                segBoxes[i].minX - boxTol > segBoxes[j].maxX ||
                segBoxes[i].maxY + boxTol < segBoxes[j].minY ||
                segBoxes[i].minY - boxTol > segBoxes[j].maxY) {
                continue;
            }

            if (segmentsIntersect2D(poly[i], poly[(i + 1) % N], poly[j], poly[(j + 1) % N])) {
                outSegA = static_cast<int>(i);
                outSegB = static_cast<int>(j);
                return true;
            }
        }
    }

    return false;
}

QuadCapResult QuadCapGenerator::generateQuadCap(Mesh& mesh, uint32_t capGroupID, float targetEdgeLen) {
    QuadCapResult result;
    auto tStart = std::chrono::high_resolution_clock::now();

    if (mesh.nbFaces == 0 || mesh.faceGroups.empty()) {
        return result;
    }

    const int nbFaces = mesh.nbFaces;

    // 1. Identify all faces belonging to capGroupID
    std::unordered_set<int> capFaceIndices;
    capFaceIndices.reserve(nbFaces / 4);
    for (int f = 0; f < nbFaces; ++f) {
        if (mesh.faceGroups[f] == capGroupID) {
            capFaceIndices.insert(f);
        }
    }

    if (capFaceIndices.empty()) {
        sculpt_log("[QUAD CAP] No faces found with capGroupID: %u (total faces: %d)\n", capGroupID, nbFaces);
        return result;
    }

    // 2. Partition cap faces into connected components (Patches)
    auto makeUndirectedEdgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
        uint32_t u = std::min(a, b);
        uint32_t v = std::max(a, b);
        return ((uint64_t)u << 32) | (uint64_t)v;
    };

    std::unordered_map<uint64_t, std::vector<int>> capEdgeToFaces;
    capEdgeToFaces.reserve(capFaceIndices.size() * 3);

    for (int f : capFaceIndices) {
        uint32_t v0 = mesh.faces[f * 4 + 0];
        uint32_t v1 = mesh.faces[f * 4 + 1];
        uint32_t v2 = mesh.faces[f * 4 + 2];
        uint32_t v3 = mesh.faces[f * 4 + 3];

        capEdgeToFaces[makeUndirectedEdgeKey(v0, v1)].push_back(f);
        capEdgeToFaces[makeUndirectedEdgeKey(v1, v2)].push_back(f);
        if (v3 != TRI_INDEX) {
            capEdgeToFaces[makeUndirectedEdgeKey(v2, v3)].push_back(f);
            capEdgeToFaces[makeUndirectedEdgeKey(v3, v0)].push_back(f);
        } else {
            capEdgeToFaces[makeUndirectedEdgeKey(v2, v0)].push_back(f);
        }
    }

    // Build face adjacency within cap
    std::unordered_map<int, std::vector<int>> faceAdj;
    faceAdj.reserve(capFaceIndices.size());
    for (const auto& [edgeKey, faceList] : capEdgeToFaces) {
        if (faceList.size() >= 2) {
            for (size_t i = 0; i < faceList.size(); ++i) {
                for (size_t j = i + 1; j < faceList.size(); ++j) {
                    faceAdj[faceList[i]].push_back(faceList[j]);
                    faceAdj[faceList[j]].push_back(faceList[i]);
                }
            }
        }
    }

    // Flood fill / BFS to discover all distinct cap patches
    std::vector<std::vector<int>> patches;
    std::unordered_set<int> visitedFaces;
    visitedFaces.reserve(capFaceIndices.size());

    for (int f : capFaceIndices) {
        if (visitedFaces.count(f)) continue;

        std::vector<int> patchFaces;
        std::vector<int> queue = { f };
        visitedFaces.insert(f);

        size_t head = 0;
        while (head < queue.size()) {
            int curr = queue[head++];
            patchFaces.push_back(curr);
            auto it = faceAdj.find(curr);
            if (it != faceAdj.end()) {
                for (int neighbor : it->second) {
                    if (visitedFaces.insert(neighbor).second) {
                        queue.push_back(neighbor);
                    }
                }
            }
        }
        patches.push_back(std::move(patchFaces));
    }

    sculpt_log("[QUAD CAP] Total mesh faces: %d | Cap faces (ID %u): %zu across %zu separate patch(es)\n",
               nbFaces, capGroupID, capFaceIndices.size(), patches.size());

    // 3. Prepare new faces buffer with all non-cap faces
    std::vector<uint32_t> newFaces;
    std::vector<uint32_t> newFaceGroups;
    newFaces.reserve(mesh.faces.size());
    newFaceGroups.reserve(mesh.faceGroups.size());

    for (int f = 0; f < nbFaces; ++f) {
        if (capFaceIndices.find(f) == capFaceIndices.end()) {
            newFaces.push_back(mesh.faces[f * 4 + 0]);
            newFaces.push_back(mesh.faces[f * 4 + 1]);
            newFaces.push_back(mesh.faces[f * 4 + 2]);
            newFaces.push_back(mesh.faces[f * 4 + 3]);
            newFaceGroups.push_back(mesh.faceGroups[f]);
        }
    }

    // 4. Staging structures for newly created vertices
    struct StagedVert {
        glm::vec3 pos;
        glm::vec3 normal;
    };
    std::vector<StagedVert> stagedVerts;

    int totalQuadsGenerated = 0;
    int totalTrisGenerated = 0;
    bool anyPatchQuadCapped = false;

    // Directed edge key for boundary loop extraction
    struct EdgeKey {
        uint32_t u, v;
        bool operator==(const EdgeKey& o) const { return u == o.u && v == o.v; }
    };
    struct EdgeKeyHash {
        size_t operator()(const EdgeKey& k) const {
            return std::hash<uint64_t>()(((uint64_t)k.u << 32) | (uint64_t)k.v);
        }
    };

    struct OrigCapTri {
        glm::vec3 p0, p1, p2;
        glm::vec3 n0, n1, n2;
        glm::vec3 normal;
        float area;
        float minU, minV, maxU, maxV;
    };

    // 5. Process each patch independently
    for (size_t patchIdx = 0; patchIdx < patches.size(); ++patchIdx) {
        const auto& patchFaces = patches[patchIdx];
        sculpt_log("----------------------------------------------------------------\n");
        sculpt_log("[QUAD CAP PATCH #%zu/%zu] Faces in patch: %zu\n", patchIdx + 1, patches.size(), patchFaces.size());

        // Helper to keep original Manifold faces for this patch upon fallback
        auto fallbackPatch = [&](const char* reason) {
            int origQ = 0, origT = 0;
            for (int f : patchFaces) {
                if (mesh.faces[f * 4 + 3] != TRI_INDEX) origQ++;
                else origT++;
                newFaces.push_back(mesh.faces[f * 4 + 0]);
                newFaces.push_back(mesh.faces[f * 4 + 1]);
                newFaces.push_back(mesh.faces[f * 4 + 2]);
                newFaces.push_back(mesh.faces[f * 4 + 3]);
                newFaceGroups.push_back(mesh.faceGroups[f]);
            }
            totalQuadsGenerated += origQ;
            totalTrisGenerated += origT;
            sculpt_log("[QUAD CAP PATCH #%zu] FALLBACK: %s. Preserved %zu original Manifold faces (%d quads, %d tris)\n",
                       patchIdx + 1, reason, patchFaces.size(), origQ, origT);
        };

        // Extract 3D triangles and analyze patch curvature
        std::vector<OrigCapTri> patchTris;
        patchTris.reserve(patchFaces.size() * 2);
        std::unordered_set<uint32_t> patchUniqueVerts;
        glm::vec3 patchBBoxMin(1e9f);
        glm::vec3 patchBBoxMax(-1e9f);

        auto addPatchTri = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
            if (i0 >= (uint32_t)mesh.nbVerts || i1 >= (uint32_t)mesh.nbVerts || i2 >= (uint32_t)mesh.nbVerts) return;
            glm::vec3 p0(mesh.verts[i0 * 3], mesh.verts[i0 * 3 + 1], mesh.verts[i0 * 3 + 2]);
            glm::vec3 p1(mesh.verts[i1 * 3], mesh.verts[i1 * 3 + 1], mesh.verts[i1 * 3 + 2]);
            glm::vec3 p2(mesh.verts[i2 * 3], mesh.verts[i2 * 3 + 1], mesh.verts[i2 * 3 + 2]);
            glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
            float len = glm::length(fn);
            if (len > 1e-6f) {
                glm::vec3 n0(0, 1, 0), n1(0, 1, 0), n2(0, 1, 0);
                if (!mesh.normals.empty() && mesh.normals.size() >= ((size_t)mesh.nbVerts * 3)) {
                    n0 = glm::vec3(mesh.normals[i0 * 3], mesh.normals[i0 * 3 + 1], mesh.normals[i0 * 3 + 2]);
                    n1 = glm::vec3(mesh.normals[i1 * 3], mesh.normals[i1 * 3 + 1], mesh.normals[i1 * 3 + 2]);
                    n2 = glm::vec3(mesh.normals[i2 * 3], mesh.normals[i2 * 3 + 1], mesh.normals[i2 * 3 + 2]);
                } else {
                    n0 = n1 = n2 = fn / len;
                }
                patchTris.push_back({p0, p1, p2, n0, n1, n2, fn / len, len * 0.5f, 0.0f, 0.0f, 0.0f, 0.0f});
            }
        };

        for (int f : patchFaces) {
            uint32_t v0 = mesh.faces[f * 4 + 0];
            uint32_t v1 = mesh.faces[f * 4 + 1];
            uint32_t v2 = mesh.faces[f * 4 + 2];
            uint32_t v3 = mesh.faces[f * 4 + 3];

            addPatchTri(v0, v1, v2);
            patchUniqueVerts.insert(v0);
            patchUniqueVerts.insert(v1);
            patchUniqueVerts.insert(v2);

            if (v3 != TRI_INDEX) {
                addPatchTri(v0, v2, v3);
                patchUniqueVerts.insert(v3);
            }
        }

        for (uint32_t vi : patchUniqueVerts) {
            glm::vec3 p(mesh.verts[vi * 3], mesh.verts[vi * 3 + 1], mesh.verts[vi * 3 + 2]);
            patchBBoxMin = glm::min(patchBBoxMin, p);
            patchBBoxMax = glm::max(patchBBoxMax, p);
        }

        glm::vec3 patchNormal(0.0f);
        glm::vec3 patchCenter(0.0f);
        float patchArea = 0.0f;
        for (const auto& tri : patchTris) {
            patchNormal += tri.normal * tri.area;
            patchCenter += (tri.p0 + tri.p1 + tri.p2) * (tri.area / 3.0f);
            patchArea += tri.area;
        }

        if (patchArea > 1e-6f) {
            patchNormal = glm::normalize(patchNormal);
            patchCenter /= patchArea;
        } else {
            patchNormal = glm::vec3(0.0f, 1.0f, 0.0f);
        }

        float minAngleDeg = 180.0f;
        float maxAngleDeg = 0.0f;
        float sumAngleWeighted = 0.0f;
        float areaUnder30 = 0.0f, area30to60 = 0.0f, area60to80 = 0.0f, areaOver80 = 0.0f;
        int countUnder30 = 0, count30to60 = 0, count60to80 = 0, countOver80 = 0;

        for (const auto& tri : patchTris) {
            float dotN = std::clamp(glm::dot(tri.normal, patchNormal), -1.0f, 1.0f);
            float angleDeg = glm::degrees(std::acos(dotN));
            minAngleDeg = std::min(minAngleDeg, angleDeg);
            maxAngleDeg = std::max(maxAngleDeg, angleDeg);
            sumAngleWeighted += angleDeg * tri.area;

            if (angleDeg < 30.0f) { countUnder30++; areaUnder30 += tri.area; }
            else if (angleDeg < 60.0f) { count30to60++; area30to60 += tri.area; }
            else if (angleDeg < 80.0f) { count60to80++; area60to80 += tri.area; }
            else { countOver80++; areaOver80 += tri.area; }
        }
        float avgAngleDeg = (patchArea > 1e-6f) ? (sumAngleWeighted / patchArea) : 0.0f;

        float depthMin = 1e9f;
        float depthMax = -1e9f;
        for (uint32_t vi : patchUniqueVerts) {
            glm::vec3 p(mesh.verts[vi * 3], mesh.verts[vi * 3 + 1], mesh.verts[vi * 3 + 2]);
            float d = glm::dot(p - patchCenter, patchNormal);
            depthMin = std::min(depthMin, d);
            depthMax = std::max(depthMax, d);
        }
        float depthSpan = depthMax - depthMin;

        sculpt_log("[QUAD CAP PATCH #%zu] Area: %.2f mm2 | BBox Extent: [%.2f, %.2f, %.2f] | Depth Span: %.2f mm\n",
                   patchIdx + 1, patchArea,
                   patchBBoxMax.x - patchBBoxMin.x, patchBBoxMax.y - patchBBoxMin.y, patchBBoxMax.z - patchBBoxMin.z,
                   depthSpan);
        sculpt_log("[QUAD CAP PATCH #%zu] Normals vs Avg [%.2f, %.2f, %.2f]: min=%.1f deg, max=%.1f deg, avg=%.1f deg\n",
                   patchIdx + 1, patchNormal.x, patchNormal.y, patchNormal.z, minAngleDeg, maxAngleDeg, avgAngleDeg);
        sculpt_log("[QUAD CAP PATCH #%zu] Angle Distribution: <30 deg: %.1f%% (%d tris) | 30-60: %.1f%% (%d) | 60-80: %.1f%% (%d) | >80: %.1f%% (%d tris, %.1f mm2)\n",
                   patchIdx + 1,
                   patchArea > 0 ? (areaUnder30 * 100.0f / patchArea) : 0.0f, countUnder30,
                   patchArea > 0 ? (area30to60 * 100.0f / patchArea) : 0.0f, count30to60,
                   patchArea > 0 ? (area60to80 * 100.0f / patchArea) : 0.0f, count60to80,
                   patchArea > 0 ? (areaOver80 * 100.0f / patchArea) : 0.0f, countOver80, areaOver80);
        if (maxAngleDeg > 75.0f && areaOver80 > 0.15f * patchArea) {
            float worstCos = std::max(0.01f, std::cos(glm::radians(std::min(89.0f, maxAngleDeg))));
            sculpt_log("[QUAD CAP PATCH #%zu] Non-planar 3D volume cut patch with steep lateral walls (tilt %.1f deg, steep area %.1f%%, 2D->3D stretch up to %.1fx). Preserving Manifold regular 3D grid.\n",
                       patchIdx + 1, maxAngleDeg, areaOver80 * 100.0f / patchArea, 1.0f / worstCos);
            fallbackPatch("Non-planar 3D volume cut patch with steep lateral walls; preserved Manifold regular 3D grid");
            continue;
        }

        // Extract boundary halfedges of this patch
        std::unordered_map<EdgeKey, int, EdgeKeyHash> patchDirectedEdges;
        patchDirectedEdges.reserve(patchFaces.size() * 3);
        for (int f : patchFaces) {
            uint32_t v0 = mesh.faces[f * 4 + 0];
            uint32_t v1 = mesh.faces[f * 4 + 1];
            uint32_t v2 = mesh.faces[f * 4 + 2];
            uint32_t v3 = mesh.faces[f * 4 + 3];

            patchDirectedEdges[{v0, v1}] = f;
            patchDirectedEdges[{v1, v2}] = f;
            if (v3 != TRI_INDEX) {
                patchDirectedEdges[{v2, v3}] = f;
                patchDirectedEdges[{v3, v0}] = f;
            } else {
                patchDirectedEdges[{v2, v0}] = f;
            }
        }

        std::unordered_map<uint32_t, uint32_t> nextVertex;
        for (const auto& [edge, fIdx] : patchDirectedEdges) {
            if (patchDirectedEdges.find({edge.v, edge.u}) == patchDirectedEdges.end()) {
                nextVertex[edge.u] = edge.v;
            }
        }

        std::vector<std::vector<uint32_t>> patchLoops;
        std::unordered_set<uint32_t> visitedVerts;
        for (const auto& [startV, _] : nextVertex) {
            if (visitedVerts.count(startV)) continue;
            std::vector<uint32_t> loop;
            uint32_t curr = startV;
            bool closed = false;
            while (visitedVerts.find(curr) == visitedVerts.end()) {
                visitedVerts.insert(curr);
                loop.push_back(curr);
                auto it = nextVertex.find(curr);
                if (it == nextVertex.end()) break;
                curr = it->second;
                if (curr == startV) { closed = true; break; }
            }
            if (closed && loop.size() >= 3) {
                patchLoops.push_back(std::move(loop));
            }
        }

        sculpt_log("[QUAD CAP PATCH #%zu] Detected %zu boundary loop(s)\n", patchIdx + 1, patchLoops.size());

        // Analyze boundary dihedral angles and existing polygroup seams
        {
            std::unordered_set<int> patchFaceSet(patchFaces.begin(), patchFaces.end());
            std::unordered_map<EdgeKey, int, EdgeKeyHash> outsideEdges;
            outsideEdges.reserve(mesh.nbFaces * 3);
            for (int f = 0; f < mesh.nbFaces; ++f) {
                if (patchFaceSet.find(f) != patchFaceSet.end()) continue;
                uint32_t ov0 = mesh.faces[f * 4 + 0];
                uint32_t ov1 = mesh.faces[f * 4 + 1];
                uint32_t ov2 = mesh.faces[f * 4 + 2];
                uint32_t ov3 = mesh.faces[f * 4 + 3];
                outsideEdges[{ov0, ov1}] = f;
                outsideEdges[{ov1, ov2}] = f;
                if (ov3 != TRI_INDEX) {
                    outsideEdges[{ov2, ov3}] = f;
                    outsideEdges[{ov3, ov0}] = f;
                } else {
                    outsideEdges[{ov2, ov0}] = f;
                }
            }

            int bUnder30 = 0, b30to60 = 0, b60to90 = 0, b90to120 = 0, bOver120 = 0;
            float minDihedral = 180.0f, maxDihedral = 0.0f, sumDihedral = 0.0f;
            int numBoundaryEdges = 0;

            struct BoundaryCorner {
                uint32_t vid;
                float x, y, z;
                float maxDihedral;
                std::string groupsStr;
            };
            std::unordered_map<uint32_t, std::unordered_set<uint32_t>> vertOutGroups;
            std::unordered_map<uint32_t, float> vertMaxDihedral;

            for (const auto& [edge, fIdx] : patchDirectedEdges) {
                if (patchDirectedEdges.find({edge.v, edge.u}) != patchDirectedEdges.end()) continue;
                // Boundary edge (edge.u -> edge.v). The matching outside half-edge is edge.v -> edge.u
                auto it = outsideEdges.find({edge.v, edge.u});
                if (it == outsideEdges.end()) continue;
                int outF = it->second;

                uint32_t outGid = (outF < (int)mesh.faceGroups.size()) ? mesh.faceGroups[outF] : 0;
                vertOutGroups[edge.u].insert(outGid);
                vertOutGroups[edge.v].insert(outGid);

                uint32_t f0 = mesh.faces[outF * 4 + 0];
                uint32_t f1 = mesh.faces[outF * 4 + 1];
                uint32_t f2 = mesh.faces[outF * 4 + 2];
                uint32_t f3 = mesh.faces[outF * 4 + 3];
                glm::vec3 op0(mesh.verts[f0 * 3], mesh.verts[f0 * 3 + 1], mesh.verts[f0 * 3 + 2]);
                glm::vec3 op1(mesh.verts[f1 * 3], mesh.verts[f1 * 3 + 1], mesh.verts[f1 * 3 + 2]);
                glm::vec3 op2(mesh.verts[f2 * 3], mesh.verts[f2 * 3 + 1], mesh.verts[f2 * 3 + 2]);
                glm::vec3 ofn = glm::cross(op1 - op0, op2 - op0);
                if (f3 != TRI_INDEX) {
                    glm::vec3 op3(mesh.verts[f3 * 3], mesh.verts[f3 * 3 + 1], mesh.verts[f3 * 3 + 2]);
                    ofn += glm::cross(op2 - op0, op3 - op0);
                }
                float oflen = glm::length(ofn);
                if (oflen > 1e-6f) ofn /= oflen;

                float dotDihedral = std::clamp(glm::dot(patchNormal, ofn), -1.0f, 1.0f);
                float dihedralDeg = glm::degrees(std::acos(dotDihedral));

                numBoundaryEdges++;
                sumDihedral += dihedralDeg;
                if (dihedralDeg < minDihedral) minDihedral = dihedralDeg;
                if (dihedralDeg > maxDihedral) maxDihedral = dihedralDeg;

                if (dihedralDeg < 30.0f) bUnder30++;
                else if (dihedralDeg < 60.0f) b30to60++;
                else if (dihedralDeg < 90.0f) b60to90++;
                else if (dihedralDeg < 120.0f) b90to120++;
                else bOver120++;

                vertMaxDihedral[edge.u] = std::max(vertMaxDihedral[edge.u], dihedralDeg);
                vertMaxDihedral[edge.v] = std::max(vertMaxDihedral[edge.v], dihedralDeg);
            }

            float avgDihedral = numBoundaryEdges > 0 ? (sumDihedral / (float)numBoundaryEdges) : 0.0f;
            sculpt_log("[QUAD CAP PATCH #%zu] Boundary Dihedral Angles (%d edges): avg=%.1f deg [min: %.1f, max: %.1f] | <30°: %d, 30-60°: %d, 60-90°: %d, 90-120°: %d, >120°: %d\n",
                       patchIdx + 1, numBoundaryEdges, avgDihedral, minDihedral, maxDihedral,
                       bUnder30, b30to60, b60to90, b90to120, bOver120);

            std::vector<BoundaryCorner> corners;
            for (const auto& [vid, gSet] : vertOutGroups) {
                if (gSet.size() >= 2) {
                    std::string gStr;
                    for (uint32_t gid : gSet) {
                        if (!gStr.empty()) gStr += ", ";
                        gStr += std::to_string(gid);
                    }
                    corners.push_back({
                        vid,
                        mesh.verts[vid * 3], mesh.verts[vid * 3 + 1], mesh.verts[vid * 3 + 2],
                        vertMaxDihedral[vid],
                        gStr
                    });
                }
            }

            if (!corners.empty()) {
                std::sort(corners.begin(), corners.end(), [](const BoundaryCorner& a, const BoundaryCorner& b) {
                    return a.maxDihedral > b.maxDihedral;
                });
                sculpt_log("[QUAD CAP PATCH #%zu] Boundary intersects %zu existing seam(s)/corner(s):\n",
                           patchIdx + 1, corners.size());
                int show = std::min((int)corners.size(), 6);
                for (int k = 0; k < show; ++k) {
                    const auto& c = corners[k];
                    sculpt_log("  Corner Vert #%u at (%.2f, %.2f, %.2f) | Outside Groups: [%s] | Max Dihedral: %.1f deg\n",
                               c.vid, c.x, c.y, c.z, c.groupsStr.c_str(), c.maxDihedral);
                }
            }
        }

        if (patchLoops.empty()) {
            fallbackPatch("No closed boundary loop detected");
            continue;
        }

        // 1. Identify outer boundary loop (largest 2D area) and hole loops
        glm::vec3 up0 = (std::abs(patchNormal.y) < 0.9f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        glm::vec3 U0 = glm::normalize(glm::cross(up0, patchNormal));
        glm::vec3 V0 = glm::cross(patchNormal, U0);

        size_t outerLoopIdx = 0;
        float maxLoopArea = -1.0f;
        for (size_t l = 0; l < patchLoops.size(); ++l) {
            const auto& lp = patchLoops[l];
            float area = 0.0f;
            for (size_t i = 0; i < lp.size(); ++i) {
                size_t j = (i + 1) % lp.size();
                glm::vec3 pi(mesh.verts[lp[i] * 3], mesh.verts[lp[i] * 3 + 1], mesh.verts[lp[i] * 3 + 2]);
                glm::vec3 pj(mesh.verts[lp[j] * 3], mesh.verts[lp[j] * 3 + 1], mesh.verts[lp[j] * 3 + 2]);
                glm::vec2 ui(glm::dot(pi, U0), glm::dot(pi, V0));
                glm::vec2 uj(glm::dot(pj, U0), glm::dot(pj, V0));
                area += (ui.x * uj.y - uj.x * ui.y);
            }
            float absA = std::abs(area) * 0.5f;
            if (absA > maxLoopArea) {
                maxLoopArea = absA;
                outerLoopIdx = l;
            }
        }
        if (outerLoopIdx != 0) {
            std::swap(patchLoops[0], patchLoops[outerLoopIdx]);
        }

        const size_t K0 = patchLoops[0].size();

        // Loop centroid and Newell normal of outer loop
        glm::vec3 loopCenter(0.0f);
        glm::vec3 loopNewell(0.0f);
        float totalEdgeLen = 0.0f;
        float minBoundaryEdge = 1e9f;
        float maxBoundaryEdge = 0.0f;
        for (size_t i = 0; i < K0; ++i) {
            size_t j = (i + 1) % K0;
            uint32_t vi = patchLoops[0][i];
            uint32_t vj = patchLoops[0][j];
            glm::vec3 pi(mesh.verts[vi * 3], mesh.verts[vi * 3 + 1], mesh.verts[vi * 3 + 2]);
            glm::vec3 pj(mesh.verts[vj * 3], mesh.verts[vj * 3 + 1], mesh.verts[vj * 3 + 2]);
            loopCenter += pi;
            float eLen = glm::distance(pi, pj);
            totalEdgeLen += eLen;
            minBoundaryEdge = std::min(minBoundaryEdge, eLen);
            maxBoundaryEdge = std::max(maxBoundaryEdge, eLen);
            loopNewell.x += (pi.y - pj.y) * (pi.z + pj.z);
            loopNewell.y += (pi.z - pj.z) * (pi.x + pj.x);
            loopNewell.z += (pi.x - pj.x) * (pi.y + pj.y);
        }
        loopCenter /= static_cast<float>(K0);

        glm::vec3 normal = patchNormal;
        if (glm::length(loopNewell) > 1e-6f && glm::dot(normal, loopNewell) < 0.0f) {
            normal = -normal;
        }

        float loopAvgEdgeLen = totalEdgeLen / static_cast<float>(K0);

        // 2. PCA Grid Alignment: compute major axis of outer loop relative to loopCenter
        float covXX = 0.0f, covYY = 0.0f, covXY = 0.0f;
        for (uint32_t vi : patchLoops[0]) {
            glm::vec3 p(mesh.verts[vi * 3], mesh.verts[vi * 3 + 1], mesh.verts[vi * 3 + 2]);
            glm::vec3 rel = p - loopCenter;
            float x = glm::dot(rel, U0);
            float y = glm::dot(rel, V0);
            covXX += x * x;
            covYY += y * y;
            covXY += x * y;
        }
        float pcaAngle = 0.5f * std::atan2(2.0f * covXY, covXX - covYY);
        glm::vec3 U = glm::normalize(std::cos(pcaAngle) * U0 + std::sin(pcaAngle) * V0);
        glm::vec3 V = glm::cross(normal, U);

        // Estimate bounds for step calculation
        float minU = 1e9f, minV = 1e9f, maxU = -1e9f, maxV = -1e9f;
        for (uint32_t vi : patchLoops[0]) {
            glm::vec3 p(mesh.verts[vi * 3], mesh.verts[vi * 3 + 1], mesh.verts[vi * 3 + 2]);
            glm::vec3 rel = p - loopCenter;
            float u = glm::dot(rel, U);
            float v = glm::dot(rel, V);
            minU = std::min(minU, u);
            minV = std::min(minV, v);
            maxU = std::max(maxU, u);
            maxV = std::max(maxV, v);
        }

        float patchWidth = maxU - minU;
        float patchHeight = maxV - minV;
        float patchMinDim = std::min(patchWidth, patchHeight);

        float step = targetEdgeLen;
        const char* stepReason = "mesh targetEdgeLen";

        if (step <= 1e-4f || step > loopAvgEdgeLen * 2.0f || (step * 1.5f > patchMinDim && patchMinDim > loopAvgEdgeLen)) {
            float idealStep = loopAvgEdgeLen * 1.25f;
            if (patchMinDim > 0.0f && idealStep * 2.0f > patchMinDim) {
                idealStep = patchMinDim * 0.40f;
                stepReason = "adapted to patch width";
            } else {
                stepReason = "adapted to boundary edge length";
            }
            step = idealStep;
        }

        float minStepAllowed = std::max(0.15f, loopAvgEdgeLen * 0.4f);
        step = std::clamp(step, minStepAllowed, 50.0f);
        float margin = step * 0.38f;

        // 3. Boundary Loop Micro-Edge Welding:
        // Eliminate ultra-short edges (< weldDist) produced by boolean cuts.
        float weldDist = std::min(0.08f, 0.12f * step);
        std::unordered_map<uint32_t, uint32_t> weldMap;

        for (auto& lp : patchLoops) {
            std::vector<uint32_t> cleanLp;
            for (uint32_t vCurr : lp) {
                if (cleanLp.empty()) {
                    cleanLp.push_back(vCurr);
                    continue;
                }
                uint32_t vPrev = cleanLp.back();
                glm::vec3 pPrev(mesh.verts[vPrev * 3], mesh.verts[vPrev * 3 + 1], mesh.verts[vPrev * 3 + 2]);
                glm::vec3 pCurr(mesh.verts[vCurr * 3], mesh.verts[vCurr * 3 + 1], mesh.verts[vCurr * 3 + 2]);
                if (glm::distance(pPrev, pCurr) < weldDist) {
                    weldMap[vCurr] = vPrev;
                } else {
                    cleanLp.push_back(vCurr);
                }
            }
            if (cleanLp.size() >= 4) {
                uint32_t vFirst = cleanLp.front();
                uint32_t vLast = cleanLp.back();
                glm::vec3 pFirst(mesh.verts[vFirst * 3], mesh.verts[vFirst * 3 + 1], mesh.verts[vFirst * 3 + 2]);
                glm::vec3 pLast(mesh.verts[vLast * 3], mesh.verts[vLast * 3 + 1], mesh.verts[vLast * 3 + 2]);
                if (glm::distance(pFirst, pLast) < weldDist) {
                    weldMap[vLast] = vFirst;
                    cleanLp.pop_back();
                }
            }
            if (cleanLp.size() >= 3) {
                lp = std::move(cleanLp);
            }
        }

        if (!weldMap.empty()) {
            for (auto& [src, dst] : weldMap) {
                uint32_t target = dst;
                while (weldMap.find(target) != weldMap.end()) {
                    target = weldMap[target];
                }
                dst = target;
            }

            // Remap vertex indices in newFaces
            for (auto& vid : newFaces) {
                if (vid != TRI_INDEX) {
                    auto it = weldMap.find(vid);
                    if (it != weldMap.end()) {
                        vid = it->second;
                    }
                }
            }

            // Clean up and compact newFaces and newFaceGroups:
            // Remove zero-area degenerate triangles, convert collapsed quads into valid triangles
            std::vector<uint32_t> compactedFaces;
            std::vector<uint32_t> compactedFaceGroups;
            compactedFaces.reserve(newFaces.size());
            compactedFaceGroups.reserve(newFaceGroups.size());

            size_t nbOrigF = newFaces.size() / 4;
            int degenerateRemoved = 0;
            int quadsToTris = 0;

            for (size_t f = 0; f < nbOrigF; ++f) {
                uint32_t v0 = newFaces[f * 4 + 0];
                uint32_t v1 = newFaces[f * 4 + 1];
                uint32_t v2 = newFaces[f * 4 + 2];
                uint32_t v3 = newFaces[f * 4 + 3];
                uint32_t gid = (f < newFaceGroups.size()) ? newFaceGroups[f] : 0;

                if (v3 == TRI_INDEX) {
                    // Triangle: must have 3 distinct vertices
                    if (v0 != v1 && v1 != v2 && v2 != v0) {
                        compactedFaces.push_back(v0);
                        compactedFaces.push_back(v1);
                        compactedFaces.push_back(v2);
                        compactedFaces.push_back(TRI_INDEX);
                        compactedFaceGroups.push_back(gid);
                    } else {
                        degenerateRemoved++;
                    }
                } else {
                    // Quad:
                    // Check duplicate vertices
                    if (v0 == v1 && v1 != v2 && v2 != v3 && v3 != v0) {
                        // v0 == v1: turns into triangle (v0, v2, v3)
                        compactedFaces.push_back(v0);
                        compactedFaces.push_back(v2);
                        compactedFaces.push_back(v3);
                        compactedFaces.push_back(TRI_INDEX);
                        compactedFaceGroups.push_back(gid);
                        quadsToTris++;
                    } else if (v1 == v2 && v0 != v1 && v2 != v3 && v3 != v0) {
                        // v1 == v2: turns into triangle (v0, v1, v3)
                        compactedFaces.push_back(v0);
                        compactedFaces.push_back(v1);
                        compactedFaces.push_back(v3);
                        compactedFaces.push_back(TRI_INDEX);
                        compactedFaceGroups.push_back(gid);
                        quadsToTris++;
                    } else if (v2 == v3 && v0 != v1 && v1 != v2 && v3 != v0) {
                        // v2 == v3: turns into triangle (v0, v1, v2)
                        compactedFaces.push_back(v0);
                        compactedFaces.push_back(v1);
                        compactedFaces.push_back(v2);
                        compactedFaces.push_back(TRI_INDEX);
                        compactedFaceGroups.push_back(gid);
                        quadsToTris++;
                    } else if (v3 == v0 && v0 != v1 && v1 != v2 && v2 != v3) {
                        // v3 == v0: turns into triangle (v0, v1, v2)
                        compactedFaces.push_back(v0);
                        compactedFaces.push_back(v1);
                        compactedFaces.push_back(v2);
                        compactedFaces.push_back(TRI_INDEX);
                        compactedFaceGroups.push_back(gid);
                        quadsToTris++;
                    } else if (v0 != v1 && v1 != v2 && v2 != v3 && v3 != v0 && v0 != v2 && v1 != v3) {
                        // All 4 distinct and no diagonal collapse
                        compactedFaces.push_back(v0);
                        compactedFaces.push_back(v1);
                        compactedFaces.push_back(v2);
                        compactedFaces.push_back(v3);
                        compactedFaceGroups.push_back(gid);
                    } else {
                        // More than 1 duplicate vertex or collapsed diagonals
                        degenerateRemoved++;
                    }
                }
            }

            newFaces = std::move(compactedFaces);
            newFaceGroups = std::move(compactedFaceGroups);

            sculpt_log("[QUAD CAP PATCH #%zu] Welded %zu micro-vertices (< %.3f mm) on boundary. Cleaned %d degenerate faces, converted %d collapsed quads to tris.\n",
                       patchIdx + 1, weldMap.size(), weldDist, degenerateRemoved, quadsToTris);
        }

        // Build 2D polygons for outer loop and all holes
        std::vector<std::vector<glm::vec2>> allPoly2D(patchLoops.size());
        struct RefPt { glm::vec2 p; float h; };
        std::vector<RefPt> allRefPts;

        minU = 1e9f; minV = 1e9f; maxU = -1e9f; maxV = -1e9f;

        for (size_t l = 0; l < patchLoops.size(); ++l) {
            const auto& lp = patchLoops[l];
            allPoly2D[l].resize(lp.size());
            for (size_t i = 0; i < lp.size(); ++i) {
                uint32_t vi = lp[i];
                glm::vec3 p(mesh.verts[vi * 3], mesh.verts[vi * 3 + 1], mesh.verts[vi * 3 + 2]);
                glm::vec3 rel = p - loopCenter;
                glm::vec2 p2D(glm::dot(rel, U), glm::dot(rel, V));
                allPoly2D[l][i] = p2D;
                float h = glm::dot(rel, normal);
                allRefPts.push_back({p2D, h});
                if (l == 0) {
                    minU = std::min(minU, p2D.x);
                    minV = std::min(minV, p2D.y);
                    maxU = std::max(maxU, p2D.x);
                    maxV = std::max(maxV, p2D.y);
                }
            }
        }

        float signedArea = 0.0f;
        for (size_t i = 0; i < allPoly2D[0].size(); ++i) {
            size_t j = (i + 1) % allPoly2D[0].size();
            signedArea += (allPoly2D[0][i].x * allPoly2D[0][j].y - allPoly2D[0][j].x * allPoly2D[0][i].y);
        }
        if (std::abs(signedArea) < 1e-4f) {
            fallbackPatch("2D projected boundary area is near zero");
            continue;
        }

        if (signedArea < 0.0f) {
            V = -V;
            normal = -normal;
            minV = 1e9f; maxV = -1e9f;
            for (auto& lp2d : allPoly2D) {
                for (auto& pt : lp2d) {
                    pt.y = -pt.y;
                }
            }
            for (auto& rp : allRefPts) {
                rp.p.y = -rp.p.y;
                rp.h = -rp.h;
            }
            for (const auto& pt : allPoly2D[0]) {
                minV = std::min(minV, pt.y);
                maxV = std::max(maxV, pt.y);
            }
        }

        // Test outer loop self-intersection
        int segA = -1, segB = -1;
        if (polygonHasSelfIntersections(allPoly2D[0], segA, segB)) {
            char failBuf[200];
            snprintf(failBuf, sizeof(failBuf), "2D boundary loop self-intersects at segments (%d, %d) and (%d, %d)",
                     segA, (segA + 1) % (int)allPoly2D[0].size(), segB, (segB + 1) % (int)allPoly2D[0].size());
            fallbackPatch(failBuf);
            continue;
        }

        int shortEdgesCount2D = 0;
        for (size_t i = 0; i < allPoly2D[0].size(); ++i) {
            size_t j = (i + 1) % allPoly2D[0].size();
            float eLen = glm::length(allPoly2D[0][j] - allPoly2D[0][i]);
            if (eLen < 0.20f * step) {
                shortEdgesCount2D++;
            }
        }

        float postWeldPerim3D = 0.0f;
        float postWeldMinEdge3D = 1e9f;
        float postWeldMaxEdge3D = 0.0f;
        int postWeldShortEdges3D = 0;
        for (size_t i = 0; i < patchLoops[0].size(); ++i) {
            size_t j = (i + 1) % patchLoops[0].size();
            glm::vec3 pi(mesh.verts[patchLoops[0][i] * 3], mesh.verts[patchLoops[0][i] * 3 + 1], mesh.verts[patchLoops[0][i] * 3 + 2]);
            glm::vec3 pj(mesh.verts[patchLoops[0][j] * 3], mesh.verts[patchLoops[0][j] * 3 + 1], mesh.verts[patchLoops[0][j] * 3 + 2]);
            float eLen = glm::distance(pi, pj);
            postWeldPerim3D += eLen;
            postWeldMinEdge3D = std::min(postWeldMinEdge3D, eLen);
            postWeldMaxEdge3D = std::max(postWeldMaxEdge3D, eLen);
            if (eLen < 0.20f * step) {
                postWeldShortEdges3D++;
            }
        }
        float postWeldAvgEdge3D = patchLoops[0].empty() ? 0.0f : (postWeldPerim3D / (float)patchLoops[0].size());

        sculpt_log("[QUAD CAP PATCH #%zu] Boundary 3D (post-weld): %zu verts (%zu loops), perim: %.2f mm, avgEdge: %.3f mm [min: %.3f, max: %.3f, <0.2*step: %d]\n",
                   patchIdx + 1, patchLoops[0].size(), patchLoops.size(), postWeldPerim3D, postWeldAvgEdge3D, postWeldMinEdge3D, postWeldMaxEdge3D, postWeldShortEdges3D);
        sculpt_log("[QUAD CAP PATCH #%zu] Boundary 2D: Size: [%.2f x %.2f mm] | 2D edges <0.2*step: %d\n",
                   patchIdx + 1, patchWidth, patchHeight, shortEdgesCount2D);
        sculpt_log("[QUAD CAP PATCH #%zu] Step: %.3f mm (%s) | Margin: %.3f mm (PCA aligned)\n",
                   patchIdx + 1, step, stepReason, margin);

        // Precompute 2D AABB of each patch triangle in (U, V) basis
        for (auto& tri : patchTris) {
            glm::vec2 u0(glm::dot(tri.p0 - loopCenter, U), glm::dot(tri.p0 - loopCenter, V));
            glm::vec2 u1(glm::dot(tri.p1 - loopCenter, U), glm::dot(tri.p1 - loopCenter, V));
            glm::vec2 u2(glm::dot(tri.p2 - loopCenter, U), glm::dot(tri.p2 - loopCenter, V));
            tri.minU = std::min({u0.x, u1.x, u2.x}) - 1e-3f;
            tri.maxU = std::max({u0.x, u1.x, u2.x}) + 1e-3f;
            tri.minV = std::min({u0.y, u1.y, u2.y}) - 1e-3f;
            tri.maxV = std::max({u0.y, u1.y, u2.y}) + 1e-3f;
        }

        auto pointInPatchDomain = [&](const glm::vec2& pt) -> bool {
            if (!pointInPoly2D(pt, allPoly2D[0])) return false;
            for (size_t h = 1; h < allPoly2D.size(); ++h) {
                if (pointInPoly2D(pt, allPoly2D[h])) return false;
            }
            return true;
        };

        auto distToPatchBoundary = [&](const glm::vec2& pt) -> float {
            float minD = distToPolyBoundary(pt, allPoly2D[0]);
            for (size_t h = 1; h < allPoly2D.size(); ++h) {
                minD = std::min(minD, distToPolyBoundary(pt, allPoly2D[h]));
            }
            return minD;
        };

        struct GridPt {
            int gx, gy;
            uint32_t meshVid;
            glm::vec2 p2D;
            glm::vec3 p3D;
            glm::vec3 n3D;
        };

        std::map<std::pair<int, int>, uint32_t> gridPtMap;
        std::vector<GridPt> gridPoints;
        std::vector<StagedVert> loopNewVerts;

        uint32_t baseVid = static_cast<uint32_t>(mesh.verts.size() / 3 + stagedVerts.size());

        int gxMin = static_cast<int>(std::floor(minU / step));
        int gxMax = static_cast<int>(std::ceil(maxU / step));
        int gyMin = static_cast<int>(std::floor(minV / step));
        int gyMax = static_cast<int>(std::ceil(maxV / step));

        int64_t totalGridCells = (int64_t)(gxMax - gxMin + 1) * (int64_t)(gyMax - gyMin + 1);
        if (totalGridCells > 50000) {
            step = std::max(step, std::max(patchWidth, patchHeight) / 200.0f);
            gxMin = static_cast<int>(std::floor(minU / step));
            gxMax = static_cast<int>(std::ceil(maxU / step));
            gyMin = static_cast<int>(std::floor(minV / step));
            gyMax = static_cast<int>(std::ceil(maxV / step));
        }

        int rayHitsCount = 0;
        int fallbackCount = 0;
        int rejectedByMargin = 0;
        float raySpan = std::max(200.0f, depthSpan * 3.0f);

        for (int gy = gyMin; gy <= gyMax; ++gy) {
            for (int gx = gxMin; gx <= gxMax; ++gx) {
                glm::vec2 pt(gx * step, gy * step);
                if (pointInPatchDomain(pt)) {
                    if (distToPatchBoundary(pt) < margin) {
                        rejectedByMargin++;
                        continue;
                    }

                    float sumW = 0.0f, sumH = 0.0f;
                    for (const auto& rp : allRefPts) {
                        float d2 = glm::dot(pt - rp.p, pt - rp.p);
                        if (d2 < 1e-4f) { sumH = rp.h; sumW = 1.0f; break; }
                        float w = 1.0f / (d2 * d2);
                        sumW += w;
                        sumH += w * rp.h;
                    }
                    float hVal = (sumW > 1e-8f) ? (sumH / sumW) : 0.0f;
                    glm::vec3 pPlane = loopCenter + pt.x * U + pt.y * V;

                    float bestDiff = 1e9f;
                    glm::vec3 p3D = pPlane + hVal * normal;
                    glm::vec3 n3D = normal;
                    bool foundHit = false;

                    for (const auto& tri : patchTris) {
                        if (pt.x < tri.minU || pt.x > tri.maxU || pt.y < tri.minV || pt.y > tri.maxV) continue;
                        float t, u, v;
                        if (intersectRayTriangle(pPlane - normal * raySpan, normal, tri.p0, tri.p1, tri.p2, t, u, v)) {
                            if (t >= 0.0f && t <= raySpan * 2.0f) {
                                float depth = t - raySpan;
                                float diff = std::abs(depth - hVal);
                                if (diff < bestDiff) {
                                    bestDiff = diff;
                                    p3D = (1.0f - u - v) * tri.p0 + u * tri.p1 + v * tri.p2;
                                    glm::vec3 interpN = (1.0f - u - v) * tri.n0 + u * tri.n1 + v * tri.n2;
                                    float lenN = glm::length(interpN);
                                    n3D = (lenN > 1e-6f) ? (interpN / lenN) : tri.normal;
                                    foundHit = true;
                                }
                            }
                        }
                    }

                    if (foundHit) {
                        rayHitsCount++;
                    } else {
                        fallbackCount++;
                        float bestD2 = 1e18f;
                        glm::vec3 pGuide = pPlane + hVal * normal;
                        for (const auto& tri : patchTris) {
                            glm::vec3 cp = closestPointOnTriangle(pGuide, tri.p0, tri.p1, tri.p2);
                            glm::vec3 diff = cp - pGuide;
                            float d2 = glm::dot(diff, diff);
                            if (d2 < bestD2) {
                                bestD2 = d2;
                                p3D = cp;
                                n3D = tri.normal;
                            }
                        }
                    }

                    uint32_t newVid = baseVid + static_cast<uint32_t>(loopNewVerts.size());
                    loopNewVerts.push_back({p3D, n3D});

                    gridPtMap[{gx, gy}] = newVid;
                    gridPoints.push_back({gx, gy, newVid, pt, p3D, n3D});
                }
            }
        }

        // Deviation analysis and 3D stretch analysis
        float minDistToOrig = 1e9f, maxDistToOrig = -1e9f, sumDistToOrig = 0.0f;
        int deviatingCount = 0;
        float maxStretch = 1.0f, sumStretch = 0.0f;
        float maxTiltDeg = 0.0f, sumTiltDeg = 0.0f;
        int highStretchCount = 0;

        for (const auto& gp : gridPoints) {
            const glm::vec3& pGrid = gp.p3D;
            float closestDist2 = 1e18f;
            for (const auto& tri : patchTris) {
                glm::vec3 cp = closestPointOnTriangle(pGrid, tri.p0, tri.p1, tri.p2);
                glm::vec3 diff = pGrid - cp;
                float d2 = glm::dot(diff, diff);
                if (d2 < closestDist2) closestDist2 = d2;
            }
            float dist = std::sqrt(closestDist2);
            minDistToOrig = std::min(minDistToOrig, dist);
            maxDistToOrig = std::max(maxDistToOrig, dist);
            sumDistToOrig += dist;
            if (dist > 0.5f * step) deviatingCount++;

            float dotTilt = std::clamp(glm::dot(gp.n3D, normal), -1.0f, 1.0f);
            float tiltDeg = glm::degrees(std::acos(dotTilt));
            maxTiltDeg = std::max(maxTiltDeg, tiltDeg);
            sumTiltDeg += tiltDeg;
            float sFactor = 1.0f / std::max(0.02f, dotTilt);
            maxStretch = std::max(maxStretch, sFactor);
            sumStretch += sFactor;
            if (sFactor > 2.0f) highStretchCount++;
        }

        float avgDistToOrig = gridPoints.empty() ? 0.0f : (sumDistToOrig / (float)gridPoints.size());
        float avgTiltDeg = gridPoints.empty() ? 0.0f : (sumTiltDeg / (float)gridPoints.size());
        float avgStretch = gridPoints.empty() ? 1.0f : (sumStretch / (float)gridPoints.size());

        sculpt_log("[QUAD CAP PATCH #%zu] Grid points: %zu (rayHits: %d, fallbackCP: %d, marginRejected: %d)\n",
                   patchIdx + 1, gridPoints.size(), rayHitsCount, fallbackCount, rejectedByMargin);
        if (!gridPoints.empty()) {
            sculpt_log("[QUAD CAP PATCH #%zu] Surface deviation: min=%.3f, max=%.3f, avg=%.3f mm (Points > 0.5*step: %d / %zu, %.1f%%)\n",
                       patchIdx + 1, minDistToOrig, maxDistToOrig, avgDistToOrig,
                       deviatingCount, gridPoints.size(), (float)deviatingCount * 100.0f / (float)gridPoints.size());
            sculpt_log("[QUAD CAP PATCH #%zu] Grid 3D Stretch: max=%.2fx (avg: %.2fx) | Tilt vs ray: max=%.1f deg, avg=%.1f deg | Steep points (>2.0x): %d (%.1f%%)\n",
                       patchIdx + 1, maxStretch, avgStretch, maxTiltDeg, avgTiltDeg,
                       highStretchCount, (float)highStretchCount * 100.0f / (float)gridPoints.size());
        }

        if (!gridPoints.empty() && deviatingCount > (int)(0.5f * (float)gridPoints.size())) {
            fallbackPatch("Post-projection surface deviation is too high (>50% points deviate > 0.5*step)");
            continue;
        }

        // Identify complete quads from grid
        struct QuadCell {
            uint32_t v00, v10, v11, v01;
            int gx, gy;
        };
        std::vector<QuadCell> quads;
        std::unordered_set<uint64_t> quadCellKeys;

        auto makeCellKey = [](int gx, int gy) -> uint64_t {
            return ((uint64_t)(gx + 100000) << 32) | (uint64_t)(gy + 100000);
        };

        for (const auto& g : gridPoints) {
            int gx = g.gx;
            int gy = g.gy;
            auto it10 = gridPtMap.find({gx + 1, gy});
            auto it11 = gridPtMap.find({gx + 1, gy + 1});
            auto it01 = gridPtMap.find({gx, gy + 1});

            if (it10 != gridPtMap.end() && it11 != gridPtMap.end() && it01 != gridPtMap.end()) {
                quads.push_back({g.meshVid, it10->second, it11->second, it01->second, gx, gy});
                quadCellKeys.insert(makeCellKey(gx, gy));
            }
        }

        // Collect all vertex IDs used by at least one candidate quad
        std::unordered_set<uint32_t> quadUsedVids;
        quadUsedVids.reserve(quads.size() * 4);
        for (const auto& q : quads) {
            quadUsedVids.insert(q.v00);
            quadUsedVids.insert(q.v10);
            quadUsedVids.insert(q.v11);
            quadUsedVids.insert(q.v01);
        }

        int orphanGridPtsPruned = 0;
        for (const auto& gp : gridPoints) {
            if (!quads.empty() && quadUsedVids.find(gp.meshVid) == quadUsedVids.end()) {
                orphanGridPtsPruned++;
            }
        }

        sculpt_log("[QUAD CAP PATCH #%zu] Candidate grid quads: %zu (grid points in quads: %zu, pruned orphans: %d)\n",
                   patchIdx + 1, quads.size(), quadUsedVids.size(), orphanGridPtsPruned);

        // CDT Triangulation for boundary transition stitching
        CDT::Triangulation<float> cdt;
        std::vector<CDT::V2d<float>> cdtVerts;
        cdtVerts.reserve(patchLoops[0].size() * 2 + gridPoints.size());

        std::vector<uint32_t> cdtToMeshVid;
        cdtToMeshVid.reserve(patchLoops[0].size() * 2 + gridPoints.size());

        std::vector<CDT::Edge> cdtEdges;

        for (size_t l = 0; l < patchLoops.size(); ++l) {
            const auto& lp = patchLoops[l];
            const auto& p2d = allPoly2D[l];
            size_t baseIdx = cdtVerts.size();
            for (size_t i = 0; i < lp.size(); ++i) {
                cdtVerts.push_back(CDT::V2d<float>::make(p2d[i].x, p2d[i].y));
                cdtToMeshVid.push_back(lp[i]);
                size_t j = (i + 1) % lp.size();
                cdtEdges.push_back(CDT::Edge(static_cast<CDT::VertInd>(baseIdx + i),
                                             static_cast<CDT::VertInd>(baseIdx + j)));
            }
        }

        for (const auto& gp : gridPoints) {
            if (!quads.empty() && quadUsedVids.find(gp.meshVid) == quadUsedVids.end()) {
                continue; // Skip orphan grid point
            }
            cdtVerts.push_back(CDT::V2d<float>::make(gp.p2D.x, gp.p2D.y));
            cdtToMeshVid.push_back(gp.meshVid);
        }

        auto tCdtStart = std::chrono::high_resolution_clock::now();
        try {
            cdt.insertVertices(cdtVerts);
            cdt.insertEdges(cdtEdges);
            cdt.eraseOuterTrianglesAndHoles();
        } catch (const std::exception& ex) {
            char failBuf[200];
            snprintf(failBuf, sizeof(failBuf), "CDT triangulation exception: %s", ex.what());
            fallbackPatch(failBuf);
            continue;
        } catch (...) {
            fallbackPatch("Unknown CDT triangulation exception");
            continue;
        }
        auto tCdtEnd = std::chrono::high_resolution_clock::now();
        double cdtMs = std::chrono::duration<double, std::milli>(tCdtEnd - tCdtStart).count();

        if (cdt.triangles.empty()) {
            fallbackPatch("CDT produced 0 triangles");
            continue;
        }

        sculpt_log("[QUAD CAP PATCH #%zu] CDT Triangulation: %zu triangles generated in %.2f ms\n",
                   patchIdx + 1, cdt.triangles.size(), cdtMs);

        // Fast O(1) lookup from vertex ID to grid coordinates (gx, gy)
        std::unordered_map<uint32_t, std::pair<int, int>> vidToGrid;
        vidToGrid.reserve(gridPoints.size());
        for (const auto& gp : gridPoints) {
            vidToGrid[gp.meshVid] = {gp.gx, gp.gy};
        }

        std::unordered_map<uint64_t, std::vector<std::array<uint32_t, 3>>> cellTriangles;
        std::vector<std::array<uint32_t, 3>> transitionTris;

        for (const auto& tri : cdt.triangles) {
            if (tri.vertices[0] >= cdtToMeshVid.size() ||
                tri.vertices[1] >= cdtToMeshVid.size() ||
                tri.vertices[2] >= cdtToMeshVid.size()) {
                fallbackPatch("CDT vertex index out of bounds");
                continue;
            }

            uint32_t m0 = cdtToMeshVid[tri.vertices[0]];
            uint32_t m1 = cdtToMeshVid[tri.vertices[1]];
            uint32_t m2 = cdtToMeshVid[tri.vertices[2]];

            bool isQuadTri = false;
            auto it0 = vidToGrid.find(m0);
            auto it1 = vidToGrid.find(m1);
            auto it2 = vidToGrid.find(m2);

            if (it0 != vidToGrid.end() && it1 != vidToGrid.end() && it2 != vidToGrid.end()) {
                int minGx = std::min({it0->second.first, it1->second.first, it2->second.first});
                int maxGx = std::max({it0->second.first, it1->second.first, it2->second.first});
                int minGy = std::min({it0->second.second, it1->second.second, it2->second.second});
                int maxGy = std::max({it0->second.second, it1->second.second, it2->second.second});

                if (maxGx - minGx <= 1 && maxGy - minGy <= 1) {
                    uint64_t candidateKey = makeCellKey(minGx, minGy);
                    if (quadCellKeys.find(candidateKey) != quadCellKeys.end()) {
                        cellTriangles[candidateKey].push_back({m0, m1, m2});
                        isQuadTri = true;
                    }
                }
            }

            if (!isQuadTri) {
                transitionTris.push_back({m0, m1, m2});
            }
        }

        // Inner Grid Quads: accept candidate cells with exactly 2 triangles from CDT
        std::vector<QuadCell> acceptedGridQuads;
        acceptedGridQuads.reserve(quads.size());

        for (const auto& q : quads) {
            uint64_t key = makeCellKey(q.gx, q.gy);
            auto it = cellTriangles.find(key);
            if (it != cellTriangles.end() && it->second.size() == 2) {
                acceptedGridQuads.push_back(q);
            } else if (it != cellTriangles.end()) {
                for (const auto& t : it->second) {
                    transitionTris.push_back(t);
                }
            }
        }

        sculpt_log("[QUAD CAP PATCH #%zu] Inner Grid: %zu candidate quads -> %zu accepted (2 tris), %zu transition belt tris\n",
                   patchIdx + 1, quads.size(), acceptedGridQuads.size(), transitionTris.size());

        // ---------------------------------------------------------
        // Transition Belt Quadification: Greedy Quad Pairing
        // ---------------------------------------------------------
        struct TransitionQuad {
            uint32_t v0, v1, v2, v3;
        };
        std::vector<TransitionQuad> transitionQuads;
        std::vector<std::array<uint32_t, 3>> remainingTris;

        auto getVertPos2D = [&](uint32_t vid) -> glm::vec2 {
            if (vid < mesh.verts.size() / 3) {
                glm::vec3 p(mesh.verts[vid * 3], mesh.verts[vid * 3 + 1], mesh.verts[vid * 3 + 2]);
                glm::vec3 rel = p - loopCenter;
                return glm::vec2(glm::dot(rel, U), glm::dot(rel, V));
            } else {
                size_t off = vid - baseVid;
                if (off < loopNewVerts.size()) {
                    glm::vec3 rel = loopNewVerts[off].pos - loopCenter;
                    return glm::vec2(glm::dot(rel, U), glm::dot(rel, V));
                }
            }
            return glm::vec2(0.0f);
        };

        auto getVertPos3D = [&](uint32_t vid) -> glm::vec3 {
            if (vid < mesh.verts.size() / 3) {
                return glm::vec3(mesh.verts[vid * 3], mesh.verts[vid * 3 + 1], mesh.verts[vid * 3 + 2]);
            } else {
                size_t off = vid - baseVid;
                if (off < loopNewVerts.size()) {
                    return loopNewVerts[off].pos;
                }
            }
            return glm::vec3(0.0f);
        };

        const size_t numTransTris = transitionTris.size();
        struct TransEdgeHalf {
            int triIdx;
            uint32_t vOpp;
            bool forward;
        };

        std::unordered_map<uint64_t, std::vector<TransEdgeHalf>> transEdgeMap;
        transEdgeMap.reserve(numTransTris * 3);

        for (size_t t = 0; t < numTransTris; ++t) {
            const auto& tri = transitionTris[t];
            uint32_t i0 = tri[0], i1 = tri[1], i2 = tri[2];
            transEdgeMap[makeUndirectedEdgeKey(i0, i1)].push_back({(int)t, i2, i0 < i1});
            transEdgeMap[makeUndirectedEdgeKey(i1, i2)].push_back({(int)t, i0, i1 < i2});
            transEdgeMap[makeUndirectedEdgeKey(i2, i0)].push_back({(int)t, i1, i2 < i0});
        }

        struct CandidateQuadPair {
            int tA, tB;
            uint32_t q0, q1, q2, q3;
            float score;
        };
        std::vector<CandidateQuadPair> candidatePairs;
        candidatePairs.reserve(numTransTris / 2);

        int rejectedNonConvex = 0;
        int rejectedDiagRatio = 0;
        int rejectedEdgeRatio = 0;
        int rejectedAngles = 0;

        for (const auto& [edgeKey, list] : transEdgeMap) {
            if (list.size() != 2) continue; // Boundary edge or non-manifold

            const auto& hA = list[0];
            const auto& hB = list[1];
            if (hA.forward == hB.forward) continue; // Inconsistent winding

            int tA = hA.triIdx;
            int tB = hB.triIdx;

            uint32_t u = (uint32_t)(edgeKey >> 32);
            uint32_t v = (uint32_t)(edgeKey & 0xFFFFFFFF);
            uint32_t wA = hA.vOpp;
            uint32_t wB = hB.vOpp;

            if (!hA.forward) {
                std::swap(u, v);
            }

            // CCW quad perimeter: u -> wB -> v -> wA
            uint32_t q0 = u;
            uint32_t q1 = wB;
            uint32_t q2 = v;
            uint32_t q3 = wA;

            glm::vec2 p0 = getVertPos2D(q0);
            glm::vec2 p1 = getVertPos2D(q1);
            glm::vec2 p2 = getVertPos2D(q2);
            glm::vec2 p3 = getVertPos2D(q3);

            auto cross2D = [](const glm::vec2& a, const glm::vec2& b) -> float {
                return a.x * b.y - a.y * b.x;
            };

            // Corner turn cross products:
            float c0 = cross2D(p0 - p3, p1 - p0);
            float c1 = cross2D(p1 - p0, p2 - p1);
            float c2 = cross2D(p2 - p1, p3 - p2);
            float c3 = cross2D(p3 - p2, p0 - p3);

            if (c0 <= 1e-6f || c1 <= 1e-6f || c2 <= 1e-6f || c3 <= 1e-6f) {
                rejectedNonConvex++;
                continue; // Non-convex or reflex corner
            }

            // Diagonal ratio
            float d1 = glm::distance(p0, p2);
            float d2 = glm::distance(p1, p3);
            if (d1 < 1e-4f || d2 < 1e-4f) { rejectedDiagRatio++; continue; }
            float diagRatio = std::min(d1, d2) / std::max(d1, d2);
            if (diagRatio < 0.16f) { rejectedDiagRatio++; continue; } // Allow slightly sheared quads over sliver triangles

            // Edge lengths and interior angles
            glm::vec2 e0 = p1 - p0;
            glm::vec2 e1 = p2 - p1;
            glm::vec2 e2 = p3 - p2;
            glm::vec2 e3 = p0 - p3;

            float l0 = glm::length(e0);
            float l1 = glm::length(e1);
            float l2 = glm::length(e2);
            float l3 = glm::length(e3);

            float lMin = std::min({l0, l1, l2, l3});
            float lMax = std::max({l0, l1, l2, l3});
            if (lMax <= 1e-5f || (lMin / lMax) < 0.05f) { rejectedEdgeRatio++; continue; }

            // Interior angles
            auto angleDeg = [](const glm::vec2& vIn, const glm::vec2& vOut, float crossVal) -> float {
                float dotVal = glm::dot(vIn, vOut);
                return glm::degrees(std::atan2(crossVal, dotVal));
            };

            float a0 = angleDeg(p0 - p3, p1 - p0, c0);
            float a1 = angleDeg(p1 - p0, p2 - p1, c1);
            float a2 = angleDeg(p2 - p1, p3 - p2, c2);
            float a3 = angleDeg(p3 - p2, p0 - p3, c3);

            if (a0 < 22.0f || a0 > 158.0f || a1 < 22.0f || a1 > 158.0f ||
                a2 < 22.0f || a2 > 158.0f || a3 < 22.0f || a3 > 158.0f) {
                rejectedAngles++;
                continue; // Extreme acute or obtuse angles
            }

            // Quality score: diagonal ratio + edge ratio + proximity of angles to 90 deg
            float angleScore = 0.0f;
            for (float a : {a0, a1, a2, a3}) {
                angleScore += std::sin(glm::radians(a));
            }
            float score = diagRatio * 2.5f + (lMin / lMax) * 1.5f + angleScore;
            candidatePairs.push_back({tA, tB, q0, q1, q2, q3, score});
        }

        std::sort(candidatePairs.begin(), candidatePairs.end(), [](const CandidateQuadPair& a, const CandidateQuadPair& b) {
            return a.score > b.score;
        });

        sculpt_log("[QUAD CAP PATCH #%zu] Transition Pairs: %zu internal edges -> %zu valid quads (Rejected: %d reflex/concave, %d sheared/diag, %d narrow edges, %d extreme angles)\n",
                   patchIdx + 1, transEdgeMap.size(), candidatePairs.size(),
                   rejectedNonConvex, rejectedDiagRatio, rejectedEdgeRatio, rejectedAngles);

        std::vector<bool> triUsed(numTransTris, false);
        for (const auto& cp : candidatePairs) {
            if (!triUsed[cp.tA] && !triUsed[cp.tB]) {
                triUsed[cp.tA] = true;
                triUsed[cp.tB] = true;
                transitionQuads.push_back({cp.q0, cp.q1, cp.q2, cp.q3});
            }
        }

        // Inner Grid 3D Quality Metrics
        float gridMinEdge3D = 1e9f, gridMaxEdge3D = 0.0f, gridSumEdge3D = 0.0f;
        float gridMaxAspect3D = 1.0f;
        int gridStretchedQuadsCount = 0;
        for (const auto& q : acceptedGridQuads) {
            glm::vec3 p0 = getVertPos3D(q.v00);
            glm::vec3 p1 = getVertPos3D(q.v10);
            glm::vec3 p2 = getVertPos3D(q.v11);
            glm::vec3 p3 = getVertPos3D(q.v01);
            float l0 = glm::distance(p0, p1);
            float l1 = glm::distance(p1, p2);
            float l2 = glm::distance(p2, p3);
            float l3 = glm::distance(p3, p0);
            float lMin = std::min({l0, l1, l2, l3});
            float lMax = std::max({l0, l1, l2, l3});
            gridMinEdge3D = std::min(gridMinEdge3D, lMin);
            gridMaxEdge3D = std::max(gridMaxEdge3D, lMax);
            gridSumEdge3D += (l0 + l1 + l2 + l3) * 0.25f;
            float asp = (lMin > 1e-5f) ? (lMax / lMin) : 999.0f;
            gridMaxAspect3D = std::max(gridMaxAspect3D, asp);
            if (asp > 2.0f) gridStretchedQuadsCount++;
        }
        float gridAvgEdge3D = acceptedGridQuads.empty() ? 0.0f : (gridSumEdge3D / (float)acceptedGridQuads.size());
        sculpt_log("[QUAD CAP PATCH #%zu] Inner Grid 3D Quality: %zu quads | avg edge: %.3f mm [min: %.3f, max: %.3f] | max aspect: %.2f | Stretched quads (>2.0x): %d (%.1f%%)\n",
                   patchIdx + 1, acceptedGridQuads.size(), gridAvgEdge3D, gridMinEdge3D, gridMaxEdge3D,
                   gridMaxAspect3D, gridStretchedQuadsCount,
                   acceptedGridQuads.empty() ? 0.0f : (float)gridStretchedQuadsCount * 100.0f / (float)acceptedGridQuads.size());

        // Transition Quads 3D Quality Metrics
        float tqMinEdge3D = 1e9f, tqMaxEdge3D = 0.0f, tqSumEdge3D = 0.0f;
        float tqMaxAspect3D = 1.0f;
        int tqStretchedCount = 0;
        for (const auto& tq : transitionQuads) {
            glm::vec3 p0 = getVertPos3D(tq.v0);
            glm::vec3 p1 = getVertPos3D(tq.v1);
            glm::vec3 p2 = getVertPos3D(tq.v2);
            glm::vec3 p3 = getVertPos3D(tq.v3);
            float l0 = glm::distance(p0, p1);
            float l1 = glm::distance(p1, p2);
            float l2 = glm::distance(p2, p3);
            float l3 = glm::distance(p3, p0);
            float lMin = std::min({l0, l1, l2, l3});
            float lMax = std::max({l0, l1, l2, l3});
            tqMinEdge3D = std::min(tqMinEdge3D, lMin);
            tqMaxEdge3D = std::max(tqMaxEdge3D, lMax);
            tqSumEdge3D += (l0 + l1 + l2 + l3) * 0.25f;
            float asp = (lMin > 1e-5f) ? (lMax / lMin) : 999.0f;
            tqMaxAspect3D = std::max(tqMaxAspect3D, asp);
            if (asp > 2.0f) tqStretchedCount++;
        }
        float tqAvgEdge3D = transitionQuads.empty() ? 0.0f : (tqSumEdge3D / (float)transitionQuads.size());
        sculpt_log("[QUAD CAP PATCH #%zu] Transition Quads 3D: %zu quads | avg edge: %.3f mm [min: %.3f, max: %.3f] | max aspect: %.2f | Stretched (>2.0x): %d (%.1f%%)\n",
                   patchIdx + 1, transitionQuads.size(), tqAvgEdge3D, tqMinEdge3D, tqMaxEdge3D,
                   tqMaxAspect3D, tqStretchedCount,
                   transitionQuads.empty() ? 0.0f : (float)tqStretchedCount * 100.0f / (float)transitionQuads.size());

        // Remaining un-paired triangles: 2D & 3D aspect ratio and spatial localization
        int elongatedTrisCount2D = 0;
        int elong3DCount = 0;
        int severeElong3DCount = 0;
        float triMinEdge3D = 1e9f, triMaxEdge3D = 0.0f, triSumEdge3D = 0.0f;
        float triMaxAspect3D = 1.0f;
        glm::vec3 elongBBoxMin(1e9f), elongBBoxMax(-1e9f);
        glm::vec3 elongNormalSum(0.0f);
        glm::vec3 elongCenterSum(0.0f);

        for (size_t t = 0; t < numTransTris; ++t) {
            if (!triUsed[t]) {
                remainingTris.push_back(transitionTris[t]);

                glm::vec2 p0_2D = getVertPos2D(transitionTris[t][0]);
                glm::vec2 p1_2D = getVertPos2D(transitionTris[t][1]);
                glm::vec2 p2_2D = getVertPos2D(transitionTris[t][2]);

                float d01 = glm::distance(p0_2D, p1_2D);
                float d12 = glm::distance(p1_2D, p2_2D);
                float d20 = glm::distance(p2_2D, p0_2D);
                float maxEdge = std::max({d01, d12, d20});
                float area2D = 0.5f * std::abs((p1_2D.x - p0_2D.x) * (p2_2D.y - p0_2D.y) - (p1_2D.y - p0_2D.y) * (p2_2D.x - p0_2D.x));
                float alt = (maxEdge > 1e-5f) ? (2.0f * area2D / maxEdge) : 0.0f;
                float aspect2D = (alt > 1e-5f) ? (maxEdge / alt) : 999.0f;
                if (aspect2D > 3.5f) {
                    elongatedTrisCount2D++;
                }

                glm::vec3 p0_3D = getVertPos3D(transitionTris[t][0]);
                glm::vec3 p1_3D = getVertPos3D(transitionTris[t][1]);
                glm::vec3 p2_3D = getVertPos3D(transitionTris[t][2]);
                float e01_3D = glm::distance(p0_3D, p1_3D);
                float e12_3D = glm::distance(p1_3D, p2_3D);
                float e20_3D = glm::distance(p2_3D, p0_3D);
                float maxE_3D = std::max({e01_3D, e12_3D, e20_3D});
                float minE_3D = std::min({e01_3D, e12_3D, e20_3D});
                triMinEdge3D = std::min(triMinEdge3D, minE_3D);
                triMaxEdge3D = std::max(triMaxEdge3D, maxE_3D);
                triSumEdge3D += (e01_3D + e12_3D + e20_3D) / 3.0f;

                glm::vec3 c_3D = glm::cross(p1_3D - p0_3D, p2_3D - p0_3D);
                float area_3D = 0.5f * glm::length(c_3D);
                float alt_3D = (maxE_3D > 1e-5f) ? (2.0f * area_3D / maxE_3D) : 0.0f;
                float aspect_3D = (alt_3D > 1e-5f) ? (maxE_3D / alt_3D) : 999.0f;
                triMaxAspect3D = std::max(triMaxAspect3D, aspect_3D);

                if (aspect_3D > 3.0f || maxE_3D > 2.5f * step) {
                    elong3DCount++;
                    if (aspect_3D > 6.0f || maxE_3D > 4.0f * step) severeElong3DCount++;
                    elongBBoxMin = glm::min(elongBBoxMin, glm::min(p0_3D, glm::min(p1_3D, p2_3D)));
                    elongBBoxMax = glm::max(elongBBoxMax, glm::max(p0_3D, glm::max(p1_3D, p2_3D)));
                    if (glm::length(c_3D) > 1e-6f) elongNormalSum += glm::normalize(c_3D);
                    elongCenterSum += (p0_3D + p1_3D + p2_3D) / 3.0f;
                }
            }
        }
        float triAvgEdge3D = remainingTris.empty() ? 0.0f : (triSumEdge3D / (float)remainingTris.size());

        sculpt_log("[QUAD CAP PATCH #%zu] Transition Belt Quadify: %zu initial tris -> %zu quads (%.1f%% of belt converted), %zu tris remaining (2D aspect > 3.5: %d)\n",
                   patchIdx + 1, numTransTris, transitionQuads.size(),
                   numTransTris > 0 ? (float)(transitionQuads.size() * 2) * 100.0f / (float)numTransTris : 0.0f,
                   remainingTris.size(), elongatedTrisCount2D);
        sculpt_log("[QUAD CAP PATCH #%zu] Remaining Tris 3D: %zu tris | avg edge: %.3f mm [min: %.3f, max: %.3f] | max aspect: %.2f | Elongated (>3.0x): %d (severe >6.0x: %d)\n",
                   patchIdx + 1, remainingTris.size(), triAvgEdge3D, triMinEdge3D, triMaxEdge3D,
                   triMaxAspect3D, elong3DCount, severeElong3DCount);

        if (elong3DCount > 0) {
            glm::vec3 elongCenter = elongCenterSum / (float)elong3DCount;
            glm::vec3 elongNormal = (glm::length(elongNormalSum) > 1e-6f) ? glm::normalize(elongNormalSum) : patchNormal;
            float dotElong = std::clamp(glm::dot(elongNormal, patchNormal), -1.0f, 1.0f);
            float angleToPatchN = glm::degrees(std::acos(dotElong));
            sculpt_log("[QUAD CAP PATCH #%zu] Elongated Tris Region: %d tris | Center=(%.2f, %.2f, %.2f) | BBox Size=[%.2f, %.2f, %.2f] | AvgNormal=[%.2f, %.2f, %.2f] (angle to patch normal: %.1f deg)\n",
                       patchIdx + 1, elong3DCount,
                       elongCenter.x, elongCenter.y, elongCenter.z,
                       elongBBoxMax.x - elongBBoxMin.x, elongBBoxMax.y - elongBBoxMin.y, elongBBoxMax.z - elongBBoxMin.z,
                       elongNormal.x, elongNormal.y, elongNormal.z, angleToPatchN);
        }

        // ---------------------------------------------------------
        // Cap Faces Normal Alignment & Inversion Diagnostics
        // ---------------------------------------------------------
        int capInverted = 0;
        int capSteep = 0;
        float capMinDot = 1.0f, capMaxDot = -1.0f, capSumDot = 0.0f;
        int capTotalFaces = (int)(acceptedGridQuads.size() + transitionQuads.size() + remainingTris.size());

        auto checkFaceNormal = [&](const glm::vec3& fn, const char* type, int idx) {
            float len = glm::length(fn);
            if (len < 1e-6f) return;
            glm::vec3 n = fn / len;
            float d = glm::dot(n, patchNormal);
            if (d < capMinDot) capMinDot = d;
            if (d > capMaxDot) capMaxDot = d;
            capSumDot += d;
            if (d < 0.0f) {
                capInverted++;
                if (capInverted <= 3) {
                    sculpt_log("[QUAD CAP PATCH #%zu] WARNING: INVERTED %s #%d! dot vs patchN: %.3f\n",
                               patchIdx + 1, type, idx, d);
                }
            } else if (d < 0.2f) {
                capSteep++;
            }
        };

        for (size_t i = 0; i < acceptedGridQuads.size(); ++i) {
            const auto& q = acceptedGridQuads[i];
            glm::vec3 p0 = getVertPos3D(q.v00), p1 = getVertPos3D(q.v10), p2 = getVertPos3D(q.v11), p3 = getVertPos3D(q.v01);
            glm::vec3 fn = glm::cross(p1 - p0, p2 - p0) + glm::cross(p2 - p0, p3 - p0);
            checkFaceNormal(fn, "GridQuad", (int)i);
        }
        for (size_t i = 0; i < transitionQuads.size(); ++i) {
            const auto& tq = transitionQuads[i];
            glm::vec3 p0 = getVertPos3D(tq.v0), p1 = getVertPos3D(tq.v1), p2 = getVertPos3D(tq.v2), p3 = getVertPos3D(tq.v3);
            glm::vec3 fn = glm::cross(p1 - p0, p2 - p0) + glm::cross(p2 - p0, p3 - p0);
            checkFaceNormal(fn, "TransQuad", (int)i);
        }
        for (size_t i = 0; i < remainingTris.size(); ++i) {
            const auto& t = remainingTris[i];
            glm::vec3 p0 = getVertPos3D(t[0]), p1 = getVertPos3D(t[1]), p2 = getVertPos3D(t[2]);
            glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
            checkFaceNormal(fn, "BeltTri", (int)i);
        }

        float capAvgDot = capTotalFaces > 0 ? (capSumDot / (float)capTotalFaces) : 1.0f;
        sculpt_log("[QUAD CAP PATCH #%zu] Cap Faces Normal Alignment: %d faces | avg dot vs patchN: %.3f [min: %.3f, max: %.3f] | Inverted (dot<0): %d, Steep (dot<0.2): %d\n",
                   patchIdx + 1, capTotalFaces, capAvgDot, capMinDot, capMaxDot, capInverted, capSteep);

        // Emit new patch faces into buffers:
        // 1. Inner grid quads
        for (const auto& q : acceptedGridQuads) {
            newFaces.push_back(q.v00);
            newFaces.push_back(q.v10);
            newFaces.push_back(q.v11);
            newFaces.push_back(q.v01);
            newFaceGroups.push_back(capGroupID);
        }

        // 2. Transition quads
        for (const auto& tq : transitionQuads) {
            newFaces.push_back(tq.v0);
            newFaces.push_back(tq.v1);
            newFaces.push_back(tq.v2);
            newFaces.push_back(tq.v3);
            newFaceGroups.push_back(capGroupID);
        }

        // 3. Remaining transition triangles
        for (const auto& t : remainingTris) {
            if (t[0] == t[1] || t[1] == t[2] || t[2] == t[0]) continue;
            newFaces.push_back(t[0]);
            newFaces.push_back(t[1]);
            newFaces.push_back(t[2]);
            newFaces.push_back(TRI_INDEX);
            newFaceGroups.push_back(capGroupID);
        }

        // Stage newly created grid vertices
        stagedVerts.insert(stagedVerts.end(), loopNewVerts.begin(), loopNewVerts.end());

        int patchQuads = static_cast<int>(acceptedGridQuads.size() + transitionQuads.size());
        int patchTrisGenerated = static_cast<int>(remainingTris.size());

        totalQuadsGenerated += patchQuads;
        totalTrisGenerated += patchTrisGenerated;
        anyPatchQuadCapped = true;

        int totalPatchFaces = patchQuads + patchTrisGenerated;
        float patchQuadRatio = totalPatchFaces > 0 ? (float)patchQuads / (float)totalPatchFaces : 0.0f;
        sculpt_log("[QUAD CAP PATCH #%zu] RESULT: SUCCESS -> %d Quads (%.1f%%) [%zu grid + %zu belt], %d Tris (%.1f%%)\n",
                   patchIdx + 1, patchQuads, patchQuadRatio * 100.0f,
                   acceptedGridQuads.size(), transitionQuads.size(),
                   patchTrisGenerated, (1.0f - patchQuadRatio) * 100.0f);
    } // end for patchIdx in patches

    sculpt_log("================================================================\n");

    // 6. Final commit if any patch succeeded
    if (anyPatchQuadCapped) {
        for (const auto& sv : stagedVerts) {
            mesh.verts.push_back(sv.pos.x);
            mesh.verts.push_back(sv.pos.y);
            mesh.verts.push_back(sv.pos.z);
            mesh.normals.push_back(sv.normal.x);
            mesh.normals.push_back(sv.normal.y);
            mesh.normals.push_back(sv.normal.z);
            mesh.colors.push_back(1.0f);
            mesh.colors.push_back(1.0f);
            mesh.colors.push_back(1.0f);
            mesh.materials.push_back(0.5f);
            mesh.materials.push_back(0.0f);
            mesh.materials.push_back(1.0f);
        }

        mesh.nbVerts = static_cast<int>(mesh.verts.size() / 3);
        mesh.nbFaces = static_cast<int>(newFaces.size() / 4);
        mesh.faces = std::move(newFaces);
        mesh.faceGroups = std::move(newFaceGroups);

        mesh.vrfStartCount.clear();
        mesh.vertRingFace.clear();
        mesh.vrvStartCount.clear();
        mesh.vertRingVert.clear();
        mesh.vertOnEdge.clear();
        mesh.edges.clear();
        mesh.faceEdges.clear();

        mesh.hasCustomNormals = false;
        mesh.initTopology();
        mesh.postInit();

        mesh.isDirty = true;
        mesh.isTopologyDirty = true;
        mesh.isVertexDirty = true;
        mesh.isColorDirty = true;
        mesh.isFaceGroupDirty = true;
        mesh.invalidateLocalRadius();

        result.success = true;
    }

    auto tEnd = std::chrono::high_resolution_clock::now();
    result.executionTimeMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    result.numQuads = totalQuadsGenerated;
    result.numTris = totalTrisGenerated;
    int totalNewFaces = totalQuadsGenerated + totalTrisGenerated;
    result.quadRatio = totalNewFaces > 0 ? (float)totalQuadsGenerated / (float)totalNewFaces : 0.0f;

    sculpt_log("[QUAD CAP TOTAL] %zu patches processed: %d Quads (%.1f%%), %d Tris (%.1f%%) in %.2f ms [%s]\n",
               patches.size(), result.numQuads, result.quadRatio * 100.0f,
               result.numTris, (1.0f - result.quadRatio) * 100.0f,
               result.executionTimeMs, anyPatchQuadCapped ? "COMMITTED" : "PRESERVED ORIGINAL MANIFOLD CAP");

    return result;
}

} // namespace sculpt
