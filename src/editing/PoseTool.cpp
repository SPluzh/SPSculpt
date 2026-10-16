#include "editing/PoseTool.h"
#include "editing/SculptManager.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "mesh/Mesh.h"
#include "editing/undo/TransformUndoEntry.h"
#include "editing/undo/UndoManager.h"
#include <imgui.h>
#include <imguizmo/ImGuizmo.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>
#include <algorithm>
#include <chrono>

PoseTool::PoseTool(SculptManager& sm)
    : m_sculpt(sm) {
}

void PoseTool::onActivate(Scene& scene) {
    Mesh* mesh = scene.getSelected();
    if (mesh && m_line.isValid) {
        computeWeights(mesh);
    }
}

void PoseTool::onDeactivate() {
    m_line.isDrawing = false;
    m_line.activeHandle = PoseHandle::None;
    m_line.isAltRepositioning = false;
    m_wasUsingGizmo = false;
    m_isUsingGizmo = false;
    m_gizmoDeformedMesh = false;
}

void PoseTool::clearLine() {
    m_line.isValid = false;
    m_line.isDrawing = false;
    m_line.activeHandle = PoseHandle::None;
    m_line.isAltRepositioning = false;
    m_weights.clear();
    m_weightsSym.clear();
    m_startVerts.clear();
    m_startNormals.clear();
    m_gizmoDeformedMesh = false;
}

