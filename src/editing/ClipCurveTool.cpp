#define GLM_ENABLE_EXPERIMENTAL
#include "editing/ClipCurveTool.h"
#include "mesh/Mesh.h"
#include "mesh/NormalCalc.h"
#include "scene/Camera.h"
#include "sculpt/SculptEngine.h"
#include "editing/SculptManager.h"
#include "common/Logger.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/norm.hpp>
#include <omp.h>

struct PolySegment2D {
    glm::vec2 pA;
    glm::vec2 pB;
    glm::vec2 dir;
    glm::vec2 norm2D; // Normal pointing toward the 'keep' side
    float len;
};

static bool projectToPolylinePlane(
    const glm::vec2& p,
    const std::vector<PolySegment2D>& segments,
    glm::vec2& outTargetScreen,
    float& outSignedDist,
    size_t& outBestSegIdx
) {
    if (segments.empty()) return false;

    float minSqDist = 1e18f;
    outBestSegIdx = 0;

    for (size_t s = 0; s < segments.size(); ++s) {
        const auto& seg = segments[s];
        glm::vec2 rel = p - seg.pA;
        float t = glm::dot(rel, seg.dir);
        float tClamped = std::clamp(t, 0.0f, seg.len);
        glm::vec2 closestOnSeg = seg.pA + tClamped * seg.dir;
        float sqDist = glm::distance2(p, closestOnSeg);

        if (sqDist < minSqDist) {
            minSqDist = sqDist;
            outBestSegIdx = s;
        }
    }

    const auto& bestSeg = segments[outBestSegIdx];
    glm::vec2 rel = p - bestSeg.pA;
    outSignedDist = glm::dot(rel, bestSeg.norm2D);

    // Orthogonal projection onto the cut plane line (preserving t along segment direction)
    outTargetScreen = p - outSignedDist * bestSeg.norm2D;
    return true;
}

static void constrainedLaplacianRelax(
    Mesh* mesh,
    const std::vector<int>& vertAffected,
    const std::vector<glm::vec3>& vertSymScale,
    SymmetryMode symMode,
    const std::vector<PolySegment2D>& segments,
    const Camera& camera,
    const glm::mat4& localToWorld,
    const glm::mat4& worldToLocal,
    int iterations
) {
    if (mesh->vrvStartCount.empty() || mesh->vertRingVert.empty()) return;

    int nbVerts = mesh->nbVerts;
    std::vector<float> smoothPos(nbVerts * 3);

    for (int iter = 0; iter < iterations; ++iter) {
        #pragma omp parallel for schedule(static)
        for (int i = 0; i < nbVerts; ++i) {
            int i3 = i * 3;
            smoothPos[i3]     = mesh->verts[i3];
            smoothPos[i3 + 1] = mesh->verts[i3 + 1];
            smoothPos[i3 + 2] = mesh->verts[i3 + 2];

            if (!vertAffected[i]) continue;

            uint32_t start = mesh->vrvStartCount[i * 2];
            uint32_t count = mesh->vrvStartCount[i * 2 + 1];
            if (count < 2) continue;

            float ax = 0.0f, ay = 0.0f, az = 0.0f;
            for (uint32_t j = start; j < start + count; ++j) {
                uint32_t nid = mesh->vertRingVert[j];
                ax += mesh->verts[nid * 3];
                ay += mesh->verts[nid * 3 + 1];
                az += mesh->verts[nid * 3 + 2];
            }
            smoothPos[i3]     = ax / count;
            smoothPos[i3 + 1] = ay / count;
            smoothPos[i3 + 2] = az / count;
        }

        const float blendWeight = 0.5f;

        #pragma omp parallel for schedule(static)
        for (int i = 0; i < nbVerts; ++i) {
            if (!vertAffected[i]) continue;

            int i3 = i * 3;
            float ox = mesh->verts[i3];
            float oy = mesh->verts[i3 + 1];
            float oz = mesh->verts[i3 + 2];

            // Blend toward neighbor average
            float bx = ox + (smoothPos[i3]     - ox) * blendWeight;
            float by = oy + (smoothPos[i3 + 1] - oy) * blendWeight;
            float bz = oz + (smoothPos[i3 + 2] - oz) * blendWeight;

            glm::vec3 bLocal(bx, by, bz);
            const glm::vec3& sScale = vertSymScale[i];
            bool isSymPass = (sScale != glm::vec3(1.0f));

            glm::vec3 bEvalLocal = isSymPass ? reflectPointSymmetry(bLocal, sScale, mesh, symMode) : bLocal;
            glm::vec3 bWorld = glm::vec3(localToWorld * glm::vec4(bEvalLocal, 1.0f));
            glm::vec3 bProj = camera.project(bWorld);

            glm::vec2 targetScreen;
            float sDist;
            size_t segIdx;
            if (projectToPolylinePlane(glm::vec2(bProj.x, bProj.y), segments, targetScreen, sDist, segIdx)) {
                float safeZ = std::clamp(bProj.z, 0.0001f, 0.9999f);
                glm::vec3 wCut = camera.unproject(targetScreen.x, targetScreen.y, safeZ);
                glm::vec3 vCutLocal = glm::vec3(worldToLocal * glm::vec4(wCut, 1.0f));
                glm::vec3 finalLocal = isSymPass ? reflectPointSymmetry(vCutLocal, sScale, mesh, symMode) : vCutLocal;

                mesh->verts[i3]     = finalLocal.x;
                mesh->verts[i3 + 1] = finalLocal.y;
                mesh->verts[i3 + 2] = finalLocal.z;
            }
        }
    }
}

