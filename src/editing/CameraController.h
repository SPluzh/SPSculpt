#pragma once
#include <SDL2/SDL.h>
#include <vector>
#include "scene/Camera.h"

class Mesh;

class CameraController {
public:
    enum class DragMode { None, Orbit, Pan, Zoom, Roll, Pan2D, Zoom2D };

    CameraController() = default;
    ~CameraController() = default;

    void handleEvent(const SDL_Event& e, Camera& camera, const std::vector<Mesh*>& meshes);
    
    void startDrag(DragMode mode, int mouseX, int mouseY, Camera& camera, const std::vector<Mesh*>& meshes);
    bool isDragging() const { return m_drag != DragMode::None; }
    void stopDrag() { m_drag = DragMode::None; m_snapTriggered = false; }
    DragMode getDragMode() const { return m_drag; }

private:
    DragMode m_drag = DragMode::None;
    int m_prevX = 0, m_prevY = 0;
    bool m_snapTriggered = false;
};

