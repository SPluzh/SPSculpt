#include "editing/SculptManager.h"
#include "brushes/BrushPresetManager.h"
#include "sculpt/SculptEngine.h"
#include "mesh/NormalCalc.h"
#include "editing/ArmatureTool.h"
#include "editing/undo/UndoManager.h"
#include "common/Logger.h"
#include <glm/gtc/matrix_transform.hpp>
#ifdef _WIN32
#include "platform/TabletInput.h"
#endif
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

static glm::vec3 vertexOnLine(const glm::vec3& vertex, const glm::vec3& vNear, const glm::vec3& vFar) {
    glm::vec3 ab = vFar - vNear;
    float len2 = glm::dot(ab, ab);
    if (len2 < 1e-12f) return vNear;
    float dot = glm::dot(ab, vertex - vNear);
    return vNear + ab * (dot / len2);
}

#include <fstream>
#include <sstream>
#include <unordered_map>

static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

static int safe_stoi(const std::string& str, int defaultVal = 0) {
    try {
        return std::stoi(str);
    } catch (...) {
        return defaultVal;
    }
}

static float safe_stof(const std::string& str, float defaultVal = 0.0f) {
    try {
        return std::stof(str);
    } catch (...) {
        return defaultVal;
    }
}

static std::vector<uint32_t> pickVerticesInSphereTopological(
    Mesh* mesh, const glm::vec3& center, float radius2, uint32_t startFaceId
) {
    std::vector<uint32_t> picked;
    if (!mesh || startFaceId == 0xffffffff || startFaceId >= (uint32_t)mesh->nbFaces) return picked;

    uint32_t nbVerts = mesh->nbVerts;
    std::vector<uint8_t> visited(nbVerts, 0);
    std::vector<uint32_t> queue;
    queue.reserve(1024);

    uint32_t fv[4] = {
        mesh->faces[startFaceId * 4],
        mesh->faces[startFaceId * 4 + 1],
        mesh->faces[startFaceId * 4 + 2],
        mesh->faces[startFaceId * 4 + 3]
    };

    for (int i = 0; i < 4; ++i) {
        uint32_t vid = fv[i];
        if (vid == 0xffffffff) continue;
        if (!mesh->vertVisible[vid]) continue;

        float vx = mesh->verts[vid * 3];
        float vy = mesh->verts[vid * 3 + 1];
        float vz = mesh->verts[vid * 3 + 2];
        float dx = vx - center.x;
        float dy = vy - center.y;
        float dz = vz - center.z;
        if (dx*dx + dy*dy + dz*dz <= radius2) {
            queue.push_back(vid);
            visited[vid] = 1;
        }
    }

    size_t head = 0;
    while (head < queue.size()) {
        uint32_t u = queue[head++];
        picked.push_back(u);

        uint32_t start = mesh->vrvStartCount[u * 2];
        uint32_t count = mesh->vrvStartCount[u * 2 + 1];
        for (uint32_t j = 0; j < count; ++j) {
            uint32_t v = mesh->vertRingVert[start + j];
            if (visited[v]) continue;
            visited[v] = 1;

            if (!mesh->vertVisible[v]) continue;

            float vx = mesh->verts[v * 3];
            float vy = mesh->verts[v * 3 + 1];
            float vz = mesh->verts[v * 3 + 2];
            float dx = vx - center.x;
            float dy = vy - center.y;
            float dz = vz - center.z;
            if (dx*dx + dy*dy + dz*dz <= radius2) {
                queue.push_back(v);
            }
        }
    }

    return picked;
}

static void filterCullingVertices(
    std::vector<uint32_t>& pickedVertices, Mesh* mesh, const glm::vec3& eyeDirLocal
) {
    std::vector<uint32_t> front;
    front.reserve(pickedVertices.size());
    for (uint32_t vid : pickedVertices) {
        float nx = mesh->normals[vid * 3];
        float ny = mesh->normals[vid * 3 + 1];
        float nz = mesh->normals[vid * 3 + 2];
        if (nx * eyeDirLocal.x + ny * eyeDirLocal.y + nz * eyeDirLocal.z <= 0.0f) {
            front.push_back(vid);
        }
    }
    pickedVertices = std::move(front);
}

SculptManager::~SculptManager() = default;

SculptManager::SculptManager() {
    m_armatureTool = std::make_unique<ArmatureTool>(*this);
    // Initialise all brushes with baseline defaults
    for (int i = 0; i < BRUSH_COUNT; ++i) {
        m_brushSettings[i].radius = 50.0f;
        m_brushSettings[i].intensity = 0.5f;
        m_brushSettings[i].focalShift = 0.0f;
        m_brushSettings[i].focalShiftFalloff = true;
        m_brushSettings[i].hardness = 0.5f;
        m_brushSettings[i].spacing = 0.15f;
        m_brushSettings[i].negative = false;
        m_brushSettings[i].culling = false;
        m_brushSettings[i].accumulate = true;
        m_brushSettings[i].lockPosition = false;
        m_brushSettings[i].clay = false;
        m_brushSettings[i].tangent = false;
        m_brushSettings[i].topoCheck = false;
        m_brushSettings[i].elasticity = 1.0f;
        m_brushSettings[i].paintColor = glm::vec3(0.72f, 0.52f, 0.45f);
        m_brushSettings[i].paintRoughness = 0.5f;
        m_brushSettings[i].paintMetallic = 0.0f;
        m_brushSettings[i].writeAlbedo = true;
        m_brushSettings[i].writeRoughness = true;
        m_brushSettings[i].writeMetalness = true;
        m_brushSettings[i].pickColor = false;
        m_brushSettings[i].maskSharpenBlurIterations = 4;
        m_brushSettings[i].maskSharpenFactor = 1.0f;
        m_brushSettings[i].maskExtractThickness = 0.05f;
    }

    // Set brush-specific defaults
    m_brushSettings[BRUSH_FLATTEN].negative = true;
    m_brushSettings[BRUSH_FLATTEN].culling = true;

    m_brushSettings[BRUSH_SMOOTH].intensity = 0.75f;
    m_brushSettings[BRUSH_SMOOTH].culling = true;

    m_brushSettings[BRUSH_INFLATE].intensity = 0.3f;
    m_brushSettings[BRUSH_INFLATE].culling = true;

    m_brushSettings[BRUSH_PINCH].intensity = 0.75f;
    m_brushSettings[BRUSH_PINCH].culling = true;

    m_brushSettings[BRUSH_CREASE].intensity = 0.75f;
    m_brushSettings[BRUSH_CREASE].negative = true;
    m_brushSettings[BRUSH_CREASE].culling = true;

    m_brushSettings[BRUSH_VTOOL].intensity = 0.75f;
    m_brushSettings[BRUSH_VTOOL].negative = true;
    m_brushSettings[BRUSH_VTOOL].culling = true;

    m_brushSettings[BRUSH_MOVE].radius = 100.0f;

    m_brushSettings[BRUSH_DRAG].radius = 100.0f;

    m_brushSettings[BRUSH_ELASTIC].radius = 100.0f;
    m_brushSettings[BRUSH_ELASTIC].elasticity = 0.5f;

    m_brushSettings[BRUSH_MASK].intensity = 1.0f;
    m_brushSettings[BRUSH_MASK].negative = true;
    m_brushSettings[BRUSH_MASK].culling = true;

    m_brushSettings[BRUSH_PAINT].hardness = 0.7f;
    m_brushSettings[BRUSH_PAINT].culling = true;

    m_brushSettings[BRUSH_POLYGROUP].radius = 60.0f;
    m_brushSettings[BRUSH_POLYGROUP].culling = true;

    m_brushSettings[BRUSH_TWIST].radius = 100.0f;
    m_brushSettings[BRUSH_TWIST].culling = true;

    m_brushSettings[BRUSH_LOCALSCALE].radius = 100.0f;
    m_brushSettings[BRUSH_LOCALSCALE].culling = true;

    m_brushSettings[BRUSH_CLAY].clay = true;
    m_brushSettings[BRUSH_CLAY].accumulate = true;
    m_brushSettings[BRUSH_CLAY].culling = false;

    m_brushSettings[BRUSH_CLAYBUILDUP].intensity = 0.3f;
    m_brushSettings[BRUSH_CLAYBUILDUP].clay = true;
    m_brushSettings[BRUSH_CLAYBUILDUP].accumulate = false;
    m_brushSettings[BRUSH_CLAYBUILDUP].culling = false;

    m_brushSettings[BRUSH_DAMSTANDARD].intensity = 0.75f;
    m_brushSettings[BRUSH_DAMSTANDARD].negative = true;
    m_brushSettings[BRUSH_DAMSTANDARD].culling = true;

    m_brushSettings[BRUSH_SQUAREBRUSH].clay = true;
    m_brushSettings[BRUSH_SQUAREBRUSH].culling = false;

    m_brushSettings[BRUSH_VISIBILITY].culling = false;

    m_brushSettings[BRUSH_MASK_GRADIENT_BLUR].maskSharpenBlurIterations = 40;
    m_brushSettings[BRUSH_MASK_GRADIENT_BLUR].culling = false;

    m_brushSettings[BRUSH_MEASURE].culling = false;
    m_brushSettings[BRUSH_DIVIDER].culling = false;
    m_brushSettings[BRUSH_TRANSFORM].culling = false;

    m_brushSettings[BRUSH_BRUSH].radius = 50.0f;
    m_brushSettings[BRUSH_BRUSH].intensity = 0.5f;
    m_brushSettings[BRUSH_BRUSH].culling = true;
    m_brushSettings[BRUSH_BRUSH].accumulate = true;
    m_brushSettings[BRUSH_BRUSH].stampBlur = 0.0f;
    m_brushSettings[BRUSH_BRUSH].stampLockRotation = false;
    m_brushSettings[BRUSH_BRUSH].stampUseTilt = false;
}

std::vector<glm::vec3> SculptManager::getActiveSymmetryScales() const {
    std::vector<glm::vec3> scales;
    if (!m_useSym || (!m_symX && !m_symY && !m_symZ)) return scales;

    std::vector<float> xVals = { 1.0f };
    std::vector<float> yVals = { 1.0f };
    std::vector<float> zVals = { 1.0f };

    if (m_symX) xVals.push_back(-1.0f);
    if (m_symY) yVals.push_back(-1.0f);
    if (m_symZ) zVals.push_back(-1.0f);

    for (float sx : xVals) {
        for (float sy : yVals) {
            for (float sz : zVals) {
                if (sx == 1.0f && sy == 1.0f && sz == 1.0f) continue;
                scales.push_back(glm::vec3(sx, sy, sz));
            }
        }
    }
    return scales;
}

