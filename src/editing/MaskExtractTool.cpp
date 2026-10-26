#include "editing/MaskExtractTool.h"
#include "editing/SculptManager.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "gui/GuiManager.h"
#include "mesh/Mesh.h"
#include "mesh/NormalCalc.h"
#include "common/Constants.h"
#include "common/Logger.h"
#include "gui/IconsLucide.h"
#include <SDL2/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include "CDT.h"

static bool isMouseOverAnyImGuiWindow() {
    ImGuiContext* g = GImGui;
    if (!g) return false;
    ImVec2 mpos = ImGui::GetMousePos();
    for (int i = 0; i < g->Windows.Size; i++) {
        ImGuiWindow* window = g->Windows[i];
        if (window->Active && window->WasActive && !window->Hidden) {
            if (!(window->Flags & ImGuiWindowFlags_NoMouseInputs)) {
                if (window->Rect().Contains(mpos)) {
                    return true;
                }
            }
        }
    }
    return false;
}

MaskExtractTool::MaskExtractTool(SculptManager& sm)
    : m_sculpt(sm) {
}

MaskExtractTool::~MaskExtractTool() {
    // Safety cleanup in destructor if preview was somehow left uncommitted
}

bool MaskExtractTool::extractRawPatch(const Mesh& baseMesh) {
    if (baseMesh.nbVerts <= 0 || baseMesh.nbFaces <= 0) return false;
    if (baseMesh.materials.size() < (size_t)baseMesh.nbVerts * 3) return false;

    const float* mats = baseMesh.materials.data();
    const float* verts = baseMesh.verts.data();
    const float* norms = baseMesh.normals.data();
    const float* cols = baseMesh.colors.data();
    bool hasColors = !baseMesh.colors.empty() && baseMesh.colors.size() == (size_t)baseMesh.nbVerts * 3;
    bool hasNormals = !baseMesh.normals.empty() && baseMesh.normals.size() == (size_t)baseMesh.nbVerts * 3;

    int nbFaces = baseMesh.nbFaces;
    const uint32_t* fData = baseMesh.faces.data();

    std::vector<int> selectedFaces;
    selectedFaces.reserve(nbFaces / 4);

    float threshold = m_params.maskThreshold;

    for (int f = 0; f < nbFaces; ++f) {
        uint32_t v0 = fData[f * 4 + 0];
        uint32_t v1 = fData[f * 4 + 1];
        uint32_t v2 = fData[f * 4 + 2];
        uint32_t v3 = fData[f * 4 + 3];

        if (v0 >= (uint32_t)baseMesh.nbVerts || v1 >= (uint32_t)baseMesh.nbVerts || v2 >= (uint32_t)baseMesh.nbVerts) continue;

        // Mask weight in sculptsp: 0.0 = unmasked (materials=1.0), 1.0 = fully masked (materials=0.0)
        float w0 = 1.0f - mats[v0 * 3 + 2];
        float w1 = 1.0f - mats[v1 * 3 + 2];
        float w2 = 1.0f - mats[v2 * 3 + 2];

        bool isTri = (v3 == TRI_INDEX || v3 >= (uint32_t)baseMesh.nbVerts);
        float avgWeight = 0.0f;
        if (isTri) {
            avgWeight = (w0 + w1 + w2) / 3.0f;
        } else {
            float w3 = 1.0f - mats[v3 * 3 + 2];
            avgWeight = (w0 + w1 + w2 + w3) / 4.0f;
        }

        if (avgWeight >= threshold) {
            selectedFaces.push_back(f);
        }
    }

    if (selectedFaces.empty()) return false;

    // Collect unique vertices and build old-to-new mapping
    std::vector<int32_t> baseToPatch(baseMesh.nbVerts, -1);
    m_patchToBaseVertMap.clear();
    m_patchBaseNormals.clear();

    for (int f : selectedFaces) {
        for (int k = 0; k < 4; ++k) {
            uint32_t vid = fData[f * 4 + k];
            if (vid != TRI_INDEX && vid < (uint32_t)baseMesh.nbVerts) {
                if (baseToPatch[vid] == -1) {
                    baseToPatch[vid] = (int32_t)m_patchToBaseVertMap.size();
                    m_patchToBaseVertMap.push_back(vid);
                    if (hasNormals) {
                        m_patchBaseNormals.push_back(glm::vec3(norms[vid * 3], norms[vid * 3 + 1], norms[vid * 3 + 2]));
                    } else {
                        m_patchBaseNormals.push_back(glm::vec3(0.0f, 1.0f, 0.0f));
                    }
                }
            }
        }
    }

    int pNbVerts = (int)m_patchToBaseVertMap.size();
    int pNbFaces = (int)selectedFaces.size();

    m_rawPatchMesh.nbVerts = pNbVerts;
    m_rawPatchMesh.nbFaces = pNbFaces;
    m_rawPatchMesh.verts.resize(pNbVerts * 3);
    m_rawPatchMesh.normals.resize(pNbVerts * 3, 0.0f);
    m_rawPatchMesh.colors.resize(pNbVerts * 3, 1.0f);
    m_rawPatchMesh.materials.resize(pNbVerts * 3);
    m_rawPatchMesh.faces.resize(pNbFaces * 4);
    m_rawPatchMesh.faceGroups.resize(pNbFaces, 0);

    for (int i = 0; i < pNbVerts; ++i) {
        uint32_t bVid = m_patchToBaseVertMap[i];
        m_rawPatchMesh.verts[i * 3 + 0] = verts[bVid * 3 + 0];
        m_rawPatchMesh.verts[i * 3 + 1] = verts[bVid * 3 + 1];
        m_rawPatchMesh.verts[i * 3 + 2] = verts[bVid * 3 + 2];

        if (hasNormals) {
            m_rawPatchMesh.normals[i * 3 + 0] = norms[bVid * 3 + 0];
            m_rawPatchMesh.normals[i * 3 + 1] = norms[bVid * 3 + 1];
            m_rawPatchMesh.normals[i * 3 + 2] = norms[bVid * 3 + 2];
        }
        if (hasColors) {
            m_rawPatchMesh.colors[i * 3 + 0] = cols[bVid * 3 + 0];
            m_rawPatchMesh.colors[i * 3 + 1] = cols[bVid * 3 + 1];
            m_rawPatchMesh.colors[i * 3 + 2] = cols[bVid * 3 + 2];
        }
        m_rawPatchMesh.materials[i * 3 + 0] = 0.5f; // roughness
        m_rawPatchMesh.materials[i * 3 + 1] = 0.0f; // metalness
        m_rawPatchMesh.materials[i * 3 + 2] = 1.0f; // clean mask on extracted mesh
    }

    for (int i = 0; i < pNbFaces; ++i) {
        int baseF = selectedFaces[i];
        uint32_t v0 = fData[baseF * 4 + 0];
        uint32_t v1 = fData[baseF * 4 + 1];
        uint32_t v2 = fData[baseF * 4 + 2];
        uint32_t v3 = fData[baseF * 4 + 3];

        m_rawPatchMesh.faces[i * 4 + 0] = baseToPatch[v0];
        m_rawPatchMesh.faces[i * 4 + 1] = baseToPatch[v1];
        m_rawPatchMesh.faces[i * 4 + 2] = baseToPatch[v2];
        m_rawPatchMesh.faces[i * 4 + 3] = (v3 == TRI_INDEX) ? TRI_INDEX : baseToPatch[v3];

        m_rawPatchMesh.faceGroups[i] = (baseF < (int)baseMesh.faceGroups.size()) ? baseMesh.faceGroups[baseF] : 0;
    }

    m_rawPatchMesh.matrix = baseMesh.matrix;
    m_rawPatchMesh.editMatrix = baseMesh.editMatrix;
    m_rawPatchMesh.center = baseMesh.center;
    m_rawPatchMesh.scale = baseMesh.scale;

    m_rawPatchMesh.initTopology();
    m_rawPatchMesh.initEdges();

    return true;
}