bool ClipCurveTool::execute(
    Mesh* mesh,
    const Camera& camera,
    const std::vector<glm::vec2>& curvePoints,
    bool altMode,
    bool useSym,
    SymmetryMode symMode,
    const std::vector<glm::vec3>& symScales
) {
    sculpt_log("[ClipCurveTool] execute enter. mesh=%p, nbVerts=%d, pointsCount=%zu, altMode=%d, useSym=%d\n",
               (void*)mesh, mesh ? mesh->nbVerts : 0, curvePoints.size(), altMode ? 1 : 0, useSym ? 1 : 0);

    if (!mesh || mesh->nbVerts == 0 || curvePoints.size() < 2) {
        sculpt_log("[ClipCurveTool] Early return: invalid mesh or curvePoints < 2\n");
        return false;
    }

    // Filter points to remove micro-jitter
    std::vector<glm::vec2> polyline;
    polyline.reserve(curvePoints.size());
    for (const auto& pt : curvePoints) {
        if (polyline.empty()) {
            polyline.push_back(pt);
        } else {
            if (glm::distance2(pt, polyline.back()) >= 4.0f) {
                polyline.push_back(pt);
            }
        }
    }
    sculpt_log("[ClipCurveTool] Polyline filtered from %zu to %zu points\n", curvePoints.size(), polyline.size());

    if (polyline.size() < 2) return false;

    // Build 2D polyline segments
    std::vector<PolySegment2D> segments;
    segments.reserve(polyline.size() - 1);
    for (size_t i = 0; i < polyline.size() - 1; ++i) {
        glm::vec2 pA = polyline[i];
        glm::vec2 pB = polyline[i + 1];
        glm::vec2 d = pB - pA;
        float len = glm::length(d);
        if (len < 1e-5f) continue;

        PolySegment2D seg;
        seg.pA = pA;
        seg.pB = pB;
        seg.dir = d / len;
        seg.len = len;

        // Normal pointing toward 'keep' side (left side of curve if altMode is false)
        // In screen space (Y down): (-dir.y, dir.x) points left/up of segment direction
        seg.norm2D = altMode ? glm::vec2(seg.dir.y, -seg.dir.x) : glm::vec2(-seg.dir.y, seg.dir.x);
        segments.push_back(seg);
    }

    if (segments.empty()) return false;

    glm::mat4 localToWorld = mesh->matrix;
    glm::mat4 worldToLocal = glm::inverse(localToWorld);

    int nbVerts = mesh->nbVerts;
    std::vector<int> vertAffected(nbVerts, 0);
    std::vector<glm::vec3> vertSymScale(nbVerts, glm::vec3(1.0f));

    // Symmetry passes
    std::vector<glm::vec3> passes = { glm::vec3(1.0f) };
    if (useSym && !symScales.empty()) {
        for (const auto& s : symScales) {
            passes.push_back(s);
        }
    }

    int affectedCount = 0;
    float globalMinDist = 1e18f;
    float globalMaxDist = -1e18f;

    for (size_t passIdx = 0; passIdx < passes.size(); ++passIdx) {
        const auto& sScale = passes[passIdx];
        bool isSymPass = (sScale != glm::vec3(1.0f));

        #pragma omp parallel for schedule(dynamic, 1024) reduction(min:globalMinDist) reduction(max:globalMaxDist)
        for (int i = 0; i < nbVerts; ++i) {
            if (!mesh->vertVisible[i]) continue;

            glm::vec3 vOrigLocal(mesh->verts[i * 3], mesh->verts[i * 3 + 1], mesh->verts[i * 3 + 2]);
            glm::vec3 vLocal = isSymPass ? reflectPointSymmetry(vOrigLocal, sScale, mesh, symMode) : vOrigLocal;

            glm::vec3 vWorld = glm::vec3(localToWorld * glm::vec4(vLocal, 1.0f));
            glm::vec3 vProj = camera.project(vWorld);
            glm::vec2 vScreen(vProj.x, vProj.y);

            glm::vec2 targetScreen;
            float signedDist;
            size_t segIdx;
            if (projectToPolylinePlane(vScreen, segments, targetScreen, signedDist, segIdx)) {
                if (signedDist < globalMinDist) globalMinDist = signedDist;
                if (signedDist > globalMaxDist) globalMaxDist = signedDist;

                if (signedDist < 0.0f) { // Clipped side
                    float safeZ = std::clamp(vProj.z, 0.0001f, 0.9999f);
                    glm::vec3 wCut = camera.unproject(targetScreen.x, targetScreen.y, safeZ);
                    glm::vec3 clippedLocal = glm::vec3(worldToLocal * glm::vec4(wCut, 1.0f));
                    glm::vec3 finalLocal = isSymPass ? reflectPointSymmetry(clippedLocal, sScale, mesh, symMode) : clippedLocal;

                    mesh->verts[i * 3]     = finalLocal.x;
                    mesh->verts[i * 3 + 1] = finalLocal.y;
                    mesh->verts[i * 3 + 2] = finalLocal.z;

                    if (!vertAffected[i]) {
                        vertAffected[i] = 1;
                        vertSymScale[i] = sScale;
                    }
                }
            }
        }
    }

    for (int i = 0; i < nbVerts; ++i) {
        if (vertAffected[i]) affectedCount++;
    }

    sculpt_log("[ClipCurveTool] Continuous polyline scan: min2DDist=%.2f, max2DDist=%.2f, affectedVerts=%d / %d\n",
               globalMinDist, globalMaxDist, affectedCount, nbVerts);

    if (affectedCount == 0) {
        sculpt_log("[ClipCurveTool] No vertices were affected by clip. Returning false.\n");
        return false;
    }

    // Collect affected vertices list
    std::vector<uint32_t> affectedVerts;
    affectedVerts.reserve(affectedCount);
    for (int i = 0; i < nbVerts; ++i) {
        if (vertAffected[i]) {
            affectedVerts.push_back((uint32_t)i);
        }
    }

    // Post-clip continuous relaxation pass
    constexpr int RELAX_ITERATIONS = 3;
    sculpt_log("[ClipCurveTool] Running continuous Laplacian relaxation (%d iterations)...\n", RELAX_ITERATIONS);
    constrainedLaplacianRelax(mesh, vertAffected, vertSymScale, symMode, segments, camera, localToWorld, worldToLocal, RELAX_ITERATIONS);

    // Recalculate face and vertex normals & update octree
    std::vector<uint32_t> iFaces(mesh->nbFaces);
    std::vector<uint32_t> tagFlags(mesh->nbFaces, 0);
    uint32_t tagEpoch = 1;

    uint32_t numIFaces = getFacesFromVerticesFast(
        affectedVerts.data(),
        (uint32_t)affectedVerts.size(),
        mesh->vrfStartCount.data(),
        mesh->vertRingFace.data(),
        iFaces.data(),
        tagFlags.data(),
        &tagEpoch,
        mesh->nbFaces
    );

    sculpt_log("[ClipCurveTool] Normals & Octree update. Affected faces: %u / %d\n", numIFaces, mesh->nbFaces);

    if (numIFaces > 0) {
        updateFaceNormalsAndBoxes(
            mesh->verts.data(), mesh->nbVerts,
            mesh->faces.data(), mesh->nbFaces,
            iFaces.data(), numIFaces,
            mesh->faceNormals.data(),
            mesh->faceBoxes.data(),
            mesh->faceCenters.data()
        );

        updateVertexNormals(
            affectedVerts.data(), (int)affectedVerts.size(), mesh->nbVerts,
            mesh->vrfStartCount.data(),
            mesh->vertRingFace.data(),
            mesh->faceNormals.data(),
            mesh->normals.data()
        );

        mesh->octree.update(
            mesh->verts.data(), mesh->nbVerts,
            mesh->faces.data(), mesh->nbFaces,
            mesh->faceBoxes.data(),
            iFaces.data(), numIFaces
        );
    }

    uint32_t minV = affectedVerts[0];
    uint32_t maxV = affectedVerts[0];
    for (size_t i = 1; i < affectedVerts.size(); ++i) {
        uint32_t v = affectedVerts[i];
        if (v < minV) minV = v;
        if (v > maxV) maxV = v;
    }

    if (mesh->isVertexDirty) {
        mesh->dirtyVertMin = std::min(mesh->dirtyVertMin, minV);
        mesh->dirtyVertMax = std::max(mesh->dirtyVertMax, maxV);
    } else {
        mesh->dirtyVertMin = minV;
        mesh->dirtyVertMax = maxV;
    }

    mesh->isDirty = true;
    mesh->isVertexDirty = true;
    mesh->invalidateLocalRadius();

    sculpt_log("[ClipCurveTool] ClipCurve Tool successfully modified mesh and updated dirty flags.\n");

    return true;
}