int SculptManager::doStrokePass(
    Scene& scene,
    Mesh* mesh,
    BrushType activeBrush,
    bool negative,
    std::vector<uint32_t>& pickedVertices,
    const glm::vec3& currentIntersection,
    const glm::vec3& currentIntersectionNormal,
    const glm::vec3& initialIntersection,
    const glm::vec3& cachedAreaNormal,
    const glm::vec3& cachedAreaCenter,
    float localRadius,
    float intensity,
    float mouseX,
    bool isSymmetry,
    const glm::vec3& sScale
) {
    int deformedCount = 0;

    switch (activeBrush) {
        case BRUSH_FLATTEN: {
            glm::vec3 areaNormal = cachedAreaNormal;
            glm::vec3 areaCenter = cachedAreaCenter;

            if (!m_firstStrokeFrame && (!getCurrentSettings().flattenLockNormal || !getCurrentSettings().flattenLockOrigin)) {
                float areaResults[7] = {0.0f};
                if (computeAreaNormalAndCenter(
                    mesh->verts.data(),
                    mesh->normals.data(),
                    mesh->materials.data(),
                    pickedVertices.data(),
                    pickedVertices.size(),
                    areaResults
                )) {
                    if (!getCurrentSettings().flattenLockNormal) {
                        areaNormal = glm::vec3(areaResults[0], areaResults[1], areaResults[2]);
                    }
                    if (!getCurrentSettings().flattenLockOrigin) {
                        areaCenter = glm::vec3(areaResults[3], areaResults[4], areaResults[5]);
                    }
                } else {
                    if (!getCurrentSettings().flattenLockNormal) {
                        areaNormal = currentIntersectionNormal;
                    }
                    if (!getCurrentSettings().flattenLockOrigin) {
                        areaCenter = currentIntersection;
                    }
                }
            }

            deformedCount = strokeFlatten(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                areaCenter.x, areaCenter.y, areaCenter.z,
                areaNormal.x, areaNormal.y, areaNormal.z,
                localRadius, intensity,
                negative, getCurrentSettings().accumulate, getCurrentSettings().lockPosition,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
            );
            break;
        }
        case BRUSH_SMOOTH: {
            deformedCount = strokeSmooth(
                mesh->verts.data(),
                mesh->normals.data(),
                mesh->materials.data(),
                mesh->vrvStartCount.data(),
                mesh->vertRingVert.data(),
                mesh->vertOnEdge.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                localRadius, intensity,
                getCurrentSettings().tangent,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
            );
            break;
        }
        case BRUSH_INFLATE: {
            deformedCount = strokeInflate(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                mesh->normals.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                localRadius, intensity,
                negative,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                false, nullptr
            );
            break;
        }
        case BRUSH_PINCH: {
            deformedCount = strokePinch(
                mesh->verts.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                localRadius, intensity,
                negative,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                false, nullptr
            );
            break;
        }
        case BRUSH_CREASE: {
            deformedCount = strokeCrease(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                currentIntersectionNormal.x, currentIntersectionNormal.y, currentIntersectionNormal.z,
                localRadius, intensity,
                negative,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                false, nullptr
            );
            break;
        }
        case BRUSH_VTOOL: {
            deformedCount = strokeVTool(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                currentIntersectionNormal.x, currentIntersectionNormal.y, currentIntersectionNormal.z,
                localRadius, intensity,
                negative,
                -0.4f, true,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                false, nullptr
            );
            break;
        }
        case BRUSH_MOVE: {
            glm::vec3 dragDirection;
            if (negative) {
                glm::vec3 norm = (glm::length(currentIntersectionNormal) > 1e-6f) ? glm::normalize(currentIntersectionNormal) : glm::vec3(0.0f, 1.0f, 0.0f);
                glm::mat4 localToView = scene.getCamera().getViewMatrix() * mesh->matrix;
                glm::vec3 dragVec = currentIntersection - initialIntersection;
                glm::vec3 dragView = glm::vec3(localToView * glm::vec4(dragVec, 0.0f));
                glm::vec3 normView = glm::vec3(localToView * glm::vec4(norm, 0.0f));
                if (glm::length(normView) > 1e-6f) {
                    normView = glm::normalize(normView);
                }
                float amount2D = dragView.x * normView.x + dragView.y * normView.y;
                float amountDepth = (dragView.x + dragView.y) * 0.7071f * std::abs(normView.z);
                float amount = amount2D + amountDepth;

                glm::vec3 col0(mesh->matrix[0][0], mesh->matrix[0][1], mesh->matrix[0][2]);
                float scale = glm::length(col0);
                if (scale < 1e-12f) scale = 1.0f;
                float localAmount = amount / scale;

                dragDirection = norm * (localAmount * intensity);
            } else {
                dragDirection = (currentIntersection - initialIntersection) * intensity;
            }
            deformedCount = strokeMove(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                initialIntersection.x, initialIntersection.y, initialIntersection.z,
                dragDirection.x, dragDirection.y, dragDirection.z,
                localRadius,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                false, nullptr
            );
            break;
        }
        case BRUSH_DRAG: {
            glm::vec3 dragDirection;
            if (negative) {
                glm::vec3 norm = (glm::length(currentIntersectionNormal) > 1e-6f) ? glm::normalize(currentIntersectionNormal) : glm::vec3(0.0f, 1.0f, 0.0f);
                glm::mat4 localToView = scene.getCamera().getViewMatrix() * mesh->matrix;
                glm::vec3 dragVec = currentIntersection - initialIntersection;
                glm::vec3 dragView = glm::vec3(localToView * glm::vec4(dragVec, 0.0f));
                glm::vec3 normView = glm::vec3(localToView * glm::vec4(norm, 0.0f));
                if (glm::length(normView) > 1e-6f) {
                    normView = glm::normalize(normView);
                }
                float amount2D = dragView.x * normView.x + dragView.y * normView.y;
                float amountDepth = (dragView.x + dragView.y) * 0.7071f * std::abs(normView.z);
                float amount = amount2D + amountDepth;

                glm::vec3 col0(mesh->matrix[0][0], mesh->matrix[0][1], mesh->matrix[0][2]);
                float scale = glm::length(col0);
                if (scale < 1e-12f) scale = 1.0f;
                float localAmount = amount / scale;

                dragDirection = norm * (localAmount * intensity);
            } else {
                dragDirection = (currentIntersection - initialIntersection) * intensity;
            }
            deformedCount = strokeDrag(
                mesh->verts.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                initialIntersection.x, initialIntersection.y, initialIntersection.z,
                dragDirection.x, dragDirection.y, dragDirection.z,
                localRadius,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                false, nullptr
            );
            break;
        }
        case BRUSH_ELASTIC: {
            glm::vec3 dragDirection;
            if (negative) {
                glm::vec3 norm = (glm::length(currentIntersectionNormal) > 1e-6f) ? glm::normalize(currentIntersectionNormal) : glm::vec3(0.0f, 1.0f, 0.0f);
                glm::mat4 localToView = scene.getCamera().getViewMatrix() * mesh->matrix;
                glm::vec3 dragVec = currentIntersection - initialIntersection;
                glm::vec3 dragView = glm::vec3(localToView * glm::vec4(dragVec, 0.0f));
                glm::vec3 normView = glm::vec3(localToView * glm::vec4(norm, 0.0f));
                if (glm::length(normView) > 1e-6f) {
                    normView = glm::normalize(normView);
                }
                float amount2D = dragView.x * normView.x + dragView.y * normView.y;
                float amountDepth = (dragView.x + dragView.y) * 0.7071f * std::abs(normView.z);
                float amount = amount2D + amountDepth;

                glm::vec3 col0(mesh->matrix[0][0], mesh->matrix[0][1], mesh->matrix[0][2]);
                float scale = glm::length(col0);
                if (scale < 1e-12f) scale = 1.0f;
                float localAmount = amount / scale;

                dragDirection = norm * (localAmount * intensity);
            } else {
                dragDirection = (currentIntersection - initialIntersection) * intensity;
            }
            deformedCount = strokeElastic(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                initialIntersection.x, initialIntersection.y, initialIntersection.z,
                dragDirection.x, dragDirection.y, dragDirection.z,
                localRadius, getCurrentSettings().elasticity,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                false, nullptr
            );
            break;
        }
        case BRUSH_MASK: {
            deformedCount = strokeMask(
                mesh->verts.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                localRadius, intensity, getSettings(activeBrush).hardness,
                negative,
                getSettings(activeBrush).focalShift, getSettings(activeBrush).focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
            );
            break;
        }
        case BRUSH_PAINT: {
            deformedCount = strokePaint(
                mesh->verts.data(),
                mesh->colors.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                localRadius, intensity, getCurrentSettings().hardness,
                getCurrentSettings().paintColor.r, getCurrentSettings().paintColor.g, getCurrentSettings().paintColor.b,
                getCurrentSettings().paintRoughness, getCurrentSettings().paintMetallic,
                getCurrentSettings().writeAlbedo, getCurrentSettings().writeRoughness, getCurrentSettings().writeMetalness,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
            );
            break;
        }
        case BRUSH_POLYGROUP: {
            for (uint32_t v : pickedVertices) {
                if (v * 2 + 1 < mesh->vrfStartCount.size()) {
                    uint32_t start = mesh->vrfStartCount[v * 2];
                    uint32_t count = mesh->vrfStartCount[v * 2 + 1];
                    for (uint32_t j = start; j < start + count; ++j) {
                        if (j < mesh->vertRingFace.size()) {
                            uint32_t f = mesh->vertRingFace[j];
                            mesh->setFaceGroup(f, m_activeGroupId);
                        }
                    }
                }
            }
            mesh->isFaceGroupDirty = true;
            deformedCount = (int)pickedVertices.size();
            break;
        }
        case BRUSH_TWIST: {
            deformedCount = strokeTwist(
                mesh->verts.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                currentIntersectionNormal.x, currentIntersectionNormal.y, currentIntersectionNormal.z,
                localRadius, intensity * 3.14159f * 0.5f,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
            );
            break;
        }
        case BRUSH_LOCALSCALE: {
            deformedCount = strokeLocalScale(
                mesh->verts.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                localRadius, intensity,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
            );
            break;
        }
        case BRUSH_CLAY: {
            glm::vec3 areaNormal = cachedAreaNormal;
            glm::vec3 areaCenter = cachedAreaCenter;

            if (!m_firstStrokeFrame && (!getCurrentSettings().flattenLockNormal || !getCurrentSettings().flattenLockOrigin)) {
                float areaResults[7] = {0.0f};
                if (computeAreaNormalAndCenter(
                    mesh->verts.data(),
                    mesh->normals.data(),
                    mesh->materials.data(),
                    pickedVertices.data(),
                    pickedVertices.size(),
                    areaResults
                )) {
                    if (!getCurrentSettings().flattenLockNormal) {
                        areaNormal = glm::vec3(areaResults[0], areaResults[1], areaResults[2]);
                    }
                    if (!getCurrentSettings().flattenLockOrigin) {
                        areaCenter = glm::vec3(areaResults[3], areaResults[4], areaResults[5]);
                    }
                } else {
                    if (!getCurrentSettings().flattenLockNormal) {
                        areaNormal = currentIntersectionNormal;
                    }
                    if (!getCurrentSettings().flattenLockOrigin) {
                        areaCenter = currentIntersection;
                    }
                }
            }

            float off = localRadius * 0.1f;
            areaCenter += areaNormal * (negative ? -off : off);

            deformedCount = strokeFlatten(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                areaCenter.x, areaCenter.y, areaCenter.z,
                areaNormal.x, areaNormal.y, areaNormal.z,
                localRadius, intensity,
                negative, getCurrentSettings().accumulate, getCurrentSettings().lockPosition,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
            );
            break;
        }
        case BRUSH_CLAYBUILDUP: {
            glm::vec3 areaNormal = cachedAreaNormal;
            glm::vec3 areaCenter = cachedAreaCenter;

            if (!m_firstStrokeFrame && (!getCurrentSettings().flattenLockNormal || !getCurrentSettings().flattenLockOrigin)) {
                float areaResults[7] = {0.0f};
                if (computeAreaNormalAndCenter(
                    mesh->verts.data(),
                    mesh->normals.data(),
                    mesh->materials.data(),
                    pickedVertices.data(),
                    pickedVertices.size(),
                    areaResults
                )) {
                    if (!getCurrentSettings().flattenLockNormal) {
                        areaNormal = glm::vec3(areaResults[0], areaResults[1], areaResults[2]);
                    }
                    if (!getCurrentSettings().flattenLockOrigin) {
                        areaCenter = glm::vec3(areaResults[3], areaResults[4], areaResults[5]);
                    }
                } else {
                    if (!getCurrentSettings().flattenLockNormal) {
                        areaNormal = currentIntersectionNormal;
                    }
                    if (!getCurrentSettings().flattenLockOrigin) {
                        areaCenter = currentIntersection;
                    }
                }
            }

            float off = localRadius * 0.1f;
            areaCenter += areaNormal * (negative ? -off : off);

            glm::vec3 strokeDir(0.0f);
            bool hasStrokeDir = false;

            glm::vec3 passAlphaOrigin = isSymmetry ? (m_alphaOrigin * sScale) : m_alphaOrigin;

            if (m_firstStrokeFrame || !m_hasAlphaOrigin) {
                // Fallback to screen-space alignment
                const glm::mat4& invMeshMatrix = m_cachedInvMatrix;
                const glm::mat4& camWorld = m_cachedCamWorldMatrix;
                glm::vec3 camRightLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[0]), 0.0f)));
                if (isSymmetry) camRightLocal = camRightLocal * sScale;
                strokeDir = glm::normalize(camRightLocal - areaNormal * glm::dot(camRightLocal, areaNormal));
            } else {
                glm::vec3 movement = currentIntersection - passAlphaOrigin;
                float movementLen = glm::length(movement);
                if (movementLen > 1e-7f) {
                    glm::vec3 movementDir = movement / movementLen;
                    strokeDir = movementDir - areaNormal * glm::dot(movementDir, areaNormal);
                    float strokeDirLen = glm::length(strokeDir);
                    if (strokeDirLen > 1e-7f) {
                        strokeDir = strokeDir / strokeDirLen;
                        hasStrokeDir = true;
                    }
                }

                if (!hasStrokeDir) {
                    const glm::mat4& invMeshMatrix = m_cachedInvMatrix;
                    const glm::mat4& camWorld = m_cachedCamWorldMatrix;
                    glm::vec3 camRightLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[0]), 0.0f)));
                    if (isSymmetry) camRightLocal = camRightLocal * sScale;
                    strokeDir = glm::normalize(camRightLocal - areaNormal * glm::dot(camRightLocal, areaNormal));
                }
            }

            glm::vec3 eye = currentIntersection;
            glm::vec3 nor = currentIntersection + areaNormal * localRadius;
            glm::mat4 lookAtMat = glm::lookAt(eye, nor, strokeDir);

            float alphaLookAt[16];
            std::memcpy(alphaLookAt, &lookAtMat[0][0], 16 * sizeof(float));

            deformedCount = strokeSquareBrush(
                mesh->verts.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                areaCenter.x, areaCenter.y, areaCenter.z,
                areaNormal.x, areaNormal.y, areaNormal.z,
                localRadius, intensity * 0.1f,
                negative,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                1.0f, 1.0f, localRadius * 0.70710678f,
                alphaLookAt, false
            );
            break;
        }
        case BRUSH_DAMSTANDARD: {
            deformedCount = strokeDamStandard(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                currentIntersectionNormal.x, currentIntersectionNormal.y, currentIntersectionNormal.z,
                localRadius, intensity,
                negative,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                false, nullptr
            );
            break;
        }
        case BRUSH_SQUAREBRUSH: {
            glm::vec3 areaCenter = currentIntersection;
            glm::vec3 areaNormal = currentIntersectionNormal;
            std::vector<float> areaResults(7, 0.0f);
            computeAreaNormalAndCenter(
                mesh->verts.data(),
                mesh->normals.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                areaResults.data()
            );
            areaNormal = glm::vec3(areaResults[0], areaResults[1], areaResults[2]);
            areaCenter = glm::vec3(areaResults[3], areaResults[4], areaResults[5]);

            float off = localRadius * 0.1f;
            areaCenter += areaNormal * (negative ? -off : off);

            glm::vec3 strokeDir(0.0f);
            bool hasStrokeDir = false;

            glm::vec3 passAlphaOrigin = isSymmetry ? (m_alphaOrigin * sScale) : m_alphaOrigin;

            if (m_firstStrokeFrame || !m_hasAlphaOrigin) {
                // Fallback to screen-space alignment
                glm::mat4 invMeshMatrix = glm::inverse(mesh->matrix);
                glm::mat4 camWorld = glm::inverse(scene.getCamera().getViewMatrix());
                glm::vec3 camRightLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[0]), 0.0f)));
                if (isSymmetry) camRightLocal = camRightLocal * sScale;
                strokeDir = glm::normalize(camRightLocal - areaNormal * glm::dot(camRightLocal, areaNormal));
            } else {
                glm::vec3 movement = currentIntersection - passAlphaOrigin;
                float movementLen = glm::length(movement);
                if (movementLen > 1e-7f) {
                    glm::vec3 movementDir = movement / movementLen;
                    strokeDir = movementDir - areaNormal * glm::dot(movementDir, areaNormal);
                    float strokeDirLen = glm::length(strokeDir);
                    if (strokeDirLen > 1e-7f) {
                        strokeDir = strokeDir / strokeDirLen;
                        hasStrokeDir = true;
                    }
                }

                if (!hasStrokeDir) {
                    glm::mat4 invMeshMatrix = glm::inverse(mesh->matrix);
                    glm::mat4 camWorld = glm::inverse(scene.getCamera().getViewMatrix());
                    glm::vec3 camRightLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[0]), 0.0f)));
                    if (isSymmetry) camRightLocal = camRightLocal * sScale;
                    strokeDir = glm::normalize(camRightLocal - areaNormal * glm::dot(camRightLocal, areaNormal));
                }
            }

            glm::vec3 eye = currentIntersection;
            glm::vec3 nor = currentIntersection + areaNormal * localRadius;
            glm::mat4 lookAtMat = glm::lookAt(eye, nor, strokeDir);

            float alphaLookAt[16];
            std::memcpy(alphaLookAt, &lookAtMat[0][0], 16 * sizeof(float));

            deformedCount = strokeSquareBrush(
                mesh->verts.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                areaCenter.x, areaCenter.y, areaCenter.z,
                areaNormal.x, areaNormal.y, areaNormal.z,
                localRadius, intensity,
                negative,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                1.0f, 1.0f, localRadius * 0.70710678f,
                alphaLookAt, false
            );
            break;
        }
        case BRUSH_BRUSH: {
            glm::vec3 areaCenter = currentIntersection;
            glm::vec3 areaNormal = currentIntersectionNormal;
            std::vector<float> areaResults(7, 0.0f);
            computeAreaNormalAndCenter(
                mesh->verts.data(),
                mesh->normals.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                areaResults.data()
            );
            if (areaResults[6] > 0.0f) {
                areaNormal = glm::vec3(areaResults[0], areaResults[1], areaResults[2]);
                areaCenter = glm::vec3(areaResults[3], areaResults[4], areaResults[5]);
            }

            if (getCurrentSettings().clay) {
                float off = localRadius * 0.1f;
                areaCenter += areaNormal * (negative ? -off : off);
            }

            glm::vec3 strokeDir(0.0f);
            bool hasStrokeDir = false;

            glm::vec3 passAlphaOrigin = isSymmetry ? (m_alphaOrigin * sScale) : m_alphaOrigin;

            if (getCurrentSettings().stampLockRotation) {
                glm::mat4 invMeshMatrix = glm::inverse(mesh->matrix);
                glm::mat4 camWorld = glm::inverse(scene.getCamera().getViewMatrix());
                glm::vec3 camRightLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[0]), 0.0f)));
                if (isSymmetry) camRightLocal = camRightLocal * sScale;
                strokeDir = glm::normalize(camRightLocal - areaNormal * glm::dot(camRightLocal, areaNormal));
            }
            else if (m_firstStrokeFrame || !m_hasAlphaOrigin) {
                // Fallback to screen-space alignment
                glm::mat4 invMeshMatrix = glm::inverse(mesh->matrix);
                glm::mat4 camWorld = glm::inverse(scene.getCamera().getViewMatrix());
                glm::vec3 camRightLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[0]), 0.0f)));
                if (isSymmetry) camRightLocal = camRightLocal * sScale;
                strokeDir = glm::normalize(camRightLocal - areaNormal * glm::dot(camRightLocal, areaNormal));
            } else {
                glm::vec3 movement = currentIntersection - passAlphaOrigin;
                float movementLen = glm::length(movement);
                if (movementLen > 1e-7f) {
                    glm::vec3 movementDir = movement / movementLen;
                    strokeDir = movementDir - areaNormal * glm::dot(movementDir, areaNormal);
                    float strokeDirLen = glm::length(strokeDir);
                    if (strokeDirLen > 1e-7f) {
                        strokeDir = strokeDir / strokeDirLen;
                        hasStrokeDir = true;
                    }
                }

                if (!hasStrokeDir) {
                    glm::mat4 invMeshMatrix = glm::inverse(mesh->matrix);
                    glm::mat4 camWorld = glm::inverse(scene.getCamera().getViewMatrix());
                    glm::vec3 camRightLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[0]), 0.0f)));
                    if (isSymmetry) camRightLocal = camRightLocal * sScale;
                    strokeDir = glm::normalize(camRightLocal - areaNormal * glm::dot(camRightLocal, areaNormal));
                }
            }

            glm::vec3 eye = currentIntersection;
            glm::vec3 nor = currentIntersection + areaNormal * localRadius;
            glm::mat4 lookAtMat = glm::lookAt(eye, nor, strokeDir);

            float alphaLookAt[16];
            std::memcpy(alphaLookAt, &lookAtMat[0][0], 16 * sizeof(float));

            float finalAngle = getCurrentSettings().stampAngle;
#ifdef _WIN32
            if (getCurrentSettings().stampUseTilt && g_tablet.isAvailable() && g_tablet.isPenActive() && g_tablet.isTiltEnabled()) {
                float tx = g_tablet.getTiltX();
                float ty = g_tablet.getTiltY();
                if (tx * tx + ty * ty > 1.0f) {
                    float tiltAngle = std::atan2(ty, tx) * (180.0f / 3.1415926535f);
                    finalAngle += tiltAngle;
                }
            }
#endif

            deformedCount = strokeBrush(
                mesh->verts.data(),
                mesh->vertProxy.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                currentIntersection.x, currentIntersection.y, currentIntersection.z,
                areaCenter.x, areaCenter.y, areaCenter.z,
                areaNormal.x, areaNormal.y, areaNormal.z,
                localRadius, intensity,
                negative, getCurrentSettings().clay,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                getCurrentSettings().stampType,
                getCurrentSettings().stampSides,
                getCurrentSettings().stampInnerRatio,
                finalAngle,
                getCurrentSettings().stampBlur,
                alphaLookAt, false
            );
            break;
        }
        default:
            break;
    }

    return deformedCount;
}

void SculptManager::cancelStroke() {
    m_isSculpting = false;
    m_grabbedVertices.clear();
    m_grabbedVerticesSyms.clear();
    m_initialSymIntersections.clear();
    m_lassoPoints.clear();
    m_isLassoActive = false;
    m_gradIsDrawing = false;
    m_draggedSegment = nullptr;
    m_hasAlphaOrigin = false;
}

