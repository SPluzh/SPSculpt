#include "editing/CurveFoldsTool.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "mesh/Mesh.h"
#include "mesh/NormalCalc.h"
#include "editing/SculptManager.h"
#include "gui/GuiManager.h"
#include "gui/IconsLucide.h"
#include <imgui.h>
#include <cmath>
#include <algorithm>
#include <limits>
#include <omp.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {
    static bool isMouseOverAnyImGuiWindow() {
        return ImGui::GetIO().WantCaptureMouse;
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
        if (a > -EPSILON && a < EPSILON) return false;
        float f = 1.0f / a;
        glm::vec3 s = rayOrigin - v0;
        float u = f * glm::dot(s, h);
        if (u < 0.0f || u > 1.0f) return false;
        glm::vec3 q = glm::cross(s, edge1);
        float v = f * glm::dot(rayDir, q);
        if (v < 0.0f || u + v > 1.0f) return false;
        t = f * glm::dot(edge2, q);
        return t > EPSILON;
    }
}

CurveFoldsTool::CurveFoldsTool(SculptManager& sm)
    : m_sculpt(sm)
{
}

CurveFoldsTool::~CurveFoldsTool() {
}

void CurveFoldsTool::onActivate(Scene& scene) {
    m_isDrawingStroke = false;
    m_isDraggingPoint = false;
    m_isDraggingRadius = false;
    m_selectedPointIdx = -1;
    m_hoveredCurveIdx = -1;
    m_hoveredPointIdx = -1;
    m_hoveredRadiusHandleCurve = -1;
    m_hoveredRadiusHandlePoint = -1;
    m_hoveredSegmentCurve = -1;
    m_rawStroke.clear();

    ensureTargetMeshInitialized(scene);
    if (m_targetMesh && !m_curves.empty()) {
        updateLivePreview(scene, true);
    }
}

void CurveFoldsTool::onDeactivate() {
    m_isDrawingStroke = false;
    m_isDraggingPoint = false;
    m_isDraggingRadius = false;
    m_rawStroke.clear();
}

void CurveFoldsTool::notifyMeshDeleted(Mesh* mesh) {
    if (mesh == m_targetMesh) {
        m_targetMesh = nullptr;
        m_baseVerts.clear();
        m_baseNormals.clear();
        m_affectedVerts.clear();
        m_curves.clear();
        m_activeCurveIdx = -1;
        m_selectedPointIdx = -1;
    }
}

void CurveFoldsTool::setActiveCurveIndex(int idx) {
    if (idx < 0 || idx >= static_cast<int>(m_curves.size())) {
        m_activeCurveIdx = -1;
        m_selectedPointIdx = -1;
    } else {
        m_activeCurveIdx = idx;
        m_selectedPointIdx = std::min(m_selectedPointIdx, static_cast<int>(m_curves[idx].spline.getPointCount()) - 1);
    }
}

void CurveFoldsTool::startNewCurve() {
    if (!m_curves.empty() && m_curves.back().spline.getPointCount() < 2) {
        m_curves.pop_back();
    }
    m_activeCurveIdx = -1;
    m_selectedPointIdx = -1;
    m_rawStroke.clear();
}

void CurveFoldsTool::deleteActiveCurve() {
    if (m_activeCurveIdx >= 0 && m_activeCurveIdx < static_cast<int>(m_curves.size())) {
        m_curves.erase(m_curves.begin() + m_activeCurveIdx);
        if (m_curves.empty()) {
            m_activeCurveIdx = -1;
        } else {
            m_activeCurveIdx = std::clamp(m_activeCurveIdx, 0, static_cast<int>(m_curves.size()) - 1);
        }
        m_selectedPointIdx = -1;
        m_previewDirty = true;
    }
}

void CurveFoldsTool::clearAllCurves() {
    m_curves.clear();
    m_activeCurveIdx = -1;
    m_selectedPointIdx = -1;
    m_rawStroke.clear();
    m_previewDirty = true;
}

void CurveFoldsTool::ensureTargetMeshInitialized(Scene& scene) {
    Mesh* sel = scene.getSelected();
    if (sel != m_targetMesh) {
        if (m_targetMesh && !m_baseVerts.empty() && m_targetMesh->verts.size() == m_baseVerts.size()) {
            m_targetMesh->verts = m_baseVerts;
            m_targetMesh->normals = m_baseNormals;
            m_targetMesh->isDirty = true;
            m_targetMesh->isVertexDirty = true;
            m_targetMesh->dirtyVertMin = 0;
            m_targetMesh->dirtyVertMax = m_targetMesh->nbVerts > 0 ? (m_targetMesh->nbVerts - 1) : 0;
        }
        m_targetMesh = sel;
        if (m_targetMesh) {
            m_baseVerts = m_targetMesh->verts;
            m_baseNormals = m_targetMesh->normals;
            m_affectedVerts.clear();
        } else {
            m_baseVerts.clear();
            m_baseNormals.clear();
            m_affectedVerts.clear();
        }
        m_previewDirty = true;
    }
}

void CurveFoldsTool::cancelFolds(Scene& scene) {
    ensureTargetMeshInitialized(scene);
    if (m_targetMesh && !m_baseVerts.empty() && m_targetMesh->verts.size() == m_baseVerts.size()) {
        m_targetMesh->verts = m_baseVerts;
        m_targetMesh->normals = m_baseNormals;
        m_targetMesh->isDirty = true;
        m_targetMesh->isVertexDirty = true;
        m_targetMesh->dirtyVertMin = 0;
        m_targetMesh->dirtyVertMax = m_targetMesh->nbVerts > 0 ? (m_targetMesh->nbVerts - 1) : 0;
        m_targetMesh->ensureBVHBuilt();
        m_affectedVerts.clear();
    }
    clearAllCurves();
    GuiManager::showToast("Curve Folds Cancelled", 1.5f);
}

