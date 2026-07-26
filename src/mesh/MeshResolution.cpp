#include "mesh/MeshResolution.h"
#include "mesh/NormalCalc.h"
#include "editing/Subdivision.h"
#include <algorithm>
#include <cmath>
#include <cstring>

MeshResolution::MeshResolution(const Mesh& other, bool keepMesh)
    : Mesh(other)
{
    if (!keepMesh) {
        // If not keeping mesh, re-allocate empty structure
        verts.clear();
        faces.clear();
        colors.clear();
        materials.clear();
        normals.clear();
        nbVerts = 0;
        nbFaces = 0;
    }
}

void MeshResolution::higherSynthesis(MeshResolution& meshDown) {
    meshDown.computePartialSubdivision(this->verts, this->colors, this->materials, this->getNbVertices());
    if (this->vrfStartCount.size() != (size_t)(this->getNbVertices() * 2) || this->vertRingFace.empty()) {
        this->initTopology();
    }
    updateFaceNormalsAndBoxes(
        this->verts.data(), this->nbVerts,
        this->faces.data(), this->nbFaces,
        nullptr, -1,
        this->faceNormals.data(),
        this->faceBoxes.data(),
        this->faceCenters.data()
    );
    updateVertexNormals(
        nullptr, -1, this->nbVerts,
        this->vrfStartCount.data(),
        this->vertRingFace.data(),
        this->faceNormals.data(),
        this->normals.data()
    );
    this->applyDetails();
    this->isDirty = true;
    this->isVertexDirty = true;
}

void MeshResolution::lowerAnalysis(MeshResolution& meshUp) {
    this->copyDataFromHigherRes(meshUp);
    int nbVerticesUp = meshUp.getNbVertices();

    std::vector<float> subdVerts(nbVerticesUp * 3, 0.0f);
    std::vector<float> subdColors(nbVerticesUp * 3, 0.0f);
    std::vector<float> subdMaterials(nbVerticesUp * 3, 0.0f);

    this->computePartialSubdivision(subdVerts, subdColors, subdMaterials, nbVerticesUp);
    if (meshUp.vrfStartCount.size() != (size_t)(meshUp.getNbVertices() * 2) || meshUp.vertRingFace.empty()) {
        meshUp.initTopology();
    }
    updateFaceNormalsAndBoxes(
        meshUp.verts.data(), meshUp.nbVerts,
        meshUp.faces.data(), meshUp.nbFaces,
        nullptr, -1,
        meshUp.faceNormals.data(),
        meshUp.faceBoxes.data(),
        meshUp.faceCenters.data()
    );
    updateVertexNormals(
        nullptr, -1, meshUp.nbVerts,
        meshUp.vrfStartCount.data(),
        meshUp.vertRingFace.data(),
        meshUp.faceNormals.data(),
        meshUp.normals.data()
    );
    meshUp.computeDetails(subdVerts, subdColors, subdMaterials, nbVerticesUp);
    this->isDirty = true;
    this->isVertexDirty = true;
}

