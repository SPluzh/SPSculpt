#pragma once
#include <vector>
#include <glm/glm.hpp>
#include "common/Enums.h"

class Mesh;
class Camera;

class ClipCurveTool {
public:
    static bool execute(
        Mesh* mesh,
        const Camera& camera,
        const std::vector<glm::vec2>& curvePoints,
        bool altMode,
        bool useSym = false,
        SymmetryMode symMode = SymmetryMode::Local,
        const std::vector<glm::vec3>& symScales = {}
    );
};
