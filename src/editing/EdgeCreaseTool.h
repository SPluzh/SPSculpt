#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>
#include "editing/SelectionManager.h"
#include "scene/Scene.h"
#include "scene/Camera.h"

class SculptManager;

class EdgeCreaseTool {
public:
    explicit EdgeCreaseTool(SculptManager& sm);
    ~EdgeCreaseTool() = default;

    void onActivate(Scene& scene);
    void onDeactivate();

    bool handleMouseDown(Scene& scene, const Camera& camera, float mouseX, float mouseY, bool ctrl, bool shift, int button);
    bool handleMouseMove(Scene& scene, const Camera& camera, float mouseX, float mouseY, bool ctrl, bool shift);
    bool handleMouseUp(Scene& scene, const Camera& camera, int button);

    void renderOverlay(Scene& scene, const Camera& camera, float vW, float vH);
    void renderGui(Scene& scene);

    // Core actions on already selected edges
    void applyCreaseToSelected(Mesh* mesh, bool enable);
    void toggleCreaseOnSelected(Mesh* mesh);

    bool isDraggingLevel() const { return m_isDraggingLevel; }

private:
    SculptManager& m_sculpt;

    bool m_isDraggingLevel = false;
    float m_dragStartX = 0.0f;
    float m_dragStartLevel = 0.0f;
    glm::vec2 m_currentMousePos{0.0f};

    SelectionManager& getSelectionManager();
    const SelectionManager& getSelectionManager() const;
};