void CurveFoldsTool::applyFolds(Scene& scene) {
    ensureTargetMeshInitialized(scene);
    if (!m_targetMesh || m_baseVerts.empty() || m_curves.empty()) {
        clearAllCurves();
        return;
    }

    // Restore to clean base verts first so UndoManager records the exact initial un-deformed mesh
    m_targetMesh->verts = m_baseVerts;
    m_targetMesh->normals = m_baseNormals;
    scene.pushHistoryState();

    // Now apply deformation permanently
    applyDeformationInternal(m_targetMesh);

    // Full normal and BVH bake
    computeNormalsDirect(
        m_targetMesh->verts.data(), m_targetMesh->nbVerts,
        m_targetMesh->faces.data(), m_targetMesh->nbFaces,
        m_targetMesh->faceNormals.data(), m_targetMesh->normals.data()
    );

    m_targetMesh->isDirty = true;
    m_targetMesh->isVertexDirty = true;
    m_targetMesh->dirtyVertMin = 0;
    m_targetMesh->dirtyVertMax = m_targetMesh->nbVerts > 0 ? (m_targetMesh->nbVerts - 1) : 0;
    m_targetMesh->ensureBVHBuilt();
    m_targetMesh->bvh.build(*m_targetMesh);

    // Update cached base geometry to new baked state
    m_baseVerts = m_targetMesh->verts;
    m_baseNormals = m_targetMesh->normals;
    m_affectedVerts.clear();
    clearAllCurves();

    GuiManager::showToast("Applied & Baked Curve Folds", 1.8f);
}

