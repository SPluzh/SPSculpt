#include "editing/CameraController.h"
#include "platform/HotkeyManager.h"
#include "mesh/Mesh.h"
#include "mesh/Octree.h"
#include "scene/Scene.h"
#include "render/AngleRenderer.h"
#include <algorithm>
#include <limits>

static bool rayTriangleIntersect(
    const glm::vec3& rayOrigin, const glm::vec3& rayDir,
    const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
    float& t
) {
    const float EPSILON = 0.0000001f;
    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;
    glm::vec3 h = glm::cross(rayDir, edge2);
    float a = glm::dot(edge1, h);
    if (a > -EPSILON && a < EPSILON)
        return false;
    float f = 1.0f / a;
    glm::vec3 s = rayOrigin - v0;
    float u = f * glm::dot(s, h);
    if (u < 0.0f || u > 1.0f)
        return false;
    glm::vec3 q = glm::cross(s, edge1);
    float v = f * glm::dot(rayDir, q);
    if (v < 0.0f || u + v > 1.0f)
        return false;
    t = f * glm::dot(edge2, q);
    return t > EPSILON;
}

static bool pickPivot(Camera& camera, float mouseX, float mouseY, const std::vector<Mesh*>& meshes) {
    if (!camera.getUsePivot()) return false;

    Ray ray = camera.getRay(mouseX, mouseY);
    float minT = std::numeric_limits<float>::infinity();
    Mesh* hitMesh = nullptr;
    glm::vec3 localHitPoint(0.0f);

    for (Mesh* mesh : meshes) {
        if (!mesh) continue;
        glm::mat4 invMatrix = glm::inverse(mesh->matrix);
        glm::vec3 localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
        glm::vec3 localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

        std::vector<uint32_t> candidateFaces = mesh->octree.collectIntersectRay(
            localRayOrigin.x, localRayOrigin.y, localRayOrigin.z,
            localRayDir.x, localRayDir.y, localRayDir.z
        );

        for (uint32_t faceId : candidateFaces) {
            if (faceId >= (uint32_t)mesh->nbFaces) continue;
            uint32_t v0Id = mesh->faces[faceId * 4];
            uint32_t v1Id = mesh->faces[faceId * 4 + 1];
            uint32_t v2Id = mesh->faces[faceId * 4 + 2];
            uint32_t v3Id = mesh->faces[faceId * 4 + 3];

            if (!mesh->vertVisible[v0Id] || !mesh->vertVisible[v1Id] || !mesh->vertVisible[v2Id] || (v3Id != 0xffffffff && !mesh->vertVisible[v3Id])) {
                continue;
            }

            glm::vec3 v0(mesh->verts[v0Id * 3], mesh->verts[v0Id * 3 + 1], mesh->verts[v0Id * 3 + 2]);
            glm::vec3 v1(mesh->verts[v1Id * 3], mesh->verts[v1Id * 3 + 1], mesh->verts[v1Id * 3 + 2]);
            glm::vec3 v2(mesh->verts[v2Id * 3], mesh->verts[v2Id * 3 + 1], mesh->verts[v2Id * 3 + 2]);

            float t;
            if (rayTriangleIntersect(localRayOrigin, localRayDir, v0, v1, v2, t)) {
                if (t < minT) {
                    minT = t;
                    hitMesh = mesh;
                    localHitPoint = localRayOrigin + t * localRayDir;
                }
            }

            if (v3Id != 0xffffffff) {
                glm::vec3 v3(mesh->verts[v3Id * 3], mesh->verts[v3Id * 3 + 1], mesh->verts[v3Id * 3 + 2]);
                if (rayTriangleIntersect(localRayOrigin, localRayDir, v0, v2, v3, t)) {
                    if (t < minT) {
                        minT = t;
                        hitMesh = mesh;
                        localHitPoint = localRayOrigin + t * localRayDir;
                    }
                }
            }
        }
    }

    if (hitMesh) {
        glm::vec3 worldHitPoint = glm::vec3(hitMesh->matrix * glm::vec4(localHitPoint, 1.0f));
        camera.setPivot(worldHitPoint);
        return true;
    }
    return false;
}

void CameraController::stopDrag(Camera* camera) {
    if (m_drag != DragMode::None) {
        if (camera) {
            camera->pushState();
        }
        m_drag = DragMode::None;
        m_dragButton = 0;
        m_snapTriggered = false;
    }
}

