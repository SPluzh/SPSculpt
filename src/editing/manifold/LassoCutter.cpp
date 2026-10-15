#include "editing/manifold/LassoCutter.h"
#include "scene/Camera.h"
#include "common/Logger.h"
#include <clipper2/clipper.h>
#include "CDT.h"
#include <chrono>
#include <cmath>
#include <algorithm>

namespace sculpt {

static std::vector<glm::vec2> chaikinSmooth(const std::vector<glm::vec2>& pts, int iterations) {
    if (pts.size() < 3 || iterations <= 0) return pts;
    std::vector<glm::vec2> cur = pts;
    for (int iter = 0; iter < iterations; ++iter) {
        std::vector<glm::vec2> next;
        next.reserve(cur.size() * 2);
        size_t n = cur.size();
        for (size_t i = 0; i < n; ++i) {
            const glm::vec2& p0 = cur[i];
            const glm::vec2& p1 = cur[(i + 1) % n];
            next.push_back(0.75f * p0 + 0.25f * p1);
            next.push_back(0.25f * p0 + 0.75f * p1);
        }
        cur = std::move(next);
    }
    return cur;
}

std::vector<std::vector<glm::vec2>> LassoCutter::cleanLasso2D(
    const std::vector<glm::vec2>& inputPoints,
    float simplifyTolerance,
    int chaikinIterations
) {
    auto tStart = std::chrono::high_resolution_clock::now();
    if (inputPoints.size() < 3) return {};

    Clipper2Lib::PathD path;
    path.reserve(inputPoints.size());
    for (const auto& pt : inputPoints) {
        path.push_back(Clipper2Lib::PointD(pt.x, pt.y));
    }

    // Simplify to remove redundant/collinear points
    path = Clipper2Lib::SimplifyPath(path, simplifyTolerance, true);
    if (path.size() < 3) return {};

    // Union with NonZero rule resolves self-intersections (including figure-8 loops)
    Clipper2Lib::PathsD subjects = { path };
    Clipper2Lib::PathsD cleaned = Clipper2Lib::Union(subjects, Clipper2Lib::FillRule::NonZero);

    std::vector<std::vector<glm::vec2>> result;
    size_t totalCleanedPoints = 0;

    for (auto& poly : cleaned) {
        if (poly.size() < 3) continue;

        // Ensure CCW orientation (positive area in Clipper2)
        if (Clipper2Lib::Area(poly) < 0.0) {
            std::reverse(poly.begin(), poly.end());
        }

        std::vector<glm::vec2> polyPts;
        polyPts.reserve(poly.size());
        for (const auto& p : poly) {
            polyPts.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y));
        }

        // Apply Chaikin smoothing
        if (chaikinIterations > 0) {
            polyPts = chaikinSmooth(polyPts, chaikinIterations);
        }

        totalCleanedPoints += polyPts.size();
        result.push_back(std::move(polyPts));
    }

    auto tEnd = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

    sculpt_log("[CLIPPER2 2D] Input Points: %zu -> Cleaned: %zu points in %zu loop(s) in %.2f ms\n",
               inputPoints.size(), totalCleanedPoints, result.size(), ms);

    return result;
}

std::vector<glm::vec2> LassoCutter::resampleEquidistant(
    const std::vector<glm::vec2>& poly,
    float targetSpacingPx
) {
    const size_t K = poly.size();
    if (K < 3) return poly;

    // 1. Calculate cumulative perimeter distances
    std::vector<float> cumDist(K + 1, 0.0f);
    for (size_t i = 0; i < K; ++i) {
        size_t next = (i + 1) % K;
        float d = glm::distance(poly[i], poly[next]);
        cumDist[i + 1] = cumDist[i] + d;
    }
    float totalPerimeter = cumDist[K];
    if (totalPerimeter < 5.0f) return poly;

    // 2. Determine number of equidistant samples
    int N = static_cast<int>(std::round(totalPerimeter / targetSpacingPx));
    N = std::clamp(N, 16, 256);
    float step = totalPerimeter / static_cast<float>(N);

    // 3. Resample along the perimeter
    std::vector<glm::vec2> resampled;
    resampled.reserve(N);

    size_t curSeg = 0;
    for (int m = 0; m < N; ++m) {
        float targetDist = static_cast<float>(m) * step;

        while (curSeg < K && cumDist[curSeg + 1] < targetDist) {
            curSeg++;
        }
        if (curSeg >= K) curSeg = K - 1;

        float segStartDist = cumDist[curSeg];
        float segLen = cumDist[curSeg + 1] - segStartDist;
        float t = (segLen > 1e-6f) ? ((targetDist - segStartDist) / segLen) : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);

        size_t nextIdx = (curSeg + 1) % K;
        glm::vec2 pt = glm::mix(poly[curSeg], poly[nextIdx], t);
        resampled.push_back(pt);
    }

    return resampled;
}

