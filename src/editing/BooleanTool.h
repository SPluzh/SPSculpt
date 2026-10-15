#pragma once

#include <string>
#include <glm/glm.hpp>

class Scene;
class Mesh;

namespace sculpt {

enum class BooleanOp {
    Union,
    Difference,
    Intersection
};

class BooleanTool {
public:
    // Performs exact polygonal CSG boolean between meshA and meshB.
    // Result replaces meshA, meshB is removed from the Scene.
    static bool execute(
        Scene& scene,
        Mesh* meshA,
        Mesh* meshB,
        BooleanOp op
    );
};

} // namespace sculpt