void PoseTool::updateGizmoMatrixFromLine() {
    if (!m_line.isValid) return;

    glm::vec3 zAxis = m_line.endWorld - m_line.startWorld;
    float len = glm::length(zAxis);
    if (len > 1e-5f) {
        zAxis /= len;
    } else {
        zAxis = glm::vec3(0.0f, 0.0f, 1.0f);
    }

    glm::vec3 up = (std::abs(zAxis.y) < 0.95f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 xAxis = glm::normalize(glm::cross(up, zAxis));
    glm::vec3 yAxis = glm::cross(zAxis, xAxis);

    m_gizmoMatrix = glm::mat4(
        glm::vec4(xAxis, 0.0f),
        glm::vec4(yAxis, 0.0f),
        glm::vec4(zAxis, 0.0f),
        glm::vec4(m_line.startWorld, 1.0f)
    );
}

glm::vec3 PoseTool::intersectRayViewPlane(const Camera& camera, float mouseX, float mouseY, const glm::vec3& planePoint) const {
    glm::mat4 invView = glm::inverse(camera.getViewMatrix());
    glm::vec3 planeNormal = glm::vec3(invView[2]);
    glm::vec3 rayOrigin = camera.unproject(mouseX, mouseY, 0.0f);
    glm::vec3 rayFar = camera.unproject(mouseX, mouseY, 0.1f);
    glm::vec3 rayDir = glm::normalize(rayFar - rayOrigin);

    float denom = glm::dot(planeNormal, rayDir);
    if (std::abs(denom) > 1e-6f) {
        float t = glm::dot(planePoint - rayOrigin, planeNormal) / denom;
        return rayOrigin + rayDir * t;
    }
    return planePoint;
}

int PoseTool::hitTestHandles(const Camera& camera, float mouseX, float mouseY, float threshold) const {
    if (!m_line.isValid) return -1;
    glm::vec2 mousePos(mouseX, mouseY);

    // 0: Start handle
    glm::vec3 sStart = camera.project(m_line.startWorld);
    if (glm::distance(mousePos, glm::vec2(sStart.x, sStart.y)) <= threshold) {
        return 0;
    }

    // 2: End handle
    glm::vec3 sEnd = camera.project(m_line.endWorld);
    if (glm::distance(mousePos, glm::vec2(sEnd.x, sEnd.y)) <= threshold) {
        return 2;
    }

    // 1: Center handle
    glm::vec3 sCenter = camera.project(m_line.centerWorld);
    if (glm::distance(mousePos, glm::vec2(sCenter.x, sCenter.y)) <= threshold) {
        return 1;
    }

    return -1;
}

int PoseTool::hitTestSymHandles(const Camera& camera, const Mesh* mesh, float mouseX, float mouseY, float threshold) const {
    if (!m_line.isValid || !mesh || !m_sculpt.getUseSym()) return -1;

    bool symX = m_sculpt.getSymX();
    bool symY = m_sculpt.getSymY();
    bool symZ = m_sculpt.getSymZ();
    if (!symX && !symY && !symZ) return -1;

    SymmetryMode symMode = m_sculpt.getSymmetryMode();
    glm::vec3 sScale(symX ? -1.0f : 1.0f,
                     symY ? -1.0f : 1.0f,
                     symZ ? -1.0f : 1.0f);

    glm::mat4 invMesh = glm::inverse(mesh->matrix);
    glm::vec3 startLocal = glm::vec3(invMesh * glm::vec4(m_line.startWorld, 1.0f));
    glm::vec3 endLocal = glm::vec3(invMesh * glm::vec4(m_line.endWorld, 1.0f));

    glm::vec3 symStartLocal = reflectPointSymmetry(startLocal, sScale, mesh, symMode);
    glm::vec3 symEndLocal = reflectPointSymmetry(endLocal, sScale, mesh, symMode);

    glm::vec3 symStartWorld = glm::vec3(mesh->matrix * glm::vec4(symStartLocal, 1.0f));
    glm::vec3 symEndWorld = glm::vec3(mesh->matrix * glm::vec4(symEndLocal, 1.0f));
    glm::vec3 symCenterWorld = (symStartWorld + symEndWorld) * 0.5f;

    glm::vec2 mousePos(mouseX, mouseY);

    glm::vec3 sStart = camera.project(symStartWorld);
    if (glm::distance(mousePos, glm::vec2(sStart.x, sStart.y)) <= threshold) return 0;

    glm::vec3 sEnd = camera.project(symEndWorld);
    if (glm::distance(mousePos, glm::vec2(sEnd.x, sEnd.y)) <= threshold) return 2;

    glm::vec3 sCenter = camera.project(symCenterWorld);
    if (glm::distance(mousePos, glm::vec2(sCenter.x, sCenter.y)) <= threshold) return 1;

    return -1;
}

void PoseTool::mirrorLineAcrossSymmetry(Mesh* mesh) {
    if (!mesh || !m_line.isValid) return;

    bool symX = m_sculpt.getSymX();
    bool symY = m_sculpt.getSymY();
    bool symZ = m_sculpt.getSymZ();
    if (!symX && !symY && !symZ) return;

    SymmetryMode symMode = m_sculpt.getSymmetryMode();
    glm::vec3 sScale(symX ? -1.0f : 1.0f,
                     symY ? -1.0f : 1.0f,
                     symZ ? -1.0f : 1.0f);

    glm::mat4 invMesh = glm::inverse(mesh->matrix);
    glm::vec3 startLocal = glm::vec3(invMesh * glm::vec4(m_line.startWorld, 1.0f));
    glm::vec3 endLocal = glm::vec3(invMesh * glm::vec4(m_line.endWorld, 1.0f));

    glm::vec3 symStartLocal = reflectPointSymmetry(startLocal, sScale, mesh, symMode);
    glm::vec3 symEndLocal = reflectPointSymmetry(endLocal, sScale, mesh, symMode);

    m_line.startWorld = glm::vec3(mesh->matrix * glm::vec4(symStartLocal, 1.0f));
    m_line.endWorld = glm::vec3(mesh->matrix * glm::vec4(symEndLocal, 1.0f));
    m_line.centerWorld = (m_line.startWorld + m_line.endWorld) * 0.5f;

    updateGizmoMatrixFromLine();
    computeWeights(mesh);
}

bool PoseTool::handleMouseDown(Scene& scene, const Camera& camera, float mouseX, float mouseY, bool isCtrl, bool isAlt, bool isShift) {
    Mesh* mesh = scene.getSelected();
    if (!mesh) return false;

    // Check if user clicked on existing handles of the line
    if (m_line.isValid) {
        int hit = hitTestHandles(camera, mouseX, mouseY, 20.0f);
        if (hit < 0 && m_sculpt.getUseSym()) {
            int hitSym = hitTestSymHandles(camera, mesh, mouseX, mouseY, 20.0f);
            if (hitSym >= 0) {
                mirrorLineAcrossSymmetry(mesh);
                hit = hitSym;
            }
        }

        if (hit >= 0) {
            m_line.activeHandle = (PoseHandle)hit;
            m_line.isAltRepositioning = isAlt;
            m_line.dragStartMouse = glm::vec2(mouseX, mouseY);
            m_line.initialStartPos = m_line.startWorld;
            m_line.initialEndPos = m_line.endWorld;
            m_line.dragPlanePoint = (hit == 0) ? m_line.startWorld : ((hit == 2) ? m_line.endWorld : m_line.centerWorld);
            glm::mat4 invView = glm::inverse(camera.getViewMatrix());
            m_line.dragPlaneNormal = glm::vec3(invView[2]);

            if (!isAlt) {
                prepareDeformation(mesh);
            }
            return true;
        }
    }

    // Otherwise, start drawing a new Action Line
    MeasurementAnchor anchor = m_sculpt.pickAnchor(mouseX, mouseY, scene, nullptr);
    glm::vec3 startPos = anchor.worldPos;
    if (anchor.type == MeasurementAnchor::FREE) {
        startPos = intersectRayViewPlane(camera, mouseX, mouseY, camera.getPivot());
    }

    m_line.startWorld = startPos;
    m_line.endWorld = startPos;
    m_line.centerWorld = startPos;
    m_line.isDrawing = true;
    m_line.isValid = false;
    m_line.activeHandle = PoseHandle::None;
    m_line.dragStartMouse = glm::vec2(mouseX, mouseY);
    m_line.dragPlanePoint = startPos;
    glm::mat4 invView = glm::inverse(camera.getViewMatrix());
    m_line.dragPlaneNormal = glm::vec3(invView[2]);

    return true;
}

bool PoseTool::handleMouseMove(Scene& scene, const Camera& camera, float mouseX, float mouseY) {
    Mesh* mesh = scene.getSelected();

    // 1. User is drawing a new Action Line
    if (m_line.isDrawing) {
        MeasurementAnchor anchor = m_sculpt.pickAnchor(mouseX, mouseY, scene, &m_line.startWorld);
        if (anchor.type == MeasurementAnchor::VERTEX) {
            m_line.endWorld = anchor.worldPos;
        } else {
            m_line.endWorld = intersectRayViewPlane(camera, mouseX, mouseY, m_line.startWorld);
        }
        m_line.centerWorld = (m_line.startWorld + m_line.endWorld) * 0.5f;
        return true;
    }

    // 2. User is dragging an Action Line handle
    if (m_line.activeHandle != PoseHandle::None && m_line.isValid) {
        glm::vec3 curPos3D = intersectRayViewPlane(camera, mouseX, mouseY, m_line.dragPlanePoint);
        glm::vec3 delta3D = curPos3D - m_line.dragPlanePoint;

        if (m_line.isAltRepositioning) {
            // Reposition handles without deforming the mesh
            if (m_line.activeHandle == PoseHandle::Start) {
                m_line.startWorld = m_line.initialStartPos + delta3D;
            } else if (m_line.activeHandle == PoseHandle::End) {
                m_line.endWorld = m_line.initialEndPos + delta3D;
            } else if (m_line.activeHandle == PoseHandle::Center) {
                m_line.startWorld = m_line.initialStartPos + delta3D;
                m_line.endWorld = m_line.initialEndPos + delta3D;
            }
            m_line.centerWorld = (m_line.startWorld + m_line.endWorld) * 0.5f;
            updateGizmoMatrixFromLine();
            return true;
        }

        // Classic Action Line manipulation mode
        const auto& settings = m_sculpt.getCurrentSettings();
        if (settings.poseGizmoType == 1 && mesh) { // Action Line mode
            glm::mat4 invView = glm::inverse(camera.getViewMatrix());
            glm::vec3 viewDir = -glm::vec3(invView[2]);

            if (m_line.activeHandle == PoseHandle::End) {
                // Rotate around Start pivot
                glm::vec3 sStart = camera.project(m_line.startWorld);
                float a0 = std::atan2(m_line.dragStartMouse.y - sStart.y, m_line.dragStartMouse.x - sStart.x);
                float a1 = std::atan2(mouseY - sStart.y, mouseX - sStart.x);
                float angleDelta = a1 - a0;

                glm::mat4 rot = glm::rotate(glm::mat4(1.0f), angleDelta, viewDir);
                glm::mat4 deltaWorld = glm::translate(glm::mat4(1.0f), m_line.startWorld) * rot * glm::translate(glm::mat4(1.0f), -m_line.startWorld);
                m_line.endWorld = glm::vec3(deltaWorld * glm::vec4(m_line.initialEndPos, 1.0f));
                m_line.centerWorld = (m_line.startWorld + m_line.endWorld) * 0.5f;
                updateGizmoMatrixFromLine();
                applyDeformation(mesh, deltaWorld, m_sculpt.getUseSym());
            } else if (m_line.activeHandle == PoseHandle::Center) {
                // Translate along drag
                glm::mat4 deltaWorld = glm::translate(glm::mat4(1.0f), delta3D);
                m_line.startWorld = m_line.initialStartPos + delta3D;
                m_line.endWorld = m_line.initialEndPos + delta3D;
                m_line.centerWorld = (m_line.startWorld + m_line.endWorld) * 0.5f;
                updateGizmoMatrixFromLine();
                applyDeformation(mesh, deltaWorld, m_sculpt.getUseSym());
            } else if (m_line.activeHandle == PoseHandle::Start) {
                // Rotate around End pivot
                glm::vec3 sEnd = camera.project(m_line.endWorld);
                float a0 = std::atan2(m_line.dragStartMouse.y - sEnd.y, m_line.dragStartMouse.x - sEnd.x);
                float a1 = std::atan2(mouseY - sEnd.y, mouseX - sEnd.x);
                float angleDelta = a1 - a0;

                glm::mat4 rot = glm::rotate(glm::mat4(1.0f), angleDelta, viewDir);
                glm::mat4 deltaWorld = glm::translate(glm::mat4(1.0f), m_line.endWorld) * rot * glm::translate(glm::mat4(1.0f), -m_line.endWorld);
                m_line.startWorld = glm::vec3(deltaWorld * glm::vec4(m_line.initialStartPos, 1.0f));
                m_line.centerWorld = (m_line.startWorld + m_line.endWorld) * 0.5f;
                updateGizmoMatrixFromLine();
                applyDeformation(mesh, deltaWorld, m_sculpt.getUseSym());
            }
            return true;
        }
    }

    return false;
}

bool PoseTool::handleMouseUp(Scene& scene) {
    Mesh* mesh = scene.getSelected();

    if (m_line.isDrawing) {
        float lineDist = glm::distance(m_line.startWorld, m_line.endWorld);
        if (lineDist > 0.005f) {
            m_line.isValid = true;
            m_line.centerWorld = (m_line.startWorld + m_line.endWorld) * 0.5f;
            updateGizmoMatrixFromLine();
            if (mesh) {
                computeWeights(mesh);
            }
        } else {
            m_line.isValid = false;
        }
        m_line.isDrawing = false;
        return true;
    }

    if (m_line.activeHandle != PoseHandle::None) {
        if (m_line.isAltRepositioning) {
            updateGizmoMatrixFromLine();
            if (mesh) {
                computeWeights(mesh);
            }
        } else {
            finalizeDeformation(scene, mesh);
        }
        m_line.activeHandle = PoseHandle::None;
        m_line.isAltRepositioning = false;
        return true;
    }

    return false;
}

float PoseTool::evaluateBoneWeight(
    const glm::vec3& v,
    const glm::vec3& start,
    const glm::vec3& dir,
    float invLenSq,
    float falloffWidth,
    float capsuleRadius,
    float maskFactor
) const {
    glm::vec3 diff = v - start;
    float proj = glm::dot(diff, dir) * invLenSq; // normalized t along bone [0..1]

    // 1. Falloff along bone
    float tScaled = proj / falloffWidth;
    float wLine = 0.0f;
    if (tScaled <= 0.0f) {
        wLine = 0.0f;
    } else if (tScaled >= 1.0f) {
        wLine = 1.0f;
    } else {
        wLine = tScaled * tScaled * (3.0f - 2.0f * tScaled);
    }

    if (wLine <= 0.0005f) return 0.0f;

    // 2. Radial capsule limit
    float wRad = 1.0f;
    if (capsuleRadius > 1e-4f) {
        glm::vec3 ptOnLine = start + dir * glm::clamp(proj, 0.0f, 1.0f);
        float dist = glm::distance(v, ptOnLine);
        if (dist > capsuleRadius) {
            return 0.0f;
        } else {
            float tr = dist / capsuleRadius;
            wRad = 1.0f - tr * tr * (3.0f - 2.0f * tr);
        }
    }

    return wLine * wRad * maskFactor;
}

void PoseTool::computeWeights(Mesh* mesh) {
    if (!mesh || !m_line.isValid) {
        m_weights.clear();
        m_weightsSym.clear();
        return;
    }

    int nbVerts = mesh->nbVerts;
    m_weights.resize(nbVerts);

    glm::mat4 invMesh = glm::inverse(mesh->matrix);
    glm::vec3 startLocal = glm::vec3(invMesh * glm::vec4(m_line.startWorld, 1.0f));
    glm::vec3 endLocal = glm::vec3(invMesh * glm::vec4(m_line.endWorld, 1.0f));
    glm::vec3 dirLocal = endLocal - startLocal;
    float lineLenSq = glm::dot(dirLocal, dirLocal);
    if (lineLenSq < 1e-6f) {
        std::fill(m_weights.begin(), m_weights.end(), 1.0f);
        m_weightsSym.clear();
        return;
    }
    float invLineLenSq = 1.0f / lineLenSq;

    const auto& settings = m_sculpt.getCurrentSettings();
    float falloffWidth = std::max(0.01f, settings.poseFalloffWidth);
    float capsuleRadius = settings.poseCapsuleRadius;
    bool useMask = settings.poseUseMask && (mesh->materials.size() == (size_t)nbVerts * 3);

    bool symX = m_sculpt.getSymX();
    bool symY = m_sculpt.getSymY();
    bool symZ = m_sculpt.getSymZ();
    bool hasAnySymAxis = symX || symY || symZ;
    bool useSym = m_sculpt.getUseSym() && hasAnySymAxis;

    SymmetryMode symMode = m_sculpt.getSymmetryMode();
    glm::vec3 sScale(symX ? -1.0f : 1.0f,
                     symY ? -1.0f : 1.0f,
                     symZ ? -1.0f : 1.0f);

    glm::vec3 symStartLocal = startLocal;
    glm::vec3 symEndLocal = endLocal;
    glm::vec3 symDirLocal = dirLocal;
    float invSymLineLenSq = invLineLenSq;

    if (useSym) {
        m_weightsSym.resize(nbVerts);
        symStartLocal = reflectPointSymmetry(startLocal, sScale, mesh, symMode);
        symEndLocal = reflectPointSymmetry(endLocal, sScale, mesh, symMode);
        symDirLocal = symEndLocal - symStartLocal;
        float symLenSq = glm::dot(symDirLocal, symDirLocal);
        invSymLineLenSq = (symLenSq > 1e-6f) ? (1.0f / symLenSq) : invLineLenSq;
    } else {
        m_weightsSym.clear();
    }

#pragma omp parallel for schedule(static) if(nbVerts > 2000)
    for (int i = 0; i < nbVerts; ++i) {
        glm::vec3 v(mesh->verts[i * 3], mesh->verts[i * 3 + 1], mesh->verts[i * 3 + 2]);
        float wMask = useMask ? mesh->materials[i * 3 + 2] : 1.0f;

        m_weights[i] = evaluateBoneWeight(v, startLocal, dirLocal, invLineLenSq, falloffWidth, capsuleRadius, wMask);

        if (useSym) {
            m_weightsSym[i] = evaluateBoneWeight(v, symStartLocal, symDirLocal, invSymLineLenSq, falloffWidth, capsuleRadius, wMask);
        }
    }
}

void PoseTool::prepareDeformation(Mesh* mesh) {
    if (!mesh) return;
    m_startVerts = mesh->verts;
    m_startNormals = mesh->normals;
    m_gizmoStartMatrix = m_gizmoMatrix;
    m_gizmoStartEndWorld = m_line.endWorld;
    m_gizmoDeformedMesh = false;

    computeWeights(mesh);
}

void PoseTool::applyDeformation(Mesh* mesh, const glm::mat4& deltaMatrixWorld, bool useSym) {
    if (!mesh || m_startVerts.empty() || m_startVerts.size() != mesh->verts.size()) {
        return;
    }

    bool symX = m_sculpt.getSymX();
    bool symY = m_sculpt.getSymY();
    bool symZ = m_sculpt.getSymZ();
    bool hasAnySymAxis = symX || symY || symZ;
    bool useSymTransform = useSym && m_sculpt.getUseSym() && hasAnySymAxis;

    glm::mat4 invMesh = glm::inverse(mesh->matrix);
    glm::mat4 deltaLocal = invMesh * deltaMatrixWorld * mesh->matrix;
    glm::mat3 normalMatrixPrimary = glm::transpose(glm::inverse(glm::mat3(deltaLocal)));

    int nbVerts = mesh->nbVerts;

    if (!useSymTransform) {
#pragma omp parallel for schedule(static) if(nbVerts > 2000)
        for (int i = 0; i < nbVerts; ++i) {
            float m = (i < (int)m_weights.size()) ? m_weights[i] : 1.0f;
            if (m <= 0.001f) {
                mesh->verts[i * 3]     = m_startVerts[i * 3];
                mesh->verts[i * 3 + 1] = m_startVerts[i * 3 + 1];
                mesh->verts[i * 3 + 2] = m_startVerts[i * 3 + 2];
                mesh->normals[i * 3]   = m_startNormals[i * 3];
                mesh->normals[i * 3 + 1] = m_startNormals[i * 3 + 1];
                mesh->normals[i * 3 + 2] = m_startNormals[i * 3 + 2];
                continue;
            }

            glm::vec4 vStart(m_startVerts[i * 3], m_startVerts[i * 3 + 1], m_startVerts[i * 3 + 2], 1.0f);
            glm::vec3 nStart(m_startNormals[i * 3], m_startNormals[i * 3 + 1], m_startNormals[i * 3 + 2]);

            glm::vec4 vTransformed = deltaLocal * vStart;
            glm::vec3 nTransformed = glm::normalize(normalMatrixPrimary * nStart);

            glm::vec3 vNew = glm::mix(glm::vec3(vStart), glm::vec3(vTransformed), m);
            glm::vec3 nNew = glm::normalize(glm::mix(nStart, nTransformed, m));

            mesh->verts[i * 3]     = vNew.x;
            mesh->verts[i * 3 + 1] = vNew.y;
            mesh->verts[i * 3 + 2] = vNew.z;
            mesh->normals[i * 3]   = nNew.x;
            mesh->normals[i * 3 + 1] = nNew.y;
            mesh->normals[i * 3 + 2] = nNew.z;
        }

        mesh->isVertexDirty = true;
        mesh->dirtyVertMin = 0;
        mesh->dirtyVertMax = nbVerts - 1;
        mesh->isDirty = true;
        return;
    }

    // --- Symmetry Mode Active ---
    if (m_weightsSym.size() != (size_t)nbVerts) {
        computeWeights(mesh);
    }

    SymmetryMode symMode = m_sculpt.getSymmetryMode();
    int axis = m_sculpt.getSymAxis();
    glm::vec3 P_plane = mesh->getSymmetryOriginForAxis(axis, symMode);
    glm::vec3 N_plane = mesh->getSymmetryNormalForAxis(axis, symMode);

    float bbox[6];
    mesh->computeBbox(bbox);
    float diagLen = std::sqrt(
        (bbox[1] - bbox[0]) * (bbox[1] - bbox[0]) +
        (bbox[3] - bbox[2]) * (bbox[3] - bbox[2]) +
        (bbox[5] - bbox[4]) * (bbox[5] - bbox[4])
    );
    float blendEps = std::max(0.005f, 0.01f * diagLen);

    // Construct M_sym: Exact reflection matrix across the symmetry plane in mesh local space
    glm::mat4 M_sym(1.0f);
    if (symMode == SymmetryMode::World) {
        float worldRadius = mesh->computeLocalRadius() * mesh->scale;
        glm::vec3 worldNormalVec(0.0f);
        if (axis == 0) worldNormalVec.x = 1.0f;
        else if (axis == 1) worldNormalVec.y = 1.0f;
        else if (axis == 2) worldNormalVec.z = 1.0f;
        glm::vec3 worldOrigin = worldNormalVec * (mesh->symmetryOffset * worldRadius);

        glm::mat4 S_axis(1.0f);
        if (symX) S_axis[0][0] = -1.0f;
        if (symY) S_axis[1][1] = -1.0f;
        if (symZ) S_axis[2][2] = -1.0f;

        glm::mat4 T_w = glm::translate(glm::mat4(1.0f), worldOrigin);
        glm::mat4 T_w_inv = glm::translate(glm::mat4(1.0f), -worldOrigin);
        glm::mat4 M_world = T_w * S_axis * T_w_inv;
        M_sym = invMesh * M_world * mesh->matrix;
    } else {
        glm::mat4 S_axis(1.0f);
        if (symX) S_axis[0][0] = -1.0f;
        if (symY) S_axis[1][1] = -1.0f;
        if (symZ) S_axis[2][2] = -1.0f;

        glm::mat4 T_plane = glm::translate(glm::mat4(1.0f), P_plane);
        glm::mat4 T_plane_inv = glm::translate(glm::mat4(1.0f), -P_plane);
        M_sym = T_plane * S_axis * T_plane_inv;
    }

    glm::mat4 deltaSym = M_sym * deltaLocal * M_sym;
    glm::mat3 normalMatrixSym = glm::transpose(glm::inverse(glm::mat3(deltaSym)));

    glm::vec3 startLocal = glm::vec3(invMesh * glm::vec4(m_line.startWorld, 1.0f));
    float d_pivot = glm::dot(startLocal - P_plane, N_plane);
    float signPivot = (d_pivot >= 0.0f) ? 1.0f : -1.0f;

#pragma omp parallel for schedule(static) if(nbVerts > 2000)
    for (int i = 0; i < nbVerts; ++i) {
        float wP = (i < (int)m_weights.size()) ? m_weights[i] : 0.0f;
        float wS = (i < (int)m_weightsSym.size()) ? m_weightsSym[i] : 0.0f;

        if (wP <= 0.001f && wS <= 0.001f) {
            mesh->verts[i * 3]     = m_startVerts[i * 3];
            mesh->verts[i * 3 + 1] = m_startVerts[i * 3 + 1];
            mesh->verts[i * 3 + 2] = m_startVerts[i * 3 + 2];
            mesh->normals[i * 3]   = m_startNormals[i * 3];
            mesh->normals[i * 3 + 1] = m_startNormals[i * 3 + 1];
            mesh->normals[i * 3 + 2] = m_startNormals[i * 3 + 2];
            continue;
        }

        glm::vec4 vStart(m_startVerts[i * 3], m_startVerts[i * 3 + 1], m_startVerts[i * 3 + 2], 1.0f);
        glm::vec3 nStart(m_startNormals[i * 3], m_startNormals[i * 3 + 1], m_startNormals[i * 3 + 2]);

        // Signed distance to symmetry plane along plane normal
        float d_v = glm::dot(glm::vec3(vStart) - P_plane, N_plane);
        float signedDist = d_v * signPivot; // > 0 for primary side, < 0 for sym side

        float weightP = 1.0f;
        if (signedDist >= blendEps) {
            weightP = 1.0f;
        } else if (signedDist <= -blendEps) {
            weightP = 0.0f;
        } else {
            float t = 0.5f + 0.5f * (signedDist / blendEps);
            weightP = t * t * (3.0f - 2.0f * t);
        }

        glm::vec4 vP = deltaLocal * vStart;
        glm::vec4 vS = deltaSym * vStart;
        glm::vec3 nP = normalMatrixPrimary * nStart;
        glm::vec3 nS = normalMatrixSym * nStart;

        glm::vec4 vTransformed = glm::mix(vS, vP, weightP);
        glm::vec3 nTransformed = glm::normalize(glm::mix(nS, nP, weightP));
        float effectiveWeight = glm::mix(wS, wP, weightP);

        glm::vec3 vNew = glm::mix(glm::vec3(vStart), glm::vec3(vTransformed), effectiveWeight);
        glm::vec3 nNew = glm::normalize(glm::mix(nStart, nTransformed, effectiveWeight));

        mesh->verts[i * 3]     = vNew.x;
        mesh->verts[i * 3 + 1] = vNew.y;
        mesh->verts[i * 3 + 2] = vNew.z;
        mesh->normals[i * 3]   = nNew.x;
        mesh->normals[i * 3 + 1] = nNew.y;
        mesh->normals[i * 3 + 2] = nNew.z;
    }

    mesh->isVertexDirty = true;
    mesh->dirtyVertMin = 0;
    mesh->dirtyVertMax = nbVerts - 1;
    mesh->isDirty = true;
}

void PoseTool::finalizeDeformation(Scene& scene, Mesh* mesh) {
    if (!mesh) return;

    mesh->postInit();
    mesh->isDirty = true;
    mesh->isVertexDirty = true;
    scene.setModified(true);

    // Record undo state
    TransformUndoEntry undoEntry("Pose Tool");
    TransformMeshState state;
    state.meshId = mesh->getID();
    state.beforeMatrix = mesh->matrix;
    state.afterMatrix = mesh->matrix;
    state.hasVertexDeformation = true;
    state.beforeVerts = m_startVerts;
    state.afterVerts = mesh->verts;
    state.beforeNormals = m_startNormals;
    state.afterNormals = mesh->normals;
    undoEntry.m_meshStates.push_back(std::move(state));
    g_undoManager.pushTransformChange(scene, std::move(undoEntry));

    // Recompute weights for the new vertex positions
    computeWeights(mesh);
}

void PoseTool::renderOverlay(Scene& scene, const Camera& camera, float viewportW, float viewportH) {
    if (!m_line.isValid && !m_line.isDrawing) return;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList) return;

    glm::vec3 sStart = camera.project(m_line.startWorld);
    glm::vec3 sEnd = camera.project(m_line.endWorld);
    glm::vec3 sCenter = camera.project(m_line.centerWorld);

    ImVec2 pStart(sStart.x, sStart.y);
    ImVec2 pEnd(sEnd.x, sEnd.y);
    ImVec2 pCenter(sCenter.x, sCenter.y);

    // Primary Action Line colors: Teal accent
    ImU32 colLine = IM_COL32(0, 210, 190, 240);
    ImU32 colShadow = IM_COL32(0, 0, 0, 160);

    // 1. Draw connecting line with drop shadow
    drawList->AddLine(pStart, pEnd, colShadow, 5.0f);
    drawList->AddLine(pStart, pEnd, colLine, 2.5f);

    // 2. Draw bone length label
    float worldLength = glm::distance(m_line.startWorld, m_line.endWorld);
    char lenText[32];
    snprintf(lenText, sizeof(lenText), "%.2f", worldLength);
    drawList->AddText(ImVec2(pCenter.x + 8.0f, pCenter.y - 14.0f), IM_COL32(255, 255, 255, 220), lenText);

    // 3. Draw Handles: Start (Pivot - Green), Center (Amber), End (Tip - Red)
    float r = 7.0f;
    // Start (Pivot)
    drawList->AddCircleFilled(pStart, r + 1.0f, colShadow);
    drawList->AddCircleFilled(pStart, r, IM_COL32(40, 220, 80, 255));
    drawList->AddCircle(pStart, r, IM_COL32(255, 255, 255, 255), 0, 1.5f);

    // End (Tip)
    drawList->AddCircleFilled(pEnd, r + 1.0f, colShadow);
    drawList->AddCircleFilled(pEnd, r, IM_COL32(240, 60, 60, 255));
    drawList->AddCircle(pEnd, r, IM_COL32(255, 255, 255, 255), 0, 1.5f);

    // Center (Midpoint)
    drawList->AddCircleFilled(pCenter, r * 0.8f + 1.0f, colShadow);
    drawList->AddCircleFilled(pCenter, r * 0.8f, IM_COL32(240, 200, 40, 255));
    drawList->AddCircle(pCenter, r * 0.8f, IM_COL32(255, 255, 255, 255), 0, 1.5f);

    // 4. Draw Symmetry preview if enabled
    if (m_sculpt.getUseSym() && scene.getSelected() && (m_line.isValid || m_line.isDrawing)) {
        Mesh* mesh = scene.getSelected();
        bool symX = m_sculpt.getSymX();
        bool symY = m_sculpt.getSymY();
        bool symZ = m_sculpt.getSymZ();
        if (symX || symY || symZ) {
            SymmetryMode symMode = m_sculpt.getSymmetryMode();
            glm::vec3 sScale(symX ? -1.0f : 1.0f,
                             symY ? -1.0f : 1.0f,
                             symZ ? -1.0f : 1.0f);

            glm::mat4 invMesh = glm::inverse(mesh->matrix);
            glm::vec3 startLocal = glm::vec3(invMesh * glm::vec4(m_line.startWorld, 1.0f));
            glm::vec3 endLocal = glm::vec3(invMesh * glm::vec4(m_line.endWorld, 1.0f));

            glm::vec3 symStartLocal = reflectPointSymmetry(startLocal, sScale, mesh, symMode);
            glm::vec3 symEndLocal = reflectPointSymmetry(endLocal, sScale, mesh, symMode);

            glm::vec3 symStartWorld = glm::vec3(mesh->matrix * glm::vec4(symStartLocal, 1.0f));
            glm::vec3 symEndWorld = glm::vec3(mesh->matrix * glm::vec4(symEndLocal, 1.0f));
            glm::vec3 symCenterWorld = (symStartWorld + symEndWorld) * 0.5f;

            glm::vec3 sSymStart = camera.project(symStartWorld);
            glm::vec3 sSymEnd = camera.project(symEndWorld);
            glm::vec3 sSymCenter = camera.project(symCenterWorld);

            ImU32 colSymLine = IM_COL32(0, 210, 190, 160);
            ImU32 colSymShadow = IM_COL32(0, 0, 0, 120);

            // Mirrored line
            drawList->AddLine(ImVec2(sSymStart.x, sSymStart.y), ImVec2(sSymEnd.x, sSymEnd.y), colSymShadow, 4.0f);
            drawList->AddLine(ImVec2(sSymStart.x, sSymStart.y), ImVec2(sSymEnd.x, sSymEnd.y), colSymLine, 2.0f);

            // Mirrored Handles
            float rSym = 6.0f;
            drawList->AddCircleFilled(ImVec2(sSymStart.x, sSymStart.y), rSym, IM_COL32(40, 220, 80, 200));
            drawList->AddCircle(ImVec2(sSymStart.x, sSymStart.y), rSym, IM_COL32(255, 255, 255, 220), 0, 1.2f);

            drawList->AddCircleFilled(ImVec2(sSymEnd.x, sSymEnd.y), rSym, IM_COL32(240, 60, 60, 200));
            drawList->AddCircle(ImVec2(sSymEnd.x, sSymEnd.y), rSym, IM_COL32(255, 255, 255, 220), 0, 1.2f);

            drawList->AddCircleFilled(ImVec2(sSymCenter.x, sSymCenter.y), rSym * 0.8f, IM_COL32(240, 200, 40, 200));
            drawList->AddCircle(ImVec2(sSymCenter.x, sSymCenter.y), rSym * 0.8f, IM_COL32(255, 255, 255, 220), 0, 1.2f);
        }
    }
}

