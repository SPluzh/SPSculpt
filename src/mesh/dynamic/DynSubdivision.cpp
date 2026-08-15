#include "mesh/dynamic/DynSubdivision.h"
#include "mesh/Mesh.h"
#include "mesh/Octree.h"
#include "common/Constants.h"
#include "common/Logger.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>

namespace DynSubdivision {

static float dist2(const glm::vec3& a, const glm::vec3& b) {
    glm::vec3 diff = a - b;
    return glm::dot(diff, diff);
}

static inline void ringRemove(std::vector<uint32_t>& ring, uint32_t val) {
    for (size_t i = 0; i < ring.size(); ++i) {
        if (ring[i] == val) {
            ring[i] = ring.back();
            ring.pop_back();
            return;
        }
    }
}

static uint64_t makeEdgeKey(uint32_t u, uint32_t v) {
    uint32_t minV = std::min(u, v);
    uint32_t maxV = std::max(u, v);
    return (static_cast<uint64_t>(minV) << 32) | static_cast<uint64_t>(maxV);
}

static void updateFaceData(Mesh& mesh, uint32_t fIdx) {
    if (fIdx >= static_cast<uint32_t>(mesh.nbFaces)) return;

    uint32_t id = fIdx * 4;
    uint32_t v1 = mesh.faces[id];
    uint32_t v2 = mesh.faces[id + 1];
    uint32_t v3 = mesh.faces[id + 2];

    if (v1 >= static_cast<uint32_t>(mesh.nbVerts) ||
        v2 >= static_cast<uint32_t>(mesh.nbVerts) ||
        v3 >= static_cast<uint32_t>(mesh.nbVerts)) {
        return;
    }

    glm::vec3 p1(mesh.verts[v1 * 3], mesh.verts[v1 * 3 + 1], mesh.verts[v1 * 3 + 2]);
    glm::vec3 p2(mesh.verts[v2 * 3], mesh.verts[v2 * 3 + 1], mesh.verts[v2 * 3 + 2]);
    glm::vec3 p3(mesh.verts[v3 * 3], mesh.verts[v3 * 3 + 1], mesh.verts[v3 * 3 + 2]);

    glm::vec3 n = glm::cross(p2 - p1, p3 - p1);
    float len = glm::length(n);
    if (len > 1e-8f) n /= len;
    else n = glm::vec3(0.0f, 1.0f, 0.0f);

    mesh.faceNormals[fIdx * 3]     = n.x;
    mesh.faceNormals[fIdx * 3 + 1] = n.y;
    mesh.faceNormals[fIdx * 3 + 2] = n.z;

    glm::vec3 cen = (p1 + p2 + p3) * (1.0f / 3.0f);
    mesh.faceCenters[fIdx * 3]     = cen.x;
    mesh.faceCenters[fIdx * 3 + 1] = cen.y;
    mesh.faceCenters[fIdx * 3 + 2] = cen.z;

    mesh.faceBoxes[fIdx * 6]     = std::min({p1.x, p2.x, p3.x});
    mesh.faceBoxes[fIdx * 6 + 1] = std::min({p1.y, p2.y, p3.y});
    mesh.faceBoxes[fIdx * 6 + 2] = std::min({p1.z, p2.z, p3.z});
    mesh.faceBoxes[fIdx * 6 + 3] = std::max({p1.x, p2.x, p3.x});
    mesh.faceBoxes[fIdx * 6 + 4] = std::max({p1.y, p2.y, p3.y});
    mesh.faceBoxes[fIdx * 6 + 5] = std::max({p1.z, p2.z, p3.z});
}

static uint32_t ensureTriangle(Mesh& mesh, uint32_t fIdx, uint32_t va = UINT32_MAX, uint32_t vb = UINT32_MAX) {
    if (fIdx >= static_cast<uint32_t>(mesh.nbFaces)) return UINT32_MAX;
    if (mesh.faces[fIdx * 4 + 3] == TRI_INDEX) {
        return fIdx;
    }
    std::vector<uint32_t> singleQuad = {fIdx};
    auto newTris = mesh.triangulateQuadsInRegion(singleQuad);
    if (va != UINT32_MAX && vb != UINT32_MAX) {
        for (uint32_t tf : newTris) {
            uint32_t t1 = mesh.faces[tf * 4];
            uint32_t t2 = mesh.faces[tf * 4 + 1];
            uint32_t t3 = mesh.faces[tf * 4 + 2];
            bool hasA = (t1 == va || t2 == va || t3 == va);
            bool hasB = (t1 == vb || t2 == vb || t3 == vb);
            if (hasA && hasB) return tf;
        }
    }
    return newTris.empty() ? fIdx : newTris[0];
}

static uint32_t findOppositeTriangle(Mesh& mesh, uint32_t iTri, uint32_t va, uint32_t vb) {
    if (va >= mesh.dynVRF.size() || vb >= mesh.dynVRF.size()) return UINT32_MAX;
    const auto& ringA = mesh.dynVRF[va];
    const auto& ringB = mesh.dynVRF[vb];

    std::unordered_set<uint32_t> setB;
    for (uint32_t fB : ringB) {
        if (fB < static_cast<uint32_t>(mesh.nbFaces) && mesh.faces[fB * 4] != UINT32_MAX) {
            setB.insert(fB);
        }
    }
    for (uint32_t fA : ringA) {
        if (fA == iTri || fA >= static_cast<uint32_t>(mesh.nbFaces) || mesh.faces[fA * 4] == UINT32_MAX) continue;
        if (setB.count(fA)) {
            return ensureTriangle(mesh, fA, va, vb);
        }
    }
    return UINT32_MAX;
}

static void splitEdge(
    Mesh& mesh,
    uint32_t iTri,
    uint32_t va,
    uint32_t vb,
    uint32_t vc,
    std::unordered_map<uint64_t, uint32_t>& edgeMap,
    std::vector<uint32_t>& activeTris)
{
    uint64_t key = makeEdgeKey(va, vb);
    uint32_t vMid = UINT32_MAX;

    auto it = edgeMap.find(key);
    if (it == edgeMap.end()) {
        vMid = mesh.addNbVert(1);
        uint32_t a3 = va * 3;
        uint32_t b3 = vb * 3;
        uint32_t m3 = vMid * 3;

        mesh.verts[m3]     = (mesh.verts[a3]     + mesh.verts[b3])     * 0.5f;
        mesh.verts[m3 + 1] = (mesh.verts[a3 + 1] + mesh.verts[b3 + 1]) * 0.5f;
        mesh.verts[m3 + 2] = (mesh.verts[a3 + 2] + mesh.verts[b3 + 2]) * 0.5f;

        if (!mesh.normals.empty()) {
            float nx = (mesh.normals[a3]     + mesh.normals[b3])     * 0.5f;
            float ny = (mesh.normals[a3 + 1] + mesh.normals[b3 + 1]) * 0.5f;
            float nz = (mesh.normals[a3 + 2] + mesh.normals[b3 + 2]) * 0.5f;
            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }
            else { nx = 0.0f; ny = 1.0f; nz = 0.0f; }
            mesh.normals[m3]     = nx;
            mesh.normals[m3 + 1] = ny;
            mesh.normals[m3 + 2] = nz;
        }
        if (!mesh.colors.empty()) {
            mesh.colors[m3]     = (mesh.colors[a3]     + mesh.colors[b3])     * 0.5f;
            mesh.colors[m3 + 1] = (mesh.colors[a3 + 1] + mesh.colors[b3 + 1]) * 0.5f;
            mesh.colors[m3 + 2] = (mesh.colors[a3 + 2] + mesh.colors[b3 + 2]) * 0.5f;
        }
        if (!mesh.materials.empty()) {
            mesh.materials[m3]     = (mesh.materials[a3]     + mesh.materials[b3])     * 0.5f;
            mesh.materials[m3 + 1] = (mesh.materials[a3 + 1] + mesh.materials[b3 + 1]) * 0.5f;
            mesh.materials[m3 + 2] = (mesh.materials[a3 + 2] + mesh.materials[b3 + 2]) * 0.5f;
        }
        if (!mesh.vertProxy.empty()) {
            mesh.vertProxy[m3]     = (mesh.vertProxy[a3]     + mesh.vertProxy[b3])     * 0.5f;
            mesh.vertProxy[m3 + 1] = (mesh.vertProxy[a3 + 1] + mesh.vertProxy[b3 + 1]) * 0.5f;
            mesh.vertProxy[m3 + 2] = (mesh.vertProxy[a3 + 2] + mesh.vertProxy[b3 + 2]) * 0.5f;
        }
        edgeMap[key] = vMid;
    } else {
        vMid = it->second;
    }

