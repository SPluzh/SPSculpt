#include "mesh/Multimesh.h"
#include "editing/Subdivision.h"
#include "editing/Reversion.h"
#include "common/Logger.h"
#include <algorithm>
#include <iostream>

RenderHint Multimesh::renderHint = RenderHint::NONE;

Multimesh::Multimesh(std::unique_ptr<MeshResolution> baseMesh) {
    if (baseMesh) {
        setID(baseMesh->getID());
        outlinerName = baseMesh->outlinerName;
        matrix = baseMesh->matrix;
        editMatrix = baseMesh->editMatrix;
        center = baseMesh->center;
        scale = baseMesh->scale;
        visibleV1 = baseMesh->visibleV1;
        visibleV2 = baseMesh->visibleV2;
        sculpt_log("[Multimesh] Constructor from unique_ptr baseMesh. ID=%u, Name=%s, Verts=%d, Faces=%d\n",
                  m_id, outlinerName.c_str(), baseMesh->getNbVertices(), baseMesh->getNbFaces());
        meshes.push_back(std::move(baseMesh));
        setSelection(0);
    }
}

Multimesh::Multimesh(Mesh* mesh) {
    if (mesh) {
        setID(mesh->getID());
        outlinerName = mesh->outlinerName;
        matrix = mesh->matrix;
        editMatrix = mesh->editMatrix;
        center = mesh->center;
        scale = mesh->scale;
        visibleV1 = mesh->visibleV1;
        visibleV2 = mesh->visibleV2;
        sculpt_log("[Multimesh] Constructor from Mesh*. ID=%u, Name=%s, Verts=%d, Faces=%d\n",
                  m_id, outlinerName.c_str(), mesh->getNbVertices(), mesh->getNbFaces());

        auto resMesh = std::make_unique<MeshResolution>(*mesh, true);
        meshes.push_back(std::move(resMesh));
        setSelection(0);
    }
}

MeshResolution* Multimesh::getCurrentMesh() {
    if (meshes.empty()) return nullptr;
    if (sel < 0) sel = 0;
    if (sel >= (int)meshes.size()) sel = (int)meshes.size() - 1;
    return meshes[sel].get();
}

const MeshResolution* Multimesh::getCurrentMesh() const {
    if (meshes.empty()) return nullptr;
    int s = sel;
    if (s < 0) s = 0;
    if (s >= (int)meshes.size()) s = (int)meshes.size() - 1;
    return meshes[s].get();
}

void Multimesh::setSelection(int s) {
    sel = s;
    if (sel < 0) sel = 0;
    if (sel >= (int)meshes.size()) sel = (int)meshes.size() - 1;

    MeshResolution* current = getCurrentMesh();
    if (current) {
        verts = current->verts;
        faces = current->faces;
        normals = current->normals;
        colors = current->colors;
        materials = current->materials;
        nbVerts = current->nbVerts;
        nbFaces = current->nbFaces;
        nbEdges = current->nbEdges;
        vertVisible = current->vertVisible;
        vrfStartCount = current->vrfStartCount;
        vertRingFace = current->vertRingFace;
        vrvStartCount = current->vrvStartCount;
        vertRingVert = current->vertRingVert;
        vertOnEdge = current->vertOnEdge;
        edges = current->edges;
        faceEdges = current->faceEdges;

        faceNormals = current->faceNormals;
        faceBoxes = current->faceBoxes;
        faceCenters = current->faceCenters;

        faceGroups = current->faceGroups;
        if (faceGroups.size() != (size_t)nbFaces) {
            initFaceGroups();
        }
        isFaceGroupDirty = true;

        octree.build(nbVerts, nbFaces, faceCenters.data(), faceBoxes.data(), verts.data(), faces.data());

        sculpt_log("[Multimesh::setSelection] Selected level %d / %zu. nbVerts=%d, nbFaces=%d, nbEdges=%d\n",
                  sel, meshes.size(), nbVerts, nbFaces, nbEdges);
    }
}

MeshResolution* Multimesh::addLevel() {
    sculpt_log("[Multimesh::addLevel] Starting addLevel. Current sel: %d, total levels: %zu\n", sel, meshes.size());
    if ((int)meshes.size() - 1 != sel) {
        sculpt_log("[Multimesh::addLevel] Warning: sel (%d) is not highest level (%zu - 1)\n", sel, meshes.size());
        return getCurrentMesh();
    }

    syncToCurrentMesh();
    MeshResolution* baseMesh = getCurrentMesh();
    if (!baseMesh) {
        sculpt_log("[Multimesh::addLevel] Error: baseMesh is null!\n");
        return nullptr;
    }

    sculpt_log("[Multimesh::addLevel] Base mesh nbVerts=%d, nbFaces=%d, nbEdges=%d\n",
              baseMesh->getNbVertices(), baseMesh->getNbFaces(), baseMesh->getNbEdges());

    if (baseMesh->vrfStartCount.size() != (size_t)(baseMesh->getNbVertices() * 2) || baseMesh->edges.empty()) {
        sculpt_log("[Multimesh::addLevel] Base topology missing. Calling initTopology()...\n");
        baseMesh->initTopology();
        sculpt_log("[Multimesh::addLevel] Topology recomputed. Base nbEdges=%d\n", baseMesh->getNbEdges());
    }

    auto newMesh = std::make_unique<MeshResolution>(*baseMesh, false);
    baseMesh->clearVerticesMapping();

    sculpt_log("[Multimesh::addLevel] Executing Subdivision::fullSubdivision...\n");
    Subdivision::fullSubdivision(*baseMesh, *newMesh);
    sculpt_log("[Multimesh::addLevel] fullSubdivision complete. New nbVerts=%d, nbFaces=%d\n",
              newMesh->getNbVertices(), newMesh->getNbFaces());

    meshes.push_back(std::move(newMesh));
    setSelection((int)meshes.size() - 1);
    updateResolution();

    sculpt_log("[Multimesh::addLevel] addLevel finished successfully. Active sel: %d\n", sel);
    return getCurrentMesh();
}