void SculptManager::executeStroke(Scene& scene, Mesh* mesh, Camera& camera, float mouseX, float mouseY, float currentPressure) {
    if (m_firstStrokeFrame) {
        m_cachedInvMatrix = glm::inverse(mesh->matrix);
        m_cachedCamWorldMatrix = glm::inverse(camera.getViewMatrix());
    }
    const glm::mat4& invMatrix = m_cachedInvMatrix;

    Ray ray = camera.getRay(mouseX, mouseY);
    glm::vec3 localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
    glm::vec3 localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

    BrushType activeBrush = m_currentBrush;
    if (SDL_GetModState() & KMOD_SHIFT) {
        activeBrush = BRUSH_SMOOTH;
    } else if ((SDL_GetModState() & KMOD_CTRL) && m_currentBrush != BRUSH_POLYGROUP) {
        activeBrush = BRUSH_MASK;
    }
    if (activeBrush != m_currentBrush && (activeBrush == BRUSH_SMOOTH || activeBrush == BRUSH_MASK)) {
        m_brushSettings[activeBrush].radius = m_brushSettings[m_currentBrush].radius;
    }

    bool isGrabBrush = (activeBrush == BRUSH_MOVE || activeBrush == BRUSH_DRAG || activeBrush == BRUSH_ELASTIC);

    uint32_t intersectFaceId = 0xffffffff;

    if (isGrabBrush && !m_firstStrokeFrame) {
        // Bypass raycast for grab brushes on subsequent frames
        glm::vec3 camFrontLocal = -glm::normalize(glm::vec3(m_cachedInvMatrix * glm::vec4(glm::vec3(m_cachedCamWorldMatrix[2]), 0.0f)));
        float denom = glm::dot(camFrontLocal, localRayDir);
        if (std::abs(denom) > 1e-12f) {
            float t = glm::dot(camFrontLocal, m_initialIntersection - localRayOrigin) / denom;
            m_currentIntersection = localRayOrigin + localRayDir * t;
        } else {
            m_currentIntersection = m_initialIntersection;
        }
        m_currentIntersectionValid = true;

        m_lastValidIntersection = m_currentIntersection;
        m_hasAnyValidIntersection = true;
    } else {
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

        if (intersectFaceId == 0xffffffff) {
            m_currentIntersectionValid = false;
            return;
        }

        m_currentIntersectionValid = true;
        m_currentIntersection = localRayOrigin + minT * localRayDir;
        m_currentIntersectionNormal = glm::vec3(
            mesh->faceNormals[intersectFaceId * 3],
            mesh->faceNormals[intersectFaceId * 3 + 1],
            mesh->faceNormals[intersectFaceId * 3 + 2]
        );

        // Save last valid intersection to prevent cursor jitter (Step 1b)
        m_lastValidIntersection = m_currentIntersection;
        m_lastValidIntersectionNormal = m_currentIntersectionNormal;
        m_hasAnyValidIntersection = true;

        if (m_firstStrokeFrame && intersectFaceId != 0xffffffff && !mesh->faceGroups.empty() && intersectFaceId < (uint32_t)mesh->nbFaces) {
            m_strokeTargetPolyGroup = mesh->faceGroups[intersectFaceId];
        }
    }

    if (activeBrush == BRUSH_POLYGROUP) {
        if ((SDL_GetModState() & KMOD_ALT) != 0) {
            if (m_currentIntersectionValid && intersectFaceId != 0xffffffff) {
                m_activeGroupId = m_polyGroupTool.getGroupAtFace(mesh, intersectFaceId);
                std::cout << "[PolyGroup] Eyedropper picked Group ID: " << m_activeGroupId << std::endl;
            }
            return;
        } else if ((SDL_GetModState() & KMOD_CTRL) != 0) {
            if (m_currentIntersectionValid && intersectFaceId != 0xffffffff && m_firstStrokeFrame) {
                m_polyGroupTool.floodFillGroup(mesh, intersectFaceId, m_activeGroupId);
            }
            return;
        }
    }

    if (activeBrush == BRUSH_PAINT && getCurrentSettings().pickColor) {
        if (m_currentIntersectionValid && intersectFaceId != 0xffffffff) {
            pickColor(mesh, intersectFaceId, m_currentIntersection);
        }
        return;
    }

    glm::vec3 cameraPos = camera.computePosition();
    glm::vec3 worldIntersection = glm::vec3(mesh->matrix * glm::vec4(m_currentIntersection, 1.0f));
    float hitDepth = glm::distance(cameraPos, worldIntersection);

    float worldRadius = 0.0f;
    float brushRadius = getSettings(activeBrush).radius;
    if (camera.isOrthographic()) {
        worldRadius = brushRadius * 2.0f * camera.getOrthoZoom();
    } else {
        float fov_rad = camera.getFovDegrees() * (float)M_PI / 180.0f;
        float screenHeight = (float)camera.getHeight();
        if (screenHeight <= 0.0f) screenHeight = 1.0f;
        worldRadius = brushRadius * hitDepth * std::tan(fov_rad * 0.5f) * 2.0f / screenHeight;
    }

    glm::vec3 col0(mesh->matrix[0][0], mesh->matrix[0][1], mesh->matrix[0][2]);
    float scale = glm::length(col0);
    if (scale < 1e-12f) scale = 1.0f;

    float sizeMultiplier = 1.0f;
    if (g_tablet.isPressureSizeEnabled() && g_tablet.isPressureEnabled()) {
        sizeMultiplier = currentPressure;
    }
    float localRadius = (worldRadius / scale) * sizeMultiplier;
    float intensity = getSettings(activeBrush).intensity;
    if (g_tablet.isPressureEnabled() && activeBrush != BRUSH_MOVE && activeBrush != BRUSH_ELASTIC) {
        intensity *= currentPressure;
    }
    float radius2 = localRadius * localRadius;

    std::vector<uint32_t> pickedVertices;
    if (isGrabBrush && !m_firstStrokeFrame) {
        pickedVertices = m_grabbedVertices;
    } else {
        if (getSettings(activeBrush).topoCheck) {
            pickedVertices = pickVerticesInSphereTopological(mesh, m_currentIntersection, radius2, intersectFaceId);
        } else {
            pickedVertices = mesh->octree.pickVerticesInSphere(
                m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z, radius2, mesh->vertVisible.data()
            );
        }

        if (getSettings(activeBrush).culling) {
            filterCullingVertices(pickedVertices, mesh, localRayDir);
        }

        if (getSettings(activeBrush).singlePolyGroup && !mesh->faceGroups.empty()) {
            filterPolyGroupVertices(pickedVertices, mesh, m_strokeTargetPolyGroup);
        }

        if (m_useSym && !pickedVertices.empty()) {
            float eps = 1e-4f;
            std::vector<uint32_t> filteredPrimary;
            filteredPrimary.reserve(pickedVertices.size());
            const float* vertProxyData = mesh->vertProxy.data();
            for (uint32_t v : pickedVertices) {
                bool keep = true;
                if (m_symX) {
                    float currVal = m_currentIntersection.x;
                    float val = vertProxyData[v * 3 + 0];
                    if (currVal >= 0.0f ? (val < -eps) : (val > eps)) keep = false;
                }
                if (keep && m_symY) {
                    float currVal = m_currentIntersection.y;
                    float val = vertProxyData[v * 3 + 1];
                    if (currVal >= 0.0f ? (val < -eps) : (val > eps)) keep = false;
                }
                if (keep && m_symZ) {
                    float currVal = m_currentIntersection.z;
                    float val = vertProxyData[v * 3 + 2];
                    if (currVal >= 0.0f ? (val < -eps) : (val > eps)) keep = false;
                }
                if (keep) filteredPrimary.push_back(v);
            }
            pickedVertices = std::move(filteredPrimary);
        }

        if (isGrabBrush) {
            if (m_firstStrokeFrame || m_grabbedVertices.empty()) {
                m_grabbedVertices = pickedVertices;
            }
        }
    }

    std::vector<uint32_t> allAffectedVerts;

    if (!pickedVertices.empty()) {
        bool altPressed = (SDL_GetModState() & KMOD_ALT) != 0;
        bool negative = getSettings(activeBrush).negative ^ altPressed;

        // Cache area normal and center for Clay/Flatten/Brush tools on first frame
        if (m_firstStrokeFrame && (activeBrush == BRUSH_CLAY || activeBrush == BRUSH_CLAYBUILDUP || activeBrush == BRUSH_FLATTEN || activeBrush == BRUSH_SQUAREBRUSH || activeBrush == BRUSH_BRUSH)) {
            float areaResults[7] = {0.0f};
            if (computeAreaNormalAndCenter(
                mesh->verts.data(),
                mesh->normals.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                areaResults
            )) {
                m_cachedAreaNormal = glm::vec3(areaResults[0], areaResults[1], areaResults[2]);
                m_cachedAreaCenter = glm::vec3(areaResults[3], areaResults[4], areaResults[5]);
            } else {
                m_cachedAreaNormal = m_currentIntersectionNormal;
                m_cachedAreaCenter = m_currentIntersection;
            }
        }

        bool strokeAffectsColors = (activeBrush == BRUSH_PAINT && getCurrentSettings().writeAlbedo);
        bool strokeAffectsMaterials = (activeBrush == BRUSH_PAINT && (getCurrentSettings().writeRoughness || getCurrentSettings().writeMetalness)) ||
                                       (activeBrush == BRUSH_MASK || activeBrush == BRUSH_MASK_GRADIENT_BLUR);

        // Record initial vertices before modification for primary pass
        g_undoManager.recordAffectedVertices(scene, mesh->m_id, pickedVertices, strokeAffectsColors, strokeAffectsMaterials);

        // Primary pass
        int primaryDeformed = doStrokePass(
            scene,
            mesh,
            activeBrush,
            negative,
            pickedVertices,
            m_currentIntersection,
            m_currentIntersectionNormal,
            m_initialIntersection,
            m_cachedAreaNormal,
            m_cachedAreaCenter,
            localRadius,
            intensity,
            mouseX,
            false,
            glm::vec3(1.0f)
        );

        if (m_useSym && activeBrush != BRUSH_PAINT && activeBrush != BRUSH_MASK && activeBrush != BRUSH_POLYGROUP) {
            float eps = 1e-4f;
            const float* vertProxyData = mesh->vertProxy.data();
            for (uint32_t v : pickedVertices) {
                if (m_symX && std::abs(vertProxyData[v * 3 + 0]) <= eps) {
                    mesh->verts[v * 3 + 0] = vertProxyData[v * 3 + 0];
                }
                if (m_symY && std::abs(vertProxyData[v * 3 + 1]) <= eps) {
                    mesh->verts[v * 3 + 1] = vertProxyData[v * 3 + 1];
                }
                if (m_symZ && std::abs(vertProxyData[v * 3 + 2]) <= eps) {
                    mesh->verts[v * 3 + 2] = vertProxyData[v * 3 + 2];
                }
            }
        }

        if (primaryDeformed > 0) {
            allAffectedVerts.insert(allAffectedVerts.end(), pickedVertices.begin(), pickedVertices.begin() + primaryDeformed);
        }

        // Symmetry pass (Step 2)
        if (m_useSym) {
            std::vector<glm::vec3> symScales = getActiveSymmetryScales();

            if (m_firstStrokeFrame) {
                m_initialSymIntersections.resize(symScales.size());
                for (size_t sIdx = 0; sIdx < symScales.size(); ++sIdx) {
                    m_initialSymIntersections[sIdx] = m_initialIntersection * symScales[sIdx];
                }
                m_grabbedVerticesSyms.resize(symScales.size());
            }

            for (size_t sIdx = 0; sIdx < symScales.size(); ++sIdx) {
                glm::vec3 sScale = symScales[sIdx];
                glm::vec3 symCenter;
                std::vector<uint32_t> symVerts;

                if (isGrabBrush && !m_firstStrokeFrame) {
                    glm::vec3 symRayOrigin = localRayOrigin * sScale;
                    glm::vec3 symRayDir = localRayDir * sScale;
                    const glm::vec3& initSymInter = (sIdx < m_initialSymIntersections.size()) ? m_initialSymIntersections[sIdx] : (m_initialIntersection * sScale);
                    glm::vec3 camFrontLocal = -glm::normalize(glm::vec3(m_cachedInvMatrix * glm::vec4(glm::vec3(m_cachedCamWorldMatrix[2]), 0.0f)));
                    glm::vec3 symCamFrontLocal = camFrontLocal * sScale;
                    float denom = glm::dot(symCamFrontLocal, symRayDir);
                    if (std::abs(denom) > 1e-12f) {
                        float t = glm::dot(symCamFrontLocal, initSymInter - symRayOrigin) / denom;
                        symCenter = symRayOrigin + symRayDir * t;
                    } else {
                        symCenter = initSymInter;
                    }
                    if (sIdx < m_grabbedVerticesSyms.size()) {
                        symVerts = m_grabbedVerticesSyms[sIdx];
                    }
                } else {
                    symCenter = m_currentIntersection * sScale;

                    symVerts = mesh->octree.pickVerticesInSphere(
                        symCenter.x, symCenter.y, symCenter.z,
                        radius2, mesh->vertVisible.data()
                    );

                    if (getCurrentSettings().culling && !symVerts.empty()) {
                        glm::vec3 symRayDir = localRayDir * sScale;
                        filterCullingVertices(symVerts, mesh, symRayDir);
                    }

                    if (getCurrentSettings().singlePolyGroup && !mesh->faceGroups.empty() && !symVerts.empty()) {
                        filterPolyGroupVertices(symVerts, mesh, m_strokeTargetPolyGroup);
                    }

                    float eps = 1e-4f;
                    std::vector<uint32_t> filteredSym;
                    filteredSym.reserve(symVerts.size());
                    const float* vertProxyData = mesh->vertProxy.data();
                    for (uint32_t v : symVerts) {
                        bool keep = true;
                        if (sScale.x < 0.0f) {
                            float symVal = symCenter.x;
                            float val = vertProxyData[v * 3 + 0];
                            if (symVal >= 0.0f ? (val < -eps) : (val > eps)) keep = false;
                        }
                        if (keep && sScale.y < 0.0f) {
                            float symVal = symCenter.y;
                            float val = vertProxyData[v * 3 + 1];
                            if (symVal >= 0.0f ? (val < -eps) : (val > eps)) keep = false;
                        }
                        if (keep && sScale.z < 0.0f) {
                            float symVal = symCenter.z;
                            float val = vertProxyData[v * 3 + 2];
                            if (symVal >= 0.0f ? (val < -eps) : (val > eps)) keep = false;
                        }
                        if (keep) filteredSym.push_back(v);
                    }
                    symVerts = std::move(filteredSym);

                    if (isGrabBrush) {
                        if (sIdx >= m_grabbedVerticesSyms.size()) m_grabbedVerticesSyms.resize(sIdx + 1);
                        m_grabbedVerticesSyms[sIdx] = symVerts;
                    }
                }

                if (!symVerts.empty()) {
                    glm::vec3 symAreaNormal = m_cachedAreaNormal * sScale;
                    glm::vec3 symAreaCenter = m_cachedAreaCenter * sScale;
                    glm::vec3 symIntersectionNormal = m_currentIntersectionNormal * sScale;
                    const glm::vec3& initSymInter = (sIdx < m_initialSymIntersections.size()) ? m_initialSymIntersections[sIdx] : (m_initialIntersection * sScale);

                    g_undoManager.recordAffectedVertices(scene, mesh->m_id, symVerts, strokeAffectsColors, strokeAffectsMaterials);

                    int symDeformed = doStrokePass(
                        scene,
                        mesh,
                        activeBrush,
                        negative,
                        symVerts,
                        symCenter,
                        symIntersectionNormal,
                        initSymInter,
                        symAreaNormal,
                        symAreaCenter,
                        localRadius,
                        intensity,
                        mouseX,
                        true,
                        sScale
                    );

                    if (symDeformed > 0) {
                        allAffectedVerts.insert(allAffectedVerts.end(), symVerts.begin(), symVerts.begin() + symDeformed);
                    }
                }
            }
        }
    }

    if (!allAffectedVerts.empty()) {
        bool isSculptDeform = (activeBrush != BRUSH_PAINT && activeBrush != BRUSH_MASK && activeBrush != BRUSH_MASK_GRADIENT_BLUR && activeBrush != BRUSH_POLYGROUP);

        if (isSculptDeform) {
            if (m_tagFlags.size() < (size_t)mesh->nbFaces) {
                m_tagFlags.assign(mesh->nbFaces, 0);
            }
            if (m_iFacesCache.size() < (size_t)mesh->nbFaces) {
                m_iFacesCache.resize(mesh->nbFaces);
            }

            uint32_t numIFaces = getFacesFromVerticesFast(
                allAffectedVerts.data(),
                allAffectedVerts.size(),
                mesh->vrfStartCount.data(),
                mesh->vertRingFace.data(),
                m_iFacesCache.data(),
                m_tagFlags.data(),
                &m_tagEpoch,
                mesh->nbFaces
            );

            updateFaceNormalsAndBoxes(
                mesh->verts.data(), mesh->nbVerts,
                mesh->faces.data(), mesh->nbFaces,
                m_iFacesCache.data(), numIFaces,
                mesh->faceNormals.data(),
                mesh->faceBoxes.data(),
                mesh->faceCenters.data()
            );

            updateVertexNormals(
                allAffectedVerts.data(), allAffectedVerts.size(), mesh->nbVerts,
                mesh->vrfStartCount.data(),
                mesh->vertRingFace.data(),
                mesh->faceNormals.data(),
                mesh->normals.data()
            );

            mesh->octree.update(
                mesh->verts.data(), mesh->nbVerts,
                mesh->faces.data(), mesh->nbFaces,
                mesh->faceBoxes.data(),
                m_iFacesCache.data(), numIFaces
            );
        }

        uint32_t minV = allAffectedVerts[0];
        uint32_t maxV = allAffectedVerts[0];
        for (size_t i = 1; i < allAffectedVerts.size(); ++i) {
            uint32_t v = allAffectedVerts[i];
            if (v < minV) minV = v;
            if (v > maxV) maxV = v;
        }

        if (mesh->isVertexDirty || mesh->isColorDirty || mesh->isMaterialDirty) {
            mesh->dirtyVertMin = std::min(mesh->dirtyVertMin, minV);
            mesh->dirtyVertMax = std::max(mesh->dirtyVertMax, maxV);
        } else {
            mesh->dirtyVertMin = minV;
            mesh->dirtyVertMax = maxV;
        }

        const auto& settings = getCurrentSettings();
        if (activeBrush == BRUSH_PAINT) {
            if (settings.writeAlbedo) {
                mesh->isColorDirty = true;
            }
            if (settings.writeRoughness || settings.writeMetalness) {
                mesh->isMaterialDirty = true;
            }
        } else if (activeBrush == BRUSH_MASK || activeBrush == BRUSH_MASK_GRADIENT_BLUR) {
            mesh->isMaterialDirty = true;
        } else if (activeBrush == BRUSH_POLYGROUP) {
            mesh->isFaceGroupDirty = true;
        } else {
            mesh->isVertexDirty = true;
        }
    }

    m_alphaOrigin = m_currentIntersection;
    m_hasAlphaOrigin = true;
    m_firstStrokeFrame = false;
}

