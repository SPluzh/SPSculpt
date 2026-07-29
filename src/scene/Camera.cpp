#include "scene/Camera.h"
#include "mesh/Mesh.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static glm::vec2 normalizedMouse(float mouseX, float mouseY, float width, float height) {
    if (width <= 0.0f || height <= 0.0f) return glm::vec2(0.0f);
    return glm::vec2(
        (mouseX - width * 0.5f) / (width * 0.5f),
        (height * 0.5f - mouseY) / (height * 0.5f)
    );
}

static float normalizeAngle(float target, float current) {
    float diff = target - current;
    diff = std::fmod(diff, 2.0f * M_PI);
    if (diff < -M_PI) diff += 2.0f * M_PI;
    if (diff > M_PI) diff -= 2.0f * M_PI;
    return current + diff;
}

Camera::Camera() {
    resetView();
}

#include "common/Logger.h"
void Camera::setProjectionType(CameraEnums::Projection projType) {
    if (m_projectionType == projType) return;

    float fovRad = getFovDegrees() * (float)M_PI / 180.0f;
    float tanHalfFov = std::tan(fovRad * 0.5f);
    float h = m_height > 0 ? (float)m_height : 1.0f;
    float oldTransZ = m_trans.z;

    if (projType == CameraEnums::Projection::ORTHOGRAPHIC) {
        // Perspective -> Ortho (Strictly Invertible)
        float eyeDist = getTransZ();
        float halfH = eyeDist * tanHalfFov;
        m_trans.z = halfH / (h * 0.00055f);
        m_offset.z = 0.0f;
        sculpt_log("[Camera] Perspective -> Ortho: oldTransZ=%.3f, eyeDist=%.3f, halfH=%.3f, newTransZ=%.3f, fov=%.1f, height=%.0f\n",
                    oldTransZ, eyeDist, halfH, m_trans.z, m_fov, h);
    } else {
        // Ortho -> Perspective (Strictly Invertible)
        float halfH = h * std::abs(m_trans.z) * 0.00055f;
        float eyeDist = halfH / tanHalfFov;
        m_trans.z = eyeDist * getFovDegrees() / 45.0f;
        sculpt_log("[Camera] Ortho -> Perspective: oldTransZ=%.3f, eyeDist=%.3f, halfH=%.3f, newTransZ=%.3f, fov=%.1f, height=%.0f\n",
                    oldTransZ, eyeDist, halfH, m_trans.z, m_fov, h);
    }
    m_trans.z = std::max(0.001f, m_trans.z);

    m_projectionType = projType;
    updateProjection();
    updateView();
}

void Camera::setMode(CameraEnums::CameraMode mode) {
    m_mode = mode;
    if (mode == CameraEnums::CameraMode::ORBIT) {
        setOrbit(0.0f, 0.0f);
    }
}

void Camera::setFov(float fov) {
    m_fov = std::max(1.0f, std::min(fov, 200.0f));
    updateView();
    updateProjection();
}

float Camera::getFovDegrees() const {
    return 2.0f * std::atan(12.0f / m_fov) * 180.0f / M_PI;
}

void Camera::start(float mouseX, float mouseY, bool cancelAnim) {
    if (cancelAnim) {
        cancelTransition();
    }
    m_lastNormalizedMouseXY = normalizedMouse(mouseX, mouseY, (float)m_width, (float)m_height);
    m_virtualNormalizedMouseXY = m_lastNormalizedMouseXY;
}

void Camera::setPivot(const glm::vec3& pivot) {
    glm::quat invQuat = glm::inverse(m_quatRot);
    m_offset = invQuat * m_offset;
    m_offset -= m_center;

    m_center = pivot;
    m_offset += m_center;
    m_offset = m_quatRot * m_offset;

    if (m_projectionType == CameraEnums::Projection::PERSPECTIVE) {
        float oldZoom = getTransZ();
        m_trans.z = glm::distance(computePosition(), m_center) * getFovDegrees() / 45.0f;
        m_offset.z += getTransZ() - oldZoom;
    } else {
        m_offset.z = 0.0f;
    }
}

