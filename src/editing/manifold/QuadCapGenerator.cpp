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
#include <omp.h>
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
        result.fallbackReason = "Empty mesh or empty faceGroups";
        return result;
    }

    const int nbFaces = mesh.nbFaces;

    // 1. Identify all faces belonging to capGroupID
    std::vector<uint8_t> isCapFace(nbFaces, 0);
    std::vector<int> capFaceIndices;
    capFaceIndices.reserve(nbFaces / 4);
    for (int f = 0; f < nbFaces; ++f) {
        if (mesh.faceGroups[f] == capGroupID) {
            isCapFace[f] = 1;
            capFaceIndices.push_back(f);
        }
    }

    if (capFaceIndices.empty()) {
        sculpt_log("[QUAD CAP] No faces found with capGroupID: %u (total faces: %d)\n", capGroupID, nbFaces);
        result.fallbackReason = "No faces found with capGroupID";
        return result;
    }

    // 2. Partition cap faces into planar sub-chart components using dihedral angle & normal clustering
    auto makeUndirectedEdgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
        uint32_t u = std::min(a, b);
        uint32_t v = std::max(a, b);
        return ((uint64_t)u << 32) | (uint64_t)v;
    };

    struct CapEdge {
        uint64_t key;
        int face;
    };
    std::vector<CapEdge> capEdges;
    capEdges.reserve(capFaceIndices.size() * 4);

    struct FaceGeom {
        glm::vec3 normal;
        float area;
    };
    std::vector<FaceGeom> faceGeom(nbFaces);

    for (int f : capFaceIndices) {
        uint32_t v0 = mesh.faces[f * 4 + 0];
        uint32_t v1 = mesh.faces[f * 4 + 1];
        uint32_t v2 = mesh.faces[f * 4 + 2];
        uint32_t v3 = mesh.faces[f * 4 + 3];

        capEdges.push_back({makeUndirectedEdgeKey(v0, v1), f});
        capEdges.push_back({makeUndirectedEdgeKey(v1, v2), f});
        if (v3 != TRI_INDEX) {
            capEdges.push_back({makeUndirectedEdgeKey(v2, v3), f});
            capEdges.push_back({makeUndirectedEdgeKey(v3, v0), f});
        } else {
            capEdges.push_back({makeUndirectedEdgeKey(v2, v0), f});
        }

        glm::vec3 p0(mesh.verts[v0 * 3], mesh.verts[v0 * 3 + 1], mesh.verts[v0 * 3 + 2]);
        glm::vec3 p1(mesh.verts[v1 * 3], mesh.verts[v1 * 3 + 1], mesh.verts[v1 * 3 + 2]);
        glm::vec3 p2(mesh.verts[v2 * 3], mesh.verts[v2 * 3 + 1], mesh.verts[v2 * 3 + 2]);
        glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
        if (v3 != TRI_INDEX) {
            glm::vec3 p3(mesh.verts[v3 * 3], mesh.verts[v3 * 3 + 1], mesh.verts[v3 * 3 + 2]);
            fn += glm::cross(p2 - p0, p3 - p0);
        }
        float len = glm::length(fn);
        faceGeom[f].normal = (len > 1e-6f) ? (fn / len) : glm::vec3(0.0f, 1.0f, 0.0f);
        faceGeom[f].area = len * 0.5f;
    }

    std::sort(capEdges.begin(), capEdges.end(), [](const CapEdge& a, const CapEdge& b) {
        return a.key < b.key;
    });

    // Build face adjacency across shared cap edges
    std::vector<std::vector<int>> capFaceAdj(nbFaces);
    for (size_t i = 0; i < capEdges.size(); ) {
        size_t j = i + 1;
        while (j < capEdges.size() && capEdges[j].key == capEdges[i].key) ++j;
        for (size_t a = i; a < j; ++a) {
            for (size_t b = a + 1; b < j; ++b) {
                capFaceAdj[capEdges[a].face].push_back(capEdges[b].face);
                capFaceAdj[capEdges[b].face].push_back(capEdges[a].face);
            }
        }
        i = j;
    }

    // Sort seed faces by area descending for stable, deterministic chart creation
    std::vector<int> sortedCapFaces = capFaceIndices;
    std::sort(sortedCapFaces.begin(), sortedCapFaces.end(), [&](int a, int b) {
        return faceGeom[a].area > faceGeom[b].area;
    });

    // Planar chart clustering:
    // 1. Crease threshold: adjacent faces with dihedral angle > 45 deg belong to different charts.
    // 2. Normal variance threshold: no face in chart may deviate by > 50 deg from chart average normal.
    const float cosCrease = std::cos(glm::radians(45.0f));
    const float cosMaxDev = std::cos(glm::radians(50.0f));

    std::vector<uint8_t> faceVisited(nbFaces, 0);
    std::vector<std::vector<int>> patches;

    for (int seedF : sortedCapFaces) {
        if (faceVisited[seedF]) continue;

        std::vector<int> currentPatch;
        std::vector<int> queue;
        queue.push_back(seedF);
        faceVisited[seedF] = 1;

        glm::vec3 chartNormSum = faceGeom[seedF].normal * faceGeom[seedF].area;
        float chartAreaSum = faceGeom[seedF].area;

        size_t qHead = 0;
        while (qHead < queue.size()) {
            int currF = queue[qHead++];
            currentPatch.push_back(currF);

            glm::vec3 currAvgN = (chartAreaSum > 1e-6f) ? glm::normalize(chartNormSum) : faceGeom[seedF].normal;

            for (int adjF : capFaceAdj[currF]) {
                if (faceVisited[adjF]) continue;

                // Crease check between adjacent faces
                float dotAdj = glm::dot(faceGeom[currF].normal, faceGeom[adjF].normal);
                if (dotAdj < cosCrease && faceGeom[adjF].area > 1e-4f) continue;

                // Deviation from current patch average normal
                float dotAvg = glm::dot(currAvgN, faceGeom[adjF].normal);
                if (dotAvg < cosMaxDev && faceGeom[adjF].area > 1e-4f) continue;

                faceVisited[adjF] = 1;
                chartNormSum += faceGeom[adjF].normal * faceGeom[adjF].area;
                chartAreaSum += faceGeom[adjF].area;
                queue.push_back(adjF);
            }
        }

        if (!currentPatch.empty()) {
            patches.push_back(std::move(currentPatch));
        }
    }

    // Absorb any remaining unvisited tiny sliver faces into an adjacent visited patch
    for (int f : sortedCapFaces) {
        if (!faceVisited[f] && faceGeom[f].area <= 1e-4f) {
            for (int adjF : capFaceAdj[f]) {
                if (faceVisited[adjF]) {
                    for (auto& patch : patches) {
                        for (int pf : patch) {
                            if (pf == adjF) {
                                patch.push_back(f);
                                faceVisited[f] = 1;
                                goto faceAbsorbed;
                            }
                        }
                    }
                }
            }
        faceAbsorbed:;
        }
    }

    result.numPatches = static_cast<int>(patches.size());

    sculpt_log("[QUAD CAP] Total mesh faces: %d | Cap faces (ID %u): %zu across %zu separate patch(es)\n",
               nbFaces, capGroupID, capFaceIndices.size(), patches.size());

    // 3. Prepare new faces buffer with all non-cap faces
    std::vector<uint32_t> newFaces;
    std::vector<uint32_t> newFaceGroups;
    newFaces.reserve(mesh.faces.size());
    newFaceGroups.reserve(mesh.faceGroups.size());

    for (int f = 0; f < nbFaces; ++f) {
        if (!isCapFace[f]) {
            newFaces.push_back(mesh.faces[f * 4 + 0]);
            newFaces.push_back(mesh.faces[f * 4 + 1]);
            newFaces.push_back(mesh.faces[f * 4 + 2]);
            newFaces.push_back(mesh.faces[f * 4 + 3]);
            newFaceGroups.push_back(mesh.faceGroups[f]);
        }
    }

    int origNbVerts = static_cast<int>(mesh.verts.size() / 3);

    // 4. Subdivide long inter-patch seam edges
    // Long straight edges along seams between adjacent cap patches (e.g. cutter prism lateral walls)
    // create severe needle triangles in CDT when triangulating against a fine grid boundary.
    // By subdividing these long shared edges with intermediate vertices at spacing step,
    // CDT triangulates the transition belt as a regular ladder of quads with AR ~ 1.0..1.4x.
    float seamStep = targetEdgeLen;
    if (seamStep <= 1e-4f) {
        float sumLen = 0.0f;
        int count = 0;
        for (int f : capFaceIndices) {
            uint32_t v0 = mesh.faces[f * 4 + 0];
            uint32_t v1 = mesh.faces[f * 4 + 1];
            uint32_t v2 = mesh.faces[f * 4 + 2];
            uint32_t v3 = mesh.faces[f * 4 + 3];
            glm::vec3 p0(mesh.verts[v0 * 3], mesh.verts[v0 * 3 + 1], mesh.verts[v0 * 3 + 2]);
            glm::vec3 p1(mesh.verts[v1 * 3], mesh.verts[v1 * 3 + 1], mesh.verts[v1 * 3 + 2]);
            glm::vec3 p2(mesh.verts[v2 * 3], mesh.verts[v2 * 3 + 1], mesh.verts[v2 * 3 + 2]);
            for (float l : {glm::distance(p0, p1), glm::distance(p1, p2), glm::distance(p2, p0)}) {
                if (l > 1e-4f && l < 0.5f) { sumLen += l; count++; }
            }
            if (v3 != TRI_INDEX) {
                glm::vec3 p3(mesh.verts[v3 * 3], mesh.verts[v3 * 3 + 1], mesh.verts[v3 * 3 + 2]);
                float l3 = glm::distance(p3, p0);
                if (l3 > 1e-4f && l3 < 0.5f) { sumLen += l3; count++; }
            }
        }
        seamStep = (count > 0) ? (sumLen / count) : 0.025f;
    }
    seamStep = std::clamp(seamStep, 0.01f, 5.0f);

    std::vector<float> patchAreas(patches.size(), 0.0f);
    for (size_t p = 0; p < patches.size(); ++p) {
        for (int f : patches[p]) {
            patchAreas[p] += faceGeom[f].area;
        }
    }

    std::vector<std::vector<uint64_t>> patchBoundaryDirectedEdges(patches.size());
    for (size_t p = 0; p < patches.size(); ++p) {
        std::vector<uint64_t> patchEdges;
        patchEdges.reserve(patches[p].size() * 4);
        for (int f : patches[p]) {
            uint32_t v0 = mesh.faces[f * 4 + 0];
            uint32_t v1 = mesh.faces[f * 4 + 1];
            uint32_t v2 = mesh.faces[f * 4 + 2];
            uint32_t v3 = mesh.faces[f * 4 + 3];
            patchEdges.push_back(((uint64_t)v0 << 32) | v1);
            patchEdges.push_back(((uint64_t)v1 << 32) | v2);
            if (v3 != TRI_INDEX) {
                patchEdges.push_back(((uint64_t)v2 << 32) | v3);
                patchEdges.push_back(((uint64_t)v3 << 32) | v0);
            } else {
                patchEdges.push_back(((uint64_t)v2 << 32) | v0);
            }
        }
        std::sort(patchEdges.begin(), patchEdges.end());
        for (uint64_t edge : patchEdges) {
            uint32_t u = (uint32_t)(edge >> 32);
            uint32_t v = (uint32_t)(edge & 0xFFFFFFFF);
            uint64_t rev = ((uint64_t)v << 32) | u;
            if (!std::binary_search(patchEdges.begin(), patchEdges.end(), rev)) {
                patchBoundaryDirectedEdges[p].push_back(edge);
            }
        }
    }

    struct SeamEdgeCandidate {
        size_t patchA;
        size_t patchB;
        uint32_t u, v;
    };
    std::unordered_map<uint64_t, SeamEdgeCandidate> candidateSeams;
    for (size_t p = 0; p < patches.size(); ++p) {
        if (patchAreas[p] < 0.01f || patches[p].size() < 3) continue;
        for (uint64_t edge : patchBoundaryDirectedEdges[p]) {
            uint32_t u = (uint32_t)(edge >> 32);
            uint32_t v = (uint32_t)(edge & 0xFFFFFFFF);
            uint64_t key = makeUndirectedEdgeKey(u, v);
            auto it = candidateSeams.find(key);
            if (it == candidateSeams.end()) {
                candidateSeams[key] = {p, SIZE_MAX, std::min(u, v), std::max(u, v)};
            } else if (it->second.patchB == SIZE_MAX && it->second.patchA != p) {
                it->second.patchB = p;
            }
        }
    }

    std::unordered_map<uint64_t, std::vector<uint32_t>> seamSubdivision;
    size_t totalSeamEdgesSubdivided = 0;
    size_t totalNewSeamVerts = 0;

    for (const auto& [key, seam] : candidateSeams) {
        if (seam.patchB == SIZE_MAX) continue;
        uint32_t u = seam.u;
        uint32_t v = seam.v;
        glm::vec3 pu(mesh.verts[u * 3], mesh.verts[u * 3 + 1], mesh.verts[u * 3 + 2]);
        glm::vec3 pv(mesh.verts[v * 3], mesh.verts[v * 3 + 1], mesh.verts[v * 3 + 2]);
        float edgeLen = glm::distance(pu, pv);

        if (edgeLen > 1.35f * seamStep) {
            int k = std::clamp(static_cast<int>(std::round(edgeLen / seamStep)), 2, 5000);
            glm::vec3 nu(0, 1, 0), nv(0, 1, 0);
            if (!mesh.normals.empty() && mesh.normals.size() >= mesh.verts.size()) {
                nu = glm::vec3(mesh.normals[u * 3], mesh.normals[u * 3 + 1], mesh.normals[u * 3 + 2]);
                nv = glm::vec3(mesh.normals[v * 3], mesh.normals[v * 3 + 1], mesh.normals[v * 3 + 2]);
            }

            std::vector<uint32_t> intermediateVerts;
            intermediateVerts.reserve(k - 1);

            for (int s = 1; s < k; ++s) {
                float t = static_cast<float>(s) / static_cast<float>(k);
                glm::vec3 p = glm::mix(pu, pv, t);
                glm::vec3 n = glm::normalize(glm::mix(nu, nv, t));
                uint32_t newVid = static_cast<uint32_t>(mesh.verts.size() / 3);
                mesh.verts.push_back(p.x);
                mesh.verts.push_back(p.y);
                mesh.verts.push_back(p.z);
                mesh.normals.push_back(n.x);
                mesh.normals.push_back(n.y);
                mesh.normals.push_back(n.z);
                mesh.colors.push_back(1.0f);
                mesh.colors.push_back(1.0f);
                mesh.colors.push_back(1.0f);
                mesh.materials.push_back(0.5f);
                mesh.materials.push_back(0.0f);
                mesh.materials.push_back(1.0f);
                intermediateVerts.push_back(newVid);
            }
            totalNewSeamVerts += intermediateVerts.size();
            totalSeamEdgesSubdivided++;
            seamSubdivision[key] = std::move(intermediateVerts);
        }
    }

    mesh.nbVerts = static_cast<int>(mesh.verts.size() / 3);

    sculpt_log("[QUAD CAP SEAM] Candidate seams: %zu | Subdivided %zu long seams (> 1.5*step) with %zu intermediate vertices (seamStep: %.3f mm)\n",
               candidateSeams.size(), totalSeamEdgesSubdivided, totalNewSeamVerts, seamStep);

    // Staging structures for newly created grid vertices
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
            result.patchesFellBack++;
            if (result.fallbackReason.empty()) {
                result.fallbackReason = reason;
            } else if (result.fallbackReason.find(reason) == std::string::npos) {
                result.fallbackReason += "; " + std::string(reason);
            }
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
        glm::vec3 patchBBoxMin(1e9f);
        glm::vec3 patchBBoxMax(-1e9f);

        auto updateBBox = [&](uint32_t vi) {
            if (vi < (uint32_t)mesh.nbVerts) {
                glm::vec3 p(mesh.verts[vi * 3], mesh.verts[vi * 3 + 1], mesh.verts[vi * 3 + 2]);
                patchBBoxMin = glm::min(patchBBoxMin, p);
                patchBBoxMax = glm::max(patchBBoxMax, p);
            }
        };

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
            updateBBox(v0);
            updateBBox(v1);
            updateBBox(v2);

            if (v3 != TRI_INDEX) {
                addPatchTri(v0, v2, v3);
                updateBBox(v3);
            }
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
        for (const auto& tri : patchTris) {
            for (const auto& p : {tri.p0, tri.p1, tri.p2}) {
                float d = glm::dot(p - patchCenter, patchNormal);
                depthMin = std::min(depthMin, d);
                depthMax = std::max(depthMax, d);
            }
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

        // Extract boundary halfedges of this patch using flat sorted array
        std::vector<uint64_t> patchDirectedEdges;
        patchDirectedEdges.reserve(patchFaces.size() * 4);
        for (int f : patchFaces) {
            uint32_t v0 = mesh.faces[f * 4 + 0];
            uint32_t v1 = mesh.faces[f * 4 + 1];
            uint32_t v2 = mesh.faces[f * 4 + 2];
            uint32_t v3 = mesh.faces[f * 4 + 3];

            patchDirectedEdges.push_back(((uint64_t)v0 << 32) | v1);
            patchDirectedEdges.push_back(((uint64_t)v1 << 32) | v2);
            if (v3 != TRI_INDEX) {
                patchDirectedEdges.push_back(((uint64_t)v2 << 32) | v3);
                patchDirectedEdges.push_back(((uint64_t)v3 << 32) | v0);
            } else {
                patchDirectedEdges.push_back(((uint64_t)v2 << 32) | v0);
            }
        }
        std::sort(patchDirectedEdges.begin(), patchDirectedEdges.end());

        // Boundary edges have no opposing halfedge within the patch
        std::vector<std::pair<uint32_t, uint32_t>> boundaryEdges;
        for (uint64_t edge : patchDirectedEdges) {
            uint32_t u = (uint32_t)(edge >> 32);
            uint32_t v = (uint32_t)(edge & 0xFFFFFFFF);
            uint64_t rev = ((uint64_t)v << 32) | u;
            if (!std::binary_search(patchDirectedEdges.begin(), patchDirectedEdges.end(), rev)) {
                uint64_t key = makeUndirectedEdgeKey(u, v);
                auto it = seamSubdivision.find(key);
                if (it != seamSubdivision.end() && !it->second.empty()) {
                    const auto& inter = it->second;
                    uint32_t prev = u;
                    if (u < v) {
                        for (uint32_t w : inter) {
                            boundaryEdges.push_back({prev, w});
                            prev = w;
                        }
                    } else {
                        for (auto rit = inter.rbegin(); rit != inter.rend(); ++rit) {
                            boundaryEdges.push_back({prev, *rit});
                            prev = *rit;
                        }
                    }
                    boundaryEdges.push_back({prev, v});
                } else {
                    boundaryEdges.push_back({u, v});
                }
            }
        }

        std::sort(boundaryEdges.begin(), boundaryEdges.end());

        std::vector<uint8_t> visitedBoundaryEdges(boundaryEdges.size(), 0);

        auto findNextVertex = [&](uint32_t u) -> std::pair<uint32_t, size_t> {
            auto it = std::lower_bound(boundaryEdges.begin(), boundaryEdges.end(), std::make_pair(u, 0u),
                                       [](const std::pair<uint32_t, uint32_t>& a, const std::pair<uint32_t, uint32_t>& b) {
                                           return a.first < b.first;
                                       });
            while (it != boundaryEdges.end() && it->first == u) {
                size_t idx = std::distance(boundaryEdges.begin(), it);
                if (!visitedBoundaryEdges[idx]) {
                    return {it->second, idx};
                }
                ++it;
            }
            return {UINT32_MAX, 0};
        };

        std::vector<std::vector<uint32_t>> patchLoops;

        for (size_t i = 0; i < boundaryEdges.size(); ++i) {
            if (visitedBoundaryEdges[i]) continue;
            uint32_t startV = boundaryEdges[i].first;
            std::vector<uint32_t> loop;
            uint32_t curr = startV;
            bool closed = false;

            std::vector<uint32_t> currentLoopVerts;
            while (true) {
                currentLoopVerts.push_back(curr);
                loop.push_back(curr);

                auto [nextV, edgeIdx] = findNextVertex(curr);
                if (nextV == UINT32_MAX) break;

                visitedBoundaryEdges[edgeIdx] = 1;

                curr = nextV;
                if (curr == startV) {
                    closed = true;
                    break;
                }
                bool visitedInLoop = false;
                for (uint32_t lv : currentLoopVerts) {
                    if (lv == curr) { visitedInLoop = true; break; }
                }
                if (visitedInLoop) break;
            }

            if (closed && loop.size() >= 3) {
                patchLoops.push_back(std::move(loop));
            }
        }

        sculpt_log("[QUAD CAP PATCH #%zu] Detected %zu boundary loop(s)\n", patchIdx + 1, patchLoops.size());

        // Boundary dihedral analysis disabled in release for high performance (saved 3.5s on 500k polygons)
#if 0
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
        }
