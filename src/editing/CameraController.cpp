#include "editing/CameraController.h"
#include <algorithm>

void CameraController::startDrag(DragMode mode, int mouseX, int mouseY, Camera& camera) {
    m_drag = mode;
    m_prevX = mouseX;
    m_prevY = mouseY;
    m_snapTriggered = false;
    camera.start(static_cast<float>(mouseX), static_cast<float>(mouseY));
}

void CameraController::handleEvent(const SDL_Event& e, Camera& camera) {
    if (e.type == SDL_MOUSEBUTTONDOWN) {
        int mouseX = e.button.x;
        int mouseY = e.button.y;
        
        bool altPressed = (SDL_GetModState() & KMOD_ALT) != 0;
        bool shiftPressed = (SDL_GetModState() & KMOD_SHIFT) != 0;
        bool ctrlPressed = (SDL_GetModState() & KMOD_CTRL) != 0;

        if (e.button.button == SDL_BUTTON_MIDDLE) {
            if (e.button.clicks >= 2) {
                camera.resetView();
                m_drag = DragMode::None;
            } else {
                startDrag(DragMode::Pan, mouseX, mouseY, camera);
            }
        } else if (e.button.button == SDL_BUTTON_RIGHT) {
            if (ctrlPressed) {
                startDrag(DragMode::Zoom, mouseX, mouseY, camera);
            } else if (shiftPressed) {
                startDrag(DragMode::Pan, mouseX, mouseY, camera);
            } else if (altPressed) {
                startDrag(DragMode::Pan, mouseX, mouseY, camera);
            } else {
                startDrag(DragMode::Orbit, mouseX, mouseY, camera);
            }
        } else if (e.button.button == SDL_BUTTON_LEFT && altPressed) {
            startDrag(DragMode::Orbit, mouseX, mouseY, camera);
        }
    } else if (e.type == SDL_MOUSEBUTTONUP) {
        if (e.button.button == SDL_BUTTON_MIDDLE || e.button.button == SDL_BUTTON_RIGHT || e.button.button == SDL_BUTTON_LEFT) {
            stopDrag();
        }
    } else if (e.type == SDL_MOUSEWHEEL) {
        camera.zoom(-static_cast<float>(e.wheel.y) * 0.05f);
    } else if (e.type == SDL_MOUSEMOTION) {
        if (m_drag != DragMode::None) {
            int mouseX = e.motion.x;
            int mouseY = e.motion.y;
            int dx = mouseX - m_prevX;
            int dy = mouseY - m_prevY;

            if (m_drag == DragMode::Orbit) {
                bool shiftPressed = (SDL_GetModState() & KMOD_SHIFT) != 0;
                if (shiftPressed) {
                    if (!m_snapTriggered) {
                        m_snapTriggered = true;
                        camera.snapClosestRotation();
                    }
                    camera.start(static_cast<float>(mouseX), static_cast<float>(mouseY));
                } else {
                    m_snapTriggered = false;
                    camera.rotate(static_cast<float>(mouseX), static_cast<float>(mouseY));
                }
            } else if (m_drag == DragMode::Pan) {
                camera.translate(static_cast<float>(dx), static_cast<float>(dy));
            } else if (m_drag == DragMode::Zoom) {
                camera.zoom(static_cast<float>(dx) * 0.01f);
            }

            m_prevX = mouseX;
            m_prevY = mouseY;
        }
    }
}