void CameraController::startDrag(DragMode mode, int mouseX, int mouseY, Camera& camera, const std::vector<Mesh*>& meshes, uint8_t button) {
    if (m_drag == DragMode::None) {
        camera.pushState();
    }
    m_drag = mode;
    m_dragButton = button;
    m_prevX = mouseX;
    m_prevY = mouseY;
    m_startX = mouseX;
    m_startY = mouseY;
    m_snapTriggered = false;
    camera.start(static_cast<float>(mouseX), static_cast<float>(mouseY));

    if (mode == DragMode::Orbit || mode == DragMode::Roll || mode == DragMode::Zoom) {
        pickPivot(camera, static_cast<float>(mouseX), static_cast<float>(mouseY), meshes);
    }
}

bool CameraController::isNavigationEvent(const SDL_Event& e, bool isRef2D) const {
    if (m_drag != DragMode::None) {
        if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONUP) {
            return true;
        }
    }

    if (e.type == SDL_MOUSEWHEEL) {
        return true;
    }

    if (m_manager) {
        return m_manager->isNavigationEvent(e, isRef2D);
    }

    return isNavigationEvent(e, CameraEnums::NavigationStyle::ZBRUSH);
}

bool CameraController::isNavigationEvent(const SDL_Event& e, CameraEnums::NavigationStyle style) const {
    if (m_manager) {
        return isNavigationEvent(e, false);
    }

    if (m_drag != DragMode::None) {
        if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONUP) {
            return true;
        }
    }

    if (e.type == SDL_MOUSEWHEEL) {
        return true;
    }

    if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
        bool altPressed = (SDL_GetModState() & KMOD_ALT) != 0;
        bool shiftPressed = (SDL_GetModState() & KMOD_SHIFT) != 0;
        bool ctrlPressed = (SDL_GetModState() & KMOD_CTRL) != 0;

        switch (style) {
            case CameraEnums::NavigationStyle::ZBRUSH: {
                if (e.button.button == SDL_BUTTON_RIGHT || e.button.button == SDL_BUTTON_MIDDLE) {
                    return true;
                }
                break;
            }
            case CameraEnums::NavigationStyle::MAYA: {
                // Alt + LMB: Orbit (except Ctrl+Alt+LMB which is SubTool selection in Variant B)
                if (altPressed && !ctrlPressed && e.button.button == SDL_BUTTON_LEFT) {
                    return true;
                }
                // Alt + MMB (Pan), Alt + RMB (Zoom)
                if (altPressed && (e.button.button == SDL_BUTTON_MIDDLE || e.button.button == SDL_BUTTON_RIGHT)) {
                    return true;
                }
                // Ctrl + Shift + MMB: Rotate light
                if (ctrlPressed && shiftPressed && e.button.button == SDL_BUTTON_MIDDLE) {
                    return true;
                }
                // Standalone MMB: Pan or ResetView (double click)
                if (e.button.button == SDL_BUTTON_MIDDLE) {
                    return true;
                }
                break;
            }
            case CameraEnums::NavigationStyle::BLENDER: {
                // MMB: Orbit, Shift+MMB: Pan, Ctrl+MMB: Zoom, Ctrl+Shift+MMB: Rotate Light
                if (e.button.button == SDL_BUTTON_MIDDLE) {
                    return true;
                }
                break;
            }
        }
    }

    return false;
}

