#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <memory>
#include <string>
#include "common/Enums.h"
#include "editing/MeasurementTypes.h"

class Scene;
class Mesh;
class Camera;
class SculptManager;

enum class PoseHandle {
    None = -1,
    Start = 0,   // Pivot / Joint Base
    Center = 1,  // Midpoint
    End = 2      // Tip / Bone Direction
};

struct PoseActionLine {
    glm::vec3 startWorld{0.0f};  // Pivot
    glm::vec3 endWorld{0.0f};    // Tip
    glm::vec3 centerWorld{0.0f}; // Center

    bool isValid = false;
    bool isDrawing = false;
    PoseHandle activeHandle = PoseHandle::None;
    bool isAltRepositioning = false;

    glm::vec2 dragStartMouse{0.0f};
    glm::vec3 initialStartPos{0.0f};
    glm::vec3 initialEndPos{0.0f};
    glm::vec3 dragPlaneNormal{0.0f};
    glm::vec3 dragPlanePoint{0.0f};
};

class PoseTool {
public:
    PoseTool(SculptManager& sm);
    ~PoseTool() = default;

    void onActivate(Scene& scene);
    void onDeactivate();

    bool handleMouseDown(Scene& scene, const Camera& camera, float mouseX, float mouseY, bool isCtrl, bool isAlt, bool isShift);
    bool handleMouseMove(Scene& scene, const Camera& camera, float mouseX, float mouseY);
    bool handleMouseUp(Scene& scene);

    void renderOverlay(Scene& scene, const Camera& camera, float viewportW, float viewportH);
    void renderGizmoAndDeform(Scene& scene, const Camera& camera, float viewportW, float viewportH);

    void computeWeights(Mesh* mesh);
    void clearLine();

    bool hasValidLine() const { return m_line.isValid; }
    const PoseActionLine& getLine() const { return m_line; }
    PoseActionLine& getLine() { return m_line; }

    void updateGizmoMatrixFromLine();
    const glm::mat4& getGizmoMatrix() const { return m_gizmoMatrix; }

    bool isUsingGizmo() const { return m_isUsingGizmo; }

private:
    SculptManager& m_sculpt;
    PoseActionLine m_line;

    glm::mat4 m_gizmoMatrix{1.0f};
    glm::mat4 m_gizmoStartMatrix{1.0f};
    glm::vec3 m_gizmoStartEndWorld{0.0f};
    bool m_wasUsingGizmo = false;
    bool m_isUsingGizmo = false;
    bool m_gizmoDeformedMesh = false;

    // Cached geometry state during active manipulation
    std::vector<float> m_startVerts;
    std::vector<float> m_startNormals;
    std::vector<float> m_weights;
    std::vector<float> m_weightsSym;

    void prepareDeformation(Mesh* mesh);
    void applyDeformation(Mesh* mesh, const glm::mat4& deltaMatrixWorld, bool useSym);
    void finalizeDeformation(Scene& scene, Mesh* mesh);

    float evaluateBoneWeight(
        const glm::vec3& v,
        const glm::vec3& start,
        const glm::vec3& dir,
        float invLenSq,
        float falloffWidth,
        float capsuleRadius,
        float maskFactor
    ) const;

    glm::vec3 intersectRayViewPlane(const Camera& camera, float mouseX, float mouseY, const glm::vec3& planePoint) const;
    int hitTestHandles(const Camera& camera, float mouseX, float mouseY, float threshold = 20.0f) const;
    int hitTestSymHandles(const Camera& camera, const Mesh* mesh, float mouseX, float mouseY, float threshold = 20.0f) const;
    void mirrorLineAcrossSymmetry(Mesh* mesh);
};