    // 1. Split primary triangle iTri into tri1 and tri2
    uint32_t tri1 = iTri;
    uint32_t tri2 = mesh.addNbFace(1);

    mesh.faces[tri1 * 4]     = va;
    mesh.faces[tri1 * 4 + 1] = vMid;
    mesh.faces[tri1 * 4 + 2] = vc;
    mesh.faces[tri1 * 4 + 3] = TRI_INDEX;

    mesh.faces[tri2 * 4]     = vMid;
    mesh.faces[tri2 * 4 + 1] = vb;
    mesh.faces[tri2 * 4 + 2] = vc;
    mesh.faces[tri2 * 4 + 3] = TRI_INDEX;

    mesh.faceGroups[tri2] = mesh.faceGroups[tri1];

    if (vb < mesh.dynVRF.size()) {
        ringRemove(mesh.dynVRF[vb], tri1);
    }

    if (vMid < mesh.dynVRF.size()) {
        mesh.dynVRF[vMid].push_back(tri1);
        mesh.dynVRF[vMid].push_back(tri2);
    }
    if (vb < mesh.dynVRF.size()) mesh.dynVRF[vb].push_back(tri2);
    if (vc < mesh.dynVRF.size()) mesh.dynVRF[vc].push_back(tri2);

    mesh.computeRingVertices(va);
    mesh.computeRingVertices(vb);
    mesh.computeRingVertices(vc);
    mesh.computeRingVertices(vMid);

