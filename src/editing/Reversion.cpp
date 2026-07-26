#include "editing/Reversion.h"
#include "mesh/MeshResolution.h"
#include "common/Constants.h"
#include <algorithm>
#include <vector>

namespace Reversion {

static std::vector<int8_t> detectExtraordinaryVertices(Mesh& mesh) {
    int nbVertices = mesh.getNbVertices();
    const auto& fAr = mesh.getFaces();
    const auto& onEdge = mesh.vertOnEdge;
    const auto& vrvStartCount = mesh.vrvStartCount;
    const auto& vrf = mesh.vertRingFace;
    const auto& vrfStartCount = mesh.vrfStartCount;

    std::vector<int8_t> vExtraTags(nbVertices, 0);

    for (int i = 0; i < nbVertices; ++i) {
        int id = i * 2;
        int len = vrvStartCount[id + 1];
        int startFace = vrfStartCount[id];
        int countFace = vrfStartCount[id + 1];
        bool vBorder = (i < (int)onEdge.size() && onEdge[i] != 0);
        int nbQuad = 0;
        for (int j = startFace, endFace = startFace + countFace; j < endFace; ++j) {
            nbQuad += (fAr[vrf[j] * 4 + 3] == TRI_INDEX) ? 0 : 1;
        }

        if (nbQuad == 0) {
            if ((!vBorder && len != 6) || (vBorder && len != 4))
                vExtraTags[i] = 1;
        } else if (nbQuad == countFace) {
            if ((!vBorder && len != 4) || (vBorder && len != 3))
                vExtraTags[i] = 1;
        } else {
            if (vBorder || len != 5)
                vExtraTags[i] = 1;
        }
    }
    return vExtraTags;
}

static int getSeed(Mesh& mesh, const std::vector<int8_t>& vEvenTags, const std::vector<int8_t>& vExtraTags) {
    int nbVertices = mesh.getNbVertices();
    int i = 0;
    for (i = 0; i < nbVertices; ++i) {
        if (vEvenTags[i] != 0) continue;
        if (vExtraTags[i] == 1) return i;
    }
    const auto& onEdge = mesh.vertOnEdge;
    for (i = 0; i < nbVertices; ++i) {
        if (vEvenTags[i] != 0) continue;
        if (i < (int)onEdge.size() && onEdge[i] != 1) break;
    }
    if (i == nbVertices) return -1;
    for (i = 0; i < nbVertices; ++i) {
        if (vEvenTags[i] == 0) return i;
    }
    return -1;
}

static bool tagVertices(Mesh& mesh, const std::vector<int8_t>& vExtraTags, std::vector<int8_t>& vEvenTags) {
    uint32_t tagFlag = mesh.getTagFlag();
    auto& vFlags = mesh.vertTagFlags;
    if (vFlags.size() < (size_t)mesh.getNbVertices()) vFlags.resize(mesh.getNbVertices(), 0);

    const auto& vrvSC = mesh.vrvStartCount;
    const auto& vrv = mesh.vertRingVert;
    const auto& onEdge = mesh.vertOnEdge;

    int vSeed = getSeed(mesh, vEvenTags, vExtraTags);
    if (vSeed < 0) return false;

    vEvenTags[vSeed] = 1;
    std::vector<uint32_t> stack(mesh.getNbVertices(), 0);
    stack[0] = (uint32_t)vSeed;
    int curStack = 1;

    while (curStack > 0) {
        uint32_t idVert = stack[--curStack];
        uint32_t start = vrvSC[idVert * 2];
        uint32_t end = start + vrvSC[idVert * 2 + 1];
        uint32_t stamp = ++tagFlag;

        for (uint32_t i = start; i < end; ++i) {
            uint32_t oddi = vrv[i];
            vFlags[oddi] = stamp;
            if (vEvenTags[oddi] == 1) {
                return false;
            }
            vEvenTags[oddi] = -1;
            vFlags[oddi] = stamp;
        }

        stamp = ++tagFlag;
        for (uint32_t i = start; i < end; ++i) {
            uint32_t oddId = vrv[i];
            bool isBorder = (oddId < onEdge.size() && onEdge[oddId]);
            if (vExtraTags[oddId] != 0 && !isBorder) {
                return false;
            }
            uint32_t oddStart = vrvSC[oddId * 2];
            uint32_t oddEnd = oddStart + vrvSC[oddId * 2 + 1];

            for (uint32_t j = oddStart; j < oddEnd; ++j) {
                uint32_t evenj = vrv[j];
                if (evenj == idVert) continue;
                if (vFlags[evenj] >= (stamp - 1)) continue;
                vFlags[evenj] = stamp;
                if (vEvenTags[evenj] != 0) continue;

                uint32_t oppStart = vrvSC[evenj * 2];
                uint32_t oppEnd = oppStart + vrvSC[evenj * 2 + 1];
                int nbOdd = 0;
                for (uint32_t k = oppStart; k < oppEnd; ++k) {
                    if (vFlags[vrv[k]] == (stamp - 1))
                        nbOdd++;
                }
                if (nbOdd == 2) {
                    vEvenTags[evenj] = -1;
                } else {
                    vEvenTags[evenj] = 1;
                    stack[curStack++] = evenj;
                }
            }
        }
    }
    return true;
}

static bool tagEvenVertices(Mesh& mesh, std::vector<int8_t>& vEvenTags) {
    int nbVertices = mesh.getNbVertices();
    auto vExtraTags = detectExtraordinaryVertices(mesh);
    bool running = true;
    while (running) {
        if (!tagVertices(mesh, vExtraTags, vEvenTags))
            return false;
        running = false;
        for (int i = 0; i < nbVertices; ++i) {
            if (vEvenTags[i] == 0) {
                running = true;
                break;
            }
        }
    }
    return true;
}

static bool createFaces(Mesh& baseMesh, MeshResolution& newMesh, const std::vector<int8_t>& vEvenTags, std::vector<int32_t>& triFaceOrQuadCenter) {
    const auto& feAr = baseMesh.faceEdges;
    const auto& fArUp = baseMesh.getFaces();
    std::vector<int32_t> tagEdges(baseMesh.getNbEdges(), 0);
    int nbFaces = baseMesh.getNbFaces();
    int acc = 0;
    std::vector<uint32_t> centerQuadUp(baseMesh.getNbVertices(), 0);

    std::vector<uint32_t> fArDown(nbFaces, TRI_INDEX);

    for (int i = 0; i < nbFaces; ++i) {
        int j = i * 4;
        uint32_t iv1 = fArUp[j];
        uint32_t iv2 = fArUp[j + 1];
        uint32_t iv3 = fArUp[j + 2];
        uint32_t iv4 = fArUp[j + 3];
        int tag1 = vEvenTags[iv1];
        int tag2 = vEvenTags[iv2];
        int tag3 = vEvenTags[iv3];

        if (iv4 == TRI_INDEX) {
            if (tag1 + tag2 + tag3 == -3) {
                triFaceOrQuadCenter[acc++] = i;
                continue;
            }
            if (tag1 == 1) tagEdges[feAr[j + 1]] = iv1 + 1;
            else if (tag2 == 1) tagEdges[feAr[j + 2]] = iv2 + 1;
            else if (tag3 == 1) tagEdges[feAr[j]] = iv3 + 1;
            continue;
        }

        uint32_t ivCorner = 0;
        uint32_t ivCenter = 0;
        int32_t oppEdge = 0;
        if (tag1 == 1) {
            ivCorner = iv1;
            ivCenter = iv3;
            oppEdge = tagEdges[feAr[j + 1]] - 1;
            tagEdges[feAr[j + 2]] = iv1 + 1;
        } else if (tag2 == 1) {
            ivCorner = iv2;
            ivCenter = iv4;
            oppEdge = tagEdges[feAr[j + 2]] - 1;
            tagEdges[feAr[j + 3]] = iv2 + 1;
        } else if (tag3 == 1) {
            ivCorner = iv3;
            ivCenter = iv1;
            oppEdge = tagEdges[feAr[j + 3]] - 1;
            tagEdges[feAr[j]] = iv3 + 1;
        } else {
            ivCorner = iv4;
            ivCenter = iv2;
            oppEdge = tagEdges[feAr[j]] - 1;
            tagEdges[feAr[j + 1]] = iv4 + 1;
        }

        int32_t quad = (int32_t)centerQuadUp[ivCenter] - 1;
        if (quad < 0) {
            triFaceOrQuadCenter[acc] = -(int32_t)ivCenter - 1;
            fArDown[acc * 4 + 3] = ivCorner;
            centerQuadUp[ivCenter] = ++acc;
            continue;
        }

        int idQuad = quad * 4;
        if (oppEdge < 0) {
            if (fArDown[idQuad + 2] >= (TRI_INDEX - 1)) {
                fArDown[idQuad + 2] = ivCorner;
                fArDown[idQuad] = TRI_INDEX - 1;
            } else if (fArDown[idQuad] == TRI_INDEX) {
                fArDown[idQuad + 1] = ivCorner;
            } else {
                fArDown[idQuad + 1] = fArDown[idQuad + 2];
                fArDown[idQuad + 2] = ivCorner;
            }
        } else {
            if (fArDown[idQuad + 1] == (uint32_t)oppEdge) {
                fArDown[idQuad] = ivCorner;
            } else {
                fArDown[idQuad] = fArDown[idQuad + 1];
                if (fArDown[idQuad + 2] == (uint32_t)oppEdge) {
                    fArDown[idQuad + 1] = ivCorner;
                } else {
                    fArDown[idQuad + 1] = fArDown[idQuad + 2];
                    fArDown[idQuad + 2] = ivCorner;
                }
            }
        }
    }

    int nbFacesDown = nbFaces / 4;
    fArDown.resize(nbFacesDown * 4);

    for (int i = 0; i < nbFacesDown; ++i) {
        int cen = triFaceOrQuadCenter[i];
        int idFace = i * 4;
        if (cen < 0) {
            uint32_t cmp = TRI_INDEX - 1;
            if (fArDown[idFace] >= cmp || fArDown[idFace + 1] >= TRI_INDEX || fArDown[idFace + 2] >= TRI_INDEX)
                return false;
            continue;
        }
        int id = cen * 4;
        fArDown[idFace]     = tagEdges[feAr[id]] - 1;
        fArDown[idFace + 1] = tagEdges[feAr[id + 1]] - 1;
        fArDown[idFace + 2] = tagEdges[feAr[id + 2]] - 1;
    }

    newMesh.getFaces() = fArDown;
    newMesh.nbFaces = nbFacesDown;
    return true;
}

static void createVertices(Mesh& baseMesh, MeshResolution& newMesh, const std::vector<int32_t>& triFaceOrQuadCenter) {
    uint32_t acc = 0;
    std::vector<uint32_t> vertexMapUp(baseMesh.getNbVertices(), 0);

    auto& fArDown = newMesh.getFaces();
    std::vector<int32_t> tagVert(baseMesh.getNbVertices(), 0);
    int len = newMesh.getNbFaces() * 4;

    for (int i = 0; i < len; ++i) {
        uint32_t iv = fArDown[i];
        if (iv == TRI_INDEX) continue;

        int32_t tag = tagVert[iv] - 1;
        if (tag == -1) {
            tag = acc++;
            tagVert[iv] = tag + 1;
            vertexMapUp[tag] = iv;
        }
        fArDown[i] = tag;
    }

    newMesh.getVertices().resize(acc * 3, 0.0f);
    const auto& fArUp = baseMesh.getFaces();
    const auto& vrf = baseMesh.vertRingFace;
    const auto& vrfStartCount = baseMesh.vrfStartCount;
    std::vector<uint8_t> tagMid(baseMesh.getNbVertices(), 0);

    int nbFacesDown = newMesh.getNbFaces();
    for (int i = 0; i < nbFacesDown; ++i) {
        int iCenter = triFaceOrQuadCenter[i];
        uint32_t mid1 = TRI_INDEX, mid2 = TRI_INDEX, mid3 = TRI_INDEX, mid4 = TRI_INDEX, mid5 = TRI_INDEX;

        if (iCenter >= 0) {
            int id = iCenter * 4;
            mid1 = fArUp[id + 1];
            mid2 = fArUp[id + 2];
            mid3 = fArUp[id];
        } else {
            mid5 = -iCenter - 1;
            int idQuadDown = i * 4;
            uint32_t corner1 = vertexMapUp[fArDown[idQuadDown]];
            uint32_t corner2 = vertexMapUp[fArDown[idQuadDown + 1]];
            uint32_t corner3 = vertexMapUp[fArDown[idQuadDown + 2]];
            uint32_t corner4 = vertexMapUp[fArDown[idQuadDown + 3]];
            uint32_t start = vrfStartCount[mid5 * 2];
            uint32_t end = start + 4;
            for (uint32_t j = start; j < end; ++j) {
                uint32_t idQuad = vrf[j] * 4;
                uint32_t id1 = fArUp[idQuad];
                uint32_t id2 = fArUp[idQuad + 1];
                uint32_t id3 = fArUp[idQuad + 2];
                uint32_t id4 = fArUp[idQuad + 3];
                if (id1 == corner1) mid1 = id2;
                else if (id2 == corner1) mid1 = id3;
                else if (id3 == corner1) mid1 = id4;
                else if (id4 == corner1) mid1 = id1;

                if (id1 == corner2) mid2 = id2;
                else if (id2 == corner2) mid2 = id3;
                else if (id3 == corner2) mid2 = id4;
                else if (id4 == corner2) mid2 = id1;

                if (id1 == corner3) mid3 = id2;
                else if (id2 == corner3) mid3 = id3;
                else if (id3 == corner3) mid3 = id4;
                else if (id4 == corner3) mid3 = id1;

                if (id1 == corner4) mid4 = id2;
                else if (id2 == corner4) mid4 = id3;
                else if (id3 == corner4) mid4 = id4;
                else if (id4 == corner4) mid4 = id1;
            }
        }

        if (mid1 != TRI_INDEX && tagMid[mid1] == 0) { tagMid[mid1] = 1; vertexMapUp[acc++] = mid1; }
        if (mid2 != TRI_INDEX && tagMid[mid2] == 0) { tagMid[mid2] = 1; vertexMapUp[acc++] = mid2; }
        if (mid3 != TRI_INDEX && tagMid[mid3] == 0) { tagMid[mid3] = 1; vertexMapUp[acc++] = mid3; }
        if (mid4 != TRI_INDEX && tagMid[mid4] == 0) { tagMid[mid4] = 1; vertexMapUp[acc++] = mid4; }
        if (mid5 != TRI_INDEX && tagMid[mid5] == 0) { tagMid[mid5] = 1; vertexMapUp[acc++] = mid5; }
    }

    vertexMapUp.resize(acc);
    newMesh.nbVerts = acc;
    newMesh.setVerticesMapping(vertexMapUp);
}

static void copyVerticesData(MeshResolution& baseMesh, MeshResolution& newMesh) {
    const auto& vArUp = baseMesh.getVertices();
    const auto& cArUp = baseMesh.getColors();
    const auto& mArUp = baseMesh.getMaterials();

    int nbVertices = newMesh.getNbVertices();
    newMesh.getVertices().resize(nbVertices * 3);
    newMesh.getColors().resize(nbVertices * 3);
    newMesh.getMaterials().resize(nbVertices * 3);

    auto& vArDown = newMesh.getVertices();
    auto& cArDown = newMesh.getColors();
    auto& mArDown = newMesh.getMaterials();

    auto& vertexMapUp = newMesh.getVerticesMapping();
    int i = 0;
    for (i = 0; i < nbVertices; ++i) {
        if (vertexMapUp[i] >= (uint32_t)nbVertices)
            break;
    }

    if (i == nbVertices) {
        auto& fArDown = newMesh.getFaces();
        size_t nb = fArDown.size();
        for (size_t k = 0; k < nb; ++k) {
            uint32_t idv = fArDown[k];
            if (idv != TRI_INDEX)
                fArDown[k] = vertexMapUp[idv];
        }
        for (int k = 0; k < nbVertices; ++k)
            vertexMapUp[k] = k;

        std::copy(vArUp.begin(), vArUp.begin() + nbVertices * 3, vArDown.begin());
        std::copy(cArUp.begin(), cArUp.begin() + nbVertices * 3, cArDown.begin());
        std::copy(mArUp.begin(), mArUp.begin() + nbVertices * 3, mArDown.begin());
        newMesh.setEvenMapping(false);
    } else {
        newMesh.setEvenMapping(true);
        for (int k = 0; k < nbVertices; ++k) {
            int id = k * 3;
            int idUp = vertexMapUp[k] * 3;
            vArDown[id]     = vArUp[idUp];
            vArDown[id + 1] = vArUp[idUp + 1];
            vArDown[id + 2] = vArUp[idUp + 2];
            cArDown[id]     = cArUp[idUp];
            cArDown[id + 1] = cArUp[idUp + 1];
            cArDown[id + 2] = cArUp[idUp + 2];
            mArDown[id]     = mArUp[idUp];
            mArDown[id + 1] = mArUp[idUp + 1];
            mArDown[id + 2] = mArUp[idUp + 2];
        }
    }
}

bool computeReverse(MeshResolution& baseMesh, MeshResolution& newMesh) {
    int nbFaces = baseMesh.getNbFaces();
    if (nbFaces % 4 != 0) return false;

    std::vector<int8_t> vEvenTags(baseMesh.getNbVertices(), 0);
    if (!tagEvenVertices(baseMesh, vEvenTags))
        return false;

    std::vector<int32_t> triFaceOrQuadCenter(nbFaces / 4, 0);
    if (!createFaces(baseMesh, newMesh, vEvenTags, triFaceOrQuadCenter))
        return false;

    createVertices(baseMesh, newMesh, triFaceOrQuadCenter);
    copyVerticesData(baseMesh, newMesh);

    newMesh.initTopology();
    return true;
}

} // namespace Reversion
