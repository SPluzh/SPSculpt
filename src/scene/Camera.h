#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vector>

namespace CameraEnums {
    enum class Projection {
        PERSPECTIVE = 0,
        ORTHOGRAPHIC = 1
    };
    enum class CameraMode {
        ORBIT = 0,
        PLANE = 1,
        SPHERICAL = 2
    };
}

struct Ray {
    glm::vec3 origin;
    glm::vec3 dir;
};

class Camera {
public:
    Camera();

    void setProjectionType(CameraEnums::Projection projType);
    CameraEnums::Projection getProjectionType() const { return m_projectionType; }
    bool isOrthographic() const { return m_projectionType == CameraEnums::Projection::ORTHOGRAPHIC; }
    
    void setMode(CameraEnums::CameraMode mode);
    CameraEnums::CameraMode getMode() const { return m_mode; }

    void setFov(float fov);
    float getFov() const { return m_fov; }
    float getFovDegrees() const;

    const glm::mat4& getViewMatrix() const { return m_viewMatrix; }
    const glm::mat4& getProjMatrix() const { return m_projMatrix; }
    const glm::mat4& getViewportMatrix() const { return m_viewportMatrix; }

    void start(float mouseX, float mouseY);
    void rotate(float mouseX, float mouseY, float speedRotate = 0.25f);
    void translate(float dx, float dy);
    void zoom(float df);

    void setPivot(const glm::vec3& pivot);
    void setPivot(float x, float y, float z) { setPivot(glm::vec3(x, y, z)); }
    const glm::vec3& getPivot() const { return m_center; }
    float getPivotX() const { return m_center.x; }
    float getPivotY() const { return m_center.y; }
    float getPivotZ() const { return m_center.z; }

    uintptr_t getViewMatrixPtr() const { return (uintptr_t)glm::value_ptr(m_viewMatrix); }
    uintptr_t getProjMatrixPtr() const { return (uintptr_t)glm::value_ptr(m_projMatrix); }
    uintptr_t getViewportMatrixPtr() const { return (uintptr_t)glm::value_ptr(m_viewportMatrix); }

    void onResize(int width, int height);
    void updateView();
    void updateProjection();
    void optimizeNearFar(const float* bbox);

    Ray getRay(float mouseX, float mouseY) const;
    glm::vec3 unproject(float mouseX, float mouseY, float z) const;
    glm::vec3 project(const glm::vec3& worldPos) const;
    glm::mat4 computeWorldToScreenMatrix() const;

    glm::vec3 computePosition() const;
    void resetView();
    void resetViewToMesh(const float* bbox);
    void snapClosestRotation();
    void toggleViewFront();
    void toggleViewTop();
    void toggleViewLeft();
    void toggleViewRight();

    float getNear() const { return m_near; }
    float getFar() const { return m_far; }

    void setUsePivot(bool use) { m_usePivot = use; }
    bool getUsePivot() const { return m_usePivot; }

    void setSpeedRotate(float val) { m_speedRotate = val; }
    float getSpeedRotate() const { return m_speedRotate; }
    void setSpeedTranslate(float val) { m_speedTranslate = val; }
    float getSpeedTranslate() const { return m_speedTranslate; }
    void setSpeedZoom(float val) { m_speedZoom = val; }
    float getSpeedZoom() const { return m_speedZoom; }
    void setSpeedRoll(float val) { m_speedRoll = val; }
    float getSpeedRoll() const { return m_speedRoll; }

    void setOrbitAngles(float rx, float ry);

    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }
    float getOrthoZoom() const;

    float getView2DOffsetX() const { return m_view2DOffsetX; }
    float getView2DOffsetY() const { return m_view2DOffsetY; }
    void setView2DOffset(float x, float y) { m_view2DOffsetX = x; m_view2DOffsetY = y; }
    float getView2DZoom() const { return m_view2DZoom; }
    void setView2DZoom(float z) { m_view2DZoom = z > 0.01f ? z : 0.01f; }
    void resetView2D() { m_view2DOffsetX = 0.0f; m_view2DOffsetY = 0.0f; m_view2DZoom = 1.0f; }

    struct CameraState {
        glm::quat quatRot;
        glm::vec3 trans;
        glm::vec3 center;
        glm::vec3 offset;
        float rotX;
        float rotY;
        float fov;
        CameraEnums::Projection projectionType;
        CameraEnums::CameraMode mode;
        bool usePivot;
        float view2DOffsetX;
        float view2DOffsetY;
        float view2DZoom;
    };

    void pushState();
    void undo();
    void redo();
    void applyState(const CameraState& state);
    void roll(float angle);

private:
    float getTransZ() const;
    void updateOrtho();
    void setOrbit(float rx, float ry);

    std::vector<CameraState> m_history;
    int m_historyIndex = -1;

    bool m_usePivot = true;

    CameraEnums::CameraMode m_mode = CameraEnums::CameraMode::ORBIT;
    CameraEnums::Projection m_projectionType = CameraEnums::Projection::PERSPECTIVE;

    glm::quat m_quatRot{1.0f, 0.0f, 0.0f, 0.0f};
    glm::mat4 m_viewMatrix{1.0f};
    glm::mat4 m_projMatrix{1.0f};
    glm::mat4 m_viewportMatrix{1.0f};

    glm::vec2 m_lastNormalizedMouseXY{0.0f};
    glm::vec2 m_virtualNormalizedMouseXY{0.0f};
    int m_width = 0;
    int m_height = 0;

    float m_speed = 1.0f;
    float m_speedRotate = 1.0f;
    float m_speedTranslate = 1.0f;
    float m_speedZoom = 1.0f;
    float m_speedRoll = 1.0f;
    float m_fov = 45.0f; // Focal length in mm

    glm::vec3 m_trans{0.0f, 0.0f, 30.0f};
    glm::vec3 m_center{0.0f}; // Pivot of rotation
    glm::vec3 m_offset{0.0f};

    float m_rotX = 0.0f;
    float m_rotY = 0.0f;

    float m_near = 0.05f;
    float m_far = 5000.0f;

    float m_view2DOffsetX = 0.0f;
    float m_view2DOffsetY = 0.0f;
    float m_view2DZoom = 1.0f;
};
