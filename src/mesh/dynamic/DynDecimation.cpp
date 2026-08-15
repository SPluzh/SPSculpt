#include "mesh/dynamic/DynDecimation.h"
#include "mesh/Mesh.h"
#include "mesh/Octree.h"
#include "common/Constants.h"
#include "common/Logger.h"
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace DynDecimation {

static float dist2(const glm::vec3& a, const glm::vec3& b) {
    glm::vec3 diff = a - b;
    return glm::dot(diff, diff);
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

static void deleteTriangle(
    Mesh& mesh,
    uint32_t fIdx)
{
    if (fIdx >= static_cast<uint32_t>(mesh.nbFaces)) return;
    if (mesh.faces[fIdx * 4] == UINT32_MAX) return;

    for (int k = 0; k < 4; ++k) {
        uint32_t v = mesh.faces[fIdx * 4 + k];
        if (v != TRI_INDEX && v < mesh.dynVRF.size()) {
            auto& ring = mesh.dynVRF[v];
            ring.erase(std::remove(ring.begin(), ring.end(), fIdx), ring.end());
        }
    }

    mesh.octree.removeFaceFromLeaf(fIdx);

    mesh.faces[fIdx * 4]     = UINT32_MAX;
    mesh.faces[fIdx * 4 + 1] = UINT32_MAX;
    mesh.faces[fIdx * 4 + 2] = UINT32_MAX;
    mesh.faces[fIdx * 4 + 3] = UINT32_MAX;
}

static uint32_t deleteVertex(Mesh& mesh, uint32_t vIdx, uint32_t va) {
    uint32_t lastVert = static_cast<uint32_t>(mesh.nbVerts - 1);
    uint32_t finalVa = va;

    if (vIdx != lastVert) {
        mesh.verts[vIdx * 3]     = mesh.verts[lastVert * 3];
        mesh.verts[vIdx * 3 + 1] = mesh.verts[lastVert * 3 + 1];
        mesh.verts[vIdx * 3 + 2] = mesh.verts[lastVert * 3 + 2];

        if (!mesh.normals.empty()) {
            mesh.normals[vIdx * 3]     = mesh.normals[lastVert * 3];
            mesh.normals[vIdx * 3 + 1] = mesh.normals[lastVert * 3 + 1];
            mesh.normals[vIdx * 3 + 2] = mesh.normals[lastVert * 3 + 2];
        }
        if (!mesh.colors.empty()) {
            mesh.colors[vIdx * 3]     = mesh.colors[lastVert * 3];
            mesh.colors[vIdx * 3 + 1] = mesh.colors[lastVert * 3 + 1];
            mesh.colors[vIdx * 3 + 2] = mesh.colors[lastVert * 3 + 2];
        }
        if (!mesh.materials.empty()) {
            mesh.materials[vIdx * 3]     = mesh.materials[lastVert * 3];
            mesh.materials[vIdx * 3 + 1] = mesh.materials[lastVert * 3 + 1];
            mesh.materials[vIdx * 3 + 2] = mesh.materials[lastVert * 3 + 2];
        }
        if (!mesh.vertProxy.empty()) {
            mesh.vertProxy[vIdx * 3]     = mesh.vertProxy[lastVert * 3];
            mesh.vertProxy[vIdx * 3 + 1] = mesh.vertProxy[lastVert * 3 + 1];
            mesh.vertProxy[vIdx * 3 + 2] = mesh.vertProxy[lastVert * 3 + 2];
        }
        if (!mesh.vertVisible.empty()) mesh.vertVisible[vIdx] = mesh.vertVisible[lastVert];
        if (!mesh.vertTagFlags.empty()) mesh.vertTagFlags[vIdx] = mesh.vertTagFlags[lastVert];
        if (!mesh.vertSculptFlags.empty()) mesh.vertSculptFlags[vIdx] = mesh.vertSculptFlags[lastVert];
        if (!mesh.vertStateFlags.empty()) mesh.vertStateFlags[vIdx] = mesh.vertStateFlags[lastVert];

        mesh.dynVRV[vIdx] = std::move(mesh.dynVRV[lastVert]);
        mesh.dynVRF[vIdx] = std::move(mesh.dynVRF[lastVert]);

        for (uint32_t fIdx : mesh.dynVRF[vIdx]) {
            if (fIdx < static_cast<uint32_t>(mesh.nbFaces) && mesh.faces[fIdx * 4] != UINT32_MAX) {
                for (int k = 0; k < 4; ++k) {
                    if (mesh.faces[fIdx * 4 + k] == lastVert) {
                        mesh.faces[fIdx * 4 + k] = vIdx;
                    }
                }
            }
        }

        for (uint32_t nIdx : mesh.dynVRV[vIdx]) {
            if (nIdx < mesh.dynVRV.size()) {
                auto& ring = mesh.dynVRV[nIdx];
                std::replace(ring.begin(), ring.end(), lastVert, vIdx);
            }
        }

        if (va == lastVert) {
            finalVa = vIdx;
        }
    }

    mesh.nbVerts--;

    if (vIdx != lastVert) {
        if (static_cast<size_t>(mesh.nbVerts) < mesh.dynVRV.size()) {
            mesh.dynVRV[mesh.nbVerts].clear();
            mesh.dynVRF[mesh.nbVerts].clear();
        }
    }
    return finalVa;
}

enum class CollapseRejectReason {
    None,
    InvalidIndex,
    SameVertex,
    TopologySharedFaces,
    LinkCondition,
    NormalFlip
};

static uint32_t ensureTriangle(Mesh& mesh, uint32_t fIdx) {
    if (fIdx >= static_cast<uint32_t>(mesh.nbFaces) || mesh.faces[fIdx * 4] == UINT32_MAX) return UINT32_MAX;
    if (mesh.faces[fIdx * 4 + 3] == TRI_INDEX) {
        return fIdx;
    }
    std::vector<uint32_t> singleQuad = {fIdx};
    auto newTris = mesh.triangulateQuadsInRegion(singleQuad);
    return newTris.empty() ? fIdx : newTris[0];
}

static void ensureRingsTriangulated(Mesh& mesh, uint32_t va, uint32_t vb) {
    if (!mesh.hasQuads) return;
    bool hadQuads = true;
    while (hadQuads) {
        hadQuads = false;
        if (va < mesh.dynVRF.size()) {
            for (size_t i = 0; i < mesh.dynVRF[va].size(); ++i) {
                uint32_t f = mesh.dynVRF[va][i];
                if (f < (uint32_t)mesh.nbFaces && mesh.faces[f * 4] != UINT32_MAX && mesh.faces[f * 4 + 3] != TRI_INDEX) {
                    ensureTriangle(mesh, f);
                    hadQuads = true;
                    break;
                }
            }
        }
        if (hadQuads) continue;
        if (vb < mesh.dynVRF.size()) {
            for (size_t i = 0; i < mesh.dynVRF[vb].size(); ++i) {
                uint32_t f = mesh.dynVRF[vb][i];
                if (f < (uint32_t)mesh.nbFaces && mesh.faces[f * 4] != UINT32_MAX && mesh.faces[f * 4 + 3] != TRI_INDEX) {
                    ensureTriangle(mesh, f);
                    hadQuads = true;
                    break;
                }
            }
        }
    }
}

static CollapseRejectReason checkCanCollapse(const Mesh& mesh, uint32_t va, uint32_t vb) {
    if (va >= static_cast<uint32_t>(mesh.nbVerts) || vb >= static_cast<uint32_t>(mesh.nbVerts)) {
        return CollapseRejectReason::InvalidIndex;
    }
    if (va == vb) return CollapseRejectReason::SameVertex;
    if (va >= mesh.dynVRF.size() || vb >= mesh.dynVRF.size() ||
        va >= mesh.dynVRV.size() || vb >= mesh.dynVRV.size()) {
        return CollapseRejectReason::InvalidIndex;
    }

    const auto& ringFa = mesh.dynVRF[va];
    const auto& ringFb = mesh.dynVRF[vb];

    uint32_t sharedFaces[4];
    int numSharedFaces = 0;
    for (uint32_t fa : ringFa) {
        if (fa >= static_cast<uint32_t>(mesh.nbFaces) || mesh.faces[fa * 4] == UINT32_MAX) continue;
        for (uint32_t fb : ringFb) {
            if (fa == fb) {
                if (numSharedFaces < 4) sharedFaces[numSharedFaces++] = fa;
                break;
            }
        }
    }
    if (numSharedFaces == 0 || numSharedFaces > 2) {
        return CollapseRejectReason::TopologySharedFaces;
    }

    const auto& ringVa = mesh.dynVRV[va];
    const auto& ringVb = mesh.dynVRV[vb];

    uint32_t sharedVerts[8];
    int numSharedVerts = 0;
    for (uint32_t v_a : ringVa) {
        if (v_a == vb || v_a >= static_cast<uint32_t>(mesh.nbVerts)) continue;
        for (uint32_t v_b : ringVb) {
            if (v_a == v_b) {
                if (numSharedVerts < 8) sharedVerts[numSharedVerts++] = v_a;
                break;
            }
        }
    }

    // STRICT LINK CONDITION: sharedVerts count MUST equal sharedFaces count
    if (numSharedVerts != numSharedFaces) {
        return CollapseRejectReason::LinkCondition;
    }

    // Ensure collapsing sharedFaces won't leave any opposite vertex isolated (with 0 valid faces)
    for (int sfIdx = 0; sfIdx < numSharedFaces; ++sfIdx) {
        uint32_t f = sharedFaces[sfIdx];
        if (f >= static_cast<uint32_t>(mesh.nbFaces) || mesh.faces[f * 4] == UINT32_MAX) continue;
        uint32_t id = f * 4;
        for (int k = 0; k < 3; ++k) {
            uint32_t v = mesh.faces[id + k];
            if (v >= static_cast<uint32_t>(mesh.nbVerts) || v == va || v == vb) continue;
            size_t countInShared = 0;
            for (int sfIdx2 = 0; sfIdx2 < numSharedFaces; ++sfIdx2) {
                uint32_t sf = sharedFaces[sfIdx2];
                if (sf >= static_cast<uint32_t>(mesh.nbFaces) || mesh.faces[sf * 4] == UINT32_MAX) continue;
                uint32_t sfId = sf * 4;
                if (mesh.faces[sfId] == v || mesh.faces[sfId + 1] == v || mesh.faces[sfId + 2] == v) {
                    countInShared++;
                }
            }
            if (v < mesh.dynVRF.size()) {
                size_t validFacesInRing = 0;
                for (uint32_t vf : mesh.dynVRF[v]) {
                    if (vf < static_cast<uint32_t>(mesh.nbFaces) && mesh.faces[vf * 4] != UINT32_MAX) {
                        validFacesInRing++;
                    }
                }
                if (validFacesInRing <= countInShared) {
                    return CollapseRejectReason::TopologySharedFaces;
                }
            }
        }
    }

    glm::vec3 pa(mesh.verts[va * 3], mesh.verts[va * 3 + 1], mesh.verts[va * 3 + 2]);
    glm::vec3 pb(mesh.verts[vb * 3], mesh.verts[vb * 3 + 1], mesh.verts[vb * 3 + 2]);
    glm::vec3 pMid = (pa + pb) * 0.5f;

    auto isSharedFace = [&](uint32_t f) {
        for (int i = 0; i < numSharedFaces; ++i) {
            if (sharedFaces[i] == f) return true;
        }
        return false;
    };

    for (uint32_t fIdx : ringFb) {
        if (fIdx >= static_cast<uint32_t>(mesh.nbFaces) || mesh.faces[fIdx * 4] == UINT32_MAX || isSharedFace(fIdx)) continue;
        uint32_t id = fIdx * 4;
        uint32_t ov1 = mesh.faces[id];
        uint32_t ov2 = mesh.faces[id + 1];
        uint32_t ov3 = mesh.faces[id + 2];
        if (ov1 >= static_cast<uint32_t>(mesh.nbVerts) ||
            ov2 >= static_cast<uint32_t>(mesh.nbVerts) ||
            ov3 >= static_cast<uint32_t>(mesh.nbVerts)) continue;

        glm::vec3 op1(mesh.verts[ov1 * 3], mesh.verts[ov1 * 3 + 1], mesh.verts[ov1 * 3 + 2]);
        glm::vec3 op2(mesh.verts[ov2 * 3], mesh.verts[ov2 * 3 + 1], mesh.verts[ov2 * 3 + 2]);
        glm::vec3 op3(mesh.verts[ov3 * 3], mesh.verts[ov3 * 3 + 1], mesh.verts[ov3 * 3 + 2]);

        glm::vec3 oldN = glm::cross(op2 - op1, op3 - op1);
        float oldLenSq = glm::dot(oldN, oldN);
        if (oldLenSq <= 1e-12f) return CollapseRejectReason::NormalFlip;
        oldN /= std::sqrt(oldLenSq);

        uint32_t v1 = (ov1 == vb) ? va : ov1;
        uint32_t v2 = (ov2 == vb) ? va : ov2;
        uint32_t v3 = (ov3 == vb) ? va : ov3;
        if (v1 >= static_cast<uint32_t>(mesh.nbVerts) ||
            v2 >= static_cast<uint32_t>(mesh.nbVerts) ||
            v3 >= static_cast<uint32_t>(mesh.nbVerts)) continue;

        glm::vec3 np1 = (v1 == va) ? pMid : glm::vec3(mesh.verts[v1 * 3], mesh.verts[v1 * 3 + 1], mesh.verts[v1 * 3 + 2]);
        glm::vec3 np2 = (v2 == va) ? pMid : glm::vec3(mesh.verts[v2 * 3], mesh.verts[v2 * 3 + 1], mesh.verts[v2 * 3 + 2]);
        glm::vec3 np3 = (v3 == va) ? pMid : glm::vec3(mesh.verts[v3 * 3], mesh.verts[v3 * 3 + 1], mesh.verts[v3 * 3 + 2]);

        glm::vec3 newN = glm::cross(np2 - np1, np3 - np1);
        float newLenSq = glm::dot(newN, newN);
        if (newLenSq <= 1e-12f) return CollapseRejectReason::NormalFlip;
        newN /= std::sqrt(newLenSq);

        if (glm::dot(oldN, newN) < 0.0f) {
            return CollapseRejectReason::NormalFlip;
        }
    }

    for (uint32_t fIdx : ringFa) {
        if (fIdx >= static_cast<uint32_t>(mesh.nbFaces) || mesh.faces[fIdx * 4] == UINT32_MAX || isSharedFace(fIdx)) continue;
        uint32_t id = fIdx * 4;
        uint32_t ov1 = mesh.faces[id];
        uint32_t ov2 = mesh.faces[id + 1];
        uint32_t ov3 = mesh.faces[id + 2];
        if (ov1 >= static_cast<uint32_t>(mesh.nbVerts) ||
            ov2 >= static_cast<uint32_t>(mesh.nbVerts) ||
            ov3 >= static_cast<uint32_t>(mesh.nbVerts)) continue;

        glm::vec3 op1(mesh.verts[ov1 * 3], mesh.verts[ov1 * 3 + 1], mesh.verts[ov1 * 3 + 2]);
        glm::vec3 op2(mesh.verts[ov2 * 3], mesh.verts[ov2 * 3 + 1], mesh.verts[ov2 * 3 + 2]);
        glm::vec3 op3(mesh.verts[ov3 * 3], mesh.verts[ov3 * 3 + 1], mesh.verts[ov3 * 3 + 2]);

        glm::vec3 oldN = glm::cross(op2 - op1, op3 - op1);
        float oldLenSq = glm::dot(oldN, oldN);
        if (oldLenSq <= 1e-12f) return CollapseRejectReason::NormalFlip;
        oldN /= std::sqrt(oldLenSq);

        glm::vec3 np1 = (ov1 == va) ? pMid : glm::vec3(mesh.verts[ov1 * 3], mesh.verts[ov1 * 3 + 1], mesh.verts[ov1 * 3 + 2]);
        glm::vec3 np2 = (ov2 == va) ? pMid : glm::vec3(mesh.verts[ov2 * 3], mesh.verts[ov2 * 3 + 1], mesh.verts[ov2 * 3 + 2]);
        glm::vec3 np3 = (ov3 == va) ? pMid : glm::vec3(mesh.verts[ov3 * 3], mesh.verts[ov3 * 3 + 1], mesh.verts[ov3 * 3 + 2]);

        glm::vec3 newN = glm::cross(np2 - np1, np3 - np1);
        float newLenSq = glm::dot(newN, newN);
        if (newLenSq <= 1e-12f) return CollapseRejectReason::NormalFlip;
        newN /= std::sqrt(newLenSq);

        if (glm::dot(oldN, newN) < 0.0f) {
            return CollapseRejectReason::NormalFlip;
        }
    }

    return CollapseRejectReason::None;
}

static bool executeCollapse(Mesh& mesh, uint32_t va, uint32_t vb, std::vector<uint32_t>& activeTris, CollapseRejectReason& outReason) {
    ensureRingsTriangulated(mesh, va, vb);
    outReason = checkCanCollapse(mesh, va, vb);
    if (outReason != CollapseRejectReason::None) return false;
    glm::vec3 pa(mesh.verts[va * 3], mesh.verts[va * 3 + 1], mesh.verts[va * 3 + 2]);
    glm::vec3 pb(mesh.verts[vb * 3], mesh.verts[vb * 3 + 1], mesh.verts[vb * 3 + 2]);
    glm::vec3 pMid = (pa + pb) * 0.5f;

    mesh.verts[va * 3]     = pMid.x;
    mesh.verts[va * 3 + 1] = pMid.y;
    mesh.verts[va * 3 + 2] = pMid.z;

    if (!mesh.colors.empty()) {
        mesh.colors[va * 3]     = (mesh.colors[va * 3]     + mesh.colors[vb * 3])     * 0.5f;
        mesh.colors[va * 3 + 1] = (mesh.colors[va * 3 + 1] + mesh.colors[vb * 3 + 1]) * 0.5f;
        mesh.colors[va * 3 + 2] = (mesh.colors[va * 3 + 2] + mesh.colors[vb * 3 + 2]) * 0.5f;
    }
    if (!mesh.materials.empty()) {
        mesh.materials[va * 3]     = (mesh.materials[va * 3]     + mesh.materials[vb * 3])     * 0.5f;
        mesh.materials[va * 3 + 1] = (mesh.materials[va * 3 + 1] + mesh.materials[vb * 3 + 1]) * 0.5f;
        mesh.materials[va * 3 + 2] = (mesh.materials[va * 3 + 2] + mesh.materials[vb * 3 + 2]) * 0.5f;
    }

    std::vector<uint32_t> ringFa = (va < mesh.dynVRF.size()) ? mesh.dynVRF[va] : std::vector<uint32_t>{};
    std::vector<uint32_t> ringFb = (vb < mesh.dynVRF.size()) ? mesh.dynVRF[vb] : std::vector<uint32_t>{};

    uint32_t sharedFaces[4];
    int numSharedFaces = 0;
    for (uint32_t fa : ringFa) {
        if (fa >= static_cast<uint32_t>(mesh.nbFaces) || mesh.faces[fa * 4] == UINT32_MAX) continue;
        for (uint32_t fb : ringFb) {
            if (fa == fb) {
                if (numSharedFaces < 4) sharedFaces[numSharedFaces++] = fa;
                break;
            }
        }
    }
    auto isSharedFace = [&](uint32_t f) {
        for (int i = 0; i < numSharedFaces; ++i) {
            if (sharedFaces[i] == f) return true;
        }
        return false;
    };

    // Save old neighbors of vb BEFORE faces and rings are modified
    std::vector<uint32_t> oldNeighborsVb = (vb < mesh.dynVRV.size()) ? mesh.dynVRV[vb] : std::vector<uint32_t>();

    // 1. Transfer non-shared faces from vb to va
    for (uint32_t fIdx : ringFb) {
        if (fIdx >= static_cast<uint32_t>(mesh.nbFaces) || mesh.faces[fIdx * 4] == UINT32_MAX) continue;
        if (!isSharedFace(fIdx)) {
            uint32_t id = fIdx * 4;
            for (int k = 0; k < 4; ++k) {
                if (mesh.faces[id + k] == vb) {
                    mesh.faces[id + k] = va;
                }
            }
            if (std::find(mesh.dynVRF[va].begin(), mesh.dynVRF[va].end(), fIdx) == mesh.dynVRF[va].end()) {
                mesh.dynVRF[va].push_back(fIdx);
            }
            updateFaceData(mesh, fIdx);
        }
    }

    // 2. Update face data for ringFa non-shared faces since vertex va moved to pMid
    for (uint32_t fIdx : ringFa) {
        if (fIdx >= static_cast<uint32_t>(mesh.nbFaces) || mesh.faces[fIdx * 4] == UINT32_MAX) continue;
        if (!isSharedFace(fIdx)) {
            updateFaceData(mesh, fIdx);
        }
    }

    // 3. Delete shared faces
    for (int i = 0; i < numSharedFaces; ++i) {
        uint32_t fIdx = sharedFaces[i];
        deleteTriangle(mesh, fIdx);
    }

    // Clean up tombstoned faces from dynVRF[va]
    auto& rFa = mesh.dynVRF[va];
    rFa.erase(std::remove_if(rFa.begin(), rFa.end(), [&](uint32_t f) {
        return f >= static_cast<uint32_t>(mesh.nbFaces) || mesh.faces[f * 4] == UINT32_MAX;
    }), rFa.end());

    uint32_t lastVertBeforeDelete = static_cast<uint32_t>(mesh.nbVerts - 1);
    uint32_t finalVa = deleteVertex(mesh, vb, va);

    uint32_t nbrsToUpdate[64];
    int numNbrsToUpdate = 0;
    auto addNbr = [&](uint32_t v) {
        if (v >= static_cast<uint32_t>(mesh.nbVerts)) return;
        for (int i = 0; i < numNbrsToUpdate; ++i) {
            if (nbrsToUpdate[i] == v) return;
        }
        if (numNbrsToUpdate < 64) {
            nbrsToUpdate[numNbrsToUpdate++] = v;
        }
    };

    addNbr(finalVa);
    if (finalVa < mesh.dynVRV.size()) {
        for (uint32_t n : mesh.dynVRV[finalVa]) addNbr(n);
    }
    for (uint32_t n : oldNeighborsVb) addNbr(n);
    if (vb != lastVertBeforeDelete && vb < static_cast<uint32_t>(mesh.nbVerts)) {
        addNbr(vb);
        if (vb < mesh.dynVRV.size()) {
            for (uint32_t n : mesh.dynVRV[vb]) addNbr(n);
        }
    }

    for (int i = 0; i < numNbrsToUpdate; ++i) {
        mesh.computeRingVertices(nbrsToUpdate[i]);
    }

    return true;
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

std::vector<uint32_t> decimation(
    Mesh& mesh,
    const std::vector<uint32_t>& iTris,
    const glm::vec3& center,
    float radius2,
    float detail2)
{
    if (!mesh.isDynamic) {
        mesh.initDynamicMode();
    }

    sculpt_log("[DynTopo Decimation Start] iTris count: %zu, nbVerts: %d, nbFaces: %d\n",
               iTris.size(), mesh.nbVerts, mesh.nbFaces);

    std::vector<uint32_t> activeTris = iTris;
    size_t idx = 0;
    size_t maxIter = 100000;
    size_t count = 0;

    uint32_t attemptedCollapses = 0;
    uint32_t successfulCollapses = 0;
    uint32_t rejectedLinkCondition = 0;
    uint32_t rejectedNormalFlip = 0;
    uint32_t rejectedSharedFaces = 0;

    while (idx < activeTris.size() && count++ < maxIter) {
        uint32_t fIdx = activeTris[idx++];
        if (fIdx >= static_cast<uint32_t>(mesh.nbFaces) || mesh.faces[fIdx * 4] == UINT32_MAX) continue;

        uint32_t id = fIdx * 4;
        uint32_t v1 = mesh.faces[id];
        uint32_t v2 = mesh.faces[id + 1];
        uint32_t v3 = mesh.faces[id + 2];
        uint32_t v4 = mesh.faces[id + 3];

        if (v4 != TRI_INDEX) continue;
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

        float minL = detail2;
        int bestEdge = -1;

        if (inSph12 && l12 < minL) { minL = l12; bestEdge = 0; }
        if (inSph23 && l23 < minL) { minL = l23; bestEdge = 1; }
        if (inSph31 && l31 < minL) { minL = l31; bestEdge = 2; }

        if (bestEdge >= 0) {
            attemptedCollapses++;
            CollapseRejectReason reason = CollapseRejectReason::None;
            bool ok = false;
            uint32_t ca = (bestEdge == 0) ? v1 : ((bestEdge == 1) ? v2 : v3);
            uint32_t cb = (bestEdge == 0) ? v2 : ((bestEdge == 1) ? v3 : v1);
            if (bestEdge == 0) ok = executeCollapse(mesh, v1, v2, activeTris, reason);
            else if (bestEdge == 1) ok = executeCollapse(mesh, v2, v3, activeTris, reason);
            else if (bestEdge == 2) ok = executeCollapse(mesh, v3, v1, activeTris, reason);

            if (ok) {
                successfulCollapses++;
            } else {
                if (reason == CollapseRejectReason::LinkCondition) rejectedLinkCondition++;
                else if (reason == CollapseRejectReason::NormalFlip) rejectedNormalFlip++;
                else if (reason == CollapseRejectReason::TopologySharedFaces) rejectedSharedFaces++;
            }
        }
    }

    sculpt_log("[DynTopo Decimation Stats] Collapses: %u ok / %u attempted (Link Rejects: %u, NormalFlip Rejects: %u, Topology Rejects: %u)\n",
              successfulCollapses, attemptedCollapses, rejectedLinkCondition, rejectedNormalFlip, rejectedSharedFaces);

    // Apply vertex array resizing ONCE after all collapses
    mesh.verts.resize(mesh.nbVerts * 3);
    if (!mesh.normals.empty()) mesh.normals.resize(mesh.nbVerts * 3);
    if (!mesh.colors.empty()) mesh.colors.resize(mesh.nbVerts * 3);
    if (!mesh.materials.empty()) mesh.materials.resize(mesh.nbVerts * 3);
    if (!mesh.vertProxy.empty()) mesh.vertProxy.resize(mesh.nbVerts * 3);
    if (!mesh.vertVisible.empty()) mesh.vertVisible.resize(mesh.nbVerts);
    if (!mesh.vertTagFlags.empty()) mesh.vertTagFlags.resize(mesh.nbVerts);
    if (!mesh.vertSculptFlags.empty()) mesh.vertSculptFlags.resize(mesh.nbVerts);
    if (!mesh.vertStateFlags.empty()) mesh.vertStateFlags.resize(mesh.nbVerts);
    mesh.dynVRV.resize(mesh.nbVerts);
    mesh.dynVRF.resize(mesh.nbVerts);

    // --- Tombstone Compaction Pass ---
    uint32_t facesBeforeCompaction = static_cast<uint32_t>(mesh.nbFaces);
    std::vector<uint32_t> remapFace(facesBeforeCompaction, UINT32_MAX);
    uint32_t newF = 0;

    for (uint32_t oldF = 0; oldF < facesBeforeCompaction; ++oldF) {
        if (mesh.faces[oldF * 4] != UINT32_MAX) {
            remapFace[oldF] = newF;
            if (oldF != newF) {
                mesh.faces[newF * 4]     = mesh.faces[oldF * 4];
                mesh.faces[newF * 4 + 1] = mesh.faces[oldF * 4 + 1];
                mesh.faces[newF * 4 + 2] = mesh.faces[oldF * 4 + 2];
                mesh.faces[newF * 4 + 3] = mesh.faces[oldF * 4 + 3];

                if (!mesh.faceGroups.empty()) mesh.faceGroups[newF] = mesh.faceGroups[oldF];
                if (!mesh.faceNormals.empty()) {
                    mesh.faceNormals[newF * 3]     = mesh.faceNormals[oldF * 3];
                    mesh.faceNormals[newF * 3 + 1] = mesh.faceNormals[oldF * 3 + 1];
                    mesh.faceNormals[newF * 3 + 2] = mesh.faceNormals[oldF * 3 + 2];
                }
                if (!mesh.faceCenters.empty()) {
                    mesh.faceCenters[newF * 3]     = mesh.faceCenters[oldF * 3];
                    mesh.faceCenters[newF * 3 + 1] = mesh.faceCenters[oldF * 3 + 1];
                    mesh.faceCenters[newF * 3 + 2] = mesh.faceCenters[oldF * 3 + 2];
                }
                if (!mesh.faceBoxes.empty()) {
                    std::memcpy(&mesh.faceBoxes[newF * 6], &mesh.faceBoxes[oldF * 6], 6 * sizeof(float));
                }
                if (!mesh.faceVisible.empty()) mesh.faceVisible[newF] = mesh.faceVisible[oldF];
                if (!mesh.facesStateFlags.empty()) mesh.facesStateFlags[newF] = mesh.facesStateFlags[oldF];

                mesh.octree.replaceFace(oldF, newF);
            }
            newF++;
        }
    }

    mesh.nbFaces = newF;
    mesh.faces.resize(mesh.nbFaces * 4);
    if (!mesh.faceGroups.empty()) mesh.faceGroups.resize(mesh.nbFaces);
    if (!mesh.faceNormals.empty()) mesh.faceNormals.resize(mesh.nbFaces * 3);
    if (!mesh.faceCenters.empty()) mesh.faceCenters.resize(mesh.nbFaces * 3);
    if (!mesh.faceBoxes.empty()) mesh.faceBoxes.resize(mesh.nbFaces * 6);
    if (!mesh.faceVisible.empty()) mesh.faceVisible.resize(mesh.nbFaces);
    if (!mesh.facesStateFlags.empty()) mesh.facesStateFlags.resize(mesh.nbFaces);

    for (auto& ring : mesh.dynVRF) {
        for (size_t i = 0; i < ring.size(); ) {
            uint32_t f = ring[i];
            if (f < facesBeforeCompaction && remapFace[f] != UINT32_MAX) {
                ring[i] = remapFace[f];
                ++i;
            } else {
                ring[i] = ring.back();
                ring.pop_back();
            }
        }
    }

    sculpt_log("[DynTopo VERIFY] Decimation compaction: %u faces before -> %d after (%u tombstones cleared). activeTris remapped once.\n",
               facesBeforeCompaction, mesh.nbFaces,
               facesBeforeCompaction - mesh.nbFaces);

#ifdef DYNTOPO_HEALTH_CHECK
    validateMeshTopology(mesh, "Post-Decimation");
#endif

    std::unordered_set<uint32_t> uniqueSet;
    for (uint32_t f : activeTris) {
        if (f < facesBeforeCompaction && remapFace[f] != UINT32_MAX) {
            uniqueSet.insert(remapFace[f]);
        }
    }

    std::vector<uint32_t> result;
    result.reserve(uniqueSet.size());
    for (uint32_t f : uniqueSet) {
        if (f < static_cast<uint32_t>(mesh.nbFaces) && mesh.faces[f * 4 + 3] == TRI_INDEX) {
            result.push_back(f);
        }
    }

    mesh.isDirty = true;
    mesh.isTopologyDirty = true;
    return result;
}

} // namespace DynDecimation

