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

    void handleEvent(const SDL_Event& e, Camera& camera, const std::vector<Mesh*>& meshes);
    
    void startDrag(DragMode mode, int mouseX, int mouseY, Camera& camera, const std::vector<Mesh*>& meshes);
    bool isDragging() const { return m_drag != DragMode::None; }
    void stopDrag(Camera* camera = nullptr);
    DragMode getDragMode() const { return m_drag; }

    int getStartX() const { return m_startX; }
    int getStartY() const { return m_startY; }

    void triggerPivotIndicator(float durationSeconds = 0.5f) {
        m_pivotIndicatorTimer = durationSeconds;
        m_pivotIndicatorDuration = durationSeconds;
    }
    bool isPivotIndicatorActive() const { return m_pivotIndicatorTimer > 0.0f; }
    float getPivotIndicatorAlpha() const {
        if (m_pivotIndicatorDuration <= 0.0f) return 0.0f;
        return std::clamp(m_pivotIndicatorTimer / m_pivotIndicatorDuration, 0.0f, 1.0f);
    }

    void triggerZoom2DIndicator(float x, float y, float durationSeconds = 0.5f) {
        m_zoom2DIndicatorPos = glm::vec2(x, y);
        m_zoom2DIndicatorTimer = durationSeconds;
        m_zoom2DIndicatorDuration = durationSeconds;
    }
    bool isZoom2DIndicatorActive() const { return m_zoom2DIndicatorTimer > 0.0f; }
    glm::vec2 getZoom2DIndicatorPos() const { return m_zoom2DIndicatorPos; }
    float getZoom2DIndicatorAlpha() const {
        if (m_zoom2DIndicatorDuration <= 0.0f) return 0.0f;
        return std::clamp(m_zoom2DIndicatorTimer / m_zoom2DIndicatorDuration, 0.0f, 1.0f);
    }
    void update(float deltaTime) {
        if (m_zoom2DIndicatorTimer > 0.0f) {
            m_zoom2DIndicatorTimer -= deltaTime;
            if (m_zoom2DIndicatorTimer < 0.0f) m_zoom2DIndicatorTimer = 0.0f;
        }
        if (m_pivotIndicatorTimer > 0.0f) {
            m_pivotIndicatorTimer -= deltaTime;
            if (m_pivotIndicatorTimer < 0.0f) m_pivotIndicatorTimer = 0.0f;
        }
    }

private:
    DragMode m_drag = DragMode::None;
    int m_prevX = 0, m_prevY = 0;
    int m_startX = 0, m_startY = 0;
    bool m_snapTriggered = false;

    float m_pivotIndicatorTimer = 0.0f;
    float m_pivotIndicatorDuration = 0.5f;

    glm::vec2 m_zoom2DIndicatorPos{0.0f};
    float m_zoom2DIndicatorTimer = 0.0f;
    float m_zoom2DIndicatorDuration = 0.5f;
};