void MaskExtractTool::applySmoothing() {
    m_smoothPatchMesh.copyDataFrom(m_rawPatchMesh);
    m_smoothPatchMesh.matrix = m_rawPatchMesh.matrix;
    m_smoothPatchMesh.editMatrix = m_rawPatchMesh.editMatrix;
    m_smoothPatchMesh.center = m_rawPatchMesh.center;
    m_smoothPatchMesh.scale = m_rawPatchMesh.scale;
    m_smoothPatchMesh.hasCustomNormals = false;

    int iters = m_params.smoothIterations;
    if (iters <= 0) {
        m_smoothPatchMesh.postInit(false);
        m_smoothPatchMesh.initEdges();
        return;
    }

    int N_v = m_smoothPatchMesh.nbVerts;
    int N_f = m_smoothPatchMesh.nbFaces;
    if (N_v <= 0 || N_f <= 0) return;

    // Ensure topology and edges are initialized
    if (m_smoothPatchMesh.vrvStartCount.size() < (size_t)(N_v * 2) || m_smoothPatchMesh.vertRingVert.empty()) {
        m_smoothPatchMesh.initTopology();
    }
    m_smoothPatchMesh.initEdges();

    const uint32_t* fData = m_smoothPatchMesh.faces.data();
    const uint32_t* feData = m_smoothPatchMesh.faceEdges.data();
    const uint32_t* eData = m_smoothPatchMesh.edges.data();
    size_t edgeCountTotal = m_smoothPatchMesh.edges.size();

    std::vector<std::vector<uint32_t>> boundaryNeighbors(N_v);
    std::vector<uint8_t> isBoundary(N_v, 0);

    for (int f = 0; f < N_f; ++f) {
        uint32_t v0 = fData[f * 4 + 0];
        uint32_t v1 = fData[f * 4 + 1];
        uint32_t v2 = fData[f * 4 + 2];
        uint32_t v3 = fData[f * 4 + 3];
        int numEdges = (v3 == TRI_INDEX) ? 3 : 4;

        for (int k = 0; k < numEdges; ++k) {
            uint32_t eIdx = feData[f * 4 + k];
            if (eIdx < edgeCountTotal && eData[eIdx] == 1) {
                uint32_t va = 0, vb = 0;
                if (k == 0)      { va = v0; vb = v1; }
                else if (k == 1) { va = v1; vb = v2; }
                else if (k == 2) { va = v2; vb = (v3 == TRI_INDEX ? v0 : v3); }
                else if (k == 3) { va = v3; vb = v0; }

                boundaryNeighbors[va].push_back(vb);
                boundaryNeighbors[vb].push_back(va);
                isBoundary[va] = 1;
                isBoundary[vb] = 1;
            }
        }
    }

    // Deduplicate boundary neighbors
    for (int i = 0; i < N_v; ++i) {
        if (!boundaryNeighbors[i].empty()) {
            std::sort(boundaryNeighbors[i].begin(), boundaryNeighbors[i].end());
            boundaryNeighbors[i].erase(std::unique(boundaryNeighbors[i].begin(), boundaryNeighbors[i].end()), boundaryNeighbors[i].end());
        }
    }

    const uint32_t* vrvStartCount = m_smoothPatchMesh.vrvStartCount.data();
    const uint32_t* vertRingVert = m_smoothPatchMesh.vertRingVert.data();

    // Mark 1-ring interior vertices adjacent to boundary for cushioning
    std::vector<uint8_t> isNearBoundary(N_v, 0);
    for (int i = 0; i < N_v; ++i) {
        if (isBoundary[i]) {
            uint32_t start = vrvStartCount[i * 2];
            uint32_t count = vrvStartCount[i * 2 + 1];
            for (uint32_t j = 0; j < count; ++j) {
                uint32_t nb = vertRingVert[start + j];
                if (!isBoundary[nb]) {
                    isNearBoundary[nb] = 1;
                }
            }
        }
    }

    float* vData = m_smoothPatchMesh.verts.data();
    std::vector<glm::vec3> tempVerts(N_v);

    for (int it = 0; it < iters; ++it) {
        for (int i = 0; i < N_v; ++i) {
            glm::vec3 cur(vData[i * 3], vData[i * 3 + 1], vData[i * 3 + 2]);
            glm::vec3 norm = (i < (int)m_patchBaseNormals.size()) ? m_patchBaseNormals[i] : glm::vec3(0.0f, 1.0f, 0.0f);
            float nLen = glm::length(norm);
            if (nLen > 1e-5f) norm /= nLen;

            if (isBoundary[i] && !boundaryNeighbors[i].empty()) {
                // Perimeter curve smoothing (along the 1D border curve ONLY)
                // NEVER pulled by interior neighbors -> strictly prevents border shrinkage in both modes!
                glm::vec3 bSum(0.0f);
                for (uint32_t nb : boundaryNeighbors[i]) {
                    bSum += glm::vec3(vData[nb * 3], vData[nb * 3 + 1], vData[nb * 3 + 2]);
                }
                glm::vec3 bAvg = bSum / (float)boundaryNeighbors[i].size();
                glm::vec3 disp = bAvg - cur;
                glm::vec3 tanDisp = disp - glm::dot(disp, norm) * norm;

                uint32_t start = vrvStartCount[i * 2];
                uint32_t count = vrvStartCount[i * 2 + 1];
                float minEdgeLen = 1e6f;
                for (uint32_t j = 0; j < count; ++j) {
                    uint32_t nb = vertRingVert[start + j];
                    float d = glm::distance(cur, glm::vec3(vData[nb * 3], vData[nb * 3 + 1], vData[nb * 3 + 2]));
                    if (d > 1e-6f && d < minEdgeLen) minEdgeLen = d;
                }
                if (minEdgeLen > 1e5f) minEdgeLen = 0.05f;

                float dLen = glm::length(tanDisp);
                float maxD = minEdgeLen * 0.35f;
                if (dLen > maxD && dLen > 1e-6f) {
                    tanDisp = tanDisp * (maxD / dLen);
                }

                tempVerts[i] = cur + 0.35f * tanDisp;
            } else if (isNearBoundary[i] && m_params.smoothMode == ExtractSmoothMode::BorderOnly) {
                // 1-ring interior cushion for BorderOnly to avoid quad distortion
                uint32_t start = vrvStartCount[i * 2];
                uint32_t count = vrvStartCount[i * 2 + 1];
                if (count > 0) {
                    glm::vec3 sum(0.0f);
                    for (uint32_t j = 0; j < count; ++j) {
                        uint32_t nb = vertRingVert[start + j];
                        sum += glm::vec3(vData[nb * 3], vData[nb * 3 + 1], vData[nb * 3 + 2]);
                    }
                    glm::vec3 avg = sum / (float)count;
                    glm::vec3 disp = avg - cur;
                    glm::vec3 tanDisp = disp - glm::dot(disp, norm) * norm;
                    tempVerts[i] = cur + 0.15f * tanDisp;
                } else {
                    tempVerts[i] = cur;
                }
            } else if (m_params.smoothMode == ExtractSmoothMode::All) {
                // Surface interior smoothing for All mode (projected tangentially onto base surface)
                uint32_t start = vrvStartCount[i * 2];
                uint32_t count = vrvStartCount[i * 2 + 1];
                if (count > 0) {
                    glm::vec3 sum(0.0f);
                    for (uint32_t j = 0; j < count; ++j) {
                        uint32_t nb = vertRingVert[start + j];
                        sum += glm::vec3(vData[nb * 3], vData[nb * 3 + 1], vData[nb * 3 + 2]);
                    }
                    glm::vec3 avg = sum / (float)count;
                    glm::vec3 disp = avg - cur;
                    glm::vec3 tanDisp = disp - glm::dot(disp, norm) * norm;
                    tempVerts[i] = cur + 0.35f * tanDisp;
                } else {
                    tempVerts[i] = cur;
                }
            } else {
                // Mode == BorderOnly: interior sculpt details remain 100% frozen
                tempVerts[i] = cur;
            }
        }

        for (int i = 0; i < N_v; ++i) {
            vData[i * 3 + 0] = tempVerts[i].x;
            vData[i * 3 + 1] = tempVerts[i].y;
            vData[i * 3 + 2] = tempVerts[i].z;
        }
    }

    m_smoothPatchMesh.hasCustomNormals = false;
    m_smoothPatchMesh.postInit(false);
    m_smoothPatchMesh.initEdges();
}