    updateFaceData(mesh, tri1);
    updateFaceData(mesh, tri2);

    if (tri1 < mesh.octree.faceLeaf.size() && mesh.octree.faceLeaf[tri1] != nullptr) {
        mesh.octree.addFaceToLeaf(tri2, mesh.octree.faceLeaf[tri1]);
    }

    // 2. Find and split opposite triangle oppTri if present
    uint32_t oppTri = findOppositeTriangle(mesh, iTri, va, vb);
    if (oppTri != UINT32_MAX) {
        uint32_t o1 = mesh.faces[oppTri * 4];
        uint32_t o2 = mesh.faces[oppTri * 4 + 1];
        uint32_t o3 = mesh.faces[oppTri * 4 + 2];
        uint32_t vd = (o1 != va && o1 != vb) ? o1 : ((o2 != va && o2 != vb) ? o2 : o3);

        uint32_t tri3 = oppTri;
        uint32_t tri4 = mesh.addNbFace(1);

        mesh.faces[tri3 * 4]     = vb;
        mesh.faces[tri3 * 4 + 1] = vMid;
        mesh.faces[tri3 * 4 + 2] = vd;
        mesh.faces[tri3 * 4 + 3] = TRI_INDEX;

        mesh.faces[tri4 * 4]     = vMid;
        mesh.faces[tri4 * 4 + 1] = va;
        mesh.faces[tri4 * 4 + 2] = vd;
        mesh.faces[tri4 * 4 + 3] = TRI_INDEX;

        mesh.faceGroups[tri4] = mesh.faceGroups[tri3];

        if (va < mesh.dynVRF.size()) {
            ringRemove(mesh.dynVRF[va], tri3);
        }

        if (vMid < mesh.dynVRF.size()) {
            mesh.dynVRF[vMid].push_back(tri3);
            mesh.dynVRF[vMid].push_back(tri4);
        }
        if (va < mesh.dynVRF.size()) mesh.dynVRF[va].push_back(tri4);
        if (vd < mesh.dynVRF.size()) mesh.dynVRF[vd].push_back(tri4);

        mesh.computeRingVertices(va);
        mesh.computeRingVertices(vb);
        mesh.computeRingVertices(vd);
        mesh.computeRingVertices(vMid);

        updateFaceData(mesh, tri3);
        updateFaceData(mesh, tri4);

        if (tri3 < mesh.octree.faceLeaf.size() && mesh.octree.faceLeaf[tri3] != nullptr) {
            mesh.octree.addFaceToLeaf(tri4, mesh.octree.faceLeaf[tri3]);
        }

        activeTris.push_back(tri3);
        activeTris.push_back(tri4);
    }

