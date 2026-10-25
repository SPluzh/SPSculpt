#pragma once
#include <vector>
#include <cstdint>
#include "mesh/Mesh.h"

namespace Subdivision {
    extern bool LINEAR;

    // Full subdivision with topology re-construction
    void fullSubdivision(Mesh& baseMesh, Mesh& newMesh);

    // Full subdivision with crease & CreaseLvl support
    void fullSubdivisionWithCreases(
        Mesh& baseMesh,
        Mesh& newMesh,
        const std::vector<uint8_t>* inCreases = nullptr,
        std::vector<uint8_t>* outCreases = nullptr,
        bool isSharpStep = false,
        bool isLinear = false,
        SubdivLevelCache* cache = nullptr,
        const std::vector<uint8_t>* inCageEdges = nullptr,
        std::vector<uint8_t>* outCageEdges = nullptr
    );

    // Multi-level dynamic subdivision (ZBrush style)
    void computeDynamicSubdivision(
        Mesh& baseMesh,
        Mesh& outMesh,
        int smoothLevel,
        int flatLevel,
        int creaseLevel
    );

    // Fast parallel cached subdivision geometry update (zero topology overhead)
    void evaluateCachedSubdivision(
        const Mesh& baseMesh,
        Mesh& outMesh,
        const SubdivLevelCache& cache,
        const std::vector<uint8_t>* inCreases = nullptr,
        bool isSharp = false,
        bool isLinear = false
    );

    // Partial subdivision (geometry smoothing only, keeping target topology buffers)
    void partialSubdivision(Mesh& baseMesh,
                            std::vector<float>& vertOut,
                            std::vector<float>& colorOut,
                            std::vector<float>& materialOut);

    // Helper functions
    void applyEvenSmooth(Mesh& baseMesh,
                         std::vector<float>& even,
                         std::vector<float>& colorOut,
                         std::vector<float>& materialOut,
                         const std::vector<uint8_t>* inCreases = nullptr,
                         bool isSharp = false,
                         bool isLinear = false);

    std::vector<int32_t> applyOddSmooth(Mesh& baseMesh,
                                         std::vector<float>& odds,
                                         std::vector<float>& colorOut,
                                         std::vector<float>& materialOut,
                                         std::vector<uint32_t>* fArOut = nullptr,
                                         const std::vector<uint8_t>* inCreases = nullptr,
                                         bool isSharp = false,
                                         bool isLinear = false,
                                         SubdivLevelCache* cache = nullptr);
}