static manifold::Manifold buildSinglePrism(
    const std::vector<glm::vec2>& poly2D,
    const Camera& camera,
    const glm::mat4& invMeshMatrix,
    uint32_t capFaceID = 0,
    const glm::vec3& localBoxMin = glm::vec3(0.0f),
    const glm::vec3& localBoxMax = glm::vec3(0.0f),
    float targetEdgeLength = 0.0f
) {
    if (poly2D.size() < 3) return manifold::Manifold();

    // 1. Compute 2D centroid of lasso in screen space
    glm::vec2 center2D(0.0f);
    for (const auto& pt : poly2D) {
        center2D += pt;
    }
    center2D /= static_cast<float>(poly2D.size());

    // 2. Compute central camera ray transformed into mesh local space
    Ray cRay = camera.getRay(center2D.x, center2D.y);
    glm::vec3 cOriginLocal = glm::vec3(invMeshMatrix * glm::vec4(cRay.origin, 1.0f));
    glm::vec3 cDirLocal = glm::vec3(invMeshMatrix * glm::vec4(cRay.origin + cRay.dir, 1.0f)) - cOriginLocal;
    float cDirLen = glm::length(cDirLocal);
    if (cDirLen > 1e-6f) cDirLocal /= cDirLen;
    else cDirLocal = glm::vec3(0.0f, 0.0f, -1.0f);

    // 3. Determine depth range [tNear, tFar] based on mesh local bounding box
    bool hasBox = (localBoxMin != localBoxMax) &&
                  (localBoxMax.x >= localBoxMin.x) &&
                  (localBoxMax.y >= localBoxMin.y) &&
                  (localBoxMax.z >= localBoxMin.z);

    float tNear = 1.0f;
    float tFar = 1000.0f;

    if (hasBox) {
        glm::vec3 corners[8] = {
            {localBoxMin.x, localBoxMin.y, localBoxMin.z},
            {localBoxMax.x, localBoxMin.y, localBoxMin.z},
            {localBoxMax.x, localBoxMax.y, localBoxMin.z},
            {localBoxMin.x, localBoxMax.y, localBoxMin.z},
            {localBoxMin.x, localBoxMin.y, localBoxMax.z},
            {localBoxMax.x, localBoxMin.y, localBoxMax.z},
            {localBoxMax.x, localBoxMax.y, localBoxMax.z},
            {localBoxMin.x, localBoxMax.y, localBoxMax.z}
        };

        float tMin = 1e9f;
        float tMax = -1e9f;
        for (int k = 0; k < 8; ++k) {
            float t = glm::dot(corners[k] - cOriginLocal, cDirLocal);
            tMin = std::min(tMin, t);
            tMax = std::max(tMax, t);
        }

        float span = tMax - tMin;
        if (span < 1.0f) span = 10.0f;

        float margin = std::max(span * 0.25f, (targetEdgeLength > 0.0f) ? (2.5f * targetEdgeLength) : 5.0f);
        tNear = tMin - margin;
        tFar  = tMax + margin;

        if (!camera.isOrthographic() && tNear < 0.1f) {
            tNear = 0.1f;
        }
        if (tFar <= tNear + 1.0f) {
            tFar = tNear + span + margin;
        }
    } else {
        tNear = 1.0f;
        tFar = 500.0f;
    }

    // 4. Determine screen pixel step corresponding to targetEdgeLength in 3D local units at tMid
    float tMid = (tNear + tFar) * 0.5f;
    glm::vec3 pMidLocal0 = cOriginLocal + tMid * cDirLocal;

    Ray cRayShift = camera.getRay(center2D.x + 20.0f, center2D.y);
    glm::vec3 cOriginShift = glm::vec3(invMeshMatrix * glm::vec4(cRayShift.origin, 1.0f));
    glm::vec3 cDirShift = glm::vec3(invMeshMatrix * glm::vec4(cRayShift.origin + cRayShift.dir, 1.0f)) - cOriginShift;
    if (glm::length(cDirShift) > 1e-6f) cDirShift = glm::normalize(cDirShift);
    glm::vec3 pMidLocalShift = cOriginShift + tMid * cDirShift;

    float localDist20 = glm::distance(pMidLocal0, pMidLocalShift);
    float localUnitsPerPx = localDist20 / 20.0f;

    float targetSpacingPx = 12.0f;
    if (targetEdgeLength > 1e-4f && localUnitsPerPx > 1e-6f) {
        targetSpacingPx = targetEdgeLength / localUnitsPerPx;
    }
    targetSpacingPx = std::clamp(targetSpacingPx, 4.0f, 50.0f);

    // 5. Resample 2D lasso points to equidistant spacing
    std::vector<glm::vec2> resampledPoly = LassoCutter::resampleEquidistant(poly2D, targetSpacingPx);
    const size_t N = resampledPoly.size();
    if (N < 3) return manifold::Manifold();

    sculpt_log("[CUTTER 3D] Camera: %s, Screen Centroid: (%.1f, %.1f), TargetEdgeLen: %.3f mm, Units/Px: %.4f\n",
               camera.isOrthographic() ? "Orthographic" : "Perspective",
               center2D.x, center2D.y, targetEdgeLength, localUnitsPerPx);
    // 6. Solid prism extrusion subdivided into M depth slices matching targetEdgeLength
    float depthSpan = tFar - tNear;
    int M = 1;
    if (targetEdgeLength > 1e-4f) {
        float sliceStep = std::max(targetEdgeLength, 0.25f);
        M = static_cast<int>(std::round(depthSpan / sliceStep));
    }
    M = std::clamp(M, 1, 80);

    sculpt_log("[CUTTER 3D] Depth Range: [%.2f, %.2f] (span: %.2f mm, M=%d slices, step: %.3f mm), Target Spacing: %.2f px -> Resampled %zu points\n",
               tNear, tFar, depthSpan, M, depthSpan / static_cast<float>(M), targetSpacingPx, N);

    // 7. Precompute local rays for all perimeter points
    struct LocalRay {
        glm::vec3 origin;
        glm::vec3 dir;
    };
    std::vector<LocalRay> localRays(N);
    float minRayAngle = 180.0f, maxRayAngle = 0.0f, sumRayAngle = 0.0f;
    for (size_t i = 0; i < N; ++i) {
        Ray r = camera.getRay(resampledPoly[i].x, resampledPoly[i].y);
        glm::vec3 o = glm::vec3(invMeshMatrix * glm::vec4(r.origin, 1.0f));
        glm::vec3 d = glm::vec3(invMeshMatrix * glm::vec4(r.origin + r.dir, 1.0f)) - o;
        float dLen = glm::length(d);
        if (dLen > 1e-6f) d /= dLen;
        else d = cDirLocal;
        localRays[i] = {o, d};

        float dotR = std::clamp(glm::dot(d, cDirLocal), -1.0f, 1.0f);
        float a = glm::degrees(std::acos(dotR));
        minRayAngle = std::min(minRayAngle, a);
        maxRayAngle = std::max(maxRayAngle, a);
        sumRayAngle += a;
    }
    float avgRayAngle = N > 0 ? (sumRayAngle / (float)N) : 0.0f;
    sculpt_log("[CUTTER 3D] Ray spread vs Center Ray [%.2f, %.2f, %.2f]: min=%.2f deg, max=%.2f deg, avg=%.2f deg\n",
               cDirLocal.x, cDirLocal.y, cDirLocal.z, minRayAngle, maxRayAngle, avgRayAngle);

    manifold::MeshGL meshGL;
    meshGL.numProp = 12; // pos(3), norm(3), color(3), material(3) to match mesh and prevent zeroed properties in CSG
    meshGL.vertProperties.resize((M + 1) * N * 12);

    for (int s = 0; s <= M; ++s) {
        float frac = static_cast<float>(s) / static_cast<float>(M);
        float t = tNear + frac * depthSpan;
        for (size_t i = 0; i < N; ++i) {
            glm::vec3 pos = localRays[i].origin + t * localRays[i].dir;
            size_t idx = (s * N + i) * 12;
            meshGL.vertProperties[idx + 0] = pos.x;
            meshGL.vertProperties[idx + 1] = pos.y;
            meshGL.vertProperties[idx + 2] = pos.z;
            meshGL.vertProperties[idx + 3] = 0.0f;
            meshGL.vertProperties[idx + 4] = 1.0f;
            meshGL.vertProperties[idx + 5] = 0.0f;
            meshGL.vertProperties[idx + 6] = 1.0f;
            meshGL.vertProperties[idx + 7] = 1.0f;
            meshGL.vertProperties[idx + 8] = 1.0f;
            meshGL.vertProperties[idx + 9] = 0.5f;
            meshGL.vertProperties[idx + 10] = 0.0f;
            meshGL.vertProperties[idx + 11] = 1.0f; // unmasked
        }
    }

    // 8. 2D Triangulation for near and far caps using CDT
    CDT::Triangulation<float> cdt;
    std::vector<CDT::V2d<float>> cdtVerts;
    cdtVerts.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        cdtVerts.push_back(CDT::V2d<float>::make(resampledPoly[i].x, resampledPoly[i].y));
    }

    try {
        cdt.insertVertices(cdtVerts);

        std::vector<CDT::Edge> edges;
        edges.reserve(N);
        for (size_t i = 0; i < N; ++i) {
            edges.push_back(CDT::Edge(static_cast<CDT::VertInd>(i), static_cast<CDT::VertInd>((i + 1) % N)));
        }
        cdt.insertEdges(edges);
        cdt.eraseOuterTrianglesAndHoles();
    } catch (const std::exception& ex) {
        sculpt_log("[LassoCutter] CDT triangulation failed: %s. Using triangle fan fallback.\n", ex.what());
    } catch (...) {
        sculpt_log("[LassoCutter] CDT triangulation unknown error. Using triangle fan fallback.\n");
    }

    std::vector<uint32_t> triVerts;
    triVerts.reserve((2 * N * M + 2 * std::max((size_t)N, cdt.triangles.size())) * 3);

    // 8.1. Lateral wall quads (subdivided into M depth slices, forming regular square grid)
    for (int s = 0; s < M; ++s) {
        for (size_t i = 0; i < N; ++i) {
            uint32_t curr     = static_cast<uint32_t>(s * N + i);
            uint32_t next     = static_cast<uint32_t>(s * N + ((i + 1) % N));
            uint32_t currNext = static_cast<uint32_t>((s + 1) * N + i);
            uint32_t nextNext = static_cast<uint32_t>((s + 1) * N + ((i + 1) % N));

            // Tri 1: next -> curr -> nextNext
            triVerts.push_back(next);
            triVerts.push_back(curr);
            triVerts.push_back(nextNext);

            // Tri 2: curr -> currNext -> nextNext
            triVerts.push_back(curr);
            triVerts.push_back(currNext);
            triVerts.push_back(nextNext);
        }
    }

    // 8.2. Near and Far caps
    uint32_t baseFar = static_cast<uint32_t>(M * N);
    if (!cdt.triangles.empty()) {
        for (const auto& tri : cdt.triangles) {
            uint32_t v0 = tri.vertices[0];
            uint32_t v1 = tri.vertices[1];
            uint32_t v2 = tri.vertices[2];

            // Verify 2D winding
            glm::vec2 p0 = resampledPoly[v0];
            glm::vec2 p1 = resampledPoly[v1];
            glm::vec2 p2 = resampledPoly[v2];
            float signedArea = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
            if (signedArea < 0.0f) {
                std::swap(v1, v2);
            }

            // Near cap (outward towards camera)
            triVerts.push_back(v0);
            triVerts.push_back(v1);
            triVerts.push_back(v2);

            // Far cap (outward away from camera, reversed winding)
            triVerts.push_back(v0 + baseFar);
            triVerts.push_back(v2 + baseFar);
            triVerts.push_back(v1 + baseFar);
        }
    } else {
        // Fallback fan if CDT produced no triangles
        for (size_t i = 1; i + 1 < N; ++i) {
            triVerts.push_back(0);
            triVerts.push_back(static_cast<uint32_t>(i));
            triVerts.push_back(static_cast<uint32_t>(i + 1));

            triVerts.push_back(baseFar);
            triVerts.push_back(baseFar + static_cast<uint32_t>(i + 1));
            triVerts.push_back(baseFar + static_cast<uint32_t>(i));
        }
    }

    meshGL.triVerts = std::move(triVerts);
    if (capFaceID != 0) {
        meshGL.faceID.assign(meshGL.triVerts.size() / 3, capFaceID);
    }

    manifold::Manifold prism(meshGL);

    if (prism.Status() != manifold::Manifold::Error::NoError) {
        sculpt_log("[CUTTER 3D] Warning: prism initial status: %d (N=%zu, M=%d, tris=%zu)\n",
                   (int)prism.Status(), N, M, meshGL.triVerts.size() / 3);
    }

    if (prism.GetProperties().volume < 0.0f) {
        for (size_t t = 0; t < meshGL.triVerts.size(); t += 3) {
            std::swap(meshGL.triVerts[t + 1], meshGL.triVerts[t + 2]);
        }
        prism = manifold::Manifold(meshGL);
    }

    return prism;
}

