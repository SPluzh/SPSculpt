#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>
#include "retopo/QuadDrawBVH.h"
#include "scene/Camera.h"
#include "mesh/Mesh.h"

namespace retopo {

enum class SnapMode {
    None = 0,
    RetopoVertex,  // Near existing retopo vertex (for weld / tweak)
    TargetSurface  // Smooth continuous position on target polygon surface
};

struct SnapResult {
    bool valid = false;
    SnapMode mode = SnapMode::None;
    glm::vec3 worldPos{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec3 screenPos{0.0f};
    int32_t retopoVertexIndex = -1;
};

struct EdgeHit {
    bool valid = false;
    int32_t v0 = -1;
    int32_t v1 = -1;
    glm::vec3 worldPos0{0.0f};
    glm::vec3 worldPos1{0.0f};
    float t = 0.5f;
    float dist = 1e30f;
    int32_t polyIndex = -1;
};

class SurfaceSnapper {
public:
    SurfaceSnapper();
    ~SurfaceSnapper();

    // Smooth continuous raycast directly to target mesh surface
    SnapResult RaycastSurface(const Camera& camera, const Mesh* targetMesh, float screenX, float screenY);

    // Project a 3D point onto target mesh surface strictly along the given normal direction
    SnapResult ProjectPointAlongNormal(const Mesh* targetMesh, const glm::vec3& worldPoint, const glm::vec3& worldNormal, float maxDist = 500.0f);

    // Find nearest retopo vertex in screen space (with depth sorting & target occlusion)
    int32_t FindNearestRetopoVertex(
        const Camera& camera, const Mesh* retopoMesh,
        float screenX, float screenY, float maxRadiusPixels = 12.0f,
        int32_t excludeIndex = -1, const Mesh* targetMesh = nullptr,
        const SnapResult* precomputedTargetSnap = nullptr
    );

    // Find nearest retopo edge in screen space within pixel distance
    EdgeHit FindNearestRetopoEdge(
        const Camera& camera, const Mesh* retopoMesh,
        float screenX, float screenY, float maxRadiusPixels = 10.0f,
        const Mesh* targetMesh = nullptr,
        const SnapResult* precomputedTargetSnap = nullptr
    );

    // Combined snap: checks retopo vertex first, then raycasts to target surface
    SnapResult Snap(
        const Camera& camera, const Mesh* targetMesh, const Mesh* retopoMesh,
        float screenX, float screenY, float snapRadiusPixels = 12.0f
    );

    void ClearCache();
    QuadDrawBVH& getBVH() { return m_bvh; }
    const QuadDrawBVH& getBVH() const { return m_bvh; }

private:
    QuadDrawBVH m_bvh;

    struct CachedRaycast {
        const Mesh* target = nullptr;
        float screenX = -1e30f;
        float screenY = -1e30f;
        uint64_t vertGen = 0;
        SnapResult result;
    };
    CachedRaycast m_cachedRaycast;
};

} // namespace retopo
