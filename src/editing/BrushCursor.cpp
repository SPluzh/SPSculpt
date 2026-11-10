#include "editing/BrushCursor.h"
#include "editing/SculptManager.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "render/AngleRenderer.h"
#include "mesh/Mesh.h"
#include "platform/TabletInput.h"
#include "common/OptFlags.h"
#include <SDL.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <limits>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

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

static bool checkOcclusion(const glm::vec3& worldPos, const Camera& camera, Mesh* mesh) {
    if (!mesh) return false;

    glm::vec3 screenPos = camera.project(worldPos);
    Ray ray = camera.getRay(screenPos.x, screenPos.y);

    glm::mat4 invMatrix = glm::inverse(mesh->matrix);
    glm::vec3 localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
    glm::vec3 localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

    float targetLocalT = glm::distance(localRayOrigin, glm::vec3(invMatrix * glm::vec4(worldPos, 1.0f)));

    mesh->ensureBVHBuilt();
    RayHit hit;
    if (mesh->bvh.raycast(*mesh, localRayOrigin, localRayDir, hit)) {
        if (hit.t < targetLocalT * 0.995f) {
            return true;
        }
    }
    return false;
}


BrushCursor::BrushCursor() {
    m_state.visible = false;
    m_state.hitMesh = false;
}

