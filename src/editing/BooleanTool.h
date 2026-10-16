#pragma once

#include <string>
#include <vector>
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

    // Performs exact polygonal CSG boolean between targetMesh and otherMeshes (2 or more meshes total).
    // Result replaces targetMesh, otherMeshes are removed from the Scene.
    static bool executeMultiple(
        Scene& scene,
        Mesh* targetMesh,
        const std::vector<Mesh*>& otherMeshes,
        BooleanOp op
    );
};

} // namespace sculpt
