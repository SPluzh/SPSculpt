#pragma once

#include <vector>
#include <glm/glm.hpp>
#include "common/Enums.h"
#include "common/Symmetry.h"

#include "editing/manifold/QuadCapGenerator.h"

class Mesh;
class Camera;

struct TrimConfig {
    bool fillHole = true;
    bool remeshFilled = true;
    float joinAngleDegree = 45.0f;
    int maxSubdivisions = 3;
    sculpt::QuadCapResult* outCapResult = nullptr;
    uint32_t* outCapGid = nullptr;
};

class TrimTool {
public:
    static bool execute(
        Mesh* mesh,
        const Camera& camera,
        const std::vector<glm::vec2>& lassoPoints,
        bool isAlt,
        const std::vector<SymCopy>& symCopies = {},
        const TrimConfig& config = TrimConfig()
    );
};
