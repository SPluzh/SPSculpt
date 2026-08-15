#include "mesh/NormalCalc.h"
#include "common/Constants.h"
#include <algorithm>

#include <cmath>


void updateFaceNormalsAndBoxes(
    const float* verts, int nbVerts,
    const uint32_t* faces, int nbFaces,
    const uint32_t* iFaces, int nbIFaces,
    float* faceNormals,
    float* faceBoxes,
    float* faceCenters
) {
    bool full = (nbIFaces < 0 || (iFaces == nullptr && nbIFaces != 0));
    int loopCount = full ? nbFaces : nbIFaces;

    #pragma omp parallel for schedule(static) if(loopCount > 500)
    for (int i = 0; i < loopCount; ++i) {
        uint32_t ind = full ? i : iFaces[i];
        if (ind >= (uint32_t)nbFaces) continue;

        uint32_t idTri = ind * 3;
        uint32_t idFace = ind * 4;
        uint32_t idBox = ind * 6;

        uint32_t f1 = faces[idFace];
        uint32_t f2 = faces[idFace + 1];
        uint32_t f3 = faces[idFace + 2];
        uint32_t f4 = faces[idFace + 3];
        if (f1 == UINT32_MAX || f2 == UINT32_MAX || f3 == UINT32_MAX ||
            f1 >= (uint32_t)nbVerts || f2 >= (uint32_t)nbVerts || f3 >= (uint32_t)nbVerts) continue;

        uint32_t ind1 = f1 * 3;
        uint32_t ind2 = f2 * 3;
        uint32_t ind3 = f3 * 3;
        bool isQuad = (f4 != TRI_INDEX && f4 != UINT32_MAX && f4 < (uint32_t)nbVerts);
        uint32_t ind4 = isQuad ? f4 * 3 : 0;

        float v1x = verts[ind1];
        float v1y = verts[ind1 + 1];
        float v1z = verts[ind1 + 2];
        float v2x = verts[ind2];
        float v2y = verts[ind2 + 1];
        float v2z = verts[ind2 + 2];
        float v3x = verts[ind3];
        float v3y = verts[ind3 + 1];
        float v3z = verts[ind3 + 2];

        // compute normals
        float ax = v2x - v1x;
        float ay = v2y - v1y;
        float az = v2z - v1z;
        float bx = v3x - v1x;
        float by = v3y - v1y;
        float bz = v3z - v1z;
        float crx = ay * bz - az * by;
        float cry = az * bx - ax * bz;
        float crz = ax * by - ay * bx;

        // compute boxes
        float xmin = v1x < v2x ? (v1x < v3x ? v1x : v3x) : (v2x < v3x ? v2x : v3x);
        float xmax = v1x > v2x ? (v1x > v3x ? v1x : v3x) : (v2x > v3x ? v2x : v3x);
        float ymin = v1y < v2y ? (v1y < v3y ? v1y : v3y) : (v2y < v3y ? v2y : v3y);
        float ymax = v1y > v2y ? (v1y > v3y ? v1y : v3y) : (v2y > v3y ? v2y : v3y);
        float zmin = v1z < v2z ? (v1z < v3z ? v1z : v3z) : (v2z < v3z ? v2z : v3z);
        float zmax = v1z > v2z ? (v1z > v3z ? v1z : v3z) : (v2z > v3z ? v2z : v3z);

        if (isQuad) {
            float v4x = verts[ind4];
            float v4y = verts[ind4 + 1];
            float v4z = verts[ind4 + 2];
            if (v4x < xmin) xmin = v4x;
            if (v4x > xmax) xmax = v4x;
            if (v4y < ymin) ymin = v4y;
            if (v4y > ymax) ymax = v4y;
            if (v4z < zmin) zmin = v4z;
            if (v4z > zmax) zmax = v4z;
            
            float ax2 = v3x - v4x;
            float ay2 = v3y - v4y;
            float az2 = v3z - v4z;
            crx += ay2 * bz - az2 * by;
            cry += az2 * bx - ax2 * bz;
            crz += ax2 * by - ay2 * bx;
        }

        // normals
        faceNormals[idTri] = crx;
        faceNormals[idTri + 1] = cry;
        faceNormals[idTri + 2] = crz;
        // boxes
        faceBoxes[idBox] = xmin;
        faceBoxes[idBox + 1] = ymin;
        faceBoxes[idBox + 2] = zmin;
        faceBoxes[idBox + 3] = xmax;
        faceBoxes[idBox + 4] = ymax;
        faceBoxes[idBox + 5] = zmax;
        // compute centers
        faceCenters[idTri] = (xmin + xmax) * 0.5f;
        faceCenters[idTri + 1] = (ymin + ymax) * 0.5f;
        faceCenters[idTri + 2] = (zmin + zmax) * 0.5f;
    }
}