MeshResolution* Multimesh::computeReverse() {
    sculpt_log("[Multimesh::computeReverse] Starting computeReverse. Current sel: %d, total levels: %zu\n", sel, meshes.size());
    if (sel != 0) {
        sculpt_log("[Multimesh::computeReverse] Warning: sel (%d) is not base level 0\n", sel);
        return getCurrentMesh();
    }

    syncToCurrentMesh();
    MeshResolution* baseMesh = getCurrentMesh();
    if (!baseMesh) {
        sculpt_log("[Multimesh::computeReverse] Error: baseMesh is null!\n");
        return nullptr;
    }

    sculpt_log("[Multimesh::computeReverse] Base mesh nbVerts=%d, nbFaces=%d\n", baseMesh->getNbVertices(), baseMesh->getNbFaces());

    if (baseMesh->vrfStartCount.size() != (size_t)(baseMesh->getNbVertices() * 2) || baseMesh->edges.empty()) {
        sculpt_log("[Multimesh::computeReverse] Base topology missing. Calling initTopology()...\n");
        baseMesh->initTopology();
    }

    auto newMesh = std::make_unique<MeshResolution>(*baseMesh, false);

    sculpt_log("[Multimesh::computeReverse] Executing Reversion::computeReverse...\n");
    bool status = Reversion::computeReverse(*baseMesh, *newMesh);
    if (!status) {
        sculpt_log("[Multimesh::computeReverse] Reversion::computeReverse failed!\n");
        return nullptr;
    }

    sculpt_log("[Multimesh::computeReverse] Reversion succeeded. Coarse nbVerts=%d, nbFaces=%d\n",
              newMesh->getNbVertices(), newMesh->getNbFaces());

    meshes.insert(meshes.begin(), std::move(newMesh));
    setSelection(1);
    lowerLevel();

    sculpt_log("[Multimesh::computeReverse] computeReverse finished successfully. Active sel: %d\n", sel);
    return getCurrentMesh();
}

void Multimesh::syncVisibility(int fromSel, int toSel) {
    if (fromSel == toSel) return;
    int step = fromSel < toSel ? 1 : -1;

    for (int k = fromSel; k != toSel; k += step) {
        MeshResolution* src = meshes[k].get();
        MeshResolution* dst = meshes[k + step].get();
        if (!src || !dst) continue;

        auto& srcVis = src->vertVisible;
        if (srcVis.empty()) continue;

        auto& dstVis = dst->vertVisible;
        int nbVertsUp = dst->getNbVertices();
        if (dstVis.size() != (size_t)nbVertsUp) {
            dstVis.assign(nbVertsUp, 1);
        }

        if (step == -1) {
            // Going down (from higher res to lower res)
            int nbVertsDown = dst->getNbVertices();
            if (!dst->getEvenMapping()) {
                for (int i = 0; i < nbVertsDown; ++i) {
                    dstVis[i] = srcVis[i];
                }
            } else {
                const auto& vertMap = dst->getVerticesMapping();
                for (int i = 0; i < nbVertsDown; ++i) {
                    if (vertMap[i] < srcVis.size())
                        dstVis[i] = srcVis[vertMap[i]];
                }
            }
        } else {
            // Going up (from lower res to higher res)
            int nbVertsDown = src->getNbVertices();
            bool evenMapping = src->getEvenMapping();
            const auto& vertMap = src->getVerticesMapping();

            std::vector<uint8_t> isParentHidden(nbVertsUp, 0);
            std::vector<uint8_t> isParent(nbVertsUp, 0);

            for (int i = 0; i < nbVertsDown; ++i) {
                uint32_t childIdx = evenMapping ? vertMap[i] : i;
                if (childIdx < (uint32_t)nbVertsUp) {
                    dstVis[childIdx] = srcVis[i];
                    isParent[childIdx] = 1;
                    if (srcVis[i] == 0) {
                        isParentHidden[childIdx] = 1;
                    }
                }
            }

            const auto& dstRing = dst->vertRingVert;
            const auto& vrvSC = dst->vrvStartCount;

            for (int j = 0; j < nbVertsUp; ++j) {
                if (isParent[j] == 0) {
                    uint32_t start = vrvSC[j * 2];
                    uint32_t count = vrvSC[j * 2 + 1];
                    for (uint32_t n = 0; n < count; ++n) {
                        uint32_t neighborIdx = dstRing[start + n];
                        if (neighborIdx < isParentHidden.size() && isParentHidden[neighborIdx] == 1) {
                            dstVis[j] = 0;
                            break;
                        }
                    }
                }
            }
        }
    }
}