void CameraController::handleEventZBrush(const SDL_Event& e, Camera& camera, const std::vector<Mesh*>& meshes, bool animate, Scene* scene, AngleRenderer* renderer) {
    int mouseX = e.button.x;
    int mouseY = e.button.y;
    
    bool altPressed = (SDL_GetModState() & KMOD_ALT) != 0;
    bool shiftPressed = (SDL_GetModState() & KMOD_SHIFT) != 0;
    bool ctrlPressed = (SDL_GetModState() & KMOD_CTRL) != 0;

    if (camera.getRef2DMode()) {
        if (e.button.button == SDL_BUTTON_RIGHT) {
            if (e.button.clicks >= 2) {
                camera.resetView2D();
                stopDrag(&camera);
            } else if (ctrlPressed) {
                startDrag(DragMode::Zoom2D, mouseX, mouseY, camera, meshes, SDL_BUTTON_RIGHT);
            } else if (altPressed) {
                startDrag(DragMode::Pan2D, mouseX, mouseY, camera, meshes, SDL_BUTTON_RIGHT);
            }
        }
        return;
    }

    if (e.button.button == SDL_BUTTON_MIDDLE) {
        if (e.button.clicks >= 2) {
            camera.resetView();
            stopDrag(&camera);
        } else if (shiftPressed) {
            startDrag(DragMode::RotateLight, mouseX, mouseY, camera, meshes, SDL_BUTTON_MIDDLE);
        } else {
            startDrag(DragMode::Pan, mouseX, mouseY, camera, meshes, SDL_BUTTON_MIDDLE);
        }
    } else if (e.button.button == SDL_BUTTON_RIGHT) {
        if (ctrlPressed) {
            startDrag(DragMode::Zoom, mouseX, mouseY, camera, meshes, SDL_BUTTON_RIGHT);
        } else if (shiftPressed && altPressed && camera.getMode() != CameraEnums::CameraMode::ORBIT) {
            startDrag(DragMode::Roll, mouseX, mouseY, camera, meshes, SDL_BUTTON_RIGHT);
        } else if (altPressed) {
            startDrag(DragMode::Pan, mouseX, mouseY, camera, meshes, SDL_BUTTON_RIGHT);
        } else {
            startDrag(DragMode::Orbit, mouseX, mouseY, camera, meshes, SDL_BUTTON_RIGHT);
        }
    }
}

void CameraController::handleEventMaya(const SDL_Event& e, Camera& camera, const std::vector<Mesh*>& meshes, bool animate, Scene* scene, AngleRenderer* renderer) {
    int mouseX = e.button.x;
    int mouseY = e.button.y;
    
    bool altPressed = (SDL_GetModState() & KMOD_ALT) != 0;
    bool shiftPressed = (SDL_GetModState() & KMOD_SHIFT) != 0;
    bool ctrlPressed = (SDL_GetModState() & KMOD_CTRL) != 0;

    if (camera.getRef2DMode()) {
        if (e.button.button == SDL_BUTTON_MIDDLE && e.button.clicks >= 2) {
            camera.resetView2D();
            stopDrag(&camera);
        } else if (altPressed) {
            if (e.button.button == SDL_BUTTON_MIDDLE) {
                startDrag(DragMode::Pan2D, mouseX, mouseY, camera, meshes, SDL_BUTTON_MIDDLE);
            } else if (e.button.button == SDL_BUTTON_RIGHT) {
                startDrag(DragMode::Zoom2D, mouseX, mouseY, camera, meshes, SDL_BUTTON_RIGHT);
            }
        }
        return;
    }

    if (e.button.button == SDL_BUTTON_MIDDLE && e.button.clicks >= 2) {
        camera.resetView();
        stopDrag(&camera);
        return;
    }

    if (ctrlPressed && shiftPressed && e.button.button == SDL_BUTTON_MIDDLE) {
        startDrag(DragMode::RotateLight, mouseX, mouseY, camera, meshes, SDL_BUTTON_MIDDLE);
        return;
    }

    if (altPressed && !ctrlPressed) {
        if (e.button.button == SDL_BUTTON_LEFT) {
            startDrag(DragMode::Orbit, mouseX, mouseY, camera, meshes, SDL_BUTTON_LEFT);
        } else if (e.button.button == SDL_BUTTON_MIDDLE) {
            startDrag(DragMode::Pan, mouseX, mouseY, camera, meshes, SDL_BUTTON_MIDDLE);
        } else if (e.button.button == SDL_BUTTON_RIGHT) {
            startDrag(DragMode::Zoom, mouseX, mouseY, camera, meshes, SDL_BUTTON_RIGHT);
        }
    } else if (e.button.button == SDL_BUTTON_MIDDLE) {
        startDrag(DragMode::Pan, mouseX, mouseY, camera, meshes, SDL_BUTTON_MIDDLE);
    }
}

