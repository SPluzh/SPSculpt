#pragma once
#include <vector>
#include <cstdint>

namespace Subdivision {

struct SubdivEdgeData {
    uint32_t midVert = 0;
    uint32_t v1 = 0;
    uint32_t v2 = 0;
    uint32_t opp[4] = {0, 0, 0, 0};
    float wOpp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float wEnd = 0.5f;
    uint8_t numOpp = 0;
};

struct SubdivQuadData {
    uint32_t cenVert = 0;
    uint32_t v1 = 0;
    uint32_t v2 = 0;
    uint32_t v3 = 0;
    uint32_t v4 = 0;
};

struct SubdivLevelCache {
    std::vector<SubdivEdgeData> edges;
    std::vector<SubdivQuadData> quads;

    void clear() {
        edges.clear();
        quads.clear();
    }
};

} // namespace Subdivision