glm::vec3 SculptManager::getAnchorWorldPos(const MeasurementAnchor& anchor) {
    if (anchor.type == MeasurementAnchor::VERTEX) {
        Mesh* mesh = anchor.mesh;
        uint32_t vertIdx = anchor.vertIdx;
        glm::vec3 localPos(
            mesh->verts[vertIdx * 3],
            mesh->verts[vertIdx * 3 + 1],
            mesh->verts[vertIdx * 3 + 2]
        );
        return glm::vec3(mesh->matrix * glm::vec4(localPos, 1.0f));
    } else {
        return anchor.worldPos;
    }
}

MeasurementAnchor SculptManager::pickAnchor(float mouseX, float mouseY, Scene& scene, const glm::vec3* referenceWorldPos) {
    Mesh* mesh = scene.getSelected();
    Camera& camera = scene.getCamera();

    if (mesh) {
        Ray ray = camera.getRay(mouseX, mouseY);
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
            glm::vec3 inter = localRayOrigin + minT * localRayDir;
            float bestDist2 = std::numeric_limits<float>::infinity();
            uint32_t closestVert = 0xffffffff;

            uint32_t faceVerts[4] = {
                mesh->faces[intersectFaceId * 4],
                mesh->faces[intersectFaceId * 4 + 1],
                mesh->faces[intersectFaceId * 4 + 2],
                mesh->faces[intersectFaceId * 4 + 3]
            };

            for (int k = 0; k < 4; ++k) {
                uint32_t vid = faceVerts[k];
                if (vid == 0xffffffff) break;
                glm::vec3 v(mesh->verts[vid * 3], mesh->verts[vid * 3 + 1], mesh->verts[vid * 3 + 2]);
                float dist2 = glm::dot(v - inter, v - inter);
                if (dist2 < bestDist2) {
                    bestDist2 = dist2;
                    closestVert = vid;
                }
            }

            if (closestVert != 0xffffffff) {
                MeasurementAnchor anchor;
                anchor.type = MeasurementAnchor::VERTEX;
                anchor.mesh = mesh;
                anchor.vertIdx = closestVert;
                glm::vec3 localPos(mesh->verts[closestVert * 3], mesh->verts[closestVert * 3 + 1], mesh->verts[closestVert * 3 + 2]);
                anchor.worldPos = glm::vec3(mesh->matrix * glm::vec4(localPos, 1.0f));
                return anchor;
            }
        }
    }

    Ray ray = camera.getRay(mouseX, mouseY);
    float depth = 0.5f;
    if (referenceWorldPos) {
        glm::vec3 screenPivot = camera.project(*referenceWorldPos);
        depth = screenPivot.z;
    } else {
        glm::vec3 screenPivot = camera.project(camera.getPivot());
        depth = screenPivot.z;
    }
    glm::vec3 worldPos = camera.unproject(mouseX, mouseY, depth);

    MeasurementAnchor anchor;
    anchor.type = MeasurementAnchor::FREE;
    anchor.worldPos = worldPos;
    return anchor;
}

void SculptManager::validateSegments(Scene& scene) {
    const auto& meshes = scene.getMeshes();
    auto validate = [&](std::vector<MeasurementSegment>& segments) {
        segments.erase(std::remove_if(segments.begin(), segments.end(), [&](const MeasurementSegment& seg) {
            if (seg.vertA.type == MeasurementAnchor::VERTEX) {
                if (std::find(meshes.begin(), meshes.end(), seg.vertA.mesh) == meshes.end()) return true;
            }
            if (seg.vertB.type == MeasurementAnchor::VERTEX) {
                if (std::find(meshes.begin(), meshes.end(), seg.vertB.mesh) == meshes.end()) return true;
            }
            return false;
        }), segments.end());
    };
    validate(m_measureSegments);
    validate(m_dividerSegments);
}

