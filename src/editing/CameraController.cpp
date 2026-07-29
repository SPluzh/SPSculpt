#include "editing/CameraController.h"
#include "mesh/Mesh.h"
#include "mesh/Octree.h"
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

void CameraController::startDrag(DragMode mode, int mouseX, int mouseY, Camera& camera, const std::vector<Mesh*>& meshes) {
    m_drag = mode;
    m_prevX = mouseX;
    m_prevY = mouseY;
    m_snapTriggered = false;
    camera.start(static_cast<float>(mouseX), static_cast<float>(mouseY));

    if ((mode == DragMode::Orbit || mode == DragMode::Roll || mode == DragMode::Zoom) && camera.getUsePivot()) {
        Ray ray = camera.getRay(static_cast<float>(mouseX), static_cast<float>(mouseY));
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
        }
    }
}

void CameraController::handleEvent(const SDL_Event& e, Camera& camera, const std::vector<Mesh*>& meshes) {
    if (e.type == SDL_MOUSEBUTTONDOWN) {
        int mouseX = e.button.x;
        int mouseY = e.button.y;
        
        bool altPressed = (SDL_GetModState() & KMOD_ALT) != 0;
        bool shiftPressed = (SDL_GetModState() & KMOD_SHIFT) != 0;
        bool ctrlPressed = (SDL_GetModState() & KMOD_CTRL) != 0;

        if (camera.getRef2DMode()) {
            if (e.button.button == SDL_BUTTON_RIGHT) {
                if (e.button.clicks >= 2) {
                    camera.resetView2D();
                    m_drag = DragMode::None;
                } else if (ctrlPressed) {
                    startDrag(DragMode::Zoom2D, mouseX, mouseY, camera, meshes);
                } else if (altPressed) {
                    startDrag(DragMode::Pan2D, mouseX, mouseY, camera, meshes);
                }
            }
            return; // Block all other 3D navigation mouse-down triggers in 2D mode
        }

        if (e.button.button == SDL_BUTTON_MIDDLE) {
            if (e.button.clicks >= 2) {
                camera.resetView();
                m_drag = DragMode::None;
            } else {
                startDrag(DragMode::Pan, mouseX, mouseY, camera, meshes);
            }
        } else if (e.button.button == SDL_BUTTON_RIGHT) {
            if (ctrlPressed) {
                startDrag(DragMode::Zoom, mouseX, mouseY, camera, meshes);
            } else if (shiftPressed && altPressed && camera.getMode() != CameraEnums::CameraMode::ORBIT) {
                startDrag(DragMode::Roll, mouseX, mouseY, camera, meshes);
            } else if (altPressed) {
                startDrag(DragMode::Pan, mouseX, mouseY, camera, meshes);
            } else {
                startDrag(DragMode::Orbit, mouseX, mouseY, camera, meshes);
            }
        }
    } else if (e.type == SDL_MOUSEBUTTONUP) {
        if (e.button.button == SDL_BUTTON_MIDDLE || e.button.button == SDL_BUTTON_RIGHT || e.button.button == SDL_BUTTON_LEFT) {
            stopDrag();
        }
    } else if (e.type == SDL_MOUSEWHEEL) {
        if (camera.getRef2DMode()) {
            return; // Block 3D scroll-zoom in 2D mode
        }
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
                    camera.start(static_cast<float>(mouseX), static_cast<float>(mouseY), false);
                } else {
                    m_snapTriggered = false;
                    camera.rotate(static_cast<float>(mouseX), static_cast<float>(mouseY));
                }
            } else if (m_drag == DragMode::Pan) {
                camera.translate(static_cast<float>(dx), static_cast<float>(dy));
            } else if (m_drag == DragMode::Zoom) {
                camera.zoom(static_cast<float>(dx) * 0.01f);
            } else if (m_drag == DragMode::Pan2D) {
                float w = camera.getWidth() > 0 ? static_cast<float>(camera.getWidth()) : 1.0f;
                float h = camera.getHeight() > 0 ? static_cast<float>(camera.getHeight()) : 1.0f;
                float dxNorm = static_cast<float>(dx) / (w / 2.0f);
                float dyNorm = -static_cast<float>(dy) / (h / 2.0f);
                camera.setView2DOffset(camera.getView2DOffsetX() + dxNorm, camera.getView2DOffsetY() + dyNorm);
            } else if (m_drag == DragMode::Zoom2D) {
                float factor = 1.0f + static_cast<float>(dx) * 0.005f;
                camera.setView2DZoom(camera.getView2DZoom() * factor);
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