void PoseTool::renderGizmoAndDeform(Scene& scene, const Camera& camera, float viewportW, float viewportH) {
    if (!m_line.isValid) return;

    Mesh* mesh = scene.getSelected();
    if (!mesh) return;

    const auto& settings = m_sculpt.getCurrentSettings();
    if (settings.poseGizmoType != 0) {
        // Mode 1: Classic Action Line, ImGuizmo is skipped
        return;
    }

    ImGuizmo::BeginFrame();
    ImGuizmo::SetOrthographic(camera.isOrthographic());
    ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
    ImGuizmo::SetRect(0.0f, 0.0f, viewportW, viewportH);
    ImGuizmo::AllowAxisFlip(false);

    bool cameraDragging = m_sculpt.getCameraController().isDragging();
    ImGuizmo::Enable(!cameraDragging);

    glm::mat4 view = camera.getViewMatrix();
    glm::mat4 proj = camera.getProjMatrix();

    // Determine ImGuizmo operation
    ImGuizmo::OPERATION op = ImGuizmo::ROTATE;
    if (settings.poseOperation == 1) {
        op = ImGuizmo::TRANSLATE;
    } else if (settings.poseOperation == 2) {
        op = ImGuizmo::SCALE;
    }

    bool isMovingPivot = ImGui::GetIO().KeyAlt;
    bool isUsingGizmoNow = ImGuizmo::IsUsing();
    m_isUsingGizmo = isUsingGizmoNow;

    if (isUsingGizmoNow && !m_wasUsingGizmo) {
        prepareDeformation(mesh);
        m_gizmoStartMatrix = m_gizmoMatrix;
        m_gizmoStartEndWorld = m_line.endWorld;
        m_gizmoDeformedMesh = false;
        m_wasUsingGizmo = true;
    }

    glm::mat4 matrix = m_gizmoMatrix;
    if (ImGuizmo::Manipulate(
        glm::value_ptr(view), glm::value_ptr(proj),
        op, ImGuizmo::LOCAL, glm::value_ptr(matrix)
    )) {
        if (isMovingPivot) {
            // Reposition pivot with Alt without modifying mesh
            m_gizmoMatrix = matrix;
            m_line.startWorld = glm::vec3(matrix[3]);
            m_line.centerWorld = (m_line.startWorld + m_line.endWorld) * 0.5f;
        } else {
            m_gizmoMatrix = matrix;
            glm::mat4 deltaMatrixWorld = m_gizmoMatrix * glm::inverse(m_gizmoStartMatrix);
            m_line.startWorld = glm::vec3(matrix[3]);
            m_line.endWorld = glm::vec3(deltaMatrixWorld * glm::vec4(m_gizmoStartEndWorld, 1.0f));
            m_line.centerWorld = (m_line.startWorld + m_line.endWorld) * 0.5f;
            applyDeformation(mesh, deltaMatrixWorld, m_sculpt.getUseSym());
            m_gizmoDeformedMesh = true;
        }
    }

    if (!isUsingGizmoNow && m_wasUsingGizmo) {
        m_wasUsingGizmo = false;
        m_line.startWorld = glm::vec3(m_gizmoMatrix[3]);
        m_line.centerWorld = (m_line.startWorld + m_line.endWorld) * 0.5f;
        if (m_gizmoDeformedMesh) {
            m_gizmoDeformedMesh = false;
            finalizeDeformation(scene, mesh);
        } else {
            computeWeights(mesh);
        }
    }
}