void CameraController::handleEventBlender(const SDL_Event& e, Camera& camera, const std::vector<Mesh*>& meshes, bool animate, Scene* scene, AngleRenderer* renderer) {
    int mouseX = e.button.x;
    int mouseY = e.button.y;
    
    bool altPressed = (SDL_GetModState() & KMOD_ALT) != 0;
    bool shiftPressed = (SDL_GetModState() & KMOD_SHIFT) != 0;
    bool ctrlPressed = (SDL_GetModState() & KMOD_CTRL) != 0;

    if (camera.getRef2DMode()) {
        if (e.button.button == SDL_BUTTON_MIDDLE) {
            if (e.button.clicks >= 2) {
                camera.resetView2D();
                stopDrag(&camera);
            } else if (shiftPressed) {
                startDrag(DragMode::Pan2D, mouseX, mouseY, camera, meshes, SDL_BUTTON_MIDDLE);
            } else if (ctrlPressed) {
                startDrag(DragMode::Zoom2D, mouseX, mouseY, camera, meshes, SDL_BUTTON_MIDDLE);
            }
        }
        return;
    }

    if (e.button.button == SDL_BUTTON_MIDDLE) {
        if (e.button.clicks >= 2) {
            camera.resetView();
            stopDrag(&camera);
        } else if (ctrlPressed && shiftPressed) {
            startDrag(DragMode::RotateLight, mouseX, mouseY, camera, meshes, SDL_BUTTON_MIDDLE);
        } else if (shiftPressed) {
            startDrag(DragMode::Pan, mouseX, mouseY, camera, meshes, SDL_BUTTON_MIDDLE);
        } else if (ctrlPressed) {
            startDrag(DragMode::Zoom, mouseX, mouseY, camera, meshes, SDL_BUTTON_MIDDLE);
        } else if (altPressed && camera.getMode() != CameraEnums::CameraMode::ORBIT) {
            startDrag(DragMode::Roll, mouseX, mouseY, camera, meshes, SDL_BUTTON_MIDDLE);
        } else {
            startDrag(DragMode::Orbit, mouseX, mouseY, camera, meshes, SDL_BUTTON_MIDDLE);
        }
    }
}

