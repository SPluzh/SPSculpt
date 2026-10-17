#include "editing/TubeTool.h"
#include "editing/SculptManager.h"
#include "gui/GuiManager.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "mesh/Mesh.h"
#include "mesh/Topology.h"
#include "common/Logger.h"

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

TubeTool::TubeTool(SculptManager& sm)
    : m_sculpt(sm) {
    resetRadiusProfile();
    resetCrossSectionProfile();
}

TubeTool::~TubeTool() {
}

void TubeTool::onActivate(Scene& scene) {
    Mesh* sel = scene.getSelected();
    if (sel && sel->isTube && sel->tubeData) {
        loadFromMesh(sel);
    } else {
        m_baseMesh = sel;
        if (m_activeTubeMesh) {
            syncToActiveMesh();
            m_activeTubeMesh = nullptr;
            m_spline.clear();
        }
    }
    m_hoveredPointIdx = -1;
    m_selectedPointIdx = -1;
    m_hoveredProfilePtIdx = -1;
    m_selectedProfilePtIdx = -1;
    m_snapTargetProfilePtIdx = -1;
    m_hoveredProfileSeg = -1;
    m_isDraggingPoint = false;
    m_isDraggingRadius = false;
    m_isDrawingStroke = false;
    m_isDraggingProfilePoint = false;
}

void TubeTool::onDeactivate() {
    syncToActiveMesh();
    m_isDrawingStroke = false;
    m_isDraggingPoint = false;
    m_isDraggingRadius = false;
    m_isDraggingProfilePoint = false;
    m_hoveredPointIdx = -1;
    m_hoveredProfilePtIdx = -1;
    m_hoveredProfileSeg = -1;
    m_snapTargetProfilePtIdx = -1;
}

void TubeTool::syncToActiveMesh() {
    if (!m_activeTubeMesh) return;
    m_activeTubeMesh->isTube = true;
    if (!m_activeTubeMesh->tubeData) {
        m_activeTubeMesh->tubeData = std::make_unique<TubeData>();
    }
    TubeData& td = *m_activeTubeMesh->tubeData;
    td.spline = m_spline;
    td.creationMode = creationMode;
    td.profileType = profileType;
    td.capType = capType;
    td.strokeSmoothing = strokeSmoothing;
    td.curveFitTolerance = curveFitTolerance;
    td.baseRadius = baseRadius;
    td.startRadiusScale = startRadiusScale;
    td.endRadiusScale = endRadiusScale;
    td.curveSegments = curveSegments;
    td.autoDensity = autoDensity;
    td.cornerSegments = cornerSegments;
    td.radialSegments = radialSegments;
    td.ribbonThickness = ribbonThickness;
    td.twistDegrees = twistDegrees;
    td.snapToSurface = snapToSurface;
    td.surfaceOffset = surfaceOffset;
    td.useRadiusProfile = useRadiusProfile;
    td.radiusProfile = radiusProfile;
    td.radiusProfileSmooth = radiusProfileSmooth;
    td.crossSectionProfile = crossSectionProfile;
    td.crossSectionSmooth = crossSectionSmooth;
    td.showProfileAtEnd = showProfileAtEnd;
}

bool TubeTool::loadFromMesh(Mesh* mesh) {
    if (!mesh || !mesh->isTube || !mesh->tubeData) return false;
    if (isInteracting()) return false;

    if (m_activeTubeMesh == mesh) return true;

    if (m_activeTubeMesh && m_activeTubeMesh != mesh) {
        syncToActiveMesh();
    }

    m_activeTubeMesh = mesh;
    const TubeData& td = *mesh->tubeData;
    m_spline = td.spline;
    creationMode = td.creationMode;
    profileType = td.profileType;
    capType = td.capType;
    strokeSmoothing = td.strokeSmoothing;
    curveFitTolerance = td.curveFitTolerance;
    baseRadius = td.baseRadius;
    startRadiusScale = td.startRadiusScale;
    endRadiusScale = td.endRadiusScale;
    curveSegments = td.curveSegments;
    autoDensity = td.autoDensity;
    cornerSegments = td.cornerSegments;
    radialSegments = td.radialSegments;
    ribbonThickness = td.ribbonThickness;
    twistDegrees = td.twistDegrees;
    snapToSurface = td.snapToSurface;
    surfaceOffset = td.surfaceOffset;
    useRadiusProfile = td.useRadiusProfile;
    radiusProfile = td.radiusProfile;
    if (radiusProfile.empty()) resetRadiusProfile();
    radiusProfileSmooth = td.radiusProfileSmooth;
    crossSectionProfile = td.crossSectionProfile;
    if (crossSectionProfile.empty()) resetCrossSectionProfile();
    crossSectionSmooth = td.crossSectionSmooth;
    showProfileAtEnd = td.showProfileAtEnd;

    m_selectedPointIdx = -1;
    m_hoveredPointIdx = -1;
    m_snapTargetPointIdx = -1;
    m_hoveredCurveSeg = -1;
    m_hoveredProfilePtIdx = -1;
    m_selectedProfilePtIdx = -1;
    m_snapTargetProfilePtIdx = -1;
    m_hoveredProfileSeg = -1;
    m_isDraggingPoint = false;
    m_isDraggingRadius = false;
    m_isDrawingStroke = false;
    m_isExtrudingPoint = false;
    m_extrudeEndIdx = -1;
    m_isDraggingProfilePoint = false;
    m_profilePointWasMoved = false;
    m_geometryDirty = false;

    return true;
}

void TubeTool::startNewTube(Scene& scene) {
    if (m_activeTubeMesh) {
        syncToActiveMesh();
        m_activeTubeMesh = nullptr;
    }
    m_spline.clear();
    m_rawStroke.clear();
    m_strokeStartHitMesh = nullptr;
    m_selectedPointIdx = -1;
    m_hoveredPointIdx = -1;
    m_snapTargetPointIdx = -1;
    m_hoveredCurveSeg = -1;
    m_hoveredProfilePtIdx = -1;
    m_selectedProfilePtIdx = -1;
    m_snapTargetProfilePtIdx = -1;
    m_hoveredProfileSeg = -1;
    m_isDraggingPoint = false;
    m_isDraggingRadius = false;
    m_isDrawingStroke = false;
    m_isExtrudingPoint = false;
    m_extrudeEndIdx = -1;
    m_isDraggingProfilePoint = false;
    m_profilePointWasMoved = false;
    m_geometryDirty = false;
}

void TubeTool::notifyMeshDeleted(Mesh* mesh) {
    if (m_strokeStartHitMesh == mesh) {
        m_strokeStartHitMesh = nullptr;
    }
    if (m_activeTubeMesh == mesh) {
        m_activeTubeMesh = nullptr;
        m_spline.clear();
        m_rawStroke.clear();
        m_selectedPointIdx = -1;
        m_hoveredPointIdx = -1;
        m_snapTargetPointIdx = -1;
        m_hoveredCurveSeg = -1;
        m_hoveredProfilePtIdx = -1;
        m_selectedProfilePtIdx = -1;
        m_snapTargetProfilePtIdx = -1;
        m_hoveredProfileSeg = -1;
        m_isDraggingPoint = false;
        m_isDraggingRadius = false;
        m_isDrawingStroke = false;
        m_isExtrudingPoint = false;
        m_extrudeEndIdx = -1;
        m_isDraggingProfilePoint = false;
        m_profilePointWasMoved = false;
        m_geometryDirty = false;
    }
    if (m_baseMesh == mesh) {
        m_baseMesh = nullptr;
    }
}

void TubeTool::clearCurve(Scene* scene) {
    m_spline.clear();
    m_rawStroke.clear();
    m_strokeStartHitMesh = nullptr;
    m_hoveredPointIdx = -1;
    m_selectedPointIdx = -1;
    m_snapTargetPointIdx = -1;
    m_hoveredCurveSeg = -1;
    m_hoveredProfilePtIdx = -1;
    m_selectedProfilePtIdx = -1;
    m_snapTargetProfilePtIdx = -1;
    m_hoveredProfileSeg = -1;
    m_isDraggingPoint = false;
    m_isDraggingRadius = false;
    m_isDrawingStroke = false;
    m_isExtrudingPoint = false;
    m_extrudeEndIdx = -1;
    m_isDraggingProfilePoint = false;
    m_profilePointWasMoved = false;
    m_geometryDirty = false;

    if (m_activeTubeMesh) {
        if (scene) {
            scene->removeMesh(m_activeTubeMesh);
        }
        m_activeTubeMesh = nullptr;
    }

    if (scene && m_baseMesh) {
        const auto& meshes = scene->getMeshes();
        if (std::find(meshes.begin(), meshes.end(), m_baseMesh) != meshes.end()) {
            scene->selectMesh(m_baseMesh);
        }
    }
}

static bool rayTriangleIntersect(
    const glm::vec3& rayOrigin, const glm::vec3& rayDir,
    const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
    float& t
) {
    const float EPSILON = 1e-7f;
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

bool TubeTool::raycastSurface(
    const Scene& scene, const Camera& camera, float mouseX, float mouseY,
    glm::vec3& outPos, glm::vec3& outNorm, Mesh** outHitMesh
) const {
    Ray ray = camera.getRay(mouseX, mouseY);

    float minT = std::numeric_limits<float>::infinity();
    glm::vec3 bestPos(0.0f);
    glm::vec3 bestNorm(0.0f, 1.0f, 0.0f);
    Mesh* bestMesh = nullptr;
    bool hitAny = false;

    const auto& meshes = scene.getMeshes();
    for (Mesh* mesh : meshes) {
        if (!mesh || mesh == m_activeTubeMesh || !mesh->visibleV1) continue;
        if (mesh->nbFaces == 0 || mesh->verts.empty()) continue;

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

            glm::vec3 v0(mesh->verts[v0Id * 3], mesh->verts[v0Id * 3 + 1], mesh->verts[v0Id * 3 + 2]);
            glm::vec3 v1(mesh->verts[v1Id * 3], mesh->verts[v1Id * 3 + 1], mesh->verts[v1Id * 3 + 2]);
            glm::vec3 v2(mesh->verts[v2Id * 3], mesh->verts[v2Id * 3 + 1], mesh->verts[v2Id * 3 + 2]);

            float t;
            if (rayTriangleIntersect(localRayOrigin, localRayDir, v0, v1, v2, t)) {
                glm::vec3 localHit = localRayOrigin + localRayDir * t;
                glm::vec3 worldHit = glm::vec3(mesh->matrix * glm::vec4(localHit, 1.0f));
                float worldT = glm::distance(ray.origin, worldHit);
                if (worldT < minT) {
                    minT = worldT;
                    bestPos = worldHit;
                    bestMesh = mesh;
                    glm::vec3 localNorm = glm::normalize(glm::cross(v1 - v0, v2 - v0));
                    bestNorm = glm::normalize(glm::vec3(glm::transpose(invMatrix) * glm::vec4(localNorm, 0.0f)));
                    hitAny = true;
                }
            }

            if (v3Id != 0xffffffff && v3Id < (uint32_t)mesh->nbVerts) {
                glm::vec3 v3(mesh->verts[v3Id * 3], mesh->verts[v3Id * 3 + 1], mesh->verts[v3Id * 3 + 2]);
                if (rayTriangleIntersect(localRayOrigin, localRayDir, v0, v2, v3, t)) {
                    glm::vec3 localHit = localRayOrigin + localRayDir * t;
                    glm::vec3 worldHit = glm::vec3(mesh->matrix * glm::vec4(localHit, 1.0f));
                    float worldT = glm::distance(ray.origin, worldHit);
                    if (worldT < minT) {
                        minT = worldT;
                        bestPos = worldHit;
                        bestMesh = mesh;
                        glm::vec3 localNorm = glm::normalize(glm::cross(v2 - v0, v3 - v0));
                        bestNorm = glm::normalize(glm::vec3(glm::transpose(invMatrix) * glm::vec4(localNorm, 0.0f)));
                        hitAny = true;
                    }
                }
            }
        }
    }

    if (hitAny) {
        outPos = bestPos;
        outNorm = bestNorm;
        if (outHitMesh) *outHitMesh = bestMesh;
        return true;
    }
    return false;
}

glm::vec3 TubeTool::intersectRayPlane(
    const Camera& camera, float mouseX, float mouseY,
    const glm::vec3& planePoint, const glm::vec3& planeNormal
) const {
    Ray ray = camera.getRay(mouseX, mouseY);
    float denom = glm::dot(planeNormal, ray.dir);
    if (std::abs(denom) > 1e-6f) {
        float t = glm::dot(planePoint - ray.origin, planeNormal) / denom;
        return ray.origin + ray.dir * t;
    }
    return planePoint;
}

int TubeTool::hitTestTubeControlPoints(const Camera& camera, const CurveSpline& spline, float mouseX, float mouseY, float thresholdPx) const {
    float bestDist = thresholdPx;
    int bestIdx = -1;

    for (size_t i = 0; i < spline.getPointCount(); ++i) {
        glm::vec3 screenPos = camera.project(spline.getPoint(i).position);
        if (screenPos.z < 0.0f || screenPos.z > 1.0f) continue;

        float dist = glm::distance(glm::vec2(mouseX, mouseY), glm::vec2(screenPos.x, screenPos.y));
        if (dist < bestDist) {
            bestDist = dist;
            bestIdx = static_cast<int>(i);
        }
    }
    return bestIdx;
}

int TubeTool::hitTestControlPoints(const Camera& camera, float mouseX, float mouseY, float thresholdPx) const {
    return hitTestTubeControlPoints(camera, m_spline, mouseX, mouseY, thresholdPx);
}

bool TubeTool::getPointHandleInfo(
    const Camera& camera, int ptIdx,
    glm::vec3& outCenterWorld, glm::vec3& outHandleWorld,
    glm::vec2& outCenterScreen, glm::vec2& outHandleScreen,
    glm::vec3& outSideDir
) const {
    if (ptIdx < 0 || ptIdx >= static_cast<int>(m_spline.getPointCount())) return false;

    const auto& pt = m_spline.getPoint(ptIdx);
    outCenterWorld = pt.position;

    glm::vec3 sCenter = camera.project(outCenterWorld);
    if (sCenter.z < 0.0f || sCenter.z > 1.0f) return false;
    outCenterScreen = glm::vec2(sCenter.x, sCenter.y);

    size_t n = m_spline.getPointCount();
    glm::vec3 tangent(0.0f, 0.0f, 1.0f);
    if (n >= 2) {
        if (ptIdx == 0) {
            tangent = m_spline.getPoint(1).position - pt.position;
        } else if (ptIdx == static_cast<int>(n) - 1) {
            tangent = pt.position - m_spline.getPoint(n - 2).position;
        } else {
            tangent = m_spline.getPoint(ptIdx + 1).position - m_spline.getPoint(ptIdx - 1).position;
        }
    }
    if (glm::length(tangent) > 1e-5f) {
        tangent = glm::normalize(tangent);
    } else {
        tangent = glm::vec3(0.0f, 0.0f, 1.0f);
    }

    glm::vec3 camPos = camera.computePosition();
    glm::vec3 viewDir = camPos - pt.position;
    if (glm::length(viewDir) > 1e-5f) {
        viewDir = glm::normalize(viewDir);
    } else {
        viewDir = glm::vec3(0.0f, 0.0f, 1.0f);
    }

    glm::vec3 side = glm::cross(tangent, viewDir);
    if (glm::length(side) < 1e-4f) {
        side = glm::cross(viewDir, glm::vec3(0.0f, 1.0f, 0.0f));
    }
    if (glm::length(side) < 1e-4f) {
        side = glm::vec3(1.0f, 0.0f, 0.0f);
    }
    side = glm::normalize(side);

    // Ensure consistent screen direction: orient handle towards screen right/top
    glm::vec3 sTest = camera.project(outCenterWorld + side * pt.radius);
    if (sTest.x < outCenterScreen.x - 0.5f) {
        side = -side;
    } else if (std::abs(sTest.x - outCenterScreen.x) <= 0.5f && sTest.y > outCenterScreen.y) {
        side = -side;
    }

    outSideDir = side;
    outHandleWorld = outCenterWorld + side * pt.radius;

    glm::vec3 sHandle = camera.project(outHandleWorld);
    if (sHandle.z < 0.0f || sHandle.z > 1.0f) return false;
    outHandleScreen = glm::vec2(sHandle.x, sHandle.y);

    return true;
}

