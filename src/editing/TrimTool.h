#pragma once

#include <vector>
#include <glm/glm.hpp>
#include "common/Enums.h"

class Mesh;
class Camera;

struct TrimConfig {
    bool fillHole = true;
    bool remeshFilled = true;
    float joinAngleDegree = 45.0f;
    int maxSubdivisions = 3;
};

class TrimTool {
public:
    static bool execute(
        Mesh* mesh,
        const Camera& camera,
        const std::vector<glm::vec2>& lassoPoints,
        bool isAlt,
        bool useSym,
        SymmetryMode symMode,
        const std::vector<glm::vec3>& symScales,
        const TrimConfig& config = TrimConfig()
    );
};
