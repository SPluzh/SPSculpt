#pragma once
#include <vector>
#include <cstdint>
#include "mesh/Mesh.h"

namespace Subdivision {
    extern bool LINEAR;

    // Full subdivision with topology re-construction
    void fullSubdivision(Mesh& baseMesh, Mesh& newMesh);

    // Partial subdivision (geometry smoothing only, keeping target topology buffers)
    void partialSubdivision(Mesh& baseMesh,
                            std::vector<float>& vertOut,
                            std::vector<float>& colorOut,
                            std::vector<float>& materialOut);

    // Helper functions
    void applyEvenSmooth(Mesh& baseMesh,
                         std::vector<float>& even,
                         std::vector<float>& colorOut,
                         std::vector<float>& materialOut);

    std::vector<int32_t> applyOddSmooth(Mesh& baseMesh,
                                         std::vector<float>& odds,
                                         std::vector<float>& colorOut,
                                         std::vector<float>& materialOut,
                                         std::vector<uint32_t>* fArOut = nullptr);
}