manifold::Manifold LassoCutter::build3DCutterPrism(
    const std::vector<glm::vec2>& lassoPoints,
    const Camera& camera,
    const glm::mat4& invMeshMatrix,
    uint32_t capFaceID,
    const glm::vec3& localBoxMin,
    const glm::vec3& localBoxMax,
    float targetEdgeLength
) {
    auto tStart = std::chrono::high_resolution_clock::now();

    auto cleanedLoops = cleanLasso2D(lassoPoints);
    if (cleanedLoops.empty()) return manifold::Manifold();

    std::vector<manifold::Manifold> prisms;
    for (const auto& loop : cleanedLoops) {
        manifold::Manifold p = buildSinglePrism(loop, camera, invMeshMatrix, capFaceID, localBoxMin, localBoxMax, targetEdgeLength);
        if (!p.IsEmpty() && p.Status() == manifold::Manifold::Error::NoError) {
            prisms.push_back(std::move(p));
        }
    }

    if (prisms.empty()) return manifold::Manifold();

    manifold::Manifold cutter = (prisms.size() == 1) ?
        std::move(prisms[0]) :
        manifold::Manifold::BatchBoolean(prisms, manifold::OpType::Add);

    auto tEnd = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    float vol = cutter.GetProperties().volume;

    sculpt_log("[CUTTER 3D] Built Frustum Prism: %d verts, %d tris, Volume: %.1f mm3 in %.2f ms (Status: %d)\n",
               cutter.NumVert(), cutter.NumTri(), vol, ms, (int)cutter.Status());

    return cutter;
}

