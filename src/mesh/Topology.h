#pragma once
#include <vector>
#include <cstdint>

void computeTopology(
    int nbVerts, const uint32_t* faces, int nbFaces,
    std::vector<uint32_t>& vrfStartCount, std::vector<uint32_t>& vertRingFace,
    std::vector<uint32_t>& vrvStartCount, std::vector<uint32_t>& vertRingVert,
    std::vector<uint8_t>&  vertOnEdge
);