void MeshResolution::copyDataFromHigherRes(MeshResolution& meshUp) {
    auto& vArDown = this->verts;
    auto& cArDown = this->colors;
    auto& mArDown = this->materials;
    int nbVertices = this->getNbVertices();

    const auto& vArUp = meshUp.verts;
    const auto& cArUp = meshUp.colors;
    const auto& mArUp = meshUp.materials;

    if (vArDown.size() < (size_t)nbVertices * 3) vArDown.resize(nbVertices * 3);
    if (cArDown.size() < (size_t)nbVertices * 3) cArDown.resize(nbVertices * 3);
    if (mArDown.size() < (size_t)nbVertices * 3) mArDown.resize(nbVertices * 3);

    if (!this->evenMapping) {
        std::copy(vArUp.begin(), vArUp.begin() + nbVertices * 3, vArDown.begin());
        std::copy(cArUp.begin(), cArUp.begin() + nbVertices * 3, cArDown.begin());
        std::copy(mArUp.begin(), mArUp.begin() + nbVertices * 3, mArDown.begin());
    } else {
        const auto& vm = this->vertMapping;
        for (int i = 0; i < nbVertices; ++i) {
            int id = i * 3;
            int idUp = vm[i] * 3;
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

void MeshResolution::computePartialSubdivision(std::vector<float>& subdVerts,
                                                std::vector<float>& subdColors,
                                                std::vector<float>& subdMaterials,
                                                int nbVerticesUp) {
    if (vertMapping.empty()) {
        Subdivision::partialSubdivision(*this, subdVerts, subdColors, subdMaterials);
        return;
    }

    std::vector<float> tempVerts(nbVerticesUp * 3, 0.0f);
    std::vector<float> tempColors(nbVerticesUp * 3, 0.0f);
    std::vector<float> tempMaterials(nbVerticesUp * 3, 0.0f);

    Subdivision::partialSubdivision(*this, tempVerts, tempColors, tempMaterials);

    int startMapping = this->evenMapping ? 0 : this->getNbVertices();
    if (startMapping > 0) {
        std::copy(tempVerts.begin(), tempVerts.begin() + startMapping * 3, subdVerts.begin());
        std::copy(tempColors.begin(), tempColors.begin() + startMapping * 3, subdColors.begin());
        std::copy(tempMaterials.begin(), tempMaterials.begin() + startMapping * 3, subdMaterials.begin());
    }

    for (int i = startMapping; i < nbVerticesUp; ++i) {
        int id = i * 3;
        int idUp = vertMapping[i] * 3;
        subdVerts[idUp]     = tempVerts[id];
        subdVerts[idUp + 1] = tempVerts[id + 1];
        subdVerts[idUp + 2] = tempVerts[id + 2];
        subdColors[idUp]     = tempColors[id];
        subdColors[idUp + 1] = tempColors[id + 1];
        subdColors[idUp + 2] = tempColors[id + 2];
        subdMaterials[idUp]     = tempMaterials[id];
        subdMaterials[idUp + 1] = tempMaterials[id + 1];
        subdMaterials[idUp + 2] = tempMaterials[id + 2];
    }
}

void MeshResolution::applyDetails() {
    int nbVerticesUp = this->getNbVertices();
    if (detailsXYZ.size() < (size_t)nbVerticesUp * 3) detailsXYZ.resize(nbVerticesUp * 3, 0.0f);
    if (detailsRGB.size() < (size_t)nbVerticesUp * 3) detailsRGB.resize(nbVerticesUp * 3, 0.0f);
    if (detailsPBR.size() < (size_t)nbVerticesUp * 3) detailsPBR.resize(nbVerticesUp * 3, 0.0f);

    const auto& vrvStartCountUp = this->vrvStartCount;
    const auto& vertRingVertUp = this->vertRingVert;

    auto& vArUp = this->verts;
    auto& nArUp = this->normals;
    auto& cArUp = this->colors;
    auto& mArUp = this->materials;

    std::vector<float> vArTemp = vArUp;

    const auto& dAr = this->detailsXYZ;
    const auto& dColorAr = this->detailsRGB;
    const auto& dMaterialAr = this->detailsPBR;

    #pragma omp parallel for schedule(static) if(nbVerticesUp > 2000)
    for (int i = 0; i < nbVerticesUp; ++i) {
        int j = i * 3;

        cArUp[j]     = std::clamp(cArUp[j]     + dColorAr[j],     0.0f, 1.0f);
        cArUp[j + 1] = std::clamp(cArUp[j + 1] + dColorAr[j + 1], 0.0f, 1.0f);
        cArUp[j + 2] = std::clamp(cArUp[j + 2] + dColorAr[j + 2], 0.0f, 1.0f);

        mArUp[j]     = std::clamp(mArUp[j]     + dMaterialAr[j],     0.0f, 1.0f);
        mArUp[j + 1] = std::clamp(mArUp[j + 1] + dMaterialAr[j + 1], 0.0f, 1.0f);
        mArUp[j + 2] = std::clamp(mArUp[j + 2] + dMaterialAr[j + 2], 0.0f, 1.0f);

        float vx = vArTemp[j];
        float vy = vArTemp[j + 1];
        float vz = vArTemp[j + 2];

        float nx = nArUp[j];
        float ny = nArUp[j + 1];
        float nz = nArUp[j + 2];

        float len = nx * nx + ny * ny + nz * nz;
        if (len == 0.0f) continue;
        len = 1.0f / std::sqrt(len);
        nx *= len; ny *= len; nz *= len;

        if (vrvStartCountUp[i * 2 + 1] == 0) continue;
        int k = vertRingVertUp[vrvStartCountUp[i * 2]] * 3;
        float tx = vArTemp[k]     - vx;
        float ty = vArTemp[k + 1] - vy;
        float tz = vArTemp[k + 2] - vz;

        len = tx * nx + ty * ny + tz * nz;
        tx -= nx * len;
        ty -= ny * len;
        tz -= nz * len;

        len = tx * tx + ty * ty + tz * tz;
        if (len == 0.0f) continue;
        len = 1.0f / std::sqrt(len);
        tx *= len; ty *= len; tz *= len;

        float bix = ny * tz - nz * ty;
        float biy = nz * tx - nx * tz;
        float biz = nx * ty - ny * tx;

        float dx = dAr[j];
        float dy = dAr[j + 1];
        float dz = dAr[j + 2];

        vArUp[j]     = vx + nx * dx + tx * dy + bix * dz;
        vArUp[j + 1] = vy + ny * dx + ty * dy + biy * dz;
        vArUp[j + 2] = vz + nz * dx + tz * dy + biz * dz;
    }
}

void MeshResolution::computeDetails(const std::vector<float>& subdVerts,
                                    const std::vector<float>& subdColors,
                                    const std::vector<float>& subdMaterials,
                                    int nbVerticesUp) {
    const auto& vrvStartCountUp = this->vrvStartCount;
    const auto& vertRingVertUp = this->vertRingVert;

    const auto& vArUp = this->verts;
    const auto& nArUp = this->normals;
    const auto& cArUp = this->colors;
    const auto& mArUp = this->materials;
    int nbVertices = this->getNbVertices();

    detailsXYZ.assign(nbVerticesUp * 3, 0.0f);
    detailsRGB.assign(nbVerticesUp * 3, 0.0f);
    detailsPBR.assign(nbVerticesUp * 3, 0.0f);

    auto& dAr = detailsXYZ;
    auto& dColorAr = detailsRGB;
    auto& dMaterialAr = detailsPBR;

    #pragma omp parallel for schedule(static) if(nbVertices > 2000)
    for (int i = 0; i < nbVertices; ++i) {
        int j = i * 3;

        dColorAr[j]     = cArUp[j]     - subdColors[j];
        dColorAr[j + 1] = cArUp[j + 1] - subdColors[j + 1];
        dColorAr[j + 2] = cArUp[j + 2] - subdColors[j + 2];

        dMaterialAr[j]     = mArUp[j]     - subdMaterials[j];
        dMaterialAr[j + 1] = mArUp[j + 1] - subdMaterials[j + 1];
        dMaterialAr[j + 2] = mArUp[j + 2] - subdMaterials[j + 2];

        float nx = nArUp[j];
        float ny = nArUp[j + 1];
        float nz = nArUp[j + 2];

        float len = nx * nx + ny * ny + nz * nz;
        if (len == 0.0f) continue;
        len = 1.0f / std::sqrt(len);
        nx *= len; ny *= len; nz *= len;

        if (vrvStartCountUp[i * 2 + 1] == 0) continue;
        int k = vertRingVertUp[vrvStartCountUp[i * 2]] * 3;
        float tx = subdVerts[k]     - subdVerts[j];
        float ty = subdVerts[k + 1] - subdVerts[j + 1];
        float tz = subdVerts[k + 2] - subdVerts[j + 2];

        len = tx * nx + ty * ny + tz * nz;
        tx -= nx * len;
        ty -= ny * len;
        tz -= nz * len;

        len = tx * tx + ty * ty + tz * tz;
        if (len == 0.0f) continue;
        len = 1.0f / std::sqrt(len);
        tx *= len; ty *= len; tz *= len;

        float bix = ny * tz - nz * ty;
        float biy = nz * tx - nx * tz;
        float biz = nx * ty - ny * tx;

        float dx = vArUp[j]     - subdVerts[j];
        float dy = vArUp[j + 1] - subdVerts[j + 1];
        float dz = vArUp[j + 2] - subdVerts[j + 2];

        dAr[j]     = nx * dx + ny * dy + nz * dz;
        dAr[j + 1] = tx * dx + ty * dy + tz * dz;
        dAr[j + 2] = bix * dx + biy * dy + biz * dz;
    }
}
