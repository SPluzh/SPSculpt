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
    for (int i = 0; i < 18; ++i) {
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
}

void SculptManager::executeStroke(Scene& scene, Mesh* mesh, Camera& camera, float mouseX, float mouseY, float currentPressure) {
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

    float localRadius = (worldRadius / scale) * (0.4f + 0.6f * currentPressure);
    float intensity = getCurrentSettings().intensity * currentPressure;
    float radius2 = localRadius * localRadius;

    std::vector<uint32_t> pickedVertices;
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

    if (!pickedVertices.empty()) {
        BrushType activeBrush = m_currentBrush;
        if (SDL_GetModState() & KMOD_SHIFT) {
            activeBrush = BRUSH_SMOOTH;
        } else if (SDL_GetModState() & KMOD_CTRL) {
            activeBrush = BRUSH_MASK;
        }

        bool altPressed = (SDL_GetModState() & KMOD_ALT) != 0;
        bool negative = getCurrentSettings().negative ^ altPressed;

        int deformedCount = 0;

        switch (activeBrush) {
            case BRUSH_FLATTEN: {
                glm::vec3 areaCenter = m_currentIntersection;
                glm::vec3 areaNormal = m_currentIntersectionNormal;
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

                deformedCount = strokeFlatten(
                    mesh->verts.data(),
                    mesh->vertProxy.data(),
                    mesh->materials.data(),
                    pickedVertices.data(),
                    pickedVertices.size(),
                    m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
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
                    m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
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
                    m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
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
                    m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
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
                    m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
                    m_currentIntersectionNormal.x, m_currentIntersectionNormal.y, m_currentIntersectionNormal.z,
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
                    m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
                    m_currentIntersectionNormal.x, m_currentIntersectionNormal.y, m_currentIntersectionNormal.z,
                    localRadius, intensity,
                    negative,
                    -0.4f, true,
                    false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                    false, nullptr
                );
                break;
            }
            case BRUSH_MOVE: {
                glm::vec3 dragDirection = m_currentIntersection - m_initialIntersection;
                deformedCount = strokeMove(
                    mesh->verts.data(),
                    mesh->vertProxy.data(),
                    mesh->materials.data(),
                    pickedVertices.data(),
                    pickedVertices.size(),
                    m_initialIntersection.x, m_initialIntersection.y, m_initialIntersection.z,
                    dragDirection.x, dragDirection.y, dragDirection.z,
                    localRadius,
                    getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                    false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                    false, nullptr
                );
                break;
            }
            case BRUSH_DRAG: {
                glm::vec3 dragDirection = m_currentIntersection - m_initialIntersection;
                deformedCount = strokeDrag(
                    mesh->verts.data(),
                    mesh->materials.data(),
                    pickedVertices.data(),
                    pickedVertices.size(),
                    m_initialIntersection.x, m_initialIntersection.y, m_initialIntersection.z,
                    dragDirection.x, dragDirection.y, dragDirection.z,
                    localRadius,
                    getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                    false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false,
                    false, nullptr
                );
                break;
            }
            case BRUSH_ELASTIC: {
                glm::vec3 dragDirection = m_currentIntersection - m_initialIntersection;
                deformedCount = strokeElastic(
                    mesh->verts.data(),
                    mesh->vertProxy.data(),
                    mesh->materials.data(),
                    pickedVertices.data(),
                    pickedVertices.size(),
                    m_initialIntersection.x, m_initialIntersection.y, m_initialIntersection.z,
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
                    m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
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
                    m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
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
                    m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
                    m_currentIntersectionNormal.x, m_currentIntersectionNormal.y, m_currentIntersectionNormal.z,
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
                    m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
                    localRadius, intensity,
                    getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                    false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
                );
                break;
            }
            case BRUSH_CLAY: {
                if (m_firstStrokeFrame) {
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
                glm::vec3 areaNormal = m_cachedAreaNormal;
                glm::vec3 areaCenter = m_cachedAreaCenter;

                float off = localRadius * 0.1f;
                areaCenter += areaNormal * (negative ? -off : off);

                deformedCount = strokeFlatten(
                    mesh->verts.data(),
                    mesh->vertProxy.data(),
                    mesh->materials.data(),
                    pickedVertices.data(),
                    pickedVertices.size(),
                    m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
                    areaCenter.x, areaCenter.y, areaCenter.z,
                    areaNormal.x, areaNormal.y, areaNormal.z,
                    localRadius, intensity,
                    negative, getCurrentSettings().accumulate, getCurrentSettings().lockPosition,
                    0.0f, true,
                    false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
                );
                break;
            }
            case BRUSH_CLAYBUILDUP: {
                if (m_firstStrokeFrame) {
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
                glm::vec3 areaNormal = m_cachedAreaNormal;
                glm::vec3 areaCenter = m_cachedAreaCenter;

                float off = localRadius * 0.1f;
                areaCenter += areaNormal * (negative ? -off : off);

                deformedCount = strokeFlatten(
                    mesh->verts.data(),
                    mesh->vertProxy.data(),
                    mesh->materials.data(),
                    pickedVertices.data(),
                    pickedVertices.size(),
                    m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
                    areaCenter.x, areaCenter.y, areaCenter.z,
                    areaNormal.x, areaNormal.y, areaNormal.z,
                    localRadius, intensity * 0.1f,
                    negative, getCurrentSettings().accumulate, getCurrentSettings().lockPosition,
                    0.0f, true,
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
                    m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
                    m_currentIntersectionNormal.x, m_currentIntersectionNormal.y, m_currentIntersectionNormal.z,
                    localRadius, intensity,
                    negative,
                    getCurrentSettings().focalShift, getCurrentSettings().focalShiftFalloff,
                    false, nullptr, 0, 0, 0.0f, 0.0f, 0.0f, nullptr, false
                );
                break;
            }
            case BRUSH_SQUAREBRUSH: {
                glm::vec3 areaCenter = m_currentIntersection;
                glm::vec3 areaNormal = m_currentIntersectionNormal;
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
                alphaLookAt[0] = sqX.x; alphaLookAt[4] = sqX.y; alphaLookAt[8] = sqX.z;  alphaLookAt[12] = -glm::dot(sqX, m_currentIntersection);
                alphaLookAt[1] = sqY.x; alphaLookAt[5] = sqY.y; alphaLookAt[9] = sqY.z;  alphaLookAt[13] = -glm::dot(sqY, m_currentIntersection);

                deformedCount = strokeSquareBrush(
                    mesh->verts.data(),
                    mesh->materials.data(),
                    pickedVertices.data(),
                    pickedVertices.size(),
                    m_currentIntersection.x, m_currentIntersection.y, m_currentIntersection.z,
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
    }
    m_firstStrokeFrame = false;
}

void SculptManager::handleEvent(const SDL_Event& event, Scene& scene) {
    Mesh* mesh = scene.getSelected();
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
            m_cameraController.handleEvent(event, camera);
            return;
        }

        // Left button:
        if (event.button.button == SDL_BUTTON_LEFT) {
            SDL_Keymod mod = SDL_GetModState();
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

                m_lastStrokeX = mouseX;
                m_lastStrokeY = mouseY;

                // Copy vertices to proxy at start of stroke
                mesh->vertProxy = mesh->verts;

                // Check stylus timeout
                if (m_usingStylus && (SDL_GetTicks() - m_lastStylusTime > 1000)) {
                    m_usingStylus = false;
                    m_stylusPressure = 1.0f;
                }
                float currentPressure = m_usingStylus ? m_stylusPressure : 1.0f;

                // Execute first stroke frame immediately
                executeStroke(scene, mesh, camera, (float)mouseX, (float)mouseY, currentPressure);
            } else {
                if (mod & KMOD_ALT) {
                    m_cameraController.startDrag(CameraController::DragMode::Orbit, mouseX, mouseY, camera);
                }
            }
        }
    } else if (event.type == SDL_MOUSEBUTTONUP) {
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

        if (m_isLassoActive) {
            if (m_lassoPoints.empty() || m_lassoPoints.back() != glm::vec2((float)mouseX, (float)mouseY)) {
                m_lassoPoints.push_back(glm::vec2((float)mouseX, (float)mouseY));
            }
            m_lassoAlt = (SDL_GetModState() & KMOD_ALT) != 0;
            return;
        }

        if (m_cameraController.isDragging()) {
            m_cameraController.handleEvent(event, camera);
        } else if (m_isSculpting && mesh) {
            // Check stylus timeout
            if (m_usingStylus && (SDL_GetTicks() - m_lastStylusTime > 1000)) {
                m_usingStylus = false;
                m_stylusPressure = 1.0f;
            }
            float currentPressure = m_usingStylus ? m_stylusPressure : 1.0f;

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
        // Resolve dynamic brush modifier swap (Shift->Smooth, Ctrl->Mask)
        BrushType activeBrush = m_currentBrush;
        SDL_Keymod mod = SDL_GetModState();
        if (mod & KMOD_SHIFT) {
            activeBrush = BRUSH_SMOOTH;
        } else if (mod & KMOD_CTRL) {
            activeBrush = BRUSH_MASK;
        }

        m_cursor.update(
            m_rawMouseX, m_rawMouseY,
            scene,
            getBrushRadius(),
            m_useSym,
            m_symAxis,
            m_isSculpting,
            activeBrush,
            m_isSculpting && m_currentIntersectionValid,
            m_currentIntersection,
            m_currentIntersectionNormal
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

    for (int i = 0; i < 18; ++i) {
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
        out << "maskExtractThickness=" << m_brushSettings[i].maskExtractThickness << "\n\n";
    }

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

    for (int i = 0; i < 18; ++i) {
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
        }
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


