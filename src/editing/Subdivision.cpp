#include "editing/Subdivision.h"
#include "common/Constants.h"
#include "common/Logger.h"
#include <algorithm>
#include <cmath>

namespace Subdivision {

bool LINEAR = false;

class OddVertexComputer {
public:
    std::vector<float>& _vArOut;
    std::vector<float>& _cArOut;
    std::vector<float>& _mArOut;
    const std::vector<float>& _vAr;
    const std::vector<float>& _cAr;
    const std::vector<float>& _mAr;
    const std::vector<uint32_t>& _eAr;
    int _nbVertices;
    std::vector<int32_t> _tagEdges;

    OddVertexComputer(Mesh& mesh, std::vector<float>& vArOut, std::vector<float>& cArOut, std::vector<float>& mArOut)
        : _vArOut(vArOut)
        , _cArOut(cArOut)
        , _mArOut(mArOut)
        , _vAr(mesh.getVertices())
        , _cAr(mesh.getColors())
        , _mAr(mesh.getMaterials())
        , _eAr(mesh.edges)
        , _nbVertices(mesh.getNbVertices())
        , _tagEdges(mesh.getNbEdges(), 0)
    {}

    uint32_t computeTriangleEdgeVertex(uint32_t iv1, uint32_t iv2, uint32_t iv3, uint32_t ide) {
        uint32_t id1 = iv1 * 3;
        uint32_t id2 = iv2 * 3;
        uint32_t idOpp = iv3 * 3;
        int32_t testEdge = _tagEdges[ide] - 1;
        uint32_t ivMid = (testEdge == -1) ? (uint32_t)_nbVertices++ : (uint32_t)testEdge;
        uint32_t idMid = ivMid * 3;
        uint32_t edgeValue = (ide < _eAr.size()) ? _eAr[ide] : 2;

        if (idMid + 2 >= _vArOut.size()) {
            _vArOut.resize(idMid + 3, 0.0f);
            _cArOut.resize(idMid + 3, 0.0f);
            _mArOut.resize(idMid + 3, 0.0f);
        }

        if (edgeValue == 1 || edgeValue >= 3 || Subdivision::LINEAR) {
            if (testEdge != -1) return ivMid;
            _tagEdges[ide] = ivMid + 1;
            _vArOut[idMid]     = 0.5f * (_vAr[id1]     + _vAr[id2]);
            _vArOut[idMid + 1] = 0.5f * (_vAr[id1 + 1] + _vAr[id2 + 1]);
            _vArOut[idMid + 2] = 0.5f * (_vAr[id1 + 2] + _vAr[id2 + 2]);

            _cArOut[idMid]     = 0.5f * (_cAr[id1]     + _cAr[id2]);
            _cArOut[idMid + 1] = 0.5f * (_cAr[id1 + 1] + _cAr[id2 + 1]);
            _cArOut[idMid + 2] = 0.5f * (_cAr[id1 + 2] + _cAr[id2 + 2]);

            _mArOut[idMid]     = 0.5f * (_mAr[id1]     + _mAr[id2]);
            _mArOut[idMid + 1] = 0.5f * (_mAr[id1 + 1] + _mAr[id2 + 1]);
            _mArOut[idMid + 2] = 0.5f * (_mAr[id1 + 2] + _mAr[id2 + 2]);
        } else if (testEdge == -1) {
            _tagEdges[ide] = ivMid + 1;
            _vArOut[idMid]     = 0.125f * _vAr[idOpp]     + 0.375f * (_vAr[id1]     + _vAr[id2]);
            _vArOut[idMid + 1] = 0.125f * _vAr[idOpp + 1] + 0.375f * (_vAr[id1 + 1] + _vAr[id2 + 1]);
            _vArOut[idMid + 2] = 0.125f * _vAr[idOpp + 2] + 0.375f * (_vAr[id1 + 2] + _vAr[id2 + 2]);

            _cArOut[idMid]     = 0.125f * _cAr[idOpp]     + 0.375f * (_cAr[id1]     + _cAr[id2]);
            _cArOut[idMid + 1] = 0.125f * _cAr[idOpp + 1] + 0.375f * (_cAr[id1 + 1] + _cAr[id2 + 1]);
            _cArOut[idMid + 2] = 0.125f * _cAr[idOpp + 2] + 0.375f * (_cAr[id1 + 2] + _cAr[id2 + 2]);

            _mArOut[idMid]     = 0.125f * _mAr[idOpp]     + 0.375f * (_mAr[id1]     + _mAr[id2]);
            _mArOut[idMid + 1] = 0.125f * _mAr[idOpp + 1] + 0.375f * (_mAr[id1 + 1] + _mAr[id2 + 1]);
            _mArOut[idMid + 2] = 0.125f * _mAr[idOpp + 2] + 0.375f * (_mAr[id1 + 2] + _mAr[id2 + 2]);
        } else {
            _vArOut[idMid]     += 0.125f * _vAr[idOpp];
            _vArOut[idMid + 1] += 0.125f * _vAr[idOpp + 1];
            _vArOut[idMid + 2] += 0.125f * _vAr[idOpp + 2];

            _cArOut[idMid]     += 0.125f * _cAr[idOpp];
            _cArOut[idMid + 1] += 0.125f * _cAr[idOpp + 1];
            _cArOut[idMid + 2] += 0.125f * _cAr[idOpp + 2];

            _mArOut[idMid]     += 0.125f * _mAr[idOpp];
            _mArOut[idMid + 1] += 0.125f * _mAr[idOpp + 1];
            _mArOut[idMid + 2] += 0.125f * _mAr[idOpp + 2];
        }
        return ivMid;
    }