void BrushCursor::update(int mouseX, int mouseY,
                          const Scene& scene,
                          float brushRadius,
                          bool useSym,
                          bool symX,
                          bool symY,
                          bool symZ,
                          bool isSculpting,
                          BrushType brushType,
                          bool hasActiveStrokeHit,
                          const glm::vec3& activeStrokeHitPt,
                          const glm::vec3& activeStrokeHitNormal,
                          float focalShift,
                          float hardness,
                          const glm::vec3& paintColor,
                          SymmetryMode symMode,
                          ModalMode modalMode) {
    if (brushType == BRUSH_VISIBILITY || brushType == BRUSH_MASK_GRADIENT_BLUR ||
        brushType == BRUSH_MEASURE || brushType == BRUSH_DIVIDER || brushType == BRUSH_TRANSFORM ||
        brushType == BRUSH_POSE || brushType == BRUSH_TUBE ||
        brushType == BRUSH_QUAD_DRAW || brushType == BRUSH_EXTRACT ||
        brushType == BRUSH_COMPONENT_SELECT || brushType == BRUSH_EDGE_CREASE ||
        brushType == BRUSH_CURVE_FOLDS ||
        brushType == BRUSH_CLIP_CURVE || brushType == BRUSH_TRIM || brushType == BRUSH_SPLIT) {
        m_state.visible = false;
        m_state.hitMesh = false;
        return;
    }

    float innerRatio = 0.5f;
    if (brushType == BRUSH_MASK || brushType == BRUSH_PAINT) {
        innerRatio = hardness;
    } else {
        innerRatio = (1.0f - focalShift) / 2.0f;
    }
    innerRatio = std::max(0.0f, std::min(1.0f, innerRatio));
    Mesh* mesh = scene.getSelected();
    const Camera& cameraLeft = scene.getCamera();
    const Camera* cameraRight = (scene.getSplitMode() != Scene::SplitMode::OFF) ? scene.getCameraByIndex(1) : nullptr;
    int activeViewport = (scene.getSplitMode() != Scene::SplitMode::OFF && scene.getActiveViewport() == 1 && cameraRight) ? 1 : 0;
    if (mesh && !scene.isMeshRenderVisible(mesh, activeViewport)) {
        mesh = nullptr;
    }
    const Camera& camera = (activeViewport == 1 && cameraRight) 
                           ? *cameraRight 
                           : cameraLeft;

    bool hitMesh = false;
    glm::vec3 worldPt{0.0f};
    glm::vec3 worldNormal{0.0f, 1.0f, 0.0f};
    glm::vec3 localPt{0.0f};
    glm::vec3 localNormal{0.0f, 1.0f, 0.0f};

    if (hasActiveStrokeHit) {
        hitMesh = true;
        worldPt = activeStrokeHitPt;
        worldNormal = activeStrokeHitNormal;
    } else if (mesh) {
        Ray ray = camera.getRay((float)mouseX, (float)mouseY);
        glm::mat4 invMatrix = glm::inverse(mesh->matrix);
        glm::vec3 localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
        glm::vec3 localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

        mesh->ensureBVHBuilt();
        RayHit hit;
        if (mesh->bvh.raycast(*mesh, localRayOrigin, localRayDir, hit)) {
            hitMesh = true;
            localPt = hit.position;
            if (hit.faceId < (uint32_t)mesh->nbFaces && mesh->faceNormals.size() >= (size_t)mesh->nbFaces * 3) {
                localNormal = glm::vec3(
                    mesh->faceNormals[hit.faceId * 3],
                    mesh->faceNormals[hit.faceId * 3 + 1],
                    mesh->faceNormals[hit.faceId * 3 + 2]
                );
            } else {
                localNormal = hit.normal;
            }

            worldPt = glm::vec3(mesh->matrix * glm::vec4(localPt, 1.0f));
            glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(mesh->matrix)));
            worldNormal = glm::normalize(normalMatrix * localNormal);
        }
    }

    m_state.visible = true;
    m_state.showCircle = !isSculpting;
    m_state.hitMesh = hitMesh;

    // Color Setup based on active brush & hover status
    bool drawCircle = !isSculpting;
    if (brushType == BRUSH_SMOOTH) {
        if (drawCircle && hitMesh) {
            m_state.color = glm::vec3(0.0f, 0.4f, 0.8f);
        } else {
            m_state.color = glm::vec3(0.0f, 0.6f, 1.0f);
        }
    } else if (brushType == BRUSH_RELAX) {
        if (drawCircle && hitMesh) {
            m_state.color = glm::vec3(0.0f, 0.75f, 0.7f);
        } else {
            m_state.color = glm::vec3(0.0f, 0.9f, 0.85f);
        }
    } else if (brushType == BRUSH_MASK) {
        if (drawCircle && hitMesh) {
            m_state.color = glm::vec3(0.9f, 0.9f, 0.0f);
        } else {
            m_state.color = glm::vec3(1.0f, 1.0f, 0.0f);
        }
    } else if (brushType == BRUSH_VISIBILITY) {
        if (drawCircle && hitMesh) {
            m_state.color = glm::vec3(0.6f, 0.0f, 0.9f);
        } else {
            m_state.color = glm::vec3(0.6f, 0.2f, 0.9f);
        }
    } else if (brushType == BRUSH_DYNAMIC_TOPOLOGY) {
        if (drawCircle && hitMesh) {
            m_state.color = glm::vec3(0.66f, 0.33f, 0.97f);
        } else {
            m_state.color = glm::vec3(0.75f, 0.45f, 1.0f);
        }
    } else if (brushType == BRUSH_PAINT) {
        m_state.color = paintColor;
    } else if (brushType == BRUSH_SNAKEHOOK) {
        if (drawCircle && hitMesh) {
            m_state.color = glm::vec3(0.95f, 0.45f, 0.1f);
        } else {
            m_state.color = glm::vec3(1.0f, 0.6f, 0.2f);
        }
    } else {
        if (drawCircle && hitMesh) {
            m_state.color = glm::vec3(0.8f, 0.0f, 0.0f);
        } else {
            m_state.color = glm::vec3(0.8f, 0.4f, 0.0f);
        }
    }

    m_state.isScreenspace = isSculpting || !hitMesh;

    if (hitMesh) {
        m_state.hitPoint = worldPt;
        m_state.hitNormal = worldNormal;

        bool isModal = (modalMode != ModalMode::NONE);
        glm::vec3 circleNormalLeft = worldNormal;
        if (isModal) {
            glm::vec3 camPos = cameraLeft.computePosition();
            glm::vec3 dir = camPos - worldPt;
            if (glm::length(dir) > 1e-6f) {
                circleNormalLeft = glm::normalize(dir);
            } else {
                glm::mat4 invViewLeft = glm::inverse(cameraLeft.getViewMatrix());
                circleNormalLeft = glm::normalize(glm::vec3(invViewLeft[2]));
            }
        }

        // --- Left Viewport MVP construction ---
        float worldRadiusLeft = 0.0f;
        if (cameraLeft.isOrthographic()) {
            worldRadiusLeft = brushRadius * 2.0f * cameraLeft.getOrthoZoom();
        } else {
            float fov_rad = cameraLeft.getFovDegrees() * (float)M_PI / 180.0f;
            float screenHeight = (float)cameraLeft.getHeight();
            if (screenHeight <= 0.0f) screenHeight = 1.0f;
            float hitDepth = glm::distance(cameraLeft.computePosition(), worldPt);
            worldRadiusLeft = brushRadius * hitDepth * std::tan(fov_rad * 0.5f) * 2.0f / screenHeight;
        }
        m_state.radius = worldRadiusLeft; // For compatibility / general usage

        float tiltX = 0.0f;
        float tiltY = 0.0f;
#ifdef _WIN32
        if (!isModal && g_tablet.isAvailable() && g_tablet.isPenActive() && g_tablet.isTiltEnabled()) {
            tiltX = g_tablet.getTiltX();
            tiltY = g_tablet.getTiltY();
        }
#endif

        m_state.circleMVP = buildCircleMVP(worldPt, circleNormalLeft, worldRadiusLeft, cameraLeft, tiltX, tiltY);
        float innerWorldRadiusLeft = worldRadiusLeft * innerRatio;
        m_state.innerCircleMVP = buildCircleMVP(worldPt, circleNormalLeft, innerWorldRadiusLeft, cameraLeft, tiltX, tiltY);

        float pressureDotFactor = 1.0f;
#ifdef _WIN32
        if (g_tablet.isPressureCursorEnabled() && g_tablet.isPressureEnabled() && g_tablet.isAvailable() && g_tablet.isPenActive()) {
            pressureDotFactor = g_tablet.getPressure();
        }
#endif
        float constRadiusLeft = 2.5f * (worldRadiusLeft / brushRadius) * pressureDotFactor;
        m_state.dotMVP = buildCircleMVP(worldPt, circleNormalLeft, constRadiusLeft, cameraLeft, tiltX, tiltY);

        m_state.symMVPs.clear();
        m_state.symOccluded.clear();
        const bool throttle = OptFlags::get().cursorThrottle;
        const uint64_t nowTicks = SDL_GetTicks64();
        const bool camChangedLeft = (cameraLeft.getViewMatrix() != m_lastOccViewLeft);
        const bool camChangedRight = (cameraRight && cameraRight->getViewMatrix() != m_lastOccViewRight);
        const float dxOcc = (float)(mouseX - m_lastOccMouseX);
        const float dyOcc = (float)(mouseY - m_lastOccMouseY);
        const bool mouseMovedSignificantly = (dxOcc * dxOcc + dyOcc * dyOcc) > 2.25f; // > 1.5 px
        const bool timeExpired = (nowTicks - m_lastOccTicks) > 40; // ~25 Hz

        if (useSym && (symX || symY || symZ) && mesh) {
            glm::mat4 invMatrix = glm::inverse(mesh->matrix);
            glm::vec3 lPt = glm::vec3(invMatrix * glm::vec4(worldPt, 1.0f));
            glm::vec3 lNormal = glm::normalize(glm::mat3(invMatrix) * worldNormal);
            glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(mesh->matrix)));

            std::vector<float> xVals = { 1.0f };
            std::vector<float> yVals = { 1.0f };
            std::vector<float> zVals = { 1.0f };
            if (symX) xVals.push_back(-1.0f);
            if (symY) yVals.push_back(-1.0f);
            if (symZ) zVals.push_back(-1.0f);

            std::vector<glm::vec3> worldSymPts;
            worldSymPts.reserve(7);

            for (float sx : xVals) {
                for (float sy : yVals) {
                    for (float sz : zVals) {
                        if (sx == 1.0f && sy == 1.0f && sz == 1.0f) continue;

                        glm::vec3 sScale(sx, sy, sz);
                        glm::vec3 localSymPt = reflectPointSymmetry(lPt, sScale, mesh, symMode);
                        glm::vec3 localSymNormal = reflectVectorSymmetry(lNormal, sScale, mesh, symMode);

                        glm::vec3 worldSymPt = glm::vec3(mesh->matrix * glm::vec4(localSymPt, 1.0f));
                        glm::vec3 worldSymNormal = glm::normalize(normalMatrix * localSymNormal);

                        glm::vec3 circleSymNormalLeft = worldSymNormal;
                        if (isModal) {
                            glm::vec3 camPos = cameraLeft.computePosition();
                            glm::vec3 dir = camPos - worldSymPt;
                            if (glm::length(dir) > 1e-6f) {
                                circleSymNormalLeft = glm::normalize(dir);
                            } else {
                                circleSymNormalLeft = circleNormalLeft;
                            }
                        }

                        glm::mat4 symMVP = buildCircleMVP(worldSymPt, circleSymNormalLeft, constRadiusLeft, cameraLeft, tiltX, tiltY);
                        m_state.symMVPs.push_back(symMVP);
                        worldSymPts.push_back(worldSymPt);
                    }
                }
            }

            bool recomputeOccLeft = !throttle || camChangedLeft || mouseMovedSignificantly || timeExpired ||
                                    (m_cachedSymOccluded.size() != m_state.symMVPs.size());
            if (recomputeOccLeft) {
                m_state.symOccluded.clear();
                m_state.symOccluded.reserve(worldSymPts.size());
                for (const auto& wpt : worldSymPts) {
                    m_occlusionQueryCount++;
                    bool occluded = checkOcclusion(wpt, cameraLeft, mesh);
                    m_state.symOccluded.push_back(occluded ? 1 : 0);
                }
                m_cachedSymOccluded = m_state.symOccluded;
            } else {
                m_state.symOccluded = m_cachedSymOccluded;
            }
        }

        // --- Right Viewport MVP construction ---
        m_state.symMVPsRight.clear();
        m_state.symOccludedRight.clear();
        if (cameraRight) {
            glm::vec3 circleNormalRight = worldNormal;
            if (isModal) {
                glm::vec3 camPosR = cameraRight->computePosition();
                glm::vec3 dirR = camPosR - worldPt;
                if (glm::length(dirR) > 1e-6f) {
                    circleNormalRight = glm::normalize(dirR);
                } else {
                    glm::mat4 invViewRight = glm::inverse(cameraRight->getViewMatrix());
                    circleNormalRight = glm::normalize(glm::vec3(invViewRight[2]));
                }
            }

            float worldRadiusRight = 0.0f;
            if (cameraRight->isOrthographic()) {
                worldRadiusRight = brushRadius * 2.0f * cameraRight->getOrthoZoom();
            } else {
                float fov_rad = cameraRight->getFovDegrees() * (float)M_PI / 180.0f;
                float screenHeight = (float)cameraRight->getHeight();
                if (screenHeight <= 0.0f) screenHeight = 1.0f;
                float hitDepth = glm::distance(cameraRight->computePosition(), worldPt);
                worldRadiusRight = brushRadius * hitDepth * std::tan(fov_rad * 0.5f) * 2.0f / screenHeight;
            }

            m_state.circleMVPRight = buildCircleMVP(worldPt, circleNormalRight, worldRadiusRight, *cameraRight, tiltX, tiltY);
            float innerWorldRadiusRight = worldRadiusRight * innerRatio;
            m_state.innerCircleMVPRight = buildCircleMVP(worldPt, circleNormalRight, innerWorldRadiusRight, *cameraRight, tiltX, tiltY);

            float constRadiusRight = 2.5f * (worldRadiusRight / brushRadius) * pressureDotFactor;
            m_state.dotMVPRight = buildCircleMVP(worldPt, circleNormalRight, constRadiusRight, *cameraRight, tiltX, tiltY);

            if (useSym && (symX || symY || symZ) && mesh) {
                glm::mat4 invMatrix = glm::inverse(mesh->matrix);
                glm::vec3 lPt = glm::vec3(invMatrix * glm::vec4(worldPt, 1.0f));
                glm::vec3 lNormal = glm::normalize(glm::mat3(invMatrix) * worldNormal);
                glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(mesh->matrix)));

                std::vector<float> xVals = { 1.0f };
                std::vector<float> yVals = { 1.0f };
                std::vector<float> zVals = { 1.0f };
                if (symX) xVals.push_back(-1.0f);
                if (symY) yVals.push_back(-1.0f);
                if (symZ) zVals.push_back(-1.0f);

                std::vector<glm::vec3> worldSymPtsRight;
                worldSymPtsRight.reserve(7);

                for (float sx : xVals) {
                    for (float sy : yVals) {
                        for (float sz : zVals) {
                            if (sx == 1.0f && sy == 1.0f && sz == 1.0f) continue;

                            glm::vec3 sScale(sx, sy, sz);
                            glm::vec3 localSymPt = reflectPointSymmetry(lPt, sScale, mesh, symMode);
                            glm::vec3 localSymNormal = reflectVectorSymmetry(lNormal, sScale, mesh, symMode);

                            glm::vec3 worldSymPt = glm::vec3(mesh->matrix * glm::vec4(localSymPt, 1.0f));
                            glm::vec3 worldSymNormal = glm::normalize(normalMatrix * localSymNormal);

                            glm::vec3 circleSymNormalRight = worldSymNormal;
                            if (isModal) {
                                glm::vec3 camPosR = cameraRight->computePosition();
                                glm::vec3 dirR = camPosR - worldSymPt;
                                if (glm::length(dirR) > 1e-6f) {
                                    circleSymNormalRight = glm::normalize(dirR);
                                } else {
                                    circleSymNormalRight = circleNormalRight;
                                }
                            }

                            glm::mat4 symMVP = buildCircleMVP(worldSymPt, circleSymNormalRight, constRadiusRight, *cameraRight, tiltX, tiltY);
                            m_state.symMVPsRight.push_back(symMVP);
                            worldSymPtsRight.push_back(worldSymPt);
                        }
                    }
                }

                bool recomputeOccRight = !throttle || camChangedRight || mouseMovedSignificantly || timeExpired ||
                                         (m_cachedSymOccludedRight.size() != m_state.symMVPsRight.size());
                if (recomputeOccRight) {
                    m_state.symOccludedRight.clear();
                    m_state.symOccludedRight.reserve(worldSymPtsRight.size());
                    for (const auto& wpt : worldSymPtsRight) {
                        m_occlusionQueryCount++;
                        bool occluded = checkOcclusion(wpt, *cameraRight, mesh);
                        m_state.symOccludedRight.push_back(occluded ? 1 : 0);
                    }
                    m_cachedSymOccludedRight = m_state.symOccludedRight;
                } else {
                    m_state.symOccludedRight = m_cachedSymOccludedRight;
                }
            }
        }

        if (throttle) {
            m_lastOccMouseX = mouseX;
            m_lastOccMouseY = mouseY;
            m_lastOccViewLeft = cameraLeft.getViewMatrix();
            m_lastOccViewRight = cameraRight ? cameraRight->getViewMatrix() : glm::mat4(0.0f);
            m_lastOccTicks = nowTicks;
        }
    } else {
        // Background Screenspace Mode
        float wLeft = cameraLeft.getWidth() * 0.5f;
        float hLeft = cameraLeft.getHeight() * 0.5f;
        glm::mat4 orthoProjLeft = glm::ortho(-wLeft, wLeft, -hLeft, hLeft, -10.0f, 10.0f);
        
        glm::mat4 transLeft = glm::mat4(1.0f);
        transLeft = glm::translate(transLeft, glm::vec3(-wLeft + (float)mouseX, hLeft - (float)mouseY, 0.0f));
        
        float backgroundDotSize = 2.5f;
#ifdef _WIN32
        if (g_tablet.isPressureCursorEnabled() && g_tablet.isPressureEnabled() && g_tablet.isAvailable() && g_tablet.isPenActive()) {
            backgroundDotSize = 2.5f * g_tablet.getPressure();
        }
#endif
        m_state.circleMVP = orthoProjLeft * glm::scale(transLeft, glm::vec3(brushRadius, brushRadius, 1.0f));
        m_state.innerCircleMVP = orthoProjLeft * glm::scale(transLeft, glm::vec3(brushRadius * innerRatio, brushRadius * innerRatio, 1.0f));
        m_state.dotMVP = orthoProjLeft * glm::scale(transLeft, glm::vec3(backgroundDotSize, backgroundDotSize, 1.0f));
        m_state.symMVPs.clear();
        m_state.symOccluded.clear();

        if (cameraRight) {
            float wRight = cameraRight->getWidth() * 0.5f;
            float hRight = cameraRight->getHeight() * 0.5f;
            glm::mat4 orthoProjRight = glm::ortho(-wRight, wRight, -hRight, hRight, -10.0f, 10.0f);
            
            glm::mat4 transRight = glm::mat4(1.0f);
            transRight = glm::translate(transRight, glm::vec3(-wRight + (float)mouseX, hRight - (float)mouseY, 0.0f));
            
            m_state.circleMVPRight = orthoProjRight * glm::scale(transRight, glm::vec3(brushRadius, brushRadius, 1.0f));
            m_state.innerCircleMVPRight = orthoProjRight * glm::scale(transRight, glm::vec3(brushRadius * innerRatio, brushRadius * innerRatio, 1.0f));
            m_state.dotMVPRight = orthoProjRight * glm::scale(transRight, glm::vec3(backgroundDotSize, backgroundDotSize, 1.0f));
            m_state.symMVPsRight.clear();
            m_state.symOccludedRight.clear();
        }
    }

    if (isSculpting) {
        // Dot always renders in screen-space at the latest mouse coordinates
        float wLeft = cameraLeft.getWidth()  * 0.5f;
        float hLeft = cameraLeft.getHeight() * 0.5f;
        glm::mat4 orthoProjLeft = glm::ortho(-wLeft, wLeft, -hLeft, hLeft, -10.0f, 10.0f);
        glm::mat4 transLeft = glm::translate(glm::mat4(1.0f),
            glm::vec3(-wLeft + (float)mouseX, hLeft - (float)mouseY, 0.0f));
        float sculptingDotSize = 3.5f;
#ifdef _WIN32
        if (g_tablet.isPressureCursorEnabled() && g_tablet.isPressureEnabled() && g_tablet.isAvailable() && g_tablet.isPenActive()) {
            sculptingDotSize = 3.5f * g_tablet.getPressure();
        }
#endif
        m_state.dotMVP = orthoProjLeft * glm::scale(transLeft, glm::vec3(sculptingDotSize, sculptingDotSize, 1.0f));

        if (cameraRight) {
            float wRight = cameraRight->getWidth()  * 0.5f;
            float hRight = cameraRight->getHeight() * 0.5f;
            glm::mat4 orthoProjRight = glm::ortho(-wRight, wRight, -hRight, hRight, -10.0f, 10.0f);
            glm::mat4 transRight = glm::translate(glm::mat4(1.0f),
                glm::vec3(-wRight + (float)mouseX, hRight - (float)mouseY, 0.0f));
            m_state.dotMVPRight = orthoProjRight * glm::scale(transRight, glm::vec3(sculptingDotSize, sculptingDotSize, 1.0f));
        }
    }
}