void Camera::rotate(float mouseX, float mouseY, float speedRotate) {
    cancelTransition();
    glm::vec2 normalizedMouseXY = normalizedMouse(mouseX, mouseY, (float)m_width, (float)m_height);
    float speedFactor = (speedRotate / 0.25f) * m_speedRotate;

    if (m_mode == CameraEnums::CameraMode::ORBIT) {
        glm::vec2 diff = normalizedMouseXY - m_lastNormalizedMouseXY;
        setOrbit(m_rotX - diff.y * 2.0f * speedFactor, m_rotY + diff.x * 2.0f * speedFactor);
    } else if (m_mode == CameraEnums::CameraMode::PLANE) {
        glm::vec2 realDiff = normalizedMouseXY - m_lastNormalizedMouseXY;
        glm::vec2 scaledDiff = realDiff * speedFactor;
        float length = glm::length(scaledDiff);
        if (length > 0.0f) {
            glm::vec3 axisRot(-scaledDiff.y, scaledDiff.x, 0.0f);
            axisRot = glm::normalize(axisRot);
            glm::quat q = glm::angleAxis(length * 2.0f, axisRot);
            m_quatRot = q * m_quatRot;
        }
    } else if (m_mode == CameraEnums::CameraMode::SPHERICAL) {
        glm::vec2 realDiff = normalizedMouseXY - m_lastNormalizedMouseXY;
        glm::vec2 scaledDiff = realDiff * speedFactor;
        glm::vec2 nextVirtualMouseXY = m_virtualNormalizedMouseXY + scaledDiff;

        auto mouseOnUnitSphere = [](const glm::vec2& mouse) -> glm::vec3 {
            float len2 = mouse.x * mouse.x + mouse.y * mouse.y;
            if (len2 < 1.0f) {
                return glm::vec3(mouse.x, mouse.y, std::sqrt(1.0f - len2));
            } else {
                return glm::normalize(glm::vec3(mouse.x, mouse.y, 0.0f));
            }
        };

        glm::vec3 mouseOnSphereBefore = mouseOnUnitSphere(m_virtualNormalizedMouseXY);
        glm::vec3 mouseOnSphereAfter = mouseOnUnitSphere(nextVirtualMouseXY);
        float dot = glm::dot(mouseOnSphereBefore, mouseOnSphereAfter);
        float angle = std::acos(std::max(-1.0f, std::min(1.0f, dot)));
        if (angle > 1e-4f) {
            glm::vec3 axisRot = glm::normalize(glm::cross(mouseOnSphereBefore, mouseOnSphereAfter));
            glm::quat q = glm::angleAxis(angle * 2.0f, axisRot);
            m_quatRot = q * m_quatRot;
        }
        m_virtualNormalizedMouseXY = nextVirtualMouseXY;
    }
    m_lastNormalizedMouseXY = normalizedMouseXY;
    updateView();
}

void Camera::roll(float angle) {
    cancelTransition();
    glm::vec3 axis(0.0f, 0.0f, 1.0f);
    glm::quat q = glm::angleAxis(-angle, axis);
    m_quatRot = q * m_quatRot;
    updateView();
}

void Camera::setOrbit(float rx, float ry) {
    float radLimit = M_PI * 0.49f;
    m_rotX = std::max(-radLimit, std::min(radLimit, rx));
    m_rotY = ry;
    m_quatRot = glm::angleAxis(m_rotX, glm::vec3(1.0f, 0.0f, 0.0f)) * 
                glm::angleAxis(m_rotY, glm::vec3(0.0f, 1.0f, 0.0f));
}

void Camera::setOrbitAngles(float rx, float ry) {
    setOrbit(rx, ry);
    updateView();
}

float Camera::getTransZ() const {
    if (m_projectionType == CameraEnums::Projection::PERSPECTIVE) {
        return m_trans.z * 45.0f / getFovDegrees();
    } else {
        // Return world-space equivalent focal distance for ortho mode.
        float h = m_height > 0 ? (float)m_height : 1.0f;
        float fovRad = getFovDegrees() * (float)M_PI / 180.0f;
        float tanHalfFov = std::tan(fovRad * 0.5f);
        return std::abs(m_trans.z) * h * 0.00055f / tanHalfFov;
    }
}

void Camera::updateView() {
    glm::vec3 eye(m_trans.x - m_offset.x, m_trans.y - m_offset.y, getTransZ() - m_offset.z);
    glm::vec3 center(m_trans.x - m_offset.x, m_trans.y - m_offset.y, -m_offset.z);
    m_viewMatrix = glm::lookAt(eye, center, glm::vec3(0.0f, 1.0f, 0.0f));
    m_viewMatrix = m_viewMatrix * glm::mat4_cast(m_quatRot);
    m_viewMatrix = glm::translate(m_viewMatrix, -m_center);
}

void Camera::optimizeNearFar(const float* bbox) {
    if (!bbox) return;
    glm::vec3 eye = computePosition();
    glm::vec3 boxCenter(
        (bbox[0] + bbox[3]) * 0.5f,
        (bbox[1] + bbox[4]) * 0.5f,
        (bbox[2] + bbox[5]) * 0.5f
    );
    float distToBoxCenter = glm::distance(eye, boxCenter);
    glm::vec3 boxMin(bbox[0], bbox[1], bbox[2]);
    glm::vec3 boxMax(bbox[3], bbox[4], bbox[5]);
    float boxRadius = 0.5f * glm::distance(boxMin, boxMax);

    float margin = std::max({10.0f, boxRadius * 1.5f, distToBoxCenter * 0.15f});
    m_near = std::max(0.01f, distToBoxCenter - margin);
    m_far = distToBoxCenter + margin;
    updateProjection();
}

void Camera::updateProjection() {
    if (m_width <= 0 || m_height <= 0) return;
    if (m_projectionType == CameraEnums::Projection::PERSPECTIVE) {
        m_projMatrix = glm::perspective(getFovDegrees() * (float)M_PI / 180.0f, (float)m_width / m_height, m_near, m_far);
        m_projMatrix[2][2] = -1.0f;
        m_projMatrix[3][2] = -2.0f * m_near;
    } else {
        updateOrtho();
    }
}