    uint32_t computeQuadEdgeVertex(uint32_t iv1, uint32_t iv2, uint32_t iv3, uint32_t iv4, uint32_t ide) {
        uint32_t id1 = iv1 * 3;
        uint32_t id2 = iv2 * 3;
        uint32_t idOpp = iv3 * 3;
        uint32_t idOpp2 = iv4 * 3;
        int32_t testEdge = _tagEdges[ide] - 1;
        uint32_t ivMid = (testEdge == -1) ? (uint32_t)_nbVertices++ : (uint32_t)testEdge;
        uint32_t idMid = ivMid * 3;
        uint32_t edgeValue = (ide < _eAr.size()) ? _eAr[ide] : 2;

        if (idMid + 2 >= _vArOut.size()) {
            _vArOut.resize(idMid + 3, 0.0f);
            _cArOut.resize(idMid + 3, 0.0f);
            _mArOut.resize(idMid + 3, 0.0f);
        }

        if (edgeValue == 1 || edgeValue >= 3 || Subdivision::LINEAR) {
            if (testEdge != -1) return ivMid;
            _tagEdges[ide] = ivMid + 1;
            _vArOut[idMid]     = 0.5f * (_vAr[id1]     + _vAr[id2]);
            _vArOut[idMid + 1] = 0.5f * (_vAr[id1 + 1] + _vAr[id2 + 1]);
            _vArOut[idMid + 2] = 0.5f * (_vAr[id1 + 2] + _vAr[id2 + 2]);

            _cArOut[idMid]     = 0.5f * (_cAr[id1]     + _cAr[id2]);
            _cArOut[idMid + 1] = 0.5f * (_cAr[id1 + 1] + _cAr[id2 + 1]);
            _cArOut[idMid + 2] = 0.5f * (_cAr[id1 + 2] + _cAr[id2 + 2]);

            _mArOut[idMid]     = 0.5f * (_mAr[id1]     + _mAr[id2]);
            _mArOut[idMid + 1] = 0.5f * (_mAr[id1 + 1] + _mAr[id2 + 1]);
            _mArOut[idMid + 2] = 0.5f * (_mAr[id1 + 2] + _mAr[id2 + 2]);
        } else if (testEdge == -1) {
            _tagEdges[ide] = ivMid + 1;
            _vArOut[idMid]     = 0.0625f * (_vAr[idOpp]     + _vAr[idOpp2])     + 0.375f * (_vAr[id1]     + _vAr[id2]);
            _vArOut[idMid + 1] = 0.0625f * (_vAr[idOpp + 1] + _vAr[idOpp2 + 1]) + 0.375f * (_vAr[id1 + 1] + _vAr[id2 + 1]);
            _vArOut[idMid + 2] = 0.0625f * (_vAr[idOpp + 2] + _vAr[idOpp2 + 2]) + 0.375f * (_vAr[id1 + 2] + _vAr[id2 + 2]);

            _cArOut[idMid]     = 0.0625f * (_cAr[idOpp]     + _cAr[idOpp2])     + 0.375f * (_cAr[id1]     + _cAr[id2]);
            _cArOut[idMid + 1] = 0.0625f * (_cAr[idOpp + 1] + _cAr[idOpp2 + 1]) + 0.375f * (_cAr[id1 + 1] + _cAr[id2 + 1]);
            _cArOut[idMid + 2] = 0.0625f * (_cAr[idOpp + 2] + _cAr[idOpp2 + 2]) + 0.375f * (_cAr[id1 + 2] + _cAr[id2 + 2]);

            _mArOut[idMid]     = 0.0625f * (_mAr[idOpp]     + _mAr[idOpp2])     + 0.375f * (_mAr[id1]     + _mAr[id2]);
            _mArOut[idMid + 1] = 0.0625f * (_mAr[idOpp + 1] + _mAr[idOpp2 + 1]) + 0.375f * (_mAr[id1 + 1] + _mAr[id2 + 1]);
            _mArOut[idMid + 2] = 0.0625f * (_mAr[idOpp + 2] + _mAr[idOpp2 + 2]) + 0.375f * (_mAr[id1 + 2] + _mAr[id2 + 2]);
        } else {
            _vArOut[idMid]     += 0.0625f * (_vAr[idOpp]     + _vAr[idOpp2]);
            _vArOut[idMid + 1] += 0.0625f * (_vAr[idOpp + 1] + _vAr[idOpp2 + 1]);
            _vArOut[idMid + 2] += 0.0625f * (_vAr[idOpp + 2] + _vAr[idOpp2 + 2]);

            _cArOut[idMid]     += 0.0625f * (_cAr[idOpp]     + _cAr[idOpp2]);
            _cArOut[idMid + 1] += 0.0625f * (_cAr[idOpp + 1] + _cAr[idOpp2 + 1]);
            _cArOut[idMid + 2] += 0.0625f * (_cAr[idOpp + 2] + _cAr[idOpp2 + 2]);

            _mArOut[idMid]     += 0.0625f * (_mAr[idOpp]     + _mAr[idOpp2]);
            _mArOut[idMid + 1] += 0.0625f * (_mAr[idOpp + 1] + _mAr[idOpp2 + 1]);
            _mArOut[idMid + 2] += 0.0625f * (_mAr[idOpp + 2] + _mAr[idOpp2 + 2]);
        }
        return ivMid;
    }