    activeTris.push_back(tri1);
    activeTris.push_back(tri2);
}

struct EdgeInfo {
    uint32_t count = 0;
    uint32_t firstFace = 0;
    bool firstDir = false;
};

static bool validateMeshTopology(Mesh& mesh, const char* label) {
    int invalidFaces = 0;
    int isolatedVerts = 0;
    int ghostFacesInRing = 0;
    int boundaryEdges = 0;
    int nonManifoldEdges = 0;
    int flippedFaces = 0;
    int vrfMissingFace = 0;
    int faceMissingFromVRF = 0;
    int duplicateInVRF = 0;
    int octreeOrphanFaces = 0;
    int octreePosMismatch = 0;

    std::unordered_map<uint64_t, EdgeInfo> edgeMap;
    edgeMap.reserve(mesh.nbFaces * 2);

    for (int i = 0; i < mesh.nbFaces; ++i) {
        uint32_t v1 = mesh.faces[i * 4];
        if (v1 == UINT32_MAX) continue;
        uint32_t v2 = mesh.faces[i * 4 + 1];
        uint32_t v3 = mesh.faces[i * 4 + 2];
        uint32_t v4 = mesh.faces[i * 4 + 3];

        bool isQuad = (v4 != TRI_INDEX);
        if (v1 >= (uint32_t)mesh.nbVerts || v2 >= (uint32_t)mesh.nbVerts || v3 >= (uint32_t)mesh.nbVerts ||
            (isQuad && v4 >= (uint32_t)mesh.nbVerts) ||
            v1 == v2 || v2 == v3 || v3 == v1 ||
            (isQuad && (v4 == v1 || v4 == v2 || v4 == v3))) {
            invalidFaces++;
            continue;
        }

        int numEdges = isQuad ? 4 : 3;
        uint32_t fVerts[4] = {v1, v2, v3, v4};
        for (int k = 0; k < numEdges; ++k) {
            uint32_t u = fVerts[k];
            uint32_t v = fVerts[(k + 1) % numEdges];

            if (u < mesh.dynVRF.size()) {
                const auto& ringF = mesh.dynVRF[u];
                if (std::find(ringF.begin(), ringF.end(), static_cast<uint32_t>(i)) == ringF.end()) {
                    faceMissingFromVRF++;
                }
            } else {
                faceMissingFromVRF++;
            }

            uint32_t minV = std::min(u, v);
            uint32_t maxV = std::max(u, v);
            uint64_t key = (static_cast<uint64_t>(minV) << 32) | static_cast<uint64_t>(maxV);
            bool dir = (u == minV);

            auto it = edgeMap.find(key);
            if (it == edgeMap.end()) {
                edgeMap[key] = {1, static_cast<uint32_t>(i), dir};
            } else {
                it->second.count++;
                if (it->second.count == 2) {
                    if (it->second.firstDir == dir) {
                        flippedFaces++;
                    }
                }
            }
        }
    }

    for (const auto& kv : edgeMap) {
        if (kv.second.count == 1) {
            boundaryEdges++;
        } else if (kv.second.count > 2) {
            nonManifoldEdges++;
        }
    }

    for (int i = 0; i < mesh.nbVerts; ++i) {
        if (mesh.dynVRF[i].empty()) {
            isolatedVerts++;
        }
        std::unordered_set<uint32_t> seen;
        for (uint32_t f : mesh.dynVRF[i]) {
            if (f >= (uint32_t)mesh.nbFaces) {
                ghostFacesInRing++;
            } else {
                uint32_t f1 = mesh.faces[f * 4];
                uint32_t f2 = mesh.faces[f * 4 + 1];
                uint32_t f3 = mesh.faces[f * 4 + 2];
                uint32_t f4 = mesh.faces[f * 4 + 3];
                if (f1 != (uint32_t)i && f2 != (uint32_t)i && f3 != (uint32_t)i && f4 != (uint32_t)i) {
                    vrfMissingFace++;
                }
            }
            if (!seen.insert(f).second) {
                duplicateInVRF++;
            }
        }
    }

    if (mesh.octree.faceLeaf.size() >= (size_t)mesh.nbFaces) {
        for (int i = 0; i < mesh.nbFaces; ++i) {
            OctreeCell* leaf = mesh.octree.faceLeaf[i];
            if (!leaf) {
                octreeOrphanFaces++;
            } else {
                int pos = mesh.octree.facePosInLeaf[i];
                if (pos < 0 || pos >= (int)leaf->iFaces.size() || leaf->iFaces[pos] != (uint32_t)i) {
                    octreePosMismatch++;
                }
            }
        }
    }

    int V = mesh.nbVerts;
    int F = mesh.nbFaces;
    int E = static_cast<int>(edgeMap.size());
    int eulerChar = V - E + F;

    bool hasIssues = (invalidFaces > 0 || isolatedVerts > 0 || ghostFacesInRing > 0 ||
                      boundaryEdges > 0 || nonManifoldEdges > 0 || flippedFaces > 0 ||
                      vrfMissingFace > 0 || faceMissingFromVRF > 0 || duplicateInVRF > 0 ||
                      octreeOrphanFaces > 0 || octreePosMismatch > 0);

    if (hasIssues) {
        sculpt_log("[DynTopo Health Check - %s] WARNING! Issues found: InvalidFaces: %d, IsolatedVerts: %d, GhostFaces: %d, "
                  "BOUNDARY_EDGES: %d (HOLES!), NonManifoldEdges: %d, FlippedFaces: %d, VRF_Mismatch: %d/%d, DupVRF: %d, "
                  "OctreeOrphans: %d, OctreePosMismatch: %d | Euler (V-E+F): %d - %d + %d = %d\n",
                  label, invalidFaces, isolatedVerts, ghostFacesInRing,
                  boundaryEdges, nonManifoldEdges, flippedFaces, vrfMissingFace, faceMissingFromVRF, duplicateInVRF,
                  octreeOrphanFaces, octreePosMismatch, V, E, F, eulerChar);
        return false;
    }

    sculpt_log("[DynTopo Health Check - %s] PASSED cleanly (no holes [0 boundary edges], no ghost faces, no isolated verts, Euler chi=%d).\n",
              label, eulerChar);
    return true;
}