void MaskExtractTool::smoothSolidMesh(Mesh& mesh, int iters) {
    if (iters <= 0) return;
    int N_v = mesh.nbVerts;
    if (N_v <= 0) return;

    if (mesh.vrvStartCount.size() < (size_t)(N_v * 2) || mesh.vertRingVert.empty()) {
        mesh.initTopology();
    }

    const uint32_t* vrvStartCount = mesh.vrvStartCount.data();
    const uint32_t* vertRingVert = mesh.vertRingVert.data();
    float* vData = mesh.verts.data();

    std::vector<glm::vec3> tempP(N_v);

    for (int it = 0; it < iters; ++it) {
        // Taubin Pass 1: Shrink / low-pass filter step (lambda = +0.50f)
        #pragma omp parallel for schedule(static) if(N_v > 1000)
        for (int i = 0; i < N_v; ++i) {
            uint32_t start = vrvStartCount[i * 2];
            uint32_t count = vrvStartCount[i * 2 + 1];
            if (count == 0) {
                tempP[i] = glm::vec3(vData[i * 3], vData[i * 3 + 1], vData[i * 3 + 2]);
                continue;
            }

            glm::vec3 cur(vData[i * 3], vData[i * 3 + 1], vData[i * 3 + 2]);
            glm::vec3 sum(0.0f);
            for (uint32_t j = 0; j < count; ++j) {
                uint32_t nb = vertRingVert[start + j];
                sum += glm::vec3(vData[nb * 3], vData[nb * 3 + 1], vData[nb * 3 + 2]);
            }
            glm::vec3 avg = sum / (float)count;
            glm::vec3 disp = avg - cur;

            tempP[i] = cur + 0.50f * disp;
        }

        for (int i = 0; i < N_v; ++i) {
            vData[i * 3 + 0] = tempP[i].x;
            vData[i * 3 + 1] = tempP[i].y;
            vData[i * 3 + 2] = tempP[i].z;
        }

        // Taubin Pass 2: Inflate / volume-preservation step (mu = -0.52f)
        #pragma omp parallel for schedule(static) if(N_v > 1000)
        for (int i = 0; i < N_v; ++i) {
            uint32_t start = vrvStartCount[i * 2];
            uint32_t count = vrvStartCount[i * 2 + 1];
            if (count == 0) {
                tempP[i] = glm::vec3(vData[i * 3], vData[i * 3 + 1], vData[i * 3 + 2]);
                continue;
            }

            glm::vec3 cur(vData[i * 3], vData[i * 3 + 1], vData[i * 3 + 2]);
            glm::vec3 sum(0.0f);
            for (uint32_t j = 0; j < count; ++j) {
                uint32_t nb = vertRingVert[start + j];
                sum += glm::vec3(vData[nb * 3], vData[nb * 3 + 1], vData[nb * 3 + 2]);
            }
            glm::vec3 avg = sum / (float)count;
            glm::vec3 disp = avg - cur;

            tempP[i] = cur - 0.52f * disp;
        }

        for (int i = 0; i < N_v; ++i) {
            vData[i * 3 + 0] = tempP[i].x;
            vData[i * 3 + 1] = tempP[i].y;
            vData[i * 3 + 2] = tempP[i].z;
        }
    }

    updateFaceNormalsOnly(
        mesh.verts.data(), mesh.nbVerts,
        mesh.faces.data(), mesh.nbFaces,
        mesh.faceNormals.data()
    );
    updateVertexNormals(
        nullptr, -1, mesh.nbVerts,
        mesh.vrfStartCount.data(),
        mesh.vertRingFace.data(),
        mesh.faceNormals.data(),
        mesh.normals.data()
    );
}

