#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>
#include <chrono>
#include "editing/SelectionManager.h"
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

    void updateHover(Scene& scene, const Camera& camera, float mouseX, float mouseY);
    void finishBoxSelect(Scene& scene, const Camera& camera, bool ctrl, bool shift);
};
