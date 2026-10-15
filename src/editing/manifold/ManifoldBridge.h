#pragma once

#include "manifold.h"
#include "mesh/Mesh.h"

namespace sculpt {

class ManifoldBridge {
public:
    // Converts an SPSculpt Mesh into a manifold::Manifold
    static manifold::Manifold fromMesh(const Mesh& mesh);

    // Converts a manifold::Manifold back into an SPSculpt Mesh
    static void toMesh(
        const manifold::Manifold& manifold,
        Mesh& outMesh,
        bool runPostInit = true,
        int cutterOriginalID = -1,
        uint32_t capGroupID = 0
    );

    // Round-trip test helper: converts mesh to manifold and back, verifying integrity
    static bool verifyRoundTrip(const Mesh& originalMesh);
};

} // namespace sculpt
