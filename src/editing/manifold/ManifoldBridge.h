#pragma once

#include <vector>
#include <cstdint>
#include "manifold.h"
#include "mesh/Mesh.h"

namespace sculpt {

class ManifoldBridge {
public:
    // Converts an SPSculpt Mesh into a manifold::Manifold
    static manifold::Manifold fromMesh(const Mesh& mesh, uint32_t originalID = 0);

    // Converts a manifold::Manifold back into an SPSculpt Mesh
    static void toMesh(
        const manifold::Manifold& manifold,
        Mesh& outMesh,
        bool runPostInit = true,
        int cutterOriginalID = -1,
        uint32_t capGroupID = 0
    );

    // Converts a manifold::Manifold back into an SPSculpt Mesh after a Boolean operation,
    // preserving 100% of the original quad topology on uncut regions of meshA and meshB.
    static void toMeshBoolean(
        const manifold::Manifold& m,
        Mesh& outMesh,
        const Mesh& meshA,
        uint32_t origIdA,
        const Mesh& meshB,
        uint32_t origIdB,
        bool runPostInit = true
    );

    // Converts a manifold::Manifold back into an SPSculpt Mesh after a Boolean operation with multiple source meshes,
    // preserving 100% of the original quad topology on uncut regions of all source meshes.
    static void toMeshBooleanMultiple(
        const manifold::Manifold& m,
        Mesh& outMesh,
        const std::vector<const Mesh*>& sourceMeshes,
        const std::vector<uint32_t>& origIds,
        bool runPostInit = true
    );

    // Round-trip test helper: converts mesh to manifold and back, verifying integrity
    static bool verifyRoundTrip(const Mesh& originalMesh);
};

} // namespace sculpt