void SculptManager::handleEvent(const SDL_Event& event, Scene& scene) {
    Mesh* mesh = scene.getSelected();
    int activeVp = scene.getActiveViewport();
    if (mesh && !mesh->isVisible(activeVp)) {
        mesh = nullptr;
    }
    Camera& camera = scene.getCamera();

    if (event.type == SDL_MOUSEBUTTONDOWN) {
        int mouseX = event.button.x;
        int mouseY = event.button.y;

        m_prevMouseX = mouseX;
        m_prevMouseY = mouseY;
        m_mouseDownX = mouseX;
        m_mouseDownY = mouseY;

        // Middle button and Right button are always camera controls
        if (event.button.button == SDL_BUTTON_MIDDLE || event.button.button == SDL_BUTTON_RIGHT) {
            m_cameraController.handleEvent(event, camera, scene.getMeshes());
            return;
        }

        // Left button:
        if (event.button.button == SDL_BUTTON_LEFT) {
            SDL_Keymod mod = SDL_GetModState();

            if ((mod & KMOD_ALT) && !(mod & KMOD_CTRL)) {
                float minT = std::numeric_limits<float>::infinity();
                Mesh* closestMesh = nullptr;
                uint32_t closestFaceId = 0xffffffff;
                float closestLocalMinT = std::numeric_limits<float>::infinity();
                glm::vec3 closestLocalRayOrigin{0.0f};
                glm::vec3 closestLocalRayDir{0.0f};

                Ray ray = camera.getRay((float)mouseX, (float)mouseY);

                for (Mesh* m : scene.getMeshes()) {
                    if (!m) continue;
                    if (!m->isVisible(activeVp)) continue;

                    glm::mat4 invMatrix = glm::inverse(m->matrix);
                    glm::vec3 localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
                    glm::vec3 localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

                    std::vector<uint32_t> candidateFaces = m->octree.collectIntersectRay(
                        localRayOrigin.x, localRayOrigin.y, localRayOrigin.z,
                        localRayDir.x, localRayDir.y, localRayDir.z
                    );

                    float localMinT = std::numeric_limits<float>::infinity();
                    uint32_t localFaceId = 0xffffffff;

                    for (uint32_t faceId : candidateFaces) {
                        if (faceId >= (uint32_t)m->nbFaces) continue;
                        uint32_t v0Id = m->faces[faceId * 4];
                        uint32_t v1Id = m->faces[faceId * 4 + 1];
                        uint32_t v2Id = m->faces[faceId * 4 + 2];
                        uint32_t v3Id = m->faces[faceId * 4 + 3];

                        if (!m->vertVisible[v0Id] || !m->vertVisible[v1Id] || !m->vertVisible[v2Id] || 
                            (v3Id != 0xffffffff && !m->vertVisible[v3Id])) {
                            continue;
                        }

                        glm::vec3 v0(m->verts[v0Id * 3], m->verts[v0Id * 3 + 1], m->verts[v0Id * 3 + 2]);
                        glm::vec3 v1(m->verts[v1Id * 3], m->verts[v1Id * 3 + 1], m->verts[v1Id * 3 + 2]);
                        glm::vec3 v2(m->verts[v2Id * 3], m->verts[v2Id * 3 + 1], m->verts[v2Id * 3 + 2]);

                        float t;
                        if (rayTriangleIntersect(localRayOrigin, localRayDir, v0, v1, v2, t)) {
                            if (t < localMinT) {
                                localMinT = t;
                                localFaceId = faceId;
                            }
                        }

                        if (v3Id != 0xffffffff) {
                            glm::vec3 v3(m->verts[v3Id * 3], m->verts[v3Id * 3 + 1], m->verts[v3Id * 3 + 2]);
                            if (rayTriangleIntersect(localRayOrigin, localRayDir, v0, v2, v3, t)) {
                                if (t < localMinT) {
                                    localMinT = t;
                                    localFaceId = faceId;
                                }
                            }
                        }
                    }

                    if (localFaceId != 0xffffffff) {
                        glm::vec3 worldHit = glm::vec3(m->matrix * glm::vec4(localRayOrigin + localMinT * localRayDir, 1.0f));
                        float worldT = glm::distance(ray.origin, worldHit);
                        if (worldT < minT) {
                            minT = worldT;
                            closestMesh = m;
                            closestFaceId = localFaceId;
                            closestLocalMinT = localMinT;
                            closestLocalRayOrigin = localRayOrigin;
                            closestLocalRayDir = localRayDir;
                        }
                    }
                }

                if (closestMesh) {
                    if (closestMesh != scene.getSelected()) {
                        scene.setOrUnsetMesh(closestMesh, false);
                        return;
                    }

                    if (m_currentBrush == BRUSH_TRANSFORM) {
                        glm::vec3 localPt = closestLocalRayOrigin + closestLocalMinT * closestLocalRayDir;
                        glm::vec3 localNormal(0.0f, 0.0f, 1.0f);
                        if (closestFaceId < (uint32_t)closestMesh->nbFaces && !closestMesh->faceNormals.empty()) {
                            localNormal = glm::vec3(
                                closestMesh->faceNormals[closestFaceId * 3],
                                closestMesh->faceNormals[closestFaceId * 3 + 1],
                                closestMesh->faceNormals[closestFaceId * 3 + 2]
                            );
                        }
                        if (glm::length(localNormal) < 1e-6f) {
                            uint32_t v0Id = closestMesh->faces[closestFaceId * 4];
                            uint32_t v1Id = closestMesh->faces[closestFaceId * 4 + 1];
                            uint32_t v2Id = closestMesh->faces[closestFaceId * 4 + 2];
                            glm::vec3 v0(closestMesh->verts[v0Id * 3], closestMesh->verts[v0Id * 3 + 1], closestMesh->verts[v0Id * 3 + 2]);
                            glm::vec3 v1(closestMesh->verts[v1Id * 3], closestMesh->verts[v1Id * 3 + 1], closestMesh->verts[v1Id * 3 + 2]);
                            glm::vec3 v2(closestMesh->verts[v2Id * 3], closestMesh->verts[v2Id * 3 + 1], closestMesh->verts[v2Id * 3 + 2]);
                            localNormal = glm::cross(v1 - v0, v2 - v0);
                        }
                        if (glm::length(localNormal) > 1e-6f) {
                            localNormal = glm::normalize(localNormal);
                        } else {
                            localNormal = glm::vec3(0.0f, 0.0f, 1.0f);
                        }

                        glm::vec3 worldPt = glm::vec3(closestMesh->matrix * glm::vec4(localPt, 1.0f));
                        glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(closestMesh->matrix)));
                        glm::vec3 worldNormal = glm::normalize(normalMatrix * localNormal);
                        if (glm::dot(worldNormal, ray.dir) > 0.0f) {
                            worldNormal = -worldNormal;
                        }

                        float sx = glm::length(glm::vec3(closestMesh->matrix[0]));
                        float sy = glm::length(glm::vec3(closestMesh->matrix[1]));
                        float sz = glm::length(glm::vec3(closestMesh->matrix[2]));
                        if (sx < 1e-6f) sx = 1.0f;
                        if (sy < 1e-6f) sy = 1.0f;
                        if (sz < 1e-6f) sz = 1.0f;

                        glm::vec3 zAxis = worldNormal;
                        glm::vec3 up = (std::abs(zAxis.y) < 0.999f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
                        glm::vec3 xAxis = glm::normalize(glm::cross(up, zAxis));
                        glm::vec3 yAxis = glm::cross(zAxis, xAxis);

                        glm::mat4 targetMatrix(1.0f);
                        targetMatrix[0] = glm::vec4(xAxis * sx, 0.0f);
                        targetMatrix[1] = glm::vec4(yAxis * sy, 0.0f);
                        targetMatrix[2] = glm::vec4(zAxis * sz, 0.0f);
                        targetMatrix[3] = glm::vec4(worldPt, 1.0f);

                        scene.pushHistoryState();

                        glm::mat4 deltaLocalMatrix = glm::inverse(closestMesh->matrix) * targetMatrix;
                        glm::mat4 deltaLocalMatrixInv = glm::inverse(deltaLocalMatrix);
                        glm::mat3 enMatrix = glm::transpose(glm::inverse(glm::mat3(deltaLocalMatrixInv)));

                        for (int i = 0; i < closestMesh->nbVerts; ++i) {
                            glm::vec4 pos(closestMesh->verts[i * 3], closestMesh->verts[i * 3 + 1], closestMesh->verts[i * 3 + 2], 1.0f);
                            glm::vec4 newPos = deltaLocalMatrixInv * pos;
                            closestMesh->verts[i * 3]     = newPos.x;
                            closestMesh->verts[i * 3 + 1] = newPos.y;
                            closestMesh->verts[i * 3 + 2] = newPos.z;

                            glm::vec3 normal(closestMesh->normals[i * 3], closestMesh->normals[i * 3 + 1], closestMesh->normals[i * 3 + 2]);
                            glm::vec3 newNormal = glm::normalize(enMatrix * normal);
                            closestMesh->normals[i * 3]     = newNormal.x;
                            closestMesh->normals[i * 3 + 1] = newNormal.y;
                            closestMesh->normals[i * 3 + 2] = newNormal.z;
                        }

                        closestMesh->matrix = targetMatrix;
                        closestMesh->postInit();
                        closestMesh->isDirty = true;
                        return;
                    }
                } else {
                    if (m_currentBrush == BRUSH_TRANSFORM) {
                        return;
                    }
                }
            }

            if (m_currentBrush == BRUSH_MEASURE || m_currentBrush == BRUSH_DIVIDER) {
                if (mod & KMOD_ALT) {
                    return;
                }

                float mouseX = (float)event.button.x;
                float mouseY = (float)event.button.y;
                float threshold = 15.0f;

                auto& segments = (m_currentBrush == BRUSH_MEASURE) ? m_measureSegments : m_dividerSegments;

                m_draggedSegment = nullptr;
                m_draggedVertexKey = "";

                // Check for dragging existing segment endpoint
                for (auto& seg : segments) {
                    glm::vec3 worldA = getAnchorWorldPos(seg.vertA);
                    glm::vec3 worldB = getAnchorWorldPos(seg.vertB);
                    glm::vec3 screenA = camera.project(worldA);
                    glm::vec3 screenB = camera.project(worldB);

                    float distA = glm::distance(glm::vec2(mouseX, mouseY), glm::vec2(screenA.x, screenA.y));
                    float distB = glm::distance(glm::vec2(mouseX, mouseY), glm::vec2(screenB.x, screenB.y));

                    if (distA < threshold && distA <= distB) {
                        m_draggedSegment = &seg;
                        m_draggedVertexKey = "vertA";
                        scene.pushHistoryState();
                        return;
                    }
                    if (distB < threshold) {
                        m_draggedSegment = &seg;
                        m_draggedVertexKey = "vertB";
                        scene.pushHistoryState();
                        return;
                    }
                }

                // If not dragging, start a new segment
                MeasurementAnchor anchor = pickAnchor(mouseX, mouseY, scene, nullptr);
                m_pendingAnchorA = anchor;
                m_pendingAnchorB = anchor;
                m_hasPending = true;
                scene.pushHistoryState();
                return;
            }

            if (m_currentBrush == BRUSH_TRANSFORM) {
                if (mod & KMOD_ALT) {
                    return;
                }
                return;
            }

            if (m_currentBrush == BRUSH_ARMATURE_SPHERES) {
                if (m_armatureTool->start(scene, camera, (float)mouseX, (float)mouseY, mod & KMOD_CTRL, mod & KMOD_ALT)) {
                    // Handled
                }
                return; // Return anyway, we don't want standard sculpting
            }

            bool isVisibilityTool = (m_currentBrush == BRUSH_VISIBILITY);
            bool isLassoMode = (mod & KMOD_CTRL) && (mod & KMOD_SHIFT);

            if (isVisibilityTool || isLassoMode) {
                m_isLassoActive = true;
                m_isMaskLasso = false;
                m_lassoPoints.clear();
                m_lassoPoints.push_back(glm::vec2((float)mouseX, (float)mouseY));
                m_lassoAlt = (mod & KMOD_ALT) != 0;
                return;
            }

            if (m_currentBrush == BRUSH_MASK_GRADIENT_BLUR && mesh) {
                if (mod & KMOD_ALT) {
                    return;
                }
                glm::vec2 mousePos((float)mouseX, (float)mouseY);
                bool interactPoint = false;
                if (m_gradActive) {
                    float distA = glm::distance(mousePos, m_gradPointA);
                    float distB = glm::distance(mousePos, m_gradPointB);
                    if (distA < 20.0f) {
                        m_gradActivePoint = 'A';
                        interactPoint = true;
                    } else if (distB < 20.0f) {
                        m_gradActivePoint = 'B';
                        interactPoint = true;
                    }
                }

                if (interactPoint) {
                    scene.pushHistoryState();
                    m_gradIsDrawing = false;
                } else {
                    scene.pushHistoryState();
                    m_gradPointA = mousePos;
                    m_gradPointB = mousePos;
                    m_gradActivePoint = 'B';
                    m_gradIsDrawing = true;
                    m_gradActive = true;
                }

                int nbVerts = mesh->nbVerts;
                m_origMasks.resize(nbVerts);
                std::vector<float> tempMasks(nbVerts);
                for (int i = 0; i < nbVerts; ++i) {
                    m_origMasks[i] = mesh->materials[i * 3 + 2];
                    tempMasks[i] = mesh->materials[i * 3 + 2];
                }

                std::vector<uint32_t> iVerts;
                iVerts.reserve(nbVerts);
                for (int i = 0; i < nbVerts; ++i) {
                    if (tempMasks[i] < 1.0f) {
                        iVerts.push_back(i);
                    }
                }

                if (!iVerts.empty()) {
                    int iterations = getCurrentSettings().maskSharpenBlurIterations;
                    std::vector<uint8_t> visited(nbVerts, 0);
                    for (uint32_t v : iVerts) visited[v] = 1;
                    std::vector<uint32_t> queue = iVerts;
                    size_t head = 0;
                    for (int step = 0; step < iterations; ++step) {
                        size_t size = queue.size();
                        while (head < size) {
                            uint32_t id = queue[head++];
                            uint32_t start = mesh->vrvStartCount[id * 2];
                            uint32_t count = mesh->vrvStartCount[id * 2 + 1];
                            for (uint32_t j = start; j < start + count; ++j) {
                                uint32_t neighbor = mesh->vertRingVert[j];
                                if (!visited[neighbor]) {
                                    visited[neighbor] = 1;
                                    queue.push_back(neighbor);
                                }
                            }
                        }
                    }
                    iVerts = queue;
                    ::blurMask(
                        iVerts.data(), (int)iVerts.size(),
                        mesh->vrvStartCount.data(),
                        mesh->vertRingVert.data(),
                        mesh->vertOnEdge.data(),
                        iterations,
                        tempMasks.data()
                    );
                }
                m_blurredMasks = tempMasks;
                m_gradActiveVerts = iVerts;
                return;
            }

            // Perform picking intersection test to see if we hit the mesh
            bool hitMesh = false;
            float minT = std::numeric_limits<float>::infinity();
            uint32_t intersectFaceId = 0xffffffff;
            glm::vec3 localRayOrigin{0.0f};
            glm::vec3 localRayDir{0.0f};

            if (mesh) {
                Ray ray = camera.getRay((float)mouseX, (float)mouseY);
                glm::mat4 invMatrix = glm::inverse(mesh->matrix);
                localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
                localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

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
                }
            }

            // Dynamic tool activation:
            // If Ctrl is held and click starts on empty space (no mesh intersection), trigger Masking Lasso
            if ((mod & KMOD_CTRL) && !hitMesh) {
                m_isLassoActive = true;
                m_isMaskLasso = true;
                m_lassoPoints.clear();
                m_lassoPoints.push_back(glm::vec2((float)mouseX, (float)mouseY));
                m_lassoAlt = (mod & KMOD_ALT) != 0;
                return;
            }

            if (hitMesh) {
                bool strokeAffectsColors = (m_currentBrush == BRUSH_PAINT && getCurrentSettings().writeAlbedo);
                bool strokeAffectsMaterials = (m_currentBrush == BRUSH_PAINT && (getCurrentSettings().writeRoughness || getCurrentSettings().writeMetalness)) ||
                                               (m_currentBrush == BRUSH_MASK || m_currentBrush == BRUSH_MASK_GRADIENT_BLUR);
                g_undoManager.beginSculptStroke(scene, mesh->m_id, {}, strokeAffectsColors, strokeAffectsMaterials, "Sculpt Stroke");

                m_isSculpting = true;
                m_currentIntersectionValid = true;
                m_firstStrokeFrame = true;
                m_hasAlphaOrigin = false;
                m_initialIntersection = localRayOrigin + minT * localRayDir;
                m_initialIntersectionNormal = glm::vec3(
                    mesh->faceNormals[intersectFaceId * 3],
                    mesh->faceNormals[intersectFaceId * 3 + 1],
                    mesh->faceNormals[intersectFaceId * 3 + 2]
                );
                m_currentIntersection = m_initialIntersection;
                m_currentIntersectionNormal = m_initialIntersectionNormal;

                std::vector<glm::vec3> symScales = getActiveSymmetryScales();
                m_initialSymIntersections.resize(symScales.size());
                for (size_t sIdx = 0; sIdx < symScales.size(); ++sIdx) {
                    m_initialSymIntersections[sIdx] = m_initialIntersection * symScales[sIdx];
                }

                m_lastStrokeX = mouseX;
                m_lastStrokeY = mouseY;

                // Copy vertices to proxy at start of stroke
                mesh->vertProxy = mesh->verts;

                // Check stylus/tablet pressure
                float currentPressure = 1.0f;
#ifdef _WIN32
                if (g_tablet.isAvailable() && g_tablet.isPenActive()) {
                    currentPressure = g_tablet.getPressure();
                } else {
                    if (m_usingStylus && (SDL_GetTicks() - m_lastStylusTime > 1000)) {
                        m_usingStylus = false;
                        m_stylusPressure = 1.0f;
                    }
                    currentPressure = m_usingStylus ? m_stylusPressure : 1.0f;
                }
#else
                if (m_usingStylus && (SDL_GetTicks() - m_lastStylusTime > 1000)) {
                    m_usingStylus = false;
                    m_stylusPressure = 1.0f;
                }
                currentPressure = m_usingStylus ? m_stylusPressure : 1.0f;
#endif

                // Execute first stroke frame immediately
                executeStroke(scene, mesh, camera, (float)mouseX, (float)mouseY, currentPressure);
            }
        }
    } else if (event.type == SDL_MOUSEBUTTONUP) {
        if (event.button.button == SDL_BUTTON_MIDDLE || event.button.button == SDL_BUTTON_RIGHT) {
            m_cameraController.handleEvent(event, camera, scene.getMeshes());
            return;
        }

        if ((m_currentBrush == BRUSH_MEASURE || m_currentBrush == BRUSH_DIVIDER) && event.button.button == SDL_BUTTON_LEFT) {
            float mouseX = (float)event.button.x;
            float mouseY = (float)event.button.y;

            auto& segments = (m_currentBrush == BRUSH_MEASURE) ? m_measureSegments : m_dividerSegments;

            if (m_draggedSegment) {
                glm::vec3 posA = getAnchorWorldPos(m_draggedSegment->vertA);
                glm::vec3 posB = getAnchorWorldPos(m_draggedSegment->vertB);
                if (glm::distance(posA, posB) < 1e-4f) {
                    segments.erase(std::remove_if(segments.begin(), segments.end(),
                        [&](const MeasurementSegment& seg) { return &seg == m_draggedSegment; }),
                        segments.end());
                }
                m_draggedSegment = nullptr;
                m_draggedVertexKey = "";

                if (m_currentBrush == BRUSH_MEASURE) {
                    bool hasReference = false;
                    for (const auto& seg : segments) {
                        if (seg.isReference) { hasReference = true; break; }
                    }
                    if (!hasReference && !segments.empty()) {
                        segments[0].isReference = true;
                    }
                }
                return;
            }

            if (m_hasPending) {
                glm::vec3 posA = getAnchorWorldPos(m_pendingAnchorA);
                glm::vec3 posB = getAnchorWorldPos(m_pendingAnchorB);
                if (glm::distance(posA, posB) > 1e-4f) {
                    MeasurementSegment newSeg;
                    newSeg.vertA = m_pendingAnchorA;
                    newSeg.vertB = m_pendingAnchorB;
                    newSeg.isReference = false;

                    if (m_currentBrush == BRUSH_MEASURE) {
                        bool hasReference = false;
                        for (const auto& seg : segments) {
                            if (seg.isReference) { hasReference = true; break; }
                        }
                        newSeg.isReference = !hasReference;
                    }

                    segments.push_back(newSeg);
                }
                m_hasPending = false;
                return;
            }
            return;
        }

        if (m_currentBrush == BRUSH_TRANSFORM) {
            m_cameraController.handleEvent(event, camera, scene.getMeshes());
            return;
        }

        if (m_currentBrush == BRUSH_ARMATURE_SPHERES) {
            if (event.button.button == SDL_BUTTON_LEFT) {
                m_armatureTool->end(scene);
            }
            return;
        }

        if (m_currentBrush == BRUSH_MASK_GRADIENT_BLUR && mesh && event.button.button == SDL_BUTTON_LEFT) {
            if (m_gradIsDrawing) {
                float dist = glm::distance(m_gradPointA, m_gradPointB);
                if (dist < 5.0f) {
                    m_gradPointA = glm::vec2(m_mouseDownX - 75.0f, (float)m_mouseDownY);
                    m_gradPointB = glm::vec2(m_mouseDownX + 75.0f, (float)m_mouseDownY);

                    glm::mat4 mvp = camera.getProjMatrix() * camera.getViewMatrix() * mesh->matrix;
                    float width = (float)camera.getWidth();
                    float height = (float)camera.getHeight();
                    float localToScreen[16] = {0.0f};

                    localToScreen[0] = (mvp[0][0] + mvp[0][3]) * 0.5f * width;
                    localToScreen[4] = (mvp[1][0] + mvp[1][3]) * 0.5f * width;
                    localToScreen[8] = (mvp[2][0] + mvp[2][3]) * 0.5f * width;
                    localToScreen[12] = (mvp[3][0] + mvp[3][3]) * 0.5f * width;

                    localToScreen[1] = (mvp[0][1] + mvp[0][3]) * 0.5f * height;
                    localToScreen[5] = (mvp[1][1] + mvp[1][3]) * 0.5f * height;
                    localToScreen[9] = (mvp[2][1] + mvp[2][3]) * 0.5f * height;
                    localToScreen[13] = (mvp[3][1] + mvp[3][3]) * 0.5f * height;

                    localToScreen[3] = mvp[0][3];
                    localToScreen[7] = mvp[1][3];
                    localToScreen[11] = mvp[2][3];
                    localToScreen[15] = mvp[3][3];

                    float ptPlaneX = 0.0f, ptPlaneY = 0.0f, ptPlaneZ = 0.0f;
                    float nPlaneX = m_symX ? 1.0f : 0.0f;
                    float nPlaneY = m_symY ? 1.0f : 0.0f;
                    float nPlaneZ = m_symZ ? 1.0f : 0.0f;

                    ::applyGradientMask(
                        mesh->verts.data(),
                        mesh->materials.data(),
                        m_gradActiveVerts.data(),
                        (int)m_gradActiveVerts.size(),
                        m_origMasks.data(),
                        m_blurredMasks.data(),
                        localToScreen,
                        height,
                        m_gradPointA.x, m_gradPointA.y,
                        m_gradPointB.x, m_gradPointB.y,
                        m_useSym,
                        ptPlaneX, ptPlaneY, ptPlaneZ,
                        nPlaneX, nPlaneY, nPlaneZ,
                        getCurrentSettings().blurMaskedOnly,
                        mesh->nbVerts
                    );

                    mesh->isVertexDirty = true;
                    mesh->dirtyVertMin = 0;
                    mesh->dirtyVertMax = mesh->nbVerts - 1;
                    mesh->isDirty = true;
                }
                m_gradIsDrawing = false;
                m_gradActivePoint = '\0';
                return;
            } else if (m_gradActivePoint != '\0') {
                m_gradActivePoint = '\0';
                return;
            }
        }

        if (m_isLassoActive) {
            m_isLassoActive = false;

            float maxDragDistSq = 0.0f;
            glm::vec2 startPt((float)m_mouseDownX, (float)m_mouseDownY);
            for (const auto& pt : m_lassoPoints) {
                glm::vec2 diff = pt - startPt;
                float d2 = glm::dot(diff, diff);
                if (d2 > maxDragDistSq) maxDragDistSq = d2;
            }
            float maxDragDist = std::sqrt(maxDragDistSq);

            const float LASSO_DRAG_THRESHOLD = 8.0f; // Threshold in pixels to distinguish click vs lasso drag
            bool isRealDrag = (m_lassoPoints.size() >= 3) && (maxDragDist >= LASSO_DRAG_THRESHOLD);

            if (isRealDrag && mesh) {
                std::vector<uint32_t> selectedVertices = getVerticesInLasso(mesh, camera);
                if (m_isMaskLasso) {
                    scene.pushHistoryState();
                    if (!selectedVertices.empty()) {
                        float maskVal = m_lassoAlt ? 1.0f : 0.0f;
                        float* materials = mesh->materials.data();
                        for (uint32_t vid : selectedVertices) {
                            materials[vid * 3 + 2] = maskVal;
                        }
                        mesh->isMaterialDirty = true;
                        mesh->dirtyVertMin = 0;
                        mesh->dirtyVertMax = mesh->nbVerts - 1;
                    } else {
                        clearMask(mesh);
                    }
                    mesh->isDirty = true;
                } else {
                    scene.pushHistoryState();
                    if (mesh->faceVisible.size() != (size_t)mesh->nbFaces) {
                        mesh->faceVisible.assign(mesh->nbFaces, 1);
                    }

                    if (!selectedVertices.empty()) {
                        std::vector<uint8_t> isVertSel(mesh->nbVerts, 0);
                        for (uint32_t vid : selectedVertices) {
                            if (vid < (uint32_t)mesh->nbVerts) isVertSel[vid] = 1;
                        }

                        bool hideUnselected = !m_lassoAlt;
                        for (int f = 0; f < mesh->nbFaces; ++f) {
                            uint32_t v0 = mesh->faces[f * 4];
                            uint32_t v1 = mesh->faces[f * 4 + 1];
                            uint32_t v2 = mesh->faces[f * 4 + 2];
                            uint32_t v3 = mesh->faces[f * 4 + 3];

                            bool fSel = isVertSel[v0] && isVertSel[v1] && isVertSel[v2] &&
                                        (v3 == 0xffffffff || isVertSel[v3]);

                            if (hideUnselected) {
                                // Isolate lasso selection: only show faces fully selected
                                mesh->faceVisible[f] = fSel ? 1 : 0;
                            } else {
                                // Hide lasso selection: hide faces fully selected
                                if (fSel) mesh->faceVisible[f] = 0;
                            }
                        }

                        // Synchronize vertVisible from faceVisible
                        std::fill(mesh->vertVisible.begin(), mesh->vertVisible.end(), 0);
                        for (int f = 0; f < mesh->nbFaces; ++f) {
                            if (mesh->faceVisible[f]) {
                                uint32_t v0 = mesh->faces[f * 4];
                                uint32_t v1 = mesh->faces[f * 4 + 1];
                                uint32_t v2 = mesh->faces[f * 4 + 2];
                                uint32_t v3 = mesh->faces[f * 4 + 3];

                                if (v0 < mesh->vertVisible.size()) mesh->vertVisible[v0] = 1;
                                if (v1 < mesh->vertVisible.size()) mesh->vertVisible[v1] = 1;
                                if (v2 < mesh->vertVisible.size()) mesh->vertVisible[v2] = 1;
                                if (v3 != 0xffffffff && v3 < mesh->vertVisible.size()) mesh->vertVisible[v3] = 1;
                            }
                        }
                    } else {
                        // Ctrl + Shift + Click Drag on empty space -> clear hiding (show all)
                        std::fill(mesh->faceVisible.begin(), mesh->faceVisible.end(), 1);
                        std::fill(mesh->vertVisible.begin(), mesh->vertVisible.end(), 1);
                    }
                    mesh->isDirty = true;
                    mesh->isFaceGroupDirty = true;
                }
            } else {
                // Click action (or micro-drag gesture under threshold)
                Ray ray = camera.getRay((float)event.button.x, (float)event.button.y);
                bool hitMesh = false;
                uint32_t closestVert = 0xffffffff;
                uint32_t intersectFaceId = 0xffffffff;
                float bestMask = 1.0f;

                if (mesh) {
                    glm::mat4 invMatrix = glm::inverse(mesh->matrix);
                    glm::vec3 localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
                    glm::vec3 localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

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

                        if (mesh->faceVisible.size() == (size_t)mesh->nbFaces && !mesh->faceVisible[faceId]) {
                            continue;
                        }

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
                        glm::vec3 inter = localRayOrigin + minT * localRayDir;
                        float bestDist2 = std::numeric_limits<float>::infinity();

                        uint32_t faceVerts[4] = {
                            mesh->faces[intersectFaceId * 4],
                            mesh->faces[intersectFaceId * 4 + 1],
                            mesh->faces[intersectFaceId * 4 + 2],
                            mesh->faces[intersectFaceId * 4 + 3]
                        };

                        for (int k = 0; k < 4; ++k) {
                            uint32_t vid = faceVerts[k];
                            if (vid == 0xffffffff) break;
                            glm::vec3 v(mesh->verts[vid * 3], mesh->verts[vid * 3 + 1], mesh->verts[vid * 3 + 2]);
                            float dist2 = glm::dot(v - inter, v - inter);
                            if (dist2 < bestDist2) {
                                bestDist2 = dist2;
                                closestVert = vid;
                                bestMask = mesh->materials[vid * 3 + 2];
                            }
                        }
                    }
                }

                if (m_isMaskLasso) {
                    if (!hitMesh && mesh) {
                        std::cout << "[MaskClick] Canvas Ctrl+Click detected (maxDragDist=" << maxDragDist << "px). Inverting mask..." << std::endl;
                        scene.pushHistoryState();
                        invertMask(mesh);
                    } else if (hitMesh && mesh) {
                        SDL_Keymod mod = SDL_GetModState();
                        bool ctrlKey = (mod & KMOD_CTRL) != 0;
                        bool altKey = (mod & KMOD_ALT) != 0;

                        scene.pushHistoryState();
                        if (ctrlKey && altKey) {
                            sharpenMask(mesh);
                        } else {
                            blurMask(mesh);
                        }
                    }
                } else {
                    if (!hitMesh && mesh) {
                        std::cout << "[VisibilityClick] Canvas Ctrl+Shift+Click detected (maxDragDist=" << maxDragDist << "px). Inverting visibility..." << std::endl;
                        scene.pushHistoryState();
                        if (mesh->faceVisible.size() != (size_t)mesh->nbFaces) {
                            mesh->faceVisible.assign(mesh->nbFaces, 1);
                        }

                        // Invert face visibility (0 -> 1, 1 -> 0)
                        for (size_t f = 0; f < mesh->faceVisible.size(); ++f) {
                            mesh->faceVisible[f] = mesh->faceVisible[f] ? 0 : 1;
                        }

                        // Synchronize vertVisible from faceVisible:
                        // Mark vertVisible = 1 for all vertices that belong to ANY visible face
                        std::vector<uint8_t> newVertVisible(mesh->nbVerts, 0);
                        for (int f = 0; f < mesh->nbFaces; ++f) {
                            if (mesh->faceVisible[f]) {
                                uint32_t v0 = mesh->faces[f * 4];
                                uint32_t v1 = mesh->faces[f * 4 + 1];
                                uint32_t v2 = mesh->faces[f * 4 + 2];
                                uint32_t v3 = mesh->faces[f * 4 + 3];

                                if (v0 < newVertVisible.size()) newVertVisible[v0] = 1;
                                if (v1 < newVertVisible.size()) newVertVisible[v1] = 1;
                                if (v2 < newVertVisible.size()) newVertVisible[v2] = 1;
                                if (v3 != 0xffffffff && v3 < newVertVisible.size()) newVertVisible[v3] = 1;
                            }
                        }

                        mesh->vertVisible = std::move(newVertVisible);
                        mesh->isDirty = true;
                        mesh->isFaceGroupDirty = true;
                    } else if (hitMesh && mesh && intersectFaceId != 0xffffffff) {
                        bool hasPolyGroups = (mesh && mesh->faceGroups.size() == (size_t)mesh->nbFaces);
                        if (hasPolyGroups) {
                            uint32_t clickedGroupId = m_polyGroupTool.getGroupAtFace(mesh, intersectFaceId);
                            SDL_Keymod mod = SDL_GetModState();
                            bool altPressed = m_lassoAlt || ((mod & KMOD_ALT) != 0);

                            scene.pushHistoryState();

                            if (mesh->faceVisible.size() != (size_t)mesh->nbFaces) {
                                mesh->faceVisible.assign(mesh->nbFaces, 1);
                            }

                            if (!altPressed) {
                                // Ctrl + Shift + Click: isolate clicked polygroup (show ONLY clickedGroupId)
                                std::cout << "[VisibilityClick] Ctrl+Shift+Click on PolyGroup " << clickedGroupId << ". Isolating polygroup..." << std::endl;
                                for (int f = 0; f < mesh->nbFaces; ++f) {
                                    uint32_t gid = (mesh->faceGroups.size() > (size_t)f) ? mesh->faceGroups[f] : 0;
                                    mesh->faceVisible[f] = (gid == clickedGroupId) ? 1 : 0;
                                }
                            } else {
                                // Ctrl + Shift + Alt + Click: hide clicked polygroup (hide clickedGroupId, keeping other currently visible faces)
                                std::cout << "[VisibilityClick] Ctrl+Shift+Alt+Click on PolyGroup " << clickedGroupId << ". Hiding polygroup..." << std::endl;
                                for (int f = 0; f < mesh->nbFaces; ++f) {
                                    uint32_t gid = (mesh->faceGroups.size() > (size_t)f) ? mesh->faceGroups[f] : 0;
                                    if (gid == clickedGroupId) {
                                        mesh->faceVisible[f] = 0;
                                    }
                                }
                            }

                            // Synchronize vertVisible from faceVisible:
                            // Mark vertVisible = 1 for all vertices that belong to ANY visible face
                            std::vector<uint8_t> newVertVisible(mesh->nbVerts, 0);
                            for (int f = 0; f < mesh->nbFaces; ++f) {
                                if (mesh->faceVisible[f]) {
                                    uint32_t v0 = mesh->faces[f * 4];
                                    uint32_t v1 = mesh->faces[f * 4 + 1];
                                    uint32_t v2 = mesh->faces[f * 4 + 2];
                                    uint32_t v3 = mesh->faces[f * 4 + 3];

                                    if (v0 < newVertVisible.size()) newVertVisible[v0] = 1;
                                    if (v1 < newVertVisible.size()) newVertVisible[v1] = 1;
                                    if (v2 < newVertVisible.size()) newVertVisible[v2] = 1;
                                    if (v3 != 0xffffffff && v3 < newVertVisible.size()) newVertVisible[v3] = 1;
                                }
                            }

                            mesh->vertVisible = newVertVisible;
                            mesh->isDirty = true;
                            mesh->isFaceGroupDirty = true;
                        }
                    }
                }
            }
            m_lassoPoints.clear();
            m_isMaskLasso = false;
            return;
        }

        if (m_isSculpting) {
            m_isSculpting = false;
            m_currentIntersectionValid = false;
            m_hasAnyValidIntersection = false;
            int dragDistX = std::abs(event.button.x - m_mouseDownX);
            int dragDistY = std::abs(event.button.y - m_mouseDownY);
            bool wasClick = (dragDistX <= 12 && dragDistY <= 12);

            if (wasClick && (m_currentBrush == BRUSH_MASK || (SDL_GetModState() & KMOD_CTRL)) && mesh) {
                sculpt_log("[MaskClick] Ctrl+Click mask action detected. Canceling active click stroke...\n");
                g_undoManager.cancelSculptStroke();

                // Mesh pointer stays valid when using delta UndoManager, but re-get just in case
                mesh = scene.getSelected();
                if (!mesh) {
                    sculpt_log("[MaskClick] Error: Selected mesh is null after cancel!\n");
                    return;
                }

                // Compute click action on the mesh at click coordinate
                Ray ray = camera.getRay((float)event.button.x, (float)event.button.y);
                bool hitMesh = false;
                uint32_t closestVert = 0xffffffff;
                float bestMask = 1.0f;

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
                    glm::vec3 inter = localRayOrigin + minT * localRayDir;
                    float bestDist2 = std::numeric_limits<float>::infinity();

                    uint32_t faceVerts[4] = {
                        mesh->faces[intersectFaceId * 4],
                        mesh->faces[intersectFaceId * 4 + 1],
                        mesh->faces[intersectFaceId * 4 + 2],
                        mesh->faces[intersectFaceId * 4 + 3]
                    };

                    for (int k = 0; k < 4; ++k) {
                        uint32_t vid = faceVerts[k];
                        if (vid == 0xffffffff) break;
                        glm::vec3 v(mesh->verts[vid * 3], mesh->verts[vid * 3 + 1], mesh->verts[vid * 3 + 2]);
                        float dist2 = glm::dot(v - inter, v - inter);
                        if (dist2 < bestDist2) {
                            bestDist2 = dist2;
                            closestVert = vid;
                            bestMask = mesh->materials[vid * 3 + 2];
                        }
                    }
                }

                if (hitMesh) {
                    SDL_Keymod mod = SDL_GetModState();
                    bool ctrlKey = (mod & KMOD_CTRL) != 0;
                    bool altKey = (mod & KMOD_ALT) != 0;

                    sculpt_log("[MaskClick] Ray hit mesh. closestVert=%u bestMask=%.2f\n", closestVert, bestMask);
                    scene.pushHistoryState();

                    mesh = scene.getSelected();
                    if (!mesh) return;

                    if (ctrlKey && altKey) {
                        sharpenMask(mesh);
                    } else {
                        blurMask(mesh);
                    }
                } else {
                    sculpt_log("[MaskClick] Ray missed mesh. Inverting mask...\n");
                    scene.pushHistoryState();
                    mesh = scene.getSelected();
                    if (mesh) {
                        invertMask(mesh);
                    }
                }
            } else {
                g_undoManager.endSculptStroke(scene);
            }
        }
        m_cameraController.handleEvent(event, camera, scene.getMeshes());
    } else if (event.type == SDL_MOUSEWHEEL) {
        m_cameraController.handleEvent(event, camera, scene.getMeshes());
    } else if (event.type == SDL_MOUSEMOTION) {
        int mouseX = event.motion.x;
        int mouseY = event.motion.y;

        int dx = mouseX - m_prevMouseX;
        int dy = mouseY - m_prevMouseY;

        m_prevMouseX = mouseX;
        m_prevMouseY = mouseY;

        if (m_currentBrush == BRUSH_MEASURE || m_currentBrush == BRUSH_DIVIDER) {
            float threshold = 15.0f;
            auto& segments = (m_currentBrush == BRUSH_MEASURE) ? m_measureSegments : m_dividerSegments;

            if (m_draggedSegment) {
                std::string otherKey = (m_draggedVertexKey == "vertA") ? "vertB" : "vertA";
                glm::vec3 refPos = getAnchorWorldPos(otherKey == "vertA" ? m_draggedSegment->vertA : m_draggedSegment->vertB);
                MeasurementAnchor anchor = pickAnchor((float)mouseX, (float)mouseY, scene, &refPos);
                if (m_draggedVertexKey == "vertA") {
                    m_draggedSegment->vertA = anchor;
                } else {
                    m_draggedSegment->vertB = anchor;
                }
                return;
            }

            if (m_hasPending) {
                glm::vec3 refPos = getAnchorWorldPos(m_pendingAnchorA);
                MeasurementAnchor anchor = pickAnchor((float)mouseX, (float)mouseY, scene, &refPos);
                m_pendingAnchorB = anchor;
                return;
            }

            // Hover scanning
            m_hoveredSegment = nullptr;
            m_hoveredVertexKey = "";

            for (auto& seg : segments) {
                glm::vec3 worldA = getAnchorWorldPos(seg.vertA);
                glm::vec3 worldB = getAnchorWorldPos(seg.vertB);
                glm::vec3 screenA = camera.project(worldA);
                glm::vec3 screenB = camera.project(worldB);

                float distA = glm::distance(glm::vec2(mouseX, mouseY), glm::vec2(screenA.x, screenA.y));
                float distB = glm::distance(glm::vec2(mouseX, mouseY), glm::vec2(screenB.x, screenB.y));

                if (distA < threshold && distA <= distB) {
                    m_hoveredSegment = &seg;
                    m_hoveredVertexKey = "vertA";
                    break;
                }
                if (distB < threshold) {
                    m_hoveredSegment = &seg;
                    m_hoveredVertexKey = "vertB";
                    break;
                }
            }

            if (m_cameraController.isDragging()) {
                m_cameraController.handleEvent(event, camera, scene.getMeshes());
            }
            return;
        }

        if (m_currentBrush == BRUSH_TRANSFORM) {
            if (m_cameraController.isDragging()) {
                m_cameraController.handleEvent(event, camera, scene.getMeshes());
            }
            return;
        }

        if (m_currentBrush == BRUSH_ARMATURE_SPHERES) {
            if (m_cameraController.isDragging()) {
                m_cameraController.handleEvent(event, camera, scene.getMeshes());
            } else {
                m_armatureTool->update(scene, camera, (float)mouseX, (float)mouseY);
            }
            return;
        }

        if (m_currentBrush == BRUSH_MASK_GRADIENT_BLUR && mesh && m_gradActive && m_gradActivePoint != '\0') {
            glm::vec2 mousePos((float)mouseX, (float)mouseY);
            if (m_gradActivePoint == 'A') {
                m_gradPointA = mousePos;
            } else if (m_gradActivePoint == 'B') {
                m_gradPointB = mousePos;
            }

            glm::mat4 mvp = camera.getProjMatrix() * camera.getViewMatrix() * mesh->matrix;
            float width = (float)camera.getWidth();
            float height = (float)camera.getHeight();
            float localToScreen[16] = {0.0f};

            localToScreen[0] = (mvp[0][0] + mvp[0][3]) * 0.5f * width;
            localToScreen[4] = (mvp[1][0] + mvp[1][3]) * 0.5f * width;
            localToScreen[8] = (mvp[2][0] + mvp[2][3]) * 0.5f * width;
            localToScreen[12] = (mvp[3][0] + mvp[3][3]) * 0.5f * width;

            localToScreen[1] = (mvp[0][1] + mvp[0][3]) * 0.5f * height;
            localToScreen[5] = (mvp[1][1] + mvp[1][3]) * 0.5f * height;
            localToScreen[9] = (mvp[2][1] + mvp[2][3]) * 0.5f * height;
            localToScreen[13] = (mvp[3][1] + mvp[3][3]) * 0.5f * height;

            localToScreen[3] = mvp[0][3];
            localToScreen[7] = mvp[1][3];
            localToScreen[11] = mvp[2][3];
            localToScreen[15] = mvp[3][3];

            float ptPlaneX = 0.0f, ptPlaneY = 0.0f, ptPlaneZ = 0.0f;
            float nPlaneX = m_symX ? 1.0f : 0.0f;
            float nPlaneY = m_symY ? 1.0f : 0.0f;
            float nPlaneZ = m_symZ ? 1.0f : 0.0f;

            ::applyGradientMask(
                mesh->verts.data(),
                mesh->materials.data(),
                m_gradActiveVerts.data(),
                (int)m_gradActiveVerts.size(),
                m_origMasks.data(),
                m_blurredMasks.data(),
                localToScreen,
                height,
                m_gradPointA.x, m_gradPointA.y,
                m_gradPointB.x, m_gradPointB.y,
                m_useSym,
                ptPlaneX, ptPlaneY, ptPlaneZ,
                nPlaneX, nPlaneY, nPlaneZ,
                getCurrentSettings().blurMaskedOnly,
                mesh->nbVerts
            );

            mesh->isVertexDirty = true;
            mesh->dirtyVertMin = 0;
            mesh->dirtyVertMax = mesh->nbVerts - 1;
            mesh->isDirty = true;
            return;
        }

        if (m_isLassoActive) {
            if (m_lassoPoints.empty() || m_lassoPoints.back() != glm::vec2((float)mouseX, (float)mouseY)) {
                m_lassoPoints.push_back(glm::vec2((float)mouseX, (float)mouseY));
            }
            m_lassoAlt = (SDL_GetModState() & KMOD_ALT) != 0;
            return;
        }

        if (m_cameraController.isDragging()) {
            m_cameraController.handleEvent(event, camera, scene.getMeshes());
        } else if (m_isSculpting && mesh) {
            // Check stylus/tablet pressure
            float currentPressure = 1.0f;
#ifdef _WIN32
            if (g_tablet.isAvailable() && g_tablet.isPenActive()) {
                currentPressure = g_tablet.getPressure();
            } else {
                if (m_usingStylus && (SDL_GetTicks() - m_lastStylusTime > 1000)) {
                    m_usingStylus = false;
                    m_stylusPressure = 1.0f;
                }
                currentPressure = m_usingStylus ? m_stylusPressure : 1.0f;
            }
#else
            if (m_usingStylus && (SDL_GetTicks() - m_lastStylusTime > 1000)) {
                m_usingStylus = false;
                m_stylusPressure = 1.0f;
            }
            currentPressure = m_usingStylus ? m_stylusPressure : 1.0f;
#endif

            float strokeDx = (float)(mouseX - m_lastStrokeX);
            float strokeDy = (float)(mouseY - m_lastStrokeY);
            float dist = std::sqrt(strokeDx * strokeDx + strokeDy * strokeDy);
            
            BrushType activeBrush = m_currentBrush;
            SDL_Keymod mod = SDL_GetModState();
            if (mod & KMOD_SHIFT) {
                activeBrush = BRUSH_SMOOTH;
            } else if ((mod & KMOD_CTRL) && m_currentBrush != BRUSH_POLYGROUP) {
                activeBrush = BRUSH_MASK;
            }
            if (activeBrush != m_currentBrush && (activeBrush == BRUSH_SMOOTH || activeBrush == BRUSH_MASK)) {
                m_brushSettings[activeBrush].radius = m_brushSettings[m_currentBrush].radius;
            }
            const auto& activeSettings = getSettings(activeBrush);
            float minSpacing = activeSettings.spacing * activeSettings.radius;

            bool isGrabBrush = (activeBrush == BRUSH_MOVE || activeBrush == BRUSH_DRAG || activeBrush == BRUSH_ELASTIC);

            if (isGrabBrush || minSpacing <= 0.0f) {
                executeStroke(scene, mesh, camera, (float)mouseX, (float)mouseY, currentPressure);
                m_lastStrokeX = mouseX;
                m_lastStrokeY = mouseY;
            } else if (dist > minSpacing) {
                float step = 1.0f / std::floor(dist / minSpacing);
                float stepDx = strokeDx * step;
                float stepDy = strokeDy * step;

                float curX = (float)m_lastStrokeX + stepDx;
                float curY = (float)m_lastStrokeY + stepDy;

                for (float i = step; i <= 1.01f; i += step) {
                    executeStroke(scene, mesh, camera, curX, curY, currentPressure);
                    curX += stepDx;
                    curY += stepDy;
                }
                m_lastStrokeX = mouseX;
                m_lastStrokeY = mouseY;
            }
        }
    }
}

