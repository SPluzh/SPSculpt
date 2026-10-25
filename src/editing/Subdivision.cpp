#include "editing/Subdivision.h"
#include "common/Constants.h"
#include "common/Logger.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

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
    const std::vector<uint8_t>* _inCreases;
    float _sharpWeight;
    bool _isLinear;
    int _nbVertices;
    std::vector<int32_t> _tagEdges;
    SubdivLevelCache* _cache;
    std::vector<int32_t> _edgeToCacheIdx;

    OddVertexComputer(Mesh& mesh, std::vector<float>& vArOut, std::vector<float>& cArOut, std::vector<float>& mArOut,
                      const std::vector<uint8_t>* inCreases = nullptr, float sharpWeight = 0.0f, bool isLinear = false,
                      SubdivLevelCache* cache = nullptr)
        : _vArOut(vArOut)
        , _cArOut(cArOut)
        , _mArOut(mArOut)
        , _vAr(mesh.getVertices())
        , _cAr(mesh.getColors())
        , _mAr(mesh.getMaterials())
        , _eAr(mesh.edges)
        , _inCreases(inCreases)
        , _sharpWeight(sharpWeight)
        , _isLinear(isLinear)
        , _nbVertices(mesh.getNbVertices())
        , _tagEdges(mesh.getNbEdges(), 0)
        , _cache(cache)
    {
        if (_cache) {
            _cache->clear();
            _cache->edges.reserve(mesh.getNbEdges());
            _cache->quads.reserve(mesh.getNbQuads());
            _edgeToCacheIdx.assign(mesh.getNbEdges(), -1);
        }
    }

    uint32_t computeTriangleEdgeVertex(uint32_t iv1, uint32_t iv2, uint32_t iv3, uint32_t ide) {
        uint32_t id1 = iv1 * 3;
        uint32_t id2 = iv2 * 3;
        uint32_t idOpp = iv3 * 3;
        int32_t testEdge = _tagEdges[ide] - 1;
        uint32_t ivMid = (testEdge == -1) ? (uint32_t)_nbVertices++ : (uint32_t)testEdge;
        uint32_t idMid = ivMid * 3;
        uint32_t edgeValue = (ide < _eAr.size()) ? _eAr[ide] : 2;
        bool isCrease = (_sharpWeight > 0.0f && _inCreases && ide < _inCreases->size() && (*_inCreases)[ide] > 0);
        bool isSharpEdge = (edgeValue == 1 || edgeValue >= 3 || Subdivision::LINEAR || _isLinear || (isCrease && _sharpWeight >= 1.0f));

        if (idMid + 2 >= _vArOut.size()) {
            _vArOut.resize(idMid + 3, 0.0f);
            _cArOut.resize(idMid + 3, 0.0f);
            _mArOut.resize(idMid + 3, 0.0f);
        }

        if (isSharpEdge) {
            if (testEdge != -1) return ivMid;
            _tagEdges[ide] = ivMid + 1;
            if (_cache && ide < _edgeToCacheIdx.size()) {
                _edgeToCacheIdx[ide] = (int32_t)_cache->edges.size();
                SubdivEdgeData ed;
                ed.midVert = ivMid;
                ed.v1 = iv1;
                ed.v2 = iv2;
                ed.wEnd = 0.5f;
                ed.numOpp = 0;
                _cache->edges.push_back(ed);
            }
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
            float s = isCrease ? _sharpWeight : 0.0f;
            float wEnd = 0.375f + 0.125f * s;
            float wOpp = (1.0f - s) * 0.125f;

            _tagEdges[ide] = ivMid + 1;
            if (_cache && ide < _edgeToCacheIdx.size()) {
                _edgeToCacheIdx[ide] = (int32_t)_cache->edges.size();
                SubdivEdgeData ed;
                ed.midVert = ivMid;
                ed.v1 = iv1;
                ed.v2 = iv2;
                ed.wEnd = wEnd;
                ed.opp[0] = iv3;
                ed.wOpp[0] = wOpp;
                ed.numOpp = 1;
                _cache->edges.push_back(ed);
            }
            _vArOut[idMid]     = wOpp * _vAr[idOpp]     + wEnd * (_vAr[id1]     + _vAr[id2]);
            _vArOut[idMid + 1] = wOpp * _vAr[idOpp + 1] + wEnd * (_vAr[id1 + 1] + _vAr[id2 + 1]);
            _vArOut[idMid + 2] = wOpp * _vAr[idOpp + 2] + wEnd * (_vAr[id1 + 2] + _vAr[id2 + 2]);

            _cArOut[idMid]     = wOpp * _cAr[idOpp]     + wEnd * (_cAr[id1]     + _cAr[id2]);
            _cArOut[idMid + 1] = wOpp * _cAr[idOpp + 1] + wEnd * (_cAr[id1 + 1] + _cAr[id2 + 1]);
            _cArOut[idMid + 2] = wOpp * _cAr[idOpp + 2] + wEnd * (_cAr[id1 + 2] + _cAr[id2 + 2]);

            _mArOut[idMid]     = wOpp * _mAr[idOpp]     + wEnd * (_mAr[id1]     + _mAr[id2]);
            _mArOut[idMid + 1] = wOpp * _mAr[idOpp + 1] + wEnd * (_mAr[id1 + 1] + _mAr[id2 + 1]);
            _mArOut[idMid + 2] = wOpp * _mAr[idOpp + 2] + wEnd * (_mAr[id1 + 2] + _mAr[id2 + 2]);
        } else {
            float s = isCrease ? _sharpWeight : 0.0f;
            float wOpp = (1.0f - s) * 0.125f;

            if (_cache && ide < _edgeToCacheIdx.size()) {
                int32_t cIdx = _edgeToCacheIdx[ide];
                if (cIdx >= 0 && cIdx < (int32_t)_cache->edges.size()) {
                    auto& ed = _cache->edges[cIdx];
                    if (ed.numOpp < 4) {
                        ed.opp[ed.numOpp] = iv3;
                        ed.wOpp[ed.numOpp] = wOpp;
                        ed.numOpp++;
                    }
                }
            }
            _vArOut[idMid]     += wOpp * _vAr[idOpp];
            _vArOut[idMid + 1] += wOpp * _vAr[idOpp + 1];
            _vArOut[idMid + 2] += wOpp * _vAr[idOpp + 2];

            _cArOut[idMid]     += wOpp * _cAr[idOpp];
            _cArOut[idMid + 1] += wOpp * _cAr[idOpp + 1];
            _cArOut[idMid + 2] += wOpp * _cAr[idOpp + 2];

            _mArOut[idMid]     += wOpp * _mAr[idOpp];
            _mArOut[idMid + 1] += wOpp * _mAr[idOpp + 1];
            _mArOut[idMid + 2] += wOpp * _mAr[idOpp + 2];
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
        bool isCrease = (_sharpWeight > 0.0f && _inCreases && ide < _inCreases->size() && (*_inCreases)[ide] > 0);
        bool isSharpEdge = (edgeValue == 1 || edgeValue >= 3 || Subdivision::LINEAR || _isLinear || (isCrease && _sharpWeight >= 1.0f));

        if (idMid + 2 >= _vArOut.size()) {
            _vArOut.resize(idMid + 3, 0.0f);
            _cArOut.resize(idMid + 3, 0.0f);
            _mArOut.resize(idMid + 3, 0.0f);
        }

        if (isSharpEdge) {
            if (testEdge != -1) return ivMid;
            _tagEdges[ide] = ivMid + 1;
            if (_cache && ide < _edgeToCacheIdx.size()) {
                _edgeToCacheIdx[ide] = (int32_t)_cache->edges.size();
                SubdivEdgeData ed;
                ed.midVert = ivMid;
                ed.v1 = iv1;
                ed.v2 = iv2;
                ed.wEnd = 0.5f;
                ed.numOpp = 0;
                _cache->edges.push_back(ed);
            }
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
            float s = isCrease ? _sharpWeight : 0.0f;
            float wEnd = 0.375f + 0.125f * s;
            float wOpp = (1.0f - s) * 0.0625f;

            _tagEdges[ide] = ivMid + 1;
            if (_cache && ide < _edgeToCacheIdx.size()) {
                _edgeToCacheIdx[ide] = (int32_t)_cache->edges.size();
                SubdivEdgeData ed;
                ed.midVert = ivMid;
                ed.v1 = iv1;
                ed.v2 = iv2;
                ed.wEnd = wEnd;
                ed.opp[0] = iv3;
                ed.wOpp[0] = wOpp;
                ed.opp[1] = iv4;
                ed.wOpp[1] = wOpp;
                ed.numOpp = 2;
                _cache->edges.push_back(ed);
            }
            _vArOut[idMid]     = wOpp * (_vAr[idOpp]     + _vAr[idOpp2])     + wEnd * (_vAr[id1]     + _vAr[id2]);
            _vArOut[idMid + 1] = wOpp * (_vAr[idOpp + 1] + _vAr[idOpp2 + 1]) + wEnd * (_vAr[id1 + 1] + _vAr[id2 + 1]);
            _vArOut[idMid + 2] = wOpp * (_vAr[idOpp + 2] + _vAr[idOpp2 + 2]) + wEnd * (_vAr[id1 + 2] + _vAr[id2 + 2]);

            _cArOut[idMid]     = wOpp * (_cAr[idOpp]     + _cAr[idOpp2])     + wEnd * (_cAr[id1]     + _cAr[id2]);
            _cArOut[idMid + 1] = wOpp * (_cAr[idOpp + 1] + _cAr[idOpp2 + 1]) + wEnd * (_cAr[id1 + 1] + _cAr[id2 + 1]);
            _cArOut[idMid + 2] = wOpp * (_cAr[idOpp + 2] + _cAr[idOpp2 + 2]) + wEnd * (_cAr[id1 + 2] + _cAr[id2 + 2]);

            _mArOut[idMid]     = wOpp * (_mAr[idOpp]     + _mAr[idOpp2])     + wEnd * (_mAr[id1]     + _mAr[id2]);
            _mArOut[idMid + 1] = wOpp * (_mAr[idOpp + 1] + _mAr[idOpp2 + 1]) + wEnd * (_mAr[id1 + 1] + _mAr[id2 + 1]);
            _mArOut[idMid + 2] = wOpp * (_mAr[idOpp + 2] + _mAr[idOpp2 + 2]) + wEnd * (_mAr[id1 + 2] + _mAr[id2 + 2]);
        } else {
            float s = isCrease ? _sharpWeight : 0.0f;
            float wOpp = (1.0f - s) * 0.0625f;

            if (_cache && ide < _edgeToCacheIdx.size()) {
                int32_t cIdx = _edgeToCacheIdx[ide];
                if (cIdx >= 0 && cIdx < (int32_t)_cache->edges.size()) {
                    auto& ed = _cache->edges[cIdx];
                    if (ed.numOpp + 2 <= 4) {
                        ed.opp[ed.numOpp] = iv3;
                        ed.wOpp[ed.numOpp] = wOpp;
                        ed.opp[ed.numOpp + 1] = iv4;
                        ed.wOpp[ed.numOpp + 1] = wOpp;
                        ed.numOpp += 2;
                    }
                }
            }
            _vArOut[idMid]     += wOpp * (_vAr[idOpp]     + _vAr[idOpp2]);
            _vArOut[idMid + 1] += wOpp * (_vAr[idOpp + 1] + _vAr[idOpp2 + 1]);
            _vArOut[idMid + 2] += wOpp * (_vAr[idOpp + 2] + _vAr[idOpp2 + 2]);

            _cArOut[idMid]     += wOpp * (_cAr[idOpp]     + _cAr[idOpp2]);
            _cArOut[idMid + 1] += wOpp * (_cAr[idOpp + 1] + _cAr[idOpp2 + 1]);
            _cArOut[idMid + 2] += wOpp * (_cAr[idOpp + 2] + _cAr[idOpp2 + 2]);

            _mArOut[idMid]     += wOpp * (_mAr[idOpp]     + _mAr[idOpp2]);
            _mArOut[idMid + 1] += wOpp * (_mAr[idOpp + 1] + _mAr[idOpp2 + 1]);
            _mArOut[idMid + 2] += wOpp * (_mAr[idOpp + 2] + _mAr[idOpp2 + 2]);
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

        if (_cache) {
            SubdivQuadData qd;
            qd.cenVert = ivCen;
            qd.v1 = iv1;
            qd.v2 = iv2;
            qd.v3 = iv3;
            qd.v4 = iv4;
            _cache->quads.push_back(qd);
        }

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

void applyEvenSmooth(Mesh& baseMesh, std::vector<float>& even, std::vector<float>& colorOut, std::vector<float>& materialOut,
                     const std::vector<uint8_t>* inCreases, float sharpWeight, bool isLinear) {
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

        if (Subdivision::LINEAR || isLinear) {
            even[j]     = vArOld[j];
            even[j + 1] = vArOld[j + 1];
            even[j + 2] = vArOld[j + 2];
            continue;
        }

        uint32_t sharpNeighbors[16];
        int numSharp = 0;
        bool vertexHasCrease = false;

        uint32_t startF = vrfStartCount[i * 2];
        uint32_t endF = startF + vrfStartCount[i * 2 + 1];

        for (k = startF; k < endF; ++k) {
            uint32_t idFace = vertRingFace[k] * 4;
            uint32_t i1 = fArOld[idFace];
            uint32_t i2 = fArOld[idFace + 1];
            uint32_t i3 = fArOld[idFace + 2];
            uint32_t i4 = fArOld[idFace + 3];
            bool isTri = (i4 == TRI_INDEX);

            auto checkEdge = [&](uint32_t edgeIdx, uint32_t otherV) {
                if (edgeIdx >= eArOld.size() || otherV == TRI_INDEX) return;
                uint32_t edgeVal = eArOld[edgeIdx];
                bool isCreased = (sharpWeight > 0.0f && inCreases && edgeIdx < inCreases->size() && (*inCreases)[edgeIdx] > 0);
                if (isCreased) vertexHasCrease = true;
                if (edgeVal == 1 || isCreased) {
                    bool found = false;
                    for (int s = 0; s < numSharp; ++s) {
                        if (sharpNeighbors[s] == otherV) { found = true; break; }
                    }
                    if (!found && numSharp < 16) {
                        sharpNeighbors[numSharp++] = otherV;
                    }
                }
            };

            if (i1 == (uint32_t)i) {
                checkEdge(feArOld[idFace], i2);
                checkEdge(feArOld[isTri ? idFace + 2 : idFace + 3], isTri ? i3 : i4);
            } else if (i2 == (uint32_t)i) {
                checkEdge(feArOld[idFace], i1);
                checkEdge(feArOld[idFace + 1], i3);
            } else if (i3 == (uint32_t)i) {
                checkEdge(feArOld[idFace + 1], i2);
                checkEdge(feArOld[idFace + 2], isTri ? i1 : i4);
            } else if (i4 == (uint32_t)i) {
                checkEdge(feArOld[idFace + 2], i3);
                checkEdge(feArOld[idFace + 3], i1);
            }
        }

        float sharpX = vArOld[j];
        float sharpY = vArOld[j + 1];
        float sharpZ = vArOld[j + 2];

        if (numSharp >= 3) {
            sharpX = vArOld[j];
            sharpY = vArOld[j + 1];
            sharpZ = vArOld[j + 2];
        } else if (numSharp == 2) {
            uint32_t idA = sharpNeighbors[0] * 3;
            uint32_t idB = sharpNeighbors[1] * 3;
            sharpX = 0.75f * vArOld[j]     + 0.125f * (vArOld[idA]     + vArOld[idB]);
            sharpY = 0.75f * vArOld[j + 1] + 0.125f * (vArOld[idA + 1] + vArOld[idB + 1]);
            sharpZ = 0.75f * vArOld[j + 2] + 0.125f * (vArOld[idA + 2] + vArOld[idB + 2]);
        } else if (numSharp == 1 && (i < (int)vertOnEdgeOld.size() && vertOnEdgeOld[i])) {
            sharpX = vArOld[j];
            sharpY = vArOld[j + 1];
            sharpZ = vArOld[j + 2];
        }

        if (numSharp > 0 && (!vertexHasCrease || sharpWeight >= 1.0f)) {
            even[j]     = sharpX;
            even[j + 1] = sharpY;
            even[j + 2] = sharpZ;
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
            if (numSharp > 0 && vertexHasCrease) {
                float s = sharpWeight;
                even[j]     = (1.0f - s) * even[j]     + s * sharpX;
                even[j + 1] = (1.0f - s) * even[j + 1] + s * sharpY;
                even[j + 2] = (1.0f - s) * even[j + 2] + s * sharpZ;
            }
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
            if (numSharp > 0 && vertexHasCrease) {
                float s = sharpWeight;
                even[j]     = (1.0f - s) * even[j]     + s * sharpX;
                even[j + 1] = (1.0f - s) * even[j + 1] + s * sharpY;
                even[j + 2] = (1.0f - s) * even[j + 2] + s * sharpZ;
            }
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
            if (numSharp > 0 && vertexHasCrease) {
                float s = sharpWeight;
                even[j]     = (1.0f - s) * even[j]     + s * sharpX;
                even[j + 1] = (1.0f - s) * even[j + 1] + s * sharpY;
                even[j + 2] = (1.0f - s) * even[j + 2] + s * sharpZ;
            }
            continue;
        }

        alpha = 1.0f / (1.0f + (float)count * 0.5f + (float)nbQuad * 0.25f);
        beta = alpha * 0.5f;
        gamma = alpha * 0.25f;
        even[j]     = vArOld[j]     * alpha + avx * beta + oppx * gamma;
        even[j + 1] = vArOld[j + 1] * alpha + avy * beta + oppy * gamma;
        even[j + 2] = vArOld[j + 2] * alpha + avz * beta + oppz * gamma;
        if (numSharp > 0 && vertexHasCrease) {
            float s = sharpWeight;
            even[j]     = (1.0f - s) * even[j]     + s * sharpX;
            even[j + 1] = (1.0f - s) * even[j + 1] + s * sharpY;
            even[j + 2] = (1.0f - s) * even[j + 2] + s * sharpZ;
        }
    }
}

std::vector<int32_t> applyOddSmooth(Mesh& baseMesh, std::vector<float>& odds, std::vector<float>& colorOut, std::vector<float>& materialOut,
                                     std::vector<uint32_t>* fArOut, const std::vector<uint8_t>* inCreases, float sharpWeight, bool isLinear,
                                     SubdivLevelCache* cache) {
    const auto& fAr = baseMesh.getFaces();
    const auto& feAr = baseMesh.faceEdges;
    OddVertexComputer oddComputer(baseMesh, odds, colorOut, materialOut, inCreases, sharpWeight, isLinear, cache);

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

void fullSubdivisionWithCreases(
    Mesh& baseMesh,
    Mesh& newMesh,
    const std::vector<uint8_t>* inCreases,
    std::vector<uint8_t>* outCreases,
    float sharpWeight,
    bool isLinear,
    SubdivLevelCache* cache,
    const std::vector<uint8_t>* inCageEdges,
    std::vector<uint8_t>* outCageEdges
) {
    if (baseMesh.vrfStartCount.size() != (size_t)(baseMesh.getNbVertices() * 2) || baseMesh.edges.empty()) {
        baseMesh.initTopology();
    }

    int nbVertices = baseMesh.getNbVertices() + baseMesh.getNbEdges() + baseMesh.getNbQuads();
    int nbFacesOut = baseMesh.getNbFaces() * 4;

    newMesh.getVertices().resize(nbVertices * 3, 0.0f);
    newMesh.getColors().resize(nbVertices * 3, 0.0f);
    newMesh.getMaterials().resize(nbVertices * 3, 0.0f);
    newMesh.getFaces().resize(nbFacesOut * 4, TRI_INDEX);

    applyEvenSmooth(baseMesh, newMesh.getVertices(), newMesh.getColors(), newMesh.getMaterials(), inCreases, sharpWeight, isLinear);

    auto tags = applyOddSmooth(baseMesh, newMesh.getVertices(), newMesh.getColors(), newMesh.getMaterials(),
                               &newMesh.getFaces(), inCreases, sharpWeight, isLinear, cache);

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

    newMesh.initTopology();

    // Propagate creases and cage edges to sub-edges in newMesh
    if (outCreases || outCageEdges) {
        if (outCreases) outCreases->assign(newMesh.getNbEdges(), 0);
        if (outCageEdges) outCageEdges->assign(newMesh.getNbEdges(), 0);

        const auto& baseFaces = baseMesh.getFaces();
        const auto& baseFaceEdges = baseMesh.faceEdges;
        const auto& newFaceEdges = newMesh.faceEdges;
        const auto& newFaces = newMesh.getFaces();

        std::unordered_set<uint64_t> creasedSubEdges;
        std::unordered_set<uint64_t> cageSubEdges;
        if (outCreases && inCreases && !inCreases->empty()) {
            creasedSubEdges.reserve(inCreases->size() * 2);
        }
        if (outCageEdges) {
            cageSubEdges.reserve((inCageEdges ? inCageEdges->size() : baseMesh.getNbEdges()) * 2);
        }

        int baseNbFaces = baseMesh.getNbFaces();
        for (int f = 0; f < baseNbFaces; ++f) {
            int id = f * 4;
            uint32_t v[4] = { baseFaces[id], baseFaces[id + 1], baseFaces[id + 2], baseFaces[id + 3] };
            bool isQuad = (v[3] != TRI_INDEX);
            int numE = isQuad ? 4 : 3;
            for (int e = 0; e < numE; ++e) {
                uint32_t ide = baseFaceEdges[id + e];
                int32_t ivMid = (ide < tags.size()) ? (tags[ide] - 1) : -1;
                if (ivMid >= 0) {
                    uint32_t va = v[e];
                    uint32_t vb = v[(e + 1) % numE];
                    uint64_t k1 = ((uint64_t)std::min(va, (uint32_t)ivMid) << 32) | std::max(va, (uint32_t)ivMid);
                    uint64_t k2 = ((uint64_t)std::min(vb, (uint32_t)ivMid) << 32) | std::max(vb, (uint32_t)ivMid);

                    if (outCreases && inCreases && ide < inCreases->size() && (*inCreases)[ide] > 0) {
                        creasedSubEdges.insert(k1);
                        creasedSubEdges.insert(k2);
                    }
                    if (outCageEdges) {
                        bool isCage = (!inCageEdges || (ide < inCageEdges->size() && (*inCageEdges)[ide] > 0));
                        if (isCage) {
                            cageSubEdges.insert(k1);
                            cageSubEdges.insert(k2);
                        }
                    }
                }
            }
        }

        int newNbFaces = newMesh.getNbFaces();
        for (int f = 0; f < newNbFaces; ++f) {
            int id = f * 4;
            uint32_t v[4] = { newFaces[id], newFaces[id + 1], newFaces[id + 2], newFaces[id + 3] };
            bool isQuad = (v[3] != TRI_INDEX);
            int numE = isQuad ? 4 : 3;
            for (int e = 0; e < numE; ++e) {
                uint32_t edgeIdx = newFaceEdges[id + e];
                uint32_t va = v[e];
                uint32_t vb = v[(e + 1) % numE];
                uint64_t k = ((uint64_t)std::min(va, vb) << 32) | std::max(va, vb);

                if (outCreases && edgeIdx < outCreases->size() && (*outCreases)[edgeIdx] == 0) {
                    if (creasedSubEdges.find(k) != creasedSubEdges.end()) {
                        (*outCreases)[edgeIdx] = 1;
                    }
                }
                if (outCageEdges && edgeIdx < outCageEdges->size() && (*outCageEdges)[edgeIdx] == 0) {
                    if (cageSubEdges.find(k) != cageSubEdges.end()) {
                        (*outCageEdges)[edgeIdx] = 1;
                    }
                }
            }
        }

        if (outCreases) newMesh.edgeCreases = *outCreases;
        if (outCageEdges) newMesh.edgeIsCage = *outCageEdges;
    }

    newMesh.postInit(false);
}

void fullSubdivision(Mesh& baseMesh, Mesh& newMesh) {
    fullSubdivisionWithCreases(baseMesh, newMesh, nullptr, nullptr, 0.0f, false);
}

void computeDynamicSubdivision(
    Mesh& baseMesh,
    Mesh& outMesh,
    int smoothLevel,
    int flatLevel,
    float creaseLevel
) {
    if (smoothLevel <= 0 && flatLevel <= 0) {
        outMesh = baseMesh;
        outMesh.edgeCreases = baseMesh.edgeCreases;
        outMesh.edgeIsCage = baseMesh.edgeIsCage;
        outMesh.postInit(false);
        return;
    }

    int totalSteps = flatLevel + smoothLevel;
    std::unique_ptr<Mesh> current = std::make_unique<Mesh>(baseMesh);
    current->edgeCreases = baseMesh.edgeCreases;
    if (current->edgeCreases.size() != (size_t)current->getNbEdges()) {
        current->edgeCreases.resize(current->getNbEdges(), 0);
    }

    for (int step = 1; step <= totalSteps; ++step) {
        auto next = std::make_unique<Mesh>();
        bool isLinear = (step <= flatLevel);
        float remaining = creaseLevel - (float)(step - 1);
        float stepSharpness = std::clamp(remaining, 0.0f, 1.0f);

        std::vector<uint8_t> outCreases;
        std::vector<uint8_t> outCageEdges;
        fullSubdivisionWithCreases(
            *current,
            *next,
            &current->edgeCreases,
            &outCreases,
            stepSharpness,
            isLinear,
            nullptr,
            step == 1 ? nullptr : &current->edgeIsCage,
            &outCageEdges
        );

        next->edgeCreases = std::move(outCreases);
        next->edgeIsCage = std::move(outCageEdges);
        current = std::move(next);
    }

    outMesh = *current;
    outMesh.postInit(false);
}

void evaluateCachedSubdivision(
    const Mesh& baseMesh,
    Mesh& outMesh,
    const SubdivLevelCache& cache,
    const std::vector<uint8_t>* inCreases,
    float sharpWeight,
    bool isLinear
) {
    applyEvenSmooth(const_cast<Mesh&>(baseMesh), outMesh.getVertices(), outMesh.getColors(), outMesh.getMaterials(), inCreases, sharpWeight, isLinear);

    const auto& vAr = baseMesh.getVertices();
    const auto& cAr = baseMesh.getColors();
    const auto& mAr = baseMesh.getMaterials();
    auto& vOut = outMesh.getVertices();
    auto& cOut = outMesh.getColors();
    auto& mOut = outMesh.getMaterials();

    int nbEdges = (int)cache.edges.size();
    #pragma omp parallel for schedule(static) if(nbEdges > 500)
    for (int i = 0; i < nbEdges; ++i) {
        const auto& ed = cache.edges[i];
        uint32_t idMid = ed.midVert * 3;
        uint32_t id1 = ed.v1 * 3;
        uint32_t id2 = ed.v2 * 3;
        float wEnd = ed.wEnd;

        float vx = wEnd * (vAr[id1]     + vAr[id2]);
        float vy = wEnd * (vAr[id1 + 1] + vAr[id2 + 1]);
        float vz = wEnd * (vAr[id1 + 2] + vAr[id2 + 2]);

        float cx = wEnd * (cAr[id1]     + cAr[id2]);
        float cy = wEnd * (cAr[id1 + 1] + cAr[id2 + 1]);
        float cz = wEnd * (cAr[id1 + 2] + cAr[id2 + 2]);

        float mx = wEnd * (mAr[id1]     + mAr[id2]);
        float my = wEnd * (mAr[id1 + 1] + mAr[id2 + 1]);
        float mz = wEnd * (mAr[id1 + 2] + mAr[id2 + 2]);

        for (int k = 0; k < ed.numOpp; ++k) {
            uint32_t idOpp = ed.opp[k] * 3;
            float w = ed.wOpp[k];
            vx += w * vAr[idOpp];
            vy += w * vAr[idOpp + 1];
            vz += w * vAr[idOpp + 2];

            cx += w * cAr[idOpp];
            cy += w * cAr[idOpp + 1];
            cz += w * cAr[idOpp + 2];

            mx += w * mAr[idOpp];
            my += w * mAr[idOpp + 1];
            mz += w * mAr[idOpp + 2];
        }

        vOut[idMid]     = vx;
        vOut[idMid + 1] = vy;
        vOut[idMid + 2] = vz;

        cOut[idMid]     = cx;
        cOut[idMid + 1] = cy;
        cOut[idMid + 2] = cz;

        mOut[idMid]     = mx;
        mOut[idMid + 1] = my;
        mOut[idMid + 2] = mz;
    }

    int nbQuads = (int)cache.quads.size();
    #pragma omp parallel for schedule(static) if(nbQuads > 500)
    for (int i = 0; i < nbQuads; ++i) {
        const auto& qd = cache.quads[i];
        uint32_t idCen = qd.cenVert * 3;
        uint32_t id1 = qd.v1 * 3;
        uint32_t id2 = qd.v2 * 3;
        uint32_t id3 = qd.v3 * 3;
        uint32_t id4 = qd.v4 * 3;

        vOut[idCen]     = 0.25f * (vAr[id1]     + vAr[id2]     + vAr[id3]     + vAr[id4]);
        vOut[idCen + 1] = 0.25f * (vAr[id1 + 1] + vAr[id2 + 1] + vAr[id3 + 1] + vAr[id4 + 1]);
        vOut[idCen + 2] = 0.25f * (vAr[id1 + 2] + vAr[id2 + 2] + vAr[id3 + 2] + vAr[id4 + 2]);

        cOut[idCen]     = 0.25f * (cAr[id1]     + cAr[id2]     + cAr[id3]     + cAr[id4]);
        cOut[idCen + 1] = 0.25f * (cAr[id1 + 1] + cAr[id2 + 1] + cAr[id3 + 1] + cAr[id4 + 1]);
        cOut[idCen + 2] = 0.25f * (cAr[id1 + 2] + cAr[id2 + 2] + cAr[id3 + 2] + cAr[id4 + 2]);

        mOut[idCen]     = 0.25f * (mAr[id1]     + mAr[id2]     + mAr[id3]     + mAr[id4]);
        mOut[idCen + 1] = 0.25f * (mAr[id1 + 1] + mAr[id2 + 1] + mAr[id3 + 1] + mAr[id4 + 1]);
        mOut[idCen + 2] = 0.25f * (mAr[id1 + 2] + mAr[id2 + 2] + mAr[id3 + 2] + mAr[id4 + 2]);
    }
}

void partialSubdivision(Mesh& baseMesh, std::vector<float>& vertOut, std::vector<float>& colorOut, std::vector<float>& materialOut) {
    applyEvenSmooth(baseMesh, vertOut, colorOut, materialOut);
    applyOddSmooth(baseMesh, vertOut, colorOut, materialOut, nullptr);
}

} // namespace Subdivision
