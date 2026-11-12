#pragma once
#include <vector>
#include <glm/glm.hpp>
#include "common/Enums.h"
#include "common/Symmetry.h"

class Mesh;
class Camera;

class ClipCurveTool {
public:
    static bool execute(
        Mesh* mesh,
        const Camera& camera,
        const std::vector<glm::vec2>& curvePoints,
        bool altMode,
        const std::vector<SymCopy>& symCopies = {}
    );
};