bool CurveFoldsTool::raycastSurface(
    const Scene& scene, const Camera& camera, float mouseX, float mouseY,
    glm::vec3& outPos, glm::vec3& outNorm, Mesh** outHitMesh
) const {
    Ray ray = camera.getRay(mouseX, mouseY);

    float minT = std::numeric_limits<float>::infinity();
    glm::vec3 bestPos(0.0f);
    glm::vec3 bestNorm(0.0f, 1.0f, 0.0f);
    Mesh* bestMesh = nullptr;
    bool hitAny = false;

    // Prioritize currently selected target mesh if available
    const auto& meshes = scene.getMeshes();
    for (Mesh* mesh : meshes) {
        if (!mesh || !mesh->visibleV1 || mesh->nbFaces == 0 || mesh->verts.empty()) continue;

        glm::mat4 invMatrix = glm::inverse(mesh->matrix);
        glm::vec3 localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
        glm::vec3 localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

        mesh->ensureBVHBuilt();
        RayHit hit;
        if (mesh->bvh.raycast(*mesh, localRayOrigin, localRayDir, hit)) {
            glm::vec3 worldHit = glm::vec3(mesh->matrix * glm::vec4(hit.position, 1.0f));
            float worldT = glm::distance(ray.origin, worldHit);
            if (worldT < minT) {
                minT = worldT;
                bestPos = worldHit;
                bestMesh = mesh;
                bestNorm = glm::normalize(glm::vec3(glm::transpose(invMatrix) * glm::vec4(hit.normal, 0.0f)));
                hitAny = true;
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

glm::vec3 CurveFoldsTool::intersectRayPlane(
    const Camera& camera, float mouseX, float mouseY,
    const glm::vec3& planePoint, const glm::vec3& planeNormal
) const {
    Ray ray = camera.getRay(mouseX, mouseY);
    float denom = glm::dot(ray.dir, planeNormal);
    if (std::abs(denom) < 1e-6f) return planePoint;
    float t = glm::dot(planePoint - ray.origin, planeNormal) / denom;
    return ray.origin + ray.dir * t;
}

bool CurveFoldsTool::getPointHandleInfo(
    const Camera& camera, const CurveSpline& spline, int ptIdx,
    glm::vec3& outCenterWorld, glm::vec3& outHandleWorld,
    glm::vec2& outCenterScreen, glm::vec2& outHandleScreen,
    glm::vec3& outSideDir
) const {
    if (ptIdx < 0 || ptIdx >= static_cast<int>(spline.getPointCount())) return false;

    const auto& pt = spline.getPoint(ptIdx);
    outCenterWorld = pt.position;

    glm::vec3 sCenter = camera.project(outCenterWorld);
    if (sCenter.z < 0.0f || sCenter.z > 1.0f) return false;
    outCenterScreen = glm::vec2(sCenter.x, sCenter.y);

    size_t n = spline.getPointCount();
    glm::vec3 tangent(0.0f, 0.0f, 1.0f);
    if (n >= 2) {
        if (ptIdx == 0) {
            tangent = spline.getPoint(1).position - pt.position;
        } else if (ptIdx == static_cast<int>(n) - 1) {
            tangent = pt.position - spline.getPoint(n - 2).position;
        } else {
            tangent = spline.getPoint(ptIdx + 1).position - spline.getPoint(ptIdx - 1).position;
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

    // Consistent screen orientation: handle towards screen right/top
    glm::vec3 sTest = camera.project(outCenterWorld + side * pt.radius);
    if (sTest.x < outCenterScreen.x - 0.5f) {
        side = -side;
    } else if (std::abs(sTest.x - outCenterScreen.x) <= 0.5f && sTest.y > outCenterScreen.y) {
        side = -side;
    }

    outSideDir = side;
    outHandleWorld = outCenterWorld + side * pt.radius;

    glm::vec3 sHandle = camera.project(outHandleWorld);
    outHandleScreen = glm::vec2(sHandle.x, sHandle.y);

    return true;
}

int CurveFoldsTool::hitTestCurveRadiusHandle(
    const Camera& camera, float mouseX, float mouseY, int& outCurveIdx, int& outPointIdx, float thresholdPx
) const {
    outCurveIdx = -1;
    outPointIdx = -1;
    glm::vec2 mousePos(mouseX, mouseY);
    float bestDist = thresholdPx;

    // Check active curve first
    std::vector<int> curveIndices;
    if (m_activeCurveIdx >= 0 && m_activeCurveIdx < static_cast<int>(m_curves.size())) {
        curveIndices.push_back(m_activeCurveIdx);
    }
    for (int i = 0; i < static_cast<int>(m_curves.size()); ++i) {
        if (i != m_activeCurveIdx) curveIndices.push_back(i);
    }

    for (int cIdx : curveIndices) {
        const auto& spline = m_curves[cIdx].spline;
        for (int pIdx = 0; pIdx < static_cast<int>(spline.getPointCount()); ++pIdx) {
            glm::vec3 cWorld, hWorld, sideDir;
            glm::vec2 cScreen, hScreen;
            if (!getPointHandleInfo(camera, spline, pIdx, cWorld, hWorld, cScreen, hScreen, sideDir)) {
                continue;
            }

            float distSq = glm::distance(mousePos, hScreen);
            if (distSq < bestDist) {
                bestDist = distSq;
                outCurveIdx = cIdx;
                outPointIdx = pIdx;
            }

            // Also check stick line
            glm::vec2 ab = hScreen - cScreen;
            float len2 = glm::dot(ab, ab);
            if (len2 > 16.0f) {
                float t = std::clamp(glm::dot(mousePos - cScreen, ab) / len2, 0.0f, 1.0f);
                if (t > 0.35f) {
                    glm::vec2 proj = cScreen + t * ab;
                    float distLine = glm::distance(mousePos, proj);
                    if (distLine < 8.0f && distLine < bestDist) {
                        bestDist = distLine;
                        outCurveIdx = cIdx;
                        outPointIdx = pIdx;
                    }
                }
            }
        }
    }

    return outCurveIdx;
}

int CurveFoldsTool::hitTestAllCurves(
    const Camera& camera, float mouseX, float mouseY, int& outCurveIdx, int& outPointIdx, float thresholdPx
) const {
    outCurveIdx = -1;
    outPointIdx = -1;
    glm::vec2 mousePos(mouseX, mouseY);
    float bestDist = thresholdPx;

    // Check active curve first
    std::vector<int> curveIndices;
    if (m_activeCurveIdx >= 0 && m_activeCurveIdx < static_cast<int>(m_curves.size())) {
        curveIndices.push_back(m_activeCurveIdx);
    }
    for (int i = 0; i < static_cast<int>(m_curves.size()); ++i) {
        if (i != m_activeCurveIdx) curveIndices.push_back(i);
    }

    for (int cIdx : curveIndices) {
        const auto& spline = m_curves[cIdx].spline;
        for (int pIdx = 0; pIdx < static_cast<int>(spline.getPointCount()); ++pIdx) {
            glm::vec3 sPos = camera.project(spline.getPoint(pIdx).position);
            if (sPos.z < 0.0f || sPos.z > 1.0f) continue;

            float d = glm::distance(mousePos, glm::vec2(sPos.x, sPos.y));
            if (d < bestDist) {
                bestDist = d;
                outCurveIdx = cIdx;
                outPointIdx = pIdx;
            }
        }
    }

    return outCurveIdx;
}

int CurveFoldsTool::findClosestCurveSegment(
    const Camera& camera, float mouseX, float mouseY, int& outCurveIdx, float& outT, float thresholdPx
) const {
    outCurveIdx = -1;
    outT = 0.5f;
    glm::vec2 mousePos(mouseX, mouseY);
    float bestDist = thresholdPx;

    for (int cIdx = 0; cIdx < static_cast<int>(m_curves.size()); ++cIdx) {
        const auto& spline = m_curves[cIdx].spline;
        if (spline.getPointCount() < 2) continue;

        int numSegments = static_cast<int>(spline.getPointCount()) - 1;
        const int stepsPerSeg = 16;

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

                float d = glm::distance(mousePos, proj);
                if (d < bestDist) {
                    bestDist = d;
                    outCurveIdx = cIdx;
                    outT = u0 + t * (u1 - u0);
                }
            }
        }
    }

    return outCurveIdx;
}

void CurveFoldsTool::applyDeformationInternal(Mesh* mesh) {
    if (!mesh || m_baseVerts.empty() || m_curves.empty()) return;

    glm::mat4 invMat = glm::inverse(mesh->matrix);
    glm::mat3 normMat = glm::transpose(glm::inverse(glm::mat3(mesh->matrix)));
    float scaleFactor = glm::length(glm::vec3(invMat * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)));
    if (scaleFactor < 1e-5f) scaleFactor = 1.0f;

    struct PreparedCurve {
        std::vector<EvaluatedFrame> frames;
        float baseRadius = 15.0f;
        float foldDepth = 1.0f;
        float pinchStrength = 0.5f;
        float sharpness = 1.0f;
        float endTaperFalloff = 1.0f;
        FoldType foldType = FoldType::Ridge;
        bool inverted = false;
        bool isClosed = false;
        glm::vec3 aabbMin{1e9f};
        glm::vec3 aabbMax{-1e9f};
    };

    std::vector<PreparedCurve> prepCurves;
    auto symScales = m_sculpt.getUseSym() ? m_sculpt.getActiveSymmetryScales() : std::vector<glm::vec3>();

    for (const auto& curve : m_curves) {
        if (curve.spline.getPointCount() < 2) continue;

        int numSamples = std::clamp(static_cast<int>(curve.spline.getPointCount() * 24), 32, 512);
        std::vector<EvaluatedFrame> worldFrames = curve.spline.evaluateFrames(numSamples);
        if (worldFrames.size() < 2) continue;

        // Convert world frames into mesh local space
        std::vector<EvaluatedFrame> localFrames = worldFrames;
        for (auto& f : localFrames) {
            f.position = glm::vec3(invMat * glm::vec4(f.position, 1.0f));
            f.tangent = glm::normalize(glm::vec3(invMat * glm::vec4(f.tangent, 0.0f)));
            f.normal = glm::normalize(normMat * f.normal);
            f.binormal = glm::normalize(glm::cross(f.tangent, f.normal));
            f.normal = glm::normalize(glm::cross(f.binormal, f.tangent));
            f.radius *= scaleFactor;
        }

        std::vector<std::vector<EvaluatedFrame>> chainVariants;
        chainVariants.push_back(std::move(localFrames));

        // Add symmetric mirrors if symmetry is enabled
        for (const glm::vec3& sScale : symScales) {
            std::vector<EvaluatedFrame> symChain = chainVariants[0];
            for (auto& f : symChain) {
                f.position *= sScale;
                f.tangent = glm::normalize(f.tangent * sScale);
                f.normal = glm::normalize(f.normal * sScale);
                f.binormal = glm::normalize(glm::cross(f.tangent, f.normal));
                f.normal = glm::normalize(glm::cross(f.binormal, f.tangent));
            }
            chainVariants.push_back(std::move(symChain));
        }

        for (auto& chain : chainVariants) {
            PreparedCurve pc;
            pc.frames = std::move(chain);
            pc.baseRadius = baseRadius * scaleFactor;
            pc.foldDepth = foldDepth * curve.depthMultiplier * scaleFactor;
            pc.pinchStrength = pinchStrength;
            pc.sharpness = sharpness;
            pc.endTaperFalloff = endTaperFalloff;
            pc.foldType = foldType;
            pc.inverted = curve.inverted;
            pc.isClosed = curve.isClosed;

            for (const auto& f : pc.frames) {
                float rPad = f.radius * 1.5f;
                pc.aabbMin = glm::min(pc.aabbMin, f.position - glm::vec3(rPad));
                pc.aabbMax = glm::max(pc.aabbMax, f.position + glm::vec3(rPad));
            }
            prepCurves.push_back(std::move(pc));
        }
    }

    if (prepCurves.empty()) return;

    int nbVerts = mesh->nbVerts;
    const float* baseVertsPtr = m_baseVerts.data();
    const float* baseNormsPtr = m_baseNormals.data();
    const float* matsPtr = mesh->materials.empty() ? nullptr : mesh->materials.data();
    float* vertsPtr = mesh->verts.data();

    std::vector<uint32_t> affected;
    affected.reserve(nbVerts / 8);

    #pragma omp parallel
    {
        std::vector<uint32_t> localAffected;

        #pragma omp for schedule(guided)
        for (int vid = 0; vid < nbVerts; ++vid) {
            uint32_t vid3 = vid * 3;
            glm::vec3 Vbase(baseVertsPtr[vid3], baseVertsPtr[vid3 + 1], baseVertsPtr[vid3 + 2]);
            glm::vec3 Vnorm(baseNormsPtr[vid3], baseNormsPtr[vid3 + 1], baseNormsPtr[vid3 + 2]);
            float mask = matsPtr ? matsPtr[vid3 + 2] : 1.0f; // 1.0 = sculptable, 0.0 = masked

            if (mask <= 1e-4f) continue;

            glm::vec3 deltaTotal(0.0f);
            bool hasDeform = false;

            for (const auto& pc : prepCurves) {
                // AABB bounding box check
                if (Vbase.x < pc.aabbMin.x || Vbase.x > pc.aabbMax.x ||
                    Vbase.y < pc.aabbMin.y || Vbase.y > pc.aabbMax.y ||
                    Vbase.z < pc.aabbMin.z || Vbase.z > pc.aabbMax.z) {
                    continue;
                }

                // Find closest point on spline frames chain
                float bestDistSq = 1e9f;
                int bestSeg = -1;
                float bestT = 0.0f;

                int numSegs = static_cast<int>(pc.frames.size()) - 1;
                for (int s = 0; s < numSegs; ++s) {
                    const auto& fa = pc.frames[s];
                    const auto& fb = pc.frames[s + 1];

                    glm::vec3 ab = fb.position - fa.position;
                    float lenSq = glm::dot(ab, ab);
                    float t = (lenSq > 1e-7f) ? std::clamp(glm::dot(Vbase - fa.position, ab) / lenSq, 0.0f, 1.0f) : 0.0f;
                    glm::vec3 pProj = fa.position + t * ab;

                    glm::vec3 diff = Vbase - pProj;
                    float distSq = glm::dot(diff, diff);
                    if (distSq < bestDistSq) {
                        bestDistSq = distSq;
                        bestSeg = s;
                        bestT = t;
                    }
                }

                if (bestSeg < 0) continue;

                const auto& fa = pc.frames[bestSeg];
                const auto& fb = pc.frames[bestSeg + 1];

                glm::vec3 Pcurve = fa.position + bestT * (fb.position - fa.position);
                float R = glm::mix(fa.radius, fb.radius, bestT);
                if (R <= 1e-4f) continue;

                glm::vec3 T = glm::normalize(glm::mix(fa.tangent, fb.tangent, bestT));
                glm::vec3 N = glm::normalize(glm::mix(fa.normal, fb.normal, bestT));
                glm::vec3 B = glm::normalize(glm::cross(T, N));
                N = glm::normalize(glm::cross(B, T));

                float u = glm::mix(fa.u, fb.u, bestT);

                glm::vec3 D = Vbase - Pcurve;

                // Backface culling: don't deform rear walls of thin surfaces
                if (glm::dot(Vnorm, N) < -0.1f) continue;

                // Normal-distance cutoff
                if (std::abs(glm::dot(D, N)) > R * 1.5f) continue;

                // Transverse relative offset x along binormal B
                float x = glm::dot(D, B) / R;
                if (std::abs(x) > 1.0f) continue;

                // Longitudinal end taper
                float w_u = 1.0f;
                if (!pc.isClosed) {
                    float sinVal = std::sin(static_cast<float>(M_PI) * std::clamp(u, 0.0f, 1.0f));
                    sinVal = std::max(0.0f, sinVal);
                    w_u = (pc.endTaperFalloff <= 0.001f) ? 1.0f : std::pow(sinVal, pc.endTaperFalloff);
                }

                // Transverse profile height h(x)
                float h = 0.0f;
                float absX = std::abs(x);
                float oneMinusX2 = std::max(0.0f, 1.0f - x * x);
                float oneMinusAbsX = std::max(0.0f, 1.0f - absX);

                switch (pc.foldType) {
                    case FoldType::Ridge:
                        h = (pc.sharpness <= 0.001f) ? 1.0f : std::pow(oneMinusX2, pc.sharpness);
                        break;
                    case FoldType::Valley:
                        h = -((pc.sharpness <= 0.001f) ? 1.0f : std::pow(oneMinusX2, pc.sharpness));
                        break;
                    case FoldType::Overlap:
                        h = std::sin(static_cast<float>(M_PI) * x) * oneMinusAbsX;
                        break;
                    case FoldType::DoubleFold:
                        h = -std::cos(2.0f * static_cast<float>(M_PI) * x) * oneMinusAbsX;
                        break;
                }

                if (pc.inverted) {
                    h = -h;
                }

                // Normal offset displacement
                glm::vec3 deltaN = N * (pc.foldDepth * h * w_u * mask);

                // Pinch displacement towards center along binormal B
                float signX = (x > 1e-5f) ? 1.0f : ((x < -1e-5f) ? -1.0f : 0.0f);
                float pinchDisp = -signX * absX * (oneMinusAbsX * oneMinusAbsX) * pc.pinchStrength * R * w_u * mask;
                glm::vec3 deltaB = B * pinchDisp;

                deltaTotal += deltaN + deltaB;
                hasDeform = true;
            }

            if (hasDeform) {
                glm::vec3 Vnew = Vbase + deltaTotal;
                vertsPtr[vid3]     = Vnew.x;
                vertsPtr[vid3 + 1] = Vnew.y;
                vertsPtr[vid3 + 2] = Vnew.z;
                localAffected.push_back(vid);
            }
        }

        #pragma omp critical
        {
            affected.insert(affected.end(), localAffected.begin(), localAffected.end());
        }
    }

    m_affectedVerts = std::move(affected);
}

void CurveFoldsTool::updateLivePreview(Scene& scene, bool force) {
    if (!livePreviewEnabled && !force) return;

    ensureTargetMeshInitialized(scene);
    if (!m_targetMesh || m_baseVerts.empty() || m_targetMesh->verts.size() != m_baseVerts.size()) return;

    // 1. Reset previously affected vertices back to pristine base
    if (!m_affectedVerts.empty()) {
        #pragma omp parallel for schedule(static)
        for (int i = 0; i < static_cast<int>(m_affectedVerts.size()); ++i) {
            uint32_t vid = m_affectedVerts[i];
            uint32_t vid3 = vid * 3;
            m_targetMesh->verts[vid3]     = m_baseVerts[vid3];
            m_targetMesh->verts[vid3 + 1] = m_baseVerts[vid3 + 1];
            m_targetMesh->verts[vid3 + 2] = m_baseVerts[vid3 + 2];
        }
        m_affectedVerts.clear();
    } else {
        m_targetMesh->verts = m_baseVerts;
    }

    // 2. If no valid curves, restore and mark dirty
    bool hasAnyValidCurve = false;
    for (const auto& c : m_curves) {
        if (c.spline.getPointCount() >= 2) {
            hasAnyValidCurve = true;
            break;
        }
    }

    if (!hasAnyValidCurve) {
        m_targetMesh->isDirty = true;
        m_targetMesh->isVertexDirty = true;
        m_targetMesh->dirtyVertMin = 0;
        m_targetMesh->dirtyVertMax = m_targetMesh->nbVerts > 0 ? (m_targetMesh->nbVerts - 1) : 0;
        return;
    }

    // 3. Compute deformation on target mesh
    applyDeformationInternal(m_targetMesh);

    // 4. Update normals and GPU vertex buffers
    computeNormalsDirect(
        m_targetMesh->verts.data(), m_targetMesh->nbVerts,
        m_targetMesh->faces.data(), m_targetMesh->nbFaces,
        m_targetMesh->faceNormals.data(), m_targetMesh->normals.data()
    );

    m_targetMesh->isDirty = true;
    m_targetMesh->isVertexDirty = true;
    m_targetMesh->dirtyVertMin = 0;
    m_targetMesh->dirtyVertMax = m_targetMesh->nbVerts > 0 ? (m_targetMesh->nbVerts - 1) : 0;
}

bool CurveFoldsTool::handleMouseDown(
    Scene& scene, const Camera& camera, float mouseX, float mouseY,
    bool isCtrl, bool isAlt, bool isShift
) {
    (void)isShift;
    if (isMouseOverAnyImGuiWindow()) return false;

    ensureTargetMeshInitialized(scene);
    if (!m_targetMesh) return false;

    // 1. Check Radius Handle hit
    int rCurve = -1, rPoint = -1;
    if (hitTestCurveRadiusHandle(camera, mouseX, mouseY, rCurve, rPoint) != -1) {
        m_activeCurveIdx = rCurve;
        m_selectedPointIdx = rPoint;
        m_isDraggingRadius = true;
        m_dragStartMouse = glm::vec2(mouseX, mouseY);
        m_dragStartRadius = m_curves[rCurve].spline.getPoint(rPoint).radius;
        return true;
    }

    // 2. Check Control Point hit
    int hitCurve = -1, hitPoint = -1;
    if (hitTestAllCurves(camera, mouseX, mouseY, hitCurve, hitPoint) != -1) {
        m_activeCurveIdx = hitCurve;
        m_selectedPointIdx = hitPoint;

        if (isAlt) {
            // Alt + Click = delete control point
            auto& spline = m_curves[hitCurve].spline;
            spline.removePoint(hitPoint);
            if (spline.getPointCount() < 2) {
                deleteActiveCurve();
            } else {
                m_selectedPointIdx = std::clamp(hitPoint, 0, static_cast<int>(spline.getPointCount()) - 1);
            }
            updateLivePreview(scene, true);
            GuiManager::showToast("Point deleted", 1.0f);
            return true;
        }

        m_isDraggingPoint = true;
        m_dragStartMouse = glm::vec2(mouseX, mouseY);
        m_dragStartPointPos = m_curves[hitCurve].spline.getPoint(hitPoint).position;
        return true;
    }

    // 3. Check Segment hit to insert a new point
    int segCurve = -1;
    float segT = 0.5f;
    if (findClosestCurveSegment(camera, mouseX, mouseY, segCurve, segT) != -1) {
        m_activeCurveIdx = segCurve;
        auto& spline = m_curves[segCurve].spline;

        glm::vec3 hitPos, hitNorm;
        bool hasHit = raycastSurface(scene, camera, mouseX, mouseY, hitPos, hitNorm);
        if (!hasHit) {
            hitPos = spline.evaluatePosition(segT);
            hitNorm = glm::vec3(0.0f, 1.0f, 0.0f);
        }

        int numSegs = static_cast<int>(spline.getPointCount()) - 1;
        int insertSeg = std::clamp(static_cast<int>(segT * numSegs), 0, numSegs - 1);
        float segLocalT = segT * numSegs - insertSeg;
        float insertR = glm::mix(spline.getPoint(insertSeg).radius, spline.getPoint(insertSeg + 1).radius, segLocalT);

        spline.insertPoint(insertSeg + 1, hitPos, insertR, hitNorm, hasHit, isCtrl);
        m_selectedPointIdx = insertSeg + 1;
        m_isDraggingPoint = true;
        m_dragStartMouse = glm::vec2(mouseX, mouseY);
        m_dragStartPointPos = hitPos;

        updateLivePreview(scene, true);
        GuiManager::showToast("Added control point", 1.0f);
        return true;
    }

    // 4. Point-by-point mode: append or create point
    if (creationMode == CreationMode::ClickPoints) {
        glm::vec3 hitPos, hitNorm;
        bool hasHit = raycastSurface(scene, camera, mouseX, mouseY, hitPos, hitNorm);
        if (!hasHit) return false;

        if (m_activeCurveIdx < 0 || m_activeCurveIdx >= static_cast<int>(m_curves.size())) {
            FoldCurve fc;
            fc.spline.addPoint(hitPos, baseRadius, hitNorm, true, isCtrl);
            m_curves.push_back(std::move(fc));
            m_activeCurveIdx = static_cast<int>(m_curves.size()) - 1;
            m_selectedPointIdx = 0;
        } else {
            m_curves[m_activeCurveIdx].spline.addPoint(hitPos, baseRadius, hitNorm, true, isCtrl);
            m_selectedPointIdx = static_cast<int>(m_curves[m_activeCurveIdx].spline.getPointCount()) - 1;
        }

        updateLivePreview(scene, true);
        return true;
    }

    // 5. Freehand stroke mode: begin new stroke on mesh surface
    if (creationMode == CreationMode::FreehandStroke) {
        if (isAlt) return false;

        glm::vec3 hitPos, hitNorm;
        bool hasHit = raycastSurface(scene, camera, mouseX, mouseY, hitPos, hitNorm);
        if (!hasHit) return false;

        m_isDrawingStroke = true;
        m_dragStartMouse = glm::vec2(mouseX, mouseY);
        m_rawStroke.clear();
        m_rawStroke.emplace_back(hitPos, baseRadius, hitNorm, true);
        return true;
    }

    return false;
}

bool CurveFoldsTool::handleMouseMove(Scene& scene, const Camera& camera, float mouseX, float mouseY) {
    if (!isInteracting() && isMouseOverAnyImGuiWindow()) {
        m_hoveredCurveIdx = -1;
        m_hoveredPointIdx = -1;
        m_hoveredRadiusHandleCurve = -1;
        m_hoveredRadiusHandlePoint = -1;
        m_hoveredSegmentCurve = -1;
        return false;
    }

    if (m_isDrawingStroke) {
        glm::vec3 hitPos, hitNorm;
        bool hasHit = raycastSurface(scene, camera, mouseX, mouseY, hitPos, hitNorm);
        if (hasHit) {
            float worldDist = m_rawStroke.empty() ? 999.0f : glm::distance(m_rawStroke.back().position, hitPos);
            if (worldDist > baseRadius * 0.15f || m_rawStroke.size() < 2) {
                m_rawStroke.emplace_back(hitPos, baseRadius, hitNorm, true);
            }
        }
        return true;
    }

    if (m_isDraggingPoint) {
        if (m_activeCurveIdx >= 0 && m_activeCurveIdx < static_cast<int>(m_curves.size()) && m_selectedPointIdx >= 0) {
            auto& pt = m_curves[m_activeCurveIdx].spline.getPoint(m_selectedPointIdx);
            glm::vec3 hitPos, hitNorm;
            if (raycastSurface(scene, camera, mouseX, mouseY, hitPos, hitNorm)) {
                pt.position = hitPos;
                pt.normal = hitNorm;
                pt.hasSurfaceNormal = true;
            } else {
                glm::vec3 planeNorm = glm::normalize(camera.computePosition() - m_dragStartPointPos);
                pt.position = intersectRayPlane(camera, mouseX, mouseY, m_dragStartPointPos, planeNorm);
            }
            updateLivePreview(scene);
        }
        return true;
    }

    if (m_isDraggingRadius) {
        if (m_activeCurveIdx >= 0 && m_activeCurveIdx < static_cast<int>(m_curves.size()) && m_selectedPointIdx >= 0) {
            float delta = (mouseX - m_dragStartMouse.x) + (m_dragStartMouse.y - mouseY);
            float newR = std::max(0.5f, m_dragStartRadius + delta * 0.2f);
            m_curves[m_activeCurveIdx].spline.getPoint(m_selectedPointIdx).radius = newR;
            baseRadius = newR;
            updateLivePreview(scene);
        }
        return true;
    }

    // Hover testing for visual highlight
    hitTestCurveRadiusHandle(camera, mouseX, mouseY, m_hoveredRadiusHandleCurve, m_hoveredRadiusHandlePoint);
    hitTestAllCurves(camera, mouseX, mouseY, m_hoveredCurveIdx, m_hoveredPointIdx);
    findClosestCurveSegment(camera, mouseX, mouseY, m_hoveredSegmentCurve, m_hoveredSegmentT);

    return false;
}

bool CurveFoldsTool::handleMouseUp(Scene& scene) {
    if (m_isDrawingStroke) {
        m_isDrawingStroke = false;
        if (m_rawStroke.size() >= 2) {
            std::vector<CurvePoint> simplified = CurveSpline::simplifyStrokeAdaptive(
                m_rawStroke, strokeSmoothing, curveFitTolerance, baseRadius
            );

            if (simplified.size() >= 2) {
                FoldCurve fc;
                for (const auto& cp : simplified) {
                    fc.spline.addPoint(cp.position, cp.radius, cp.normal, cp.hasSurfaceNormal, cp.isCorner);
                }
                m_curves.push_back(std::move(fc));
                m_activeCurveIdx = static_cast<int>(m_curves.size()) - 1;
                m_selectedPointIdx = static_cast<int>(m_curves.back().spline.getPointCount()) - 1;
                updateLivePreview(scene, true);
                GuiManager::showToast("Created fold curve", 1.2f);
            }
        }
        m_rawStroke.clear();
        return true;
    }

    if (m_isDraggingPoint) {
        m_isDraggingPoint = false;
        updateLivePreview(scene, true);
        return true;
    }

    if (m_isDraggingRadius) {
        m_isDraggingRadius = false;
        updateLivePreview(scene, true);
        return true;
    }

    return false;
}

void CurveFoldsTool::renderFloatingHUD(Scene& scene, float viewportW, float viewportH) {
    (void)viewportH;
    ImGui::SetNextWindowPos(ImVec2(viewportW * 0.5f, 42.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGuiWindowFlags hudFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                ImGuiWindowFlags_NoNav;

    if (ImGui::Begin("##CurveFoldsFloatingHud", nullptr, hudFlags)) {
        ImGui::TextColored(ImVec4(0.2f, 0.95f, 0.85f, 1.0f), "%s Curve Folds", ICON_LC_SPLINE);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("Curves: %d", static_cast<int>(m_curves.size()));

        if (m_activeCurveIdx >= 0 && m_activeCurveIdx < static_cast<int>(m_curves.size())) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "#%d", m_activeCurveIdx + 1);
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.65f, 0.34f, 1.0f));
        if (ImGui::Button("Apply (Enter)", ImVec2(95, 24))) {
            applyFolds(scene);
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.22f, 0.22f, 1.0f));
        if (ImGui::Button("Cancel (Esc)", ImVec2(90, 24))) {
            cancelFolds(scene);
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();

        if (ImGui::Button("New Curve", ImVec2(80, 24))) {
            startNewCurve();
        }

        if (m_activeCurveIdx >= 0 && m_activeCurveIdx < static_cast<int>(m_curves.size())) {
            ImGui::SameLine();
            if (ImGui::Button("Delete Curve", ImVec2(95, 24))) {
                deleteActiveCurve();
                updateLivePreview(scene, true);
            }
        }
    }
    ImGui::End();

    // Hotkey support for Enter, Esc, Delete, Backspace
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
            applyFolds(scene);
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            cancelFolds(scene);
        } else if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
            deleteActiveCurve();
            updateLivePreview(scene, true);
        }
    }
}