#endif

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

        float minStepAllowed = std::max(0.02f, loopAvgEdgeLen * 0.35f);
        step = std::clamp(step, minStepAllowed, 50.0f);
        float margin = std::clamp(step * 0.72f, 0.01f, step * 0.85f);

        // 3. Exact Boundary Loop Preservation:
        // Preserving exact boundary loop vertices is critical to guarantee watertight 2-manifold
        // topology with non-cap mesh faces and avoid T-junctions/holes between adjacent patches.
        // Mesh boundary vertices generated by Manifold CSG are preserved 1:1 without collapsing.

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

        const int gridCols = gxMax - gxMin + 1;
        const int gridRows = gyMax - gyMin + 1;

        // Flat 2D grid index replacing std::map<std::pair<int, int>, uint32_t> gridPtMap
        std::vector<uint32_t> gridPtIndex(gridCols * gridRows, UINT32_MAX);
        auto getGridPtVid = [&](int gx, int gy) -> uint32_t {
            if (gx < gxMin || gx > gxMax || gy < gyMin || gy > gyMax) return UINT32_MAX;
            return gridPtIndex[(gy - gyMin) * gridCols + (gx - gxMin)];
        };
        auto setGridPtVid = [&](int gx, int gy, uint32_t vid) {
            if (gx >= gxMin && gx <= gxMax && gy >= gyMin && gy <= gyMax) {
                gridPtIndex[(gy - gyMin) * gridCols + (gx - gxMin)] = vid;
            }
        };

        // 1. Fast 2D Scanline Domain Rasterization (O(1) domain membership check)
        std::vector<uint8_t> inDomain(gridCols * gridRows, 0);
        for (int row = 0; row < gridRows; ++row) {
            int gy = gyMin + row;
            float y = static_cast<float>(gy) * step;

            // Outer loop scanline intersections
            std::vector<float> outerX;
            const auto& poly0 = allPoly2D[0];
            const size_t n0 = poly0.size();
            for (size_t i = 0; i < n0; ++i) {
                const glm::vec2& p1 = poly0[i];
                const glm::vec2& p2 = poly0[(i + 1) % n0];
                if ((p1.y <= y && p2.y > y) || (p2.y <= y && p1.y > y)) {
                    float x = p1.x + (y - p1.y) / (p2.y - p1.y) * (p2.x - p1.x);
                    outerX.push_back(x);
                }
            }
            std::sort(outerX.begin(), outerX.end());

            for (size_t k = 0; k + 1 < outerX.size(); k += 2) {
                int colStart = std::max(gxMin, static_cast<int>(std::ceil(outerX[k] / step)));
                int colEnd   = std::min(gxMax, static_cast<int>(std::floor(outerX[k + 1] / step)));
                for (int gx = colStart; gx <= colEnd; ++gx) {
                    inDomain[row * gridCols + (gx - gxMin)] = 1;
                }
            }

            // Exclude hole loops
            for (size_t h = 1; h < allPoly2D.size(); ++h) {
                const auto& holePoly = allPoly2D[h];
                const size_t nh = holePoly.size();
                std::vector<float> holeX;
                for (size_t i = 0; i < nh; ++i) {
                    const glm::vec2& p1 = holePoly[i];
                    const glm::vec2& p2 = holePoly[(i + 1) % nh];
                    if ((p1.y <= y && p2.y > y) || (p2.y <= y && p1.y > y)) {
                        float x = p1.x + (y - p1.y) / (p2.y - p1.y) * (p2.x - p1.x);
                        holeX.push_back(x);
                    }
                }
                std::sort(holeX.begin(), holeX.end());
                for (size_t k = 0; k + 1 < holeX.size(); k += 2) {
                    int colStart = std::max(gxMin, static_cast<int>(std::ceil(holeX[k] / step)));
                    int colEnd   = std::min(gxMax, static_cast<int>(std::floor(holeX[k + 1] / step)));
                    for (int gx = colStart; gx <= colEnd; ++gx) {
                        inDomain[row * gridCols + (gx - gxMin)] = 0;
                    }
                }
            }
        }

        // 2. 2D Spatial Grid for Boundary Segments (O(1) margin rejection)
        struct BoundarySeg2D {
            glm::vec2 a, b;
        };
        float segCellSize = std::max(step * 2.0f, margin * 2.5f);
        float segMinU = minU - margin - segCellSize;
        float segMinV = minV - margin - segCellSize;
        float segMaxU = maxU + margin + segCellSize;
        float segMaxV = maxV + margin + segCellSize;
        int segCols = std::max(1, static_cast<int>(std::ceil((segMaxU - segMinU) / segCellSize)));
        int segRows = std::max(1, static_cast<int>(std::ceil((segMaxV - segMinV) / segCellSize)));
        std::vector<std::vector<BoundarySeg2D>> segGrid(segCols * segRows);

        for (const auto& loop : allPoly2D) {
            const size_t nL = loop.size();
            for (size_t i = 0; i < nL; ++i) {
                const glm::vec2& a = loop[i];
                const glm::vec2& b = loop[(i + 1) % nL];
                float bMinX = std::min(a.x, b.x) - margin;
                float bMaxX = std::max(a.x, b.x) + margin;
                float bMinY = std::min(a.y, b.y) - margin;
                float bMaxY = std::max(a.y, b.y) + margin;

                int c0x = std::clamp(static_cast<int>(std::floor((bMinX - segMinU) / segCellSize)), 0, segCols - 1);
                int c1x = std::clamp(static_cast<int>(std::floor((bMaxX - segMinU) / segCellSize)), 0, segCols - 1);
                int c0y = std::clamp(static_cast<int>(std::floor((bMinY - segMinV) / segCellSize)), 0, segRows - 1);
                int c1y = std::clamp(static_cast<int>(std::floor((bMaxY - segMinV) / segCellSize)), 0, segRows - 1);

                for (int cy = c0y; cy <= c1y; ++cy) {
                    for (int cx = c0x; cx <= c1x; ++cx) {
                        segGrid[cy * segCols + cx].push_back({a, b});
                    }
                }
            }
        }

        // 3. 2D Spatial Grid for Patch Triangles (O(1) ray intersection query)
        float triCellSize = std::max(step * 4.0f, 1.0f);
        float triMinU = minU - triCellSize;
        float triMinV = minV - triCellSize;
        float triMaxU = maxU + triCellSize;
        float triMaxV = maxV + triCellSize;
        int triCols = std::max(1, static_cast<int>(std::ceil((triMaxU - triMinU) / triCellSize)));
        int triRows = std::max(1, static_cast<int>(std::ceil((triMaxV - triMinV) / triCellSize)));
        std::vector<std::vector<int>> triGrid(triCols * triRows);

        for (int t = 0; t < static_cast<int>(patchTris.size()); ++t) {
            const auto& tri = patchTris[t];
            int tc0x = std::clamp(static_cast<int>(std::floor((tri.minU - triMinU) / triCellSize)), 0, triCols - 1);
            int tc1x = std::clamp(static_cast<int>(std::floor((tri.maxU - triMinU) / triCellSize)), 0, triCols - 1);
            int tc0y = std::clamp(static_cast<int>(std::floor((tri.minV - triMinV) / triCellSize)), 0, triRows - 1);
            int tc1y = std::clamp(static_cast<int>(std::floor((tri.maxV - triMinV) / triCellSize)), 0, triRows - 1);

            for (int cy = tc0y; cy <= tc1y; ++cy) {
                for (int cx = tc0x; cx <= tc1x; ++cx) {
                    triGrid[cy * triCols + cx].push_back(t);
                }
            }
        }

        // 4. OpenMP Parallel Grid Sampling
        struct TempGridPt {
            int gx, gy;
            glm::vec2 p2D;
            glm::vec3 p3D;
            glm::vec3 n3D;
        };

        struct RowResult {
            std::vector<TempGridPt> pts;
            int rayHits = 0;
            int fallback = 0;
            int rejectedMargin = 0;
        };

        std::vector<RowResult> rowResults(gridRows);
        float raySpan = std::clamp(depthSpan * 3.0f + step * 4.0f, 0.2f, 20.0f);
        const float marginSq = margin * margin;

        #pragma omp parallel for schedule(dynamic)
        for (int row = 0; row < gridRows; ++row) {
            int gy = gyMin + row;
            auto& rowRes = rowResults[row];

            for (int gx = gxMin; gx <= gxMax; ++gx) {
                int cellIdx = row * gridCols + (gx - gxMin);
                if (!inDomain[cellIdx]) continue;

                glm::vec2 pt(static_cast<float>(gx) * step, static_cast<float>(gy) * step);

                // Boundary margin check via spatial grid
                int scx = static_cast<int>(std::floor((pt.x - segMinU) / segCellSize));
                int scy = static_cast<int>(std::floor((pt.y - segMinV) / segCellSize));
                if (scx >= 0 && scx < segCols && scy >= 0 && scy < segRows) {
                    const auto& cellSegs = segGrid[scy * segCols + scx];
                    bool withinMargin = false;
                    for (const auto& seg : cellSegs) {
                        glm::vec2 ab = seg.b - seg.a;
                        float len2 = glm::dot(ab, ab);
                        float t = (len2 > 1e-8f) ? std::clamp(glm::dot(pt - seg.a, ab) / len2, 0.0f, 1.0f) : 0.0f;
                        glm::vec2 diff = pt - (seg.a + t * ab);
                        if (glm::dot(diff, diff) < marginSq) {
                            withinMargin = true;
                            break;
                        }
                    }
                    if (withinMargin) {
                        rowRes.rejectedMargin++;
                        continue;
                    }
                }

                glm::vec3 pPlane = loopCenter + pt.x * U + pt.y * V;

                // Query candidate patch triangles via spatial grid
                int tcx = static_cast<int>(std::floor((pt.x - triMinU) / triCellSize));
                int tcy = static_cast<int>(std::floor((pt.y - triMinV) / triCellSize));

                struct LocalHit {
                    float t, u, v;
                    int triIdx;
                };
                std::vector<LocalHit> hits;

                if (tcx >= 0 && tcx < triCols && tcy >= 0 && tcy < triRows) {
                    const auto& cellTris = triGrid[tcy * triCols + tcx];
                    for (int triIdx : cellTris) {
                        const auto& tri = patchTris[triIdx];
                        if (pt.x < tri.minU || pt.x > tri.maxU || pt.y < tri.minV || pt.y > tri.maxV) continue;
                        float t, u, v;
                        if (intersectRayTriangle(pPlane - normal * raySpan, normal, tri.p0, tri.p1, tri.p2, t, u, v)) {
                            if (t >= 0.0f && t <= raySpan * 2.0f) {
                                hits.push_back({t, u, v, triIdx});
                            }
                        }
                    }
                }

                glm::vec3 p3D(0.0f);
                glm::vec3 n3D = normal;

                auto getHVal = [&]() -> float {
                    float sumW = 0.0f, sumH = 0.0f;
                    for (const auto& rp : allRefPts) {
                        float d2 = glm::dot(pt - rp.p, pt - rp.p);
                        if (d2 < 1e-4f) return rp.h;
                        float w = 1.0f / (d2 * d2);
                        sumW += w;
                        sumH += w * rp.h;
                    }
                    return (sumW > 1e-8f) ? (sumH / sumW) : 0.0f;
                };

                if (hits.size() == 1) {
                    const auto& h = hits[0];
                    const auto& tri = patchTris[h.triIdx];
                    p3D = (1.0f - h.u - h.v) * tri.p0 + h.u * tri.p1 + h.v * tri.p2;
                    glm::vec3 interpN = (1.0f - h.u - h.v) * tri.n0 + h.u * tri.n1 + h.v * tri.n2;
                    float lenN = glm::length(interpN);
                    n3D = (lenN > 1e-6f) ? (interpN / lenN) : tri.normal;
                    rowRes.rayHits++;
                } else if (hits.size() > 1) {
                    float hVal = getHVal();
                    float bestDiff = 1e9f;
                    for (const auto& h : hits) {
                        float depth = h.t - raySpan;
                        float diff = std::abs(depth - hVal);
                        if (diff < bestDiff) {
                            bestDiff = diff;
                            const auto& tri = patchTris[h.triIdx];
                            p3D = (1.0f - h.u - h.v) * tri.p0 + h.u * tri.p1 + h.v * tri.p2;
                            glm::vec3 interpN = (1.0f - h.u - h.v) * tri.n0 + h.u * tri.n1 + h.v * tri.n2;
                            float lenN = glm::length(interpN);
                            n3D = (lenN > 1e-6f) ? (interpN / lenN) : tri.normal;
                        }
                    }
                    if (bestDiff > depthSpan + step * 2.0f) {
                        p3D = pPlane + std::clamp(hVal, -depthSpan, depthSpan) * normal;
                    }
                    rowRes.rayHits++;
                } else {
                    // Fallback projection
                    rowRes.fallback++;
                    float hVal = getHVal();
                    glm::vec3 pGuide = pPlane + std::clamp(hVal, -depthSpan, depthSpan) * normal;
                    float bestD2 = 1e18f;

                    const auto* searchTris = (tcx >= 0 && tcx < triCols && tcy >= 0 && tcy < triRows && !triGrid[tcy * triCols + tcx].empty())
                        ? &triGrid[tcy * triCols + tcx] : nullptr;

                    if (searchTris) {
                        for (int triIdx : *searchTris) {
                            const auto& tri = patchTris[triIdx];
                            glm::vec3 cp = closestPointOnTriangle(pGuide, tri.p0, tri.p1, tri.p2);
                            glm::vec3 diff = cp - pGuide;
                            float d2 = glm::dot(diff, diff);
                            if (d2 < bestD2) {
                                bestD2 = d2;
                                p3D = cp;
                                n3D = tri.normal;
                            }
                        }
                    } else {
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
                }

                rowRes.pts.push_back({gx, gy, pt, p3D, n3D});
            }
        }

        int rayHitsCount = 0;
        int fallbackCount = 0;
        int rejectedByMargin = 0;

        for (const auto& rowRes : rowResults) {
            rayHitsCount += rowRes.rayHits;
            fallbackCount += rowRes.fallback;
            rejectedByMargin += rowRes.rejectedMargin;
            for (const auto& tp : rowRes.pts) {
                uint32_t newVid = baseVid + static_cast<uint32_t>(loopNewVerts.size());
                loopNewVerts.push_back({tp.p3D, tp.n3D});
                setGridPtVid(tp.gx, tp.gy, newVid);
                gridPoints.push_back({tp.gx, tp.gy, newVid, tp.p2D, tp.p3D, tp.n3D});
            }
        }

        // Deviation analysis and 3D stretch analysis (O(N) direct calculation)
        float minDistToOrig = 0.0f, maxDistToOrig = 0.0f, sumDistToOrig = 0.0f;
        int deviatingCount = 0;
        float maxStretch = 1.0f, sumStretch = 0.0f;
        float maxTiltDeg = 0.0f, sumTiltDeg = 0.0f;
        int highStretchCount = 0;

        for (const auto& gp : gridPoints) {
            float dotTilt = std::clamp(glm::dot(gp.n3D, normal), -1.0f, 1.0f);
            float tiltDeg = glm::degrees(std::acos(dotTilt));
            maxTiltDeg = std::max(maxTiltDeg, tiltDeg);
            sumTiltDeg += tiltDeg;
            float sFactor = 1.0f / std::max(0.02f, dotTilt);
            maxStretch = std::max(maxStretch, sFactor);
            sumStretch += sFactor;
            if (sFactor > 2.0f) highStretchCount++;
        }

        float avgDistToOrig = 0.0f;
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

        // ---------------------------------------------------------
        // Belt-Only CDT Architecture: Direct Inner Grid Quads
        // ---------------------------------------------------------
        struct QuadCell {
            uint32_t v00, v10, v11, v01;
            int gx, gy;
        };

        // Helper: 2D segment-segment intersection test
        auto segmentsIntersect2D = [](const glm::vec2& p1, const glm::vec2& p2,
                                       const glm::vec2& q1, const glm::vec2& q2) -> bool {
            auto ccw = [](const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) -> float {
                return (c.y - a.y) * (b.x - a.x) - (b.y - a.y) * (c.x - a.x);
            };
            float c1 = ccw(p1, p2, q1);
            float c2 = ccw(p1, p2, q2);
            float c3 = ccw(q1, q2, p1);
            float c4 = ccw(q1, q2, p2);
            return (((c1 > 1e-6f && c2 < -1e-6f) || (c1 < -1e-6f && c2 > 1e-6f)) &&
                    ((c3 > 1e-6f && c4 < -1e-6f) || (c3 < -1e-6f && c4 > 1e-6f)));
        };

        auto quadEdgesIntersectBoundary = [&](int gx, int gy) -> bool {
            glm::vec2 p00(static_cast<float>(gx) * step, static_cast<float>(gy) * step);
            glm::vec2 p10(static_cast<float>(gx + 1) * step, static_cast<float>(gy) * step);
            glm::vec2 p11(static_cast<float>(gx + 1) * step, static_cast<float>(gy + 1) * step);
            glm::vec2 p01(static_cast<float>(gx) * step, static_cast<float>(gy + 1) * step);

            float bMinX = p00.x - 1e-4f;
            float bMaxX = p11.x + 1e-4f;
            float bMinY = p00.y - 1e-4f;
            float bMaxY = p11.y + 1e-4f;

            int sc0x = std::clamp(static_cast<int>(std::floor((bMinX - segMinU) / segCellSize)), 0, segCols - 1);
            int sc1x = std::clamp(static_cast<int>(std::floor((bMaxX - segMinU) / segCellSize)), 0, segCols - 1);
            int sc0y = std::clamp(static_cast<int>(std::floor((bMinY - segMinV) / segCellSize)), 0, segRows - 1);
            int sc1y = std::clamp(static_cast<int>(std::floor((bMaxY - segMinV) / segCellSize)), 0, segRows - 1);

            for (int scy = sc0y; scy <= sc1y; ++scy) {
                for (int scx = sc0x; scx <= sc1x; ++scx) {
                    const auto& cellSegs = segGrid[scy * segCols + scx];
                    for (const auto& seg : cellSegs) {
                        if (segmentsIntersect2D(p00, p10, seg.a, seg.b) ||
                            segmentsIntersect2D(p10, p11, seg.a, seg.b) ||
                            segmentsIntersect2D(p11, p01, seg.a, seg.b) ||
                            segmentsIntersect2D(p01, p00, seg.a, seg.b)) {
                            return true;
                        }
                    }
                }
            }
            return false;
        };

        std::vector<uint8_t> isCandidateQuad(gridCols * gridRows, 0);

        for (const auto& g : gridPoints) {
            int gx = g.gx;
            int gy = g.gy;
            uint32_t v10 = getGridPtVid(gx + 1, gy);
            uint32_t v11 = getGridPtVid(gx + 1, gy + 1);
            uint32_t v01 = getGridPtVid(gx, gy + 1);

            if (v10 != UINT32_MAX && v11 != UINT32_MAX && v01 != UINT32_MAX) {
                glm::vec2 pCenter((static_cast<float>(gx) + 0.5f) * step, (static_cast<float>(gy) + 0.5f) * step);
                if (pointInPatchDomain(pCenter) && !quadEdgesIntersectBoundary(gx, gy)) {
                    isCandidateQuad[(gy - gyMin) * gridCols + (gx - gxMin)] = 1;
                }
            }
        }

        // Checkerboard pinch resolution: eliminate diagonal-only corner contacts
        // to guarantee that the quad complex boundary consists of strictly degree-2 vertices.
        bool pinchesRemoved = true;
        int pinchIterations = 0;
        while (pinchesRemoved && pinchIterations < 5) {
            pinchesRemoved = false;
            pinchIterations++;
            for (int gy = gyMin + 1; gy <= gyMax; ++gy) {
                for (int gx = gxMin + 1; gx <= gxMax; ++gx) {
                    int c00 = (gy - 1 - gyMin) * gridCols + (gx - 1 - gxMin);
                    int c10 = (gy - 1 - gyMin) * gridCols + (gx - gxMin);
                    int c11 = (gy - gyMin) * gridCols + (gx - gxMin);
                    int c01 = (gy - gyMin) * gridCols + (gx - 1 - gxMin);

                    uint8_t q00 = isCandidateQuad[c00];
                    uint8_t q10 = isCandidateQuad[c10];
                    uint8_t q11 = isCandidateQuad[c11];
                    uint8_t q01 = isCandidateQuad[c01];

                    if (q00 && q11 && !q10 && !q01) {
                        isCandidateQuad[c11] = 0;
                        pinchesRemoved = true;
                    } else if (q10 && q01 && !q00 && !q11) {
                        isCandidateQuad[c01] = 0;
                        pinchesRemoved = true;
                    }
                }
            }
        }

        std::vector<QuadCell> acceptedGridQuads;
        std::vector<uint8_t> hasQuadCell(gridCols * gridRows, 0);

        for (int gy = gyMin; gy <= gyMax; ++gy) {
            for (int gx = gxMin; gx <= gxMax; ++gx) {
                int cellIdx = (gy - gyMin) * gridCols + (gx - gxMin);
                if (!isCandidateQuad[cellIdx]) continue;
                uint32_t v00 = getGridPtVid(gx, gy);
                uint32_t v10 = getGridPtVid(gx + 1, gy);
                uint32_t v11 = getGridPtVid(gx + 1, gy + 1);
                uint32_t v01 = getGridPtVid(gx, gy + 1);
                if (v00 != UINT32_MAX && v10 != UINT32_MAX && v11 != UINT32_MAX && v01 != UINT32_MAX) {
                    acceptedGridQuads.push_back({v00, v10, v11, v01, gx, gy});
                    hasQuadCell[cellIdx] = 1;
                }
            }
        }

        // Extract boundary half-edges of accepted grid quads
        struct GridBndEdge {
            uint32_t u, v;
            int gx_u, gy_u, gx_v, gy_v;
        };
        std::vector<GridBndEdge> gridBndEdges;

        for (const auto& q : acceptedGridQuads) {
            int gx = q.gx;
            int gy = q.gy;
            // Bottom: (gx, gy) -> (gx+1, gy)
            if (gy == gyMin || !hasQuadCell[(gy - 1 - gyMin) * gridCols + (gx - gxMin)]) {
                gridBndEdges.push_back({q.v00, q.v10, gx, gy, gx + 1, gy});
            }
            // Right: (gx+1, gy) -> (gx+1, gy+1)
            if (gx == gxMax || !hasQuadCell[(gy - gyMin) * gridCols + (gx + 1 - gxMin)]) {
                gridBndEdges.push_back({q.v10, q.v11, gx + 1, gy, gx + 1, gy + 1});
            }
            // Top: (gx+1, gy+1) -> (gx, gy+1)
            if (gy == gyMax || !hasQuadCell[(gy + 1 - gyMin) * gridCols + (gx - gxMin)]) {
                gridBndEdges.push_back({q.v11, q.v01, gx + 1, gy + 1, gx, gy + 1});
            }
            // Left: (gx, gy+1) -> (gx, gy)
            if (gx == gxMin || !hasQuadCell[(gy - gyMin) * gridCols + (gx - 1 - gxMin)]) {
                gridBndEdges.push_back({q.v01, q.v00, gx, gy + 1, gx, gy});
            }
        }

        std::unordered_map<uint32_t, uint32_t> nextBndVert;
        std::unordered_map<uint32_t, glm::vec2> bndVertPos2D;
        for (const auto& e : gridBndEdges) {
            nextBndVert[e.u] = e.v;
            bndVertPos2D[e.u] = glm::vec2(static_cast<float>(e.gx_u) * step, static_cast<float>(e.gy_u) * step);
            bndVertPos2D[e.v] = glm::vec2(static_cast<float>(e.gx_v) * step, static_cast<float>(e.gy_v) * step);
        }

        std::vector<std::vector<uint32_t>> gridBoundaryLoops;
        std::unordered_set<uint32_t> visitedBndVerts;
        for (const auto& e : gridBndEdges) {
            if (visitedBndVerts.count(e.u)) continue;
            std::vector<uint32_t> loop;
            uint32_t curr = e.u;
            while (!visitedBndVerts.count(curr)) {
                visitedBndVerts.insert(curr);
                loop.push_back(curr);
                auto it = nextBndVert.find(curr);
                if (it == nextBndVert.end()) break;
                curr = it->second;
            }
            if (loop.size() >= 3) {
                gridBoundaryLoops.push_back(std::move(loop));
            }
        }

        sculpt_log("[QUAD CAP PATCH #%zu] Belt-Only CDT: %zu accepted grid quads (100%% clean interior), %zu boundary loops (%zu bnd edges)\n",
                   patchIdx + 1, acceptedGridQuads.size(), gridBoundaryLoops.size(), gridBndEdges.size());

        // CDT Triangulation for transition belt only
        CDT::Triangulation<float> cdt(CDT::VertexInsertionOrder::Auto, CDT::IntersectingConstraintEdges::TryResolve, 1e-4f);
        std::vector<CDT::V2d<float>> cdtVerts;
        std::vector<uint32_t> cdtToMeshVid;
        std::vector<CDT::Edge> cdtEdges;

        if (acceptedGridQuads.empty()) {
            // Fallback: triangulate patch loops directly
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
        } else {
            // 1. Patch outer & hole loops (mesh boundary)
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

            // 2. Grid boundary loops (inner boundary / hole of transition belt)
            std::unordered_map<uint32_t, CDT::VertInd> meshVidToCdtVert;
            for (const auto& loop : gridBoundaryLoops) {
                for (uint32_t vid : loop) {
                    if (meshVidToCdtVert.find(vid) == meshVidToCdtVert.end()) {
                        CDT::VertInd cdtIdx = static_cast<CDT::VertInd>(cdtVerts.size());
                        meshVidToCdtVert[vid] = cdtIdx;
                        const glm::vec2& p = bndVertPos2D[vid];
                        cdtVerts.push_back(CDT::V2d<float>::make(p.x, p.y));
                        cdtToMeshVid.push_back(vid);
                    }
                }
            }

            for (const auto& loop : gridBoundaryLoops) {
                for (size_t i = 0; i < loop.size(); ++i) {
                    size_t j = (i + 1) % loop.size();
                    CDT::VertInd vA = meshVidToCdtVert[loop[i]];
                    CDT::VertInd vB = meshVidToCdtVert[loop[j]];
                    if (vA != vB) {
                        cdtEdges.push_back(CDT::Edge(vA, vB));
                    }
                }
            }
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

        sculpt_log("[QUAD CAP PATCH #%zu] Transition Belt CDT: %zu triangles generated in %.2f ms (cdtVerts: %zu, cdtEdges: %zu)\n",
                   patchIdx + 1, cdt.triangles.size(), cdtMs, cdtVerts.size(), cdtEdges.size());

        std::vector<std::array<uint32_t, 3>> transitionTris;
        transitionTris.reserve(cdt.triangles.size());

        for (const auto& tri : cdt.triangles) {
            if (tri.vertices[0] >= cdtToMeshVid.size() ||
                tri.vertices[1] >= cdtToMeshVid.size() ||
                tri.vertices[2] >= cdtToMeshVid.size()) {
                continue;
            }
            uint32_t m0 = cdtToMeshVid[tri.vertices[0]];
            uint32_t m1 = cdtToMeshVid[tri.vertices[1]];
            uint32_t m2 = cdtToMeshVid[tri.vertices[2]];
            if (m0 != m1 && m1 != m2 && m2 != m0) {
                transitionTris.push_back({m0, m1, m2});
            }
        }

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

        auto getVertNorm3D = [&](uint32_t vid) -> glm::vec3 {
            if (vid < mesh.verts.size() / 3) {
                if (!mesh.normals.empty() && vid * 3 + 2 < mesh.normals.size()) {
                    return glm::vec3(mesh.normals[vid * 3], mesh.normals[vid * 3 + 1], mesh.normals[vid * 3 + 2]);
                }
                return patchNormal;
            } else {
                size_t off = vid - baseVid;
                if (off < loopNewVerts.size()) {
                    return loopNewVerts[off].normal;
                }
            }
            return patchNormal;
        };

        const size_t numTransTris = transitionTris.size();
        struct TransEdgeHalf {
            uint64_t key;
            int triIdx;
            uint32_t vOpp;
            bool forward;
        };

        std::vector<TransEdgeHalf> transEdges(numTransTris * 3);
        for (size_t t = 0; t < numTransTris; ++t) {
            const auto& tri = transitionTris[t];
            uint32_t i0 = tri[0], i1 = tri[1], i2 = tri[2];
            transEdges[t * 3 + 0] = {makeUndirectedEdgeKey(i0, i1), (int)t, i2, i0 < i1};
            transEdges[t * 3 + 1] = {makeUndirectedEdgeKey(i1, i2), (int)t, i0, i1 < i2};
            transEdges[t * 3 + 2] = {makeUndirectedEdgeKey(i2, i0), (int)t, i1, i2 < i0};
        }

        std::sort(transEdges.begin(), transEdges.end(), [](const TransEdgeHalf& a, const TransEdgeHalf& b) {
            return a.key < b.key;
        });

        struct CandidateQuadPair {
            int tA, tB;
            uint32_t q0, q1, q2, q3;
            float score;
        };
        int numThreads = omp_get_max_threads();
        std::vector<std::vector<CandidateQuadPair>> threadCandidates(numThreads);
        std::vector<int> threadNonConvex(numThreads, 0);
        std::vector<int> threadDiagRatio(numThreads, 0);
        std::vector<int> threadEdgeRatio(numThreads, 0);
        std::vector<int> threadAngles(numThreads, 0);
        std::vector<size_t> threadTransEdges(numThreads, 0);

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            size_t n = transEdges.size();
            size_t rawStart = (n * tid) / numThreads;
            size_t rawEnd = (n * (tid + 1)) / numThreads;

            if (rawStart > 0) {
                while (rawStart < n && transEdges[rawStart].key == transEdges[rawStart - 1].key) {
                    ++rawStart;
                }
            }
            if (rawEnd < n) {
                while (rawEnd < n && transEdges[rawEnd].key == transEdges[rawEnd - 1].key) {
                    ++rawEnd;
                }
            }

            auto& localCands = threadCandidates[tid];
            if (rawStart < rawEnd) {
                localCands.reserve((rawEnd - rawStart) / 4);
                for (size_t i = rawStart; i < rawEnd; ) {
                    size_t j = i + 1;
                    while (j < rawEnd && transEdges[j].key == transEdges[i].key) ++j;
                    threadTransEdges[tid]++;
                    size_t count = j - i;
                    if (count == 2) {
                        const auto& hA = transEdges[i];
                        const auto& hB = transEdges[i + 1];
                        if (hA.forward != hB.forward) { // Consistent winding
                            int tA = hA.triIdx;
                            int tB = hB.triIdx;

                            uint64_t edgeKey = hA.key;
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
                                threadNonConvex[tid]++;
                            } else {
                                // Diagonal ratio
                                float d1 = glm::distance(p0, p2);
                                float d2 = glm::distance(p1, p3);
                                if (d1 < 1e-4f || d2 < 1e-4f) {
                                    threadDiagRatio[tid]++;
                                } else {
                                    bool isGridOnlyA = (transitionTris[tA][0] >= baseVid && transitionTris[tA][1] >= baseVid && transitionTris[tA][2] >= baseVid);
                                    bool isGridOnlyB = (transitionTris[tB][0] >= baseVid && transitionTris[tB][1] >= baseVid && transitionTris[tB][2] >= baseVid);
                                    bool isCornerPair = (isGridOnlyA || isGridOnlyB);

                                    float minDiagRatioAllowed = 0.40f;
                                    float minEdgeRatioAllowed = 0.40f;
                                    float minAngleAllowed = isCornerPair ? 16.0f : 22.0f;
                                    float maxAngleAllowed = isCornerPair ? 164.0f : 158.0f;

                                    float diagRatio = std::min(d1, d2) / std::max(d1, d2);
                                    if (diagRatio < minDiagRatioAllowed) {
                                        threadDiagRatio[tid]++;
                                    } else {
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
                                        if (lMax <= 1e-5f || (lMin / lMax) < minEdgeRatioAllowed) {
                                            threadEdgeRatio[tid]++;
                                        } else {
                                            // Interior angles
                                            auto angleDeg = [](const glm::vec2& vIn, const glm::vec2& vOut, float crossVal) -> float {
                                                float dotVal = glm::dot(vIn, vOut);
                                                return glm::degrees(std::atan2(crossVal, dotVal));
                                            };

                                            float a0 = angleDeg(p0 - p3, p1 - p0, c0);
                                            float a1 = angleDeg(p1 - p0, p2 - p1, c1);
                                            float a2 = angleDeg(p2 - p1, p3 - p2, c2);
                                            float a3 = angleDeg(p3 - p2, p0 - p3, c3);

                                            if (a0 < minAngleAllowed || a0 > maxAngleAllowed || a1 < minAngleAllowed || a1 > maxAngleAllowed ||
                                                a2 < minAngleAllowed || a2 > maxAngleAllowed || a3 < minAngleAllowed || a3 > maxAngleAllowed) {
                                                threadAngles[tid]++;
                                            } else {
                                                // Quality score: diagonal ratio + edge ratio + proximity of angles to 90 deg
                                                float angleScore = 0.0f;
                                                for (float a : {a0, a1, a2, a3}) {
                                                    angleScore += std::sin(glm::radians(a));
                                                }
                                                float score = diagRatio * 2.5f + (lMin / lMax) * 1.5f + angleScore;
                                                if (isCornerPair) score += 10.0f;

                                                bool isLadderQuad = false;
                                                if ((q0 < baseVid && q1 < baseVid && q2 >= baseVid && q3 >= baseVid) ||
                                                    (q1 < baseVid && q2 < baseVid && q3 >= baseVid && q0 >= baseVid) ||
                                                    (q2 < baseVid && q3 < baseVid && q0 >= baseVid && q1 >= baseVid) ||
                                                    (q3 < baseVid && q0 < baseVid && q1 >= baseVid && q2 >= baseVid)) {
                                                    isLadderQuad = true;
                                                }
                                                if (isLadderQuad) score += 50.0f;

                                                localCands.push_back({tA, tB, q0, q1, q2, q3, score});
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    i = j;
                }
            }
        }

        size_t totalCands = 0;
        for (const auto& tc : threadCandidates) totalCands += tc.size();
        std::vector<CandidateQuadPair> candidatePairs;
        candidatePairs.reserve(totalCands);
        for (auto& tc : threadCandidates) {
            candidatePairs.insert(candidatePairs.end(), tc.begin(), tc.end());
        }

        int rejectedNonConvex = 0;
        int rejectedDiagRatio = 0;
        int rejectedEdgeRatio = 0;
        int rejectedAngles = 0;
        size_t totalTransEdgesCount = 0;
        for (int tid = 0; tid < numThreads; ++tid) {
            rejectedNonConvex += threadNonConvex[tid];
            rejectedDiagRatio += threadDiagRatio[tid];
            rejectedEdgeRatio += threadEdgeRatio[tid];
            rejectedAngles += threadAngles[tid];
            totalTransEdgesCount += threadTransEdges[tid];
        }

        std::sort(candidatePairs.begin(), candidatePairs.end(), [](const CandidateQuadPair& a, const CandidateQuadPair& b) {
            return a.score > b.score;
        });

        sculpt_log("[QUAD CAP PATCH #%zu] Transition Pairs: %zu internal edges -> %zu valid quads (Rejected: %d reflex/concave, %d sheared/diag, %d narrow edges, %d extreme angles)\n",
                   patchIdx + 1, totalTransEdgesCount, candidatePairs.size(),
                   rejectedNonConvex, rejectedDiagRatio, rejectedEdgeRatio, rejectedAngles);

        std::vector<bool> triUsed(numTransTris, false);
        for (const auto& cp : candidatePairs) {
            if (!triUsed[cp.tA] && !triUsed[cp.tB]) {
                triUsed[cp.tA] = true;
                triUsed[cp.tB] = true;
                transitionQuads.push_back({cp.q0, cp.q1, cp.q2, cp.q3});
            }
        }

        // 2nd pass: pair any remaining adjacent triangles that form a well-proportioned convex quad
        for (size_t i = 0; i < transEdges.size(); ) {
            size_t j = i + 1;
            while (j < transEdges.size() && transEdges[j].key == transEdges[i].key) ++j;
            if (j - i == 2) {
                const auto& hA = transEdges[i];
                const auto& hB = transEdges[i + 1];
                int tA = hA.triIdx;
                int tB = hB.triIdx;
                if (!triUsed[tA] && !triUsed[tB] && hA.forward != hB.forward) {
                    uint32_t u = (uint32_t)(hA.key >> 32);
                    uint32_t v = (uint32_t)(hA.key & 0xFFFFFFFF);
                    uint32_t wA = hA.vOpp;
                    uint32_t wB = hB.vOpp;
                    if (!hA.forward) std::swap(u, v);
                    uint32_t q0 = u, q1 = wB, q2 = v, q3 = wA;
                    glm::vec2 p0 = getVertPos2D(q0);
                    glm::vec2 p1 = getVertPos2D(q1);
                    glm::vec2 p2 = getVertPos2D(q2);
                    glm::vec2 p3 = getVertPos2D(q3);
                    auto cross2D = [](const glm::vec2& a, const glm::vec2& b) -> float {
                        return a.x * b.y - a.y * b.x;
                    };
                    float c0 = cross2D(p0 - p3, p1 - p0);
                    float c1 = cross2D(p1 - p0, p2 - p1);
                    float c2 = cross2D(p2 - p1, p3 - p2);
                    float c3 = cross2D(p3 - p2, p0 - p3);
                    if (c0 > 1e-6f && c1 > 1e-6f && c2 > 1e-6f && c3 > 1e-6f) {
                        float d1 = glm::distance(p0, p2);
                        float d2 = glm::distance(p1, p3);
                        float l0 = glm::distance(p0, p1);
                        float l1 = glm::distance(p1, p2);
                        float l2 = glm::distance(p2, p3);
                        float l3 = glm::distance(p3, p0);
                        float lMin = std::min({l0, l1, l2, l3});
                        float lMax = std::max({l0, l1, l2, l3});
                        float diagRatio = (std::max(d1, d2) > 1e-5f) ? (std::min(d1, d2) / std::max(d1, d2)) : 0.0f;
                        float edgeRatio = (lMax > 1e-5f) ? (lMin / lMax) : 0.0f;
                        if (diagRatio >= 0.38f && edgeRatio >= 0.38f) {
                            triUsed[tA] = true;
                            triUsed[tB] = true;
                            transitionQuads.push_back({q0, q1, q2, q3});
                        }
                    }
                }
            }
            i = j;
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

        // Vertex compaction: only stage vertices that are actually referenced in faces
        std::vector<uint32_t> remapNewVid(loopNewVerts.size(), UINT32_MAX);
        std::vector<StagedVert> compactedVerts;
        compactedVerts.reserve(loopNewVerts.size());

        auto getOrAddCompactedVid = [&](uint32_t vid) -> uint32_t {
            if (vid < baseVid) return vid;
            size_t off = vid - baseVid;
            if (off >= loopNewVerts.size()) return vid;
            if (remapNewVid[off] == UINT32_MAX) {
                uint32_t newVid = baseVid + static_cast<uint32_t>(compactedVerts.size());
                remapNewVid[off] = newVid;
                compactedVerts.push_back(loopNewVerts[off]);
            }
            return remapNewVid[off];
        };

        // Emit new patch faces into buffers:
        // 1. Inner grid quads
        for (const auto& q : acceptedGridQuads) {
            newFaces.push_back(getOrAddCompactedVid(q.v00));
            newFaces.push_back(getOrAddCompactedVid(q.v10));
            newFaces.push_back(getOrAddCompactedVid(q.v11));
            newFaces.push_back(getOrAddCompactedVid(q.v01));
            newFaceGroups.push_back(capGroupID);
        }

        // 2. Transition quads
        for (const auto& tq : transitionQuads) {
            newFaces.push_back(getOrAddCompactedVid(tq.v0));
            newFaces.push_back(getOrAddCompactedVid(tq.v1));
            newFaces.push_back(getOrAddCompactedVid(tq.v2));
            newFaces.push_back(getOrAddCompactedVid(tq.v3));
            newFaceGroups.push_back(capGroupID);
        }

        // 3. Remaining transition triangles
        for (const auto& t : remainingTris) {
            if (t[0] == t[1] || t[1] == t[2] || t[2] == t[0]) continue;
            newFaces.push_back(getOrAddCompactedVid(t[0]));
            newFaces.push_back(getOrAddCompactedVid(t[1]));
            newFaces.push_back(getOrAddCompactedVid(t[2]));
            newFaces.push_back(TRI_INDEX);
            newFaceGroups.push_back(capGroupID);
        }

        // Stage newly created grid vertices
        stagedVerts.insert(stagedVerts.end(), compactedVerts.begin(), compactedVerts.end());

        int patchQuads = static_cast<int>(acceptedGridQuads.size() + transitionQuads.size());
        int patchTrisGenerated = static_cast<int>(remainingTris.size());

        totalQuadsGenerated += patchQuads;
        totalTrisGenerated += patchTrisGenerated;
        anyPatchQuadCapped = true;
        result.patchesQuadCapped++;

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
        mesh.postInit();

        mesh.isDirty = true;
        mesh.isTopologyDirty = true;
        mesh.isVertexDirty = true;
        mesh.isColorDirty = true;
        mesh.isFaceGroupDirty = true;
        mesh.invalidateLocalRadius();

        result.success = true;
    } else {
        mesh.verts.resize(origNbVerts * 3);
        mesh.normals.resize(origNbVerts * 3);
        mesh.colors.resize(origNbVerts * 3);
        mesh.materials.resize(origNbVerts * 3);
        mesh.nbVerts = origNbVerts;

        // If no patch was quad capped (e.g. preserved original manifold cap),
        // ensure mesh has valid topology, normals, and octree because toMesh skipped runPostInit.
        mesh.hasCustomNormals = false;
        mesh.postInit();
        mesh.isDirty = true;
        mesh.isTopologyDirty = true;
        mesh.isVertexDirty = true;
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
