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

    // 2. Compute area-weighted average normal and center of the cap faces
    glm::vec3 cutNormal(0.0f);
    glm::vec3 cutCenter(0.0f);
    float totalCapArea = 0.0f;
    for (int f : capFaceIndices) {
        uint32_t v0 = mesh.faces[f * 4 + 0];
        uint32_t v1 = mesh.faces[f * 4 + 1];
        uint32_t v2 = mesh.faces[f * 4 + 2];
        if (v0 >= (uint32_t)mesh.nbVerts || v1 >= (uint32_t)mesh.nbVerts || v2 >= (uint32_t)mesh.nbVerts) continue;
        glm::vec3 p0(mesh.verts[v0 * 3], mesh.verts[v0 * 3 + 1], mesh.verts[v0 * 3 + 2]);
        glm::vec3 p1(mesh.verts[v1 * 3], mesh.verts[v1 * 3 + 1], mesh.verts[v1 * 3 + 2]);
        glm::vec3 p2(mesh.verts[v2 * 3], mesh.verts[v2 * 3 + 1], mesh.verts[v2 * 3 + 2]);
        glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
        float area = glm::length(fn);
        if (area > 1e-6f) {
            cutNormal += fn;
            cutCenter += (p0 + p1 + p2) * (area / 3.0f);
            totalCapArea += area;
        }
    }

    if (totalCapArea > 1e-6f) {
        cutNormal = glm::normalize(cutNormal);
        cutCenter /= totalCapArea;
    } else {
        cutNormal = glm::vec3(0.0f, 1.0f, 0.0f);
    }

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

        // Use targetEdgeLen from mesh
        float step = targetEdgeLen;
        if (step <= 1e-4f) {
            step = totalEdgeLen / static_cast<float>(K);
        }
        step = std::clamp(step, 0.5f, 50.0f);
        float margin = step * 0.70f;

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

        for (int gy = gyMin; gy <= gyMax; ++gy) {
            for (int gx = gxMin; gx <= gxMax; ++gx) {
                glm::vec2 pt(gx * step, gy * step);
                if (pointInPoly2D(pt, poly2D) && distToPolyBoundary(pt, poly2D) >= margin) {
                    // Interpolate height offset hVal from boundary using IDW
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
                    glm::vec3 p3D = loopCenter + pt.x * U + pt.y * V + hVal * normal;

                    uint32_t newVid = static_cast<uint32_t>(mesh.verts.size() / 3);
                    mesh.verts.push_back(p3D.x);
                    mesh.verts.push_back(p3D.y);
                    mesh.verts.push_back(p3D.z);

                    mesh.normals.push_back(normal.x);
                    mesh.normals.push_back(normal.y);
                    mesh.normals.push_back(normal.z);

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
