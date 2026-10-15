#pragma once

#include <vector>
#include <string>
#include <glm/glm.hpp>
#include "common/Enums.h"

class Scene;
class Mesh;
class Camera;

namespace sculpt {

class SplitTool {
public:
    // Slices mesh along a screen-space line using a cutting plane.
    // Replaces target mesh with Part 1 and adds Part 2 to the Scene as "OriginalName_Cut".
    // Automatically caps both parts with QuadCapGenerator and assigns distinct polygroups.
    static bool sliceByLine(
        Scene& scene,
        Mesh* mesh,
        const Camera& camera,
        const glm::vec2& lineStart,
        const glm::vec2& lineEnd
    );

    // Splits mesh by a 3D lasso cutter prism.
    // Replaces target mesh with outside part and adds inside part to the Scene as "OriginalName_Cut".
    static bool splitByLasso(
        Scene& scene,
        Mesh* mesh,
        const Camera& camera,
        const std::vector<glm::vec2>& lassoPoints
    );
};

} // namespace sculpt