void BrushCursor::applyToRenderer(AngleRenderer& renderer) const {
    if (m_state.visible) {
        uintptr_t circlePtr = reinterpret_cast<uintptr_t>(glm::value_ptr(m_state.circleMVP));
        uintptr_t innerPtr = reinterpret_cast<uintptr_t>(glm::value_ptr(m_state.innerCircleMVP));
        uintptr_t dotPtr = reinterpret_cast<uintptr_t>(glm::value_ptr(m_state.dotMVP));
        uintptr_t symPtr = m_state.symMVPs.empty() ? 0 : reinterpret_cast<uintptr_t>(m_state.symMVPs.data());
        int symCount = static_cast<int>(m_state.symMVPs.size());
        uintptr_t colorPtr = reinterpret_cast<uintptr_t>(glm::value_ptr(m_state.color));
        uintptr_t occludedPtr = m_state.symOccluded.empty() ? 0 : reinterpret_cast<uintptr_t>(m_state.symOccluded.data());

        renderer.setCursorParametersFast(
            true,
            m_state.showCircle,
            circlePtr,
            innerPtr,
            dotPtr,
            symPtr,
            symCount,
            colorPtr,
            occludedPtr,
            m_state.isScreenspace
        );

        uintptr_t circlePtrR = reinterpret_cast<uintptr_t>(glm::value_ptr(m_state.circleMVPRight));
        uintptr_t innerPtrR = reinterpret_cast<uintptr_t>(glm::value_ptr(m_state.innerCircleMVPRight));
        uintptr_t dotPtrR = reinterpret_cast<uintptr_t>(glm::value_ptr(m_state.dotMVPRight));
        uintptr_t symPtrR = m_state.symMVPsRight.empty() ? 0 : reinterpret_cast<uintptr_t>(m_state.symMVPsRight.data());
        int symCountR = static_cast<int>(m_state.symMVPsRight.size());
        uintptr_t occludedPtrR = m_state.symOccludedRight.empty() ? 0 : reinterpret_cast<uintptr_t>(m_state.symOccludedRight.data());

        renderer.setCursorParametersRightFast(
            circlePtrR,
            innerPtrR,
            dotPtrR,
            symPtrR,
            symCountR,
            occludedPtrR
        );
    } else {
        renderer.setCursorParametersFast(false, false, 0, 0, 0, 0, 0, 0, 0, false);
        renderer.setCursorParametersRightFast(0, 0, 0, 0, 0, 0);
    }
}

