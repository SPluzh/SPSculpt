#pragma once
#include <SDL2/SDL.h>
#include <vector>
#include "scene/Camera.h"

#include <glm/glm.hpp>
#include <algorithm>

class Mesh;

class CameraController {
public:
    enum class DragMode { None, Orbit, Pan, Zoom, Roll, Pan2D, Zoom2D };

    CameraController() = default;
    ~CameraController() = default;

    void handleEvent(const SDL_Event& e, Camera& camera, const std::vector<Mesh*>& meshes, bool animate = true);
    
    void startDrag(DragMode mode, int mouseX, int mouseY, Camera& camera, const std::vector<Mesh*>& meshes);
    bool isDragging() const { return m_drag != DragMode::None; }
    void stopDrag(Camera* camera = nullptr);
    DragMode getDragMode() const { return m_drag; }

    int getStartX() const { return m_startX; }
    int getStartY() const { return m_startY; }

    void update(float deltaTime) {}

private:
    DragMode m_drag = DragMode::None;
    int m_prevX = 0, m_prevY = 0;
    int m_startX = 0, m_startY = 0;
    bool m_snapTriggered = false;
};

