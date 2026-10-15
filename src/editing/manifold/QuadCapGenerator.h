#pragma once

#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

class Mesh;

namespace sculpt {

struct QuadCapResult {
    int numQuads = 0;
    int numTris = 0;
    float quadRatio = 0.0f; // numQuads / totalFaces
    double executionTimeMs = 0.0;
    bool success = false;
};

class QuadCapGenerator {
public:
    // Generates a regular quad grid (with CDT boundary transition belt)
    // for all cap faces of the specified capGroupID on the mesh.
    static QuadCapResult generateQuadCap(
        Mesh& mesh,
        uint32_t capGroupID,
        float targetEdgeLen = 0.0f
    );
};

} // namespace sculpt
