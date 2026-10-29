#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>
#include <chrono>
#include "editing/SelectionManager.h"
#include "editing/ComponentOps.h"
#include "scene/Scene.h"
#include "scene/Camera.h"

class SculptManager;

class ComponentSelectTool {
public:
    explicit ComponentSelectTool(SculptManager& sm);
    ~ComponentSelectTool() = default;

    void onActivate(Scene& scene);
    void onDeactivate();

    bool handleMouseDown(Scene& scene, const Camera& camera, float mouseX, float mouseY, bool ctrl, bool shift, int button);
    bool handleMouseMove(Scene& scene, const Camera& camera, float mouseX, float mouseY, bool ctrl, bool shift);
    bool handleMouseUp(Scene& scene, const Camera& camera, int button);

    void renderOverlay(Scene& scene, const Camera& camera, float vW, float vH);
    void renderGui(Scene& scene);

    SelectionManager& getSelectionManager();
    const SelectionManager& getSelectionManager() const;

    bool isBoxSelecting() const { return m_isBoxSelecting; }

    // Interactive Bevel
    bool isBevelActive() const { return m_isBevelActive; }
    bool isBevelDragging() const { return m_isBevelDragging; }
    float getBevelWidth() const { return m_bevelWidth; }
    float getBevelHeight() const { return m_bevelHeight; }
    int getBevelSegments() const { return m_bevelSegments; }
    BevelType getBevelType() const { return m_bevelType; }

    bool startInteractiveBevel(Scene& scene, float initWidth = 0.2f, int initSegments = 1, BevelType initType = BevelType::Chamfer, float initHeight = 0.0f);
    void setBevelParams(Scene& scene, float width, int segments, BevelType type, float height = 0.0f);
    void applyInteractiveBevel(Scene& scene);
    void cancelInteractiveBevel(Scene& scene);
    bool handleKeyDown(Scene& scene, int keycode);
    bool handleMouseWheel(Scene& scene, int wheelY);

private:
    SculptManager& m_sculpt;

    // Hover state
    uint32_t m_hoveredVert = 0xffffffff;
    uint32_t m_hoveredEdge = 0xffffffff;
    uint32_t m_hoveredFace = 0xffffffff;

    // Box select state
    bool m_isBoxSelecting = false;
    glm::vec2 m_boxStart{0.0f};
    glm::vec2 m_boxCurrent{0.0f};

    // Double click tracking
    uint32_t m_lastClickedEdge = 0xffffffff;
    std::chrono::steady_clock::time_point m_lastClickTime;

    // Interactive Bevel
    bool m_isBevelActive = false;
    bool m_isBevelDragging = false;
    float m_bevelWidth = 0.2f;
    float m_bevelHeight = 0.0f;
    int m_bevelSegments = 1;
    BevelType m_bevelType = BevelType::Chamfer;
    glm::vec2 m_bevelDragStartMouse{0.0f};
    float m_bevelDragStartWidth = 0.2f;
    float m_bevelDragStartHeight = 0.0f;
    float m_bevelMeshRadius = 1.0f;

    std::unique_ptr<Mesh> m_bevelBackupMesh;
    ComponentMode m_bevelSavedMode = ComponentMode::Edge;
    std::unordered_set<uint32_t> m_bevelSavedVerts;
    std::unordered_set<uint32_t> m_bevelSavedEdges;
    std::unordered_set<uint32_t> m_bevelSavedFaces;

    void updateBevelLive(Scene& scene, bool finalApply = false);

    void updateHover(Scene& scene, const Camera& camera, float mouseX, float mouseY);
    void finishBoxSelect(Scene& scene, const Camera& camera, bool ctrl, bool shift);
};