void CurveFoldsTool::renderOverlay(
    Scene& scene, const Camera& camera, float viewportW, float viewportH
) {
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    if (!drawList) return;

    if (!isInteracting() && isMouseOverAnyImGuiWindow()) {
        m_hoveredCurveIdx = -1;
        m_hoveredPointIdx = -1;
        m_hoveredRadiusHandleCurve = -1;
        m_hoveredRadiusHandlePoint = -1;
        m_hoveredSegmentCurve = -1;
    }

    // 1. Draw raw freehand stroke in progress
    if (m_isDrawingStroke && m_rawStroke.size() >= 2) {
        for (size_t i = 1; i < m_rawStroke.size(); ++i) {
            glm::vec3 s0 = camera.project(m_rawStroke[i - 1].position);
            glm::vec3 s1 = camera.project(m_rawStroke[i].position);
            if (s0.z < 0.0f || s1.z < 0.0f) continue;
            drawList->AddLine(
                ImVec2(s0.x, s0.y), ImVec2(s1.x, s1.y),
                IM_COL32(0, 230, 210, 240), 2.5f
            );
        }
        renderFloatingHUD(scene, viewportW, viewportH);
        return;
    }

    // 2. Draw all existing curves
    for (int cIdx = 0; cIdx < static_cast<int>(m_curves.size()); ++cIdx) {
        const auto& curve = m_curves[cIdx];
        const auto& spline = curve.spline;
        if (spline.getPointCount() < 2) continue;

        bool isActiveCurve = (cIdx == m_activeCurveIdx);
        int numSamples = std::clamp(static_cast<int>(spline.getPointCount() * 24), 32, 256);
        std::vector<EvaluatedFrame> frames = spline.evaluateFrames(numSamples);
        if (frames.size() < 2) continue;

        // Spline Centerline
        ImU32 lineCol = isActiveCurve ? IM_COL32(0, 230, 220, 240) : IM_COL32(90, 150, 190, 180);
        float lineThick = isActiveCurve ? 2.5f : 1.5f;

        for (size_t i = 1; i < frames.size(); ++i) {
            glm::vec3 s0 = camera.project(frames[i - 1].position);
            glm::vec3 s1 = camera.project(frames[i].position);
            if (s0.z < 0.0f || s1.z < 0.0f) continue;
            drawList->AddLine(ImVec2(s0.x, s0.y), ImVec2(s1.x, s1.y), lineCol, lineThick);
        }

        // Width / Influence Boundaries (Left and Right along binormal B)
        ImU32 boundCol = isActiveCurve ? IM_COL32(0, 190, 220, 90) : IM_COL32(70, 120, 160, 60);
        for (size_t i = 1; i < frames.size(); ++i) {
            const auto& f0 = frames[i - 1];
            const auto& f1 = frames[i];

            glm::vec3 l0 = f0.position - f0.binormal * f0.radius;
            glm::vec3 l1 = f1.position - f1.binormal * f1.radius;
            glm::vec3 r0 = f0.position + f0.binormal * f0.radius;
            glm::vec3 r1 = f1.position + f1.binormal * f1.radius;

            glm::vec3 sl0 = camera.project(l0);
            glm::vec3 sl1 = camera.project(l1);
            glm::vec3 sr0 = camera.project(r0);
            glm::vec3 sr1 = camera.project(r1);

            if (sl0.z >= 0.0f && sl1.z >= 0.0f) {
                drawList->AddLine(ImVec2(sl0.x, sl0.y), ImVec2(sl1.x, sl1.y), boundCol, 1.0f);
            }
            if (sr0.z >= 0.0f && sr1.z >= 0.0f) {
                drawList->AddLine(ImVec2(sr0.x, sr0.y), ImVec2(sr1.x, sr1.y), boundCol, 1.0f);
            }
        }

        // Control Points
        for (int pIdx = 0; pIdx < static_cast<int>(spline.getPointCount()); ++pIdx) {
            const auto& pt = spline.getPoint(pIdx);
            glm::vec3 sPos = camera.project(pt.position);
            if (sPos.z < 0.0f || sPos.z > 1.0f) continue;

            bool isSelected = (isActiveCurve && pIdx == m_selectedPointIdx);
            bool isHovered = (cIdx == m_hoveredCurveIdx && pIdx == m_hoveredPointIdx);

            ImU32 ptCol = isSelected ? IM_COL32(255, 230, 40, 255) :
                          (isHovered ? IM_COL32(255, 255, 255, 255) :
                          (isActiveCurve ? IM_COL32(0, 210, 240, 220) : IM_COL32(110, 160, 200, 180)));
            float ptRadius = isSelected ? 6.5f : (isHovered ? 5.5f : 4.0f);

            drawList->AddCircleFilled(ImVec2(sPos.x, sPos.y), ptRadius, ptCol);
            drawList->AddCircle(ImVec2(sPos.x, sPos.y), ptRadius + 1.0f, IM_COL32(10, 15, 20, 220), 0, 1.5f);

            // Radius Handle for selected or active point
            if (isSelected || (isActiveCurve && pIdx == 0 && m_selectedPointIdx == -1)) {
                glm::vec3 cWorld, hWorld, sideDir;
                glm::vec2 cScreen, hScreen;
                if (getPointHandleInfo(camera, spline, pIdx, cWorld, hWorld, cScreen, hScreen, sideDir)) {
                    bool isHandleHovered = (cIdx == m_hoveredRadiusHandleCurve && pIdx == m_hoveredRadiusHandlePoint);
                    ImU32 hCol = isHandleHovered ? IM_COL32(255, 210, 0, 255) : IM_COL32(0, 230, 220, 240);

                    drawList->AddLine(ImVec2(cScreen.x, cScreen.y), ImVec2(hScreen.x, hScreen.y), hCol, 1.5f);
                    drawList->AddRectFilled(
                        ImVec2(hScreen.x - 4.5f, hScreen.y - 4.5f),
                        ImVec2(hScreen.x + 4.5f, hScreen.y + 4.5f),
                        hCol, 2.0f
                    );
                    drawList->AddRect(
                        ImVec2(hScreen.x - 4.5f, hScreen.y - 4.5f),
                        ImVec2(hScreen.x + 4.5f, hScreen.y + 4.5f),
                        IM_COL32(15, 20, 25, 220), 2.0f, 0, 1.2f
                    );
                }
            }
        }
    }

    renderFloatingHUD(scene, viewportW, viewportH);
}
