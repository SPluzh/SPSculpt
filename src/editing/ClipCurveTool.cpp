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
    glm::vec2 norm2D; // Normal pointing toward the 'keep' side in 2D
    float len;
    glm::vec2 miterNormStart; // Miter normal at start joint
    glm::vec2 miterNormEnd;   // Miter normal at end joint
};

static void douglasPeucker(
    const std::vector<glm::vec2>& pts,
    float epsilon,
    std::vector<glm::vec2>& result
) {
    if (pts.size() < 3) { result = pts; return; }

    float maxDist = 0.0f;
    size_t maxIdx = 0;
    glm::vec2 start = pts.front(), end = pts.back();
    glm::vec2 line = end - start;
    float lineLen = glm::length(line);

    for (size_t i = 1; i < pts.size() - 1; ++i) {
        float dist = (lineLen > 1e-5f)
            ? std::abs(glm::dot(glm::vec2(-line.y, line.x) / lineLen, pts[i] - start))
            : glm::distance(pts[i], start);
        if (dist > maxDist) { maxDist = dist; maxIdx = i; }
    }

    if (maxDist > epsilon) {
        std::vector<glm::vec2> left(pts.begin(), pts.begin() + maxIdx + 1);
        std::vector<glm::vec2> right(pts.begin() + maxIdx, pts.end());
        std::vector<glm::vec2> r1, r2;
        douglasPeucker(left, epsilon, r1);
        douglasPeucker(right, epsilon, r2);
        result = r1;
        result.insert(result.end(), r2.begin() + 1, r2.end());
    } else {
        result = { start, end };
    }
}

static std::vector<PolySegment2D> buildSegmentsWithMiters(
    const std::vector<glm::vec2>& pts,
    bool altMode
) {
    std::vector<PolySegment2D> segs;
    if (pts.size() < 2) return segs;

    int n = static_cast<int>(pts.size());
    std::vector<glm::vec2> normals(n - 1);
    std::vector<glm::vec2> dirs(n - 1);
    std::vector<float> lens(n - 1);

    for (int i = 0; i < n - 1; ++i) {
        glm::vec2 d = pts[i + 1] - pts[i];
        float len = glm::length(d);
        lens[i] = len;
        if (len > 1e-5f) {
            d /= len;
        } else {
            d = glm::vec2(1.0f, 0.0f);
        }
        dirs[i] = d;
        normals[i] = altMode ? glm::vec2(d.y, -d.x) : glm::vec2(-d.y, d.x);
    }

    segs.reserve(n - 1);
    for (int i = 0; i < n - 1; ++i) {
        if (lens[i] < 1e-5f) continue;

        PolySegment2D seg;
        seg.pA = pts[i];
        seg.pB = pts[i + 1];
        seg.dir = dirs[i];
        seg.len = lens[i];
        seg.norm2D = normals[i];

        // Miter normal at start
        if (i == 0) {
            seg.miterNormStart = normals[i];
        } else {
            glm::vec2 avg = normals[i - 1] + normals[i];
            float lenAvg = glm::length(avg);
            if (lenAvg > 1e-5f) {
                avg /= lenAvg;
                float cosA = glm::dot(avg, normals[i]);
                float clampedLen = std::max(cosA, 0.2f);
                seg.miterNormStart = avg / clampedLen;
            } else {
                seg.miterNormStart = normals[i];
            }
        }

        // Miter normal at end
        if (i == n - 2) {
            seg.miterNormEnd = normals[i];
        } else {
            glm::vec2 avg = normals[i] + normals[i + 1];
            float lenAvg = glm::length(avg);
            if (lenAvg > 1e-5f) {
                avg /= lenAvg;
                float cosA = glm::dot(avg, normals[i]);
                float clampedLen = std::max(cosA, 0.2f);
                seg.miterNormEnd = avg / clampedLen;
            } else {
                seg.miterNormEnd = normals[i];
            }
        }

        segs.push_back(seg);
    }

    return segs;
}