bool LassoCutter::stressTestFigure8(const Camera& camera) {
    sculpt_log("[CUTTER STRESS TEST] Testing self-intersecting Figure-8 lasso...\n");

    // Construct a self-intersecting figure-8 in screen space (around screen center)
    float cx = camera.getWidth() > 0 ? camera.getWidth() * 0.5f : 600.0f;
    float cy = camera.getHeight() > 0 ? camera.getHeight() * 0.5f : 400.0f;
    float r = 150.0f;

    std::vector<glm::vec2> fig8;
    const int sampleCount = 64;
    for (int i = 0; i < sampleCount; ++i) {
        float t = (float)i / (float)sampleCount * 2.0f * 3.1415926535f;
        // Lemniscate of Gerono: x = cos(t), y = sin(2t)/2
        float x = cx + r * std::cos(t);
        float y = cy + r * 0.5f * std::sin(2.0f * t);
        fig8.emplace_back(x, y);
    }

    manifold::Manifold cutter = build3DCutterPrism(fig8, camera);
    float vol = cutter.GetProperties().volume;
    int degen = cutter.NumDegenerateTris();

    bool success = (!cutter.IsEmpty() && cutter.Status() == manifold::Manifold::Error::NoError && vol > 0.0f && degen == 0);
    sculpt_log("[CUTTER STRESS TEST] Result: %s | Volume: %.2f | Degenerate faces: %d | Status: %d\n",
               success ? "SUCCESS" : "FAILED", vol, degen, (int)cutter.Status());

    return success;
}

} // namespace sculpt
