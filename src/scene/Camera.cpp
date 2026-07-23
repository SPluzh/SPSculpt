#include "scene/Camera.h"
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

Camera::Camera() {
    resetView();
}

void Camera::setProjectionType(CameraEnums::Projection projType) {
    if (m_projectionType == projType) return;
    
    // Smooth transition matching JS Camera.js setProjectionType
    float tanHalfFov = std::tan(getFovDegrees() * M_PI / 360.0f);
    float h = m_height > 0 ? (float)m_height : 1.0f;
    
    if (projType == CameraEnums::Projection::ORTHOGRAPHIC) {
        float eyeDist = getTransZ();
        m_trans.z = eyeDist * tanHalfFov / (h * 0.00055f);
        m_offset.z = 0.0f;
    } else {
        float halfH = h * std::abs(m_trans.z) * 0.00055f;
        float eyeDist = halfH / tanHalfFov;
        m_trans.z = eyeDist * getFovDegrees() / 45.0f;
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

void Camera::start(float mouseX, float mouseY) {
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
    glm::vec2 normalizedMouseXY = normalizedMouse(mouseX, mouseY, (float)m_width, (float)m_height);
    float speedFactor = (speedRotate / 0.25f) * m_speedRotate;

    if (m_mode == CameraEnums::CameraMode::ORBIT) {
        glm::vec2 diff = normalizedMouseXY - m_lastNormalizedMouseXY;
        setOrbit(m_rotX - diff.y * 2.0f * speedFactor, m_rotY + diff.x * 2.0f * speedFactor);
    } else if (m_mode == CameraEnums::CameraMode::PLANE) {
        glm::vec2 realDiff = normalizedMouseXY - m_lastNormalizedMouseXY;
        glm::vec2 scaledDiff = realDiff * speedFactor;
        glm::vec2 nextVirtualMouseXY = m_virtualNormalizedMouseXY + scaledDiff;
        float len = glm::length(nextVirtualMouseXY);
        if (len > 1.0f) {
            nextVirtualMouseXY /= len;
        }

        float length = glm::distance(m_virtualNormalizedMouseXY, nextVirtualMouseXY);
        if (length > 0.0f) {
            glm::vec3 axisRot(-scaledDiff.y, scaledDiff.x, 0.0f);
            axisRot = glm::normalize(axisRot);
            glm::quat q = glm::angleAxis(length * 2.0f, axisRot);
            m_quatRot = q * m_quatRot;
        }
        m_virtualNormalizedMouseXY = nextVirtualMouseXY;
    } else if (m_mode == CameraEnums::CameraMode::SPHERICAL) {
        glm::vec2 realDiff = normalizedMouseXY - m_lastNormalizedMouseXY;
        glm::vec2 scaledDiff = realDiff * speedFactor;
        glm::vec2 nextVirtualMouseXY = m_virtualNormalizedMouseXY + scaledDiff;
        float len = glm::length(nextVirtualMouseXY);
        if (len > 1.0f) {
            nextVirtualMouseXY /= len;
        }

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
    return m_projectionType == CameraEnums::Projection::PERSPECTIVE ? m_trans.z * 45.0f / getFovDegrees() : 1000.0f;
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
    float h = m_height > 0 ? static_cast<float>(m_height) : 1.0f;
    float factor = (m_speed * m_trans.z / 54.0f) * m_speedTranslate / h;
    glm::vec3 delta(-dx * factor, dy * factor, 0.0f);
    m_trans += delta;
    if (m_projectionType == CameraEnums::Projection::ORTHOGRAPHIC) {
        updateOrtho();
    }
    updateView();
}

void Camera::zoom(float df) {
    glm::vec3 delta = m_offset - m_trans;
    delta *= df * (m_speed / 54.0f) * m_speedZoom;
    if (df < 0.0f) {
        delta.x = delta.y = 0.0f;
    }
    m_trans += delta;
    if (m_projectionType == CameraEnums::Projection::ORTHOGRAPHIC) {
        updateOrtho();
    }
    updateView();
}

glm::vec3 Camera::computePosition() const {
    return glm::vec3(glm::inverse(m_viewMatrix)[3]);
}

void Camera::resetView() {
    m_speed = 100.0f * 1.5f; // matches Utils.SCALE * 1.5
    m_center = glm::vec3(0.0f);
    m_offset = glm::vec3(0.0f);
    m_trans = glm::vec3(0.0f, 0.0f, 30.0f + m_speed / 3.0f);
    m_quatRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    m_rotX = 0.0f;
    m_rotY = 0.0f;
    m_view2DOffsetX = 0.0f;
    m_view2DOffsetY = 0.0f;
    m_view2DZoom = 1.0f;
    updateView();
    pushState();
}

void Camera::resetViewToMesh(const float* bbox) {
    if (!bbox) {
        resetView();
        return;
    }
    float centerX = (bbox[0] + bbox[3]) * 0.5f;
    float centerY = (bbox[1] + bbox[4]) * 0.5f;
    float centerZ = (bbox[2] + bbox[5]) * 0.5f;
    m_center = glm::vec3(centerX, centerY, centerZ);
    m_offset = m_center;

    float dx = bbox[3] - bbox[0];
    float dy = bbox[4] - bbox[1];
    float dz = bbox[5] - bbox[2];
    float radius = std::sqrt(dx*dx + dy*dy + dz*dz) * 0.5f;
    if (radius < 1e-4f) radius = 50.0f;

    m_speed = radius * 1.5f;
    m_trans = glm::vec3(0.0f, 0.0f, radius * 3.0f);
    m_quatRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    m_rotX = 0.0f;
    m_rotY = 0.0f;
    m_view2DOffsetX = 0.0f;
    m_view2DOffsetY = 0.0f;
    m_view2DZoom = 1.0f;
    updateView();
    pushState();
}

Ray Camera::getRay(float mouseX, float mouseY) const {
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
    m_quatRot = QUAT_COMP[bestId];
    
    if (m_mode == CameraEnums::CameraMode::ORBIT) {
        float qx = m_quatRot.x;
        float qy = m_quatRot.y;
        float qz = m_quatRot.z;
        float qw = m_quatRot.w;
        m_rotY = std::atan2(2.0f * (qw * qy + qz * qx), 1.0f - 2.0f * (qy * qy + qz * qz));
        m_rotX = std::max<float>(-M_PI * 0.49f, std::min<float>(M_PI * 0.49f, std::atan2(2.0f * (qw * qx + qy * qz), 1.0f - 2.0f * (qz * qz + qx * qx))));
    }
    updateView();
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
        m_view2DZoom
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
                     std::abs(prev.view2DZoom - state.view2DZoom) < 1e-5f;
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
    updateView();
    updateProjection();
}

void Camera::toggleViewFront() {
    m_quatRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (m_mode == CameraEnums::CameraMode::ORBIT) {
        m_rotX = 0.0f;
        m_rotY = 0.0f;
    }
    updateView();
    pushState();
}

void Camera::toggleViewTop() {
    float SQ = 0.70710678f;
    m_quatRot = glm::quat(SQ, SQ, 0.0f, 0.0f);
    if (m_mode == CameraEnums::CameraMode::ORBIT) {
        m_rotX = 1.570796f; // PI/2
        m_rotY = 0.0f;
    }
    updateView();
    pushState();
}

void Camera::toggleViewLeft() {
    float SQ = 0.70710678f;
    m_quatRot = glm::quat(SQ, 0.0f, -SQ, 0.0f);
    if (m_mode == CameraEnums::CameraMode::ORBIT) {
        m_rotX = 0.0f;
        m_rotY = -1.570796f; // -PI/2
    }
    updateView();
    pushState();
}