void Multimesh::syncToCurrentMesh() {
    MeshResolution* current = getCurrentMesh();
    if (current) {
        current->verts = verts;
        current->colors = colors;
        current->materials = materials;
        current->normals = normals;
        current->vertVisible = vertVisible;
        current->faceGroups = faceGroups;
        current->isFaceGroupDirty = isFaceGroupDirty;
    }
}

MeshResolution* Multimesh::lowerLevel() {
    if (sel == 0)
        return meshes[0].get();

    syncToCurrentMesh();
    syncVisibility(sel, sel - 1);
    MeshResolution* cur = getCurrentMesh();
    MeshResolution* prev = meshes[sel - 1].get();
    meshes[sel - 1]->lowerAnalysis(*cur);

    if (cur && prev && cur->faceGroups.size() == (size_t)cur->nbFaces) {
        prev->faceGroups.resize(prev->nbFaces);
        for (int i = 0; i < prev->nbFaces; ++i) {
            if (i * 4 + 3 < cur->nbFaces) {
                uint32_t g0 = cur->faceGroups[i * 4 + 0];
                uint32_t g1 = cur->faceGroups[i * 4 + 1];
                uint32_t g2 = cur->faceGroups[i * 4 + 2];
                uint32_t g3 = cur->faceGroups[i * 4 + 3];

                uint32_t majority = g0;
                if (g1 == g2 || g1 == g3) majority = g1;
                else if (g2 == g3) majority = g2;
                prev->faceGroups[i] = majority;
            }
        }
        prev->isFaceGroupDirty = true;
    }

    setSelection(sel - 1);
    updateResolution();

    return getCurrentMesh();
}

MeshResolution* Multimesh::higherLevel() {
    if (sel == (int)meshes.size() - 1)
        return getCurrentMesh();

    syncToCurrentMesh();
    syncVisibility(sel, sel + 1);
    MeshResolution* cur = getCurrentMesh();
    MeshResolution* next = meshes[sel + 1].get();
    meshes[sel + 1]->higherSynthesis(*cur);

    if (cur && next && cur->faceGroups.size() == (size_t)cur->nbFaces) {
        next->faceGroups.resize(next->nbFaces);
        for (int i = 0; i < cur->nbFaces; ++i) {
            uint32_t gid = cur->faceGroups[i];
            if (i * 4 + 3 < next->nbFaces) {
                next->faceGroups[i * 4 + 0] = gid;
                next->faceGroups[i * 4 + 1] = gid;
                next->faceGroups[i * 4 + 2] = gid;
                next->faceGroups[i * 4 + 3] = gid;
            }
        }
        next->isFaceGroupDirty = true;
    }

    setSelection(sel + 1);
    updateResolution();

    return getCurrentMesh();
}

void Multimesh::updateResolution() {
    MeshResolution* cur = getCurrentMesh();
    if (cur) {
        cur->postInit();
        setSelection(sel);
    }
}

void Multimesh::selectResolution(int targetSel) {
    while (sel > targetSel && sel > 0) {
        lowerLevel();
    }
    while (sel < targetSel && sel < (int)meshes.size() - 1) {
        higherLevel();
    }
}

void Multimesh::deleteLower() {
    if (sel <= 0) return;
    syncToCurrentMesh();
    meshes.erase(meshes.begin(), meshes.begin() + sel);
    setSelection(0);
}

void Multimesh::deleteHigher() {
    if (sel >= (int)meshes.size() - 1) return;
    syncToCurrentMesh();
    meshes.erase(meshes.begin() + sel + 1, meshes.end());
}

int Multimesh::getLowIndexRender() const {
    int limit = 500000;
    int s = sel;
    while (s >= 0) {
        const MeshResolution* mesh = meshes[s].get();
        if (mesh->getEvenMapping())
            return (s == sel) ? s : s + 1;
        if (mesh->getNbTriangles() < limit)
            return s;
        --s;
    }
    return 0;
}

void Multimesh::flip(int axisIndex) {
    Mesh::flip(axisIndex);
    meshes.clear();
    meshes.push_back(std::make_unique<MeshResolution>(*this, true));
    setSelection(0);
}

void Multimesh::mirror(int axisIndex, bool positiveToNegative, SymmetryMode mode) {
    Mesh::mirror(axisIndex, positiveToNegative, mode);
    meshes.clear();
    meshes.push_back(std::make_unique<MeshResolution>(*this, true));
    setSelection(0);
}