void Camera::updateOrtho() {
    float delta = getOrthoZoom();
    float w = m_width * delta;
    float h = m_height * delta;
    m_projMatrix = glm::ortho(-w, w, -h, h, -m_near, m_far);
}

float Camera::getOrthoZoom() const {
    return std::abs(m_trans.z) * 0.00055f;
}

void Camera::translate(float dx, float dy) {
    cancelTransition();
    float w = m_width > 0 ? static_cast<float>(m_width) : 1.0f;
    float h = m_height > 0 ? static_cast<float>(m_height) : 1.0f;

    float factorX = 0.0f;
    float factorY = 0.0f;

    if (m_projectionType == CameraEnums::Projection::PERSPECTIVE) {
        float eyeDist = getTransZ();
        float proj00 = m_projMatrix[0][0] != 0.0f ? m_projMatrix[0][0] : 1.0f;
        float proj11 = m_projMatrix[1][1] != 0.0f ? m_projMatrix[1][1] : 1.0f;
        factorX = (2.0f / w) / proj00 * eyeDist * m_speedTranslate;
        factorY = (2.0f / h) / proj11 * eyeDist * m_speedTranslate;
    } else {
        float orthoZoom = getOrthoZoom();
        factorX = 2.0f * orthoZoom * m_speedTranslate;
        factorY = 2.0f * orthoZoom * m_speedTranslate;
    }

    glm::vec3 delta(-dx * factorX, dy * factorY, 0.0f);
    m_trans += delta;
    if (m_projectionType == CameraEnums::Projection::ORTHOGRAPHIC) {
        updateOrtho();
    }
    updateView();
}

void Camera::zoom(float df) {
    cancelTransition();
    float scaleFactor = std::pow(2.0f, -df * m_speedZoom);
    m_trans.z *= scaleFactor;
    m_trans.z = std::max(0.001f, std::min(m_trans.z, 100000.0f));

    if (m_projectionType == CameraEnums::Projection::ORTHOGRAPHIC) {
        updateOrtho();
    }
    updateView();
}

glm::vec3 Camera::computePosition() const {
    return glm::vec3(glm::inverse(m_viewMatrix)[3]);
}

void Camera::resetView() {
    float speed = 100.0f * 1.5f; // matches Utils.SCALE * 1.5
    
    CameraState targetState;
    targetState.center = glm::vec3(0.0f);
    targetState.offset = glm::vec3(0.0f);
    targetState.trans = glm::vec3(0.0f, 0.0f, 30.0f + speed / 3.0f);
    targetState.quatRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    targetState.rotX = 0.0f;
    targetState.rotY = 0.0f;
    targetState.fov = m_fov;
    targetState.projectionType = m_projectionType;
    targetState.mode = m_mode;
    targetState.usePivot = m_usePivot;
    targetState.view2DOffsetX = 0.0f;
    targetState.view2DOffsetY = 0.0f;
    targetState.view2DZoom = 1.0f;
    targetState.ref2DMode = m_ref2DMode;
    targetState.refDrag = m_refDrag;

    m_speed = speed;

    startTransition(targetState, 0.2f);
    pushState();
}

void Camera::resetViewToWorldPoints(const std::vector<glm::vec3>& worldPoints) {
    if (worldPoints.empty()) {
        resetView();
        return;
    }

    // 1. Compute World AABB of points
    glm::vec3 minW = worldPoints[0];
    glm::vec3 maxW = worldPoints[0];
    for (const auto& p : worldPoints) {
        minW = glm::min(minW, p);
        maxW = glm::max(maxW, p);
    }
    glm::vec3 centerWorld = (minW + maxW) * 0.5f;

    // 2. Transform points to camera view space relative to centerWorld
    glm::vec3 camMin(1e9f);
    glm::vec3 camMax(-1e9f);
    std::vector<glm::vec3> pCamList;
    pCamList.reserve(worldPoints.size());

    for (const auto& p : worldPoints) {
        glm::vec3 pCam = m_quatRot * (p - centerWorld);
        pCamList.push_back(pCam);
        camMin = glm::min(camMin, pCam);
        camMax = glm::max(camMax, pCam);
    }

    // 3. Align target center precisely with view-space center of bounding box
    glm::vec3 camCenter = (camMin + camMax) * 0.5f;
    glm::vec3 adjustedCenterWorld = centerWorld + glm::inverse(m_quatRot) * camCenter;

    // 4. Recalculate view space points relative to adjustedCenterWorld
    for (size_t i = 0; i < worldPoints.size(); ++i) {
        pCamList[i] = m_quatRot * (worldPoints[i] - adjustedCenterWorld);
    }

    float w = m_width > 0 ? static_cast<float>(m_width) : 1.0f;
    float h = m_height > 0 ? static_cast<float>(m_height) : 1.0f;

    float targetTransZ = 30.0f;

    if (m_projectionType == CameraEnums::Projection::PERSPECTIVE) {
        float fovY_deg = getFovDegrees();
        float theta_y = (fovY_deg * 0.5f) * (M_PI / 180.0f);
        float tan_y = std::tan(theta_y);
        float aspect = w / h;
        float tan_x = aspect * tan_y;

        float maxReqD = 0.001f;
        for (const auto& pCam : pCamList) {
            float reqD_y = std::abs(pCam.y) / tan_y + pCam.z;
            float reqD_x = std::abs(pCam.x) / tan_x + pCam.z;
            maxReqD = std::max({maxReqD, reqD_y, reqD_x});
        }

        // Apply a comfortable 15% safety margin factor (1.15)
        float eyeDist = std::max(1.0f, maxReqD * 1.15f);
        targetTransZ = eyeDist * (fovY_deg / 45.0f);
    } else {
        // Orthographic projection
        float maxReqTransZ = 0.001f;
        for (const auto& pCam : pCamList) {
            float reqZ_x = (std::abs(pCam.x) * 1.15f) / (w * 0.00055f);
            float reqZ_y = (std::abs(pCam.y) * 1.15f) / (h * 0.00055f);
            maxReqTransZ = std::max({maxReqTransZ, reqZ_x, reqZ_y});
        }
        targetTransZ = std::max(1.0f, maxReqTransZ);
    }

    CameraState targetState;
    targetState.center = adjustedCenterWorld;
    targetState.offset = glm::vec3(0.0f);
    targetState.trans = glm::vec3(0.0f, 0.0f, targetTransZ);
    targetState.quatRot = m_quatRot;
    targetState.rotX = m_rotX;
    targetState.rotY = m_rotY;
    targetState.fov = m_fov;
    targetState.projectionType = m_projectionType;
    targetState.mode = m_mode;
    targetState.usePivot = m_usePivot;
    targetState.view2DOffsetX = 0.0f;
    targetState.view2DOffsetY = 0.0f;
    targetState.view2DZoom = 1.0f;
    targetState.ref2DMode = m_ref2DMode;
    targetState.refDrag = m_refDrag;

    float radius = glm::distance(minW, maxW) * 0.5f;
    m_speed = std::max(10.0f, radius * 1.5f);

    startTransition(targetState, 0.2f);
    pushState();
}