static bool projectToPolylineMiter(
    const glm::vec2& p,
    const std::vector<PolySegment2D>& segments,
    glm::vec2& outTargetScreen,
    float& outSignedDist,
    size_t& outBestSegIdx
) {
    if (segments.empty()) return false;

    float bestScore = 1e18f;
    outBestSegIdx = 0;

    for (size_t s = 0; s < segments.size(); ++s) {
        const auto& seg = segments[s];
        glm::vec2 rel = p - seg.pA;
        float tAlong = glm::dot(rel, seg.dir);

        float tClamped = std::clamp(tAlong, 0.0f, seg.len);
        glm::vec2 closestOnSeg = seg.pA + tClamped * seg.dir;
        float dist = glm::distance(p, closestOnSeg);

        // Additional penalty if point falls outside segment's miter sector
        if (tAlong < 0.0f) {
            float mDot = glm::dot(p - seg.pA, seg.miterNormStart);
            if (mDot < 0.0f) dist += 1000.0f;
        } else if (tAlong > seg.len) {
            float mDot = glm::dot(p - seg.pB, seg.miterNormEnd);
            if (mDot < 0.0f) dist += 1000.0f;
        }

        if (dist < bestScore) {
            bestScore = dist;
            outBestSegIdx = s;
        }
    }

    const auto& seg = segments[outBestSegIdx];
    glm::vec2 relA = p - seg.pA;
    outSignedDist = glm::dot(relA, seg.norm2D);

    // Continuous orthogonal projection onto segment line plane (miter sector defines sector ownership)
    outTargetScreen = p - outSignedDist * seg.norm2D;

    return true;
}

