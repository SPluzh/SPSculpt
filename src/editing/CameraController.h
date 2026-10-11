#pragma once
#include <SDL2/SDL.h>
#include <vector>
#include "scene/Camera.h"

#include <glm/glm.hpp>
#include <algorithm>

class Mesh;
class Scene;
class AngleRenderer;

class CameraController {
public:
    enum class DragMode { None, Orbit, Pan, Zoom, Roll, Pan2D, Zoom2D, RotateLight };

    CameraController() = default;
    ~CameraController() = default;

    void handleEvent(const SDL_Event& e, Camera& camera, const std::vector<Mesh*>& meshes, bool animate = true, Scene* scene = nullptr, AngleRenderer* renderer = nullptr);
    
    bool isNavigationEvent(const SDL_Event& e, CameraEnums::NavigationStyle style) const;

    void startDrag(DragMode mode, int mouseX, int mouseY, Camera& camera, const std::vector<Mesh*>& meshes, uint8_t button = 0);
    bool isDragging() const { return m_drag != DragMode::None; }
    void stopDrag(Camera* camera = nullptr);
    DragMode getDragMode() const { return m_drag; }
    uint8_t getDragButton() const { return m_dragButton; }

    int getStartX() const { return m_startX; }
    int getStartY() const { return m_startY; }

    void update(float deltaTime) {}

private:
    void handleEventZBrush(const SDL_Event& e, Camera& camera, const std::vector<Mesh*>& meshes, bool animate, Scene* scene, AngleRenderer* renderer);
    void handleEventMaya(const SDL_Event& e, Camera& camera, const std::vector<Mesh*>& meshes, bool animate, Scene* scene, AngleRenderer* renderer);
    void handleEventBlender(const SDL_Event& e, Camera& camera, const std::vector<Mesh*>& meshes, bool animate, Scene* scene, AngleRenderer* renderer);

    DragMode m_drag = DragMode::None;
    uint8_t m_dragButton = 0;
    int m_prevX = 0, m_prevY = 0;
    int m_startX = 0, m_startY = 0;
    bool m_snapTriggered = false;
};