void Camera::resetViewToMeshes(const std::vector<Mesh*>& meshes) {
    std::vector<glm::vec3> worldPoints;
    for (const Mesh* mesh : meshes) {
        if (!mesh || mesh->nbVerts == 0) continue;
        float localBbox[6];
        mesh->computeBbox(localBbox);

        glm::vec3 corners[8] = {
            {localBbox[0], localBbox[1], localBbox[2]},
            {localBbox[3], localBbox[1], localBbox[2]},
            {localBbox[0], localBbox[4], localBbox[2]},
            {localBbox[3], localBbox[4], localBbox[2]},
            {localBbox[0], localBbox[1], localBbox[5]},
            {localBbox[3], localBbox[1], localBbox[5]},
            {localBbox[0], localBbox[4], localBbox[5]},
            {localBbox[3], localBbox[4], localBbox[5]}
        };

        for (int i = 0; i < 8; ++i) {
            glm::vec3 wPos = glm::vec3(mesh->matrix * glm::vec4(corners[i], 1.0f));
            worldPoints.push_back(wPos);
        }
    }

    resetViewToWorldPoints(worldPoints);
}

void Camera::resetViewToMesh(const Mesh* mesh) {
    if (!mesh) {
        resetView();
        return;
    }
    resetViewToMeshes({const_cast<Mesh*>(mesh)});
}

void Camera::resetViewToMesh(const float* bbox) {
    if (!bbox) {
        resetView();
        return;
    }
    std::vector<glm::vec3> worldPoints = {
        {bbox[0], bbox[1], bbox[2]},
        {bbox[3], bbox[1], bbox[2]},
        {bbox[0], bbox[4], bbox[2]},
        {bbox[3], bbox[4], bbox[2]},
        {bbox[0], bbox[1], bbox[5]},
        {bbox[3], bbox[1], bbox[5]},
        {bbox[0], bbox[4], bbox[5]},
        {bbox[3], bbox[4], bbox[5]}
    };

    resetViewToWorldPoints(worldPoints);
}

float Camera::computeFrustumFit() const {
    float nearVal = m_near;
    float x;

    if (m_projectionType == CameraEnums::Projection::ORTHOGRAPHIC) {
        x = std::min(m_width, m_height) / nearVal * 0.5f;
        return std::sqrt(1.0f + x * x) / x;
    }

    float proj0 = m_projMatrix[0][0];
    float proj5 = m_projMatrix[1][1];
    float proj8 = m_projMatrix[2][0];
    float proj9 = m_projMatrix[2][1];

    float left = nearVal * (proj8 - 1.0f) / proj0;
    float right = nearVal * (1.0f + proj8) / proj0;
    float top = nearVal * (1.0f + proj9) / proj5;
    float bottom = nearVal * (proj9 - 1.0f) / proj5;
    float vertical2 = std::abs(right - left);
    float horizontal2 = std::abs(top - bottom);

    x = std::min(horizontal2, vertical2) / nearVal * 0.5f;
    if (x < 1e-4f) return 1.0f;
    return (getFovDegrees() / 45.0f) * std::sqrt(1.0f + x * x) / x;
}

