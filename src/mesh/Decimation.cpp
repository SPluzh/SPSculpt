#include "mesh/Decimation.h"
#include "mesh/Mesh.h"
#include "mesh/Topology.h"
#include "common/Constants.h"
#include "common/Logger.h"

#include <cmath>
#include <chrono>
#include <queue>
#include <unordered_set>
#include <algorithm>
#include <iostream>

// ------------------------------------------------------------------------------------------------
// Quadric Member Functions
// ------------------------------------------------------------------------------------------------

bool Quadric::optimalPosition(glm::dvec3& outPos) const {
    double det = a2 * (b2 * c2 - bc * bc) -
                 ab * (ab * c2 - bc * ac) +
                 ac * (ab * bc - b2 * ac);

    double tr = a2 + b2 + c2;
    if (tr < 1e-15) {
        return false;
    }
    double tr3 = (tr / 3.0);
    double cond = std::abs(det) / (tr3 * tr3 * tr3);
    if (cond < 1e-4) {
        return false; // Singular or ill-conditioned (plane, cylinder, etc.)
    }

    double invDet = 1.0 / det;

    double c00 = b2 * c2 - bc * bc;
    double c01 = ac * bc - ab * c2;
    double c02 = ab * bc - ac * b2;

    double c11 = a2 * c2 - ac * ac;
    double c12 = ab * ac - a2 * bc;

    double c22 = a2 * b2 - ab * ab;

    outPos.x = -(c00 * ad + c01 * bd + c02 * cd) * invDet;
    outPos.y = -(c01 * ad + c11 * bd + c12 * cd) * invDet;
    outPos.z = -(c02 * ad + c12 * bd + c22 * cd) * invDet;

    return true;
}

// ------------------------------------------------------------------------------------------------
// Internal Data Structures for Mesh Decimation
// ------------------------------------------------------------------------------------------------

namespace {

struct InternalVertex {
    glm::dvec3 pos{0.0};
    glm::vec3 color{1.0f};
    glm::vec3 material{0.5f, 0.0f, 1.0f};
    Quadric quadric;
    std::vector<uint32_t> incidentFaces;
    std::vector<uint32_t> incidentEdges;
    uint8_t onBoundary = 0;
    bool onSymmetryPlane = false;
    bool alive = true;
};

struct InternalTriangle {
    uint32_t v[3] = {0, 0, 0};
    uint32_t groupID = 0;
    glm::dvec3 normal{0.0, 1.0, 0.0};
    double area = 0.0;
    bool alive = true;

    bool hasVertex(uint32_t vid) const {
        return v[0] == vid || v[1] == vid || v[2] == vid;
    }

    void replaceVertex(uint32_t oldV, uint32_t newV) {
        if (v[0] == oldV) v[0] = newV;
        if (v[1] == oldV) v[1] = newV;
        if (v[2] == oldV) v[2] = newV;
    }

    uint32_t getOppositeVertex(uint32_t v0, uint32_t v1) const {
        for (int i = 0; i < 3; ++i) {
            if (v[i] != v0 && v[i] != v1) return v[i];
        }
        return 0xFFFFFFFF;
    }
};

struct InternalEdge {
    uint32_t v0 = 0;
    uint32_t v1 = 0;
    glm::dvec3 optimalPos{0.0};
    double cost = 0.0;
    uint32_t version = 0;
    bool isBoundary = false;
    bool alive = true;
};

struct HeapEntry {
    uint32_t edgeIdx;
    uint32_t version;
    double cost;

    bool operator>(const HeapEntry& o) const {
        return cost > o.cost;
    }
};

static inline double triangleAreaAndNormal(const glm::dvec3& p0, const glm::dvec3& p1, const glm::dvec3& p2, glm::dvec3& outNormal) {
    glm::dvec3 cross = glm::cross(p1 - p0, p2 - p0);
    double len = glm::length(cross);
    if (len > 1e-14) {
        outNormal = cross / len;
    } else {
        outNormal = glm::dvec3(0.0, 1.0, 0.0);
    }
    return 0.5 * len;
}

static inline double computeTriangleQuality(const glm::dvec3& p0, const glm::dvec3& p1, const glm::dvec3& p2, double area) {
    glm::dvec3 d0 = p1 - p0;
    glm::dvec3 d1 = p2 - p1;
    glm::dvec3 d2 = p0 - p2;
    double l0 = glm::dot(d0, d0);
    double l1 = glm::dot(d1, d1);
    double l2 = glm::dot(d2, d2);
    double sumL2 = l0 + l1 + l2;
    if (sumL2 < 1e-14) return 0.0;
    // Normalized aspect ratio: 4 * sqrt(3) * Area / (l0 + l1 + l2). Returns 1.0 for equilateral.
    return (6.928203230275509 * area) / sumL2;
}

} // anonymous namespace