void MaskExtractTool::computeAnchorAndNormal() {
    int N_v = m_smoothPatchMesh.nbVerts;
    if (N_v <= 0 || !m_baseMesh) return;

    const float* vData = m_smoothPatchMesh.verts.data();
    glm::vec3 centroid(0.0f);
    glm::vec3 avgNorm(0.0f);

    for (int i = 0; i < N_v; ++i) {
        centroid += glm::vec3(vData[i * 3], vData[i * 3 + 1], vData[i * 3 + 2]);
        if (i < (int)m_patchBaseNormals.size()) {
            avgNorm += m_patchBaseNormals[i];
        }
    }
    centroid /= (float)N_v;
    float nLen = glm::length(avgNorm);
    avgNorm = (nLen > 1e-5f) ? (avgNorm / nLen) : glm::vec3(0.0f, 1.0f, 0.0f);

    // Find vertex closest to centroid to serve as surface anchor
    int closestIdx = 0;
    float minD2 = 1e30f;
    for (int i = 0; i < N_v; ++i) {
        glm::vec3 p(vData[i * 3], vData[i * 3 + 1], vData[i * 3 + 2]);
        glm::vec3 diff = p - centroid;
        float d2 = glm::dot(diff, diff);
        if (d2 < minD2) {
            minD2 = d2;
            closestIdx = i;
        }
    }

    glm::vec3 anchorLocal(vData[closestIdx * 3], vData[closestIdx * 3 + 1], vData[closestIdx * 3 + 2]);
    glm::vec3 normalLocal = (closestIdx < (int)m_patchBaseNormals.size()) ? m_patchBaseNormals[closestIdx] : avgNorm;

    glm::mat4 worldMat = m_baseMesh->matrix * m_baseMesh->editMatrix;
    m_anchorWorld = glm::vec3(worldMat * glm::vec4(anchorLocal, 1.0f));
    m_normalWorld = glm::normalize(glm::mat3(worldMat) * normalLocal);

    float dHandle = m_params.thickness * (1.0f + m_params.offset) * 0.5f;
    m_handlePosWorld = m_anchorWorld + m_normalWorld * dHandle;
}

void MaskExtractTool::buildShellTopology() {
    Thickness::ThicknessParams tp;
    tp.thickness = m_params.thickness;
    tp.offset = m_params.offset;
    tp.rimSegments = m_params.rimSegments;
    tp.creaseRim = (m_params.smoothMode == ExtractSmoothMode::BorderOnly) && m_params.creaseRim;
    tp.generateGroups = true;
    tp.evenThickness = true;

    Thickness::generateThickness(m_smoothPatchMesh, *m_previewMesh, tp, &m_thickCache);
}

void MaskExtractTool::buildNoneTopology() {
    m_previewMesh->copyDataFrom(m_smoothPatchMesh);
    m_previewMesh->matrix = m_baseMesh->matrix;
    m_previewMesh->editMatrix = m_baseMesh->editMatrix;
    m_previewMesh->center = m_baseMesh->center;
    m_previewMesh->scale = m_baseMesh->scale;

    float dFront = m_params.thickness * (1.0f + m_params.offset) * 0.5f;
    int N_v = m_previewMesh->nbVerts;
    float* vData = m_previewMesh->verts.data();

    if (std::abs(dFront) > 1e-6f) {
        for (int i = 0; i < N_v; ++i) {
            glm::vec3 norm = (i < (int)m_patchBaseNormals.size()) ? m_patchBaseNormals[i] : glm::vec3(0.0f, 1.0f, 0.0f);
            float nLen = glm::length(norm);
            if (nLen > 1e-5f) norm /= nLen;
            vData[i * 3 + 0] += norm.x * dFront;
            vData[i * 3 + 1] += norm.y * dFront;
            vData[i * 3 + 2] += norm.z * dFront;
        }
    }

    m_previewMesh->hasCustomNormals = false;
    m_previewMesh->postInit(false);
    m_previewMesh->initEdges();
}