Ray Camera::getRay(float mouseX, float mouseY) const {
    if (m_ref2DMode) {
        float w = m_width > 0 ? (float)m_width : 1.0f;
        float h = m_height > 0 ? (float)m_height : 1.0f;
        float ndcX = (mouseX / w) * 2.0f - 1.0f;
        float ndcY = 1.0f - (mouseY / h) * 2.0f;
        float ndcXReal = (ndcX - m_view2DOffsetX) / m_view2DZoom;
        float ndcYReal = (ndcY - m_view2DOffsetY) / m_view2DZoom;
        mouseX = (ndcXReal + 1.0f) * 0.5f * w;
        mouseY = (1.0f - ndcYReal) * 0.5f * h;
    }
    float w = m_width > 0 ? (float)m_width : 1.0f;
    float h = m_height > 0 ? (float)m_height : 1.0f;
    float ndcX = (mouseX / w) * 2.0f - 1.0f;
    float ndcY = 1.0f - (mouseY / h) * 2.0f;

    Ray r;
    r.origin = glm::vec3(0.0f);
    r.dir = glm::vec3(0.0f);

    if (m_projectionType == CameraEnums::Projection::PERSPECTIVE) {
        r.origin = computePosition();
        glm::vec3 eyeDir(
            m_projMatrix[0][0] != 0.0f ? ndcX / m_projMatrix[0][0] : 0.0f,
            m_projMatrix[1][1] != 0.0f ? ndcY / m_projMatrix[1][1] : 0.0f,
            -1.0f
        );
        glm::mat3 rot = glm::transpose(glm::mat3(m_viewMatrix));
        r.dir = glm::normalize(rot * eyeDir);
    } else {
        glm::mat4 invW2S = glm::inverse(computeWorldToScreenMatrix());
        glm::vec4 vNear = invW2S * glm::vec4(mouseX, h - mouseY, 0.0f, 1.0f);
        glm::vec4 vFar = invW2S * glm::vec4(mouseX, h - mouseY, 1.0f, 1.0f);
        r.origin = glm::vec3(vNear) / vNear.w;
        glm::vec3 farPos = glm::vec3(vFar) / vFar.w;
        r.dir = glm::normalize(farPos - r.origin);
    }
    return r;
}

glm::vec3 Camera::unproject(float mouseX, float mouseY, float z) const {
    if (m_ref2DMode) {
        float w = m_width > 0 ? (float)m_width : 1.0f;
        float h = m_height > 0 ? (float)m_height : 1.0f;
        float ndcX = (mouseX / w) * 2.0f - 1.0f;
        float ndcY = 1.0f - (mouseY / h) * 2.0f;
        float ndcXReal = (ndcX - m_view2DOffsetX) / m_view2DZoom;
        float ndcYReal = (ndcY - m_view2DOffsetY) / m_view2DZoom;
        mouseX = (ndcXReal + 1.0f) * 0.5f * w;
        mouseY = (1.0f - ndcYReal) * 0.5f * h;
    }
    float h = m_height > 0 ? (float)m_height : 1.0f;
    glm::mat4 invW2S = glm::inverse(computeWorldToScreenMatrix());
    glm::vec4 screenPos(mouseX, h - mouseY, z, 1.0f);
    glm::vec4 worldPos = invW2S * screenPos;
    return glm::vec3(worldPos) / worldPos.w;
}

glm::vec3 Camera::project(const glm::vec3& worldPos) const {
    glm::vec4 screen = computeWorldToScreenMatrix() * glm::vec4(worldPos, 1.0f);
    glm::vec3 proj = glm::vec3(screen) / screen.w;
    proj.y = (float)m_height - proj.y;

    if (m_ref2DMode) {
        float w = m_width > 0 ? (float)m_width : 1.0f;
        float h = m_height > 0 ? (float)m_height : 1.0f;
        float ndcX = (proj.x / w) * 2.0f - 1.0f;
        float ndcY = 1.0f - (proj.y / h) * 2.0f;
        float ndcXS = ndcX * m_view2DZoom + m_view2DOffsetX;
        float ndcYS = ndcY * m_view2DZoom + m_view2DOffsetY;
        proj.x = (ndcXS + 1.0f) * 0.5f * w;
        proj.y = (1.0f - ndcYS) * 0.5f * h;
    }
    return proj;
}

glm::mat4 Camera::computeWorldToScreenMatrix() const {
    return m_viewportMatrix * m_projMatrix * m_viewMatrix;
}

void Camera::onResize(int width, int height) {
    m_width = width;
    m_height = height;
    m_viewportMatrix = glm::mat4(1.0f);
    m_viewportMatrix = glm::scale(m_viewportMatrix, glm::vec3(0.5f * width, 0.5f * height, 0.5f));
    m_viewportMatrix = glm::translate(m_viewportMatrix, glm::vec3(1.0f, 1.0f, 1.0f));
    updateProjection();
}

