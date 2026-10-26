#include "Thickness.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include "mesh/Mesh.h"
#include "common/Constants.h"
#include "sculpt/SculptEngine.h"

namespace Thickness {

void computeMiterNormals(
    const Mesh& baseMesh,
    const ThicknessParams& params,
    std::vector<glm::vec3>& outMiterNormals
) {
    int nbVerts = baseMesh.nbVerts;
    int nbFaces = baseMesh.nbFaces;
    outMiterNormals.resize(nbVerts, glm::vec3(0.0f));

    if (nbVerts <= 0 || nbFaces <= 0) return;

    // 1. Calculate face normals (area-weighted)
    std::vector<glm::vec3> faceNormals(nbFaces, glm::vec3(0.0f));
    const float* vData = baseMesh.verts.data();
    const uint32_t* fData = baseMesh.faces.data();

    #pragma omp parallel for schedule(static)
    for (int f = 0; f < nbFaces; ++f) {
        uint32_t iv0 = fData[f * 4 + 0];
        uint32_t iv1 = fData[f * 4 + 1];
        uint32_t iv2 = fData[f * 4 + 2];

        glm::vec3 p0(vData[iv0 * 3], vData[iv0 * 3 + 1], vData[iv0 * 3 + 2]);
        glm::vec3 p1(vData[iv1 * 3], vData[iv1 * 3 + 1], vData[iv1 * 3 + 2]);
        glm::vec3 p2(vData[iv2 * 3], vData[iv2 * 3 + 1], vData[iv2 * 3 + 2]);

        faceNormals[f] = glm::cross(p1 - p0, p2 - p0);
    }

    // 2. Accumulate vertex normals using vertRingFace if available
    bool hasVRF = (baseMesh.vrfStartCount.size() >= (size_t)(nbVerts * 2) &&
                   !baseMesh.vertRingFace.empty());

    if (hasVRF) {
        const uint32_t* vrfStartCount = baseMesh.vrfStartCount.data();
        const uint32_t* vertRingFace = baseMesh.vertRingFace.data();

        #pragma omp parallel for schedule(static)
        for (int i = 0; i < nbVerts; ++i) {
            uint32_t start = vrfStartCount[i * 2];
            uint32_t count = vrfStartCount[i * 2 + 1];

            glm::vec3 sum(0.0f);
            for (uint32_t j = 0; j < count; ++j) {
                uint32_t fIdx = vertRingFace[start + j];
                sum += faceNormals[fIdx];
            }

            float len = glm::length(sum);
            glm::vec3 vn = (len > 1e-6f) ? (sum / len) : glm::vec3(0.0f, 1.0f, 0.0f);

            if (params.evenThickness && count > 0) {
                float minDot = 1.0f;
                for (uint32_t j = 0; j < count; ++j) {
                    uint32_t fIdx = vertRingFace[start + j];
                    float fnLen = glm::length(faceNormals[fIdx]);
                    if (fnLen > 1e-6f) {
                        glm::vec3 fnNorm = faceNormals[fIdx] / fnLen;
                        float d = glm::dot(vn, fnNorm);
                        if (d < minDot) minDot = d;
                    }
                }
                float miterScale = 1.0f;
                if (minDot > 0.35f) {
                    miterScale = std::min(1.414f, 1.0f / minDot);
                }
                outMiterNormals[i] = vn * miterScale;
            } else {
                outMiterNormals[i] = vn;
            }
        }
    } else {
        // Fallback: iterate over faces and accumulate into vertex normals
        std::vector<glm::vec3> vertSums(nbVerts, glm::vec3(0.0f));
        for (int f = 0; f < nbFaces; ++f) {
            glm::vec3 fn = faceNormals[f];
            for (int k = 0; k < 4; ++k) {
                uint32_t vid = fData[f * 4 + k];
                if (vid != TRI_INDEX && vid < (uint32_t)nbVerts) {
                    vertSums[vid] += fn;
                }
            }
        }
        #pragma omp parallel for schedule(static)
        for (int i = 0; i < nbVerts; ++i) {
            float len = glm::length(vertSums[i]);
            outMiterNormals[i] = (len > 1e-6f) ? (vertSums[i] / len) : glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }
}

struct BoundaryHalfEdge {
    uint32_t va;
    uint32_t vb;
    uint32_t baseEdgeIdx;
    uint32_t faceIdx;
};

void generateThickness(
    const Mesh& baseMesh,
    Mesh& outMesh,
    const ThicknessParams& params,
    ThicknessCache* cache
) {
    if (baseMesh.nbVerts <= 0 || baseMesh.nbFaces <= 0) {
        outMesh.nbVerts = 0;
        outMesh.nbFaces = 0;
        outMesh.nbEdges = 0;
        outMesh.verts.clear();
        outMesh.normals.clear();
        outMesh.colors.clear();
        outMesh.materials.clear();
        outMesh.faces.clear();
        outMesh.faceGroups.clear();
        return;
    }

    // Ensure edges are initialized for boundary detection
    if (baseMesh.edges.empty() || baseMesh.faceEdges.empty()) {
        const_cast<Mesh&>(baseMesh).initEdges();
    }

    int N_v = baseMesh.nbVerts;
    int N_f = baseMesh.nbFaces;

    // 1. Detect boundary edges
    std::vector<BoundaryHalfEdge> boundaryEdges;
    const uint32_t* fData = baseMesh.faces.data();
    const uint32_t* feData = baseMesh.faceEdges.data();
    const uint32_t* eData = baseMesh.edges.data();
    size_t edgeCountTotal = baseMesh.edges.size();

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
                boundaryEdges.push_back({va, vb, eIdx, (uint32_t)f});
            }
        }
    }

    bool isClosed = boundaryEdges.empty();
    int S = std::clamp(params.rimSegments, 1, 4);

    // 2. Identify boundary vertices
    std::vector<uint32_t> boundaryBaseVerts;
    std::vector<int32_t> baseToBoundaryIdx(N_v, -1);
    if (!isClosed) {
        for (const auto& be : boundaryEdges) {
            if (baseToBoundaryIdx[be.va] == -1) {
                baseToBoundaryIdx[be.va] = (int32_t)boundaryBaseVerts.size();
                boundaryBaseVerts.push_back(be.va);
            }
            if (baseToBoundaryIdx[be.vb] == -1) {
                baseToBoundaryIdx[be.vb] = (int32_t)boundaryBaseVerts.size();
                boundaryBaseVerts.push_back(be.vb);
            }
        }
    }

    // 3. Compute sizes and allocate
    int nbRimInterVerts = isClosed ? 0 : (int)(boundaryBaseVerts.size() * (S - 1));
    int outNbVerts = 2 * N_v + nbRimInterVerts;

    int B = (int)boundaryEdges.size();
    int nbRimFaces = isClosed ? 0 : (B * S);
    int outNbFaces = 2 * N_f + nbRimFaces;

    outMesh.nbVerts = outNbVerts;
    outMesh.nbFaces = outNbFaces;
    outMesh.verts.resize(outNbVerts * 3);
    outMesh.normals.resize(outNbVerts * 3, 0.0f);
    outMesh.colors.resize(outNbVerts * 3, 1.0f);
    outMesh.materials.resize(outNbVerts * 3);
    outMesh.faces.resize(outNbFaces * 4);
    outMesh.faceGroups.resize(outNbFaces);

    // 4. Compute normals & miter
    std::vector<glm::vec3> miterNormals;
    computeMiterNormals(baseMesh, params, miterNormals);

    // 5. Generate Vertex Positions, Colors, Materials
    float dFront = params.thickness * (1.0f + params.offset) * 0.5f;
    float dBack  = params.thickness * (1.0f - params.offset) * 0.5f;

    const float* bVerts = baseMesh.verts.data();
    const float* bCols  = baseMesh.colors.data();
    const float* bMats  = baseMesh.materials.data();
    bool hasColors = !baseMesh.colors.empty();
    bool hasMats   = !baseMesh.materials.empty();

    float* outV = outMesh.verts.data();
    float* outC = outMesh.colors.data();
    float* outM = outMesh.materials.data();

    #pragma omp parallel for schedule(static)
    for (int i = 0; i < N_v; ++i) {
        glm::vec3 pBase(bVerts[i * 3], bVerts[i * 3 + 1], bVerts[i * 3 + 2]);
        glm::vec3 m = miterNormals[i];

        glm::vec3 pFront = pBase + m * dFront;
        glm::vec3 pBack  = pBase - m * dBack;

        // Front vertex
        outV[i * 3 + 0] = pFront.x;
        outV[i * 3 + 1] = pFront.y;
        outV[i * 3 + 2] = pFront.z;

        // Back vertex
        int backIdx = N_v + i;
        outV[backIdx * 3 + 0] = pBack.x;
        outV[backIdx * 3 + 1] = pBack.y;
        outV[backIdx * 3 + 2] = pBack.z;

        // Colors and materials
        if (hasColors) {
            outC[i * 3 + 0] = bCols[i * 3 + 0];
            outC[i * 3 + 1] = bCols[i * 3 + 1];
            outC[i * 3 + 2] = bCols[i * 3 + 2];
            outC[backIdx * 3 + 0] = bCols[i * 3 + 0];
            outC[backIdx * 3 + 1] = bCols[i * 3 + 1];
            outC[backIdx * 3 + 2] = bCols[i * 3 + 2];
        }
        if (hasMats) {
            outM[i * 3 + 0] = bMats[i * 3 + 0];
            outM[i * 3 + 1] = bMats[i * 3 + 1];
            outM[i * 3 + 2] = bMats[i * 3 + 2];
            outM[backIdx * 3 + 0] = bMats[i * 3 + 0];
            outM[backIdx * 3 + 1] = bMats[i * 3 + 1];
            outM[backIdx * 3 + 2] = bMats[i * 3 + 2];
        } else {
            outM[i * 3 + 0] = 0.5f; outM[i * 3 + 1] = 0.0f; outM[i * 3 + 2] = 1.0f;
            outM[backIdx * 3 + 0] = 0.5f; outM[backIdx * 3 + 1] = 0.0f; outM[backIdx * 3 + 2] = 1.0f;
        }
    }

    // Intermediate rim vertices
    if (!isClosed && S > 1) {
        int nbBv = (int)boundaryBaseVerts.size();
        #pragma omp parallel for schedule(static)
        for (int bIdx = 0; bIdx < nbBv; ++bIdx) {
            uint32_t vBase = boundaryBaseVerts[bIdx];
            glm::vec3 pFront(outV[vBase * 3], outV[vBase * 3 + 1], outV[vBase * 3 + 2]);
            glm::vec3 pBack(outV[(N_v + vBase) * 3], outV[(N_v + vBase) * 3 + 1], outV[(N_v + vBase) * 3 + 2]);

            for (int s = 1; s < S; ++s) {
                float t = (float)s / (float)S;
                glm::vec3 pRim = glm::mix(pFront, pBack, t);
                int rimVIdx = 2 * N_v + bIdx * (S - 1) + (s - 1);

                outV[rimVIdx * 3 + 0] = pRim.x;
                outV[rimVIdx * 3 + 1] = pRim.y;
                outV[rimVIdx * 3 + 2] = pRim.z;

                if (hasColors) {
                    outC[rimVIdx * 3 + 0] = bCols[vBase * 3 + 0];
                    outC[rimVIdx * 3 + 1] = bCols[vBase * 3 + 1];
                    outC[rimVIdx * 3 + 2] = bCols[vBase * 3 + 2];
                }
                if (hasMats) {
                    outM[rimVIdx * 3 + 0] = bMats[vBase * 3 + 0];
                    outM[rimVIdx * 3 + 1] = bMats[vBase * 3 + 1];
                    outM[rimVIdx * 3 + 2] = bMats[vBase * 3 + 2];
                } else {
                    outM[rimVIdx * 3 + 0] = 0.5f; outM[rimVIdx * 3 + 1] = 0.0f; outM[rimVIdx * 3 + 2] = 1.0f;
                }
            }
        }
    }

    // 6. Generate Faces
    uint32_t* outFData = outMesh.faces.data();

    // Front faces (preserve winding)
    #pragma omp parallel for schedule(static)
    for (int f = 0; f < N_f; ++f) {
        outFData[f * 4 + 0] = fData[f * 4 + 0];
        outFData[f * 4 + 1] = fData[f * 4 + 1];
        outFData[f * 4 + 2] = fData[f * 4 + 2];
        outFData[f * 4 + 3] = fData[f * 4 + 3];
    }

    // Back faces (inverted winding)
    #pragma omp parallel for schedule(static)
    for (int f = 0; f < N_f; ++f) {
        uint32_t v0 = fData[f * 4 + 0] + N_v;
        uint32_t v1 = fData[f * 4 + 1] + N_v;
        uint32_t v2 = fData[f * 4 + 2] + N_v;
        uint32_t v3 = fData[f * 4 + 3];

        int outFIdx = N_f + f;
        if (v3 == TRI_INDEX) {
            outFData[outFIdx * 4 + 0] = v0;
            outFData[outFIdx * 4 + 1] = v2;
            outFData[outFIdx * 4 + 2] = v1;
            outFData[outFIdx * 4 + 3] = TRI_INDEX;
        } else {
            uint32_t v3Back = v3 + N_v;
            outFData[outFIdx * 4 + 0] = v0;
            outFData[outFIdx * 4 + 1] = v3Back;
            outFData[outFIdx * 4 + 2] = v2;
            outFData[outFIdx * 4 + 3] = v1;
        }
    }

    // Rim faces
    auto getRimVert = [&](uint32_t baseV, int step) -> uint32_t {
        if (step == 0) return baseV;
        if (step == S) return (uint32_t)(N_v + baseV);
        int bIdx = baseToBoundaryIdx[baseV];
        return (uint32_t)(2 * N_v + bIdx * (S - 1) + (step - 1));
    };

    if (!isClosed) {
        #pragma omp parallel for schedule(static)
        for (int b = 0; b < B; ++b) {
            uint32_t va = boundaryEdges[b].va;
            uint32_t vb = boundaryEdges[b].vb;

            for (int s = 0; s < S; ++s) {
                uint32_t q0 = getRimVert(va, s);
                uint32_t q1 = getRimVert(va, s + 1);
                uint32_t q2 = getRimVert(vb, s + 1);
                uint32_t q3 = getRimVert(vb, s);

                int outFIdx = 2 * N_f + b * S + s;
                outFData[outFIdx * 4 + 0] = q0;
                outFData[outFIdx * 4 + 1] = q1;
                outFData[outFIdx * 4 + 2] = q2;
                outFData[outFIdx * 4 + 3] = q3;
            }
        }
    }

    // 7. PolyGroups assignment
    uint32_t maxGid = 0;
    for (uint32_t g : baseMesh.faceGroups) {
        if (g > maxGid) maxGid = g;
    }
    uint32_t backOffset = maxGid + 1;
    uint32_t rimGid = maxGid + backOffset + 1;

    for (int f = 0; f < N_f; ++f) {
        uint32_t baseG = (f < (int)baseMesh.faceGroups.size()) ? baseMesh.faceGroups[f] : 0;
        outMesh.faceGroups[f] = baseG;
        outMesh.faceGroups[N_f + f] = params.generateGroups ? (baseG + backOffset) : baseG;
    }
    if (!isClosed) {
        for (int b = 0; b < B; ++b) {
            for (int s = 0; s < S; ++s) {
                int outFIdx = 2 * N_f + b * S + s;
                uint32_t baseG = (boundaryEdges[b].faceIdx < baseMesh.faceGroups.size())
                    ? baseMesh.faceGroups[boundaryEdges[b].faceIdx] : 0;
                outMesh.faceGroups[outFIdx] = params.generateGroups ? rimGid : baseG;
            }
        }
    }
    outMesh.isFaceGroupDirty = true;

    // 8. Initialize topology & edges on outMesh
    outMesh.postInit(false);
    outMesh.initCreases();

    // 9. Edge Creasing
    if (outMesh.nbEdges > 0) {
        outMesh.edgeCreases.assign(outMesh.nbEdges, 0);

        // Precompute sharp boundary vertices in base mesh
        std::vector<uint8_t> baseVertIsSharp(N_v, 0);
        if (baseMesh.edgeCreases.size() == baseMesh.edges.size() &&
            baseMesh.vrfStartCount.size() >= (size_t)(N_v * 2)) {
            for (int i = 0; i < N_v; ++i) {
                uint32_t st = baseMesh.vrfStartCount[i * 2];
                uint32_t cnt = baseMesh.vrfStartCount[i * 2 + 1];
                for (uint32_t j = 0; j < cnt; ++j) {
                    uint32_t fIdx = baseMesh.vertRingFace[st + j];
                    for (int k = 0; k < 4; ++k) {
                        uint32_t be = baseMesh.faceEdges[fIdx * 4 + k];
                        if (be < baseMesh.edgeCreases.size() && baseMesh.edgeCreases[be] == 1) {
                            baseVertIsSharp[i] = 1;
                            break;
                        }
                    }
                    if (baseVertIsSharp[i]) break;
                }
            }
        }

        // Crease front and back faces
        for (int f = 0; f < N_f; ++f) {
            for (int k = 0; k < 4; ++k) {
                uint32_t baseEIdx = baseMesh.faceEdges[f * 4 + k];
                if (baseEIdx == TRI_INDEX) continue;

                bool wasCreased = (baseEIdx < baseMesh.edgeCreases.size() && baseMesh.edgeCreases[baseEIdx] == 1);
                bool isBaseBoundary = (baseEIdx < baseMesh.edges.size() && baseMesh.edges[baseEIdx] == 1);

                // Front edge
                uint32_t frontE = outMesh.faceEdges[f * 4 + k];
                if (frontE < (uint32_t)outMesh.nbEdges) {
                    if (params.creaseRim && (wasCreased || isBaseBoundary)) {
                        outMesh.edgeCreases[frontE] = 1;
                    }
                }

                // Back edge
                uint32_t backE = outMesh.faceEdges[(N_f + f) * 4 + k];
                if (backE < (uint32_t)outMesh.nbEdges) {
                    if (params.creaseRim && (wasCreased || isBaseBoundary)) {
                        outMesh.edgeCreases[backE] = 1;
                    }
                }
            }
        }

        // Crease rim boundary loops and cross-edges
        if (!isClosed) {
            for (int b = 0; b < B; ++b) {
                uint32_t va = boundaryEdges[b].va;
                bool vaSharp = (va < (uint32_t)N_v) ? (baseVertIsSharp[va] == 1) : false;

                for (int s = 0; s < S; ++s) {
                    int outFIdx = 2 * N_f + b * S + s;

                    // Edge 0: along extrusion at va
                    uint32_t e0 = outMesh.faceEdges[outFIdx * 4 + 0];
                    if (params.creaseRim && vaSharp && e0 < (uint32_t)outMesh.nbEdges) {
                        outMesh.edgeCreases[e0] = 1;
                    }

                    // Edge 1: along back loop if s == S - 1
                    uint32_t e1 = outMesh.faceEdges[outFIdx * 4 + 1];
                    if (s == S - 1 && params.creaseRim && e1 < (uint32_t)outMesh.nbEdges) {
                        outMesh.edgeCreases[e1] = 1;
                    }

                    // Edge 3: along front loop if s == 0
                    uint32_t e3 = outMesh.faceEdges[outFIdx * 4 + 3];
                    if (s == 0 && params.creaseRim && e3 < (uint32_t)outMesh.nbEdges) {
                        outMesh.edgeCreases[e3] = 1;
                    }
                }
            }
        }

        // Assign edgeIsCage for front, back, and rim faces
        outMesh.edgeIsCage.assign(outMesh.nbEdges, 0);
        for (int f = 0; f < N_f; ++f) {
            for (int k = 0; k < 4; ++k) {
                uint32_t baseEIdx = baseMesh.faceEdges[f * 4 + k];
                if (baseEIdx == TRI_INDEX) continue;

                bool isCage = baseMesh.edgeIsCage.empty() ||
                              (baseEIdx < baseMesh.edgeIsCage.size() && baseMesh.edgeIsCage[baseEIdx] == 1);
                if (isCage) {
                    uint32_t frontE = outMesh.faceEdges[f * 4 + k];
                    if (frontE < (uint32_t)outMesh.nbEdges) {
                        outMesh.edgeIsCage[frontE] = 1;
                    }
                    uint32_t backE = outMesh.faceEdges[(N_f + f) * 4 + k];
                    if (backE < (uint32_t)outMesh.nbEdges) {
                        outMesh.edgeIsCage[backE] = 1;
                    }
                }
            }
        }

        if (!isClosed) {
            for (int b = 0; b < B; ++b) {
                for (int s = 0; s < S; ++s) {
                    int outFIdx = 2 * N_f + b * S + s;
                    uint32_t e0 = outMesh.faceEdges[outFIdx * 4 + 0];
                    if (e0 < (uint32_t)outMesh.nbEdges) {
                        outMesh.edgeIsCage[e0] = 1;
                    }
                    uint32_t e1 = outMesh.faceEdges[outFIdx * 4 + 1];
                    if (s == S - 1 && e1 < (uint32_t)outMesh.nbEdges) {
                        outMesh.edgeIsCage[e1] = 1;
                    }
                    uint32_t e3 = outMesh.faceEdges[outFIdx * 4 + 3];
                    if (s == 0 && e3 < (uint32_t)outMesh.nbEdges) {
                        outMesh.edgeIsCage[e3] = 1;
                    }
                }
            }
        }
    }

    outMesh.matrix = baseMesh.matrix;
    outMesh.editMatrix = baseMesh.editMatrix;
    outMesh.center = baseMesh.center;
    outMesh.scale = baseMesh.scale;

    // 10. Populate cache if requested
    if (cache) {
        cache->isValid = true;
        cache->baseNbVerts = N_v;
        cache->baseNbFaces = N_f;
        cache->rimSegments = S;
        cache->isClosedMesh = isClosed;
        cache->outNbVerts = outNbVerts;
        cache->outNbFaces = outNbFaces;
        cache->miterNormals = std::move(miterNormals);
        cache->boundaryBaseVerts = std::move(boundaryBaseVerts);
        cache->baseToBoundaryIdx = std::move(baseToBoundaryIdx);
    }
}