int TubeTool::hitTestRadiusHandle(const Camera& camera, float mouseX, float mouseY, float thresholdPx) const {
    float bestDist = thresholdPx;
    int bestIdx = -1;
    glm::vec2 mousePos(mouseX, mouseY);

    for (size_t i = 0; i < m_spline.getPointCount(); ++i) {
        glm::vec3 cWorld, hWorld, side;
        glm::vec2 cScreen, hScreen;
        if (!getPointHandleInfo(camera, static_cast<int>(i), cWorld, hWorld, cScreen, hScreen, side)) {
            continue;
        }

        // Distance to square handle
        float distSquare = glm::distance(mousePos, hScreen);
        if (distSquare < bestDist) {
            bestDist = distSquare;
            bestIdx = static_cast<int>(i);
        }

        // Distance to stick line (outer section)
        glm::vec2 ab = hScreen - cScreen;
        float len2 = glm::dot(ab, ab);
        if (len2 > 16.0f) {
            float t = std::clamp(glm::dot(mousePos - cScreen, ab) / len2, 0.0f, 1.0f);
            if (t > 0.35f) {
                glm::vec2 proj = cScreen + t * ab;
                float distLine = glm::distance(mousePos, proj);
                if (distLine < 8.0f && distLine < bestDist) {
                    bestDist = distLine;
                    bestIdx = static_cast<int>(i);
                }
            }
        }
    }
    return bestIdx;
}

bool TubeTool::hitTestRadiusRing(const Camera& camera, float mouseX, float mouseY, int ptIdx, float thresholdPx) const {
    if (ptIdx < 0 || ptIdx >= static_cast<int>(m_spline.getPointCount())) return false;

    const auto& pt = m_spline.getPoint(ptIdx);
    glm::vec3 sCenter = camera.project(pt.position);
    if (sCenter.z < 0.0f || sCenter.z > 1.0f) return false;

    // View direction
    glm::vec3 viewDir = glm::normalize(camera.getPivot() - camera.computePosition());
    glm::vec3 right = glm::normalize(glm::cross(viewDir, glm::vec3(0.0f, 1.0f, 0.0f)));
    if (glm::length(right) < 1e-4f) right = glm::vec3(1.0f, 0.0f, 0.0f);

    glm::vec3 sRadiusPt = camera.project(pt.position + right * pt.radius);
    float screenRadius = glm::distance(glm::vec2(sCenter.x, sCenter.y), glm::vec2(sRadiusPt.x, sRadiusPt.y));
    screenRadius = std::max(screenRadius, 10.0f);

    float mouseDist = glm::distance(glm::vec2(mouseX, mouseY), glm::vec2(sCenter.x, sCenter.y));
    return std::abs(mouseDist - screenRadius) <= thresholdPx;
}