void updateVertexNormals(
    const uint32_t* iVerts, int nbIVerts, int totalNbVerts,
    const uint32_t* vrfStartCount,
    const uint32_t* vertRingFace,
    const float* faceNormals,
    float* outNormals,
    int totalNbFaces
) {
    if (!vrfStartCount || !vertRingFace || !faceNormals || !outNormals || totalNbVerts <= 0) return;

    bool full = (nbIVerts < 0 || (iVerts == nullptr && nbIVerts != 0));
    int loopCount = full ? totalNbVerts : nbIVerts;

    #pragma omp parallel for schedule(static) if(loopCount > 500)
    for (int i = 0; i < loopCount; ++i) {
        uint32_t ind = full ? i : iVerts[i];
        if (ind >= (uint32_t)totalNbVerts) continue;

        uint32_t start = vrfStartCount[ind * 2];
        uint32_t count = vrfStartCount[ind * 2 + 1];
        uint32_t end = start + count;

        float nx = 0.0f;
        float ny = 0.0f;
        float nz = 0.0f;
        uint32_t validCount = 0;

        for (uint32_t j = start; j < end; ++j) {
            uint32_t fIdx = vertRingFace[j];
            if (fIdx == UINT32_MAX || (totalNbFaces > 0 && fIdx >= (uint32_t)totalNbFaces)) continue;
            uint32_t id = fIdx * 3;
            nx += faceNormals[id];
            ny += faceNormals[id + 1];
            nz += faceNormals[id + 2];
            validCount++;
        }

        float len = (float)validCount;
        if (len != 0.0f) len = 1.0f / len;

        uint32_t ind3 = ind * 3;
        outNormals[ind3] = nx * len;
        outNormals[ind3 + 1] = ny * len;
        outNormals[ind3 + 2] = nz * len;
    }
}

void updateVertexNormals(
    const uint32_t* iVerts, int nbIVerts, int totalNbVerts,
    const std::vector<std::vector<uint32_t>>& dynVRF,
    const float* faceNormals,
    float* outNormals,
    int totalNbFaces
) {
    if (!faceNormals || !outNormals || totalNbVerts <= 0) return;

    bool full = (nbIVerts < 0 || (iVerts == nullptr && nbIVerts != 0));
    int loopCount = full ? totalNbVerts : nbIVerts;

    #pragma omp parallel for schedule(static) if(loopCount > 500)
    for (int i = 0; i < loopCount; ++i) {
        uint32_t ind = full ? i : iVerts[i];
        if (ind >= (uint32_t)totalNbVerts || ind >= (uint32_t)dynVRF.size()) continue;

        const auto& ring = dynVRF[ind];
        float nx = 0.0f;
        float ny = 0.0f;
        float nz = 0.0f;
        uint32_t validCount = 0;

        for (uint32_t fIdx : ring) {
            if (fIdx == UINT32_MAX || (totalNbFaces > 0 && fIdx >= (uint32_t)totalNbFaces)) continue;
            uint32_t id = fIdx * 3;
            nx += faceNormals[id];
            ny += faceNormals[id + 1];
            nz += faceNormals[id + 2];
            validCount++;
        }

        float len = (float)validCount;
        if (len != 0.0f) len = 1.0f / len;

        uint32_t ind3 = ind * 3;
        outNormals[ind3] = nx * len;
        outNormals[ind3 + 1] = ny * len;
        outNormals[ind3 + 2] = nz * len;
    }
}

