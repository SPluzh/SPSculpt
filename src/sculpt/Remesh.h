#pragma once
#include <vector>
#include <cstdint>

struct RemeshResult {
    std::vector<float> vertices;
    std::vector<uint32_t> faces;
    std::vector<float> colors;
    std::vector<float> materials;
};

RemeshResult doRemesh(
    const float* verts, int nbVerts,
    const uint32_t* tris, int nbTris,
    const float* colors,
    const float* materials,
    const float* box,              // float[6]
    float resolution,
    bool block,
    bool smooth,
    bool manifold,
    const float* uniformColor,     // float[3]
    const float* uniformMaterial,  // float[3]
    bool hasColors,
    bool hasMaterials
);