int TubeTool::findClosestSegmentOnSpline(
    const Camera& camera, const CurveSpline& spline, float mouseX, float mouseY, float thresholdPx, float* outT
) const {
    if (spline.getPointCount() < 2) return -1;

    float bestDist = 999999.0f;
    int bestSeg = -1;
    float bestT = 0.5f;

    const int stepsPerSeg = 20;
    int numSegments = static_cast<int>(spline.getPointCount()) - 1;

    glm::vec2 mousePos(mouseX, mouseY);

    for (int seg = 0; seg < numSegments; ++seg) {
        for (int i = 0; i < stepsPerSeg; ++i) {
            float t0 = static_cast<float>(i) / stepsPerSeg;
            float t1 = static_cast<float>(i + 1) / stepsPerSeg;
            float u0 = (static_cast<float>(seg) + t0) / numSegments;
            float u1 = (static_cast<float>(seg) + t1) / numSegments;

            glm::vec3 p0 = spline.evaluatePosition(u0);
            glm::vec3 p1 = spline.evaluatePosition(u1);

            glm::vec3 s0 = camera.project(p0);
            glm::vec3 s1 = camera.project(p1);

            if (s0.z < 0.0f || s0.z > 1.0f || s1.z < 0.0f || s1.z > 1.0f) continue;

            glm::vec2 a(s0.x, s0.y);
            glm::vec2 b(s1.x, s1.y);

            glm::vec2 ab = b - a;
            float len2 = glm::dot(ab, ab);
            float t = (len2 > 1e-6f) ? std::clamp(glm::dot(mousePos - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
            glm::vec2 proj = a + t * ab;

            float subT = t0 + t * (t1 - t0);
            float rApprox = glm::mix(spline.getPoint(seg).radius, spline.getPoint(seg + 1).radius, subT);
            glm::vec3 pMid = glm::mix(p0, p1, t);
            glm::vec3 sOff = camera.project(pMid + glm::vec3(0.0f, rApprox, 0.0f));
            float rPx = glm::distance(glm::vec2(s0.x, s0.y), glm::vec2(sOff.x, sOff.y));
            float effectiveThresh = std::max(thresholdPx, rPx + 8.0f);

            float d = glm::distance(mousePos, proj);
            if (d < effectiveThresh && d < bestDist) {
                bestDist = d;
                bestSeg = seg;
                bestT = subT;
            }
        }
    }

    if (bestSeg != -1 && outT) {
        *outT = bestT;
    }
    return bestSeg;
}

int TubeTool::findClosestSegmentOnCurve(
    const Camera& camera, float mouseX, float mouseY, float thresholdPx, float* outT
) const {
    return findClosestSegmentOnSpline(camera, m_spline, mouseX, mouseY, thresholdPx, outT);
}

bool TubeTool::getEndProfileFrame(EvaluatedFrame& outFrame, int* outPointIdx) const {
    if (!showProfileAtEnd || profileType != TubeProfileType::CustomGraph) return false;
    if (m_spline.getPointCount() < 2) return false;

    bool useStart = (m_selectedPointIdx == 0);
    int ptIdx = useStart ? 0 : (static_cast<int>(m_spline.getPointCount()) - 1);
    if (outPointIdx) *outPointIdx = ptIdx;

    int numSamples = std::clamp(curveSegments, 16, 256);
    std::vector<EvaluatedFrame> frames = m_spline.evaluateFrames(
        numSamples, startRadiusScale, endRadiusScale, 0.0f, twistDegrees, cornerSegments,
        useRadiusProfile, radiusProfile, radiusProfileSmooth
    );
    if (frames.size() < 2) return false;

    if (useStart) {
        outFrame = frames.front();
    } else {
        outFrame = frames.back();
    }
    return true;
}

glm::vec2 TubeTool::evaluateCrossSectionContour(float u) const {
    if (crossSectionProfile.empty()) return glm::vec2(0.0f);
    int N = static_cast<int>(crossSectionProfile.size());
    if (N == 1) return crossSectionProfile[0].pos;
    if (N == 2) {
        float t = glm::fract(u);
        if (t < 0.0f) t += 1.0f;
        return glm::mix(crossSectionProfile[0].pos, crossSectionProfile[1].pos, t);
    }
    float uNorm = glm::fract(u);
    if (uNorm < 0.0f) uNorm += 1.0f;
    float globalU = uNorm * static_cast<float>(N);
    int seg = static_cast<int>(std::floor(globalU));
    float t = globalU - static_cast<float>(seg);
    seg = (seg % N + N) % N;

    int i1 = seg;
    int i2 = (seg + 1) % N;
    const auto& pt1 = crossSectionProfile[i1];
    const auto& pt2 = crossSectionProfile[i2];

    if (!crossSectionSmooth || (pt1.isCorner && pt2.isCorner)) {
        return glm::mix(pt1.pos, pt2.pos, t);
    }

    glm::vec2 p1 = pt1.pos;
    glm::vec2 p2 = pt2.pos;
    glm::vec2 p0 = pt1.isCorner ? (p1 - (p2 - p1)) : crossSectionProfile[(i1 - 1 + N) % N].pos;
    glm::vec2 p3 = pt2.isCorner ? (p2 + (p2 - p1)) : crossSectionProfile[(i2 + 1) % N].pos;

    float t2 = t * t;
    float t3 = t2 * t;

    return 0.5f * (
        2.0f * p1 +
        (-p0 + p2) * t +
        (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
        (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3
    );
}

int TubeTool::hitTestEndProfilePoint(const Camera& camera, float mouseX, float mouseY, float thresholdPx) const {
    EvaluatedFrame frame;
    if (!getEndProfileFrame(frame)) return -1;
    if (crossSectionProfile.empty()) return -1;

    glm::vec2 mousePos(mouseX, mouseY);
    int bestIdx = -1;
    float bestDist = thresholdPx;
    float r = std::max(frame.radius, 0.001f);

    for (int i = 0; i < static_cast<int>(crossSectionProfile.size()); ++i) {
        const auto& pt = crossSectionProfile[i];
        glm::vec3 worldPos = frame.position + (frame.normal * pt.pos.x + frame.binormal * pt.pos.y) * r;
        glm::vec3 screenPos = camera.project(worldPos);
        if (screenPos.z < 0.0f || screenPos.z > 1.0f) continue;

        float d = glm::distance(mousePos, glm::vec2(screenPos.x, screenPos.y));
        if (d < bestDist) {
            bestDist = d;
            bestIdx = i;
        }
    }
    return bestIdx;
}

bool TubeTool::hitTestEndProfileContour(
    const Camera& camera, float mouseX, float mouseY, float thresholdPx,
    int* outSeg, glm::vec2* out2DPos, glm::vec2* outScreenPos
) const {
    EvaluatedFrame frame;
    if (!getEndProfileFrame(frame)) return false;
    int N = static_cast<int>(crossSectionProfile.size());
    if (N < 2) return false;

    glm::vec2 mousePos(mouseX, mouseY);
    float r = std::max(frame.radius, 0.001f);

    int numSamples = std::max(64, N * 16);
    float bestDist = thresholdPx;
    float bestU = 0.0f;
    glm::vec2 bestScreenPos(0.0f);
    bool found = false;

    glm::vec2 p0_2D = evaluateCrossSectionContour(0.0f);
    glm::vec3 prevWorld = frame.position + (frame.normal * p0_2D.x + frame.binormal * p0_2D.y) * r;
    glm::vec3 prevScreen = camera.project(prevWorld);

    for (int k = 1; k <= numSamples; ++k) {
        float u = static_cast<float>(k) / static_cast<float>(numSamples);
        glm::vec2 p2D = evaluateCrossSectionContour(u);
        glm::vec3 currWorld = frame.position + (frame.normal * p2D.x + frame.binormal * p2D.y) * r;
        glm::vec3 currScreen = camera.project(currWorld);

        if (prevScreen.z >= 0.0f && prevScreen.z <= 1.0f && currScreen.z >= 0.0f && currScreen.z <= 1.0f) {
            glm::vec2 a(prevScreen.x, prevScreen.y);
            glm::vec2 b(currScreen.x, currScreen.y);
            glm::vec2 ab = b - a;
            float len2 = glm::dot(ab, ab);
            float t = (len2 > 1e-6f) ? std::clamp(glm::dot(mousePos - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
            glm::vec2 proj = a + t * ab;
            float d = glm::distance(mousePos, proj);
            if (d < bestDist) {
                bestDist = d;
                float uPrev = static_cast<float>(k - 1) / static_cast<float>(numSamples);
                bestU = uPrev + t * (u - uPrev);
                bestScreenPos = proj;
                found = true;
            }
        }
        prevScreen = currScreen;
    }

    if (found) {
        float normU = glm::fract(bestU);
        if (normU < 0.0f) normU += 1.0f;
        float globalU = normU * static_cast<float>(N);
        int seg = static_cast<int>(std::floor(globalU));
        seg = (seg % N + N) % N;
        if (outSeg) *outSeg = seg;
        glm::vec2 p2D = evaluateCrossSectionContour(bestU);
        if (out2DPos) *out2DPos = p2D;
        if (outScreenPos) *outScreenPos = bestScreenPos;
        return true;
    }
    return false;
}

bool TubeTool::handleMouseDown(
    Scene& scene, const Camera& camera,
    float mouseX, float mouseY,
    bool isCtrl, bool isAlt, bool isShift
) {
    if (m_baseMesh == nullptr) {
        m_baseMesh = scene.getSelected();
    }

    // 0. Check hit on end profile points or contour if CustomGraph is active and shown
    if (showProfileAtEnd && profileType == TubeProfileType::CustomGraph) {
        int hitProfPt = hitTestEndProfilePoint(camera, mouseX, mouseY, 14.0f);
        if (hitProfPt != -1) {
            // If also near the spline center point, pick whichever screen distance is closer
            int hitSplinePt = hitTestControlPoints(camera, mouseX, mouseY, 14.0f);
            if (hitSplinePt != -1) {
                glm::vec3 sSpline = camera.project(m_spline.getPoint(hitSplinePt).position);
                EvaluatedFrame f;
                if (getEndProfileFrame(f)) {
                    glm::vec3 sProf = camera.project(f.position + (f.normal * crossSectionProfile[hitProfPt].pos.x + f.binormal * crossSectionProfile[hitProfPt].pos.y) * f.radius);
                    float dSpline = glm::distance(glm::vec2(mouseX, mouseY), glm::vec2(sSpline.x, sSpline.y));
                    float dProf = glm::distance(glm::vec2(mouseX, mouseY), glm::vec2(sProf.x, sProf.y));
                    if (dSpline < dProf) {
                        hitProfPt = -1; // Spline point is closer, let spline handle it
                    }
                }
            }
        }

        if (hitProfPt != -1) {
            if (isAlt) {
                if (crossSectionProfile.size() > 3) {
                    crossSectionProfile.erase(crossSectionProfile.begin() + hitProfPt);
                    m_selectedProfilePtIdx = -1;
                    m_hoveredProfilePtIdx = -1;
                    m_snapTargetProfilePtIdx = -1;
                    rebuildMeshGeometry(scene);
                    scene.pushHistoryState();
                    GuiManager::showToast("Profile Point Deleted", 1.2f);
                } else {
                    GuiManager::showToast("Profile requires at least 3 points", 1.5f);
                }
                return true;
            }

            m_selectedProfilePtIdx = hitProfPt;
            m_hoveredProfilePtIdx = hitProfPt;
            m_isDraggingProfilePoint = true;
            m_profilePointWasMoved = false;
            m_snapTargetProfilePtIdx = -1;
            m_dragProfileStartMouse = glm::vec2(mouseX, mouseY);
            return true;
        }

        // Check hit on profile contour ring (to insert a point)
        int seg = -1;
        glm::vec2 p2D(0.0f), screenPos(0.0f);
        if (hitTestEndProfileContour(camera, mouseX, mouseY, 14.0f, &seg, &p2D, &screenPos)) {
            ProfilePoint2D newPt;
            newPt.pos = p2D;
            newPt.isCorner = isCtrl; // Ctrl-click creates a corner point
            int insertIdx = seg + 1;
            if (insertIdx >= static_cast<int>(crossSectionProfile.size())) {
                crossSectionProfile.push_back(newPt);
                m_selectedProfilePtIdx = static_cast<int>(crossSectionProfile.size()) - 1;
            } else {
                crossSectionProfile.insert(crossSectionProfile.begin() + insertIdx, newPt);
                m_selectedProfilePtIdx = insertIdx;
            }
            m_hoveredProfilePtIdx = m_selectedProfilePtIdx;
            m_isDraggingProfilePoint = true;
            m_profilePointWasMoved = true;
            m_snapTargetProfilePtIdx = -1;
            m_dragProfileStartMouse = glm::vec2(mouseX, mouseY);
            rebuildMeshGeometry(scene);
            scene.pushHistoryState();
            GuiManager::showToast(isCtrl ? "Added Corner Point" : "Added Profile Point", 1.2f);
            return true;
        }
    }

    m_selectedProfilePtIdx = -1;

    // 1. Check if clicking on control point or radius handle
    int hitPoint = hitTestControlPoints(camera, mouseX, mouseY);
    int hitHandle = hitTestRadiusHandle(camera, mouseX, mouseY);

    // If both are within threshold (e.g. small radius), choose the closer one
    if (hitPoint != -1 && hitHandle != -1) {
        glm::vec3 cWorld, hWorld, side;
        glm::vec2 cScreen, hScreen;
        getPointHandleInfo(camera, hitHandle, cWorld, hWorld, cScreen, hScreen, side);
        glm::vec3 sPt = camera.project(m_spline.getPoint(hitPoint).position);
        float dPt = glm::distance(glm::vec2(mouseX, mouseY), glm::vec2(sPt.x, sPt.y));
        float dH = glm::distance(glm::vec2(mouseX, mouseY), hScreen);
        if (dH < dPt) {
            hitPoint = -1;
        } else {
            hitHandle = -1;
        }
    }

    // Direct radius handle interaction: drag square stick to change point thickness!
    if (hitHandle != -1) {
        m_selectedPointIdx = hitHandle;
        m_isDraggingRadius = true;
        m_dragStartMouse = glm::vec2(mouseX, mouseY);
        m_dragStartRadius = m_spline.getPoint(hitHandle).radius;
        return true;
    }

    // Existing control point clicked
    if (hitPoint != -1) {
        size_t ptCount = m_spline.getPointCount();
        bool isEndPoint = (hitPoint == 0 || hitPoint == static_cast<int>(ptCount) - 1);

        if (isAlt) {
            if (isEndPoint && ptCount >= 1) {
                // Alt + Drag from either end of the curve extrudes a new point!
                // NOTE: Do NOT add a 0-distance duplicate point immediately to prevent
                // degenerate Catmull-Rom intervals, frame twisting, and tube wrinkling.
                // The new point is instantiated in handleMouseMove once drag movement begins.
                m_selectedPointIdx = hitPoint;
                m_extrudeEndIdx = hitPoint;
                m_isDraggingPoint = true;
                m_isExtrudingPoint = true;
                m_pointWasMoved = false;
                m_snapTargetPointIdx = -1;
                m_dragStartMouse = glm::vec2(mouseX, mouseY);
                m_dragStartPointPos = m_spline.getPoint(hitPoint).position;

                glm::vec3 camPos = camera.computePosition();
                m_dragPlanePoint = m_dragStartPointPos;
                m_dragPlaneNormal = glm::normalize(camPos - m_dragPlanePoint);
                return true;
            } else {
                // Alt + Click on an interior point = delete control point
                m_spline.removePoint(hitPoint);
                m_selectedPointIdx = -1;
                m_hoveredPointIdx = -1;
                m_hoveredRadiusHandleIdx = -1;
                m_snapTargetPointIdx = -1;
                rebuildMeshGeometry(scene);
                scene.pushHistoryState();
                GuiManager::showToast("Point deleted", 1.2f);
                return true;
            }
        }

        m_selectedPointIdx = hitPoint;
        m_isDraggingPoint = true;
        m_isExtrudingPoint = false;
        m_pointWasMoved = false;
        m_snapTargetPointIdx = -1;
        m_dragStartMouse = glm::vec2(mouseX, mouseY);
        m_dragStartPointPos = m_spline.getPoint(hitPoint).position;

        // Plane perpendicular to camera view direction passing through point
        glm::vec3 camPos = camera.computePosition();
        m_dragPlanePoint = m_dragStartPointPos;
        m_dragPlaneNormal = glm::normalize(camPos - m_dragPlanePoint);
        return true;
    }

    // 3. If curve already exists, check if clicking on the curve to insert a new control point
    if (m_spline.getPointCount() >= 2) {
        float segT = 0.5f;
        int hitSeg = findClosestSegmentOnCurve(camera, mouseX, mouseY, 18.0f, &segT);
        if (hitSeg != -1) {
            float globalU = (static_cast<float>(hitSeg) + segT) / static_cast<float>(m_spline.getPointCount() - 1);
            glm::vec3 insertPos = m_spline.evaluatePosition(globalU);
            float insertR = glm::mix(m_spline.getPoint(hitSeg).radius, m_spline.getPoint(hitSeg + 1).radius, segT);

            glm::vec3 hitPos, hitNorm;
            bool hasNorm = false;
            if (snapToSurface && raycastSurface(scene, camera, mouseX, mouseY, hitPos, hitNorm)) {
                insertPos = hitPos + hitNorm * (insertR + surfaceOffset);
                hasNorm = true;
            }

            // If both bounding points are sharp corners, create the new point as sharp corner too
            bool bothSharp = m_spline.getPoint(hitSeg).isCorner && m_spline.getPoint(hitSeg + 1).isCorner;

            m_spline.insertPoint(hitSeg + 1, insertPos, insertR, hasNorm ? hitNorm : glm::vec3(0.0f, 1.0f, 0.0f), hasNorm, bothSharp);
            m_selectedPointIdx = hitSeg + 1;
            m_isDraggingPoint = true;
            m_isExtrudingPoint = false;
            m_pointWasMoved = true;
            m_snapTargetPointIdx = -1;
            m_dragStartMouse = glm::vec2(mouseX, mouseY);
            m_dragStartPointPos = insertPos;

            glm::vec3 camPos = camera.computePosition();
            m_dragPlanePoint = m_dragStartPointPos;
            m_dragPlaneNormal = glm::normalize(camPos - m_dragPlanePoint);

            rebuildMeshGeometry(scene);
            scene.pushHistoryState();
            return true;
        }
    }

    // Check if Ctrl+Clicking on another unfinalized tube to switch active tube
    if (isCtrl) {
        glm::vec3 hitPos, hitNorm;
        Mesh* hitMesh = nullptr;
        if (raycastSurface(scene, camera, mouseX, mouseY, hitPos, hitNorm, &hitMesh)) {
            if (hitMesh && hitMesh->isTube && hitMesh->tubeData && hitMesh != m_activeTubeMesh) {
                scene.selectMesh(hitMesh);
                loadFromMesh(hitMesh);
                GuiManager::showToast("Loaded tube for editing", 1.5f);
                return true;
            }
        }
    }

    // 4. Point-by-point mode: append point at click
    if (creationMode == TubeCreationMode::PointByPoint) {
        glm::vec3 hitPos, hitNorm;
        if (snapToSurface && raycastSurface(scene, camera, mouseX, mouseY, hitPos, hitNorm)) {
            glm::vec3 ptPos = hitPos + hitNorm * (baseRadius + surfaceOffset);
            m_spline.addPoint(ptPos, baseRadius, hitNorm, true);
        } else {
            glm::vec3 planePt = m_spline.getPointCount() > 0 ? m_spline.getPoint(m_spline.getPointCount() - 1).position : camera.getPivot();
            glm::vec3 planeNorm = glm::normalize(camera.computePosition() - planePt);
            glm::vec3 ptPos = intersectRayPlane(camera, mouseX, mouseY, planePt, planeNorm);
            m_spline.addPoint(ptPos, baseRadius);
        }
        m_selectedPointIdx = static_cast<int>(m_spline.getPointCount()) - 1;
        rebuildMeshGeometry(scene);
        scene.pushHistoryState();
        return true;
    }

    // 5. Freehand stroke mode: begin stroke
    if (creationMode == TubeCreationMode::FreehandStroke) {
        if (isAlt) {
            return false;
        }

        m_isDrawingStroke = true;
        m_dragStartMouse = glm::vec2(mouseX, mouseY);
        m_rawStroke.clear();
        m_strokeStartHitMesh = nullptr;

        glm::vec3 hitPos, hitNorm;
        Mesh* hitMesh = nullptr;
        bool hasHit = raycastSurface(scene, camera, mouseX, mouseY, hitPos, hitNorm, &hitMesh);
        if (hasHit && hitMesh) {
            m_strokeStartHitMesh = hitMesh;
        }

        if (snapToSurface && hasHit) {
            glm::vec3 ptPos = hitPos + hitNorm * (baseRadius + surfaceOffset);
            m_rawStroke.emplace_back(ptPos, baseRadius, hitNorm, true);
        } else {
            glm::vec3 planePt = camera.getPivot();
            glm::vec3 planeNorm = glm::normalize(camera.computePosition() - planePt);
            glm::vec3 ptPos = intersectRayPlane(camera, mouseX, mouseY, planePt, planeNorm);
            m_rawStroke.emplace_back(ptPos, baseRadius);
        }
        return true;
    }

    return false;
}

bool TubeTool::handleMouseMove(Scene& scene, const Camera& camera, float mouseX, float mouseY) {
    if (m_isDrawingStroke) {
        glm::vec3 hitPos, hitNorm;
        glm::vec3 ptPos;
        bool hasNorm = false;

        if (snapToSurface && raycastSurface(scene, camera, mouseX, mouseY, hitPos, hitNorm)) {
            ptPos = hitPos + hitNorm * (baseRadius + surfaceOffset);
            hasNorm = true;
        } else {
            glm::vec3 planePt = m_rawStroke.empty() ? camera.getPivot() : m_rawStroke.back().position;
            glm::vec3 planeNorm = glm::normalize(camera.computePosition() - planePt);
            ptPos = intersectRayPlane(camera, mouseX, mouseY, planePt, planeNorm);
        }

        float mouseDist = glm::distance(glm::vec2(mouseX, mouseY), m_dragStartMouse);
        float worldDist = m_rawStroke.empty() ? 999.0f : glm::distance(m_rawStroke.back().position, ptPos);

        // Responsive stroke sampling: register drag once mouse moved >= 4px or world dist >= 0.05f
        if (m_rawStroke.size() == 1) {
            if (mouseDist > 4.0f || worldDist > 0.05f) {
                // Dragging across clean space: detach previous active tube (preserving its mesh & data)
                // so this stroke authors a fresh new tube automatically!
                if (m_activeTubeMesh) {
                    syncToActiveMesh();
                    m_activeTubeMesh = nullptr;
                    m_spline.clear();
                    scene.selectMesh(nullptr);
                }
                m_rawStroke.emplace_back(ptPos, baseRadius, hitNorm, hasNorm);
            }
        } else if (m_rawStroke.size() >= 2) {
            // Once dragging has commenced, add points along stroke
            if (worldDist > 0.05f || mouseDist > 6.0f) {
                if (worldDist > 0.02f) {
                    m_rawStroke.emplace_back(ptPos, baseRadius, hitNorm, hasNorm);
                }
            }
        }
        return true;
    }

    if (m_isDraggingProfilePoint && m_selectedProfilePtIdx >= 0 && m_selectedProfilePtIdx < static_cast<int>(crossSectionProfile.size())) {
        EvaluatedFrame frame;
        if (getEndProfileFrame(frame)) {
            Ray ray = camera.getRay(mouseX, mouseY);
            glm::vec3 planeNorm = frame.tangent;
            if (std::abs(glm::dot(frame.tangent, ray.dir)) < 0.12f) {
                planeNorm = -ray.dir;
            }
            glm::vec3 hitWorld = intersectRayPlane(camera, mouseX, mouseY, frame.position, planeNorm);
            glm::vec3 offset = hitWorld - frame.position;
            float r = std::max(frame.radius, 0.001f);
            float x2D = glm::dot(offset, frame.normal) / r;
            float y2D = glm::dot(offset, frame.binormal) / r;

            // Clamp coordinate distance to reasonable range (<= 3.0f)
            float dist2D = std::sqrt(x2D * x2D + y2D * y2D);
            if (dist2D > 3.0f) {
                x2D = (x2D / dist2D) * 3.0f;
                y2D = (y2D / dist2D) * 3.0f;
            }

            crossSectionProfile[m_selectedProfilePtIdx].pos = glm::vec2(x2D, y2D);

            if (glm::distance(glm::vec2(mouseX, mouseY), m_dragProfileStartMouse) > 3.0f) {
                m_profilePointWasMoved = true;
            }

            // Proximity snap with immediate neighbors in closed loop
            m_snapTargetProfilePtIdx = -1;
            int N = static_cast<int>(crossSectionProfile.size());
            if (N > 3) {
                int curIdx = m_selectedProfilePtIdx;
                glm::vec3 curWorld = frame.position + (frame.normal * x2D + frame.binormal * y2D) * r;
                glm::vec3 curScreen = camera.project(curWorld);
                float bestSnapDist = 20.0f;

                int candidateNeighbors[2] = { (curIdx - 1 + N) % N, (curIdx + 1) % N };
                for (int neighborIdx : candidateNeighbors) {
                    const auto& otherPt = crossSectionProfile[neighborIdx];
                    glm::vec3 otherWorld = frame.position + (frame.normal * otherPt.pos.x + frame.binormal * otherPt.pos.y) * r;
                    glm::vec3 otherScreen = camera.project(otherWorld);
                    if (otherScreen.z < 0.0f || otherScreen.z > 1.0f) continue;

                    float dPx = glm::distance(glm::vec2(curScreen.x, curScreen.y), glm::vec2(otherScreen.x, otherScreen.y));
                    float d2D = glm::distance(glm::vec2(x2D, y2D), otherPt.pos);
                    if (dPx < bestSnapDist || d2D < 0.15f) {
                        bestSnapDist = dPx;
                        m_snapTargetProfilePtIdx = neighborIdx;
                    }
                }
            }

            rebuildMeshGeometry(scene);
        }
        return true;
    }

    if (m_isDraggingPoint && m_selectedPointIdx >= 0 && m_selectedPointIdx < static_cast<int>(m_spline.getPointCount())) {
        glm::vec3 newPos;
        glm::vec3 hitPos, hitNorm;
        if (snapToSurface && raycastSurface(scene, camera, mouseX, mouseY, hitPos, hitNorm)) {
            newPos = hitPos + hitNorm * (m_spline.getPoint(m_selectedPointIdx).radius + surfaceOffset);
            m_spline.getPoint(m_selectedPointIdx).normal = hitNorm;
            m_spline.getPoint(m_selectedPointIdx).hasSurfaceNormal = true;
        } else {
            newPos = intersectRayPlane(camera, mouseX, mouseY, m_dragPlanePoint, m_dragPlaneNormal);
        }

        if (m_isExtrudingPoint) {
            // Snapping to merge must NEVER occur while extruding!
            m_snapTargetPointIdx = -1;

            if (!m_pointWasMoved) {
                float mouseMovedDist = glm::distance(glm::vec2(mouseX, mouseY), m_dragStartMouse);
                float worldMovedDist = glm::distance(newPos, m_dragStartPointPos);
                // Instantiate the extruded point only when cursor has actually moved away from origin
                if (mouseMovedDist > 4.0f || worldMovedDist > 0.08f) {
                    if (m_extrudeEndIdx >= 0 && m_extrudeEndIdx < static_cast<int>(m_spline.getPointCount())) {
                        const auto& srcPt = m_spline.getPoint(m_extrudeEndIdx);
                        float spawnRadius = srcPt.radius;
                        glm::vec3 spawnNorm = srcPt.normal;
                        bool spawnHasNorm = srcPt.hasSurfaceNormal;
                        bool spawnIsCorner = srcPt.isCorner;

                        if (m_extrudeEndIdx == 0) {
                            m_spline.insertPoint(0, newPos, spawnRadius, spawnNorm, spawnHasNorm, spawnIsCorner);
                            m_selectedPointIdx = 0;
                        } else {
                            m_spline.addPoint(newPos, spawnRadius, spawnNorm, spawnHasNorm, spawnIsCorner);
                            m_selectedPointIdx = static_cast<int>(m_spline.getPointCount()) - 1;
                        }
                        m_pointWasMoved = true;
                        rebuildMeshGeometry(scene);
                    }
                }
            } else {
                // Point is already instantiated, update its position cleanly
                if (m_selectedPointIdx >= 0 && m_selectedPointIdx < static_cast<int>(m_spline.getPointCount())) {
                    // Prevent degenerate 0-distance overlap with adjacent neighbor
                    int parentIdx = (m_extrudeEndIdx == 0) ? 1 : (static_cast<int>(m_spline.getPointCount()) - 2);
                    if (parentIdx >= 0 && parentIdx < static_cast<int>(m_spline.getPointCount())) {
                        const glm::vec3& parentPos = m_spline.getPoint(parentIdx).position;
                        if (glm::distance(newPos, parentPos) < 0.04f) {
                            glm::vec3 dir = newPos - parentPos;
                            float d = glm::length(dir);
                            glm::vec3 uDir = (d > 1e-4f) ? (dir / d) : glm::normalize(m_dragPlaneNormal);
                            newPos = parentPos + uDir * 0.04f;
                        }
                    }

                    m_spline.getPoint(m_selectedPointIdx).position = newPos;
                    rebuildMeshGeometry(scene);
                }
            }
            return true;
        }

        // Regular point dragging (not extruding)
        m_spline.getPoint(m_selectedPointIdx).position = newPos;
        if (glm::distance(newPos, m_dragStartPointPos) > 0.05f || glm::distance(glm::vec2(mouseX, mouseY), m_dragStartMouse) > 4.0f) {
            m_pointWasMoved = true;
        }

        // Proximity detection for collapsing / merging points:
        // Merge must ONLY be available with immediate adjacent neighbors!
        m_snapTargetPointIdx = -1;
        if (m_spline.getPointCount() > 2) {
            int curIdx = m_selectedPointIdx;
            glm::vec3 curScreen = camera.project(newPos);
            float bestSnapDist = 24.0f; // Screen threshold ~24px

            int candidateNeighbors[2] = { curIdx - 1, curIdx + 1 };
            for (int neighborIdx : candidateNeighbors) {
                if (neighborIdx < 0 || neighborIdx >= static_cast<int>(m_spline.getPointCount())) continue;

                const auto& otherPt = m_spline.getPoint(neighborIdx);
                glm::vec3 otherScreen = camera.project(otherPt.position);
                if (otherScreen.z < 0.0f || otherScreen.z > 1.0f) continue;

                float dPx = glm::distance(glm::vec2(curScreen.x, curScreen.y), glm::vec2(otherScreen.x, otherScreen.y));
                float d3D = glm::distance(newPos, otherPt.position);
                float thresh3D = std::max(otherPt.radius * 1.5f, baseRadius * 0.8f);

                if (dPx < bestSnapDist || d3D < thresh3D) {
                    bestSnapDist = dPx;
                    m_snapTargetPointIdx = neighborIdx;
                }
            }
        }

        rebuildMeshGeometry(scene);
        return true;
    }

    if (m_isDraggingRadius && m_selectedPointIdx >= 0 && m_selectedPointIdx < static_cast<int>(m_spline.getPointCount())) {
        glm::vec3 cWorld, hWorld, side;
        glm::vec2 cScreen, hScreen;
        if (getPointHandleInfo(camera, m_selectedPointIdx, cWorld, hWorld, cScreen, hScreen, side)) {
            float distPx = glm::distance(glm::vec2(mouseX, mouseY), cScreen);
            glm::vec3 sUnit = camera.project(cWorld + side * 1.0f);
            float pxPerUnit = glm::distance(cScreen, glm::vec2(sUnit.x, sUnit.y));
            if (pxPerUnit > 1e-4f) {
                float newRadius = std::max(0.05f, distPx / pxPerUnit);
                m_spline.getPoint(m_selectedPointIdx).radius = newRadius;
                rebuildMeshGeometry(scene);
            }
        }
        return true;
    }

    bool isTubeTool = (m_sculpt.getBrush() == BRUSH_TUBE);
    if (!isTubeTool) {
        m_hoveredProfilePtIdx = -1;
        m_hoveredProfileSeg = -1;
        m_hoveredRadiusHandleIdx = -1;
        m_hoveredRadiusRing = false;
        m_hoveredCurveSeg = -1;
        m_hoveredPointIdx = hitTestControlPoints(camera, mouseX, mouseY);
        return false;
    }

    // Hover detection on end profile points and profile contour ring
    m_hoveredProfilePtIdx = -1;
    m_hoveredProfileSeg = -1;
    if (showProfileAtEnd && profileType == TubeProfileType::CustomGraph) {
        m_hoveredProfilePtIdx = hitTestEndProfilePoint(camera, mouseX, mouseY, 14.0f);
        if (m_hoveredProfilePtIdx == -1) {
            glm::vec2 p2D(0.0f), sPos(0.0f);
            int seg = -1;
            if (hitTestEndProfileContour(camera, mouseX, mouseY, 14.0f, &seg, &p2D, &sPos)) {
                m_hoveredProfileSeg = seg;
                m_hoveredProfile2DPos = p2D;
                m_hoveredProfileScreenPos = sPos;
            }
        }
    }

    if (m_hoveredProfilePtIdx != -1 || m_hoveredProfileSeg != -1) {
        m_hoveredPointIdx = -1;
        m_hoveredRadiusHandleIdx = -1;
        m_hoveredRadiusRing = false;
        m_hoveredCurveSeg = -1;
        return true;
    }

    // Hover detection on control points and radius handles
    m_hoveredPointIdx = hitTestControlPoints(camera, mouseX, mouseY);
    m_hoveredRadiusHandleIdx = hitTestRadiusHandle(camera, mouseX, mouseY);
    m_hoveredRadiusRing = (m_selectedPointIdx != -1) && hitTestRadiusRing(camera, mouseX, mouseY, m_selectedPointIdx);

    // Hover over curve for click-to-insert preview
    m_hoveredCurveSeg = -1;
    if (m_hoveredPointIdx == -1 && m_hoveredRadiusHandleIdx == -1 && !m_hoveredRadiusRing && m_spline.getPointCount() >= 2) {
        float segT = 0.5f;
        int hitSeg = findClosestSegmentOnCurve(camera, mouseX, mouseY, 18.0f, &segT);
        if (hitSeg != -1) {
            m_hoveredCurveSeg = hitSeg;
            m_hoveredCurveT = segT;
            float globalU = (static_cast<float>(hitSeg) + segT) / static_cast<float>(m_spline.getPointCount() - 1);
            glm::vec3 worldPos = m_spline.evaluatePosition(globalU);
            glm::vec3 sPos = camera.project(worldPos);
            if (sPos.z >= 0.0f && sPos.z <= 1.0f) {
                m_hoveredCurveScreenPos = glm::vec2(sPos.x, sPos.y);
            } else {
                m_hoveredCurveSeg = -1;
            }
        }
    }

    return false;
}

bool TubeTool::handleMouseUp(Scene& scene) {
    if (m_isDrawingStroke) {
        m_isDrawingStroke = false;
        if (m_rawStroke.size() >= 2) {
            // Freehand drag occurred: automatically finalize/preserve prior tube and create the new tube!
            if (m_activeTubeMesh) {
                syncToActiveMesh();
                m_activeTubeMesh = nullptr;
                m_spline.clear();
                scene.selectMesh(nullptr);
            }

            // Average / smooth stroke slightly and extract natural control points to describe curve
            std::vector<CurvePoint> simplified = CurveSpline::simplifyStrokeAdaptive(
                m_rawStroke,
                strokeSmoothing,
                curveFitTolerance,
                baseRadius
            );

            if (simplified.size() < 2 && m_rawStroke.size() >= 2) {
                simplified.clear();
                simplified.push_back(m_rawStroke.front());
                simplified.push_back(m_rawStroke.back());
            }

            // Re-assign spline points
            m_spline.clear();
            for (const auto& pt : simplified) {
                m_spline.addPoint(pt.position, pt.radius, pt.normal, pt.hasSurfaceNormal);
            }

            m_selectedPointIdx = static_cast<int>(m_spline.getPointCount()) - 1;
            m_snapTargetPointIdx = -1;
            rebuildMeshGeometry(scene);
            scene.pushHistoryState();
            GuiManager::showToast("New Tube Created", 1.5f);
        } else {
            // User merely clicked without dragging:
            if (m_strokeStartHitMesh && m_strokeStartHitMesh->isTube && m_strokeStartHitMesh->tubeData) {
                // Tapped on another unfinalized tube: switch active selection to that tube!
                if (m_activeTubeMesh && m_activeTubeMesh != m_strokeStartHitMesh) {
                    syncToActiveMesh();
                }
                scene.selectMesh(m_strokeStartHitMesh);
                loadFromMesh(m_strokeStartHitMesh);
                GuiManager::showToast("Loaded tube for editing", 1.2f);
            }
            // Do not deactivate curve on empty click: curve stays active until another is selected or drawn
        }
        m_rawStroke.clear();
        m_strokeStartHitMesh = nullptr;
        return true;
    }

    if (m_isDraggingProfilePoint) {
        bool shouldPushHistory = true;
        int N = static_cast<int>(crossSectionProfile.size());

        if (m_snapTargetProfilePtIdx != -1 && N > 3 && m_selectedProfilePtIdx >= 0 && m_selectedProfilePtIdx < N) {
            int targetIdx = m_snapTargetProfilePtIdx;
            int dragIdx = m_selectedProfilePtIdx;

            int diff = std::abs(targetIdx - dragIdx);
            bool isAdjacent = (diff == 1) || (diff == N - 1);
            if (isAdjacent && targetIdx >= 0 && targetIdx < N) {
                crossSectionProfile[targetIdx].pos = 0.5f * (crossSectionProfile[dragIdx].pos + crossSectionProfile[targetIdx].pos);
                crossSectionProfile[targetIdx].isCorner = crossSectionProfile[dragIdx].isCorner || crossSectionProfile[targetIdx].isCorner;

                crossSectionProfile.erase(crossSectionProfile.begin() + dragIdx);
                m_selectedProfilePtIdx = (targetIdx > dragIdx) ? (targetIdx - 1) : targetIdx;
                m_snapTargetProfilePtIdx = -1;

                rebuildMeshGeometry(scene);
                GuiManager::showToast("Profile points collapsed / merged", 1.8f);
            }
        } else if (!m_profilePointWasMoved && m_selectedProfilePtIdx >= 0 && m_selectedProfilePtIdx < N) {
            auto& pt = crossSectionProfile[m_selectedProfilePtIdx];
            pt.isCorner = !pt.isCorner;
            rebuildMeshGeometry(scene);
            if (pt.isCorner) {
                GuiManager::showToast("Profile Corner Point (Sharp)", 1.5f);
            } else {
                GuiManager::showToast("Profile Smooth Point", 1.5f);
            }
        }

        m_isDraggingProfilePoint = false;
        m_profilePointWasMoved = false;
        m_snapTargetProfilePtIdx = -1;

        if (shouldPushHistory) {
            scene.pushHistoryState();
        }
        return true;
    }

    if (m_isDraggingPoint || m_isDraggingRadius) {
        bool shouldPushHistory = true;

        if (m_isDraggingPoint && !m_isExtrudingPoint && m_snapTargetPointIdx != -1 && m_spline.getPointCount() > 2 && m_selectedPointIdx >= 0) {
            int targetIdx = m_snapTargetPointIdx;
            int dragIdx = m_selectedPointIdx;

            // Strict check: only merge with immediate adjacent neighbor
            if (std::abs(targetIdx - dragIdx) == 1 && targetIdx >= 0 && targetIdx < static_cast<int>(m_spline.getPointCount())) {
                // Merge radii
                float mergedRadius = (m_spline.getPoint(dragIdx).radius + m_spline.getPoint(targetIdx).radius) * 0.5f;
                m_spline.getPoint(targetIdx).radius = mergedRadius;

                // Remove the dragged point
                m_spline.removePoint(dragIdx);
                m_selectedPointIdx = (targetIdx > dragIdx) ? (targetIdx - 1) : targetIdx;
                m_snapTargetPointIdx = -1;

                rebuildMeshGeometry(scene);
                GuiManager::showToast("Points collapsed / merged", 1.8f);
            }
        } else if (m_isDraggingPoint && !m_pointWasMoved && !m_isExtrudingPoint && m_selectedPointIdx >= 0 && m_selectedPointIdx < static_cast<int>(m_spline.getPointCount())) {
            // Single click on control point (without Alt and without drag) -> toggle right-angle corner vs smooth curve
            auto& pt = m_spline.getPoint(m_selectedPointIdx);
            pt.isCorner = !pt.isCorner;
            rebuildMeshGeometry(scene);
            if (pt.isCorner) {
                GuiManager::showToast("Corner Point (Sharp Angle)", 1.5f);
            } else {
                GuiManager::showToast("Smooth Point", 1.5f);
            }
        } else if (m_isExtrudingPoint) {
            if (m_pointWasMoved) {
                GuiManager::showToast("Extruded Point Added", 1.5f);
            } else {
                shouldPushHistory = false; // Alt-clicked without dragging; nothing was added or changed
            }
        }

        m_isDraggingPoint = false;
        m_isDraggingRadius = false;
        m_isExtrudingPoint = false;
        m_extrudeEndIdx = -1;
        m_snapTargetPointIdx = -1;
        m_pointWasMoved = false;

        if (shouldPushHistory) {
            scene.pushHistoryState();
        }
        return true;
    }

    return false;
}

void TubeTool::updateGeometryIfNeeded(Scene& scene) {
    if (m_geometryDirty) {
        rebuildMeshGeometry(scene);
        m_geometryDirty = false;
    }
}

void TubeTool::setBaseRadius(float newRadius) {
    newRadius = std::max(0.05f, newRadius);
    if (std::abs(newRadius - baseRadius) < 1e-4f) return;

    float ratio = (baseRadius > 1e-4f) ? (newRadius / baseRadius) : 1.0f;
    baseRadius = newRadius;

    // Scale radii of all existing points on the active spline proportionally
    for (size_t i = 0; i < m_spline.getPointCount(); ++i) {
        m_spline.getPoint(i).radius = std::max(0.01f, m_spline.getPoint(i).radius * ratio);
    }
    m_geometryDirty = true;
}

void TubeTool::resetRadiusProfile() {
    radiusProfile = { {0.0f, 1.0f, false}, {1.0f, 1.0f, false} };
    radiusProfileSmooth = true;
}

void TubeTool::setRadiusProfilePreset(int presetIdx) {
    switch (presetIdx) {
        case 1: // Uniform (Flat)
            radiusProfile = { {0.0f, 1.0f, false}, {1.0f, 1.0f, false} };
            break;
        case 2: // Linear Taper Down (Spike/Claw)
            radiusProfile = { {0.0f, 1.0f, false}, {0.7f, 0.5f, false}, {1.0f, 0.05f, false} };
            break;
        case 3: // Linear Taper Up (Funnel)
            radiusProfile = { {0.0f, 0.05f, false}, {0.3f, 0.5f, false}, {1.0f, 1.0f, false} };
            break;
        case 4: // Bulb / Tentacle Bead
            radiusProfile = { {0.0f, 0.35f, false}, {0.5f, 1.6f, false}, {1.0f, 0.35f, false} };
            break;
        case 5: // Double Swell (Handle/Bone)
            radiusProfile = { {0.0f, 0.8f, false}, {0.25f, 1.4f, false}, {0.5f, 0.65f, false}, {0.75f, 1.4f, false}, {1.0f, 0.8f, false} };
            break;
        case 6: // Sharp Spike
            radiusProfile = { {0.0f, 1.0f, false}, {0.85f, 0.95f, true}, {1.0f, 0.0f, true} };
            break;
        case 7: // Horn Ridges / Waves
            radiusProfile = {
                {0.0f, 0.7f, false}, {0.15f, 1.35f, false}, {0.3f, 0.75f, false}, {0.45f, 1.3f, false},
                {0.6f, 0.7f, false}, {0.75f, 1.25f, false}, {0.9f, 0.65f, false}, {1.0f, 0.2f, false}
            };
            break;
        case 8: // Bell / Flare
            radiusProfile = { {0.0f, 0.3f, false}, {0.6f, 0.45f, false}, {0.85f, 0.9f, false}, {1.0f, 1.6f, false} };
            break;
        default:
            break;
    }
}

void TubeTool::resetCrossSectionProfile() {
    crossSectionProfile.clear();
    const int n = 8;
    crossSectionProfile.reserve(n);
    for (int i = 0; i < n; ++i) {
        float angle = 2.0f * (float)M_PI * static_cast<float>(i) / static_cast<float>(n);
        crossSectionProfile.emplace_back(std::cos(angle), std::sin(angle), false);
    }
    crossSectionSmooth = true;
}

void TubeTool::setCrossSectionPreset(int presetIdx) {
    crossSectionProfile.clear();
    switch (presetIdx) {
        case 1: // Circle
            resetCrossSectionProfile();
            break;
        case 2: { // Star 5-Point
            const int n = 10;
            crossSectionProfile.reserve(n);
            for (int i = 0; i < n; ++i) {
                float angle = 2.0f * (float)M_PI * static_cast<float>(i) / static_cast<float>(n) - (float)M_PI * 0.5f;
                float r = (i % 2 == 0) ? 1.0f : 0.42f;
                crossSectionProfile.emplace_back(std::cos(angle) * r, std::sin(angle) * r, true);
            }
            crossSectionSmooth = true;
            break;
        }
        case 3: { // Star 6-Point
            const int n = 12;
            crossSectionProfile.reserve(n);
            for (int i = 0; i < n; ++i) {
                float angle = 2.0f * (float)M_PI * static_cast<float>(i) / static_cast<float>(n);
                float r = (i % 2 == 0) ? 1.0f : 0.45f;
                crossSectionProfile.emplace_back(std::cos(angle) * r, std::sin(angle) * r, true);
            }
            crossSectionSmooth = true;
            break;
        }
        case 4: { // Oval / Ellipse
            const int n = 8;
            crossSectionProfile.reserve(n);
            for (int i = 0; i < n; ++i) {
                float angle = 2.0f * (float)M_PI * static_cast<float>(i) / static_cast<float>(n);
                crossSectionProfile.emplace_back(std::cos(angle) * 1.35f, std::sin(angle) * 0.65f, false);
            }
            crossSectionSmooth = true;
            break;
        }
        case 5: { // Chamfered Box / Rounded Square
            crossSectionProfile = {
                { -0.85f, -0.4f, true }, { -0.85f,  0.4f, true },
                { -0.4f,  0.85f, true }, {  0.4f,  0.85f, true },
                {  0.85f,  0.4f, true }, {  0.85f, -0.4f, true },
                {  0.4f, -0.85f, true }, { -0.4f, -0.85f, true }
            };
            crossSectionSmooth = true;
            break;
        }
        case 6: { // Heart / Teardrop
            crossSectionProfile = {
                {  0.0f,  0.35f, true },
                {  0.5f,  0.9f,  false },
                {  0.9f,  0.4f,  false },
                {  0.55f, -0.4f, false },
                {  0.0f, -0.95f, true },
                { -0.55f, -0.4f, false },
                { -0.9f,  0.4f,  false },
                { -0.5f,  0.9f,  false }
            };
            crossSectionSmooth = true;
            break;
        }
        case 7: { // Crescent / C-Gutter (indented on left)
            crossSectionProfile = {
                {  0.85f,  0.0f, false },
                {  0.6f,   0.7f, false },
                {  0.0f,   0.9f, true },
                { -0.6f,   0.7f, false },
                { -0.35f,  0.0f, false },
                { -0.6f,  -0.7f, false },
                {  0.0f,  -0.9f, true },
                {  0.6f,  -0.7f, false }
            };
            crossSectionSmooth = true;
            break;
        }
        case 8: { // Cross / Plus
            crossSectionProfile = {
                { -0.3f, -0.9f, true }, {  0.3f, -0.9f, true },
                {  0.3f, -0.3f, true }, {  0.9f, -0.3f, true },
                {  0.9f,  0.3f, true }, {  0.3f,  0.3f, true },
                {  0.3f,  0.9f, true }, { -0.3f,  0.9f, true },
                { -0.3f,  0.3f, true }, { -0.9f,  0.3f, true },
                { -0.9f, -0.3f, true }, { -0.3f, -0.3f, true }
            };
            crossSectionSmooth = true;
            break;
        }
        default:
            resetCrossSectionProfile();
            break;
    }
}

static void generateCrossSectionRing(
    TubeProfileType profileType,
    int radialSegments,
    float ribbonThickness,
    const std::vector<ProfilePoint2D>& customProfile,
    bool customSmooth,
    std::vector<glm::vec2>& outPts,
    std::vector<glm::vec2>& outNorms,
    bool& outIsCCW
) {
    outPts.clear();
    outNorms.clear();
    outIsCCW = true;

    if (profileType == TubeProfileType::CustomGraph) {
        if (customProfile.size() < 3) {
            int sides = std::max(8, radialSegments);
            outPts.reserve(sides);
            outNorms.reserve(sides);
            for (int j = 0; j < sides; ++j) {
                float phi = 2.0f * (float)M_PI * static_cast<float>(j) / static_cast<float>(sides);
                float c = std::cos(phi);
                float s = std::sin(phi);
                outPts.emplace_back(c, s);
                outNorms.emplace_back(c, s);
            }
            return;
        }

        int N = static_cast<int>(customProfile.size());

        // Signed area to determine CCW orientation
        float signedArea = 0.0f;
        for (int i = 0; i < N; ++i) {
            int next = (i + 1) % N;
            signedArea += (customProfile[i].pos.x * customProfile[next].pos.y - customProfile[next].pos.x * customProfile[i].pos.y);
        }
        outIsCCW = (signedArea >= 0.0f);

        if (customSmooth) {
            int sides = std::max(8, radialSegments);
            outPts.reserve(sides);
            outNorms.reserve(sides);

            for (int k = 0; k < sides; ++k) {
                float u = static_cast<float>(k) / static_cast<float>(sides);
                float globalU = u * static_cast<float>(N);
                int seg = static_cast<int>(std::floor(globalU));
                float t = globalU - static_cast<float>(seg);
                seg = (seg % N + N) % N;

                int i1 = seg;
                int i2 = (seg + 1) % N;

                const auto& pt1 = customProfile[i1];
                const auto& pt2 = customProfile[i2];

                glm::vec2 pos(0.0f);
                glm::vec2 tan(0.0f);

                if (pt1.isCorner && pt2.isCorner) {
                    // Straight linear segment between two sharp corners
                    pos = glm::mix(pt1.pos, pt2.pos, t);
                    glm::vec2 chord = pt2.pos - pt1.pos;
                    float cLen = glm::length(chord);
                    tan = (cLen > 1e-6f) ? (chord / cLen) : glm::vec2(0.0f, 1.0f);
                } else {
                    // Catmull-Rom with corner phantom reflection
                    glm::vec2 p1 = pt1.pos;
                    glm::vec2 p2 = pt2.pos;
                    glm::vec2 p0 = pt1.isCorner ? (p1 - (p2 - p1)) : customProfile[(i1 - 1 + N) % N].pos;
                    glm::vec2 p3 = pt2.isCorner ? (p2 + (p2 - p1)) : customProfile[(i2 + 1) % N].pos;

                    float t2 = t * t;
                    float t3 = t2 * t;

                    pos = 0.5f * (
                        2.0f * p1 +
                        (-p0 + p2) * t +
                        (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                        (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3
                    );

                    tan = 0.5f * (
                        (-p0 + p2) +
                        2.0f * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t +
                        3.0f * (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t2
                    );
                }

                float tLen = glm::length(tan);
                if (tLen < 1e-5f) {
                    glm::vec2 chord = pt2.pos - pt1.pos;
                    tLen = glm::length(chord);
                    if (tLen > 1e-5f) tan = chord / tLen;
                    else tan = glm::vec2(0.0f, 1.0f);
                } else {
                    tan /= tLen;
                }

                glm::vec2 norm = outIsCCW ? glm::vec2(tan.y, -tan.x) : glm::vec2(-tan.y, tan.x);
                float nLen = glm::length(norm);
                if (nLen > 1e-5f) norm /= nLen;
                else norm = (glm::length(pos) > 1e-4f) ? glm::normalize(pos) : glm::vec2(1.0f, 0.0f);

                outPts.push_back(pos);
                outNorms.push_back(norm);
            }
        } else {
            // Sharp polygon without spline subdivision
            outPts.reserve(N);
            outNorms.resize(N);

            for (int i = 0; i < N; ++i) {
                outPts.push_back(customProfile[i].pos);
            }

            for (int i = 0; i < N; ++i) {
                int prev = (i - 1 + N) % N;
                int next = (i + 1) % N;
                glm::vec2 tan = customProfile[next].pos - customProfile[prev].pos;
                float tLen = glm::length(tan);
                if (tLen > 1e-5f) tan /= tLen;
                else tan = glm::vec2(0.0f, 1.0f);

                glm::vec2 norm = outIsCCW ? glm::vec2(tan.y, -tan.x) : glm::vec2(-tan.y, tan.x);
                float nLen = glm::length(norm);
                if (nLen > 1e-5f) norm /= nLen;
                else norm = (glm::length(customProfile[i].pos) > 1e-4f) ? glm::normalize(customProfile[i].pos) : glm::vec2(1.0f, 0.0f);
                outNorms[i] = norm;
            }
        }
    } else if (profileType == TubeProfileType::FlatRibbon) {
        outPts = {
            { -1.5f,  ribbonThickness },
            {  1.5f,  ribbonThickness },
            {  1.5f, -ribbonThickness },
            { -1.5f, -ribbonThickness }
        };
        outNorms = {
            glm::normalize(glm::vec2(-1.0f,  1.0f)),
            glm::normalize(glm::vec2( 1.0f,  1.0f)),
            glm::normalize(glm::vec2( 1.0f, -1.0f)),
            glm::normalize(glm::vec2(-1.0f, -1.0f))
        };
        outIsCCW = false;
    } else if (profileType == TubeProfileType::Square) {
        outPts.reserve(4);
        outNorms.reserve(4);
        for (int j = 0; j < 4; ++j) {
            float phi45 = 2.0f * (float)M_PI * static_cast<float>(j) / 4.0f + (float)M_PI * 0.25f;
            float c = std::cos(phi45);
            float s = std::sin(phi45);
            outPts.emplace_back(c * 1.4142f, s * 1.4142f);
            outNorms.emplace_back(c, s);
        }
        outIsCCW = true;
    } else {
        // Round / Polygon
        int sides = std::max(3, radialSegments);
        outPts.reserve(sides);
        outNorms.reserve(sides);
        for (int j = 0; j < sides; ++j) {
            float phi = 2.0f * (float)M_PI * static_cast<float>(j) / static_cast<float>(sides);
            float c = std::cos(phi);
            float s = std::sin(phi);
            outPts.emplace_back(c, s);
            outNorms.emplace_back(c, s);
        }
        outIsCCW = true;
    }
}

static void buildTubeGeometry(
    const std::vector<EvaluatedFrame>& frames,
    TubeProfileType profileType,
    TubeCapType capType,
    int radialSegments,
    float ribbonThickness,
    const std::vector<ProfilePoint2D>& customProfile,
    bool customSmooth,
    std::vector<float>& outVerts,
    std::vector<float>& outNormals,
    std::vector<float>& outColors,
    std::vector<uint32_t>& outFaces,
    bool flipWinding = false
) {
    if (frames.size() < 2) return;

    std::vector<glm::vec2> ringPts;
    std::vector<glm::vec2> ringNorms;
    bool isCCW = true;
    generateCrossSectionRing(
        profileType, radialSegments, ribbonThickness, customProfile, customSmooth,
        ringPts, ringNorms, isCCW
    );

    int radSides = static_cast<int>(ringPts.size());
    if (radSides < 3) return;

    bool effectiveFlip = (isCCW ? flipWinding : !flipWinding);

    int numFrames = static_cast<int>(frames.size());
    uint32_t vertOffset = static_cast<uint32_t>(outVerts.size() / 3);

    // 1. Generate Ring Vertices
    for (int i = 0; i < numFrames; ++i) {
        const auto& f = frames[i];
        float r = std::max(f.radius, 0.001f);

        // Slope dr/ds along the tube centerline for accurate, smooth surface normals
        float dr_ds = 0.0f;
        if (numFrames > 1) {
            if (i == 0) {
                float ds = frames[1].arcLength - frames[0].arcLength;
                if (ds > 1e-4f) dr_ds = (frames[1].radius - frames[0].radius) / ds;
            } else if (i == numFrames - 1) {
                float ds = frames[numFrames - 1].arcLength - frames[numFrames - 2].arcLength;
                if (ds > 1e-4f) dr_ds = (frames[numFrames - 1].radius - frames[numFrames - 2].radius) / ds;
            } else {
                float ds = frames[i + 1].arcLength - frames[i - 1].arcLength;
                if (ds > 1e-4f) dr_ds = (frames[i + 1].radius - frames[i - 1].radius) / ds;
            }
        }
        dr_ds = std::clamp(dr_ds, -3.0f, 3.0f);

        for (int j = 0; j < radSides; ++j) {
            const glm::vec2& p2D = ringPts[j];
            const glm::vec2& n2D = ringNorms[j];

            glm::vec3 pos = f.position + (f.normal * p2D.x + f.binormal * p2D.y) * r;
            glm::vec3 dir = glm::normalize(f.normal * n2D.x + f.binormal * n2D.y);
            glm::vec3 norm = glm::normalize(dir - f.tangent * dr_ds);

            outVerts.push_back(pos.x);
            outVerts.push_back(pos.y);
            outVerts.push_back(pos.z);

            outNormals.push_back(norm.x);
            outNormals.push_back(norm.y);
            outNormals.push_back(norm.z);

            outColors.push_back(1.0f);
            outColors.push_back(1.0f);
            outColors.push_back(1.0f);
        }
    }

    // 2. Tube Body Quad Faces (CCW outward winding)
    for (int i = 0; i < numFrames - 1; ++i) {
        for (int j = 0; j < radSides; ++j) {
            int nextJ = (j + 1) % radSides;
            uint32_t v0 = vertOffset + i * radSides + j;
            uint32_t v1 = vertOffset + i * radSides + nextJ;
            uint32_t v2 = vertOffset + (i + 1) * radSides + nextJ;
            uint32_t v3 = vertOffset + (i + 1) * radSides + j;

            if (effectiveFlip) {
                outFaces.push_back(v0);
                outFaces.push_back(v3);
                outFaces.push_back(v2);
                outFaces.push_back(v1);
            } else {
                outFaces.push_back(v0);
                outFaces.push_back(v1);
                outFaces.push_back(v2);
                outFaces.push_back(v3);
            }
        }
    }

    // 3. Caps (CCW outward winding)
    if (capType == TubeCapType::Flat) {
        // Start cap (outward normal = -tangent)
        uint32_t centerStartIdx = static_cast<uint32_t>(outVerts.size() / 3);
        const auto& f0 = frames.front();
        glm::vec3 nStart = -f0.tangent;
        outVerts.push_back(f0.position.x);
        outVerts.push_back(f0.position.y);
        outVerts.push_back(f0.position.z);
        outNormals.push_back(nStart.x);
        outNormals.push_back(nStart.y);
        outNormals.push_back(nStart.z);
        outColors.push_back(1.0f); outColors.push_back(1.0f); outColors.push_back(1.0f);

        for (int j = 0; j < radSides; ++j) {
            int nextJ = (j + 1) % radSides;
            outFaces.push_back(centerStartIdx);
            if (effectiveFlip) {
                outFaces.push_back(vertOffset + nextJ);
                outFaces.push_back(vertOffset + j);
            } else {
                outFaces.push_back(vertOffset + j);
                outFaces.push_back(vertOffset + nextJ);
            }
            outFaces.push_back(0xffffffff);
        }

        // End cap (outward normal = +tangent)
        uint32_t centerEndIdx = static_cast<uint32_t>(outVerts.size() / 3);
        const auto& fEnd = frames.back();
        glm::vec3 nEnd = fEnd.tangent;
        outVerts.push_back(fEnd.position.x);
        outVerts.push_back(fEnd.position.y);
        outVerts.push_back(fEnd.position.z);
        outNormals.push_back(nEnd.x);
        outNormals.push_back(nEnd.y);
        outNormals.push_back(nEnd.z);
        outColors.push_back(1.0f); outColors.push_back(1.0f); outColors.push_back(1.0f);

        uint32_t lastRingOffset = vertOffset + (numFrames - 1) * radSides;
        for (int j = 0; j < radSides; ++j) {
            int nextJ = (j + 1) % radSides;
            outFaces.push_back(centerEndIdx);
            if (effectiveFlip) {
                outFaces.push_back(lastRingOffset + j);
                outFaces.push_back(lastRingOffset + nextJ);
            } else {
                outFaces.push_back(lastRingOffset + nextJ);
                outFaces.push_back(lastRingOffset + j);
            }
            outFaces.push_back(0xffffffff);
        }
    } else if (capType == TubeCapType::Rounded) {
        // Rounded dome cap for start and end conforming to cross-section profile
        auto addDomeCap = [&](const EvaluatedFrame& f, bool isStart, uint32_t ringOffset) {
            int domeRings = std::max(2, radSides / 4);
            glm::vec3 axis = isStart ? -f.tangent : f.tangent;
            float r = std::max(f.radius, 0.001f);

            uint32_t prevRing = ringOffset;
            for (int d = 1; d <= domeRings; ++d) {
                float theta = ((float)M_PI * 0.5f) * (static_cast<float>(d) / domeRings);
                float sinTheta = std::sin(theta);
                float cosTheta = std::cos(theta);

                if (d == domeRings) {
                    // Tip pole vertex
                    uint32_t poleIdx = static_cast<uint32_t>(outVerts.size() / 3);
                    glm::vec3 polePos = f.position + axis * r;
                    outVerts.push_back(polePos.x); outVerts.push_back(polePos.y); outVerts.push_back(polePos.z);
                    outNormals.push_back(axis.x); outNormals.push_back(axis.y); outNormals.push_back(axis.z);
                    outColors.push_back(1.0f); outColors.push_back(1.0f); outColors.push_back(1.0f);

                    for (int j = 0; j < radSides; ++j) {
                        int nextJ = (j + 1) % radSides;
                        outFaces.push_back(poleIdx);
                        if (isStart) {
                            if (effectiveFlip) {
                                outFaces.push_back(prevRing + nextJ);
                                outFaces.push_back(prevRing + j);
                            } else {
                                outFaces.push_back(prevRing + j);
                                outFaces.push_back(prevRing + nextJ);
                            }
                        } else {
                            if (effectiveFlip) {
                                outFaces.push_back(prevRing + j);
                                outFaces.push_back(prevRing + nextJ);
                            } else {
                                outFaces.push_back(prevRing + nextJ);
                                outFaces.push_back(prevRing + j);
                            }
                        }
                        outFaces.push_back(0xffffffff);
                    }
                } else {
                    uint32_t curRing = static_cast<uint32_t>(outVerts.size() / 3);
                    float curR = r * cosTheta;
                    float curH = r * sinTheta;

                    for (int j = 0; j < radSides; ++j) {
                        const glm::vec2& p2D = ringPts[j];
                        const glm::vec2& n2D = ringNorms[j];
                        glm::vec3 dir = f.normal * p2D.x + f.binormal * p2D.y;
                        glm::vec3 normDir = glm::normalize(f.normal * n2D.x + f.binormal * n2D.y);
                        glm::vec3 pos = f.position + axis * curH + dir * curR;
                        glm::vec3 norm = glm::normalize(axis * sinTheta + normDir * cosTheta);

                        outVerts.push_back(pos.x); outVerts.push_back(pos.y); outVerts.push_back(pos.z);
                        outNormals.push_back(norm.x); outNormals.push_back(norm.y); outNormals.push_back(norm.z);
                        outColors.push_back(1.0f); outColors.push_back(1.0f); outColors.push_back(1.0f);
                    }

                    for (int j = 0; j < radSides; ++j) {
                        int nextJ = (j + 1) % radSides;
                        if (isStart) {
                            if (effectiveFlip) {
                                outFaces.push_back(prevRing + nextJ);
                                outFaces.push_back(prevRing + j);
                                outFaces.push_back(curRing + j);
                                outFaces.push_back(curRing + nextJ);
                            } else {
                                outFaces.push_back(prevRing + j);
                                outFaces.push_back(curRing + j);
                                outFaces.push_back(curRing + nextJ);
                                outFaces.push_back(prevRing + nextJ);
                            }
                        } else {
                            if (effectiveFlip) {
                                outFaces.push_back(prevRing + j);
                                outFaces.push_back(curRing + j);
                                outFaces.push_back(curRing + nextJ);
                                outFaces.push_back(prevRing + nextJ);
                            } else {
                                outFaces.push_back(prevRing + j);
                                outFaces.push_back(prevRing + nextJ);
                                outFaces.push_back(curRing + nextJ);
                                outFaces.push_back(curRing + j);
                            }
                        }
                    }
                    prevRing = curRing;
                }
            }
        };

        addDomeCap(frames.front(), true, vertOffset);
        addDomeCap(frames.back(), false, vertOffset + (numFrames - 1) * radSides);
    }
}

void TubeTool::rebuildTubeMesh(Scene& scene, Mesh* tubeMesh) {
    if (!tubeMesh) {
        tubeMesh = m_activeTubeMesh;
        if (!tubeMesh) return;
    }
    if (!tubeMesh->isTube || !tubeMesh->tubeData) return;

    bool isPrimary = (tubeMesh == m_activeTubeMesh);

    if (isPrimary) {
        if (m_spline.getPointCount() < 2) {
            scene.removeMesh(tubeMesh);
            m_activeTubeMesh = nullptr;
            return;
        }
    } else {
        if (tubeMesh->tubeData->spline.getPointCount() < 2) {
            return;
        }
    }

    TubeProfileType profType = isPrimary ? profileType : tubeMesh->tubeData->profileType;
    TubeCapType cType = isPrimary ? capType : tubeMesh->tubeData->capType;
    int radSides = isPrimary ? radialSegments : tubeMesh->tubeData->radialSegments;
    float ribbonThick = isPrimary ? ribbonThickness : tubeMesh->tubeData->ribbonThickness;
    const std::vector<ProfilePoint2D>& csProfile = isPrimary ? crossSectionProfile : tubeMesh->tubeData->crossSectionProfile;
    bool csSmooth = isPrimary ? crossSectionSmooth : tubeMesh->tubeData->crossSectionSmooth;
    float sRadScale = isPrimary ? startRadiusScale : tubeMesh->tubeData->startRadiusScale;
    float eRadScale = isPrimary ? endRadiusScale : tubeMesh->tubeData->endRadiusScale;
    float twist = isPrimary ? twistDegrees : tubeMesh->tubeData->twistDegrees;
    int cSegments = isPrimary ? cornerSegments : tubeMesh->tubeData->cornerSegments;
    bool useRProfile = isPrimary ? useRadiusProfile : tubeMesh->tubeData->useRadiusProfile;
    const std::vector<ProfilePoint2D>& rProfile = isPrimary ? radiusProfile : tubeMesh->tubeData->radiusProfile;
    bool rProfileSmooth = isPrimary ? radiusProfileSmooth : tubeMesh->tubeData->radiusProfileSmooth;
    int numSamples = isPrimary ? curveSegments : tubeMesh->tubeData->curveSegments;
    bool isAutoDensity = isPrimary ? autoDensity : tubeMesh->tubeData->autoDensity;
    float bRadius = isPrimary ? baseRadius : tubeMesh->tubeData->baseRadius;

    const CurveSpline* splineToUse = nullptr;
    CurveSpline tempSpline;

    if (isPrimary) {
        splineToUse = &m_spline;
        if (m_isDraggingPoint && !m_isExtrudingPoint && m_snapTargetPointIdx != -1 && m_spline.getPointCount() > 2 && m_selectedPointIdx >= 0) {
            if (std::abs(m_snapTargetPointIdx - m_selectedPointIdx) == 1 &&
                m_snapTargetPointIdx >= 0 && m_snapTargetPointIdx < static_cast<int>(m_spline.getPointCount())) {
                tempSpline = m_spline;
                int dragIdx = m_selectedPointIdx;
                int targetIdx = m_snapTargetPointIdx;
                float mergedR = (tempSpline.getPoint(dragIdx).radius + tempSpline.getPoint(targetIdx).radius) * 0.5f;
                tempSpline.getPoint(targetIdx).radius = mergedR;
                tempSpline.removePoint(dragIdx);
                splineToUse = &tempSpline;
            }
        }
    } else {
        splineToUse = &tubeMesh->tubeData->spline;
    }

    if (!splineToUse || splineToUse->getPointCount() < 2) return;

    if (isAutoDensity) {
        float totalLen = splineToUse->getTotalLength();
        if (totalLen > 1e-4f) {
            float totalR = 0.0f;
            size_t ptCount = splineToUse->getPointCount();
            for (size_t i = 0; i < ptCount; ++i) {
                totalR += splineToUse->getPoint(i).radius;
            }
            float avgR = (ptCount > 0) ? (totalR / static_cast<float>(ptCount)) : bRadius;
            float avgRadius = std::max(0.05f, avgR * (sRadScale + eRadScale) * 0.5f);
            float targetStep = std::max(0.15f, avgRadius * 0.35f);
            int needed = static_cast<int>(std::ceil(totalLen / targetStep));
            numSamples = std::clamp(std::max(numSamples, needed), 16, 1024);
        }
    }

    std::vector<EvaluatedFrame> frames = splineToUse->evaluateFrames(
        numSamples, sRadScale, eRadScale, 0.0f, twist, cSegments,
        useRProfile, rProfile, rProfileSmooth
    );

    if (frames.size() < 2) return;

    std::vector<float> verts;
    std::vector<float> normals;
    std::vector<float> colors;
    std::vector<uint32_t> faces;

    // 1. Build primary tube
    buildTubeGeometry(
        frames, profType, cType, radSides, ribbonThick,
        csProfile, csSmooth,
        verts, normals, colors, faces, false
    );

    // 2. If symmetry is enabled, build symmetric tube
    if (m_sculpt.getUseSym() && (m_sculpt.getSymX() || m_sculpt.getSymY() || m_sculpt.getSymZ())) {
        glm::vec3 sScale(m_sculpt.getSymX() ? -1.0f : 1.0f,
                         m_sculpt.getSymY() ? -1.0f : 1.0f,
                         m_sculpt.getSymZ() ? -1.0f : 1.0f);
        bool flipSym = (sScale.x * sScale.y * sScale.z < 0.0f);

        std::vector<EvaluatedFrame> symFrames = frames;
        for (auto& f : symFrames) {
            f.position = f.position * sScale;
            f.tangent = f.tangent * sScale;
            f.normal = f.normal * sScale;
            f.binormal = glm::normalize(glm::cross(f.tangent, f.normal));
        }

        buildTubeGeometry(
            symFrames, profType, cType, radSides, ribbonThick,
            csProfile, csSmooth,
            verts, normals, colors, faces, flipSym
        );
    }

    int nbVerts = static_cast<int>(verts.size() / 3);
    int nbFaces = static_cast<int>(faces.size() / 4);

    if (nbVerts == 0 || nbFaces == 0) return;

    // 3. Compute topology
    std::vector<uint32_t> vrvStartCount, vertRingVert, vrfStartCount, vertRingFace;
    std::vector<uint8_t> vertOnEdge;
    computeTopology(
        nbVerts, faces.data(), nbFaces,
        vrfStartCount, vertRingFace, vrvStartCount, vertRingVert, vertOnEdge
    );

    tubeMesh->verts = std::move(verts);
    tubeMesh->normals = std::move(normals);
    tubeMesh->colors = std::move(colors);
    tubeMesh->faces = std::move(faces);
    tubeMesh->nbVerts = nbVerts;
    tubeMesh->nbFaces = nbFaces;
    tubeMesh->vrfStartCount = std::move(vrfStartCount);
    tubeMesh->vertRingFace = std::move(vertRingFace);
    tubeMesh->vrvStartCount = std::move(vrvStartCount);
    tubeMesh->vertRingVert = std::move(vertRingVert);
    tubeMesh->vertOnEdge = std::move(vertOnEdge);

    // Initialize visibility and material attributes so wireframe shading is fully visible
    tubeMesh->vertVisible.assign(nbVerts, 1);
    tubeMesh->faceVisible.assign(nbFaces, 1);
    tubeMesh->materials.assign(nbVerts * 3, 0.0f);
    for (int i = 0; i < nbVerts; ++i) {
        tubeMesh->materials[i * 3 + 0] = 0.5f; // roughness
        tubeMesh->materials[i * 3 + 1] = 0.0f; // metalness
        tubeMesh->materials[i * 3 + 2] = 0.0f; // mask = 0 (unmasked)
    }

    tubeMesh->initFaceGroups(scene.getNextFreePolyGroupID());
    tubeMesh->hasCustomNormals = false;
    tubeMesh->postInit();
    tubeMesh->isDirty = true;
    tubeMesh->isTube = true;

    if (isPrimary) {
        syncToActiveMesh();
    }
}

void TubeTool::rebuildMeshGeometry(Scene& scene) {
    if (!m_activeTubeMesh) {
        if (m_spline.getPointCount() >= 2) {
            m_activeTubeMesh = new Mesh();
            m_activeTubeMesh->isTube = true;
            m_activeTubeMesh->tubeData = std::make_unique<TubeData>();
            m_activeTubeMesh->outlinerName = "Tube " + std::to_string(m_activeTubeMesh->m_id);
            scene.addMesh(m_activeTubeMesh);
            scene.selectMesh(m_activeTubeMesh);
        } else {
            return;
        }
    }
    rebuildTubeMesh(scene, m_activeTubeMesh);
}

Mesh* TubeTool::bakeToMesh(Scene& scene) {
    if (!m_activeTubeMesh) return nullptr;
    return bakeMesh(scene, m_activeTubeMesh);
}

Mesh* TubeTool::bakeMesh(Scene& scene, Mesh* mesh) {
    if (!mesh) return nullptr;

    if (mesh == m_activeTubeMesh) {
        rebuildMeshGeometry(scene);
        m_activeTubeMesh = nullptr;
        m_spline.clear();
        m_rawStroke.clear();
        m_selectedPointIdx = -1;
        m_hoveredPointIdx = -1;
        m_snapTargetPointIdx = -1;
        m_hoveredCurveSeg = -1;
        m_hoveredProfilePtIdx = -1;
        m_selectedProfilePtIdx = -1;
        m_snapTargetProfilePtIdx = -1;
        m_hoveredProfileSeg = -1;
        m_isDraggingProfilePoint = false;
        m_profilePointWasMoved = false;
    }

    mesh->isTube = false;
    mesh->tubeData = nullptr;

    scene.pushHistoryState();
    scene.selectMesh(mesh);
    GuiManager::showToast("Tube baked into editable mesh", 2.5f);
    return mesh;
}

void TubeTool::bakeAllTubes(Scene& scene) {
    int count = 0;
    for (Mesh* m : scene.getMeshes()) {
        if (m && m->isTube) {
            m->isTube = false;
            m->tubeData = nullptr;
            count++;
        }
    }
    if (m_activeTubeMesh) {
        m_activeTubeMesh = nullptr;
        m_spline.clear();
        m_rawStroke.clear();
        m_selectedPointIdx = -1;
        m_hoveredPointIdx = -1;
        m_snapTargetPointIdx = -1;
        m_hoveredCurveSeg = -1;
        m_hoveredProfilePtIdx = -1;
        m_selectedProfilePtIdx = -1;
        m_snapTargetProfilePtIdx = -1;
        m_hoveredProfileSeg = -1;
        m_isDraggingProfilePoint = false;
        m_profilePointWasMoved = false;
    }
    if (count > 0) {
        scene.pushHistoryState();
        GuiManager::showToast("Baked all tubes into editable meshes", 2.5f);
    }
}

void TubeTool::renderOverlay(
    Scene& scene, const Camera& camera, float viewportW, float viewportH
) {
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList) return;

    // 1. Draw raw freehand stroke in progress
    if (m_isDrawingStroke) {
        if (m_rawStroke.size() >= 2) {
            for (size_t i = 1; i < m_rawStroke.size(); ++i) {
                glm::vec3 s0 = camera.project(m_rawStroke[i - 1].position);
                glm::vec3 s1 = camera.project(m_rawStroke[i].position);
                if (s0.z < 0.0f || s1.z < 0.0f) continue;
                drawList->AddLine(
                    ImVec2(s0.x, s0.y), ImVec2(s1.x, s1.y),
                    IM_COL32(0, 230, 210, 240), 2.5f
                );
            }
        }
        return;
    }

    // Collect all selected unfinalized tubes
    std::vector<Mesh*> selectedTubes;
    for (Mesh* m : scene.getSelectedMeshes()) {
        if (m && m->isTube && m->tubeData != nullptr) {
            if (std::find(selectedTubes.begin(), selectedTubes.end(), m) == selectedTubes.end()) {
                selectedTubes.push_back(m);
            }
        }
    }
    Mesh* primaryMesh = scene.getSelected();
    if (primaryMesh && primaryMesh->isTube && primaryMesh->tubeData != nullptr) {
        if (std::find(selectedTubes.begin(), selectedTubes.end(), primaryMesh) == selectedTubes.end()) {
            selectedTubes.push_back(primaryMesh);
        }
    }

    // In BRUSH_TUBE mode, if no mesh is explicitly selected in scene yet, but m_activeTubeMesh exists with curve:
    if (selectedTubes.empty() && m_sculpt.getBrush() == BRUSH_TUBE) {
        if (m_activeTubeMesh && m_activeTubeMesh->isTube && m_activeTubeMesh->tubeData != nullptr) {
            selectedTubes.push_back(m_activeTubeMesh);
        }
    }

    if (selectedTubes.empty()) {
        return;
    }

    bool isTubeTool = (m_sculpt.getBrush() == BRUSH_TUBE);

    // If primaryMesh is a tube and not loaded yet, ensure it is loaded into tubeTool
    if (!isInteracting() && primaryMesh && primaryMesh->isTube && primaryMesh->tubeData != nullptr && m_activeTubeMesh != primaryMesh) {
        loadFromMesh(primaryMesh);
    }

    // Render curves and control points for all selected tubes
    for (Mesh* tMesh : selectedTubes) {
        bool isPrimary = (tMesh == m_activeTubeMesh);
        const CurveSpline& spline = isPrimary ? m_spline : tMesh->tubeData->spline;
        if (spline.getPointCount() < 2) continue;

        // 2. Draw smooth spline centerline (dense sampling to look smooth on screen)
        int numSamples = std::clamp(static_cast<int>(spline.getPointCount() * 32), 64, 512);
        std::vector<ImVec2> screenPoints;
        screenPoints.reserve(numSamples);

        for (int i = 0; i < numSamples; ++i) {
            float u = static_cast<float>(i) / static_cast<float>(numSamples - 1);
            glm::vec3 worldPos = spline.evaluatePosition(u);
            glm::vec3 screenPos = camera.project(worldPos);
            if (screenPos.z >= 0.0f && screenPos.z <= 1.0f) {
                screenPoints.emplace_back(screenPos.x, screenPos.y);
            }
        }

        if (screenPoints.size() >= 2) {
            // Shadow line
            ImU32 shadowCol = isPrimary ? IM_COL32(0, 0, 0, 160) : IM_COL32(0, 0, 0, 130);
            float shadowThick = isPrimary ? 4.5f : 3.8f;
            for (size_t i = 1; i < screenPoints.size(); ++i) {
                drawList->AddLine(screenPoints[i - 1], screenPoints[i], shadowCol, shadowThick);
            }
            // Accent curve line
            ImU32 lineCol = isPrimary ? IM_COL32(0, 220, 200, 255) : IM_COL32(0, 190, 220, 210);
            float lineThick = isPrimary ? 2.0f : 1.6f;
            for (size_t i = 1; i < screenPoints.size(); ++i) {
                drawList->AddLine(screenPoints[i - 1], screenPoints[i], lineCol, lineThick);
            }
        }

        // Hover dot along curve indicating click to insert point (ONLY in Tube Tool and on primary active tube)
        if (isTubeTool && isPrimary && m_hoveredCurveSeg != -1 && m_hoveredPointIdx == -1 && !m_isDraggingPoint && !m_isDrawingStroke) {
            ImVec2 hPos(m_hoveredCurveScreenPos.x, m_hoveredCurveScreenPos.y);
            drawList->AddCircleFilled(hPos, 6.0f, IM_COL32(0, 0, 0, 180));
            drawList->AddCircleFilled(hPos, 4.5f, IM_COL32(255, 255, 255, 255));
            drawList->AddCircle(hPos, 4.5f, IM_COL32(0, 230, 220, 255), 0, 2.0f);
            bool bothSharp = (m_hoveredCurveSeg >= 0 && m_hoveredCurveSeg + 1 < static_cast<int>(spline.getPointCount()) &&
                              spline.getPoint(m_hoveredCurveSeg).isCorner && spline.getPoint(m_hoveredCurveSeg + 1).isCorner);
            const char* addTip = bothSharp ? "+ Add Corner Point" : "+ Add Point";
            drawList->AddText(ImVec2(hPos.x + 8.0f, hPos.y - 8.0f), bothSharp ? IM_COL32(255, 220, 100, 230) : IM_COL32(255, 255, 255, 230), addTip);
        }

        // Draw snap guide line when point is magnetically attracted to adjacent neighbor (ONLY in Tube Tool and on primary active tube)
        if (isTubeTool && isPrimary && m_isDraggingPoint && !m_isExtrudingPoint && m_snapTargetPointIdx >= 0 && m_snapTargetPointIdx < static_cast<int>(spline.getPointCount()) &&
            m_selectedPointIdx >= 0 && m_selectedPointIdx < static_cast<int>(spline.getPointCount()) &&
            std::abs(m_snapTargetPointIdx - m_selectedPointIdx) == 1) {
            glm::vec3 sDrag = camera.project(spline.getPoint(m_selectedPointIdx).position);
            glm::vec3 sTarget = camera.project(spline.getPoint(m_snapTargetPointIdx).position);
            if (sDrag.z >= 0.0f && sDrag.z <= 1.0f && sTarget.z >= 0.0f && sTarget.z <= 1.0f) {
                drawList->AddLine(ImVec2(sDrag.x, sDrag.y), ImVec2(sTarget.x, sTarget.y), IM_COL32(255, 140, 0, 230), 2.5f);
            }
        }

        // 3. Draw Control Points & Thickness Handles
        float baseR = isPrimary ? 7.0f : 5.8f;
        for (size_t i = 0; i < spline.getPointCount(); ++i) {
            const auto& pt = spline.getPoint(i);
            glm::vec3 sPos = camera.project(pt.position);
            if (sPos.z < 0.0f || sPos.z > 1.0f) continue;

            ImVec2 p(sPos.x, sPos.y);
            bool isPointHovered = isPrimary && (static_cast<int>(i) == m_hoveredPointIdx);
            bool isSelected = isPrimary && (static_cast<int>(i) == m_selectedPointIdx);
            bool isSnapTarget = isTubeTool && isPrimary && (!m_isExtrudingPoint && static_cast<int>(i) == m_snapTargetPointIdx);
            bool isHandleHovered = isTubeTool && isPrimary && (static_cast<int>(i) == m_hoveredRadiusHandleIdx);
            bool isHandleDragging = isTubeTool && isPrimary && (m_isDraggingRadius && static_cast<int>(i) == m_selectedPointIdx);

            // Compute thickness stick and square handle (ONLY in Tube Tool and on primary active tube)
            if (isTubeTool && isPrimary) {
                glm::vec3 cWorld, hWorld, sideDir;
                glm::vec2 cScreen, hScreen;
                bool hasHandle = getPointHandleInfo(camera, static_cast<int>(i), cWorld, hWorld, cScreen, hScreen, sideDir);

                if (hasHandle) {
                    // Optional faint cross-section ring if handle or point is hovered/dragged/selected
                    if (isSelected || isPointHovered || isHandleHovered || isHandleDragging) {
                        glm::vec3 viewDir = glm::normalize(camera.getPivot() - camera.computePosition());
                        glm::vec3 right = sideDir;
                        glm::vec3 up = glm::normalize(glm::cross(right, viewDir));

                        const int ringSteps = 24;
                        std::vector<ImVec2> ringScreenPts;
                        ringScreenPts.reserve(ringSteps + 1);

                        for (int k = 0; k <= ringSteps; ++k) {
                            float phi = 2.0f * (float)M_PI * static_cast<float>(k) / static_cast<float>(ringSteps);
                            glm::vec3 ringWorld = pt.position + (right * std::cos(phi) + up * std::sin(phi)) * pt.radius;
                            glm::vec3 sRing = camera.project(ringWorld);
                            if (sRing.z >= 0.0f && sRing.z <= 1.0f) {
                                ringScreenPts.emplace_back(sRing.x, sRing.y);
                            }
                        }

                        ImU32 ringCol = (isHandleHovered || isHandleDragging) ? IM_COL32(255, 220, 50, 200) : IM_COL32(255, 255, 255, 120);
                        if (ringScreenPts.size() >= 2) {
                            for (size_t k = 1; k < ringScreenPts.size(); ++k) {
                                drawList->AddLine(ringScreenPts[k - 1], ringScreenPts[k], ringCol, (isHandleHovered || isHandleDragging) ? 1.8f : 1.0f);
                            }
                        }
                    }

                    // Draw Stick Line from point to square
                    // Shadow
                    drawList->AddLine(ImVec2(cScreen.x, cScreen.y), ImVec2(hScreen.x, hScreen.y), IM_COL32(0, 0, 0, 160), 3.5f);
                    // Accent stick
                    ImU32 stickCol;
                    if (isHandleHovered || isHandleDragging) {
                        stickCol = IM_COL32(255, 230, 40, 255); // Glowing yellow
                    } else if (isSelected) {
                        stickCol = IM_COL32(255, 200, 50, 230); // Amber
                    } else {
                        stickCol = IM_COL32(180, 235, 255, 200); // Light cyan
                    }
                    drawList->AddLine(ImVec2(cScreen.x, cScreen.y), ImVec2(hScreen.x, hScreen.y), stickCol, (isHandleHovered || isHandleDragging || isSelected) ? 2.5f : 1.8f);

                    // Draw Square Handle at end of stick
                    float sqHalf = (isHandleHovered || isHandleDragging) ? 6.5f : (isSelected ? 5.5f : 4.5f);
                    ImVec2 sqMin(hScreen.x - sqHalf, hScreen.y - sqHalf);
                    ImVec2 sqMax(hScreen.x + sqHalf, hScreen.y + sqHalf);

                    // Square shadow
                    drawList->AddRectFilled(ImVec2(sqMin.x - 1.5f, sqMin.y - 1.5f), ImVec2(sqMax.x + 1.5f, sqMax.y + 1.5f), IM_COL32(0, 0, 0, 180), 2.0f);

                    // Square fill
                    ImU32 sqFill;
                    if (isHandleHovered || isHandleDragging) {
                        sqFill = IM_COL32(255, 230, 40, 255);
                    } else if (isSelected) {
                        sqFill = IM_COL32(255, 195, 30, 240);
                    } else {
                        sqFill = IM_COL32(245, 250, 255, 235);
                    }
                    drawList->AddRectFilled(sqMin, sqMax, sqFill, 2.0f);
                    drawList->AddRect(sqMin, sqMax, IM_COL32(20, 25, 30, 240), 2.0f, 0, 1.5f);

                    // Radius tooltip
                    if (isHandleHovered || isHandleDragging || isSelected) {
                        char rText[32];
                        snprintf(rText, sizeof(rText), "R: %.2f", pt.radius);
                        drawList->AddText(ImVec2(sqMax.x + 5.0f, hScreen.y - 7.0f), (isHandleHovered || isHandleDragging) ? IM_COL32(255, 240, 100, 255) : IM_COL32(255, 255, 255, 220), rText);
                    }
                }
            }

            // Draw Center Control Point
            float r = (isPointHovered || isSnapTarget) ? (baseR + 2.5f) : baseR;

            ImU32 fillCol;
            if (isSnapTarget) {
                fillCol = IM_COL32(255, 140, 20, 255); // Glowing Orange (Snap target)
            } else if (i == 0) {
                fillCol = IM_COL32(40, 220, 90, 255); // Green (Start)
            } else if (i == spline.getPointCount() - 1) {
                fillCol = IM_COL32(240, 60, 60, 255); // Red (End/Tip)
            } else {
                fillCol = isPrimary ? IM_COL32(0, 200, 230, 255) : IM_COL32(0, 180, 220, 235); // Cyan
            }

            // Highlight ring if snap target
            if (isSnapTarget) {
                drawList->AddCircle(p, r + 7.0f, IM_COL32(255, 140, 0, 255), 0, 3.0f);
                drawList->AddCircleFilled(p, r + 5.0f, IM_COL32(255, 160, 40, 90));
                drawList->AddText(ImVec2(p.x + r + 8.0f, p.y + 6.0f), IM_COL32(255, 180, 50, 255), "Snap & Merge");
            }

            // Drop shadow & Fill: Square for sharp corner point, Circle for smooth point
            if (pt.isCorner) {
                drawList->AddRectFilled(ImVec2(p.x - r - 1.5f, p.y - r - 1.5f), ImVec2(p.x + r + 1.5f, p.y + r + 1.5f), IM_COL32(0, 0, 0, 180), 2.0f);
                drawList->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), fillCol, 2.0f);
                drawList->AddRect(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), isSelected ? IM_COL32(255, 230, 40, 255) : IM_COL32(255, 255, 255, 255), 2.0f, 0, isSelected ? 2.5f : 1.5f);
            } else {
                drawList->AddCircleFilled(p, r + 1.5f, IM_COL32(0, 0, 0, 180));
                drawList->AddCircleFilled(p, r, fillCol);
                drawList->AddCircle(p, r, isSelected ? IM_COL32(255, 230, 40, 255) : IM_COL32(255, 255, 255, 255), 0, isSelected ? 2.5f : 1.5f);
            }

            // Hover hint to inform user about click-to-toggle or Alt-extrude (ONLY in Tube Tool on primary tube)
            if (isTubeTool && isPrimary && isPointHovered && !m_isDraggingPoint && !m_isDraggingRadius) {
                bool isEndPoint = (i == 0 || i == spline.getPointCount() - 1);
                if (isEndPoint) {
                    const char* tip = pt.isCorner ? "Corner (Click: Smooth | Alt+Drag: Extrude)" : "Smooth (Click: Break Angle | Alt+Drag: Extrude)";
                    drawList->AddText(ImVec2(p.x + r + 6.0f, p.y + 8.0f), IM_COL32(255, 230, 120, 255), tip);
                } else {
                    const char* tip = pt.isCorner ? "Corner (Click: Smooth | Alt+Click: Delete)" : "Smooth (Click: Break Angle | Alt+Click: Delete)";
                    drawList->AddText(ImVec2(p.x + r + 6.0f, p.y + 8.0f), IM_COL32(255, 230, 120, 255), tip);
                }
            }
        }
    }

    // 4. Render Cross-Section Profile Graph directly on tube end cross-section (ONLY in Tube Tool)
    if (isTubeTool && showProfileAtEnd && profileType == TubeProfileType::CustomGraph && !crossSectionProfile.empty()) {
        EvaluatedFrame frame;
        int ptIdx = -1;
        if (getEndProfileFrame(frame, &ptIdx)) {
            float r = std::max(frame.radius, 0.001f);
            glm::vec3 cWorld = frame.position;
            glm::vec3 cProj = camera.project(cWorld);

            if (cProj.z >= 0.0f && cProj.z <= 1.0f) {
                ImVec2 centerScreen(cProj.x, cProj.y);

                // A. Local coordinate axes (Normal = X in red, Binormal = Y in green)
                glm::vec3 xWorld = cWorld + frame.normal * (r * 1.35f);
                glm::vec3 yWorld = cWorld + frame.binormal * (r * 1.35f);
                glm::vec3 xProj = camera.project(xWorld);
                glm::vec3 yProj = camera.project(yWorld);

                if (xProj.z >= 0.0f && xProj.z <= 1.0f) {
                    drawList->AddLine(centerScreen, ImVec2(xProj.x, xProj.y), IM_COL32(255, 90, 90, 180), 1.5f);
                    drawList->AddText(ImVec2(xProj.x + 3.0f, xProj.y - 7.0f), IM_COL32(255, 120, 120, 220), "X");
                }
                if (yProj.z >= 0.0f && yProj.z <= 1.0f) {
                    drawList->AddLine(centerScreen, ImVec2(yProj.x, yProj.y), IM_COL32(90, 230, 120, 180), 1.5f);
                    drawList->AddText(ImVec2(yProj.x + 3.0f, yProj.y - 7.0f), IM_COL32(120, 255, 150, 220), "Y");
                }

                // B. Reference Unit Circle (radius = 1.0)
                const int circleSteps = 32;
                std::vector<ImVec2> unitCirclePts;
                unitCirclePts.reserve(circleSteps + 1);
                for (int k = 0; k <= circleSteps; ++k) {
                    float phi = 2.0f * (float)M_PI * static_cast<float>(k) / static_cast<float>(circleSteps);
                    glm::vec3 circWorld = cWorld + (frame.normal * std::cos(phi) + frame.binormal * std::sin(phi)) * r;
                    glm::vec3 circProj = camera.project(circWorld);
                    if (circProj.z >= 0.0f && circProj.z <= 1.0f) {
                        unitCirclePts.emplace_back(circProj.x, circProj.y);
                    }
                }
                if (unitCirclePts.size() >= 2) {
                    for (size_t k = 1; k < unitCirclePts.size(); ++k) {
                        drawList->AddLine(unitCirclePts[k - 1], unitCirclePts[k], IM_COL32(255, 255, 255, 55), 1.0f);
                    }
                }

                // C. Profile Contour Loop
                int N = static_cast<int>(crossSectionProfile.size());
                int contourSamples = std::max(64, N * 16);
                std::vector<ImVec2> contourScreenPts;
                contourScreenPts.reserve(contourSamples + 1);

                for (int k = 0; k <= contourSamples; ++k) {
                    float u = static_cast<float>(k) / static_cast<float>(contourSamples);
                    glm::vec2 p2D = evaluateCrossSectionContour(u);
                    glm::vec3 loopWorld = cWorld + (frame.normal * p2D.x + frame.binormal * p2D.y) * r;
                    glm::vec3 loopProj = camera.project(loopWorld);
                    if (loopProj.z >= 0.0f && loopProj.z <= 1.0f) {
                        contourScreenPts.emplace_back(loopProj.x, loopProj.y);
                    }
                }

                if (contourScreenPts.size() >= 2) {
                    // Shadow line
                    for (size_t k = 1; k < contourScreenPts.size(); ++k) {
                        drawList->AddLine(contourScreenPts[k - 1], contourScreenPts[k], IM_COL32(0, 0, 0, 180), 4.0f);
                    }
                    // Glowing Magenta/Violet contour line
                    ImU32 contourCol = (m_isDraggingProfilePoint || m_hoveredProfileSeg != -1) ?
                        IM_COL32(255, 140, 230, 255) : IM_COL32(225, 70, 200, 230);
                    for (size_t k = 1; k < contourScreenPts.size(); ++k) {
                        drawList->AddLine(contourScreenPts[k - 1], contourScreenPts[k], contourCol, 2.2f);
                    }
                }

                // D. Hover indicator dot on contour to add point
                if (m_hoveredProfileSeg != -1 && m_hoveredProfilePtIdx == -1 && !m_isDraggingProfilePoint) {
                    ImVec2 hPos(m_hoveredProfileScreenPos.x, m_hoveredProfileScreenPos.y);
                    drawList->AddCircleFilled(hPos, 6.0f, IM_COL32(0, 0, 0, 180));
                    drawList->AddCircleFilled(hPos, 4.5f, IM_COL32(255, 255, 255, 255));
                    drawList->AddCircle(hPos, 4.5f, IM_COL32(255, 70, 200, 255), 0, 2.0f);

                    int i1 = m_hoveredProfileSeg;
                    int i2 = (m_hoveredProfileSeg + 1) % N;
                    bool bothCorners = crossSectionProfile[i1].isCorner && crossSectionProfile[i2].isCorner;
                    const char* tip = bothCorners ? "+ Add Corner Point" : "+ Add Profile Point";
                    drawList->AddText(ImVec2(hPos.x + 8.0f, hPos.y - 8.0f),
                        bothCorners ? IM_COL32(255, 220, 100, 240) : IM_COL32(255, 255, 255, 240), tip);
                }

                // E. Snap guide line during profile point dragging
                if (m_isDraggingProfilePoint && m_snapTargetProfilePtIdx >= 0 && m_snapTargetProfilePtIdx < N &&
                    m_selectedProfilePtIdx >= 0 && m_selectedProfilePtIdx < N) {
                    const auto& dragPt = crossSectionProfile[m_selectedProfilePtIdx];
                    const auto& targetPt = crossSectionProfile[m_snapTargetProfilePtIdx];
                    glm::vec3 wDrag = cWorld + (frame.normal * dragPt.pos.x + frame.binormal * dragPt.pos.y) * r;
                    glm::vec3 wTarget = cWorld + (frame.normal * targetPt.pos.x + frame.binormal * targetPt.pos.y) * r;
                    glm::vec3 sDrag = camera.project(wDrag);
                    glm::vec3 sTarget = camera.project(wTarget);
                    if (sDrag.z >= 0.0f && sDrag.z <= 1.0f && sTarget.z >= 0.0f && sTarget.z <= 1.0f) {
                        drawList->AddLine(ImVec2(sDrag.x, sDrag.y), ImVec2(sTarget.x, sTarget.y), IM_COL32(255, 140, 0, 230), 2.5f);
                    }
                }

                // F. Profile Control Points
                float profPtR = 6.0f;
                for (int i = 0; i < N; ++i) {
                    const auto& pt = crossSectionProfile[i];
                    glm::vec3 ptWorld = cWorld + (frame.normal * pt.pos.x + frame.binormal * pt.pos.y) * r;
                    glm::vec3 sPos = camera.project(ptWorld);
                    if (sPos.z < 0.0f || sPos.z > 1.0f) continue;

                    ImVec2 p(sPos.x, sPos.y);
                    bool isHovered = (i == m_hoveredProfilePtIdx);
                    bool isSelected = (i == m_selectedProfilePtIdx);
                    bool isSnapTarget = (i == m_snapTargetProfilePtIdx);

                    float curR = (isHovered || isSnapTarget) ? (profPtR + 2.5f) : profPtR;

                    ImU32 fillCol;
                    if (isSnapTarget) {
                        fillCol = IM_COL32(255, 140, 20, 255); // Orange
                    } else if (isSelected) {
                        fillCol = IM_COL32(255, 215, 30, 255); // Amber / Gold
                    } else if (pt.isCorner) {
                        fillCol = IM_COL32(255, 80, 150, 255); // Vivid Pink
                    } else {
                        fillCol = IM_COL32(200, 60, 220, 255); // Violet
                    }

                    // Snap target highlight
                    if (isSnapTarget) {
                        drawList->AddCircle(p, curR + 6.0f, IM_COL32(255, 140, 0, 255), 0, 3.0f);
                        drawList->AddCircleFilled(p, curR + 4.0f, IM_COL32(255, 160, 40, 90));
                        drawList->AddText(ImVec2(p.x + curR + 6.0f, p.y + 4.0f), IM_COL32(255, 180, 50, 255), "Snap & Merge");
                    }

                    // Shadow and shape: Square for sharp corner, Circle for smooth point
                    if (pt.isCorner) {
                        drawList->AddRectFilled(ImVec2(p.x - curR - 1.5f, p.y - curR - 1.5f), ImVec2(p.x + curR + 1.5f, p.y + curR + 1.5f), IM_COL32(0, 0, 0, 180), 2.0f);
                        drawList->AddRectFilled(ImVec2(p.x - curR, p.y - curR), ImVec2(p.x + curR, p.y + curR), fillCol, 2.0f);
                        drawList->AddRect(ImVec2(p.x - curR, p.y - curR), ImVec2(p.x + curR, p.y + curR),
                            isSelected ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 230, 250, 220), 2.0f, 0, isSelected ? 2.2f : 1.5f);
                    } else {
                        drawList->AddCircleFilled(p, curR + 1.5f, IM_COL32(0, 0, 0, 180));
                        drawList->AddCircleFilled(p, curR, fillCol);
                        drawList->AddCircle(p, curR,
                            isSelected ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 230, 250, 220), 0, isSelected ? 2.2f : 1.5f);
                    }

                    // Tooltip on hovered point
                    if (isHovered && !m_isDraggingProfilePoint) {
                        const char* tip = pt.isCorner ?
                            "Profile Corner (Click: Smooth | Alt+Click: Delete)" :
                            "Profile Smooth (Click: Sharp Corner | Alt+Click: Delete)";
                        drawList->AddText(ImVec2(p.x + curR + 6.0f, p.y + 6.0f), IM_COL32(255, 220, 140, 255), tip);
                    }
                }

                // G. Header Badge "Profile Graph (End)" / "Profile Graph (Start)"
                const char* badgeText = (ptIdx == 0) ? "Profile Graph [Start]" : "Profile Graph [End]";
                ImVec2 badgePos(centerScreen.x - 50.0f, centerScreen.y - (r * 30.0f + 25.0f));
                badgePos.y = std::clamp(badgePos.y, centerScreen.y - 70.0f, centerScreen.y - 20.0f);
                ImVec2 badgeTextSize = ImGui::CalcTextSize(badgeText);
                ImVec2 bMin(badgePos.x - 6.0f, badgePos.y - 2.0f);
                ImVec2 bMax(badgePos.x + badgeTextSize.x + 6.0f, badgePos.y + badgeTextSize.y + 2.0f);
                drawList->AddRectFilled(bMin, bMax, IM_COL32(20, 20, 30, 200), 4.0f);
                drawList->AddRect(bMin, bMax, IM_COL32(220, 80, 200, 220), 4.0f, 0, 1.2f);
                drawList->AddText(badgePos, IM_COL32(255, 220, 255, 240), badgeText);
            }
        }
    }
}
