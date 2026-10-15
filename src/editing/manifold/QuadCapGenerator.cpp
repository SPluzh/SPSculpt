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
    sculpt_log("[QUAD CAP] Total faces: %d, Cap faces count with ID %u: %zu\n", nbFaces, capGroupID, capFaceIndices.size());

    // 2. Extract original cap triangles and analyze 3D geometry / curvature
    struct OrigCapTri {
        glm::vec3 p0, p1, p2;
        glm::vec3 n0, n1, n2;
        glm::vec3 normal;
        float area;
        float minU, minV, maxU, maxV; // 2D AABB in (U, V) basis
    };
    std::vector<OrigCapTri> origCapTris;
    origCapTris.reserve(capFaceIndices.size() * 2);

    std::unordered_set<uint32_t> capUniqueVerts;
    glm::vec3 capBBoxMin(1e9f);
    glm::vec3 capBBoxMax(-1e9f);

    auto addCapTri = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
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
            origCapTris.push_back({p0, p1, p2, n0, n1, n2, fn / len, len * 0.5f, 0.0f, 0.0f, 0.0f, 0.0f});
        }
    };

    for (int f : capFaceIndices) {
        uint32_t v0 = mesh.faces[f * 4 + 0];
        uint32_t v1 = mesh.faces[f * 4 + 1];
        uint32_t v2 = mesh.faces[f * 4 + 2];
        uint32_t v3 = mesh.faces[f * 4 + 3];

        addCapTri(v0, v1, v2);
        capUniqueVerts.insert(v0);
        capUniqueVerts.insert(v1);
        capUniqueVerts.insert(v2);

        if (v3 != TRI_INDEX) {
            addCapTri(v0, v2, v3);
            capUniqueVerts.insert(v3);
        }
    }

    for (uint32_t vi : capUniqueVerts) {
        glm::vec3 p(mesh.verts[vi * 3], mesh.verts[vi * 3 + 1], mesh.verts[vi * 3 + 2]);
        capBBoxMin = glm::min(capBBoxMin, p);
        capBBoxMax = glm::max(capBBoxMax, p);
    }

    glm::vec3 cutNormal(0.0f);
    glm::vec3 cutCenter(0.0f);
    float totalCapArea = 0.0f;
    for (const auto& tri : origCapTris) {
        cutNormal += tri.normal * tri.area;
        cutCenter += (tri.p0 + tri.p1 + tri.p2) * (tri.area / 3.0f);
        totalCapArea += tri.area;
    }

    if (totalCapArea > 1e-6f) {
        cutNormal = glm::normalize(cutNormal);
        cutCenter /= totalCapArea;
    } else {
        cutNormal = glm::vec3(0.0f, 1.0f, 0.0f);
    }

    float minAngleDeg = 180.0f;
    float maxAngleDeg = 0.0f;
    float sumAngleWeighted = 0.0f;
    for (const auto& tri : origCapTris) {
        float dotN = std::clamp(glm::dot(tri.normal, cutNormal), -1.0f, 1.0f);
        float angleDeg = glm::degrees(std::acos(dotN));
        minAngleDeg = std::min(minAngleDeg, angleDeg);
        maxAngleDeg = std::max(maxAngleDeg, angleDeg);
        sumAngleWeighted += angleDeg * tri.area;
    }
    float avgAngleDeg = (totalCapArea > 1e-6f) ? (sumAngleWeighted / totalCapArea) : 0.0f;

    float capDepthMin = 1e9f;
    float capDepthMax = -1e9f;
    float sumDepth = 0.0f;
    for (uint32_t vi : capUniqueVerts) {
        glm::vec3 p(mesh.verts[vi * 3], mesh.verts[vi * 3 + 1], mesh.verts[vi * 3 + 2]);
        float d = glm::dot(p - cutCenter, cutNormal);
        capDepthMin = std::min(capDepthMin, d);
        capDepthMax = std::max(capDepthMax, d);
        sumDepth += d;
    }
    float capDepthSpan = capDepthMax - capDepthMin;
    float avgCapDepth = capUniqueVerts.empty() ? 0.0f : (sumDepth / (float)capUniqueVerts.size());

    sculpt_log("[QUAD CAP CURVATURE] Cap faces: %zu (tris: %zu, unique verts: %zu), Area: %.2f mm2\n",
               capFaceIndices.size(), origCapTris.size(), capUniqueVerts.size(), totalCapArea);
    sculpt_log("[QUAD CAP CURVATURE] 3D BBox: [%.2f, %.2f, %.2f] to [%.2f, %.2f, %.2f], extent: [%.2f, %.2f, %.2f]\n",
               capBBoxMin.x, capBBoxMin.y, capBBoxMin.z, capBBoxMax.x, capBBoxMax.y, capBBoxMax.z,
               capBBoxMax.x - capBBoxMin.x, capBBoxMax.y - capBBoxMin.y, capBBoxMax.z - capBBoxMin.z);
    sculpt_log("[QUAD CAP CURVATURE] Face Normals vs Avg Normal [%.2f, %.2f, %.2f]: min=%.1f deg, max=%.1f deg, area-weighted avg=%.1f deg\n",
               cutNormal.x, cutNormal.y, cutNormal.z, minAngleDeg, maxAngleDeg, avgAngleDeg);
    sculpt_log("[QUAD CAP CURVATURE] Original Cap Depth along CutNormal: min=%.2f, max=%.2f, span=%.2f mm (avg=%.2f)\n",
               capDepthMin, capDepthMax, capDepthSpan, avgCapDepth);

    // 2. Extract boundary halfedges of the cap region
    // A directed edge (u, v) is a boundary edge if the face it belongs to is in capFaceIndices,
    // but the twin edge (v, u) does not exist or belongs to a non-cap face.
    struct EdgeKey {
        uint32_t u, v;
        bool operator==(const EdgeKey& o) const { return u == o.u && v == o.v; }
    };
    struct EdgeKeyHash {
        size_t operator()(const EdgeKey& k) const {
            return std::hash<uint64_t>()(((uint64_t)k.u << 32) | (uint64_t)k.v);
        }
    };

    std::unordered_map<EdgeKey, int, EdgeKeyHash> directedEdges;
    directedEdges.reserve(capFaceIndices.size() * 3);

    for (int f : capFaceIndices) {
        uint32_t v0 = mesh.faces[f * 4 + 0];
        uint32_t v1 = mesh.faces[f * 4 + 1];
        uint32_t v2 = mesh.faces[f * 4 + 2];
        uint32_t v3 = mesh.faces[f * 4 + 3];

        directedEdges[{v0, v1}] = f;
        directedEdges[{v1, v2}] = f;
        if (v3 != TRI_INDEX) {
            directedEdges[{v2, v3}] = f;
            directedEdges[{v3, v0}] = f;
        } else {
            directedEdges[{v2, v0}] = f;
        }
    }

    // A boundary directed edge (u, v) has no reverse edge (v, u) in cap faces
    std::unordered_map<uint32_t, uint32_t> nextVertex;
    for (const auto& [edge, fIdx] : directedEdges) {
        EdgeKey rev{edge.v, edge.u};
        if (directedEdges.find(rev) == directedEdges.end()) {
            nextVertex[edge.u] = edge.v;
        }
    }

    if (nextVertex.empty()) {
        return result;
    }

    // 3. Assemble boundary edges into closed loops
    std::vector<std::vector<uint32_t>> loops;
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
            if (curr == startV) {
                closed = true;
                break;
            }
        }

        if (closed && loop.size() >= 3) {
            loops.push_back(std::move(loop));
        }
    }

    if (loops.empty()) {
        return result;
    }

    sculpt_log("[QUAD CAP] Detected %zu boundary loop(s) for Cap Group ID: %u\n", loops.size(), capGroupID);

    // Filter out old cap faces
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

    int totalQuadsGenerated = 0;
    int totalTrisGenerated = 0;

    // Process each boundary loop
    for (const auto& loop : loops) {
        const size_t K = loop.size();
        if (K < 3) continue;

        // Loop centroid and Newell normal
        glm::vec3 loopCenter(0.0f);
        glm::vec3 loopNewell(0.0f);
        float totalEdgeLen = 0.0f;

        for (size_t i = 0; i < K; ++i) {
            size_t j = (i + 1) % K;
            uint32_t vi = loop[i];
            uint32_t vj = loop[j];

            glm::vec3 pi(mesh.verts[vi * 3], mesh.verts[vi * 3 + 1], mesh.verts[vi * 3 + 2]);
            glm::vec3 pj(mesh.verts[vj * 3], mesh.verts[vj * 3 + 1], mesh.verts[vj * 3 + 2]);

            loopCenter += pi;
            totalEdgeLen += glm::distance(pi, pj);

            loopNewell.x += (pi.y - pj.y) * (pi.z + pj.z);
            loopNewell.y += (pi.z - pj.z) * (pi.x + pj.x);
            loopNewell.z += (pi.x - pj.x) * (pi.y + pj.y);
        }

        loopCenter /= static_cast<float>(K);

        // Align cutNormal with loop's boundary orientation
        glm::vec3 normal = cutNormal;
        if (glm::length(loopNewell) > 1e-6f && glm::dot(normal, loopNewell) < 0.0f) {
            normal = -normal;
        }

        float loopDepthMin = 1e9f;
        float loopDepthMax = -1e9f;
        for (size_t i = 0; i < K; ++i) {
            uint32_t vi = loop[i];
            glm::vec3 p(mesh.verts[vi * 3], mesh.verts[vi * 3 + 1], mesh.verts[vi * 3 + 2]);
            float d = glm::dot(p - loopCenter, normal);
            loopDepthMin = std::min(loopDepthMin, d);
            loopDepthMax = std::max(loopDepthMax, d);
        }
        float loopDepthSpan = loopDepthMax - loopDepthMin;

        sculpt_log("[QUAD CAP CURVATURE] Boundary Loop %zu verts, perimeter=%.2f mm\n", K, totalEdgeLen);
        sculpt_log("[QUAD CAP CURVATURE] Boundary Loop Depth along Normal: min=%.2f, max=%.2f, span=%.2f mm (vs Cap span: %.2f mm)\n",
                   loopDepthMin, loopDepthMax, loopDepthSpan, capDepthSpan);
        if (capDepthSpan > 0.01f && loopDepthSpan > 0.01f) {
            sculpt_log("[QUAD CAP CURVATURE] Interior vs Boundary Depth Ratio: %.2fx (%s)\n",
                       capDepthSpan / loopDepthSpan,
                       (capDepthSpan > 1.5f * loopDepthSpan) ? "Interior cut is SIGNIFICANTLY DEEPER than boundary rim!" : "Relatively planar cut");
        }

        // Build 2D basis (U, V) perpendicular to normal
        glm::vec3 up = (std::abs(normal.y) < 0.9f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        glm::vec3 U = glm::normalize(glm::cross(up, normal));
        glm::vec3 V = glm::cross(normal, U);

        // Project loop into 2D and record 3D height offsets relative to the plane through loopCenter
        std::vector<glm::vec2> poly2D(K);
        std::vector<float> hList(K);
        float minU = 1e9f, minV = 1e9f, maxU = -1e9f, maxV = -1e9f;
        for (size_t i = 0; i < K; ++i) {
            uint32_t vi = loop[i];
            glm::vec3 p(mesh.verts[vi * 3], mesh.verts[vi * 3 + 1], mesh.verts[vi * 3 + 2]);
            glm::vec3 rel = p - loopCenter;
            poly2D[i] = glm::vec2(glm::dot(rel, U), glm::dot(rel, V));
            hList[i] = glm::dot(rel, normal);
            minU = std::min(minU, poly2D[i].x);
            minV = std::min(minV, poly2D[i].y);
            maxU = std::max(maxU, poly2D[i].x);
            maxV = std::max(maxV, poly2D[i].y);
        }

        // Ensure CCW 2D polygon winding without reversing the 3D boundary vertex sequence
        float signedArea = 0.0f;
        for (size_t i = 0; i < K; ++i) {
            size_t j = (i + 1) % K;
            signedArea += (poly2D[i].x * poly2D[j].y - poly2D[j].x * poly2D[i].y);
        }
        if (signedArea < 0.0f) {
            V = -V;
            normal = -normal;
            minV = 1e9f;
            maxV = -1e9f;
            for (size_t i = 0; i < K; ++i) {
                poly2D[i].y = -poly2D[i].y;
                hList[i] = -hList[i];
                minV = std::min(minV, poly2D[i].y);
                maxV = std::max(maxV, poly2D[i].y);
            }
        }

        // Precompute 2D AABB of each original cap triangle in (U, V) basis
        for (auto& tri : origCapTris) {
            glm::vec2 u0(glm::dot(tri.p0 - loopCenter, U), glm::dot(tri.p0 - loopCenter, V));
            glm::vec2 u1(glm::dot(tri.p1 - loopCenter, U), glm::dot(tri.p1 - loopCenter, V));
            glm::vec2 u2(glm::dot(tri.p2 - loopCenter, U), glm::dot(tri.p2 - loopCenter, V));
            tri.minU = std::min({u0.x, u1.x, u2.x}) - 1e-3f;
            tri.maxU = std::max({u0.x, u1.x, u2.x}) + 1e-3f;
            tri.minV = std::min({u0.y, u1.y, u2.y}) - 1e-3f;
            tri.maxV = std::max({u0.y, u1.y, u2.y}) + 1e-3f;
        }

        // Use targetEdgeLen from mesh
        float step = targetEdgeLen;
        if (step <= 1e-4f) {
            step = totalEdgeLen / static_cast<float>(K);
        }
        step = std::clamp(step, 0.5f, 50.0f);
        float margin = step * 0.50f;

        struct GridPt {
            int gx, gy;
            uint32_t meshVid;
            glm::vec2 p2D;
        };

        std::map<std::pair<int, int>, uint32_t> gridPtMap;
        std::vector<GridPt> gridPoints;

        int gxMin = static_cast<int>(std::floor(minU / step));
        int gxMax = static_cast<int>(std::ceil(maxU / step));
        int gyMin = static_cast<int>(std::floor(minV / step));
        int gyMax = static_cast<int>(std::ceil(maxV / step));

        int rayHitsCount = 0;
        int fallbackCount = 0;
        float raySpan = std::max(200.0f, capDepthSpan * 3.0f);

        for (int gy = gyMin; gy <= gyMax; ++gy) {
            for (int gx = gxMin; gx <= gxMax; ++gx) {
                glm::vec2 pt(gx * step, gy * step);
                if (pointInPoly2D(pt, poly2D) && distToPolyBoundary(pt, poly2D) >= margin) {
                    // Initial IDW reference height from boundary as topological sheet hint
                    float sumW = 0.0f;
                    float sumH = 0.0f;
                    for (size_t i = 0; i < K; ++i) {
                        float d2 = glm::dot(pt - poly2D[i], pt - poly2D[i]);
                        if (d2 < 1e-4f) {
                            sumH = hList[i];
                            sumW = 1.0f;
                            break;
                        }
                        float w = 1.0f / (d2 * d2);
                        sumW += w;
                        sumH += w * hList[i];
                    }
                    float hVal = (sumW > 1e-8f) ? (sumH / sumW) : 0.0f;
                    glm::vec3 pPlane = loopCenter + pt.x * U + pt.y * V;

                    // Project onto exact 3D cut surface via ray intersection along normal
                    float bestDiff = 1e9f;
                    glm::vec3 p3D = pPlane + hVal * normal;
                    glm::vec3 n3D = normal;
                    bool foundHit = false;

                    for (const auto& tri : origCapTris) {
                        // Quick 2D AABB rejection
                        if (pt.x < tri.minU || pt.x > tri.maxU || pt.y < tri.minV || pt.y > tri.maxV) {
                            continue;
                        }
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
                        // Fallback to closest point if ray missed
                        fallbackCount++;
                        float bestD2 = 1e18f;
                        glm::vec3 pGuide = pPlane + hVal * normal;
                        for (const auto& tri : origCapTris) {
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

                    uint32_t newVid = static_cast<uint32_t>(mesh.verts.size() / 3);
                    mesh.verts.push_back(p3D.x);
                    mesh.verts.push_back(p3D.y);
                    mesh.verts.push_back(p3D.z);

                    mesh.normals.push_back(n3D.x);
                    mesh.normals.push_back(n3D.y);
                    mesh.normals.push_back(n3D.z);

                    mesh.colors.push_back(1.0f);
                    mesh.colors.push_back(1.0f);
                    mesh.colors.push_back(1.0f);

                    mesh.materials.push_back(0.5f);
                    mesh.materials.push_back(0.0f);
                    mesh.materials.push_back(1.0f);

                    gridPtMap[{gx, gy}] = newVid;
                    gridPoints.push_back({gx, gy, newVid, pt});
                }
            }
        }

        // Curvature & deviation analysis for generated grid points vs original cut surface
        float minDistToOrig = 1e9f, maxDistToOrig = -1e9f, sumDistToOrig = 0.0f;
        int deviatingCount = 0;

        for (const auto& gp : gridPoints) {
            uint32_t vid = gp.meshVid;
            glm::vec3 pGrid(mesh.verts[vid * 3], mesh.verts[vid * 3 + 1], mesh.verts[vid * 3 + 2]);

            float closestDist2 = 1e18f;
            for (const auto& tri : origCapTris) {
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
        }

        float avgDistToOrig = gridPoints.empty() ? 0.0f : (sumDistToOrig / (float)gridPoints.size());

        sculpt_log("[QUAD CAP CURVATURE] Grid points: %zu (ray-projected: %d, closest-pt fallback: %d)\n",
                   gridPoints.size(), rayHitsCount, fallbackCount);
        sculpt_log("[QUAD CAP CURVATURE] Post-projection 3D surface deviation: min=%.3f, max=%.3f, avg=%.3f mm (step: %.3f)\n",
                   minDistToOrig, maxDistToOrig, avgDistToOrig, step);
        sculpt_log("[QUAD CAP CURVATURE] Points deviating > 0.5*step: %d / %zu (%.1f%%)\n",
                   deviatingCount, gridPoints.size(), gridPoints.empty() ? 0.0f : ((float)deviatingCount * 100.0f / (float)gridPoints.size()));

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

        // CDT Triangulation for perimeter stitching
        CDT::Triangulation<float> cdt;
        std::vector<CDT::V2d<float>> cdtVerts;
        cdtVerts.reserve(K + gridPoints.size());

        std::vector<uint32_t> cdtToMeshVid;
        cdtToMeshVid.reserve(K + gridPoints.size());

        // 1. Boundary loop vertices
        for (size_t i = 0; i < K; ++i) {
            cdtVerts.push_back(CDT::V2d<float>::make(poly2D[i].x, poly2D[i].y));
            cdtToMeshVid.push_back(loop[i]);
        }

        // 2. Grid vertices
        for (const auto& gp : gridPoints) {
            cdtVerts.push_back(CDT::V2d<float>::make(gp.p2D.x, gp.p2D.y));
            cdtToMeshVid.push_back(gp.meshVid);
        }

        cdt.insertVertices(cdtVerts);

        // Constrain boundary loop edges
        std::vector<CDT::Edge> cdtEdges;
        cdtEdges.reserve(K);
        for (size_t i = 0; i < K; ++i) {
            cdtEdges.push_back(CDT::Edge(static_cast<CDT::VertInd>(i), static_cast<CDT::VertInd>((i + 1) % K)));
        }
        cdt.insertEdges(cdtEdges);
        cdt.eraseOuterTrianglesAndHoles();

        sculpt_log("[QUAD CAP DEBUG] Loop %zu verts, AABB: [%.2f, %.2f] to [%.2f, %.2f], step: %.3f, gridPts: %zu, quads: %zu, CDT tris: %zu\n",
                   K, minU, minV, maxU, maxV, step, gridPoints.size(), quads.size(), cdt.triangles.size());

        // Classify CDT triangles:
        // Group triangles belonging to each candidate quad cell
        std::unordered_map<uint64_t, std::vector<std::array<uint32_t, 3>>> cellTriangles;
        std::vector<std::array<uint32_t, 3>> transitionTris;

        for (const auto& tri : cdt.triangles) {
            uint32_t m0 = cdtToMeshVid[tri.vertices[0]];
            uint32_t m1 = cdtToMeshVid[tri.vertices[1]];
            uint32_t m2 = cdtToMeshVid[tri.vertices[2]];

            uint64_t matchedCellKey = 0;
            bool isQuadTri = false;
            for (const auto& q : quads) {
                int matches = (m0 == q.v00 || m0 == q.v10 || m0 == q.v11 || m0 == q.v01) +
                              (m1 == q.v00 || m1 == q.v10 || m1 == q.v11 || m1 == q.v01) +
                              (m2 == q.v00 || m2 == q.v10 || m2 == q.v11 || m2 == q.v01);
                if (matches == 3) {
                    matchedCellKey = makeCellKey(q.gx, q.gy);
                    isQuadTri = true;
                    break;
                }
            }

            if (isQuadTri) {
                cellTriangles[matchedCellKey].push_back({m0, m1, m2});
            } else {
                transitionTris.push_back({m0, m1, m2});
            }
        }

        // Add all transition triangles
        for (const auto& t : transitionTris) {
            newFaces.push_back(t[0]);
            newFaces.push_back(t[1]);
            newFaces.push_back(t[2]);
            newFaces.push_back(TRI_INDEX);
            newFaceGroups.push_back(capGroupID);
            totalTrisGenerated++;
        }

        // For each candidate quad: if CDT produced exactly 2 triangles inside it, emit the Quad!
        // Otherwise, emit the triangles so that watertightness is 100% strictly guaranteed!
        for (const auto& q : quads) {
            uint64_t key = makeCellKey(q.gx, q.gy);
            auto it = cellTriangles.find(key);
            if (it != cellTriangles.end() && it->second.size() == 2) {
                newFaces.push_back(q.v00);
                newFaces.push_back(q.v10);
                newFaces.push_back(q.v11);
                newFaces.push_back(q.v01);
                newFaceGroups.push_back(capGroupID);
                totalQuadsGenerated++;
            } else if (it != cellTriangles.end()) {
                for (const auto& t : it->second) {
                    newFaces.push_back(t[0]);
                    newFaces.push_back(t[1]);
                    newFaces.push_back(t[2]);
                    newFaces.push_back(TRI_INDEX);
                    newFaceGroups.push_back(capGroupID);
                    totalTrisGenerated++;
                }
            }
        }
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

    mesh.initTopology();
    mesh.postInit();

    mesh.isDirty = true;
    mesh.isTopologyDirty = true;
    mesh.isVertexDirty = true;
    mesh.isColorDirty = true;
    mesh.isFaceGroupDirty = true;
    mesh.invalidateLocalRadius();

    auto tEnd = std::chrono::high_resolution_clock::now();
    result.executionTimeMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    result.numQuads = totalQuadsGenerated;
    result.numTris = totalTrisGenerated;
    int totalNewFaces = totalQuadsGenerated + totalTrisGenerated;
    result.quadRatio = totalNewFaces > 0 ? (float)totalQuadsGenerated / (float)totalNewFaces : 0.0f;
    result.success = true;

    sculpt_log("[QUAD CAP] Cap generated: %d Quads (%.1f%%), %d Tris (%.1f%%) in %.2f ms\n",
               result.numQuads, result.quadRatio * 100.0f,
               result.numTris, (1.0f - result.quadRatio) * 100.0f,
               result.executionTimeMs);

    return result;
}

} // namespace sculpt
