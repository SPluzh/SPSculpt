#define GLM_ENABLE_EXPERIMENTAL
#include "editing/ClipCurveTool.h"
#include "mesh/Mesh.h"
#include "mesh/NormalCalc.h"
#include "scene/Camera.h"
#include "sculpt/SculptEngine.h"
#include "editing/SculptManager.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/norm.hpp>
#include <omp.h>

struct SegmentData {
    glm::vec2 pA;
    glm::vec2 pB;
    glm::vec2 dir;
    float len;
    glm::vec2 normal;
};

static void buildSegments(
    const std::vector<glm::vec2>& polyline,
    bool altMode,
    std::vector<SegmentData>& segments,
    bool& isStraightLine
) {
    segments.clear();
    if (polyline.size() < 2) return;

    isStraightLine = (polyline.size() == 2);
    if (!isStraightLine) {
        glm::vec2 pA = polyline.front();
        glm::vec2 pB = polyline.back();
        glm::vec2 dirAB = pB - pA;
        float lenAB = glm::length(dirAB);
        if (lenAB > 1e-4f) {
            glm::vec2 normAB(-dirAB.y / lenAB, dirAB.x / lenAB);
            bool maxDevExceeded = false;
            for (size_t i = 1; i < polyline.size() - 1; ++i) {
                float dist = std::abs(glm::dot(polyline[i] - pA, normAB));
                if (dist > 3.0f) {
                    maxDevExceeded = true;
                    break;
                }
            }
            if (!maxDevExceeded) {
                isStraightLine = true;
            }
        }
    }

    if (isStraightLine) {
        SegmentData seg;
        seg.pA = polyline.front();
        seg.pB = polyline.back();
        glm::vec2 d = seg.pB - seg.pA;
        seg.len = glm::length(d);
        if (seg.len < 1e-5f) return;
        seg.dir = d / seg.len;
        seg.normal = glm::vec2(-seg.dir.y, seg.dir.x);
        if (altMode) seg.normal = -seg.normal;
        segments.push_back(seg);
    } else {
        segments.reserve(polyline.size() - 1);
        for (size_t i = 0; i < polyline.size() - 1; ++i) {
            SegmentData seg;
            seg.pA = polyline[i];
            seg.pB = polyline[i + 1];
            glm::vec2 d = seg.pB - seg.pA;
            seg.len = glm::length(d);
            if (seg.len < 1e-5f) continue;
            seg.dir = d / seg.len;
            seg.normal = glm::vec2(-seg.dir.y, seg.dir.x);
            if (altMode) seg.normal = -seg.normal;
            segments.push_back(seg);
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
    if (!mesh || mesh->nbVerts == 0 || curvePoints.size() < 2) return false;

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
    if (polyline.size() < 2) return false;

    bool isStraightLine = false;
    std::vector<SegmentData> segments;
    buildSegments(polyline, altMode, segments, isStraightLine);
    if (segments.empty()) return false;

    int nbVerts = mesh->nbVerts;
    std::vector<uint8_t> isVertAffected(nbVerts, 0);
    glm::mat4 localToWorld = mesh->matrix;
    glm::mat4 worldToLocal = glm::inverse(localToWorld);

    // Build symmetry passes: primary pass (identity) + symmetry passes
    std::vector<glm::vec3> passes = { glm::vec3(1.0f) };
    if (useSym && !symScales.empty()) {
        for (const auto& s : symScales) {
            passes.push_back(s);
        }
    }

    for (const auto& sScale : passes) {
        bool isSymPass = (sScale != glm::vec3(1.0f));

        #pragma omp parallel for schedule(dynamic, 1024)
        for (int i = 0; i < nbVerts; ++i) {
            if (!mesh->vertVisible[i]) continue;

            glm::vec3 vOrigLocal(mesh->verts[i * 3], mesh->verts[i * 3 + 1], mesh->verts[i * 3 + 2]);
            glm::vec3 vLocal = isSymPass ? reflectPointSymmetry(vOrigLocal, sScale, mesh, symMode) : vOrigLocal;

            glm::vec3 vWorld = glm::vec3(localToWorld * glm::vec4(vLocal, 1.0f));
            glm::vec3 vProj = camera.project(vWorld); // (x_screen, y_screen, z_depth)
            glm::vec2 vScreen(vProj.x, vProj.y);

            if (isStraightLine) {
                const auto& seg = segments[0];
                glm::vec2 rel = vScreen - seg.pA;
                float signedDist = glm::dot(rel, seg.normal);

                if (signedDist < 0.0f) { // Clipped side
                    float projT = glm::dot(rel, seg.dir);
                    glm::vec2 targetScreen = seg.pA + projT * seg.dir;
                    glm::vec3 clippedWorld = camera.unproject(targetScreen.x, targetScreen.y, vProj.z);
                    glm::vec3 clippedLocal = glm::vec3(worldToLocal * glm::vec4(clippedWorld, 1.0f));

                    glm::vec3 finalLocal = isSymPass ? reflectPointSymmetry(clippedLocal, sScale, mesh, symMode) : clippedLocal;

                    mesh->verts[i * 3]     = finalLocal.x;
                    mesh->verts[i * 3 + 1] = finalLocal.y;
                    mesh->verts[i * 3 + 2] = finalLocal.z;

                    isVertAffected[i] = 1;
                }
            } else {
                float minSqDist = 1e18f;
                int bestSegIdx = 0;
                glm::vec2 bestClosestPt(0.0f);

                for (size_t s = 0; s < segments.size(); ++s) {
                    const auto& seg = segments[s];
                    glm::vec2 rel = vScreen - seg.pA;
                    float t = glm::dot(rel, seg.dir) / seg.len;
                    float tClamped = std::clamp(t, 0.0f, 1.0f);
                    glm::vec2 closestPt = seg.pA + tClamped * seg.dir;
                    float sqDist = glm::distance2(vScreen, closestPt);

                    if (sqDist < minSqDist) {
                        minSqDist = sqDist;
                        bestSegIdx = (int)s;
                        bestClosestPt = closestPt;
                    }
                }

                const auto& seg = segments[bestSegIdx];
                glm::vec2 rel = vScreen - bestClosestPt;
                float signedDist = glm::dot(rel, seg.normal);
                if (signedDist == 0.0f) {
                    signedDist = glm::dot(vScreen - seg.pA, seg.normal);
                }

                if (signedDist < 0.0f) { // Clipped side
                    glm::vec3 clippedWorld = camera.unproject(bestClosestPt.x, bestClosestPt.y, vProj.z);
                    glm::vec3 clippedLocal = glm::vec3(worldToLocal * glm::vec4(clippedWorld, 1.0f));

                    glm::vec3 finalLocal = isSymPass ? reflectPointSymmetry(clippedLocal, sScale, mesh, symMode) : clippedLocal;

                    mesh->verts[i * 3]     = finalLocal.x;
                    mesh->verts[i * 3 + 1] = finalLocal.y;
                    mesh->verts[i * 3 + 2] = finalLocal.z;

                    isVertAffected[i] = 1;
                }
            }
        }
    }

    // Collect affected vertex list
    std::vector<uint32_t> affectedVerts;
    affectedVerts.reserve(nbVerts / 4);
    for (int i = 0; i < nbVerts; ++i) {
        if (isVertAffected[i]) {
            affectedVerts.push_back((uint32_t)i);
        }
    }

    if (affectedVerts.empty()) return false;

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

    return true;
}