void SculptManager::processFrame(Scene& scene) {
    if (m_cameraController.isDragging()) {
        m_cursor.hide();
    } else {
        // Resolve dynamic brush modifier swap (Shift->Smooth, Ctrl->Mask, Ctrl+Shift->Visibility)
        BrushType activeBrush = m_currentBrush;
        SDL_Keymod mod = SDL_GetModState();

        if (activeBrush == BRUSH_ARMATURE_SPHERES) {
            m_armatureTool->preUpdate(scene, scene.getCamera(), (float)m_rawMouseX, (float)m_rawMouseY, (mod & KMOD_CTRL) != 0, (mod & KMOD_ALT) != 0);
        }
        if ((mod & KMOD_CTRL) && (mod & KMOD_SHIFT)) {
            activeBrush = BRUSH_VISIBILITY;
        } else if (mod & KMOD_SHIFT) {
            activeBrush = BRUSH_SMOOTH;
        } else if (mod & KMOD_CTRL) {
            activeBrush = BRUSH_MASK;
        }

        if (activeBrush != m_currentBrush && (activeBrush == BRUSH_SMOOTH || activeBrush == BRUSH_MASK || activeBrush == BRUSH_VISIBILITY)) {
            m_brushSettings[activeBrush].radius = m_brushSettings[m_currentBrush].radius;
        }

        const auto& activeSettings = getSettings(activeBrush);
        m_cursor.update(
            m_rawMouseX, m_rawMouseY,
            scene,
            activeSettings.radius,
            m_useSym,
            m_symX,
            m_symY,
            m_symZ,
            m_isSculpting,
            activeBrush,
            m_isSculpting ? m_hasAnyValidIntersection : m_currentIntersectionValid,
            m_isSculpting ? m_lastValidIntersection : m_currentIntersection,
            m_isSculpting ? m_lastValidIntersectionNormal : m_currentIntersectionNormal,
            activeSettings.focalShift,
            activeSettings.hardness,
            activeSettings.paintColor
        );
    }
}

