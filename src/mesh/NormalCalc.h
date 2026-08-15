#pragma once
#include <cstdint>

void updateFaceNormalsAndBoxes(
    const float* verts, int nbVerts,
    const uint32_t* faces, int nbFaces,
    const uint32_t* iFaces, int nbIFaces,
    float* faceNormals,
    float* faceBoxes,
    float* faceCenters
);

void updateVertexNormals(
    const uint32_t* iVerts, int nbIVerts, int totalNbVerts,
    const uint32_t* vrfStartCount,
    const uint32_t* vertRingFace,
    const float* faceNormals,
    float* outNormals,
    int totalNbFaces = -1
);
