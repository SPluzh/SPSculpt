#pragma once
#include "sculpt/PrimitiveData.h"
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <memory>

class Scene;
class Mesh;
class Camera;
class SculptManager;

enum class PrimitiveHandleType {
    None = 0,
    SphereRadius,
    GeosphereRadius,
    CubeSizeX,
    CubeSizeY,
    CubeSizeZ,
    CubeCornerRadius,
    CylinderHeight,
    CylinderRadiusTop,
    CylinderRadiusBottom,
    CylinderInnerRadius,
    ConeHeight,
    ConeRadiusBottom,
    ConeRadiusTop,
    TorusMajorRadius,
    TorusTubeRadius,
    PlaneSizeX,
    PlaneSizeZ,
    CapsuleRadius,
    CapsuleHeight
};

struct PrimitiveHandle {
    PrimitiveHandleType type = PrimitiveHandleType::None;
    glm::vec3 localPos{0.0f};
    glm::vec3 localAxis{0.0f};
    glm::vec3 worldPos{0.0f};
    glm::vec2 screenPos{0.0f};
    std::string label;
    float startVal = 0.0f;
    float minVal = 0.1f;
    float maxVal = 1000.0f;
};

class PrimitiveTool {
public:
    PrimitiveTool(SculptManager& sm);
    ~PrimitiveTool();

    void onActivate(Scene& scene);
    void onDeactivate();

    bool handleMouseDown(Scene& scene, const Camera& camera, float mouseX, float mouseY, bool isCtrl, bool isAlt, bool isShift);
    bool handleMouseMove(Scene& scene, const Camera& camera, float mouseX, float mouseY);
    bool handleMouseUp(Scene& scene);

    void renderOverlay(Scene& scene, const Camera& camera, float viewportW, float viewportH);

    Mesh* spawnPrimitive(Scene& scene, PrimitiveType type, const glm::vec3& position = glm::vec3(0.0f), float scale = 1.0f);
    void updateMeshGeometry(Mesh* mesh);
    Mesh* bakeToMesh(Scene& scene, Mesh* mesh);
    void bakeAllPrimitives(Scene& scene);

    bool isInteracting() const { return m_isDraggingHandle; }
    bool isHoveringHandle() const { return m_hoveredHandleIdx != -1; }
    Mesh* getActivePrimitiveMesh() const { return m_activeMesh; }
    void setActivePrimitiveMesh(Mesh* m) { m_activeMesh = m; }

    glm::mat4 getPrimitiveLocalTransform(const Mesh* mesh) const;

    void markGeometryDirty() { m_geometryDirty = true; }
    void updateGeometryIfNeeded(Scene& scene);

private:
    SculptManager& m_sculpt;
    Mesh* m_activeMesh = nullptr;

    bool m_isDraggingHandle = false;
    int m_hoveredHandleIdx = -1;
    int m_activeHandleIdx = -1;
    glm::vec2 m_dragStartMouse{0.0f};
    PrimitiveData m_startPrimitiveData;

    std::vector<PrimitiveHandle> m_currentHandles;
    bool m_geometryDirty = false;

    void buildHandlesForMesh(Mesh* mesh, const Camera& camera);
    void applyHandleDelta(PrimitiveHandleType type, float delta);
};