static const float SQ = 0.7071067811865476f;
static const float D = 0.5f;
static const glm::quat QUAT_COMP[24] = {
    glm::quat(0.0f, 1.0f, 0.0f, 0.0f),
    glm::quat(0.0f, 0.0f, 1.0f, 0.0f),
    glm::quat(0.0f, 0.0f, 0.0f, 1.0f),
    glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
    glm::quat(0.0f, SQ, SQ, 0.0f),
    glm::quat(0.0f, SQ, -SQ, 0.0f),
    glm::quat(0.0f, SQ, 0.0f, SQ),
    glm::quat(0.0f, SQ, 0.0f, -SQ),
    glm::quat(SQ, SQ, 0.0f, 0.0f),
    glm::quat(-SQ, SQ, 0.0f, 0.0f),
    glm::quat(0.0f, 0.0f, SQ, SQ),
    glm::quat(0.0f, 0.0f, SQ, -SQ),
    glm::quat(SQ, 0.0f, SQ, 0.0f),
    glm::quat(-SQ, 0.0f, SQ, 0.0f),
    glm::quat(SQ, 0.0f, 0.0f, SQ),
    glm::quat(-SQ, 0.0f, 0.0f, SQ),
    glm::quat(D, D, D, D),
    glm::quat(-D, D, D, D),
    glm::quat(D, D, D, -D),
    glm::quat(-D, D, D, -D),
    glm::quat(D, D, -D, D),
    glm::quat(-D, D, -D, D),
    glm::quat(D, D, -D, -D),
    glm::quat(D, -D, D, D)
};

void Camera::snapClosestRotation() {
    float minVal = 1e9f;
    int bestId = 0;
    for (int i = 0; i < 24; ++i) {
        float dVal = glm::dot(m_quatRot, QUAT_COMP[i]);
        float diff = 1.0f - dVal * dVal;
        if (diff < minVal) {
            minVal = diff;
            bestId = i;
        }
    }
    glm::quat targetQuat = QUAT_COMP[bestId];
    
    float targetRotX = m_rotX;
    float targetRotY = m_rotY;
    if (m_mode == CameraEnums::CameraMode::ORBIT) {
        float qx = targetQuat.x;
        float qy = targetQuat.y;
        float qz = targetQuat.z;
        float qw = targetQuat.w;
        float rawRotY = std::atan2(2.0f * (qw * qy + qz * qx), 1.0f - 2.0f * (qy * qy + qz * qz));
        targetRotY = normalizeAngle(rawRotY, m_rotY);
        targetRotX = std::max<float>(-M_PI * 0.49f, std::min<float>(M_PI * 0.49f, std::atan2(2.0f * (qw * qx + qy * qz), 1.0f - 2.0f * (qz * qz + qx * qx))));
    }

    if (glm::dot(m_quatRot, targetQuat) < 0.0f) {
        targetQuat = -targetQuat;
    }

    CameraState targetState;
    targetState.center = m_center;
    targetState.offset = m_offset;
    targetState.trans = m_trans;
    targetState.quatRot = targetQuat;
    targetState.rotX = targetRotX;
    targetState.rotY = targetRotY;
    targetState.fov = m_fov;
    targetState.projectionType = m_projectionType;
    targetState.mode = m_mode;
    targetState.usePivot = m_usePivot;
    targetState.view2DOffsetX = m_view2DOffsetX;
    targetState.view2DOffsetY = m_view2DOffsetY;
    targetState.view2DZoom = m_view2DZoom;
    targetState.ref2DMode = m_ref2DMode;
    targetState.refDrag = m_refDrag;

    startTransition(targetState, 0.2f);
}

void Camera::pushState() {
    CameraState state{
        m_quatRot,
        m_trans,
        m_center,
        m_offset,
        m_rotX,
        m_rotY,
        m_fov,
        m_projectionType,
        m_mode,
        m_usePivot,
        m_view2DOffsetX,
        m_view2DOffsetY,
        m_view2DZoom,
        m_ref2DMode,
        m_refDrag
    };

    if (m_historyIndex >= 0) {
        const auto& prev = m_history[m_historyIndex];
        bool isSame = prev.mode == state.mode &&
                     prev.projectionType == state.projectionType &&
                     std::abs(prev.fov - state.fov) < 1e-5f &&
                     prev.usePivot == state.usePivot &&
                     std::abs(prev.rotX - state.rotX) < 1e-5f &&
                     std::abs(prev.rotY - state.rotY) < 1e-5f &&
                     glm::distance(prev.trans, state.trans) < 1e-5f &&
                     glm::distance(prev.center, state.center) < 1e-5f &&
                     glm::distance(prev.offset, state.offset) < 1e-5f &&
                     std::abs(glm::dot(prev.quatRot, state.quatRot)) > 1.0f - 1e-5f &&
                     std::abs(prev.view2DOffsetX - state.view2DOffsetX) < 1e-5f &&
                     std::abs(prev.view2DOffsetY - state.view2DOffsetY) < 1e-5f &&
                     std::abs(prev.view2DZoom - state.view2DZoom) < 1e-5f &&
                     prev.ref2DMode == state.ref2DMode &&
                     prev.refDrag == state.refDrag;
        if (isSame) return;
    }

    m_history.resize(m_historyIndex + 1);
    m_history.push_back(state);
    m_historyIndex++;
    if (m_history.size() > 100) {
        m_history.erase(m_history.begin());
        m_historyIndex--;
    }
}