static bool isPointInPolygon(float x, float y, const std::vector<glm::vec2>& polygon) {
    bool inside = false;
    int count = (int)polygon.size();
    for (int i = 0, j = count - 1; i < count; j = i++) {
        float xi = polygon[i].x, yi = polygon[i].y;
        float xj = polygon[j].x, yj = polygon[j].y;

        bool intersect = ((yi > y) != (yj > y))
            && (x < (xj - xi) * (y - yi) / (yj - yi + 1e-12f) + xi);
        if (intersect) inside = !inside;
    }
    return inside;
}

std::vector<uint32_t> SculptManager::getVerticesInLasso(Mesh* mesh, const Camera& camera) {
    std::vector<uint32_t> insideVertices;
    if (!mesh) return insideVertices;

    glm::mat4 mvp = camera.getProjMatrix() * camera.getViewMatrix() * mesh->matrix;
    float width = (float)camera.getWidth();
    float height = (float)camera.getHeight();

    int nbVerts = mesh->nbVerts;
    const float* verts = mesh->verts.data();

    for (int i = 0; i < nbVerts; ++i) {
        int ind = i * 3;
        glm::vec4 localPos(verts[ind], verts[ind + 1], verts[ind + 2], 1.0f);
        glm::vec4 clipPos = mvp * localPos;
        float w = clipPos.w;
        if (w <= 0.0f) continue;

        float ndcX = clipPos.x / w;
        float ndcY = clipPos.y / w;

        if (camera.getRef2DMode()) {
            ndcX = ndcX * camera.getView2DZoom() + camera.getView2DOffsetX();
            ndcY = ndcY * camera.getView2DZoom() + camera.getView2DOffsetY();
        }

        float screenX = (ndcX + 1.0f) * 0.5f * width;
        float screenY = (1.0f - ndcY) * 0.5f * height;

        if (isPointInPolygon(screenX, screenY, m_lassoPoints)) {
            insideVertices.push_back(i);
        }
    }

    return insideVertices;
}

bool SculptManager::saveSettings(const std::string& filepath) {
    std::ofstream out(filepath);
    if (!out.is_open()) {
        std::cerr << "Failed to open brush settings file for writing: " << filepath << std::endl;
        return false;
    }

    for (int i = 0; i < BRUSH_COUNT; ++i) {
        out << "[Brush_" << i << "]\n";
        out << "radius=" << m_brushSettings[i].radius << "\n";
        out << "intensity=" << m_brushSettings[i].intensity << "\n";
        out << "focalShift=" << m_brushSettings[i].focalShift << "\n";
        out << "focalShiftFalloff=" << (m_brushSettings[i].focalShiftFalloff ? "true" : "false") << "\n";
        out << "hardness=" << m_brushSettings[i].hardness << "\n";
        out << "spacing=" << m_brushSettings[i].spacing << "\n";
        out << "negative=" << (m_brushSettings[i].negative ? "true" : "false") << "\n";
        out << "culling=" << (m_brushSettings[i].culling ? "true" : "false") << "\n";
        out << "accumulate=" << (m_brushSettings[i].accumulate ? "true" : "false") << "\n";
        out << "lockPosition=" << (m_brushSettings[i].lockPosition ? "true" : "false") << "\n";
        out << "idAlpha=" << m_brushSettings[i].idAlpha << "\n";
        out << "clay=" << (m_brushSettings[i].clay ? "true" : "false") << "\n";
        out << "tangent=" << (m_brushSettings[i].tangent ? "true" : "false") << "\n";
        out << "topoCheck=" << (m_brushSettings[i].topoCheck ? "true" : "false") << "\n";
        out << "elasticity=" << m_brushSettings[i].elasticity << "\n";
        out << "paintColor=" << m_brushSettings[i].paintColor.r << " " << m_brushSettings[i].paintColor.g << " " << m_brushSettings[i].paintColor.b << "\n";
        out << "paintRoughness=" << m_brushSettings[i].paintRoughness << "\n";
        out << "paintMetallic=" << m_brushSettings[i].paintMetallic << "\n";
        out << "writeAlbedo=" << (m_brushSettings[i].writeAlbedo ? "true" : "false") << "\n";
        out << "writeRoughness=" << (m_brushSettings[i].writeRoughness ? "true" : "false") << "\n";
        out << "writeMetalness=" << (m_brushSettings[i].writeMetalness ? "true" : "false") << "\n";
        out << "maskSharpenBlurIterations=" << m_brushSettings[i].maskSharpenBlurIterations << "\n";
        out << "maskSharpenFactor=" << m_brushSettings[i].maskSharpenFactor << "\n";
        out << "maskExtractThickness=" << m_brushSettings[i].maskExtractThickness << "\n";
        out << "blurMaskedOnly=" << (m_brushSettings[i].blurMaskedOnly ? "true" : "false") << "\n";
        out << "stampType=" << m_brushSettings[i].stampType << "\n";
        out << "stampSides=" << m_brushSettings[i].stampSides << "\n";
        out << "stampInnerRatio=" << m_brushSettings[i].stampInnerRatio << "\n";
        out << "stampAngle=" << m_brushSettings[i].stampAngle << "\n";
        out << "stampBlur=" << m_brushSettings[i].stampBlur << "\n";
        out << "stampLockRotation=" << (m_brushSettings[i].stampLockRotation ? "true" : "false") << "\n";
        out << "stampUseTilt=" << (m_brushSettings[i].stampUseTilt ? "true" : "false") << "\n\n";
    }

    out << "[General]\n";
    out << "dividerDivisions=" << m_dividerDivisions << "\n";
    out << "measureUseDistanceThickness=" << (m_measureUseDistanceThickness ? "true" : "false") << "\n";
#ifdef _WIN32
    out << "usePressure=" << (g_tablet.isPressureEnabled() ? "true" : "false") << "\n";
    out << "usePressureSize=" << (g_tablet.isPressureSizeEnabled() ? "true" : "false") << "\n";
    out << "usePressureCursor=" << (g_tablet.isPressureCursorEnabled() ? "true" : "false") << "\n";
    out << "useTilt=" << (g_tablet.isTiltEnabled() ? "true" : "false") << "\n";
    out << "pressureCurve=" << g_tablet.getPressureCurveString() << "\n";
    out << "pressureCurveType=" << (int)g_tablet.getInterpolationType() << "\n";
#endif
    out << "\n[Symmetry]\n";
    out << "useSym=" << (m_useSym ? "true" : "false") << "\n";
    out << "symX=" << (m_symX ? "true" : "false") << "\n";
    out << "symY=" << (m_symY ? "true" : "false") << "\n";
    out << "symZ=" << (m_symZ ? "true" : "false") << "\n";
    out << "symmetryMode=" << (m_symmetryMode == SymmetryMode::World ? "world" : "local") << "\n\n";

    std::cout << "Successfully saved brush settings to: " << filepath << std::endl;
    return true;
}

bool SculptManager::loadSettings(const std::string& filepath) {
    std::ifstream in(filepath);
    if (!in.is_open()) {
        std::cerr << "Failed to open brush settings file for reading: " << filepath << std::endl;
        return false;
    }

    std::string line;
    std::string currentSection = "";
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> sections;

    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        if (line[0] == '[' && line[line.size() - 1] == ']') {
            currentSection = trim(line.substr(1, line.size() - 2));
            continue;
        }

        size_t eqPos = line.find('=');
        if (eqPos != std::string::npos && !currentSection.empty()) {
            std::string key = trim(line.substr(0, eqPos));
            std::string val = trim(line.substr(eqPos + 1));
            sections[currentSection][key] = val;
        }
    }

    for (int i = 0; i < BRUSH_COUNT; ++i) {
        std::string sectionName = "Brush_" + std::to_string(i);
        auto itSection = sections.find(sectionName);
        if (itSection != sections.end()) {
            const auto& params = itSection->second;

            auto getParam = [&](const std::string& key, auto& outVal, auto converter) {
                auto it = params.find(key);
                if (it != params.end()) {
                    outVal = converter(it->second);
                }
            };

            auto getBoolParam = [&](const std::string& key, bool& outVal) {
                auto it = params.find(key);
                if (it != params.end()) {
                    outVal = (it->second == "true" || it->second == "1");
                }
            };

            getParam("radius", m_brushSettings[i].radius, [](const std::string& s) { return safe_stof(s, 50.0f); });
            getParam("intensity", m_brushSettings[i].intensity, [](const std::string& s) { return safe_stof(s, 0.5f); });
            getParam("focalShift", m_brushSettings[i].focalShift, [](const std::string& s) { return safe_stof(s, 0.0f); });
            getBoolParam("focalShiftFalloff", m_brushSettings[i].focalShiftFalloff);
            getParam("hardness", m_brushSettings[i].hardness, [](const std::string& s) { return safe_stof(s, 0.5f); });
            getParam("spacing", m_brushSettings[i].spacing, [](const std::string& s) { return safe_stof(s, 0.15f); });
            getBoolParam("negative", m_brushSettings[i].negative);
            getBoolParam("culling", m_brushSettings[i].culling);
            getBoolParam("accumulate", m_brushSettings[i].accumulate);
            getBoolParam("lockPosition", m_brushSettings[i].lockPosition);
            getParam("idAlpha", m_brushSettings[i].idAlpha, [](const std::string& s) { return safe_stoi(s, 0); });
            getBoolParam("clay", m_brushSettings[i].clay);
            getBoolParam("tangent", m_brushSettings[i].tangent);
            getBoolParam("topoCheck", m_brushSettings[i].topoCheck);
            getParam("elasticity", m_brushSettings[i].elasticity, [](const std::string& s) { return safe_stof(s, 1.0f); });

            auto itColor = params.find("paintColor");
            if (itColor != params.end()) {
                std::stringstream ss(itColor->second);
                float r, g, b;
                if (ss >> r >> g >> b) {
                    m_brushSettings[i].paintColor = glm::vec3(r, g, b);
                }
            }

            getParam("paintRoughness", m_brushSettings[i].paintRoughness, [](const std::string& s) { return safe_stof(s, 0.5f); });
            getParam("paintMetallic", m_brushSettings[i].paintMetallic, [](const std::string& s) { return safe_stof(s, 0.0f); });

            getBoolParam("writeAlbedo", m_brushSettings[i].writeAlbedo);
            getBoolParam("writeRoughness", m_brushSettings[i].writeRoughness);
            getBoolParam("writeMetalness", m_brushSettings[i].writeMetalness);

            getParam("maskSharpenBlurIterations", m_brushSettings[i].maskSharpenBlurIterations, [](const std::string& s) { return safe_stoi(s, 4); });
            getParam("maskSharpenFactor", m_brushSettings[i].maskSharpenFactor, [](const std::string& s) { return safe_stof(s, 1.0f); });
            getParam("maskExtractThickness", m_brushSettings[i].maskExtractThickness, [](const std::string& s) { return safe_stof(s, 0.05f); });
            getBoolParam("blurMaskedOnly", m_brushSettings[i].blurMaskedOnly);
            getParam("stampType", m_brushSettings[i].stampType, [](const std::string& s) { return safe_stoi(s, 0); });
            getParam("stampSides", m_brushSettings[i].stampSides, [](const std::string& s) { return safe_stoi(s, 5); });
            getParam("stampInnerRatio", m_brushSettings[i].stampInnerRatio, [](const std::string& s) { return safe_stof(s, 0.5f); });
            getParam("stampAngle", m_brushSettings[i].stampAngle, [](const std::string& s) { return safe_stof(s, 0.0f); });
            getParam("stampBlur", m_brushSettings[i].stampBlur, [](const std::string& s) { return safe_stof(s, 0.0f); });
            getBoolParam("stampLockRotation", m_brushSettings[i].stampLockRotation);
            getBoolParam("stampUseTilt", m_brushSettings[i].stampUseTilt);
        }
    }

    auto itGeneral = sections.find("General");
    if (itGeneral != sections.end()) {
        const auto& params = itGeneral->second;
        auto it = params.find("dividerDivisions");
        if (it != params.end()) {
            m_dividerDivisions = std::stoi(it->second);
        }
        it = params.find("measureUseDistanceThickness");
        if (it != params.end()) {
            m_measureUseDistanceThickness = (it->second == "true" || it->second == "1");
        }
#ifdef _WIN32
        it = params.find("usePressure");
        if (it != params.end()) {
            g_tablet.setPressureEnabled(it->second == "true" || it->second == "1");
        }
        it = params.find("usePressureSize");
        if (it != params.end()) {
            g_tablet.setPressureSizeEnabled(it->second == "true" || it->second == "1");
        }
        it = params.find("usePressureCursor");
        if (it != params.end()) {
            g_tablet.setPressureCursorEnabled(it->second == "true" || it->second == "1");
        }
        it = params.find("useTilt");
        if (it != params.end()) {
            g_tablet.setTiltEnabled(it->second == "true" || it->second == "1");
        }
        it = params.find("pressureCurve");
        if (it != params.end()) {
            g_tablet.setPressureCurveFromString(it->second);
        }
        it = params.find("pressureCurveType");
        if (it != params.end()) {
            g_tablet.setInterpolationType((TabletInput::InterpolationType)std::stoi(it->second));
        } else {
            it = params.find("useSplineCurve");
            if (it != params.end()) {
                g_tablet.setSplineEnabled(it->second == "true" || it->second == "1");
            }
        }
#endif
    }

    auto itSymmetry = sections.find("Symmetry");
    if (itSymmetry != sections.end()) {
        const auto& params = itSymmetry->second;
        auto getBoolParam = [&](const std::string& key, bool& outVal) {
            auto it = params.find(key);
            if (it != params.end()) {
                outVal = (it->second == "true" || it->second == "1");
            }
        };

        getBoolParam("useSym", m_useSym);
        getBoolParam("symX", m_symX);
        getBoolParam("symY", m_symY);
        getBoolParam("symZ", m_symZ);

        auto itMode = params.find("symmetryMode");
        if (itMode != params.end()) {
            if (itMode->second == "world" || itMode->second == "1") {
                m_symmetryMode = SymmetryMode::World;
            } else {
                m_symmetryMode = SymmetryMode::Local;
            }
        }
    }

    std::cout << "Successfully loaded brush settings from: " << filepath << std::endl;
    return true;
}

void SculptManager::clearMask(Mesh* mesh) {
    if (!mesh) return;
    if (mesh->materials.size() < (size_t)mesh->nbVerts * 3) return;
    float* materials = mesh->materials.data();
    int nbVerts = mesh->nbVerts;
    for (int i = 0; i < nbVerts; ++i) {
        materials[i * 3 + 2] = 1.0f;
    }
    mesh->isMaterialDirty = true;
    mesh->dirtyVertMin = 0;
    mesh->dirtyVertMax = nbVerts - 1;
}