static void constrainedLaplacianRelax(
    Mesh* mesh,
    const std::vector<int>& vertAffected,
    const std::vector<int>& vertBoundary,
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

            // Respect boundary vertices (vertBoundary or vertOnEdge) in Laplacian averaging
            bool isCutBoundary = vertBoundary[i] || (!mesh->vertOnEdge.empty() && mesh->vertOnEdge[i] == 1);
            if (isCutBoundary) {
                int nbEdgeNeighbors = 0;
                float ax = 0.0f, ay = 0.0f, az = 0.0f;
                for (uint32_t j = start; j < start + count; ++j) {
                    uint32_t nid = mesh->vertRingVert[j];
                    bool nidIsCutBoundary = vertBoundary[nid] || (!mesh->vertOnEdge.empty() && mesh->vertOnEdge[nid] == 1);
                    if (nidIsCutBoundary) {
                        ax += mesh->verts[nid * 3];
                        ay += mesh->verts[nid * 3 + 1];
                        az += mesh->verts[nid * 3 + 2];
                        ++nbEdgeNeighbors;
                    }
                }
                if (nbEdgeNeighbors >= 2) {
                    smoothPos[i3]     = ax / nbEdgeNeighbors;
                    smoothPos[i3 + 1] = ay / nbEdgeNeighbors;
                    smoothPos[i3 + 2] = az / nbEdgeNeighbors;
                    continue;
                }
            }

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

            if (bProj.z <= 0.0f || bProj.z >= 1.0f) continue;

            glm::vec2 targetScreen;
            float sDist;
            size_t segIdx;
            if (projectToPolylineMiter(glm::vec2(bProj.x, bProj.y), segments, targetScreen, sDist, segIdx)) {
                // Tangential relaxation: project all affected vertices onto the cut plane so they slide in-plane
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

    // Option 5: Douglas-Peucker curve simplification
    std::vector<glm::vec2> simplified;
    douglasPeucker(polyline, 2.5f, simplified);
    sculpt_log("[ClipCurveTool] Polyline filtered & simplified from %zu to %zu points\n", curvePoints.size(), simplified.size());

    if (simplified.size() < 2) return false;

    // Build 2D polyline segments with miter joint sectors
    std::vector<PolySegment2D> segments = buildSegmentsWithMiters(simplified, altMode);

    if (segments.empty()) return false;

    glm::mat4 localToWorld = mesh->matrix;
    glm::mat4 worldToLocal = glm::inverse(localToWorld);

    int nbVerts = mesh->nbVerts;
    std::vector<int> vertAffected(nbVerts, 0);
    std::vector<int> vertBoundary(nbVerts, 0); // Option 2: boundary vertices near cut line
    std::vector<glm::vec3> vertSymScale(nbVerts, glm::vec3(1.0f));

    // Immutable snapshot of initial vertex positions before symmetry passes
    std::vector<glm::vec3> origVerts(nbVerts);
    for (int i = 0; i < nbVerts; ++i) {
        origVerts[i] = glm::vec3(mesh->verts[i * 3], mesh->verts[i * 3 + 1], mesh->verts[i * 3 + 2]);
    }

    constexpr float BOUNDARY_THRESHOLD_PX = 10.0f; // boundary threshold in pixels

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

    // Run 2 projection passes to handle overlapping clipped half-spaces at 90° / acute corners
    constexpr int NUM_PROJECTION_PASSES = 2;

    for (int projPass = 0; projPass < NUM_PROJECTION_PASSES; ++projPass) {
        for (size_t passIdx = 0; passIdx < passes.size(); ++passIdx) {
            const auto& sScale = passes[passIdx];
            bool isSymPass = (sScale != glm::vec3(1.0f));

            #pragma omp parallel for schedule(dynamic, 1024) reduction(min:globalMinDist) reduction(max:globalMaxDist)
            for (int i = 0; i < nbVerts; ++i) {
                if (!mesh->vertVisible[i]) continue;

                // In first pass use origVerts; in second pass re-evaluate updated mesh->verts for affected vertices
                glm::vec3 vLocalCurrent = (projPass == 0) ? origVerts[i] : glm::vec3(mesh->verts[i * 3], mesh->verts[i * 3 + 1], mesh->verts[i * 3 + 2]);
                if (projPass > 0 && !vertAffected[i]) continue;

                glm::vec3 vLocal = isSymPass ? reflectPointSymmetry(vLocalCurrent, sScale, mesh, symMode) : vLocalCurrent;

                glm::vec3 vWorld = glm::vec3(localToWorld * glm::vec4(vLocal, 1.0f));
                glm::vec3 vProj = camera.project(vWorld);
                if (vProj.z <= 0.0f || vProj.z >= 1.0f) continue;

                glm::vec2 vScreen(vProj.x, vProj.y);

                glm::vec2 targetScreen;
                float signedDist;
                size_t segIdx;
                if (projectToPolylineMiter(vScreen, segments, targetScreen, signedDist, segIdx)) {
                    if (signedDist < globalMinDist) globalMinDist = signedDist;
                    if (signedDist > globalMaxDist) globalMaxDist = signedDist;

                    if (signedDist < -0.01f) { // Clipped side
                        float safeZ = std::clamp(vProj.z, 0.0001f, 0.9999f);
                        glm::vec3 wCut = camera.unproject(targetScreen.x, targetScreen.y, safeZ);
                        glm::vec3 clippedLocal = glm::vec3(worldToLocal * glm::vec4(wCut, 1.0f));
                        glm::vec3 finalLocal = isSymPass ? reflectPointSymmetry(clippedLocal, sScale, mesh, symMode) : clippedLocal;

                        mesh->verts[i * 3]     = finalLocal.x;
                        mesh->verts[i * 3 + 1] = finalLocal.y;
                        mesh->verts[i * 3 + 2] = finalLocal.z;

                        if (!vertAffected[i] || !isSymPass) {
                            vertAffected[i] = 1;
                            vertSymScale[i] = sScale;
                        }

                        if (signedDist > -BOUNDARY_THRESHOLD_PX) {
                            vertBoundary[i] = 1;
                        }
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

    // Post-clip continuous relaxation pass (redistributes polygons around corners and interior)
    constexpr int RELAX_ITERATIONS = 8;
    sculpt_log("[ClipCurveTool] Running continuous Laplacian relaxation (%d iterations)...\n", RELAX_ITERATIONS);
    constrainedLaplacianRelax(mesh, vertAffected, vertBoundary, vertSymScale, symMode, segments, camera, localToWorld, worldToLocal, RELAX_ITERATIONS);

    // Final post-relaxation crisp boundary snap pass
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < nbVerts; ++i) {
        if (!vertAffected[i]) continue;
        bool isBoundary = vertBoundary[i] || (!mesh->vertOnEdge.empty() && mesh->vertOnEdge[i] == 1);
        if (!isBoundary) continue;

        int i3 = i * 3;
        const glm::vec3& sScale = vertSymScale[i];
        bool isSymPass = (sScale != glm::vec3(1.0f));

        glm::vec3 vLocal(mesh->verts[i3], mesh->verts[i3 + 1], mesh->verts[i3 + 2]);
        glm::vec3 vEvalLocal = isSymPass ? reflectPointSymmetry(vLocal, sScale, mesh, symMode) : vLocal;
        glm::vec3 vWorld = glm::vec3(localToWorld * glm::vec4(vEvalLocal, 1.0f));
        glm::vec3 vProj = camera.project(vWorld);

        if (vProj.z <= 0.0f || vProj.z >= 1.0f) continue;

        glm::vec2 targetScreen;
        float sDist;
        size_t segIdx;
        if (projectToPolylineMiter(glm::vec2(vProj.x, vProj.y), segments, targetScreen, sDist, segIdx)) {
            float safeZ = std::clamp(vProj.z, 0.0001f, 0.9999f);
            glm::vec3 wCut = camera.unproject(targetScreen.x, targetScreen.y, safeZ);
            glm::vec3 vCutLocal = glm::vec3(worldToLocal * glm::vec4(wCut, 1.0f));
            glm::vec3 finalLocal = isSymPass ? reflectPointSymmetry(vCutLocal, sScale, mesh, symMode) : vCutLocal;

            mesh->verts[i3]     = finalLocal.x;
            mesh->verts[i3 + 1] = finalLocal.y;
            mesh->verts[i3 + 2] = finalLocal.z;
        }
    }

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