void Camera::undo() {
    if (m_historyIndex <= 0) return;
    m_historyIndex--;
    applyState(m_history[m_historyIndex]);
}

void Camera::redo() {
    if (m_historyIndex >= (int)m_history.size() - 1) return;
    m_historyIndex++;
    applyState(m_history[m_historyIndex]);
}

void Camera::applyState(const CameraState& state) {
    m_mode = state.mode;
    m_projectionType = state.projectionType;
    m_quatRot = state.quatRot;
    m_trans = state.trans;
    m_center = state.center;
    m_offset = state.offset;
    m_rotX = state.rotX;
    m_rotY = state.rotY;
    m_fov = state.fov;
    m_usePivot = state.usePivot;
    m_view2DOffsetX = state.view2DOffsetX;
    m_view2DOffsetY = state.view2DOffsetY;
    m_view2DZoom = state.view2DZoom;
    m_ref2DMode = state.ref2DMode;
    m_refDrag = state.refDrag;
    updateView();
    updateProjection();
}

void Camera::toggleViewFront() {
    CameraState targetState;
    targetState.center = m_center;
    targetState.offset = m_offset;
    targetState.trans = m_trans;
    targetState.quatRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    targetState.rotX = 0.0f;
    targetState.rotY = normalizeAngle(0.0f, m_rotY);
    targetState.fov = m_fov;
    targetState.projectionType = m_projectionType;
    targetState.mode = m_mode;
    targetState.usePivot = m_usePivot;
    targetState.view2DOffsetX = m_view2DOffsetX;
    targetState.view2DOffsetY = m_view2DOffsetY;
    targetState.view2DZoom = m_view2DZoom;
    targetState.ref2DMode = m_ref2DMode;
    targetState.refDrag = m_refDrag;

    if (glm::dot(m_quatRot, targetState.quatRot) < 0.0f) {
        targetState.quatRot = -targetState.quatRot;
    }

    startTransition(targetState, 0.2f);
    pushState();
}

void Camera::toggleViewTop() {
    float SQ = 0.70710678f;
    CameraState targetState;
    targetState.center = m_center;
    targetState.offset = m_offset;
    targetState.trans = m_trans;
    targetState.quatRot = glm::quat(SQ, -SQ, 0.0f, 0.0f);
    targetState.rotX = -1.570796f; // -PI/2
    targetState.rotY = normalizeAngle(0.0f, m_rotY);
    targetState.fov = m_fov;
    targetState.projectionType = m_projectionType;
    targetState.mode = m_mode;
    targetState.usePivot = m_usePivot;
    targetState.view2DOffsetX = m_view2DOffsetX;
    targetState.view2DOffsetY = m_view2DOffsetY;
    targetState.view2DZoom = m_view2DZoom;
    targetState.ref2DMode = m_ref2DMode;
    targetState.refDrag = m_refDrag;

    if (glm::dot(m_quatRot, targetState.quatRot) < 0.0f) {
        targetState.quatRot = -targetState.quatRot;
    }

    startTransition(targetState, 0.2f);
    pushState();
}

void Camera::toggleViewLeft() {
    float SQ = 0.70710678f;
    CameraState targetState;
    targetState.center = m_center;
    targetState.offset = m_offset;
    targetState.trans = m_trans;
    targetState.quatRot = glm::quat(SQ, 0.0f, SQ, 0.0f);
    targetState.rotX = 0.0f;
    targetState.rotY = normalizeAngle(1.570796f, m_rotY); // PI/2
    targetState.fov = m_fov;
    targetState.projectionType = m_projectionType;
    targetState.mode = m_mode;
    targetState.usePivot = m_usePivot;
    targetState.view2DOffsetX = m_view2DOffsetX;
    targetState.view2DOffsetY = m_view2DOffsetY;
    targetState.view2DZoom = m_view2DZoom;
    targetState.ref2DMode = m_ref2DMode;
    targetState.refDrag = m_refDrag;

    if (glm::dot(m_quatRot, targetState.quatRot) < 0.0f) {
        targetState.quatRot = -targetState.quatRot;
    }

    startTransition(targetState, 0.2f);
    pushState();
}

void Camera::toggleViewRight() {
    float SQ = 0.70710678f;
    CameraState targetState;
    targetState.center = m_center;
    targetState.offset = m_offset;
    targetState.trans = m_trans;
    targetState.quatRot = glm::quat(SQ, 0.0f, -SQ, 0.0f);
    targetState.rotX = 0.0f;
    targetState.rotY = normalizeAngle(-1.570796f, m_rotY); // -PI/2
    targetState.fov = m_fov;
    targetState.projectionType = m_projectionType;
    targetState.mode = m_mode;
    targetState.usePivot = m_usePivot;
    targetState.view2DOffsetX = m_view2DOffsetX;
    targetState.view2DOffsetY = m_view2DOffsetY;
    targetState.view2DZoom = m_view2DZoom;
    targetState.ref2DMode = m_ref2DMode;
    targetState.refDrag = m_refDrag;

    if (glm::dot(m_quatRot, targetState.quatRot) < 0.0f) {
        targetState.quatRot = -targetState.quatRot;
    }

    startTransition(targetState, 0.2f);
    pushState();
}

