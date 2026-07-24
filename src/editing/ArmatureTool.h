#pragma once
#include "sculpt/ArmatureGraph.h"
#include "scene/Camera.h"
#include "scene/Scene.h"
#include "editing/SculptManager.h"
#include <SDL2/SDL.h>
#include <unordered_map>

enum class ArmatureMode {
    DRAW,
    INSERT,
    MOVE,
    SCALE,
    ROTATE
};

struct ArmatureHitResult {
    enum Type { NONE, NODE, LINK } type = NONE;
    ArmatureNode* node = nullptr;
    ArmatureNode* linkParent = nullptr;
    ArmatureNode* linkChild = nullptr;
    float t = -1.0f;
    float u = 0.0f; // along link
    glm::vec3 position{0.0f};
    float radius = 0.0f;
};

class ArmatureTool {
public:
    ArmatureTool(SculptManager& sculptManager);
    ~ArmatureTool() = default;

    void onActivate();
    void onDeactivate();

    void preUpdate(const Camera& camera, float mouseX, float mouseY, bool isCtrl, bool isAlt);
    bool start(const Camera& camera, float mouseX, float mouseY, bool isCtrl, bool isAlt);
    void update(const Camera& camera, float mouseX, float mouseY);
    void end();

    void setMode(ArmatureMode mode) { m_mode = mode; }
    ArmatureMode getMode() const { return m_mode; }

    ArmatureGraph& getGraph() { return m_graph; }
    const ArmatureGraph& getGraph() const { return m_graph; }

    ArmatureNode* getSelectedNode() const { return m_selectedNode; }
    ArmatureNode* getHoveredLinkParent() const { return m_hoveredLinkParent; }
    ArmatureNode* getHoveredLinkChild() const { return m_hoveredLinkChild; }
    const glm::vec3& getPreviewPosition() const { return m_previewPosition; }
    float getPreviewRadius() const { return m_previewRadius; }

    void createMesh(Scene& scene);

private:
    SculptManager& m_sculptManager;
    ArmatureGraph m_graph;
    ArmatureMode m_mode = ArmatureMode::DRAW;

    ArmatureNode* m_activeNode = nullptr;
    bool m_isDragging = false;
    ArmatureMode m_dragMode = ArmatureMode::DRAW;

    float m_screenZ = 0.0f;
    float m_startRadius = 1.0f;
    float m_startMouseX = 0.0f;
    float m_startMouseY = 0.0f;

    ArmatureNode* m_selectedNode = nullptr;
    ArmatureNode* m_hoveredLinkParent = nullptr;
    ArmatureNode* m_hoveredLinkChild = nullptr;
    glm::vec3 m_previewPosition{0.0f};
    float m_previewRadius = 0.0f;

    std::string m_historyState;

    std::unordered_map<uint32_t, glm::vec3> m_initialPositions;

    void clearHoverAndSelection();
    void pushHistoryState();

    glm::vec3 getSymmetricPosition(const glm::vec3& pos) const;
    float getDistanceToSymmetryPlane(const glm::vec3& pos) const;
    glm::vec3 snapToSymmetryPlane(const glm::vec3& pos) const;
    float getSymmetrySnapThreshold() const;

    float intersectRaySphere(const glm::vec3& rayOrigin, const glm::vec3& rayDir, const glm::vec3& center, float radius) const;
    bool intersectLink(const glm::vec3& rayOrigin, const glm::vec3& rayDir, ArmatureNode* parent, ArmatureNode* child, ArmatureHitResult& outRes) const;
    ArmatureHitResult hitTest(const glm::vec3& rayOrigin, const glm::vec3& rayDir) const;

    Mesh* getActiveMesh() const;
};
