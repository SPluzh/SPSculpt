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

static manifold::Manifold buildSinglePrism(
    const std::vector<glm::vec2>& poly2D,
    const Camera& camera,
    const glm::mat4& invMeshMatrix,
    uint32_t capFaceID = 0
) {
    const size_t N = poly2D.size();
    if (N < 3) return manifold::Manifold();

    manifold::MeshGL meshGL;
    meshGL.numProp = 3; // positions only for cutter
    meshGL.vertProperties.resize(2 * N * 3);

    // Construct finite near and far bounds using camera rays
    for (size_t i = 0; i < N; ++i) {
        Ray ray = camera.getRay(poly2D[i].x, poly2D[i].y);
        glm::vec3 wNear = ray.origin;
        glm::vec3 wFar  = ray.origin + ray.dir * 2000.0f;

        glm::vec3 localNear = glm::vec3(invMeshMatrix * glm::vec4(wNear, 1.0f));
        glm::vec3 localFar  = glm::vec3(invMeshMatrix * glm::vec4(wFar, 1.0f));

        meshGL.vertProperties[i * 3 + 0] = localNear.x;
        meshGL.vertProperties[i * 3 + 1] = localNear.y;
        meshGL.vertProperties[i * 3 + 2] = localNear.z;

        meshGL.vertProperties[(i + N) * 3 + 0] = localFar.x;
        meshGL.vertProperties[(i + N) * 3 + 1] = localFar.y;
        meshGL.vertProperties[(i + N) * 3 + 2] = localFar.z;
    }

    // 2D Triangulation for near and far caps using CDT
    CDT::Triangulation<float> cdt;
    std::vector<CDT::V2d<float>> cdtVerts;
    cdtVerts.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        cdtVerts.push_back(CDT::V2d<float>::make(poly2D[i].x, poly2D[i].y));
    }
    cdt.insertVertices(cdtVerts);

    std::vector<CDT::Edge> edges;
    edges.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        edges.push_back(CDT::Edge(static_cast<CDT::VertInd>(i), static_cast<CDT::VertInd>((i + 1) % N)));
    }
    cdt.insertEdges(edges);
    cdt.eraseOuterTrianglesAndHoles();

    std::vector<uint32_t> triVerts;
    // Estimated triangles: side walls (2*N) + 2 caps (2 * (N-2))
    triVerts.reserve((2 * N + 2 * cdt.triangles.size()) * 3);

    // 1. Side wall quads (2 triangles each, oriented opposite to caps)
    for (size_t i = 0; i < N; ++i) {
        uint32_t curr = static_cast<uint32_t>(i);
        uint32_t next = static_cast<uint32_t>((i + 1) % N);
        uint32_t currFar = curr + static_cast<uint32_t>(N);
        uint32_t nextFar = next + static_cast<uint32_t>(N);

        // Tri 1: next -> curr -> nextFar
        triVerts.push_back(next);
        triVerts.push_back(curr);
        triVerts.push_back(nextFar);

        // Tri 2: curr -> currFar -> nextFar
        triVerts.push_back(curr);
        triVerts.push_back(currFar);
        triVerts.push_back(nextFar);
    }

    // 2. Near and Far caps from CDT
    for (const auto& tri : cdt.triangles) {
        uint32_t v0 = tri.vertices[0];
        uint32_t v1 = tri.vertices[1];
        uint32_t v2 = tri.vertices[2];

        // Verify 2D winding
        glm::vec2 p0 = poly2D[v0];
        glm::vec2 p1 = poly2D[v1];
        glm::vec2 p2 = poly2D[v2];
        float signedArea = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
        if (signedArea < 0.0f) {
            std::swap(v1, v2);
        }

        // Near cap (outward towards camera)
        triVerts.push_back(v0);
        triVerts.push_back(v1);
        triVerts.push_back(v2);

        // Far cap (outward away from camera, reversed winding)
        triVerts.push_back(v0 + static_cast<uint32_t>(N));
        triVerts.push_back(v2 + static_cast<uint32_t>(N));
        triVerts.push_back(v1 + static_cast<uint32_t>(N));
    }

    meshGL.triVerts = std::move(triVerts);
    if (capFaceID != 0) {
        meshGL.faceID.assign(meshGL.triVerts.size() / 3, capFaceID);
    }

    manifold::Manifold prism(meshGL);

    if (prism.Status() != manifold::Manifold::Error::NoError) {
        sculpt_log("[CUTTER 3D] Warning: prism initial status: %d (N=%zu, tris=%zu)\n",
                   (int)prism.Status(), N, meshGL.triVerts.size() / 3);
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
    uint32_t capFaceID
) {
    auto tStart = std::chrono::high_resolution_clock::now();

    auto cleanedLoops = cleanLasso2D(lassoPoints);
    if (cleanedLoops.empty()) return manifold::Manifold();

    std::vector<manifold::Manifold> prisms;
    for (const auto& loop : cleanedLoops) {
        manifold::Manifold p = buildSinglePrism(loop, camera, invMeshMatrix, capFaceID);
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