std::vector<uint32_t> subdivision(
    Mesh& mesh,
    const std::vector<uint32_t>& iTris,
    const glm::vec3& center,
    float radius2,
    float detail2,
    bool linear)
{
    if (!mesh.isDynamic) {
        mesh.initDynamicMode();
    }

    std::vector<uint32_t> activeTris = iTris;
    activeTris.reserve(iTris.size() * 4);
    std::unordered_map<uint64_t, uint32_t> edgeMap;
    edgeMap.reserve(iTris.size() * 2);

    size_t idx = 0;
    size_t maxIter = 100000;
    size_t count = 0;
    uint32_t splitsPerformed = 0;

    while (idx < activeTris.size() && count++ < maxIter) {
        uint32_t fIdx = activeTris[idx++];
        if (fIdx >= static_cast<uint32_t>(mesh.nbFaces)) continue;

        uint32_t id = fIdx * 4;
        uint32_t v1 = mesh.faces[id];
        uint32_t v2 = mesh.faces[id + 1];
        uint32_t v3 = mesh.faces[id + 2];
        uint32_t v4 = mesh.faces[id + 3];

        if (v4 != TRI_INDEX) {
            std::vector<uint32_t> singleQuad = {fIdx};
            auto triRes = mesh.triangulateQuadsInRegion(singleQuad);
            for (uint32_t tf : triRes) {
                activeTris.push_back(tf);
            }
            continue;
        }

        if (v1 >= static_cast<uint32_t>(mesh.nbVerts) ||
            v2 >= static_cast<uint32_t>(mesh.nbVerts) ||
            v3 >= static_cast<uint32_t>(mesh.nbVerts)) {
            continue;
        }

        glm::vec3 p1(mesh.verts[v1 * 3], mesh.verts[v1 * 3 + 1], mesh.verts[v1 * 3 + 2]);
        glm::vec3 p2(mesh.verts[v2 * 3], mesh.verts[v2 * 3 + 1], mesh.verts[v2 * 3 + 2]);
        glm::vec3 p3(mesh.verts[v3 * 3], mesh.verts[v3 * 3 + 1], mesh.verts[v3 * 3 + 2]);

        float l12 = dist2(p1, p2);
        float l23 = dist2(p2, p3);
        float l31 = dist2(p3, p1);

        glm::vec3 m12 = (p1 + p2) * 0.5f;
        glm::vec3 m23 = (p2 + p3) * 0.5f;
        glm::vec3 m31 = (p3 + p1) * 0.5f;

        bool inSph12 = (dist2(m12, center) <= radius2);
        bool inSph23 = (dist2(m23, center) <= radius2);
        bool inSph31 = (dist2(m31, center) <= radius2);

        float maxL = detail2;
        int bestEdge = -1;

        if (inSph12 && l12 > maxL) { maxL = l12; bestEdge = 0; }
        if (inSph23 && l23 > maxL) { maxL = l23; bestEdge = 1; }
        if (inSph31 && l31 > maxL) { maxL = l31; bestEdge = 2; }

        if (bestEdge == 0) {
            splitEdge(mesh, fIdx, v1, v2, v3, edgeMap, activeTris);
            splitsPerformed++;
        } else if (bestEdge == 1) {
            splitEdge(mesh, fIdx, v2, v3, v1, edgeMap, activeTris);
            splitsPerformed++;
        } else if (bestEdge == 2) {
            splitEdge(mesh, fIdx, v3, v1, v2, edgeMap, activeTris);
            splitsPerformed++;
        }
    }

    sculpt_log("[DynTopo Subdivision Stats] Edge splits performed: %u (New Verts: %zu)\n", splitsPerformed, edgeMap.size());
#ifdef DYNTOPO_HEALTH_CHECK
    validateMeshTopology(mesh, "Post-Subdivision");
#endif

    std::sort(activeTris.begin(), activeTris.end());
    activeTris.erase(std::unique(activeTris.begin(), activeTris.end()), activeTris.end());

    std::vector<uint32_t> result;
    result.reserve(activeTris.size());
    for (uint32_t f : activeTris) {
        if (f < static_cast<uint32_t>(mesh.nbFaces) && mesh.faces[f * 4 + 3] == TRI_INDEX) {
            result.push_back(f);
        }
    }

    mesh.isDirty = true;
    mesh.isTopologyDirty = true;
    return result;
}

} // namespace DynSubdivision

