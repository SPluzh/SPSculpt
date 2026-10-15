#pragma once

#include "manifold.h"
#include <vector>
#include <glm/glm.hpp>

class Camera;

namespace sculpt {

class LassoCutter {
public:
    // Cleans screen 2D lasso points using Clipper2 and Chaikin smoothing
    static std::vector<std::vector<glm::vec2>> cleanLasso2D(
        const std::vector<glm::vec2>& inputPoints,
        float simplifyTolerance = 1.0f,
        int chaikinIterations = 2
    );

    // Resamples a 2D closed polygon along its perimeter to equidistant spacing
    static std::vector<glm::vec2> resampleEquidistant(
        const std::vector<glm::vec2>& poly,
        float targetSpacingPx
    );

    // Builds a 3D cutter Manifold from screen lasso points and camera
    // If invMeshMatrix is provided, cutter is transformed into mesh local space.
    // If localBoxMin/Max and targetEdgeLength are provided, subdivides lateral walls into regular square quads.
    static manifold::Manifold build3DCutterPrism(
        const std::vector<glm::vec2>& lassoPoints,
        const Camera& camera,
        const glm::mat4& invMeshMatrix = glm::mat4(1.0f),
        uint32_t capFaceID = 0,
        const glm::vec3& localBoxMin = glm::vec3(0.0f),
        const glm::vec3& localBoxMax = glm::vec3(0.0f),
        float targetEdgeLength = 0.0f
    );

    // Stress test: builds a figure-8 self-intersecting lasso and tests cutter generation
    static bool stressTestFigure8(const Camera& camera);
};

} // namespace sculpt