void CameraController::handleEvent(const SDL_Event& e, Camera& camera, const std::vector<Mesh*>& meshes, bool animate, Scene* scene, AngleRenderer* renderer) {
    if (e.type == SDL_MOUSEBUTTONDOWN) {
        if (m_manager) {
            bool isRef2D = camera.getRef2DMode();
            bool ctrl = (SDL_GetModState() & KMOD_CTRL) != 0;
            bool alt = (SDL_GetModState() & KMOD_ALT) != 0;
            bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
            bool doubleClick = (e.button.clicks >= 2);

            NavAction act = m_manager->lookupMouseNavAction(e.button.button, ctrl, alt, shift, doubleClick, isRef2D);
            switch (act) {
                case NavAction::ResetView2D:
                    camera.resetView2D();
                    stopDrag(&camera);
                    return;
                case NavAction::ResetView:
                    camera.resetView();
                    stopDrag(&camera);
                    return;
                case NavAction::Orbit:
                    startDrag(DragMode::Orbit, e.button.x, e.button.y, camera, meshes, e.button.button);
                    return;
                case NavAction::Pan:
                    startDrag(DragMode::Pan, e.button.x, e.button.y, camera, meshes, e.button.button);
                    return;
                case NavAction::Zoom:
                    startDrag(DragMode::Zoom, e.button.x, e.button.y, camera, meshes, e.button.button);
                    return;
                case NavAction::Roll:
                    if (camera.getMode() != CameraEnums::CameraMode::ORBIT) {
                        startDrag(DragMode::Roll, e.button.x, e.button.y, camera, meshes, e.button.button);
                    }
                    return;
                case NavAction::RotateLight:
                    startDrag(DragMode::RotateLight, e.button.x, e.button.y, camera, meshes, e.button.button);
                    return;
                case NavAction::Pan2D:
                    startDrag(DragMode::Pan2D, e.button.x, e.button.y, camera, meshes, e.button.button);
                    return;
                case NavAction::Zoom2D:
                    startDrag(DragMode::Zoom2D, e.button.x, e.button.y, camera, meshes, e.button.button);
                    return;
                default:
                    break;
            }
        } else {
            switch (camera.getNavigationStyle()) {
                case CameraEnums::NavigationStyle::MAYA:
                    handleEventMaya(e, camera, meshes, animate, scene, renderer);
                    break;
                case CameraEnums::NavigationStyle::BLENDER:
                    handleEventBlender(e, camera, meshes, animate, scene, renderer);
                    break;
                case CameraEnums::NavigationStyle::ZBRUSH:
                default:
                    handleEventZBrush(e, camera, meshes, animate, scene, renderer);
                    break;
            }
        }
    } else if (e.type == SDL_MOUSEBUTTONUP) {
        if (m_drag != DragMode::None) {
            if (m_dragButton == 0 || e.button.button == m_dragButton ||
                e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_MIDDLE || e.button.button == SDL_BUTTON_RIGHT) {
                stopDrag(&camera);
            }
        }
    } else if (e.type == SDL_MOUSEWHEEL) {
        if (camera.getRef2DMode()) {
            int rawX = 0, rawY = 0;
            SDL_GetMouseState(&rawX, &rawY);
            float mouseX = static_cast<float>(rawX);
            float mouseY = static_cast<float>(rawY);
            if (camera.isSplitViewport() && rawX >= camera.getWidth()) {
                mouseX -= static_cast<float>(camera.getWidth());
            }
            float factor = (e.wheel.y > 0) ? 1.15f : (e.wheel.y < 0 ? (1.0f / 1.15f) : 1.0f);
            camera.pushState();
            camera.zoom2D(factor, mouseX, mouseY);
            camera.pushState();
            return;
        }

        int rawX = 0, rawY = 0;
        SDL_GetMouseState(&rawX, &rawY);
        float mouseX = static_cast<float>(rawX);
        float mouseY = static_cast<float>(rawY);
        if (camera.isSplitViewport() && rawX >= camera.getWidth()) {
            mouseX -= static_cast<float>(camera.getWidth());
        }

        pickPivot(camera, mouseX, mouseY, meshes);

        camera.pushState();
        camera.zoom(static_cast<float>(e.wheel.y) * 0.05f, mouseX, mouseY);
        camera.pushState();
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
                        camera.snapClosestRotation(animate);
                    }
                    camera.start(static_cast<float>(mouseX), static_cast<float>(mouseY), false);
                } else {
                    m_snapTriggered = false;
                    camera.rotate(static_cast<float>(mouseX), static_cast<float>(mouseY));
                }
            } else if (m_drag == DragMode::Pan) {
                camera.translate(static_cast<float>(dx), static_cast<float>(dy));
            } else if (m_drag == DragMode::Zoom) {
                float zoomDelta = (static_cast<float>(dx) - static_cast<float>(dy)) * 0.008f;
                camera.zoom(zoomDelta, static_cast<float>(m_startX), static_cast<float>(m_startY));
            } else if (m_drag == DragMode::RotateLight) {
                float deltaAngle = static_cast<float>(dx) * 0.008f;
                if (scene) {
                    scene->rotateAllLights(deltaAngle);
                }
                if (renderer) {
                    renderer->rotateEnv(deltaAngle);
                }
            } else if (m_drag == DragMode::Pan2D) {
                float w = camera.getWidth() > 0 ? static_cast<float>(camera.getWidth()) : 1.0f;
                float h = camera.getHeight() > 0 ? static_cast<float>(camera.getHeight()) : 1.0f;
                float dxNorm = static_cast<float>(dx) / (w / 2.0f);
                float dyNorm = -static_cast<float>(dy) / (h / 2.0f);
                camera.setView2DOffset(camera.getView2DOffsetX() + dxNorm, camera.getView2DOffsetY() + dyNorm);
            } else if (m_drag == DragMode::Zoom2D) {
                float factor = 1.0f + (static_cast<float>(dx) - static_cast<float>(dy)) * 0.004f;
                camera.zoom2D(factor, static_cast<float>(m_startX), static_cast<float>(m_startY));
            } else if (m_drag == DragMode::Roll) {
                float w = camera.getWidth() > 0 ? static_cast<float>(camera.getWidth()) : 1000.0f;
                float piVal = 3.14159265358979323846f;
                float baseAngleScale = camera.isSplitViewport() ? piVal : (2.0f * piVal);
                float angle = (static_cast<float>(dx) / w) * baseAngleScale * camera.getSpeedRoll();
                camera.roll(angle);
            }

            m_prevX = mouseX;
            m_prevY = mouseY;
        }
    }
}