void Camera::toggleViewAngles(float rx, float ry) {
    CameraState targetState;
    targetState.center = m_center;
    targetState.offset = m_offset;
    targetState.trans = m_trans;

    // If currently in perspective, convert trans.z so the object stays the same
    // apparent size after switching to orthographic.
    if (m_projectionType == CameraEnums::Projection::PERSPECTIVE) {
        float fovRad = getFovDegrees() * (float)M_PI / 180.0f;
        float tanHalfFov = std::tan(fovRad * 0.5f);
        float h = m_height > 0 ? (float)m_height : 1.0f;
        float eyeDist = getTransZ();
        float halfH = eyeDist * tanHalfFov;
        targetState.trans.z = std::max(0.001f, halfH / (h * 0.00055f));
        targetState.offset.z = 0.0f;
    }

    targetState.rotX = rx;
    targetState.rotY = normalizeAngle(ry, m_rotY);
    targetState.quatRot = glm::angleAxis(targetState.rotX, glm::vec3(1.0f, 0.0f, 0.0f)) *
                          glm::angleAxis(targetState.rotY, glm::vec3(0.0f, 1.0f, 0.0f));
    targetState.fov = m_fov;
    targetState.projectionType = CameraEnums::Projection::ORTHOGRAPHIC;
    targetState.mode = m_mode;
    targetState.usePivot = m_usePivot;
    targetState.view2DOffsetX = m_view2DOffsetX;
    targetState.view2DOffsetY = m_view2DOffsetY;
    targetState.view2DZoom = m_view2DZoom;
    targetState.ref2DMode = m_ref2DMode;
    targetState.refDrag = m_refDrag;

    if (glm::dot(m_quatRot, targetState.quatRot) < 0.0f) {
        targetState.quatRot = -targetState.quatRot;
    }

    startTransition(targetState, 0.2f);
    pushState();
}


void Camera::update(float deltaTime) {
    if (m_transitionActive) {
        m_transitionTime += deltaTime;
        float t = m_transitionTime / m_transitionDuration;
        if (t >= 1.0f) {
            m_trans = m_targetState.trans;
            m_center = m_targetState.center;
            m_offset = m_targetState.offset;
            m_quatRot = m_targetState.quatRot;
            m_rotX = m_targetState.rotX;
            m_rotY = m_targetState.rotY;
            m_fov = m_targetState.fov;
            m_projectionType = m_targetState.projectionType;
            m_mode = m_targetState.mode;
            m_usePivot = m_targetState.usePivot;
            m_view2DOffsetX = m_targetState.view2DOffsetX;
            m_view2DOffsetY = m_targetState.view2DOffsetY;
            m_view2DZoom = m_targetState.view2DZoom;
            m_ref2DMode = m_targetState.ref2DMode;
            m_refDrag = m_targetState.refDrag;
            m_transitionActive = false;
        } else {
            // Easing function: easeOutQuart
            float r = t - 1.0f;
            float tEscaped = -(r * r * r * r - 1.0f);

            m_trans = glm::mix(m_startState.trans, m_targetState.trans, tEscaped);
            m_center = glm::mix(m_startState.center, m_targetState.center, tEscaped);
            m_offset = glm::mix(m_startState.offset, m_targetState.offset, tEscaped);
            m_quatRot = glm::slerp(m_startState.quatRot, m_targetState.quatRot, tEscaped);
            m_rotX = glm::mix(m_startState.rotX, m_targetState.rotX, tEscaped);
            m_rotY = glm::mix(m_startState.rotY, m_targetState.rotY, tEscaped);
            m_fov = glm::mix(m_startState.fov, m_targetState.fov, tEscaped);
            m_view2DOffsetX = glm::mix(m_startState.view2DOffsetX, m_targetState.view2DOffsetX, tEscaped);
            m_view2DOffsetY = glm::mix(m_startState.view2DOffsetY, m_targetState.view2DOffsetY, tEscaped);
            m_view2DZoom = glm::mix(m_startState.view2DZoom, m_targetState.view2DZoom, tEscaped);
            m_ref2DMode = m_targetState.ref2DMode;
            m_refDrag = m_targetState.refDrag;
        }
        updateView();
        updateProjection();
    }
}

void Camera::startTransition(const CameraState& targetState, float duration) {
    m_startState = CameraState{
        m_quatRot,
        m_trans,
        m_center,
        m_offset,
        m_rotX,
        m_rotY,
        m_fov,
        m_projectionType,
        m_mode,
        m_usePivot,
        m_view2DOffsetX,
        m_view2DOffsetY,
        m_view2DZoom,
        m_ref2DMode,
        m_refDrag
    };
    m_targetState = targetState;
    m_projectionType = targetState.projectionType;
    m_mode = targetState.mode;
    m_usePivot = targetState.usePivot;
    m_ref2DMode = targetState.ref2DMode;
    m_refDrag = targetState.refDrag;

    m_transitionDuration = duration;
    m_transitionTime = 0.0f;
    m_transitionActive = true;
}

void Camera::cancelTransition() {
    m_transitionActive = false;
}