    uint32_t computeFaceVertex(uint32_t iv1, uint32_t iv2, uint32_t iv3, uint32_t iv4) {
        uint32_t id1 = iv1 * 3;
        uint32_t id2 = iv2 * 3;
        uint32_t id3 = iv3 * 3;
        uint32_t id4 = iv4 * 3;
        uint32_t ivCen = (uint32_t)_nbVertices++;
        uint32_t idCen = ivCen * 3;

        if (idCen + 2 >= _vArOut.size()) {
            _vArOut.resize(idCen + 3, 0.0f);
            _cArOut.resize(idCen + 3, 0.0f);
            _mArOut.resize(idCen + 3, 0.0f);
        }

        _vArOut[idCen]     = 0.25f * (_vAr[id1]     + _vAr[id2]     + _vAr[id3]     + _vAr[id4]);
        _vArOut[idCen + 1] = 0.25f * (_vAr[id1 + 1] + _vAr[id2 + 1] + _vAr[id3 + 1] + _vAr[id4 + 1]);
        _vArOut[idCen + 2] = 0.25f * (_vAr[id1 + 2] + _vAr[id2 + 2] + _vAr[id3 + 2] + _vAr[id4 + 2]);

        _cArOut[idCen]     = 0.25f * (_cAr[id1]     + _cAr[id2]     + _cAr[id3]     + _cAr[id4]);
        _cArOut[idCen + 1] = 0.25f * (_cAr[id1 + 1] + _cAr[id2 + 1] + _cAr[id3 + 1] + _cAr[id4 + 1]);
        _cArOut[idCen + 2] = 0.25f * (_cAr[id1 + 2] + _cAr[id2 + 2] + _cAr[id3 + 2] + _cAr[id4 + 2]);

        _mArOut[idCen]     = 0.25f * (_mAr[id1]     + _mAr[id2]     + _mAr[id3]     + _mAr[id4]);
        _mArOut[idCen + 1] = 0.25f * (_mAr[id1 + 1] + _mAr[id2 + 1] + _mAr[id3 + 1] + _mAr[id4 + 1]);
        _mArOut[idCen + 2] = 0.25f * (_mAr[id1 + 2] + _mAr[id2 + 2] + _mAr[id3 + 2] + _mAr[id4 + 2]);
        return ivCen;
    }
};

void applyEvenSmooth(Mesh& baseMesh, std::vector<float>& even, std::vector<float>& colorOut, std::vector<float>& materialOut) {
    int nbVerts = baseMesh.getNbVertices();
    if ((int)even.size() < nbVerts * 3) even.resize(nbVerts * 3);
    if ((int)colorOut.size() < nbVerts * 3) colorOut.resize(nbVerts * 3);
    if ((int)materialOut.size() < nbVerts * 3) materialOut.resize(nbVerts * 3);

    const auto& vArOld = baseMesh.getVertices();
    const auto& cArOld = baseMesh.getColors();
    const auto& mArOld = baseMesh.getMaterials();
    const auto& fArOld = baseMesh.getFaces();
    const auto& eArOld = baseMesh.edges;
    const auto& feArOld = baseMesh.faceEdges;
    const auto& vertOnEdgeOld = baseMesh.vertOnEdge;
    const auto& vrvStartCount = baseMesh.vrvStartCount;
    const auto& vertRingVert = baseMesh.vertRingVert;
    const auto& vrfStartCount = baseMesh.vrfStartCount;
    const auto& vertRingFace = baseMesh.vertRingFace;
    bool onlyTri = baseMesh.hasOnlyTriangles();

    std::copy(cArOld.begin(), cArOld.begin() + nbVerts * 3, colorOut.begin());
    std::copy(mArOld.begin(), mArOld.begin() + nbVerts * 3, materialOut.begin());

    #pragma omp parallel for schedule(static) if(nbVerts > 2000)
    for (int i = 0; i < nbVerts; ++i) {
        int j = i * 3;
        float avx = 0.0f, avy = 0.0f, avz = 0.0f;
        float beta = 0.0f, alpha = 0.0f;
        uint32_t k = 0, id = 0;

        if ((i < (int)vertOnEdgeOld.size() && vertOnEdgeOld[i]) || Subdivision::LINEAR) {
            uint32_t startF = vrfStartCount[i * 2];
            uint32_t endF = startF + vrfStartCount[i * 2 + 1];
            for (k = startF; k < endF; ++k) {
                uint32_t idFace = vertRingFace[k] * 4;
                uint32_t i1 = fArOld[idFace];
                uint32_t i2 = fArOld[idFace + 1];
                uint32_t i3 = fArOld[idFace + 2];
                uint32_t i4 = fArOld[idFace + 3];
                bool isTri = (i4 == TRI_INDEX);
                id = TRI_INDEX;

                if (i1 == (uint32_t)i) {
                    if (eArOld[feArOld[idFace]] == 1) id = i2;
                    else if (eArOld[feArOld[isTri ? idFace + 2 : idFace + 3]] == 1) id = isTri ? i3 : i4;
                } else if (i2 == (uint32_t)i) {
                    if (eArOld[feArOld[idFace]] == 1) id = i1;
                    else if (eArOld[feArOld[idFace + 1]] == 1) id = i3;
                } else if (i3 == (uint32_t)i) {
                    if (eArOld[feArOld[idFace + 1]] == 1) id = i2;
                    else if (eArOld[feArOld[idFace + 2]] == 1) id = isTri ? i1 : i4;
                } else if (i4 == (uint32_t)i) {
                    if (eArOld[feArOld[idFace + 2]] == 1) id = i3;
                    else if (eArOld[feArOld[idFace + 3]] == 1) id = i1;
                }

                if (id == TRI_INDEX) continue;
                id *= 3;
                avx += vArOld[id];
                avy += vArOld[id + 1];
                avz += vArOld[id + 2];
                beta += 1.0f;
            }
            if (beta < 2.0f) {
                even[j]     = vArOld[j];
                even[j + 1] = vArOld[j + 1];
                even[j + 2] = vArOld[j + 2];
            } else {
                beta = 0.25f / beta;
                alpha = 0.75f;
                even[j]     = vArOld[j]     * alpha + avx * beta;
                even[j + 1] = vArOld[j + 1] * alpha + avy * beta;
                even[j + 2] = vArOld[j + 2] * alpha + avz * beta;
            }
            continue;
        }

        uint32_t start = vrvStartCount[i * 2];
        uint32_t count = vrvStartCount[i * 2 + 1];
        uint32_t end = start + count;

        for (k = start; k < end; ++k) {
            id = vertRingVert[k] * 3;
            avx += vArOld[id];
            avy += vArOld[id + 1];
            avz += vArOld[id + 2];
        }

        if (onlyTri) {
            if (count == 6) {
                beta = 0.0625f;
                alpha = 0.625f;
            } else if (count == 3) {
                beta = 0.1875f;
                alpha = 0.4375f;
            } else {
                beta = 0.375f / (float)count;
                alpha = 0.625f;
            }
            even[j]     = vArOld[j]     * alpha + avx * beta;
            even[j + 1] = vArOld[j + 1] * alpha + avy * beta;
            even[j + 2] = vArOld[j + 2] * alpha + avz * beta;
            continue;
        }

        float oppx = 0.0f, oppy = 0.0f, oppz = 0.0f;
        float gamma = 0.0f;

        uint32_t startFace = vrfStartCount[i * 2];
        uint32_t countFace = vrfStartCount[i * 2 + 1];
        uint32_t endFace = startFace + countFace;
        uint32_t nbQuad = 0;

        for (k = startFace; k < endFace; ++k) {
            id = vertRingFace[k] * 4;
            uint32_t iv4 = fArOld[id + 3];
            if (iv4 == TRI_INDEX) continue;

            nbQuad++;
            uint32_t iv1 = fArOld[id];
            uint32_t iv2 = fArOld[id + 1];
            uint32_t iv3 = fArOld[id + 2];
            uint32_t ivOpp = 0;
            if (iv1 == (uint32_t)i) ivOpp = iv3 * 3;
            else if (iv2 == (uint32_t)i) ivOpp = iv4 * 3;
            else if (iv3 == (uint32_t)i) ivOpp = iv1 * 3;
            else ivOpp = iv2 * 3;
            oppx += vArOld[ivOpp];
            oppy += vArOld[ivOpp + 1];
            oppz += vArOld[ivOpp + 2];
        }

        if (nbQuad == countFace) {
            if (count == 4) {
                alpha = 0.5625f;
                beta = 0.09375f;
                gamma = 0.015625f;
            } else {
                beta = 1.5f / (float)(count * count);
                gamma = 0.25f / (float)(count * count);
                alpha = 1.0f - (beta + gamma) * (float)count;
            }
            even[j]     = vArOld[j]     * alpha + avx * beta + oppx * gamma;
            even[j + 1] = vArOld[j + 1] * alpha + avy * beta + oppy * gamma;
            even[j + 2] = vArOld[j + 2] * alpha + avz * beta + oppz * gamma;
            continue;
        }

        if (nbQuad == 0) {
            if (count == 6) {
                beta = 0.0625f;
                alpha = 0.625f;
            } else if (count == 3) {
                beta = 0.1875f;
                alpha = 0.4375f;
            } else {
                beta = 0.375f / (float)count;
                alpha = 0.625f;
            }
            even[j]     = vArOld[j]     * alpha + avx * beta;
            even[j + 1] = vArOld[j + 1] * alpha + avy * beta;
            even[j + 2] = vArOld[j + 2] * alpha + avz * beta;
            continue;
        }

        alpha = 1.0f / (1.0f + (float)count * 0.5f + (float)nbQuad * 0.25f);
        beta = alpha * 0.5f;
        gamma = alpha * 0.25f;
        even[j]     = vArOld[j]     * alpha + avx * beta + oppx * gamma;
        even[j + 1] = vArOld[j + 1] * alpha + avy * beta + oppy * gamma;
        even[j + 2] = vArOld[j + 2] * alpha + avz * beta + oppz * gamma;
    }
}

std::vector<int32_t> applyOddSmooth(Mesh& baseMesh, std::vector<float>& odds, std::vector<float>& colorOut, std::vector<float>& materialOut, std::vector<uint32_t>* fArOut) {
    const auto& fAr = baseMesh.getFaces();
    const auto& feAr = baseMesh.faceEdges;
    OddVertexComputer oddComputer(baseMesh, odds, colorOut, materialOut);

    int nbFaces = baseMesh.getNbFaces();
    for (int i = 0; i < nbFaces; ++i) {
        int id = i * 4;
        uint32_t iv1 = fAr[id];
        uint32_t iv2 = fAr[id + 1];
        uint32_t iv3 = fAr[id + 2];
        uint32_t iv4 = fAr[id + 3];
        bool isQuad = (iv4 != TRI_INDEX);
        uint32_t ivMid1 = 0, ivMid2 = 0, ivMid3 = 0, ivMid4 = 0, ivCen = 0;

        if (isQuad) {
            ivMid1 = oddComputer.computeQuadEdgeVertex(iv1, iv2, iv3, iv4, feAr[id]);
            ivMid2 = oddComputer.computeQuadEdgeVertex(iv2, iv3, iv4, iv1, feAr[id + 1]);
            ivMid3 = oddComputer.computeQuadEdgeVertex(iv3, iv4, iv1, iv2, feAr[id + 2]);
            ivMid4 = oddComputer.computeQuadEdgeVertex(iv4, iv1, iv2, iv3, feAr[id + 3]);
            ivCen  = oddComputer.computeFaceVertex(iv1, iv2, iv3, iv4);
        } else {
            ivMid1 = oddComputer.computeTriangleEdgeVertex(iv1, iv2, iv3, feAr[id]);
            ivMid2 = oddComputer.computeTriangleEdgeVertex(iv2, iv3, iv1, feAr[id + 1]);
            ivMid3 = oddComputer.computeTriangleEdgeVertex(iv3, iv1, iv2, feAr[id + 2]);
        }

        if (!fArOut) continue;

        int idOut = i * 16;
        if (isQuad) {
            (*fArOut)[idOut + 1]  = (*fArOut)[idOut + 4]  = ivMid1;
            (*fArOut)[idOut + 6]  = (*fArOut)[idOut + 9]  = ivMid2;
            (*fArOut)[idOut + 11] = (*fArOut)[idOut + 14] = ivMid3;
            (*fArOut)[idOut + 3]  = (*fArOut)[idOut + 12] = ivMid4;
            (*fArOut)[idOut + 2]  = (*fArOut)[idOut + 7]  = (*fArOut)[idOut + 8] = (*fArOut)[idOut + 13] = ivCen;
            (*fArOut)[idOut]      = iv1;
            (*fArOut)[idOut + 5]  = iv2;
            (*fArOut)[idOut + 10] = iv3;
            (*fArOut)[idOut + 15] = iv4;
        } else {
            (*fArOut)[idOut]     = (*fArOut)[idOut + 5]  = (*fArOut)[idOut + 8]  = ivMid1;
            (*fArOut)[idOut + 1] = (*fArOut)[idOut + 10] = (*fArOut)[idOut + 12] = ivMid2;
            (*fArOut)[idOut + 2] = (*fArOut)[idOut + 6]  = (*fArOut)[idOut + 14] = ivMid3;
            (*fArOut)[idOut + 3] = (*fArOut)[idOut + 7]  = (*fArOut)[idOut + 11] = (*fArOut)[idOut + 15] = TRI_INDEX;
            (*fArOut)[idOut + 4]  = iv1;
            (*fArOut)[idOut + 9]  = iv2;
            (*fArOut)[idOut + 13] = iv3;
        }
    }
    return oddComputer._tagEdges;
}

void fullSubdivision(Mesh& baseMesh, Mesh& newMesh) {
    sculpt_log("[Subdivision::fullSubdivision] Start. baseVerts=%d, baseEdges=%d, baseQuads=%d, baseFaces=%d\n",
              baseMesh.getNbVertices(), baseMesh.getNbEdges(), baseMesh.getNbQuads(), baseMesh.getNbFaces());

    if (baseMesh.vrfStartCount.size() != (size_t)(baseMesh.getNbVertices() * 2) || baseMesh.edges.empty()) {
        sculpt_log("[Subdivision::fullSubdivision] Base topology uninitialized. Calling initTopology()...\n");
        baseMesh.initTopology();
    }

    int nbVertices = baseMesh.getNbVertices() + baseMesh.getNbEdges() + baseMesh.getNbQuads();
    int nbFacesOut = baseMesh.getNbFaces() * 4;

    sculpt_log("[Subdivision::fullSubdivision] Target nbVertices=%d, nbFacesOut=%d\n", nbVertices, nbFacesOut);

    newMesh.getVertices().resize(nbVertices * 3, 0.0f);
    newMesh.getColors().resize(nbVertices * 3, 0.0f);
    newMesh.getMaterials().resize(nbVertices * 3, 0.0f);
    newMesh.getFaces().resize(nbFacesOut * 4, TRI_INDEX);

    sculpt_log("[Subdivision::fullSubdivision] Running applyEvenSmooth...\n");
    applyEvenSmooth(baseMesh, newMesh.getVertices(), newMesh.getColors(), newMesh.getMaterials());

    sculpt_log("[Subdivision::fullSubdivision] Running applyOddSmooth...\n");
    auto tags = applyOddSmooth(baseMesh, newMesh.getVertices(), newMesh.getColors(), newMesh.getMaterials(), &newMesh.getFaces());

    newMesh.nbVerts = nbVertices;
    newMesh.nbFaces = nbFacesOut;

    if (baseMesh.faceGroups.size() == (size_t)baseMesh.getNbFaces()) {
        newMesh.faceGroups.resize(nbFacesOut);
        for (int i = 0; i < baseMesh.getNbFaces(); ++i) {
            uint32_t gid = baseMesh.faceGroups[i];
            newMesh.faceGroups[i * 4 + 0] = gid;
            newMesh.faceGroups[i * 4 + 1] = gid;
            newMesh.faceGroups[i * 4 + 2] = gid;
            newMesh.faceGroups[i * 4 + 3] = gid;
        }
        newMesh.isFaceGroupDirty = true;
    } else {
        newMesh.initFaceGroups();
    }

    sculpt_log("[Subdivision::fullSubdivision] Calling newMesh.postInit()...\n");
    newMesh.postInit();
    sculpt_log("[Subdivision::fullSubdivision] fullSubdivision completed successfully.\n");
}

void partialSubdivision(Mesh& baseMesh, std::vector<float>& vertOut, std::vector<float>& colorOut, std::vector<float>& materialOut) {
    applyEvenSmooth(baseMesh, vertOut, colorOut, materialOut);
    applyOddSmooth(baseMesh, vertOut, colorOut, materialOut, nullptr);
}

} // namespace Subdivision
