#pragma once
#include <glm/glm.hpp>
#include <vector>
#include "common/Enums.h"
#include "common/Symmetry.h"

class AngleRenderer;
class Scene;
class Camera;

struct BrushCursorState {
    bool      visible        = false;
    bool      showCircle     = true;
    bool      isScreenspace  = false;
    bool      hitMesh        = false;
    glm::vec3 hitPoint       {0.0f};
    glm::vec3 hitNormal      {0.0f, 1.0f, 0.0f};
    float     radius         = 8.0f;   // in world units
    glm::vec3 color          {1.0f, 0.3f, 0.1f};

    // Computed MVPs for AngleRenderer
    glm::mat4 circleMVP      {1.0f};
    glm::mat4 innerCircleMVP {1.0f};
    glm::mat4 dotMVP         {1.0f};
    std::vector<glm::mat4> symMVPs;
    std::vector<char> symOccluded;

    // Right camera MVPs for split viewport support
    glm::mat4 circleMVPRight      {1.0f};
    glm::mat4 innerCircleMVPRight {1.0f};
    glm::mat4 dotMVPRight         {1.0f};
    std::vector<glm::mat4> symMVPsRight;
    std::vector<char> symOccludedRight;
};

class BrushCursor {
public:
    BrushCursor();
    ~BrushCursor() = default;

    // Called on mouse move over viewport
    void update(int mouseX, int mouseY,
                const Scene& scene,
                float brushRadius,
                const std::vector<SymCopy>& symCopies = {},
                bool isSculpting = false,
                BrushType brushType = BRUSH_FLATTEN,
                bool hasActiveStrokeHit = false,
                const glm::vec3& activeStrokeHitPt = glm::vec3(0.0f),
                const glm::vec3& activeStrokeHitNormal = glm::vec3(0.0f, 1.0f, 0.0f),
                float focalShift = 0.0f,
                float hardness = 0.5f,
                const glm::vec3& paintColor = glm::vec3(0.8f, 0.0f, 0.0f),
                ModalMode modalMode = ModalMode::NONE);

    void applyToRenderer(AngleRenderer& renderer) const;

    void hide() { m_state.visible = false; m_state.hitMesh = false; }

    const BrushCursorState& getState() const { return m_state; }

    size_t getOcclusionQueryCount() const { return m_occlusionQueryCount; }
    void resetOcclusionQueryCount() { m_occlusionQueryCount = 0; }

private:
    BrushCursorState m_state;

    int m_lastOccMouseX = -99999;
    int m_lastOccMouseY = -99999;
    glm::mat4 m_lastOccViewLeft{0.0f};
    glm::mat4 m_lastOccViewRight{0.0f};
    uint64_t m_lastOccTicks = 0;
    std::vector<char> m_cachedSymOccluded;
    std::vector<char> m_cachedSymOccludedRight;
    size_t m_occlusionQueryCount = 0;

    glm::mat4 buildCircleMVP(const glm::vec3& center,
                             const glm::vec3& normal,
                             float radius,
                             const Camera& cam,
                             float tiltX = 0.0f,
                             float tiltY = 0.0f) const;
};