void evaluateThicknessGeometry(
    const Mesh& baseMesh,
    Mesh& outMesh,
    const ThicknessParams& params,
    ThicknessCache& cache
) {
    int N_v = baseMesh.nbVerts;
    if (!cache.isValid || cache.baseNbVerts != N_v || cache.outNbVerts != outMesh.nbVerts) {
        // Topology cache invalid, trigger full rebuild
        generateThickness(baseMesh, outMesh, params, &cache);
        return;
    }

    // Recompute miter normals for changed vertex positions
    computeMiterNormals(baseMesh, params, cache.miterNormals);

    float dFront = params.thickness * (1.0f + params.offset) * 0.5f;
    float dBack  = params.thickness * (1.0f - params.offset) * 0.5f;

    const float* bVerts = baseMesh.verts.data();
    float* outV = outMesh.verts.data();
    const glm::vec3* mNormals = cache.miterNormals.data();

    // Fast parallel update of front and back vertices
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < N_v; ++i) {
        glm::vec3 pBase(bVerts[i * 3], bVerts[i * 3 + 1], bVerts[i * 3 + 2]);
        glm::vec3 m = mNormals[i];

        glm::vec3 pFront = pBase + m * dFront;
        glm::vec3 pBack  = pBase - m * dBack;

        outV[i * 3 + 0] = pFront.x;
        outV[i * 3 + 1] = pFront.y;
        outV[i * 3 + 2] = pFront.z;

        int backIdx = N_v + i;
        outV[backIdx * 3 + 0] = pBack.x;
        outV[backIdx * 3 + 1] = pBack.y;
        outV[backIdx * 3 + 2] = pBack.z;
    }

    // Intermediate rim vertices
    int S = cache.rimSegments;
    if (!cache.isClosedMesh && S > 1) {
        int nbBv = (int)cache.boundaryBaseVerts.size();
        const uint32_t* bVertsList = cache.boundaryBaseVerts.data();

        #pragma omp parallel for schedule(static)
        for (int bIdx = 0; bIdx < nbBv; ++bIdx) {
            uint32_t vBase = bVertsList[bIdx];
            glm::vec3 pFront(outV[vBase * 3], outV[vBase * 3 + 1], outV[vBase * 3 + 2]);
            glm::vec3 pBack(outV[(N_v + vBase) * 3], outV[(N_v + vBase) * 3 + 1], outV[(N_v + vBase) * 3 + 2]);

            for (int s = 1; s < S; ++s) {
                float t = (float)s / (float)S;
                glm::vec3 pRim = glm::mix(pFront, pBack, t);
                int rimVIdx = 2 * N_v + bIdx * (S - 1) + (s - 1);

                outV[rimVIdx * 3 + 0] = pRim.x;
                outV[rimVIdx * 3 + 1] = pRim.y;
                outV[rimVIdx * 3 + 2] = pRim.z;
            }
        }
    }

    outMesh.matrix = baseMesh.matrix;
    outMesh.editMatrix = baseMesh.editMatrix;
    outMesh.center = baseMesh.center;
    outMesh.scale = baseMesh.scale;
}

} // namespace Thickness
