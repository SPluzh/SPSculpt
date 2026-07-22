#include "editing/SculptManager.h"
#include "sculpt/SculptEngine.h"
#include "mesh/NormalCalc.h"
#include "mesh/Topology.h"
#include "mesh/Mesh.h"
#include <vector>
#include <algorithm>
#include <iostream>
#include <limits>
#include <cmath>

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

SculptManager::SculptManager() {}

void SculptManager::handleEvent(const SDL_Event& event, Scene& scene) {
    Mesh* mesh = scene.getSelected();
    Camera& camera = scene.getCamera();

    if (event.type == SDL_MOUSEBUTTONDOWN) {
        int mouseX = event.button.x;
        int mouseY = event.button.y;

        m_prevMouseX = mouseX;
        m_prevMouseY = mouseY;

        // Middle button and Right button are always camera controls
        if (event.button.button == SDL_BUTTON_MIDDLE || event.button.button == SDL_BUTTON_RIGHT) {
            m_cameraController.handleEvent(event, camera);
            return;
        }

        // Left button:
        if (event.button.button == SDL_BUTTON_LEFT) {
            if (!mesh) {
                m_cameraController.startDrag(CameraController::DragMode::Orbit, mouseX, mouseY, camera);
                return;
            }

            // Cast ray using Camera
            Ray ray = camera.getRay((float)mouseX, (float)mouseY);
            glm::mat4 invMatrix = glm::inverse(mesh->matrix);
            glm::vec3 localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
            glm::vec3 localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

            // Find intersection using Octree candidate faces in local space
            std::vector<uint32_t> candidateFaces = mesh->octree.collectIntersectRay(
                localRayOrigin.x, localRayOrigin.y, localRayOrigin.z,
                localRayDir.x, localRayDir.y, localRayDir.z
            );

            float minT = std::numeric_limits<float>::infinity();
            uint32_t intersectFaceId = 0xffffffff;

            for (uint32_t faceId : candidateFaces) {
                if (faceId >= (uint32_t)mesh->nbFaces) continue;
                uint32_t v0Id = mesh->faces[faceId * 4];
                uint32_t v1Id = mesh->faces[faceId * 4 + 1];
                uint32_t v2Id = mesh->faces[faceId * 4 + 2];
                uint32_t v3Id = mesh->faces[faceId * 4 + 3];

                glm::vec3 v0(mesh->verts[v0Id * 3], mesh->verts[v0Id * 3 + 1], mesh->verts[v0Id * 3 + 2]);
                glm::vec3 v1(mesh->verts[v1Id * 3], mesh->verts[v1Id * 3 + 1], mesh->verts[v1Id * 3 + 2]);
                glm::vec3 v2(mesh->verts[v2Id * 3], mesh->verts[v2Id * 3 + 1], mesh->verts[v2Id * 3 + 2]);

                float t;
                if (rayTriangleIntersect(localRayOrigin, localRayDir, v0, v1, v2, t)) {
                    if (t < minT) {
                        minT = t;
                        intersectFaceId = faceId;
                    }
                }

                if (v3Id != 0xffffffff) {
                    glm::vec3 v3(mesh->verts[v3Id * 3], mesh->verts[v3Id * 3 + 1], mesh->verts[v3Id * 3 + 2]);
                    if (rayTriangleIntersect(localRayOrigin, localRayDir, v0, v2, v3, t)) {
                        if (t < minT) {
                            minT = t;
                            intersectFaceId = faceId;
                        }
                    }
                }
            }

            if (intersectFaceId != 0xffffffff) {
                scene.pushHistoryState();
                m_isSculpting = true;
                m_initialIntersection = localRayOrigin + minT * localRayDir;
                m_initialIntersectionNormal = glm::vec3(
                    mesh->faceNormals[intersectFaceId * 3],
                    mesh->faceNormals[intersectFaceId * 3 + 1],
                    mesh->faceNormals[intersectFaceId * 3 + 2]
                );
                m_currentIntersection = m_initialIntersection;
                m_currentIntersectionNormal = m_initialIntersectionNormal;

                // Copy vertices to proxy at start of stroke
                mesh->vertProxy = mesh->verts;
            } else {
                m_cameraController.startDrag(CameraController::DragMode::Orbit, mouseX, mouseY, camera);
            }
        }
    } else if (event.type == SDL_MOUSEBUTTONUP) {
        m_isSculpting = false;
        m_cameraController.handleEvent(event, camera);
    } else if (event.type == SDL_MOUSEWHEEL) {
        m_cameraController.handleEvent(event, camera);
    } else if (event.type == SDL_MOUSEMOTION) {
        int mouseX = event.motion.x;
        int mouseY = event.motion.y;

        int dx = mouseX - m_prevMouseX;
        int dy = mouseY - m_prevMouseY;

        m_prevMouseX = mouseX;
        m_prevMouseY = mouseY;

        if (m_cameraController.isDragging()) {
            m_cameraController.handleEvent(event, camera);
        } else if (m_isSculpting && mesh) {
            Ray ray = camera.getRay((float)mouseX, (float)mouseY);
            glm::mat4 invMatrix = glm::inverse(mesh->matrix);
            glm::vec3 localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
            glm::vec3 localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

            // Simple raycast intersection update in local space
            std::vector<uint32_t> candidateFaces = mesh->octree.collectIntersectRay(
                localRayOrigin.x, localRayOrigin.y, localRayOrigin.z,
                localRayDir.x, localRayDir.y, localRayDir.z
            );

            float minT = std::numeric_limits<float>::infinity();
            uint32_t intersectFaceId = 0xffffffff;

            for (uint32_t faceId : candidateFaces) {
                if (faceId >= (uint32_t)mesh->nbFaces) continue;
                uint32_t v0Id = mesh->faces[faceId * 4];
                uint32_t v1Id = mesh->faces[faceId * 4 + 1];
                uint32_t v2Id = mesh->faces[faceId * 4 + 2];
                uint32_t v3Id = mesh->faces[faceId * 4 + 3];

                glm::vec3 v0(mesh->verts[v0Id * 3], mesh->verts[v0Id * 3 + 1], mesh->verts[v0Id * 3 + 2]);
                glm::vec3 v1(mesh->verts[v1Id * 3], mesh->verts[v1Id * 3 + 1], mesh->verts[v1Id * 3 + 2]);
                glm::vec3 v2(mesh->verts[v2Id * 3], mesh->verts[v2Id * 3 + 1], mesh->verts[v2Id * 3 + 2]);

                float t;
                if (rayTriangleIntersect(localRayOrigin, localRayDir, v0, v1, v2, t)) {
                    if (t < minT) {
                        minT = t;
                        intersectFaceId = faceId;
                    }
                }

                if (v3Id != 0xffffffff) {
                    glm::vec3 v3(mesh->verts[v3Id * 3], mesh->verts[v3Id * 3 + 1], mesh->verts[v3Id * 3 + 2]);
                    if (rayTriangleIntersect(localRayOrigin, localRayDir, v0, v2, v3, t)) {
                        if (t < minT) {
                            minT = t;
                            intersectFaceId = faceId;
                        }
                    }
                }
            }

            if (intersectFaceId != 0xffffffff) {
                m_currentIntersection = localRayOrigin + minT * localRayDir;
                m_currentIntersectionNormal = glm::vec3(
                    mesh->faceNormals[intersectFaceId * 3],
                    mesh->faceNormals[intersectFaceId * 3 + 1],
                    mesh->faceNormals[intersectFaceId * 3 + 2]
                );
            }

            // Calculate dynamic world radius based on camera distance / type
            glm::vec3 cameraPos = camera.computePosition();
            glm::vec3 worldIntersection = glm::vec3(mesh->matrix * glm::vec4(m_currentIntersection, 1.0f));
            float hitDepth = glm::distance(cameraPos, worldIntersection);

            float worldRadius = 0.0f;
            if (camera.isOrthographic()) {
                worldRadius = m_brushRadius * 2.0f * camera.getOrthoZoom();
            } else {
                float fov_rad = camera.getFovDegrees() * (float)M_PI / 180.0f;
                float screenHeight = (float)camera.getHeight();
                if (screenHeight <= 0.0f) screenHeight = 1.0f;
                worldRadius = m_brushRadius * hitDepth * std::tan(fov_rad * 0.5f) * 2.0f / screenHeight;
            }

            // Convert world radius to local radius
            glm::vec3 col0(mesh->matrix[0][0], mesh->matrix[0][1], mesh->matrix[0][2]);
            float scale = glm::length(col0);
            if (scale < 1e-12f) scale = 1.0f;
            float localRadius = worldRadius / scale;

            // Perform brush deformation
            float radius2 = localRadius * localRadius;
            std::vector<uint32_t> pickedVertices = mesh->octree.pickVerticesInSphere(
                m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z, radius2, mesh->vertVisible.data()
            );

            if (!pickedVertices.empty()) {
                BrushType activeBrush = m_currentBrush;
                
                // Shift key switches to Smooth brush temporarily
                if (SDL_GetModState() & KMOD_SHIFT) {
                    activeBrush = BRUSH_SMOOTH;
                }

                // Alt key inverts the tool's base negative state (alternative brush variation like Ctrl used to be)
                bool altPressed = (SDL_GetModState() & KMOD_ALT) != 0;
                bool negative = m_negative ^ altPressed;

                int deformedCount = 0;

                switch (activeBrush) {
                    case BRUSH_FLATTEN: {
                        glm::vec3 areaCenter = m_currentIntersection;
                        glm::vec3 areaNormal = m_currentIntersectionNormal;
                        std::vector<float> areaResults(7, 0.0f);
                        computeAreaNormalAndCenter(
                            reinterpret_cast<uintptr_t>(mesh->verts.data()),
                            reinterpret_cast<uintptr_t>(mesh->normals.data()),
                            reinterpret_cast<uintptr_t>(mesh->materials.data()),
                            reinterpret_cast<uintptr_t>(pickedVertices.data()),
                            pickedVertices.size(),
                            reinterpret_cast<uintptr_t>(areaResults.data())
                        );
                        areaNormal = glm::vec3(areaResults[0], areaResults[1], areaResults[2]);
                        areaCenter = glm::vec3(areaResults[3], areaResults[4], areaResults[5]);

                        deformedCount = strokeFlatten(
                            reinterpret_cast<uintptr_t>(mesh->verts.data()),
                            reinterpret_cast<uintptr_t>(mesh->vertProxy.data()),
                            reinterpret_cast<uintptr_t>(mesh->materials.data()),
                            reinterpret_cast<uintptr_t>(pickedVertices.data()),
                            pickedVertices.size(),
                            m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
                            areaCenter.x, areaCenter.y, areaCenter.z,
                            areaNormal.x, areaNormal.y, areaNormal.z,
                            localRadius, m_brushIntensity,
                            negative, false, false,
                            0.0f, true,
                            false, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0, false
                        );
                        break;
                    }
                    case BRUSH_SMOOTH: {
                        deformedCount = strokeSmooth(
                            reinterpret_cast<uintptr_t>(mesh->verts.data()),
                            reinterpret_cast<uintptr_t>(mesh->normals.data()),
                            reinterpret_cast<uintptr_t>(mesh->materials.data()),
                            reinterpret_cast<uintptr_t>(mesh->vrvStartCount.data()),
                            reinterpret_cast<uintptr_t>(mesh->vertRingVert.data()),
                            reinterpret_cast<uintptr_t>(mesh->vertOnEdge.data()),
                            reinterpret_cast<uintptr_t>(pickedVertices.data()),
                            pickedVertices.size(),
                            m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
                            localRadius, m_brushIntensity,
                            false,
                            0.0f, true,
                            false, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0, false
                        );
                        break;
                    }
                    case BRUSH_INFLATE: {
                        deformedCount = strokeInflate(
                            reinterpret_cast<uintptr_t>(mesh->verts.data()),
                            reinterpret_cast<uintptr_t>(mesh->vertProxy.data()),
                            reinterpret_cast<uintptr_t>(mesh->materials.data()),
                            reinterpret_cast<uintptr_t>(mesh->normals.data()),
                            reinterpret_cast<uintptr_t>(pickedVertices.data()),
                            pickedVertices.size(),
                            m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
                            localRadius, m_brushIntensity,
                            negative,
                            0.0f, true,
                            false, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0, false,
                            false, 0
                        );
                        break;
                    }
                    case BRUSH_PINCH: {
                        deformedCount = strokePinch(
                            reinterpret_cast<uintptr_t>(mesh->verts.data()),
                            reinterpret_cast<uintptr_t>(mesh->materials.data()),
                            reinterpret_cast<uintptr_t>(pickedVertices.data()),
                            pickedVertices.size(),
                            m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
                            localRadius, m_brushIntensity,
                            negative,
                            0.0f, true,
                            false, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0, false,
                            false, 0
                        );
                        break;
                    }
                    case BRUSH_CREASE: {
                        deformedCount = strokeCrease(
                            reinterpret_cast<uintptr_t>(mesh->verts.data()),
                            reinterpret_cast<uintptr_t>(mesh->vertProxy.data()),
                            reinterpret_cast<uintptr_t>(mesh->materials.data()),
                            reinterpret_cast<uintptr_t>(pickedVertices.data()),
                            pickedVertices.size(),
                            m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
                            m_currentIntersectionNormal.x, m_currentIntersectionNormal.y, m_currentIntersectionNormal.z,
                            localRadius, m_brushIntensity,
                            negative,
                            0.0f, true,
                            false, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0, false,
                            false, 0
                        );
                        break;
                    }
                    case BRUSH_VTOOL: {
                        deformedCount = strokeVTool(
                            reinterpret_cast<uintptr_t>(mesh->verts.data()),
                            reinterpret_cast<uintptr_t>(mesh->vertProxy.data()),
                            reinterpret_cast<uintptr_t>(mesh->materials.data()),
                            reinterpret_cast<uintptr_t>(pickedVertices.data()),
                            pickedVertices.size(),
                            m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
                            m_currentIntersectionNormal.x, m_currentIntersectionNormal.y, m_currentIntersectionNormal.z,
                            localRadius, m_brushIntensity,
                            negative,
                            -0.4f, true,
                            false, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0, false,
                            false, 0
                        );
                        break;
                    }
                    case BRUSH_MOVE: {
                        glm::vec3 dragDirection = m_currentIntersection - m_initialIntersection;
                        deformedCount = strokeMove(
                            reinterpret_cast<uintptr_t>(mesh->verts.data()),
                            reinterpret_cast<uintptr_t>(mesh->vertProxy.data()),
                            reinterpret_cast<uintptr_t>(mesh->materials.data()),
                            reinterpret_cast<uintptr_t>(pickedVertices.data()),
                            pickedVertices.size(),
                            m_initialIntersection.x, m_initialIntersection.y, m_initialIntersection.z,
                            dragDirection.x, dragDirection.y, dragDirection.z,
                            localRadius,
                            0.0f, true,
                            false, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0, false,
                            false, 0
                        );
                        break;
                    }
                    case BRUSH_DRAG: {
                        glm::vec3 dragDirection = m_currentIntersection - m_initialIntersection;
                        deformedCount = strokeDrag(
                            reinterpret_cast<uintptr_t>(mesh->verts.data()),
                            reinterpret_cast<uintptr_t>(mesh->materials.data()),
                            reinterpret_cast<uintptr_t>(pickedVertices.data()),
                            pickedVertices.size(),
                            m_initialIntersection.x, m_initialIntersection.y, m_initialIntersection.z,
                            dragDirection.x, dragDirection.y, dragDirection.z,
                            localRadius,
                            0.0f, true,
                            false, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0, false,
                            false, 0
                        );
                        break;
                    }
                    case BRUSH_ELASTIC: {
                        glm::vec3 dragDirection = m_currentIntersection - m_initialIntersection;
                        deformedCount = strokeElastic(
                            reinterpret_cast<uintptr_t>(mesh->verts.data()),
                            reinterpret_cast<uintptr_t>(mesh->vertProxy.data()),
                            reinterpret_cast<uintptr_t>(mesh->materials.data()),
                            reinterpret_cast<uintptr_t>(pickedVertices.data()),
                            pickedVertices.size(),
                            m_initialIntersection.x, m_initialIntersection.y, m_initialIntersection.z,
                            dragDirection.x, dragDirection.y, dragDirection.z,
                            localRadius, 0.5f,
                            0.0f, true,
                            false, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0, false,
                            false, 0
                        );
                        break;
                    }
                }

                if (deformedCount > 0) {
                    std::vector<uint32_t> dirtyFaces(mesh->nbFaces, 0);
                    std::vector<uint32_t> iFaces;
                    for (uint32_t vid : pickedVertices) {
                        uint32_t start = mesh->vrfStartCount[vid * 2];
                        uint32_t count = mesh->vrfStartCount[vid * 2 + 1];
                        for (uint32_t k = 0; k < count; ++k) {
                            uint32_t fid = mesh->vertRingFace[start + k];
                            if (dirtyFaces[fid] == 0) {
                                dirtyFaces[fid] = 1;
                                iFaces.push_back(fid);
                            }
                        }
                    }

                    updateFaceNormalsAndBoxes(
                        mesh->verts.data(), mesh->nbVerts,
                        mesh->faces.data(), mesh->nbFaces,
                        iFaces.data(), iFaces.size(),
                        mesh->faceNormals.data(),
                        mesh->faceBoxes.data(),
                        mesh->faceCenters.data()
                    );

                    updateVertexNormals(
                        pickedVertices.data(), pickedVertices.size(), mesh->nbVerts,
                        mesh->vrfStartCount.data(),
                        mesh->vertRingFace.data(),
                        mesh->faceNormals.data(),
                        mesh->normals.data()
                    );

                    mesh->octree.update(
                        mesh->verts.data(), mesh->nbVerts,
                        mesh->faces.data(), mesh->nbFaces,
                        mesh->faceBoxes.data(),
                        iFaces.data(), iFaces.size()
                    );

                    mesh->setDirty(true);
                }
            }
        }
    }
}

void SculptManager::processFrame(Scene& scene) {
    if (m_cameraController.isDragging()) {
        m_cursor.hide();
    } else {
        m_cursor.update(m_prevMouseX, m_prevMouseY, scene, m_brushRadius, m_useSym, m_symAxis);
    }
}