// ------------------------------------------------------------------------------------------------
// MeshDecimator Implementation
// ------------------------------------------------------------------------------------------------

DecimationResult MeshDecimator::decimate(Mesh& mesh, const DecimationOptions& opts) {
    auto tStart = std::chrono::high_resolution_clock::now();

    DecimationResult result;
    result.originalVerts = mesh.nbVerts;
    result.originalFaces = mesh.nbFaces;

    if (mesh.nbVerts < 4 || mesh.nbFaces < 2) {
        result.success = false;
        return result;
    }

    // Determine target face count
    int targetFaces = opts.targetFaces;
    if (targetFaces < 0) {
        float ratio = std::clamp(opts.targetRatio, 0.01f, 0.99f);
        targetFaces = std::max(4, static_cast<int>(mesh.nbFaces * ratio));
    }

    sculpt_log("[Decimation] Starting decimation: originalVerts=%d, originalFaces=%d, targetFaces=%d, volumeWeight=%.2f\n",
               mesh.nbVerts, mesh.nbFaces, targetFaces, opts.volumeWeight);

    // 1. Unpack mesh into internal vertex and triangle arrays
    std::vector<InternalVertex> vertices(mesh.nbVerts);
    for (int i = 0; i < mesh.nbVerts; ++i) {
        vertices[i].pos = glm::dvec3(mesh.verts[i * 3 + 0], mesh.verts[i * 3 + 1], mesh.verts[i * 3 + 2]);
        if (mesh.colors.size() == (size_t)mesh.nbVerts * 3) {
            vertices[i].color = glm::vec3(mesh.colors[i * 3 + 0], mesh.colors[i * 3 + 1], mesh.colors[i * 3 + 2]);
        }
        if (mesh.materials.size() == (size_t)mesh.nbVerts * 3) {
            vertices[i].material = glm::vec3(mesh.materials[i * 3 + 0], mesh.materials[i * 3 + 1], mesh.materials[i * 3 + 2]);
        }
        if (opts.lockSymmetryPlane && mesh.symEnabled && mesh.symX) {
            if (std::abs(vertices[i].pos.x) < 1e-4) {
                vertices[i].onSymmetryPlane = true;
                vertices[i].pos.x = 0.0;
            }
        }
    }

    std::vector<InternalTriangle> triangles;
    triangles.reserve(mesh.nbFaces * 2);

    for (int f = 0; f < mesh.nbFaces; ++f) {
        uint32_t v0 = mesh.faces[f * 4 + 0];
        uint32_t v1 = mesh.faces[f * 4 + 1];
        uint32_t v2 = mesh.faces[f * 4 + 2];
        uint32_t v3 = mesh.faces[f * 4 + 3];
        uint32_t gid = (f < (int)mesh.faceGroups.size()) ? mesh.faceGroups[f] : 0;

        // Triangle 1: (v0, v1, v2)
        InternalTriangle t1;
        t1.v[0] = v0; t1.v[1] = v1; t1.v[2] = v2;
        t1.groupID = gid;
        t1.area = triangleAreaAndNormal(vertices[v0].pos, vertices[v1].pos, vertices[v2].pos, t1.normal);
        triangles.push_back(t1);

        // Quad -> Triangle 2: (v0, v2, v3)
        if (v3 != TRI_INDEX) {
            InternalTriangle t2;
            t2.v[0] = v0; t2.v[1] = v2; t2.v[2] = v3;
            t2.groupID = gid;
            t2.area = triangleAreaAndNormal(vertices[v0].pos, vertices[v2].pos, vertices[v3].pos, t2.normal);
            triangles.push_back(t2);
        }
    }

    // Connect vertices to incident triangles
    for (size_t i = 0; i < triangles.size(); ++i) {
        const auto& tri = triangles[i];
        vertices[tri.v[0]].incidentFaces.push_back(static_cast<uint32_t>(i));
        vertices[tri.v[1]].incidentFaces.push_back(static_cast<uint32_t>(i));
        vertices[tri.v[2]].incidentFaces.push_back(static_cast<uint32_t>(i));
    }

    // 2. Compute signed volume of initial mesh
    double initialSignedVolume = 0.0;
    for (const auto& tri : triangles) {
        initialSignedVolume += glm::dot(vertices[tri.v[0]].pos, glm::cross(vertices[tri.v[1]].pos, vertices[tri.v[2]].pos));
    }
    initialSignedVolume /= 6.0;

    // 3. Build Quadrics for each vertex
    for (const auto& tri : triangles) {
        if (tri.area < 1e-12) continue;
        Quadric q = Quadric::fromPlane(tri.normal, vertices[tri.v[0]].pos, tri.area);
        vertices[tri.v[0]].quadric += q;
        vertices[tri.v[1]].quadric += q;
        vertices[tri.v[2]].quadric += q;
    }

    // 4. Build edge graph and identify boundary edges
    struct EdgeKey {
        uint32_t u, v;
        bool operator==(const EdgeKey& o) const { return u == o.u && v == o.v; }
    };
    struct EdgeKeyHash {
        size_t operator()(const EdgeKey& k) const {
            return (static_cast<size_t>(k.u) * 73856093u) ^ (static_cast<size_t>(k.v) * 19349663u);
        }
    };

    struct EdgeOccurrence {
        uint32_t triIdx;
        uint32_t oppV;
    };

    std::unordered_map<EdgeKey, std::vector<EdgeOccurrence>, EdgeKeyHash> edgeOccurrences;
    edgeOccurrences.reserve(triangles.size() * 3);

    for (size_t t = 0; t < triangles.size(); ++t) {
        const auto& tri = triangles[t];
        for (int i = 0; i < 3; ++i) {
            uint32_t u = tri.v[i];
            uint32_t v = tri.v[(i + 1) % 3];
            uint32_t opp = tri.v[(i + 2) % 3];
            EdgeKey k = (u < v) ? EdgeKey{u, v} : EdgeKey{v, u};
            edgeOccurrences[k].push_back({static_cast<uint32_t>(t), opp});
        }
    }

    std::vector<InternalEdge> edges;
    edges.reserve(edgeOccurrences.size());

    for (const auto& kv : edgeOccurrences) {
        InternalEdge e;
        e.v0 = kv.first.u;
        e.v1 = kv.first.v;
        e.isBoundary = (kv.second.size() == 1);

        if (e.isBoundary) {
            vertices[e.v0].onBoundary = 1;
            vertices[e.v1].onBoundary = 1;

            if (opts.preserveBoundary) {
                // Add boundary plane quadric perpendicular to triangle
                uint32_t tIdx = kv.second[0].triIdx;
                glm::dvec3 triN = triangles[tIdx].normal;
                glm::dvec3 edgeDir = glm::normalize(vertices[e.v1].pos - vertices[e.v0].pos);
                glm::dvec3 boundaryN = glm::normalize(glm::cross(triN, edgeDir));

                double edgeLen = glm::length(vertices[e.v1].pos - vertices[e.v0].pos);
                Quadric bQ = Quadric::fromPlane(boundaryN, vertices[e.v0].pos, edgeLen * opts.boundaryWeight);
                vertices[e.v0].quadric += bQ;
                vertices[e.v1].quadric += bQ;
            }
        }

        uint32_t edgeIdx = static_cast<uint32_t>(edges.size());
        vertices[e.v0].incidentEdges.push_back(edgeIdx);
        vertices[e.v1].incidentEdges.push_back(edgeIdx);
        edges.push_back(e);
    }

    // Helper: evaluate collapse cost and optimal position for edge
    auto computeEdgeCost = [&](InternalEdge& e) -> bool {
        const auto& v0 = vertices[e.v0];
        const auto& v1 = vertices[e.v1];

        if (!v0.alive || !v1.alive) {
            e.cost = 1e30;
            return false;
        }

        // Feature lock 1: PolyGroup preservation
        if (opts.preservePolyGroups) {
            // Find distinct PolyGroups around v0 and v1
            uint32_t g0 = 0xFFFFFFFF, g1 = 0xFFFFFFFF;
            bool multi0 = false, multi1 = false;
            for (uint32_t f : v0.incidentFaces) {
                if (!triangles[f].alive) continue;
                if (g0 == 0xFFFFFFFF) g0 = triangles[f].groupID;
                else if (g0 != triangles[f].groupID) multi0 = true;
            }
            for (uint32_t f : v1.incidentFaces) {
                if (!triangles[f].alive) continue;
                if (g1 == 0xFFFFFFFF) g1 = triangles[f].groupID;
                else if (g1 != triangles[f].groupID) multi1 = true;
            }
            // If vertices belong to different groups or both are border vertices of different regions, skip
            if (g0 != g1 || (multi0 && multi1)) {
                e.cost = 1e30;
                return false;
            }
        }

        // Feature lock 2: Boundary preservation
        if (opts.preserveBoundary) {
            // Interior edge cannot collapse into a boundary vertex
            if ((v0.onBoundary && !v1.onBoundary) || (!v0.onBoundary && v1.onBoundary)) {
                // Moving interior to boundary or vice versa could distort boundary
                // We permit collapse ONLY if v stays strictly on boundary vertex
            }
        }

        Quadric qSum = v0.quadric + v1.quadric;
        glm::dvec3 optPos;

        // Candidates for optimal position:
        // 1. Analytical minimum of QEM
        // 2. Midpoint
        // 3. v0
        // 4. v1
        glm::dvec3 candMid = 0.5 * (v0.pos + v1.pos);
        glm::dvec3 candV0 = v0.pos;
        glm::dvec3 candV1 = v1.pos;

        bool solved = qSum.optimalPosition(optPos);
        double edgeLen = glm::distance(v0.pos, v1.pos);

        // Sanity check for analytical minimum: don't drift too far from edge
        if (!solved || glm::distance(optPos, candMid) > 2.0 * edgeLen) {
            optPos = candMid;
            double bestErr = qSum.evaluate(optPos);

            double err0 = qSum.evaluate(candV0);
            if (err0 < bestErr) { bestErr = err0; optPos = candV0; }

            double err1 = qSum.evaluate(candV1);
            if (err1 < bestErr) { bestErr = err1; optPos = candV1; }
        }

        // Symmetry constraint
        if (v0.onSymmetryPlane && v1.onSymmetryPlane) {
            optPos.x = 0.0;
        } else if (v0.onSymmetryPlane) {
            optPos = v0.pos; // Must snap to symmetry plane
        } else if (v1.onSymmetryPlane) {
            optPos = v1.pos; // Must snap to symmetry plane
        }

        // Boundary constraint: if edge is boundary, keep pos along the edge segment
        if (opts.preserveBoundary && v0.onBoundary && v1.onBoundary) {
            // Keep optPos along v0..v1 line
            glm::dvec3 dir = v1.pos - v0.pos;
            double l2 = glm::dot(dir, dir);
            if (l2 > 1e-12) {
                double t = glm::clamp(glm::dot(optPos - v0.pos, dir) / l2, 0.0, 1.0);
                optPos = v0.pos + t * dir;
            }
        }

        e.optimalPos = optPos;
        double qemError = std::max(0.0, qSum.evaluate(optPos));

        // -----------------------------------------------------------------------------------------
        // Link Condition Check: ensure 2-manifold topology is preserved
        // -----------------------------------------------------------------------------------------
        // Count shared neighbors between v0 and v1
        std::unordered_set<uint32_t> n0;
        for (uint32_t fIdx : v0.incidentFaces) {
            const auto& t = triangles[fIdx];
            if (!t.alive) continue;
            for (int i = 0; i < 3; ++i) {
                if (t.v[i] != e.v0 && t.v[i] != e.v1) n0.insert(t.v[i]);
            }
        }

        int sharedCount = 0;
        int edgeTriangleCount = 0;
        for (uint32_t fIdx : v1.incidentFaces) {
            const auto& t = triangles[fIdx];
            if (!t.alive) continue;
            if (t.hasVertex(e.v0)) {
                edgeTriangleCount++;
            }
            for (int i = 0; i < 3; ++i) {
                if (t.v[i] != e.v0 && t.v[i] != e.v1) {
                    if (n0.count(t.v[i])) {
                        sharedCount++;
                        n0.erase(t.v[i]); // Avoid duplicate count
                    }
                }
            }
        }

        // For manifold surfaces:
        // Interior edge: exactly 2 incident triangles, exactly 2 shared neighbors.
        // Boundary edge: exactly 1 incident triangle, exactly 1 shared neighbor.
        if (edgeTriangleCount == 1) {
            if (sharedCount != 1) {
                e.cost = 1e30;
                return false;
            }
        } else if (edgeTriangleCount == 2) {
            if (v0.onBoundary && v1.onBoundary && !e.isBoundary) {
                // Interior edge connecting two boundary vertices would pinch boundary loop
                e.cost = 1e30;
                return false;
            }
            if (sharedCount != 2) {
                e.cost = 1e30;
                return false;
            }
        } else {
            // Non-manifold edge
            e.cost = 1e30;
            return false;
        }

        // -----------------------------------------------------------------------------------------
        // Face Inversion / Normal Flip, Dihedral Curvature, and Relative Volume Delta
        // -----------------------------------------------------------------------------------------
        std::vector<uint32_t> uniqueFaces;
        uniqueFaces.reserve(v0.incidentFaces.size() + v1.incidentFaces.size());
        for (uint32_t f : v0.incidentFaces) {
            if (triangles[f].alive) uniqueFaces.push_back(f);
        }
        for (uint32_t f : v1.incidentFaces) {
            if (!triangles[f].alive) continue;
            if (std::find(uniqueFaces.begin(), uniqueFaces.end(), f) == uniqueFaces.end()) {
                uniqueFaces.push_back(f);
            }
        }

        double localArea = 0.0;
        double volumeChange = 0.0;
        double shapePenalty = 0.0;
        uint32_t sharedTriA = 0xFFFFFFFF, sharedTriB = 0xFFFFFFFF;

        for (uint32_t fIdx : uniqueFaces) {
            const auto& tri = triangles[fIdx];
            localArea += tri.area;

            // Volume of tetrahedron from optPos to old triangle (signed volume relative to optPos)
            glm::dvec3 a = vertices[tri.v[0]].pos - optPos;
            glm::dvec3 b = vertices[tri.v[1]].pos - optPos;
            glm::dvec3 c = vertices[tri.v[2]].pos - optPos;
            double tetVol = glm::dot(a, glm::cross(b, c)) / 6.0;
            volumeChange -= tetVol;

            bool has0 = tri.hasVertex(e.v0);
            bool has1 = tri.hasVertex(e.v1);

            if (has0 && has1) {
                // Shared triangle that will collapse
                if (sharedTriA == 0xFFFFFFFF) sharedTriA = fIdx;
                else if (sharedTriB == 0xFFFFFFFF) sharedTriB = fIdx;
                continue;
            }

            // Survived triangle with modified vertex
            glm::dvec3 p0 = (tri.v[0] == e.v0 || tri.v[0] == e.v1) ? optPos : vertices[tri.v[0]].pos;
            glm::dvec3 p1 = (tri.v[1] == e.v0 || tri.v[1] == e.v1) ? optPos : vertices[tri.v[1]].pos;
            glm::dvec3 p2 = (tri.v[2] == e.v0 || tri.v[2] == e.v1) ? optPos : vertices[tri.v[2]].pos;

            glm::dvec3 newN;
            double newArea = triangleAreaAndNormal(p0, p1, p2, newN);

            if (newArea < 1e-12) {
                e.cost = 1e30;
                return false; // Degenerate triangle
            }

            // Normal flip test
            double dotN = glm::dot(newN, tri.normal);
            if (dotN < opts.maxNormalFlip) {
                e.cost = 1e30;
                return false; // Flipped triangle!
            }

            // Triangle shape quality check
            double quality = computeTriangleQuality(p0, p1, p2, newArea);
            if (quality < 0.08) {
                shapePenalty += (0.08 - quality) * opts.shapeWeight * (edgeLen * edgeLen);
            }
        }

        // Dihedral Curvature Penalty: protects curved surfaces and sharp ridges!
        double curvaturePenalty = 0.0;
        if (sharedTriA != 0xFFFFFFFF && sharedTriB != 0xFFFFFFFF) {
            double dotDihedral = glm::clamp(glm::dot(triangles[sharedTriA].normal, triangles[sharedTriB].normal), -1.0, 1.0);
            // 0 on flat plane (dot == 1.0), up to 2.0 on sharp 180-deg fold
            double angleDiff = std::max(0.0, 1.0 - dotDihedral);
            curvaturePenalty = angleDiff * (edgeLen * edgeLen);
        } else if (e.isBoundary) {
            // Boundary edge penalty
            curvaturePenalty = opts.boundaryWeight * (edgeLen * edgeLen);
        }

        // Volume Penalty: physically scaled by local surface area, invariant to translation!
        double volumePenalty = 0.0;
        if (opts.volumeWeight > 0.0f && localArea > 1e-12) {
            double deltaHeight = std::abs(volumeChange) / localArea;
            volumePenalty = opts.volumeWeight * (deltaHeight * deltaHeight);
        }

        // Tie-breaker: ensures short edges on flat planes collapse first
        double tieBreaker = 1e-8 * (edgeLen * edgeLen);

        e.cost = qemError + volumePenalty + curvaturePenalty + shapePenalty + tieBreaker;
        return true;
    };

    // 5. Initialize priority queue with all edges
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, std::greater<HeapEntry>> heap;

    for (size_t i = 0; i < edges.size(); ++i) {
        computeEdgeCost(edges[i]);
        if (edges[i].cost < opts.maxError) {
            heap.push({static_cast<uint32_t>(i), edges[i].version, edges[i].cost});
        }
    }

    sculpt_log("[Decimation] Initialized %zu edges in priority queue. Beginning collapse loop...\n", edges.size());

    // 6. Main decimation collapse loop
    int activeFaces = static_cast<int>(triangles.size());
    int totalCollapses = 0;
    int lastReportedPct = -1;

    int facesToRemove = activeFaces - targetFaces;

    while (activeFaces > targetFaces && !heap.empty()) {
        HeapEntry top = heap.top();
        heap.pop();

        uint32_t edgeIdx = top.edgeIdx;
        auto& edge = edges[edgeIdx];

        // Skip invalidated / outdated heap entries
        if (!edge.alive || edge.version != top.version) continue;
        if (top.cost >= opts.maxError) break;

        // Re-evaluate cost to guarantee valid topology and state before commit
        if (!computeEdgeCost(edge) || edge.cost >= opts.maxError) {
            continue;
        }

        uint32_t vKeep = edge.v0;
        uint32_t vRemove = edge.v1;
        glm::dvec3 newPos = edge.optimalPos;

        // Determine color & material interpolation
        double dist0 = glm::distance(newPos, vertices[vKeep].pos);
        double dist1 = glm::distance(newPos, vertices[vRemove].pos);
        double tInterp = (dist0 + dist1 > 1e-12) ? (dist0 / (dist0 + dist1)) : 0.5;

        glm::vec3 newColor = glm::mix(vertices[vKeep].color, vertices[vRemove].color, static_cast<float>(tInterp));
        glm::vec3 newMaterial = glm::mix(vertices[vKeep].material, vertices[vRemove].material, static_cast<float>(tInterp));

        // Update kept vertex
        vertices[vKeep].pos = newPos;
        vertices[vKeep].color = newColor;
        vertices[vKeep].material = newMaterial;
        vertices[vKeep].quadric += vertices[vRemove].quadric;
        vertices[vKeep].onBoundary = (vertices[vKeep].onBoundary || vertices[vRemove].onBoundary);
        vertices[vKeep].onSymmetryPlane = (vertices[vKeep].onSymmetryPlane || vertices[vRemove].onSymmetryPlane);

        // Update triangles around vRemove: redirect to vKeep, or collapse degenerate triangles
        for (uint32_t fIdx : vertices[vRemove].incidentFaces) {
            auto& tri = triangles[fIdx];
            if (!tri.alive) continue;

            if (tri.hasVertex(vKeep)) {
                // Degenerate triangle! Collapse it
                tri.alive = false;
                activeFaces--;
            } else {
                tri.replaceVertex(vRemove, vKeep);
                // Recompute normal & area
                tri.area = triangleAreaAndNormal(vertices[tri.v[0]].pos, vertices[tri.v[1]].pos, vertices[tri.v[2]].pos, tri.normal);
                vertices[vKeep].incidentFaces.push_back(fIdx);
            }
        }

        // Clean up vKeep incident faces: mark collapsed ones as removed
        for (uint32_t fIdx : vertices[vKeep].incidentFaces) {
            auto& tri = triangles[fIdx];
            if (!tri.alive) continue;
            if (tri.hasVertex(vRemove)) {
                tri.alive = false;
                activeFaces--;
            } else {
                tri.area = triangleAreaAndNormal(vertices[tri.v[0]].pos, vertices[tri.v[1]].pos, vertices[tri.v[2]].pos, tri.normal);
            }
        }

        // Mark removed vertex as dead
        vertices[vRemove].alive = false;
        edge.alive = false;
        totalCollapses++;

        // Reconnect all edges of vRemove to vKeep
        for (uint32_t eIdx : vertices[vRemove].incidentEdges) {
            if (!edges[eIdx].alive) continue;
            auto& e = edges[eIdx];
            if (e.v0 == vRemove) e.v0 = vKeep;
            if (e.v1 == vRemove) e.v1 = vKeep;

            if (e.v0 == e.v1) {
                e.alive = false;
            } else {
                if (e.v0 > e.v1) std::swap(e.v0, e.v1);
                vertices[vKeep].incidentEdges.push_back(eIdx);
            }
        }

        // Deduplicate edges incident to vKeep
        std::unordered_map<uint32_t, uint32_t> neighborToEdge;
        std::vector<uint32_t> uniqueKeepEdges;
        uniqueKeepEdges.reserve(vertices[vKeep].incidentEdges.size());
        std::unordered_set<uint32_t> edgesToUpdate;

        for (uint32_t eIdx : vertices[vKeep].incidentEdges) {
            if (!edges[eIdx].alive) continue;
            uint32_t otherV = (edges[eIdx].v0 == vKeep) ? edges[eIdx].v1 : edges[eIdx].v0;
            if (otherV == vKeep) {
                edges[eIdx].alive = false;
                continue;
            }
            auto it = neighborToEdge.find(otherV);
            if (it == neighborToEdge.end()) {
                neighborToEdge[otherV] = eIdx;
                uniqueKeepEdges.push_back(eIdx);
                edgesToUpdate.insert(eIdx);
            } else {
                // Duplicate edge between vKeep and otherV: deactivate duplicate
                edges[eIdx].alive = false;
            }
        }
        vertices[vKeep].incidentEdges = std::move(uniqueKeepEdges);

        // Update neighbor edges and push to priority queue
        for (uint32_t eIdx : edgesToUpdate) {
            auto& e = edges[eIdx];
            if (!e.alive) continue;
            e.version++;
            if (computeEdgeCost(e) && e.cost < opts.maxError) {
                heap.push({eIdx, e.version, e.cost});
            }
        }

        // Progress notification
        if (opts.onProgress && facesToRemove > 0) {
            int facesDone = (mesh.nbFaces * 2) - activeFaces;
            int pct = std::clamp(static_cast<int>((facesDone * 100) / facesToRemove), 0, 100);
            if (pct != lastReportedPct) {
                lastReportedPct = pct;
                opts.onProgress(pct);
            }
        }
    }

    sculpt_log("[Decimation] Collapse loop finished: totalCollapses=%d, remainingActiveFaces=%d\n",
               totalCollapses, activeFaces);

    // 7. Compact and re-index surviving geometry back into Mesh
    std::vector<int32_t> vertMap(vertices.size(), -1);
    std::vector<float> newVerts;
    std::vector<float> newColors;
    std::vector<float> newMaterials;
    std::vector<uint32_t> newFaces;
    std::vector<uint32_t> newFaceGroups;

    newVerts.reserve(mesh.nbVerts * 3);
    newColors.reserve(mesh.nbVerts * 3);
    newMaterials.reserve(mesh.nbVerts * 3);
    newFaces.reserve(activeFaces * 4);
    newFaceGroups.reserve(activeFaces);

    int finalVertCount = 0;
    for (size_t i = 0; i < vertices.size(); ++i) {
        if (!vertices[i].alive) continue;
        vertMap[i] = finalVertCount++;
        newVerts.push_back(static_cast<float>(vertices[i].pos.x));
        newVerts.push_back(static_cast<float>(vertices[i].pos.y));
        newVerts.push_back(static_cast<float>(vertices[i].pos.z));

        newColors.push_back(vertices[i].color.r);
        newColors.push_back(vertices[i].color.g);
        newColors.push_back(vertices[i].color.b);

        newMaterials.push_back(vertices[i].material.x);
        newMaterials.push_back(vertices[i].material.y);
        newMaterials.push_back(vertices[i].material.z);
    }

    int finalFaceCount = 0;
    double finalSignedVolume = 0.0;

    for (const auto& tri : triangles) {
        if (!tri.alive) continue;
        int32_t v0 = vertMap[tri.v[0]];
        int32_t v1 = vertMap[tri.v[1]];
        int32_t v2 = vertMap[tri.v[2]];

        if (v0 < 0 || v1 < 0 || v2 < 0 || v0 == v1 || v1 == v2 || v2 == v0) {
            continue; // Safety check
        }

        newFaces.push_back(static_cast<uint32_t>(v0));
        newFaces.push_back(static_cast<uint32_t>(v1));
        newFaces.push_back(static_cast<uint32_t>(v2));
        newFaces.push_back(TRI_INDEX); // Triangle sentinel

        newFaceGroups.push_back(tri.groupID);
        finalFaceCount++;

        glm::dvec3 p0(newVerts[v0 * 3], newVerts[v0 * 3 + 1], newVerts[v0 * 3 + 2]);
        glm::dvec3 p1(newVerts[v1 * 3], newVerts[v1 * 3 + 1], newVerts[v1 * 3 + 2]);
        glm::dvec3 p2(newVerts[v2 * 3], newVerts[v2 * 3 + 1], newVerts[v2 * 3 + 2]);
        finalSignedVolume += glm::dot(p0, glm::cross(p1, p2));
    }
    finalSignedVolume /= 6.0;

    // 8. Assign new geometry buffers to Mesh
    mesh.verts = std::move(newVerts);
    mesh.colors = std::move(newColors);
    mesh.materials = std::move(newMaterials);
    mesh.faces = std::move(newFaces);
    mesh.faceGroups = std::move(newFaceGroups);
    mesh.nbVerts = finalVertCount;
    mesh.nbFaces = finalFaceCount;

    // Rebuild topology, normals, and octree
    mesh.vrfStartCount.clear();
    mesh.vertRingFace.clear();
    mesh.vrvStartCount.clear();
    mesh.vertRingVert.clear();
    mesh.initTopology();
    mesh.postInit();

    mesh.setDirty(true);
    mesh.isVertexDirty = true;
    mesh.isTopologyDirty = true;

    auto tEnd = std::chrono::high_resolution_clock::now();
    float duration = std::chrono::duration<float, std::milli>(tEnd - tStart).count();

    result.success = true;
    result.finalVerts = finalVertCount;
    result.finalFaces = finalFaceCount;
    result.durationMs = duration;

    if (std::abs(initialSignedVolume) > 1e-6) {
        result.volumeChange = static_cast<float>(((finalSignedVolume - initialSignedVolume) / std::abs(initialSignedVolume)) * 100.0);
    } else {
        result.volumeChange = 0.0f;
    }

    sculpt_log("[Decimation] Completed in %.2f ms: verts %d -> %d, faces %d -> %d, volumeChange = %.2f%%\n",
               duration, result.originalVerts, result.finalVerts, result.originalFaces, result.finalFaces, result.volumeChange);

    return result;
}
