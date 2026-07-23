#include "editing/SculptManager.h"
#include "brushes/BrushPresetManager.h"
#include "sculpt/SculptEngine.h"
#include "mesh/NormalCalc.h"
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

SculptManager::SculptManager() {
    // Initialise all brushes with baseline defaults
    for (int i = 0; i < 22; ++i) {
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
    float mouseX
) {
    int deformedCount = 0;

    switch (activeBrush) {
        case BRUSH_FLATTEN: {
            glm::vec3 areaNormal = cachedAreaNormal;
            glm::vec3 areaCenter = cachedAreaCenter;

            if (!getCurrentSettings().flattenLockNormal || !getCurrentSettings().flattenLockOrigin) {
                std::vector<float> areaResults(7, 0.0f);
                if (computeAreaNormalAndCenter(
                    mesh->verts.data(),
                    mesh->normals.data(),
                    mesh->materials.data(),
                    pickedVertices.data(),
                    pickedVertices.size(),
                    areaResults.data()
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
                localRadius, intensity, getCurrentSettings().hardness,
                negative,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
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

            if (!getCurrentSettings().flattenLockNormal || !getCurrentSettings().flattenLockOrigin) {
                std::vector<float> areaResults(7, 0.0f);
                if (computeAreaNormalAndCenter(
                    mesh->verts.data(),
                    mesh->normals.data(),
                    mesh->materials.data(),
                    pickedVertices.data(),
                    pickedVertices.size(),
                    areaResults.data()
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

            if (!getCurrentSettings().flattenLockNormal || !getCurrentSettings().flattenLockOrigin) {
                std::vector<float> areaResults(7, 0.0f);
                if (computeAreaNormalAndCenter(
                    mesh->verts.data(),
                    mesh->normals.data(),
                    mesh->materials.data(),
                    pickedVertices.data(),
                    pickedVertices.size(),
                    areaResults.data()
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
                localRadius, intensity * 0.1f,
                negative, getCurrentSettings().accumulate, getCurrentSettings().lockPosition,
                getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
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
                false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
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

            glm::mat4 invMeshMatrix = glm::inverse(mesh->matrix);
            glm::mat4 camWorld = glm::inverse(scene.getCamera().getViewMatrix());
            glm::vec3 camRightLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[0]), 0.0f)));
            glm::vec3 camUpLocal = glm::normalize(glm::vec3(invMeshMatrix * glm::vec4(glm::vec3(camWorld[1]), 0.0f)));

            glm::vec3 sqX = glm::normalize(camRightLocal - areaNormal * glm::dot(camRightLocal, areaNormal));
            glm::vec3 sqY = glm::normalize(glm::cross(areaNormal, sqX));

            float alphaLookAt[16] = {0.0f};
            alphaLookAt[0] = sqX.x; alphaLookAt[4] = sqX.y; alphaLookAt[8] = sqX.z;  alphaLookAt[12] = -glm::dot(sqX, currentIntersection);
            alphaLookAt[1] = sqY.x; alphaLookAt[5] = sqY.y; alphaLookAt[9] = sqY.z;  alphaLookAt[13] = -glm::dot(sqY, currentIntersection);

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
                1.0f, 1.0f, localRadius,
                alphaLookAt, false
            );
            break;
        }
        default:
            break;
    }

    if (deformedCount > 0) {
        if (m_tagFlags.size() < (size_t)mesh->nbFaces) {
            m_tagFlags.assign(mesh->nbFaces, 0);
        }
        if (m_iFacesCache.size() < (size_t)mesh->nbFaces) {
            m_iFacesCache.resize(mesh->nbFaces);
        }

        uint32_t numIFaces = getFacesFromVerticesFast(
            pickedVertices.data(),
            pickedVertices.size(),
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
            m_iFacesCache.data(), numIFaces
        );

        uint32_t minV = pickedVertices[0];
        uint32_t maxV = pickedVertices[0];
        for (int i = 1; i < deformedCount; ++i) {
            uint32_t v = pickedVertices[i];
            if (v < minV) minV = v;
            if (v > maxV) maxV = v;
        }
        if (mesh->isVertexDirty) {
            mesh->dirtyVertMin = std::min(mesh->dirtyVertMin, minV);
            mesh->dirtyVertMax = std::max(mesh->dirtyVertMax, maxV);
        } else {
            mesh->dirtyVertMin = minV;
            mesh->dirtyVertMax = maxV;
            mesh->isVertexDirty = true;
        }
    }

    return deformedCount;
}

void SculptManager::executeStroke(Scene& scene, Mesh* mesh, Camera& camera, float mouseX, float mouseY, float currentPressure) {
    Ray ray = camera.getRay(mouseX, mouseY);
    glm::mat4 invMatrix = glm::inverse(mesh->matrix);
    glm::vec3 localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
    glm::vec3 localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

    BrushType activeBrush = m_currentBrush;
    if (SDL_GetModState() & KMOD_SHIFT) {
        activeBrush = BRUSH_SMOOTH;
    } else if (SDL_GetModState() & KMOD_CTRL) {
        activeBrush = BRUSH_MASK;
    }

    bool isGrabBrush = (activeBrush == BRUSH_MOVE || activeBrush == BRUSH_DRAG || activeBrush == BRUSH_ELASTIC);

    uint32_t intersectFaceId = 0xffffffff;

    if (isGrabBrush && !m_firstStrokeFrame) {
        // Bypass raycast for grab brushes on subsequent frames
        glm::vec3 rayNear = localRayOrigin;
        glm::vec3 rayFar = localRayOrigin + localRayDir;
        glm::vec3 dragDir = vertexOnLine(m_initialIntersection, rayNear, rayFar) - m_initialIntersection;
        m_currentIntersection = m_initialIntersection + dragDir;
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
    }

    glm::vec3 cameraPos = camera.computePosition();
    glm::vec3 worldIntersection = glm::vec3(mesh->matrix * glm::vec4(m_currentIntersection, 1.0f));
    float hitDepth = glm::distance(cameraPos, worldIntersection);

    float worldRadius = 0.0f;
    float brushRadius = getCurrentSettings().radius;
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
    float intensity = getCurrentSettings().intensity * currentPressure;
    float radius2 = localRadius * localRadius;

    std::vector<uint32_t> pickedVertices;
    if (isGrabBrush && !m_firstStrokeFrame) {
        pickedVertices = m_grabbedVertices;
    } else {
        if (getCurrentSettings().topoCheck) {
            pickedVertices = pickVerticesInSphereTopological(mesh, m_currentIntersection, radius2, intersectFaceId);
        } else {
            pickedVertices = mesh->octree.pickVerticesInSphere(
                m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z, radius2, mesh->vertVisible.data()
            );
        }

        if (getCurrentSettings().culling) {
            filterCullingVertices(pickedVertices, mesh, localRayDir);
        }

        if (isGrabBrush) {
            m_grabbedVertices = pickedVertices;
        }
    }

    if (!pickedVertices.empty()) {
        bool altPressed = (SDL_GetModState() & KMOD_ALT) != 0;
        bool negative = getCurrentSettings().negative ^ altPressed;

        // Cache area normal and center for Clay/Flatten brushes on first frame
        if (m_firstStrokeFrame && (activeBrush == BRUSH_CLAY || activeBrush == BRUSH_CLAYBUILDUP || activeBrush == BRUSH_FLATTEN)) {
            std::vector<float> areaResults(7, 0.0f);
            computeAreaNormalAndCenter(
                mesh->verts.data(),
                mesh->normals.data(),
                mesh->materials.data(),
                pickedVertices.data(),
                pickedVertices.size(),
                areaResults.data()
            );
            m_cachedAreaNormal = glm::vec3(areaResults[0], areaResults[1], areaResults[2]);
            m_cachedAreaCenter = glm::vec3(areaResults[3], areaResults[4], areaResults[5]);
        }

        // Primary pass
        doStrokePass(
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
            mouseX
        );

        // Symmetry pass (Step 2)
        if (m_useSym) {
            glm::vec3 symCenter;
            std::vector<uint32_t> symVerts;

            if (isGrabBrush && !m_firstStrokeFrame) {
                // Calculate symmetry drag direction and center
                glm::vec3 symRayOrigin = localRayOrigin;
                glm::vec3 symRayDir = localRayDir;
                if (m_symAxis == 0) {
                    symRayOrigin.x = -symRayOrigin.x;
                    symRayDir.x = -symRayDir.x;
                } else if (m_symAxis == 1) {
                    symRayOrigin.y = -symRayOrigin.y;
                    symRayDir.y = -symRayDir.y;
                } else if (m_symAxis == 2) {
                    symRayOrigin.z = -symRayOrigin.z;
                    symRayDir.z = -symRayDir.z;
                }
                glm::vec3 symDragDir = vertexOnLine(m_initialSymIntersection, symRayOrigin, symRayOrigin + symRayDir) - m_initialSymIntersection;
                symCenter = m_initialSymIntersection + symDragDir;
                symVerts = m_grabbedVerticesSym;
            } else {
                // Reflect center of stroke in mesh's local space
                symCenter = m_currentIntersection;
                if (m_symAxis == 0) {
                    symCenter.x = -symCenter.x;
                } else if (m_symAxis == 1) {
                    symCenter.y = -symCenter.y;
                } else if (m_symAxis == 2) {
                    symCenter.z = -symCenter.z;
                }

                // Gather symmetry vertices in local space
                symVerts = mesh->octree.pickVerticesInSphere(
                    symCenter.x, symCenter.y, symCenter.z,
                    radius2, mesh->vertVisible.data()
                );

                if (getCurrentSettings().culling && !symVerts.empty()) {
                    glm::vec3 symRayDir = localRayDir;
                    if (m_symAxis == 0) symRayDir.x = -symRayDir.x;
                    else if (m_symAxis == 1) symRayDir.y = -symRayDir.y;
                    else if (m_symAxis == 2) symRayDir.z = -symRayDir.z;
                    filterCullingVertices(symVerts, mesh, symRayDir);
                }

                if (isGrabBrush) {
                    m_grabbedVerticesSym = symVerts;
                }
            }

            if (!symVerts.empty()) {
                // Mirror cached area normal and center for symmetry pass
                glm::vec3 symAreaNormal = m_cachedAreaNormal;
                glm::vec3 symAreaCenter = m_cachedAreaCenter;
                if (m_symAxis == 0) {
                    symAreaNormal.x = -symAreaNormal.x;
                    symAreaCenter.x = -symAreaCenter.x;
                } else if (m_symAxis == 1) {
                    symAreaNormal.y = -symAreaNormal.y;
                    symAreaCenter.y = -symAreaCenter.y;
                } else if (m_symAxis == 2) {
                    symAreaNormal.z = -symAreaNormal.z;
                    symAreaCenter.z = -symAreaCenter.z;
                }

                // Mirror current intersection normal for symmetry pass
                glm::vec3 symIntersectionNormal = m_currentIntersectionNormal;
                if (m_symAxis == 0) symIntersectionNormal.x = -symIntersectionNormal.x;
                else if (m_symAxis == 1) symIntersectionNormal.y = -symIntersectionNormal.y;
                else if (m_symAxis == 2) symIntersectionNormal.z = -symIntersectionNormal.z;

                doStrokePass(
                    scene,
                    mesh,
                    activeBrush,
                    negative,
                    symVerts,
                    symCenter,
                    symIntersectionNormal,
                    m_initialSymIntersection,
                    symAreaNormal,
                    symAreaCenter,
                    localRadius,
                    intensity,
                    mouseX
                );
            }
        }
    }
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

            if (mod & KMOD_ALT) {
                float minT = std::numeric_limits<float>::infinity();
                Mesh* closestMesh = nullptr;

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
                    bool hitThisMesh = false;

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
                                hitThisMesh = true;
                            }
                        }

                        if (v3Id != 0xffffffff) {
                            glm::vec3 v3(m->verts[v3Id * 3], m->verts[v3Id * 3 + 1], m->verts[v3Id * 3 + 2]);
                            if (rayTriangleIntersect(localRayOrigin, localRayDir, v0, v2, v3, t)) {
                                if (t < localMinT) {
                                    localMinT = t;
                                    hitThisMesh = true;
                                }
                            }
                        }
                    }

                    if (hitThisMesh) {
                        glm::vec3 worldHit = glm::vec3(m->matrix * glm::vec4(localRayOrigin + localMinT * localRayDir, 1.0f));
                        float worldT = glm::distance(ray.origin, worldHit);
                        if (worldT < minT) {
                            minT = worldT;
                            closestMesh = m;
                        }
                    }
                }

                if (closestMesh) {
                    if (closestMesh != scene.getSelected()) {
                        scene.setOrUnsetMesh(closestMesh, false);
                        return;
                    }
                } else {
                    return;
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
                scene.pushHistoryState();
                m_isSculpting = true;
                m_currentIntersectionValid = true;
                m_firstStrokeFrame = true;
                m_initialIntersection = localRayOrigin + minT * localRayDir;
                m_initialIntersectionNormal = glm::vec3(
                    mesh->faceNormals[intersectFaceId * 3],
                    mesh->faceNormals[intersectFaceId * 3 + 1],
                    mesh->faceNormals[intersectFaceId * 3 + 2]
                );
                m_currentIntersection = m_initialIntersection;
                m_currentIntersectionNormal = m_initialIntersectionNormal;

                m_initialSymIntersection = m_initialIntersection;
                if (m_symAxis == 0) m_initialSymIntersection.x = -m_initialSymIntersection.x;
                else if (m_symAxis == 1) m_initialSymIntersection.y = -m_initialSymIntersection.y;
                else if (m_symAxis == 2) m_initialSymIntersection.z = -m_initialSymIntersection.z;

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
                    float nPlaneX = (m_symAxis == 0) ? 1.0f : 0.0f;
                    float nPlaneY = (m_symAxis == 1) ? 1.0f : 0.0f;
                    float nPlaneZ = (m_symAxis == 2) ? 1.0f : 0.0f;

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
            if (m_lassoPoints.size() >= 3 && mesh) {
                std::vector<uint32_t> selectedVertices = getVerticesInLasso(mesh, camera);
                if (m_isMaskLasso) {
                    scene.pushHistoryState();
                    if (!selectedVertices.empty()) {
                        float maskVal = m_lassoAlt ? 1.0f : 0.0f;
                        float* materials = mesh->materials.data();
                        for (uint32_t vid : selectedVertices) {
                            materials[vid * 3 + 2] = maskVal;
                        }
                        mesh->isVertexDirty = true;
                        mesh->dirtyVertMin = 0;
                        mesh->dirtyVertMax = mesh->nbVerts - 1;
                    } else {
                        clearMask(mesh);
                    }
                    mesh->isDirty = true;
                } else {
                    if (!selectedVertices.empty()) {
                        scene.pushHistoryState();
                        bool hideUnselected = !m_lassoAlt;
                        if (hideUnselected) {
                            std::fill(mesh->vertVisible.begin(), mesh->vertVisible.end(), 0);
                            for (uint32_t vid : selectedVertices) {
                                mesh->vertVisible[vid] = 1;
                            }
                        } else {
                            for (uint32_t vid : selectedVertices) {
                                mesh->vertVisible[vid] = 0;
                            }
                        }
                        mesh->isDirty = true;
                    }
                }
            } else if (m_lassoPoints.size() < 3) {
                // Click action
                Ray ray = camera.getRay((float)event.button.x, (float)event.button.y);
                bool hitMesh = false;
                uint32_t closestVert = 0xffffffff;
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
                }

                if (m_isMaskLasso) {
                    if (!hitMesh && mesh) {
                        scene.pushHistoryState();
                        invertMask(mesh);
                    } else if (hitMesh && mesh) {
                        SDL_Keymod mod = SDL_GetModState();
                        bool ctrlKey = (mod & KMOD_CTRL) != 0;
                        bool altKey = (mod & KMOD_ALT) != 0;

                        scene.pushHistoryState();
                        if (bestMask < 1.0f) {
                            if (ctrlKey && altKey) {
                                sharpenMask(mesh);
                            } else {
                                blurMask(mesh);
                            }
                        } else {
                            sharpenMask(mesh);
                        }
                    }
                } else {
                    if (!hitMesh && mesh) {
                        scene.pushHistoryState();
                        std::fill(mesh->vertVisible.begin(), mesh->vertVisible.end(), 1);
                        mesh->isDirty = true;
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
            bool wasClick = (dragDistX <= 3 && dragDistY <= 3);

            if (wasClick && m_currentBrush == BRUSH_MASK && mesh) {
                // Undo the tiny stroke we started on mouse down
                scene.undo();

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

                    scene.pushHistoryState();
                    if (bestMask < 1.0f) {
                        if (ctrlKey && altKey) {
                            sharpenMask(mesh);
                        } else {
                            blurMask(mesh);
                        }
                    } else {
                        sharpenMask(mesh);
                    }
                }
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
            float nPlaneX = (m_symAxis == 0) ? 1.0f : 0.0f;
            float nPlaneY = (m_symAxis == 1) ? 1.0f : 0.0f;
            float nPlaneZ = (m_symAxis == 2) ? 1.0f : 0.0f;

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
            float minSpacing = getCurrentSettings().spacing * getCurrentSettings().radius;

            if (minSpacing <= 0.0f) {
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
        if ((mod & KMOD_CTRL) && (mod & KMOD_SHIFT)) {
            activeBrush = BRUSH_VISIBILITY;
        } else if (mod & KMOD_SHIFT) {
            activeBrush = BRUSH_SMOOTH;
        } else if (mod & KMOD_CTRL) {
            activeBrush = BRUSH_MASK;
        }

        const auto& activeSettings = getSettings(activeBrush);
        m_cursor.update(
            m_rawMouseX, m_rawMouseY,
            scene,
            getBrushRadius(),
            m_useSym,
            m_symAxis,
            m_isSculpting,
            activeBrush,
            m_isSculpting ? m_hasAnyValidIntersection : m_currentIntersectionValid,
            m_isSculpting ? m_lastValidIntersection : m_currentIntersection,
            m_isSculpting ? m_lastValidIntersectionNormal : m_currentIntersectionNormal,
            activeSettings.focalShift,
            activeSettings.hardness
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

    for (int i = 0; i < 22; ++i) {
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
        out << "blurMaskedOnly=" << (m_brushSettings[i].blurMaskedOnly ? "true" : "false") << "\n\n";
    }

    out << "[General]\n";
    out << "dividerDivisions=" << m_dividerDivisions << "\n";
    out << "measureUseDistanceThickness=" << (m_measureUseDistanceThickness ? "true" : "false") << "\n";
#ifdef _WIN32
    out << "usePressure=" << (g_tablet.isPressureEnabled() ? "true" : "false") << "\n";
    out << "usePressureSize=" << (g_tablet.isPressureSizeEnabled() ? "true" : "false") << "\n";
    out << "usePressureCursor=" << (g_tablet.isPressureCursorEnabled() ? "true" : "false") << "\n";
    out << "useTilt=" << (g_tablet.isTiltEnabled() ? "true" : "false") << "\n";
#endif
    out << "\n";

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

    for (int i = 0; i < 22; ++i) {
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
#endif
    }

    std::cout << "Successfully loaded brush settings from: " << filepath << std::endl;
    return true;
}

void SculptManager::clearMask(Mesh* mesh) {
    if (!mesh) return;
    float* materials = mesh->materials.data();
    int nbVerts = mesh->nbVerts;
    for (int i = 0; i < nbVerts; ++i) {
        materials[i * 3 + 2] = 1.0f;
    }
    mesh->isVertexDirty = true;
    mesh->dirtyVertMin = 0;
    mesh->dirtyVertMax = nbVerts - 1;
}

void SculptManager::invertMask(Mesh* mesh) {
    if (!mesh) return;
    float* materials = mesh->materials.data();
    int nbVerts = mesh->nbVerts;
    for (int i = 0; i < nbVerts; ++i) {
        materials[i * 3 + 2] = 1.0f - materials[i * 3 + 2];
    }
    mesh->isVertexDirty = true;
    mesh->dirtyVertMin = 0;
    mesh->dirtyVertMax = nbVerts - 1;
}

void SculptManager::blurMask(Mesh* mesh) {
    if (!mesh) return;
    float* mAr = mesh->materials.data();
    int nbVerts = mesh->nbVerts;

    std::vector<uint32_t> iVerts;
    iVerts.reserve(nbVerts);
    for (int i = 0; i < nbVerts; ++i) {
        if (mAr[i * 3 + 2] < 1.0f) {
            iVerts.push_back(i);
        }
    }
    if (iVerts.empty()) return;

    int iterations = m_brushSettings[m_currentBrush].maskSharpenBlurIterations;
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
        mAr
    );

    mesh->isVertexDirty = true;
    mesh->dirtyVertMin = 0;
    mesh->dirtyVertMax = nbVerts - 1;
}

void SculptManager::sharpenMask(Mesh* mesh) {
    if (!mesh) return;
    float* mAr = mesh->materials.data();
    int nbVerts = mesh->nbVerts;

    std::vector<uint32_t> iVerts;
    iVerts.reserve(nbVerts);
    for (int i = 0; i < nbVerts; ++i) {
        if (mAr[i * 3 + 2] < 1.0f) {
            iVerts.push_back(i);
        }
    }
    if (iVerts.empty()) return;

    int iterations = m_brushSettings[m_currentBrush].maskSharpenBlurIterations;
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

    int nbActiveVerts = (int)iVerts.size();
    std::vector<float> originalMask(nbActiveVerts);
    for (int i = 0; i < nbActiveVerts; ++i) {
        originalMask[i] = mAr[iVerts[i] * 3 + 2];
    }

    ::blurMask(
        iVerts.data(), nbActiveVerts,
        mesh->vrvStartCount.data(),
        mesh->vertRingVert.data(),
        mesh->vertOnEdge.data(),
        iterations,
        mAr
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

    mesh->isVertexDirty = true;
    mesh->dirtyVertMin = 0;
    mesh->dirtyVertMax = nbVerts - 1;
}

void SculptManager::applyPreset(const BrushPreset& preset) {
    BrushType targetBrush = m_currentBrush;
    switch (preset.deformMode) {
        case DeformMode::Normal:
            targetBrush = BRUSH_SQUAREBRUSH;
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


