#include "editing/BrushCursor.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "render/AngleRenderer.h"
#include "mesh/Mesh.h"
#ifdef _WIN32
#include "platform/TabletInput.h"
#endif
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

    std::vector<uint32_t> candidateFaces = mesh->octree.collectIntersectRay(
        localRayOrigin.x, localRayOrigin.y, localRayOrigin.z,
        localRayDir.x, localRayDir.y, localRayDir.z
    );

    float minT = std::numeric_limits<float>::infinity();

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
            }
        }

        if (v3Id != 0xffffffff) {
            glm::vec3 v3(mesh->verts[v3Id * 3], mesh->verts[v3Id * 3 + 1], mesh->verts[v3Id * 3 + 2]);
            if (rayTriangleIntersect(localRayOrigin, localRayDir, v0, v2, v3, t)) {
                if (t < minT) {
                    minT = t;
                }
            }
        }
    }

    if (minT < targetLocalT * 0.995f) {
        return true;
    }
    return false;
}


BrushCursor::BrushCursor() {
    m_state.visible = false;
}

void BrushCursor::update(int mouseX, int mouseY,
                          const Scene& scene,
                          float brushRadius,
                          bool useSym,
                          int symAxis,
                          bool isSculpting,
                          BrushType brushType,
                          bool hasActiveStrokeHit,
                          const glm::vec3& activeStrokeHitPt,
                          const glm::vec3& activeStrokeHitNormal,
                          float focalShift,
                          float hardness) {
    if (brushType == BRUSH_VISIBILITY || brushType == BRUSH_MASK_GRADIENT_BLUR) {
        m_state.visible = false;
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
    if (mesh && !mesh->isVisible(activeViewport)) {
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
            hitMesh = true;
            localPt = localRayOrigin + minT * localRayDir;
            localNormal = glm::vec3(
                mesh->faceNormals[intersectFaceId * 3],
                mesh->faceNormals[intersectFaceId * 3 + 1],
                mesh->faceNormals[intersectFaceId * 3 + 2]
            );

            worldPt = glm::vec3(mesh->matrix * glm::vec4(localPt, 1.0f));
            glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(mesh->matrix)));
            worldNormal = glm::normalize(normalMatrix * localNormal);
        }
    }

    m_state.visible = true;
    m_state.showCircle = !isSculpting;

    // Color Setup based on active brush & hover status
    bool drawCircle = !isSculpting;
    if (brushType == BRUSH_SMOOTH) {
        if (drawCircle && hitMesh) {
            m_state.color = glm::vec3(0.0f, 0.4f, 0.8f);
        } else {
            m_state.color = glm::vec3(0.0f, 0.6f, 1.0f);
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
        if (g_tablet.isAvailable() && g_tablet.isPenActive() && g_tablet.isTiltEnabled()) {
            tiltX = g_tablet.getTiltX();
            tiltY = g_tablet.getTiltY();
        }
#endif

        m_state.circleMVP = buildCircleMVP(worldPt, worldNormal, worldRadiusLeft, cameraLeft, tiltX, tiltY);
        float innerWorldRadiusLeft = worldRadiusLeft * innerRatio;
        m_state.innerCircleMVP = buildCircleMVP(worldPt, worldNormal, innerWorldRadiusLeft, cameraLeft, tiltX, tiltY);

        float pressureDotFactor = 1.0f;
#ifdef _WIN32
        if (g_tablet.isPressureCursorEnabled() && g_tablet.isPressureEnabled() && g_tablet.isAvailable() && g_tablet.isPenActive()) {
            pressureDotFactor = g_tablet.getPressure();
        }
#endif
        float constRadiusLeft = 2.5f * (worldRadiusLeft / brushRadius) * pressureDotFactor;
        m_state.dotMVP = buildCircleMVP(worldPt, worldNormal, constRadiusLeft, cameraLeft, tiltX, tiltY);

        m_state.symMVPs.clear();
        m_state.symOccluded.clear();
        if (useSym && mesh) {
            glm::mat4 invMatrix = glm::inverse(mesh->matrix);
            glm::vec3 lPt = glm::vec3(invMatrix * glm::vec4(worldPt, 1.0f));
            glm::vec3 lNormal = glm::normalize(glm::vec3(glm::transpose(mesh->matrix) * glm::vec4(worldNormal, 0.0f)));

            glm::vec3 localSymPt = lPt;
            glm::vec3 localSymNormal = lNormal;
            if (symAxis == 0) { // X
                localSymPt.x = -localSymPt.x;
                localSymNormal.x = -localSymNormal.x;
            } else if (symAxis == 1) { // Y
                localSymPt.y = -localSymPt.y;
                localSymNormal.y = -localSymNormal.y;
            } else if (symAxis == 2) { // Z
                localSymPt.z = -localSymPt.z;
                localSymNormal.z = -localSymNormal.z;
            }

            glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(mesh->matrix)));
            glm::vec3 worldSymPt = glm::vec3(mesh->matrix * glm::vec4(localSymPt, 1.0f));
            glm::vec3 worldSymNormal = glm::normalize(normalMatrix * localSymNormal);

            glm::mat4 symMVP = buildCircleMVP(worldSymPt, worldSymNormal, constRadiusLeft, cameraLeft, tiltX, tiltY);
            m_state.symMVPs.push_back(symMVP);
            
            bool occluded = checkOcclusion(worldSymPt, cameraLeft, mesh);
            m_state.symOccluded.push_back(occluded ? 1 : 0);
        }

        // --- Right Viewport MVP construction ---
        m_state.symMVPsRight.clear();
        m_state.symOccludedRight.clear();
        if (cameraRight) {
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

            m_state.circleMVPRight = buildCircleMVP(worldPt, worldNormal, worldRadiusRight, *cameraRight, tiltX, tiltY);
            float innerWorldRadiusRight = worldRadiusRight * innerRatio;
            m_state.innerCircleMVPRight = buildCircleMVP(worldPt, worldNormal, innerWorldRadiusRight, *cameraRight, tiltX, tiltY);

            float constRadiusRight = 2.5f * (worldRadiusRight / brushRadius) * pressureDotFactor;
            m_state.dotMVPRight = buildCircleMVP(worldPt, worldNormal, constRadiusRight, *cameraRight, tiltX, tiltY);

            if (useSym && mesh) {
                glm::mat4 invMatrix = glm::inverse(mesh->matrix);
                glm::vec3 lPt = glm::vec3(invMatrix * glm::vec4(worldPt, 1.0f));
                glm::vec3 lNormal = glm::normalize(glm::vec3(glm::transpose(mesh->matrix) * glm::vec4(worldNormal, 0.0f)));

                glm::vec3 localSymPt = lPt;
                glm::vec3 localSymNormal = lNormal;
                if (symAxis == 0) { // X
                    localSymPt.x = -localSymPt.x;
                    localSymNormal.x = -localSymNormal.x;
                } else if (symAxis == 1) { // Y
                    localSymPt.y = -localSymPt.y;
                    localSymNormal.y = -localSymNormal.y;
                } else if (symAxis == 2) { // Z
                    localSymPt.z = -localSymPt.z;
                    localSymNormal.z = -localSymNormal.z;
                }

                glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(mesh->matrix)));
                glm::vec3 worldSymPt = glm::vec3(mesh->matrix * glm::vec4(localSymPt, 1.0f));
                glm::vec3 worldSymNormal = glm::normalize(normalMatrix * localSymNormal);

                glm::mat4 symMVP = buildCircleMVP(worldSymPt, worldSymNormal, constRadiusRight, *cameraRight, tiltX, tiltY);
                m_state.symMVPsRight.push_back(symMVP);
                
                bool occluded = checkOcclusion(worldSymPt, *cameraRight, mesh);
                m_state.symOccludedRight.push_back(occluded ? 1 : 0);
            }
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
    glm::vec3 n = glm::normalize(normal);
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