void MaskExtractTool::buildFillTopology() {
    int N_v = m_smoothPatchMesh.nbVerts;
    int N_f = m_smoothPatchMesh.nbFaces;
    if (N_v <= 0 || N_f <= 0) return;

    if (m_smoothPatchMesh.vrvStartCount.size() < (size_t)(N_v * 2) || m_smoothPatchMesh.vertRingVert.empty()) {
        m_smoothPatchMesh.initTopology();
    }
    m_smoothPatchMesh.initEdges();

    const uint32_t* fData = m_smoothPatchMesh.faces.data();
    const uint32_t* feData = m_smoothPatchMesh.faceEdges.data();
    const uint32_t* eData = m_smoothPatchMesh.edges.data();
    size_t edgeCountTotal = m_smoothPatchMesh.edges.size();

    struct BEdge { uint32_t va, vb, faceIdx, localEdge; };
    std::vector<BEdge> boundaryEdges;
    for (int f = 0; f < N_f; ++f) {
        uint32_t v0 = fData[f * 4 + 0];
        uint32_t v1 = fData[f * 4 + 1];
        uint32_t v2 = fData[f * 4 + 2];
        uint32_t v3 = fData[f * 4 + 3];
        int numEdges = (v3 == TRI_INDEX) ? 3 : 4;
        for (int k = 0; k < numEdges; ++k) {
            uint32_t eIdx = feData[f * 4 + k];
            if (eIdx < edgeCountTotal && eData[eIdx] == 1) {
                uint32_t va = 0, vb = 0;
                if (k == 0)      { va = v0; vb = v1; }
                else if (k == 1) { va = v1; vb = v2; }
                else if (k == 2) { va = v2; vb = (v3 == TRI_INDEX ? v0 : v3); }
                else if (k == 3) { va = v3; vb = v0; }
                boundaryEdges.push_back({va, vb, (uint32_t)f, (uint32_t)k});
            }
        }
    }

    if (boundaryEdges.empty()) {
        m_previewMesh->copyDataFrom(m_smoothPatchMesh);
        m_previewMesh->postInit(false);
        m_previewMesh->initEdges();
        return;
    }

    // Chain boundary edges into ordered loops
    std::unordered_map<uint32_t, uint32_t> nextVert;
    for (const auto& be : boundaryEdges) {
        nextVert[be.va] = be.vb;
    }

    std::unordered_set<uint32_t> visited;
    std::vector<std::vector<uint32_t>> loops;
    for (const auto& be : boundaryEdges) {
        if (visited.count(be.va)) continue;
        std::vector<uint32_t> loop;
        uint32_t cur = be.va;
        while (!visited.count(cur)) {
            visited.insert(cur);
            loop.push_back(cur);
            auto it = nextVert.find(cur);
            if (it == nextVert.end()) break;
            cur = it->second;
        }
        if (loop.size() >= 3) {
            loops.push_back(std::move(loop));
        }
    }

    // Direct positions from base patch
    const float* bVerts = m_smoothPatchMesh.verts.data();
    std::vector<glm::vec3> outPositions(N_v);
    for (int i = 0; i < N_v; ++i) {
        outPositions[i] = glm::vec3(bVerts[i * 3], bVerts[i * 3 + 1], bVerts[i * 3 + 2]);
    }

    struct Face4 { uint32_t v[4]; uint32_t gid; };
    std::vector<Face4> outFaces;
    outFaces.reserve(N_f + boundaryEdges.size());

    // 1. Front faces
    uint32_t maxGid = 0;
    for (int f = 0; f < N_f; ++f) {
        uint32_t gid = (f < (int)m_smoothPatchMesh.faceGroups.size()) ? m_smoothPatchMesh.faceGroups[f] : 0;
        if (gid > maxGid) maxGid = gid;
        outFaces.push_back({
            fData[f * 4 + 0],
            fData[f * 4 + 1],
            fData[f * 4 + 2],
            fData[f * 4 + 3],
            gid
        });
    }

    uint32_t capGid = maxGid + 1;

    // 2. Direct Cap faces closing the boundary hole immediately
    for (const auto& loop : loops) {
        size_t loopN = loop.size();
        if (loopN < 3) continue;

        glm::vec3 C(0.0f);
        for (uint32_t v : loop) {
            C += outPositions[v];
        }
        C /= static_cast<float>(loopN);

        glm::vec3 N(0.0f);
        for (size_t i = 0; i < loopN; ++i) {
            glm::vec3 p1 = outPositions[loop[i]];
            glm::vec3 p2 = outPositions[loop[(i + 1) % loopN]];
            N.x += (p1.y - p2.y) * (p1.z + p2.z);
            N.y += (p1.z - p2.z) * (p1.x + p2.x);
            N.z += (p1.x - p2.x) * (p1.y + p2.y);
        }
        float nLen = glm::length(N);
        if (nLen > 1e-6f) N /= nLen;
        else N = glm::vec3(0.0f, 1.0f, 0.0f);

        glm::vec3 up = (std::abs(N.y) < 0.9f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        glm::vec3 U = glm::normalize(glm::cross(N, up));
        glm::vec3 V = glm::cross(N, U);

        std::vector<CDT::V2d<float>> cdtVerts(loopN);
        for (size_t i = 0; i < loopN; ++i) {
            glm::vec3 diff = outPositions[loop[i]] - C;
            cdtVerts[i] = CDT::V2d<float>::make(glm::dot(diff, U), glm::dot(diff, V));
        }

        float area2D = 0.0f;
        for (size_t i = 0; i < loopN; ++i) {
            size_t next = (i + 1) % loopN;
            area2D += (cdtVerts[i].x * cdtVerts[next].y - cdtVerts[next].x * cdtVerts[i].y);
        }
        bool flipped = false;
        if (area2D < 0.0f) {
            flipped = true;
            for (size_t i = 0; i < loopN; ++i) {
                cdtVerts[i].x = -cdtVerts[i].x;
            }
        }

        bool cdtSuccess = false;
        CDT::Triangulation<float> cdt(CDT::VertexInsertionOrder::Auto, CDT::IntersectingConstraintEdges::TryResolve, 1e-4f);
        try {
            cdt.insertVertices(cdtVerts);
            std::vector<CDT::Edge> edges;
            edges.reserve(loopN);
            for (size_t i = 0; i < loopN; ++i) {
                edges.push_back(CDT::Edge(static_cast<CDT::VertInd>(i), static_cast<CDT::VertInd>((i + 1) % loopN)));
            }
            cdt.insertEdges(edges);
            cdt.eraseOuterTrianglesAndHoles();
            cdtSuccess = !cdt.triangles.empty();
        } catch (...) {
            cdtSuccess = false;
        }

        if (cdtSuccess) {
            std::vector<uint32_t> cdtVertToOutIdx(cdt.vertices.size());
            for (size_t i = 0; i < loopN; ++i) {
                cdtVertToOutIdx[i] = loop[i];
            }
            for (size_t i = loopN; i < cdt.vertices.size(); ++i) {
                float x2d = flipped ? -cdt.vertices[i].x : cdt.vertices[i].x;
                float y2d = cdt.vertices[i].y;
                glm::vec3 p3d = C + U * x2d + V * y2d;
                cdtVertToOutIdx[i] = static_cast<uint32_t>(outPositions.size());
                outPositions.push_back(p3d);
            }

            for (const auto& tri : cdt.triangles) {
                uint32_t v0 = cdtVertToOutIdx[tri.vertices[0]];
                uint32_t v1, v2;
                if (!flipped) {
                    v1 = cdtVertToOutIdx[tri.vertices[2]]; // swapped for outward normal
                    v2 = cdtVertToOutIdx[tri.vertices[1]];
                } else {
                    v1 = cdtVertToOutIdx[tri.vertices[1]];
                    v2 = cdtVertToOutIdx[tri.vertices[2]];
                }
                outFaces.push_back({v0, v1, v2, TRI_INDEX, capGid});
            }
        } else {
            // Triangle fan fallback directly across boundary loop to centroid
            uint32_t centerIdx = static_cast<uint32_t>(outPositions.size());
            outPositions.push_back(C);
            for (size_t i = 0; i < loopN; ++i) {
                uint32_t v0 = centerIdx;
                uint32_t v1 = loop[(i + 1) % loopN];
                uint32_t v2 = loop[i];
                outFaces.push_back({v0, v1, v2, TRI_INDEX, capGid});
            }
        }
    }

    int finalNbVerts = static_cast<int>(outPositions.size());
    int finalNbFaces = static_cast<int>(outFaces.size());

    m_previewMesh->nbVerts = finalNbVerts;
    m_previewMesh->nbFaces = finalNbFaces;
    m_previewMesh->verts.resize(finalNbVerts * 3);
    m_previewMesh->normals.resize(finalNbVerts * 3, 0.0f);
    m_previewMesh->colors.resize(finalNbVerts * 3, 1.0f);
    m_previewMesh->materials.resize(finalNbVerts * 3);
    for (int i = 0; i < finalNbVerts; ++i) {
        m_previewMesh->materials[i * 3 + 0] = 0.5f;
        m_previewMesh->materials[i * 3 + 1] = 0.0f;
        m_previewMesh->materials[i * 3 + 2] = 1.0f;
    }
    m_previewMesh->faces.resize(finalNbFaces * 4);
    m_previewMesh->faceGroups.resize(finalNbFaces);

    m_fillBasePositions = outPositions;
    m_fillNormals.resize(finalNbVerts);
    for (int i = 0; i < finalNbVerts; ++i) {
        if (i < N_v && i < (int)m_patchBaseNormals.size()) {
            m_fillNormals[i] = m_patchBaseNormals[i];
        } else {
            m_fillNormals[i] = m_normalWorld;
        }
        float nLen = glm::length(m_fillNormals[i]);
        if (nLen > 1e-5f) m_fillNormals[i] /= nLen;
    }

    float dFront = m_params.thickness * (1.0f + m_params.offset) * 0.5f;
    for (int i = 0; i < finalNbVerts; ++i) {
        glm::vec3 p = outPositions[i] + m_fillNormals[i] * dFront;
        m_previewMesh->verts[i * 3 + 0] = p.x;
        m_previewMesh->verts[i * 3 + 1] = p.y;
        m_previewMesh->verts[i * 3 + 2] = p.z;
    }
    for (int f = 0; f < finalNbFaces; ++f) {
        m_previewMesh->faces[f * 4 + 0] = outFaces[f].v[0];
        m_previewMesh->faces[f * 4 + 1] = outFaces[f].v[1];
        m_previewMesh->faces[f * 4 + 2] = outFaces[f].v[2];
        m_previewMesh->faces[f * 4 + 3] = outFaces[f].v[3];
        m_previewMesh->faceGroups[f] = outFaces[f].gid;
    }

    m_previewMesh->matrix = m_baseMesh->matrix;
    m_previewMesh->editMatrix = m_baseMesh->editMatrix;
    m_previewMesh->center = m_baseMesh->center;
    m_previewMesh->scale = m_baseMesh->scale;

    m_previewMesh->postInit(false);
    m_previewMesh->initCreases();

    if (m_params.smoothMode == ExtractSmoothMode::BorderOnly && m_params.creaseRim && m_previewMesh->nbEdges > 0) {
        m_previewMesh->edgeCreases.assign(m_previewMesh->nbEdges, 0);
        for (const auto& be : boundaryEdges) {
            uint32_t e = m_previewMesh->faceEdges[be.faceIdx * 4 + be.localEdge];
            if (e < (uint32_t)m_previewMesh->nbEdges) {
                m_previewMesh->edgeCreases[e] = 1;
            }
        }
    } else {
        if (!m_previewMesh->edgeCreases.empty()) {
            std::fill(m_previewMesh->edgeCreases.begin(), m_previewMesh->edgeCreases.end(), 0);
        }
    }
}

void MaskExtractTool::rebuildPreviewMeshTopology() {
    if (!m_baseMesh || !m_previewMesh) return;

    if (m_params.fillMode == ExtractFillMode::Shell) {
        buildShellTopology();
    } else if (m_params.fillMode == ExtractFillMode::Fill) {
        buildFillTopology();
    } else if (m_params.fillMode == ExtractFillMode::None) {
        buildNoneTopology();
    }

    // If Mode == All, smooth the 3D solid mesh
    if (m_params.smoothMode == ExtractSmoothMode::All) {
        if (!m_previewMesh->edgeCreases.empty()) {
            std::fill(m_previewMesh->edgeCreases.begin(), m_previewMesh->edgeCreases.end(), 0);
        }
        if (m_params.smoothIterations > 0) {
            smoothSolidMesh(*m_previewMesh, m_params.smoothIterations);
        }
    }

    // Clear mask on preview mesh (set 3rd material channel to 1.0f)
    int outNv = m_previewMesh->nbVerts;
    if (m_previewMesh->materials.size() == (size_t)outNv * 3) {
        for (int i = 0; i < outNv; ++i) {
            m_previewMesh->materials[i * 3 + 2] = 1.0f;
        }
    }

    computeAnchorAndNormal();

    m_previewMesh->isDirty = true;
    m_previewMesh->isVertexDirty = true;
}

bool MaskExtractTool::startPreview(Scene& scene, Mesh* baseMesh) {
    if (!baseMesh) return false;

    // If already in preview, cancel previous first
    if (m_isPreviewActive) {
        cancel(scene);
    }

    m_baseMesh = baseMesh;

    if (!extractRawPatch(*m_baseMesh)) {
        GuiManager::showToast("No masked area to extract! Hold Ctrl and paint a mask first.", 3.5f);
        return false;
    }

    applySmoothing();

    m_previewMesh = new Mesh();
    m_previewMesh->matrix = m_baseMesh->matrix;
    m_previewMesh->editMatrix = m_baseMesh->editMatrix;
    m_previewMesh->center = m_baseMesh->center;
    m_previewMesh->scale = m_baseMesh->scale;
    m_previewMesh->visibleV1 = m_baseMesh->visibleV1;
    m_previewMesh->visibleV2 = m_baseMesh->visibleV2;
    m_previewMesh->outlinerName = m_baseMesh->outlinerName + "_Extract";

    rebuildPreviewMeshTopology();

    scene.addMesh(m_previewMesh);
    m_isPreviewActive = true;
    m_isDragging = false;
    m_isHovered = false;

    char toast[128];
    snprintf(toast, sizeof(toast), "[Extract Preview] Adjust thickness in viewport or panel.",
             m_previewMesh->outlinerName.c_str());
    GuiManager::showToast(toast, 2.5f);

    return true;
}

void MaskExtractTool::updateSmoothAndTopology() {
    if (!m_isPreviewActive || !m_previewMesh || !m_baseMesh) return;
    applySmoothing();
    rebuildPreviewMeshTopology();
}

void MaskExtractTool::updateShellGeometry(float newThickness) {
    Thickness::ThicknessParams tp;
    tp.thickness = newThickness;
    tp.offset = m_params.offset;
    tp.rimSegments = m_params.rimSegments;
    tp.creaseRim = (m_params.smoothMode == ExtractSmoothMode::BorderOnly) && m_params.creaseRim;
    tp.generateGroups = true;
    tp.evenThickness = true;

    Thickness::evaluateThicknessGeometry(m_smoothPatchMesh, *m_previewMesh, tp, m_thickCache);
}

void MaskExtractTool::updateNoneGeometry(float newThickness) {
    float dFront = newThickness * (1.0f + m_params.offset) * 0.5f;
    int N_v = m_previewMesh->nbVerts;
    float* outV = m_previewMesh->verts.data();
    const float* inV = m_smoothPatchMesh.verts.data();

    #pragma omp parallel for schedule(static)
    for (int i = 0; i < N_v; ++i) {
        glm::vec3 norm = (i < (int)m_patchBaseNormals.size()) ? m_patchBaseNormals[i] : glm::vec3(0.0f, 1.0f, 0.0f);
        float nLen = glm::length(norm);
        if (nLen > 1e-5f) norm /= nLen;
        outV[i * 3 + 0] = inV[i * 3 + 0] + norm.x * dFront;
        outV[i * 3 + 1] = inV[i * 3 + 1] + norm.y * dFront;
        outV[i * 3 + 2] = inV[i * 3 + 2] + norm.z * dFront;
    }
}

void MaskExtractTool::updateFillGeometry(float newThickness) {
    if (m_fillBasePositions.size() != (size_t)m_previewMesh->nbVerts) {
        buildFillTopology();
        return;
    }
    float dFront = newThickness * (1.0f + m_params.offset) * 0.5f;
    int N_v = m_previewMesh->nbVerts;
    float* outV = m_previewMesh->verts.data();

    #pragma omp parallel for schedule(static)
    for (int i = 0; i < N_v; ++i) {
        glm::vec3 pBase = m_fillBasePositions[i];
        glm::vec3 norm = (i < (int)m_fillNormals.size()) ? m_fillNormals[i] : m_normalWorld;
        glm::vec3 p = pBase + norm * dFront;
        outV[i * 3 + 0] = p.x;
        outV[i * 3 + 1] = p.y;
        outV[i * 3 + 2] = p.z;
    }
}

void MaskExtractTool::updateThicknessFast(float newThickness) {
    if (!m_isPreviewActive || !m_previewMesh) return;

    m_params.thickness = std::max(0.0f, newThickness);

    if (m_params.fillMode == ExtractFillMode::Shell) {
        updateShellGeometry(m_params.thickness);
    } else if (m_params.fillMode == ExtractFillMode::Fill) {
        updateFillGeometry(m_params.thickness);
    } else if (m_params.fillMode == ExtractFillMode::None) {
        updateNoneGeometry(m_params.thickness);
    }

    if (m_params.smoothMode == ExtractSmoothMode::All) {
        if (!m_previewMesh->edgeCreases.empty()) {
            std::fill(m_previewMesh->edgeCreases.begin(), m_previewMesh->edgeCreases.end(), 0);
        }
        if (m_params.smoothIterations > 0) {
            smoothSolidMesh(*m_previewMesh, m_params.smoothIterations);
        }
    } else {
        // Update normals
        updateFaceNormalsOnly(
            m_previewMesh->verts.data(), m_previewMesh->nbVerts,
            m_previewMesh->faces.data(), m_previewMesh->nbFaces,
            m_previewMesh->faceNormals.data()
        );
        updateVertexNormals(
            nullptr, -1, m_previewMesh->nbVerts,
            m_previewMesh->vrfStartCount.data(),
            m_previewMesh->vertRingFace.data(),
            m_previewMesh->faceNormals.data(),
            m_previewMesh->normals.data()
        );
    }

    float dHandle = m_params.thickness * (1.0f + m_params.offset) * 0.5f;
    m_handlePosWorld = m_anchorWorld + m_normalWorld * dHandle;

    m_previewMesh->isDirty = true;
    m_previewMesh->isVertexDirty = true;
    m_previewMesh->dirtyVertMin = 0;
    m_previewMesh->dirtyVertMax = m_previewMesh->nbVerts - 1;
}

bool MaskExtractTool::accept(Scene& scene) {
    if (!m_isPreviewActive || !m_previewMesh) return false;

    // Full initialization with Octree for sculpting
    m_previewMesh->postInit(true);

    scene.pushHistoryState();

    Mesh* finalMesh = m_previewMesh;
    m_previewMesh = nullptr;
    m_isPreviewActive = false;
    m_isDragging = false;
    m_isHovered = false;
    m_thickCache.clear();

    scene.selectMesh(finalMesh);
    scene.setModified(true);

    GuiManager::invalidateThumbnail(finalMesh->getID());
    char toast[128];
    snprintf(toast, sizeof(toast), "[Extract] Created '%s' (%d faces)",
             finalMesh->outlinerName.c_str(), finalMesh->nbFaces);
    GuiManager::showToast(toast, 3.5f);

    return true;
}

void MaskExtractTool::cancel(Scene& scene) {
    if (!m_isPreviewActive) return;

    if (m_previewMesh) {
        scene.removeMesh(m_previewMesh);
        m_previewMesh = nullptr;
    }
    m_isPreviewActive = false;
    m_isDragging = false;
    m_isHovered = false;
    m_thickCache.clear();
    GuiManager::showToast("[Extract] Cancelled.", 2.0f);
}

void MaskExtractTool::renderOverlay(
    Scene& scene, const Camera& camera,
    float viewportW, float viewportH
) {
    (void)scene; (void)viewportW; (void)viewportH;
    if (!m_isPreviewActive || !m_previewMesh) return;

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    if (!drawList) return;

    // Viewport 3D thickness handle
    glm::vec3 anchorProj = camera.project(m_anchorWorld);
    glm::vec3 handleProj = camera.project(m_handlePosWorld);

    // Behind camera check
    if (anchorProj.z >= 0.0f && anchorProj.z <= 1.0f && handleProj.z >= 0.0f && handleProj.z <= 1.0f) {
        ImVec2 anchorScreen(anchorProj.x, anchorProj.y);
        ImVec2 handleScreen(handleProj.x, handleProj.y);

        // Hover detection when not already dragging
        if (!m_isDragging) {
            if (!isMouseOverAnyImGuiWindow()) {
                ImVec2 m = ImGui::GetMousePos();
                float dMouse = glm::distance(glm::vec2(m.x, m.y), glm::vec2(handleScreen.x, handleScreen.y));
                m_isHovered = (dMouse < 22.0f);
            } else {
                m_isHovered = false;
            }
        }

        bool isHovered = m_isHovered;
        bool isActive = m_isDragging;

        // 1. Guide line from surface anchor to handle
        ImU32 shadowCol = IM_COL32(0, 0, 0, 180);
        ImU32 lineCol = (isHovered || isActive) ? IM_COL32(255, 230, 40, 240) : IM_COL32(0, 215, 245, 180);
        float lineWidth = (isHovered || isActive) ? 2.5f : 1.8f;
        drawList->AddLine(anchorScreen, handleScreen, shadowCol, lineWidth + 2.0f);
        drawList->AddLine(anchorScreen, handleScreen, lineCol, lineWidth);

        // 2. Surface anchor dot
        drawList->AddCircleFilled(anchorScreen, 3.5f, IM_COL32(255, 255, 255, 220), 12);
        drawList->AddCircle(anchorScreen, 3.5f, IM_COL32(0, 0, 0, 200), 12, 1.5f);

        // 3. Handle outer aura on hover / drag
        float radius = (isHovered || isActive) ? 10.0f : 7.5f;
        if (isHovered || isActive) {
            drawList->AddCircle(handleScreen, radius + 4.0f, IM_COL32(255, 220, 40, 140), 16, 2.0f);
        }

        // 4. Handle shadow
        drawList->AddCircleFilled(ImVec2(handleScreen.x + 1.5f, handleScreen.y + 1.5f), radius + 1.0f, IM_COL32(0, 0, 0, 180), 16);

        // 5. Outer rim
        ImU32 rimCol = (isHovered || isActive) ? IM_COL32(255, 240, 100, 255) : IM_COL32(20, 25, 30, 240);
        drawList->AddCircle(handleScreen, radius, rimCol, 16, 2.0f);

        // 6. Inner fill
        ImU32 fillCol;
        if (isActive) {
            fillCol = IM_COL32(255, 150, 20, 255); // Vibrant Orange
        } else if (isHovered) {
            fillCol = IM_COL32(255, 225, 40, 255); // Glowing Yellow
        } else {
            fillCol = IM_COL32(0, 215, 245, 240);  // Cyan
        }
        drawList->AddCircleFilled(handleScreen, radius - 1.5f, fillCol, 16);

        // 7. Value Badge
        if (isHovered || isActive) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        char valStr[64];
        snprintf(valStr, sizeof(valStr), "Thickness: %.3f", m_params.thickness);
        ImVec2 txtSize = ImGui::CalcTextSize(valStr);
        ImVec2 badgeMin(handleScreen.x + 14.0f, handleScreen.y - txtSize.y * 0.5f - 4.0f);
        ImVec2 badgeMax(badgeMin.x + txtSize.x + 12.0f, badgeMin.y + txtSize.y + 8.0f);

        drawList->AddRectFilled(badgeMin, badgeMax, IM_COL32(15, 18, 22, 230), 4.0f);
        drawList->AddRect(badgeMin, badgeMax, (isHovered || isActive) ? IM_COL32(255, 220, 50, 220) : IM_COL32(0, 215, 245, 180), 4.0f, 0, 1.5f);
        drawList->AddText(ImVec2(badgeMin.x + 6.0f, badgeMin.y + 4.0f), IM_COL32(255, 255, 255, 255), valStr);
    } else {
        m_isHovered = false;
    }

    // Top-center floating HUD Bar for quick Accept / Cancel
    ImGui::SetNextWindowPos(ImVec2(viewportW * 0.5f, 42.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGuiWindowFlags hudFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##ExtractFloatingHud", nullptr, hudFlags)) {
        const char* fillName = (m_params.fillMode == ExtractFillMode::Shell) ? "Shell" :
                               ((m_params.fillMode == ExtractFillMode::Fill) ? "Fill" : "None");
        ImGui::TextColored(ImVec4(0.2f, 0.95f, 0.85f, 1.0f), "%s Extract [%s]", ICON_LC_BOX, fillName);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("Thick: %.3f", m_params.thickness);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.65f, 0.34f, 1.0f));
        if (ImGui::Button("Accept", ImVec2(70, 24))) {
            accept(scene);
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.22f, 0.22f, 1.0f));
        if (ImGui::Button("Cancel", ImVec2(70, 24))) {
            cancel(scene);
        }
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

bool MaskExtractTool::handleMouseDown(
    Scene& scene, const Camera& camera,
    float mouseX, float mouseY,
    bool isCtrl, bool isAlt, bool isShift
) {
    (void)scene; (void)camera; (void)isCtrl; (void)isAlt; (void)isShift;
    if (!m_isPreviewActive || !m_previewMesh) return false;
    if (isMouseOverAnyImGuiWindow()) return false;

    if (m_isHovered) {
        m_isDragging = true;
        m_dragStartMouse = glm::vec2(mouseX, mouseY);
        m_dragStartThickness = m_params.thickness;
        return true;
    }

    return false;
}

bool MaskExtractTool::handleMouseMove(
    Scene& scene, const Camera& camera,
    float mouseX, float mouseY
) {
    (void)scene;
    if (!m_isPreviewActive || !m_previewMesh) return false;

    if (m_isDragging) {
        glm::vec3 anchorProj = camera.project(m_anchorWorld);
        glm::vec3 handleProj = camera.project(m_handlePosWorld);
        glm::vec2 p0(anchorProj.x, anchorProj.y);
        glm::vec2 p1(handleProj.x, handleProj.y);

        glm::vec2 screenDir = p1 - p0;
        float screenLen = glm::length(screenDir);
        if (screenLen < 1e-4f) {
            glm::vec3 testWorld = m_anchorWorld + m_normalWorld * 1.0f;
            glm::vec3 testProj = camera.project(testWorld);
            screenDir = glm::vec2(testProj.x - anchorProj.x, testProj.y - anchorProj.y);
            screenLen = glm::length(screenDir);
        }
        if (screenLen < 1e-4f) return true;

        screenDir /= screenLen;

        glm::vec3 testWorld = m_anchorWorld + m_normalWorld * 1.0f;
        glm::vec3 testProj = camera.project(testWorld);
        float testScreenLen = glm::length(glm::vec2(testProj.x - anchorProj.x, testProj.y - anchorProj.y));
        float worldPerPixel = (testScreenLen > 1e-3f) ? (1.0f / testScreenLen) : 0.001f;

        glm::vec2 curMouse(mouseX, mouseY);
        glm::vec2 mouseDelta = curMouse - m_dragStartMouse;
        float projPixels = glm::dot(mouseDelta, screenDir);

        float newThick = m_dragStartThickness + projPixels * worldPerPixel;
        if (newThick < 0.0f) newThick = 0.0f;

        updateThicknessFast(newThick);
        return true;
    }

    return false;
}

bool MaskExtractTool::handleMouseUp(Scene& scene) {
    (void)scene;
    if (m_isDragging) {
        m_isDragging = false;
        return true;
    }
    return false;
}

bool MaskExtractTool::handleKeyDown(Scene& scene, int keycode) {
    if (!m_isPreviewActive) return false;
    if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER) {
        return accept(scene);
    }
    if (keycode == SDLK_ESCAPE) {
        cancel(scene);
        return true;
    }
    return false;
}

void MaskExtractTool::notifyMeshDeleted(Mesh* mesh) {
    if (m_baseMesh == mesh || m_previewMesh == mesh) {
        m_previewMesh = nullptr;
        m_baseMesh = nullptr;
        m_isPreviewActive = false;
        m_isDragging = false;
        m_isHovered = false;
        m_thickCache.clear();
    }
}