void SculptManager::invertMask(Mesh* mesh) {
    std::cout << "[MaskInvert] Inverting mask for mesh " << mesh << " (nbVerts=" << (mesh ? mesh->nbVerts : 0) << ")..." << std::endl;
    if (!mesh) return;
    if (mesh->materials.size() < (size_t)mesh->nbVerts * 3) return;
    float* materials = mesh->materials.data();
    int nbVerts = mesh->nbVerts;
    for (int i = 0; i < nbVerts; ++i) {
        materials[i * 3 + 2] = 1.0f - materials[i * 3 + 2];
    }
    mesh->isMaterialDirty = true;
    mesh->dirtyVertMin = 0;
    mesh->dirtyVertMax = nbVerts - 1;
    std::cout << "[MaskInvert] Mask inverted successfully." << std::endl;
}

void SculptManager::blurMask(Mesh* mesh, int iterations) {
    std::cout << "[MaskBlur] Call blurMask(mesh=" << mesh << ")..." << std::endl;
    if (!mesh) {
        std::cout << "[MaskBlur] Error: mesh is null!" << std::endl;
        return;
    }
    if (mesh->nbVerts <= 0) {
        std::cout << "[MaskBlur] Error: mesh->nbVerts <= 0 (" << mesh->nbVerts << ")" << std::endl;
        return;
    }
    if (mesh->materials.size() < (size_t)mesh->nbVerts * 3) {
        std::cout << "[MaskBlur] Error: materials array size mismatch! size=" << mesh->materials.size() << " expected=" << (mesh->nbVerts * 3) << std::endl;
        return;
    }
    if (mesh->vrvStartCount.empty() || mesh->vertRingVert.empty()) {
        std::cout << "[MaskBlur] Error: topology empty! vrvStartCount.size=" << mesh->vrvStartCount.size() << " vertRingVert.size=" << mesh->vertRingVert.size() << std::endl;
        return;
    }

    float* mAr = mesh->materials.data();
    int nbVerts = mesh->nbVerts;

    std::vector<uint32_t> iVerts;
    iVerts.reserve(nbVerts);
    for (int i = 0; i < nbVerts; ++i) {
        if (mAr[i * 3 + 2] < 1.0f) {
            iVerts.push_back(i);
        }
    }
    std::cout << "[MaskBlur] Initial masked verts count: " << iVerts.size() << " / " << nbVerts << std::endl;
    if (iVerts.empty()) return;

    if (iterations <= 0) {
        iterations = m_brushSettings[m_currentBrush].maskSharpenBlurIterations;
    }
    if (iterations <= 0) iterations = 2;

    std::vector<uint8_t> visited(nbVerts, 0);
    for (uint32_t v : iVerts) {
        if (v < (uint32_t)nbVerts) visited[v] = 1;
    }

    std::vector<uint32_t> queue = iVerts;
    size_t head = 0;
    for (int step = 0; step < iterations; ++step) {
        size_t size = queue.size();
        while (head < size) {
            uint32_t id = queue[head++];
            if (id >= (uint32_t)nbVerts) continue;
            if (id * 2 + 1 >= mesh->vrvStartCount.size()) continue;

            uint32_t start = mesh->vrvStartCount[id * 2];
            uint32_t count = mesh->vrvStartCount[id * 2 + 1];
            if (start + count > mesh->vertRingVert.size()) continue;

            for (uint32_t j = start; j < start + count; ++j) {
                uint32_t neighbor = mesh->vertRingVert[j];
                if (neighbor < (uint32_t)nbVerts && !visited[neighbor]) {
                    visited[neighbor] = 1;
                    queue.push_back(neighbor);
                }
            }
        }
    }
    iVerts = queue;
    std::cout << "[MaskBlur] Expanded queue size: " << iVerts.size() << ", invoking ::blurMask..." << std::endl;

    int processed = ::blurMask(
        iVerts.data(), (int)iVerts.size(),
        mesh->vrvStartCount.data(),
        mesh->vertRingVert.data(),
        mesh->vertOnEdge.empty() ? nullptr : mesh->vertOnEdge.data(),
        iterations,
        mAr,
        3, // stride (materials contains 3 floats per vertex)
        2  // offset (mask is at index 2)
    );
    std::cout << "[MaskBlur] ::blurMask finished. Processed: " << processed << " vertices." << std::endl;

    mesh->isMaterialDirty = true;
    mesh->dirtyVertMin = 0;
    mesh->dirtyVertMax = nbVerts - 1;
}

void SculptManager::sharpenMask(Mesh* mesh, int iterations) {
    std::cout << "[MaskSharpen] Call sharpenMask(mesh=" << mesh << ")..." << std::endl;
    if (!mesh || mesh->nbVerts <= 0) return;
    if (mesh->materials.size() < (size_t)mesh->nbVerts * 3) return;
    if (mesh->vrvStartCount.empty() || mesh->vertRingVert.empty()) return;

    float* mAr = mesh->materials.data();
    int nbVerts = mesh->nbVerts;

    std::vector<uint32_t> iVerts;
    iVerts.reserve(nbVerts);
    for (int i = 0; i < nbVerts; ++i) {
        if (mAr[i * 3 + 2] < 1.0f) {
            iVerts.push_back(i);
        }
    }
    std::cout << "[MaskSharpen] Initial masked verts count: " << iVerts.size() << " / " << nbVerts << std::endl;
    if (iVerts.empty()) return;

    if (iterations <= 0) {
        iterations = m_brushSettings[m_currentBrush].maskSharpenBlurIterations;
    }
    if (iterations <= 0) iterations = 2;

    std::vector<uint8_t> visited(nbVerts, 0);
    for (uint32_t v : iVerts) {
        if (v < (uint32_t)nbVerts) visited[v] = 1;
    }

    std::vector<uint32_t> queue = iVerts;
    size_t head = 0;
    for (int step = 0; step < iterations; ++step) {
        size_t size = queue.size();
        while (head < size) {
            uint32_t id = queue[head++];
            if (id >= (uint32_t)nbVerts) continue;
            if (id * 2 + 1 >= mesh->vrvStartCount.size()) continue;

            uint32_t start = mesh->vrvStartCount[id * 2];
            uint32_t count = mesh->vrvStartCount[id * 2 + 1];
            if (start + count > mesh->vertRingVert.size()) continue;

            for (uint32_t j = start; j < start + count; ++j) {
                uint32_t neighbor = mesh->vertRingVert[j];
                if (neighbor < (uint32_t)nbVerts && !visited[neighbor]) {
                    visited[neighbor] = 1;
                    queue.push_back(neighbor);
                }
            }
        }
    }
    iVerts = queue;

    int nbActiveVerts = (int)iVerts.size();
    std::vector<float> originalMask(nbActiveVerts);
    for (int i = 0; i < nbActiveVerts; ++i) {
        originalMask[i] = mAr[iVerts[i] * 3 + 2];
    }

    ::blurMask(
        iVerts.data(), nbActiveVerts,
        mesh->vrvStartCount.data(),
        mesh->vertRingVert.data(),
        mesh->vertOnEdge.empty() ? nullptr : mesh->vertOnEdge.data(),
        iterations,
        mAr,
        3, // stride
        2  // offset
    );

    float factor = m_brushSettings[m_currentBrush].maskSharpenFactor;
    for (int i = 0; i < nbActiveVerts; ++i) {
        int idm = iVerts[i] * 3 + 2;
        float orig = originalMask[i];
        float blurred = mAr[idm];
        float val = orig + factor * (orig - blurred);
        if (val < 0.0f) val = 0.0f;
        if (val > 1.0f) val = 1.0f;
        mAr[idm] = val;
    }

    mesh->isMaterialDirty = true;
    mesh->dirtyVertMin = 0;
    mesh->dirtyVertMax = nbVerts - 1;
}

void SculptManager::applyPreset(const BrushPreset& preset) {
    BrushType targetBrush = m_currentBrush;
    switch (preset.deformMode) {
        case DeformMode::Normal:
            targetBrush = BRUSH_BRUSH;
            break;
        case DeformMode::Clay:
            if (!preset.accumulate) {
                targetBrush = BRUSH_CLAYBUILDUP;
            } else {
                targetBrush = BRUSH_CLAY;
            }
            break;
        case DeformMode::Inflate:
            targetBrush = BRUSH_INFLATE;
            break;
        case DeformMode::Pinch:
            targetBrush = BRUSH_PINCH;
            break;
        case DeformMode::Crease:
            {
                std::string lowerName = preset.name;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                if (lowerName.find("dam") != std::string::npos) {
                    targetBrush = BRUSH_DAMSTANDARD;
                } else {
                    targetBrush = BRUSH_CREASE;
                }
            }
            break;
        case DeformMode::Flatten:
            targetBrush = BRUSH_FLATTEN;
            break;
        case DeformMode::Smooth:
            targetBrush = BRUSH_SMOOTH;
            break;
        case DeformMode::Move:
            {
                std::string lowerName = preset.name;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                if (lowerName.find("elastic") != std::string::npos) {
                    targetBrush = BRUSH_ELASTIC;
                } else if (lowerName.find("drag") != std::string::npos) {
                    targetBrush = BRUSH_DRAG;
                } else {
                    targetBrush = BRUSH_MOVE;
                }
            }
            break;
    }

    m_currentBrush = targetBrush;
    
    BrushSettings& settings = m_brushSettings[targetBrush];
    settings.radius = preset.radius;
    settings.intensity = preset.intensity;
    settings.spacing = preset.spacing;
    settings.hardness = preset.hardness;
    settings.focalShift = preset.focalShift;
    settings.focalShiftFalloff = preset.focalShiftFalloff;
    settings.negative = preset.negative;
    settings.culling = preset.culling;
    settings.singlePolyGroup = preset.singlePolyGroup;
    settings.accumulate = preset.accumulate;
    settings.lockPosition = preset.lockPosition;
    settings.idAlpha = preset.idAlpha;
    
    settings.strokeMode = preset.strokeMode;
    settings.deformMode = preset.deformMode;
    settings.altmode = preset.altmode;
    settings.lazyRadius = preset.lazyRadius;
    settings.lazySmooth = preset.lazySmooth;
    settings.grabRadius = preset.grabRadius;
    settings.grabRadiusScale = preset.grabRadiusScale;
    settings.areaNormalRadius = preset.areaNormalRadius;
    settings.areaPointRadius = preset.areaPointRadius;
    settings.areaSharp = preset.areaSharp;
    settings.areaSampling = preset.areaSampling;
    
    settings.flattenLockNormal = preset.flattenLockNormal;
    settings.flattenLockOrigin = preset.flattenLockOrigin;
    
    settings.smoothTaubin = preset.smoothTaubin;
    settings.smoothTaubinInflate = preset.smoothTaubinInflate;
    settings.smoothTaubinShrink = preset.smoothTaubinShrink;
    settings.smoothRelax = preset.smoothRelax;
    settings.smoothStable = preset.smoothStable;
    settings.smoothStickyBorder = preset.smoothStickyBorder;
    settings.tangent = preset.tangent;
    
    settings.depthFilter = preset.depthFilter;
    settings.connectedTopology = preset.connectedTopology;
    settings.onlyFrontFace = preset.onlyFrontFace;
    settings.topoCheck = preset.topoCheck;
    settings.useDynamicTopology = preset.useDynamicTopology;
    settings.elasticity = preset.elasticity;
    
    settings.paintColor = glm::vec3(preset.paintColor[0], preset.paintColor[1], preset.paintColor[2]);
    settings.paintRoughness = preset.roughness;
    settings.paintMetallic = preset.metallic;
    settings.writeAlbedo = preset.writeAlbedo;
    settings.writeRoughness = preset.writeRoughness;
    settings.writeMetalness = preset.writeMetalness;
    
    settings.pressureIntensity = preset.pressureIntensity;
    settings.pressureRadius = preset.pressureRadius;
    settings.useGlobalPressure = preset.useGlobalPressure;
    
    settings.falloff.preset = preset.falloff.preset;
    settings.falloff.points.clear();
    for (const auto& pt : preset.falloff.points) {
        settings.falloff.points.push_back({pt[0], pt[1]});
    }
    
    settings.subdivFactor = preset.subdivFactor;
    settings.decimFactor = preset.decimFactor;
}

void SculptManager::applyActivePreset() {
    const BrushPreset* activePreset = BrushPresetManager::instance().active();
    if (activePreset) {
        applyPreset(*activePreset);
    }
}

static glm::vec3 polyLerp(Mesh* mesh, uint32_t faceId, const glm::vec3& interPoint, const float* field) {
    if (!mesh || faceId == 0xffffffff || faceId >= (uint32_t)mesh->nbFaces) return glm::vec3(0.0f);
    
    uint32_t iv1 = mesh->faces[faceId * 4];
    uint32_t iv2 = mesh->faces[faceId * 4 + 1];
    uint32_t iv3 = mesh->faces[faceId * 4 + 2];
    uint32_t iv4 = mesh->faces[faceId * 4 + 3];
    bool isQuad = (iv4 != 0xffffffff);
    
    glm::vec3 v1(mesh->verts[iv1 * 3], mesh->verts[iv1 * 3 + 1], mesh->verts[iv1 * 3 + 2]);
    glm::vec3 v2(mesh->verts[iv2 * 3], mesh->verts[iv2 * 3 + 1], mesh->verts[iv2 * 3 + 2]);
    glm::vec3 v3(mesh->verts[iv3 * 3], mesh->verts[iv3 * 3 + 1], mesh->verts[iv3 * 3 + 2]);
    glm::vec3 v4(0.0f);
    if (isQuad) {
        v4 = glm::vec3(mesh->verts[iv4 * 3], mesh->verts[iv4 * 3 + 1], mesh->verts[iv4 * 3 + 2]);
    }
    
    float len1 = 1.0f / std::max(glm::distance(interPoint, v1), 1e-6f);
    float len2 = 1.0f / std::max(glm::distance(interPoint, v2), 1e-6f);
    float len3 = 1.0f / std::max(glm::distance(interPoint, v3), 1e-6f);
    float len4 = isQuad ? (1.0f / std::max(glm::distance(interPoint, v4), 1e-6f)) : 0.0f;
    
    float invSum = 1.0f / (len1 + len2 + len3 + len4);
    
    glm::vec3 f1(field[iv1 * 3], field[iv1 * 3 + 1], field[iv1 * 3 + 2]);
    glm::vec3 f2(field[iv2 * 3], field[iv2 * 3 + 1], field[iv2 * 3 + 2]);
    glm::vec3 f3(field[iv3 * 3], field[iv3 * 3 + 1], field[iv3 * 3 + 2]);
    glm::vec3 f4(0.0f);
    if (isQuad) {
        f4 = glm::vec3(field[iv4 * 3], field[iv4 * 3 + 1], field[iv4 * 3 + 2]);
    }
    
    return (f1 * len1 + f2 * len2 + f3 * len3 + f4 * len4) * invSum;
}

void SculptManager::pickColor(Mesh* mesh, uint32_t faceId, const glm::vec3& interPoint) {
    if (!mesh) return;
    glm::vec3 mat = polyLerp(mesh, faceId, interPoint, mesh->materials.data());
    glm::vec3 col = polyLerp(mesh, faceId, interPoint, mesh->colors.data());
    
    auto& settings = getCurrentSettings();
    settings.paintColor = col;
    settings.paintRoughness = mat.x;
    settings.paintMetallic = mat.y;
}

void SculptManager::paintAll(Scene& scene, Mesh* mesh) {
    if (!mesh) return;
    
    scene.pushHistoryState();

    int nbVerts = mesh->nbVerts;
    std::vector<uint32_t> iVerts;
    iVerts.reserve(nbVerts);
    for (int i = 0; i < nbVerts; ++i) {
        if (mesh->materials[i * 3 + 2] > 0.0f) {
            iVerts.push_back(i);
        }
    }
    
    if (iVerts.empty()) return;

    const auto& settings = m_brushSettings[BRUSH_PAINT];
    strokePaintAll(
        mesh->colors.data(),
        mesh->materials.data(),
        iVerts.data(),
        iVerts.size(),
        settings.paintColor.r, settings.paintColor.g, settings.paintColor.b,
        settings.paintRoughness, settings.paintMetallic,
        settings.writeAlbedo, settings.writeRoughness, settings.writeMetalness
    );

    mesh->isColorDirty = true;
    mesh->isMaterialDirty = true;
    mesh->dirtyVertMin = 0;
    mesh->dirtyVertMax = nbVerts - 1;
}

void SculptManager::filterPolyGroupVertices(std::vector<uint32_t>& pickedVertices, const Mesh* mesh, uint32_t targetGroupId) {
    if (!mesh || mesh->faceGroups.empty() || mesh->faceGroups.size() != (size_t)mesh->nbFaces) return;
    if (mesh->vrfStartCount.empty() || mesh->vertRingFace.empty()) return;

    pickedVertices.erase(
        std::remove_if(pickedVertices.begin(), pickedVertices.end(), [&](uint32_t v) {
            if (v >= (uint32_t)mesh->nbVerts) return true;
            uint32_t start = mesh->vrfStartCount[v * 2];
            uint32_t count = mesh->vrfStartCount[v * 2 + 1];
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t f = mesh->vertRingFace[start + i];
                if (f < (uint32_t)mesh->nbFaces && mesh->faceGroups[f] == targetGroupId) {
                    return false;
                }
            }
            return true;
        }),
        pickedVertices.end()
    );
}



