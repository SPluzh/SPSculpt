#pragma once
#include <SDL2/SDL.h>
#include "scene/Camera.h"

class CameraController {
public:
    enum class DragMode { None, Orbit, Pan, Zoom };

    CameraController() = default;
    ~CameraController() = default;

    void handleEvent(const SDL_Event& e, Camera& camera);
    
    void startDrag(DragMode mode, int mouseX, int mouseY, Camera& camera);
    bool isDragging() const { return m_drag != DragMode::None; }
    void stopDrag() { m_drag = DragMode::None; m_snapTriggered = false; }
    DragMode getDragMode() const { return m_drag; }

private:
    DragMode m_drag = DragMode::None;
    int m_prevX = 0, m_prevY = 0;
    bool m_snapTriggered = false;
};
