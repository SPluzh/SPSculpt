#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "sculpt/Remesh.h"

namespace sculpt {

// A vertex counts as masked for Keep masked when materials[v * 3 + 2] < kKeepMaskedThreshold
// In SPSculpt, materials channel 2 is mask: 0.0 = masked (frozen/protected), 1.0 = unmasked (free)
inline constexpr float kKeepMaskedThreshold = 0.5f;

// Per triangle: 1 when all three corners are masked. Empty if no mask or nothing masked.
std::vector<uint8_t> getKeepMaskedTriangles(
    const float* materials,
    int nbVerts,
    const uint32_t* tris,
    int nbTris
);

// Puts the kept triangles of the original mesh (positions, colors, materials, polygroups unchanged)
// into the remeshed mesh: the remeshed surface over and next to them is cut away and each edge loop
// of the kept area is bridged to the cut's matching edge loop with a zipper strip of quads/triangles.
// Resulting mesh is guaranteed to be watertight and 2-manifold with zero boundary/non-manifold edges.
bool stitchKeptTriangles(
    const float* srcVerts, int srcNbVerts,
    const uint32_t* srcTris, int srcNbTris,
    const float* srcColors,
    const float* srcMaterials,
    const uint32_t* srcFaceGroups,
    const std::vector<uint8_t>& keep,
    const RemeshResult& remeshed,
    RemeshResult& out,
    std::string& error,
    std::string* note = nullptr
);

} // namespace sculpt
