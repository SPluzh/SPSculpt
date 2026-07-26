#pragma once
#include <vector>
#include <cstdint>

class MeshResolution;

namespace Reversion {
    bool computeReverse(MeshResolution& baseMesh, MeshResolution& newMesh);
}