glm::mat4 BrushCursor::buildCircleMVP(const glm::vec3& center,
                                      const glm::vec3& normal,
                                      float radius,
                                      const Camera& cam,
                                      float tiltX,
                                      float tiltY) const {
    if (glm::length(normal) < 1e-6f || std::isnan(normal.x) || std::isnan(normal.y) || std::isnan(normal.z) ||
        std::isnan(center.x) || std::isnan(center.y) || std::isnan(center.z)) {
        return glm::mat4(0.0f);
    }
    glm::vec3 n = glm::normalize(normal);
    if (glm::any(glm::isnan(n))) {
        return glm::mat4(0.0f);
    }
    glm::vec3 base(0.0f, 0.0f, 1.0f);
    float dot = glm::dot(base, n);
    float rad = 0.0f;
    glm::vec3 axis(0.0f, 0.0f, 1.0f);

    if (dot > 0.9999f) {
        rad = 0.0f;
        axis = glm::vec3(0.0f, 0.0f, 1.0f);
    } else if (dot < -0.9999f) {
        rad = (float)M_PI;
        axis = glm::vec3(1.0f, 0.0f, 0.0f);
    } else {
        rad = std::acos(dot);
        axis = glm::normalize(glm::cross(base, n));
    }

    glm::mat4 model = glm::mat4(1.0f);
    model = glm::translate(model, center);
    if (dot < 0.9999f) {
        model = glm::rotate(model, rad, axis);
    }

    float scaleX = 1.0f - std::abs(tiltX) / 90.0f * 0.5f;
    float scaleY = 1.0f - std::abs(tiltY) / 90.0f * 0.5f;
    scaleX = std::max(0.2f, std::min(1.0f, scaleX));
    scaleY = std::max(0.2f, std::min(1.0f, scaleY));

    model = glm::scale(model, glm::vec3(radius * scaleX, radius * scaleY, radius));

    return cam.getProjMatrix() * cam.getViewMatrix() * model;
}
