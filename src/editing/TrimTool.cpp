#define GLM_ENABLE_EXPERIMENTAL
#include "editing/TrimTool.h"
#include "editing/SculptManager.h"
#include "mesh/Mesh.h"
#include "mesh/NormalCalc.h"
#include "scene/Camera.h"
#include "common/Logger.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <set>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/norm.hpp>

#include <functional>
#include <array>

static float computeAverageMeshEdgeScreenLength(
    const std::vector<float>& verts,
    const std::vector<uint32_t>& keptFaces,
    const Camera& camera,
    const glm::mat4& meshMatrix
) {
    int numFaces = static_cast<int>(keptFaces.size() / 4);
    if (numFaces <= 0) return 15.0f;

    int step = std::max(1, numFaces / 500);
    double totalLen = 0.0;
    int count = 0;

    for (int f = 0; f < numFaces; f += step) {
        uint32_t v0 = keptFaces[f * 4];
        uint32_t v1 = keptFaces[f * 4 + 1];
        uint32_t v2 = keptFaces[f * 4 + 2];
        uint32_t v3 = keptFaces[f * 4 + 3];

        uint32_t fVerts[4] = {v0, v1, v2, v3};
        int numV = (v3 == 0xffffffff) ? 3 : 4;

        for (int i = 0; i < numV; ++i) {
            uint32_t a = fVerts[i];
            uint32_t b = fVerts[(i + 1) % numV];
            if (a >= verts.size() / 3 || b >= verts.size() / 3) continue;

            glm::vec3 aLocal(verts[a * 3], verts[a * 3 + 1], verts[a * 3 + 2]);
            glm::vec3 bLocal(verts[b * 3], verts[b * 3 + 1], verts[b * 3 + 2]);

            glm::vec3 aWorld = glm::vec3(meshMatrix * glm::vec4(aLocal, 1.0f));
            glm::vec3 bWorld = glm::vec3(meshMatrix * glm::vec4(bLocal, 1.0f));

            glm::vec3 aProj = camera.project(aWorld);
            glm::vec3 bProj = camera.project(bWorld);

            float d = glm::distance(glm::vec2(aProj.x, aProj.y), glm::vec2(bProj.x, bProj.y));
            if (d > 0.1f && d < 1000.0f) {
                totalLen += d;
                count++;
            }
        }
    }

    if (count == 0) return 15.0f;
    float avg = static_cast<float>(totalLen / count);
    return std::clamp(avg, 4.0f, 35.0f);
}

struct CapVert {
    glm::vec2 p2D;
    float pz;
    bool isBoundary;
    uint32_t origVid;
};

struct CapTri {
    int a, b, c;
};

static bool pointInPolygon2D(const glm::vec2& p, const std::vector<glm::vec2>& polygon) {
    int wn = 0;
    int n = static_cast<int>(polygon.size());
    for (int i = 0; i < n; ++i) {
        glm::vec2 p1 = polygon[i];
        glm::vec2 p2 = polygon[(i + 1) % n];
        if (p1.y <= p.y) {
            if (p2.y > p.y) {
                float vt = (p.y - p1.y) / (p2.y - p1.y);
                if (p.x < p1.x + vt * (p2.x - p1.x)) ++wn;
            }
        } else {
            if (p2.y <= p.y) {
                float vt = (p.y - p1.y) / (p2.y - p1.y);
                if (p.x < p1.x + vt * (p2.x - p1.x)) --wn;
            }
        }
    }
    return wn != 0;
}

static float minDistToPoly2D(const glm::vec2& p, const std::vector<glm::vec2>& poly) {
    float minD2 = 1e18f;
    int n = static_cast<int>(poly.size());
    for (int i = 0; i < n; ++i) {
        glm::vec2 a = poly[i];
        glm::vec2 b = poly[(i + 1) % n];
        glm::vec2 ab = b - a;
        float len2 = glm::length2(ab);
        float t = (len2 > 1e-8f) ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        glm::vec2 proj = a + t * ab;
        minD2 = std::min(minD2, glm::distance2(p, proj));
    }
    return std::sqrt(minD2);
}

static std::vector<glm::vec2> generateQuincunxGrid(
    const std::vector<glm::vec2>& poly2D,
    float step
) {
    std::vector<glm::vec2> grid;
    if (poly2D.size() < 3 || step <= 0.0f) return grid;

    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    glm::vec2 centroid(0.0f);
    for (const auto& pt : poly2D) {
        minX = std::min(minX, pt.x);
        minY = std::min(minY, pt.y);
        maxX = std::max(maxX, pt.x);
        maxY = std::max(maxY, pt.y);
        centroid += pt;
    }
    centroid /= static_cast<float>(poly2D.size());

    float width = maxX - minX;
    float height = maxY - minY;

    auto buildGrid = [&](float curStep) {
        std::vector<glm::vec2> res;
        float margin = std::clamp(curStep * 0.15f, 0.5f, 4.0f);
        int row = 0;
        float rowHeight = curStep * 0.8660254f;
        for (float y = minY + margin; y < maxY - margin; y += rowHeight, ++row) {
            float offsetX = (row % 2 == 1) ? curStep * 0.5f : 0.0f;
            for (float x = minX + margin + offsetX; x < maxX - margin; x += curStep) {
                glm::vec2 p(x, y);
                if (pointInPolygon2D(p, poly2D)) {
                    if (minDistToPoly2D(p, poly2D) >= margin) {
                        res.push_back(p);
                    }
                }
            }
        }
        return res;
    };

    grid = buildGrid(step);
    if (grid.empty()) {
        grid = buildGrid(step * 0.5f);
    }

    if (grid.empty()) {
        if (pointInPolygon2D(centroid, poly2D)) {
            grid.push_back(centroid);
        } else {
            // Pick midpoint between bbox center and first vertex inside
            glm::vec2 bboxCenter((minX + maxX) * 0.5f, (minY + maxY) * 0.5f);
            if (pointInPolygon2D(bboxCenter, poly2D)) {
                grid.push_back(bboxCenter);
            } else {
                for (size_t i = 0; i < poly2D.size(); ++i) {
                    glm::vec2 m = (poly2D[i] + poly2D[(i + 1) % poly2D.size()]) * 0.5f;
                    glm::vec2 dir = glm::normalize(centroid - m);
                    glm::vec2 testP = m + dir * 2.0f;
                    if (pointInPolygon2D(testP, poly2D)) {
                        grid.push_back(testP);
                        break;
                    }
                }
            }
        }
    }

    sculpt_log("[TrimTool] generateQuincunxGrid: step=%.2f, AABB=(%.1fx%.1f), grid vertices=%zu\n",
                step, width, height, grid.size());

    return grid;
}

static std::vector<CapTri> earClipPolygon2D(const std::vector<glm::vec2>& poly) {
    std::vector<CapTri> tris;
    int n = static_cast<int>(poly.size());
    if (n < 3) return tris;
    if (n == 3) {
        tris.push_back({0, 1, 2});
        return tris;
    }

    double area = 0.0;
    for (int i = 0; i < n; ++i) {
        int next = (i + 1) % n;
        area += (double)poly[i].x * poly[next].y - (double)poly[next].x * poly[i].y;
    }
    bool isCCW = (area > 0.0);

    auto cross2D = [](const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };

    auto isConvex = [&](int u, int v, int w) {
        float cp = cross2D(poly[u], poly[v], poly[w]);
        return isCCW ? (cp > 1e-6f) : (cp < -1e-6f);
    };

    auto pointInTri = [&](const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
        float c1 = cross2D(a, b, p);
        float c2 = cross2D(b, c, p);
        float c3 = cross2D(c, a, p);
        bool has_neg = (c1 < -1e-4f) || (c2 < -1e-4f) || (c3 < -1e-4f);
        bool has_pos = (c1 > 1e-4f) || (c2 > 1e-4f) || (c3 > 1e-4f);
        return !(has_neg && has_pos);
    };

    auto isEar = [&](int u, int v, int w, const std::vector<int>& Vlist) {
        if (!isConvex(u, v, w)) return false;
        for (int idx : Vlist) {
            if (idx == u || idx == v || idx == w) continue;
            if (pointInTri(poly[idx], poly[u], poly[v], poly[w])) return false;
        }
        return true;
    };

    std::function<void(const std::vector<int>&)> triangulateSubPoly = [&](const std::vector<int>& V) {
        int sz = static_cast<int>(V.size());
        if (sz < 3) return;
        if (sz == 3) {
            tris.push_back({V[0], V[1], V[2]});
            return;
        }

        std::vector<int> currentV = V;
        int count = 3 * sz;
        int curr = 0;

        while (currentV.size() > 2 && count > 0) {
            count--;
            int curSz = static_cast<int>(currentV.size());
            if (curSz == 3) {
                tris.push_back({currentV[0], currentV[1], currentV[2]});
                currentV.clear();
                break;
            }

            int prev = (curr + curSz - 1) % curSz;
            int next = (curr + 1) % curSz;

            int u = currentV[prev], v = currentV[curr], w = currentV[next];

            if (isEar(u, v, w, currentV)) {
                tris.push_back({u, v, w});
                currentV.erase(currentV.begin() + curr);
                if (curr >= (int)currentV.size()) curr = 0;
                count = 3 * static_cast<int>(currentV.size());
            } else {
                curr = (curr + 1) % curSz;
            }
        }

        if (currentV.size() > 3) {
            int curSz = static_cast<int>(currentV.size());
            int bestI = 0, bestJ = curSz / 2;
            float minDiagDist = 1e18f;

            auto segmentsIntersect = [](const glm::vec2& a1, const glm::vec2& a2, const glm::vec2& b1, const glm::vec2& b2) {
                auto ccw = [](const glm::vec2& p1, const glm::vec2& p2, const glm::vec2& p3) {
                    return (p3.y - p1.y) * (p2.x - p1.x) > (p2.y - p1.y) * (p3.x - p1.x);
                };
                if (glm::distance2(a1, b1) < 1e-4f || glm::distance2(a1, b2) < 1e-4f ||
                    glm::distance2(a2, b1) < 1e-4f || glm::distance2(a2, b2) < 1e-4f) return false;
                return (ccw(a1, b1, b2) != ccw(a2, b1, b2)) && (ccw(a1, a2, b1) != ccw(a1, a2, b2));
            };

            bool foundSplit = false;
            for (int i = 0; i < curSz; ++i) {
                for (int j = i + 2; j < curSz; ++j) {
                    if (i == 0 && j == curSz - 1) continue;
                    int u = currentV[i], v = currentV[j];
                    glm::vec2 pA = poly[u], pB = poly[v];

                    bool crosses = false;
                    for (int kIdx = 0; kIdx < curSz; ++kIdx) {
                        int eU = currentV[kIdx], eV = currentV[(kIdx + 1) % curSz];
                        if (segmentsIntersect(pA, pB, poly[eU], poly[eV])) {
                            crosses = true;
                            break;
                        }
                    }
                    if (!crosses) {
                        float d = glm::distance(pA, pB);
                        if (d < minDiagDist) {
                            minDiagDist = d;
                            bestI = i;
                            bestJ = j;
                            foundSplit = true;
                        }
                    }
                }
            }

            if (!foundSplit) {
                bestI = 0;
                bestJ = curSz / 2;
            }

            std::vector<int> sub1, sub2;
            for (int kIdx = bestI; kIdx <= bestJ; ++kIdx) sub1.push_back(currentV[kIdx]);
            for (int kIdx = bestJ; kIdx < curSz; ++kIdx) sub2.push_back(currentV[kIdx]);
            for (int kIdx = 0; kIdx <= bestI; ++kIdx) sub2.push_back(currentV[kIdx]);

            triangulateSubPoly(sub1);
            triangulateSubPoly(sub2);
        }
    };

    std::vector<int> initV(n);
    for (int i = 0; i < n; ++i) initV[i] = i;
    triangulateSubPoly(initV);

    return tris;
}

static std::vector<float> computeMeanValueWeights(
    const glm::vec2& p,
    const std::vector<glm::vec2>& poly
) {
    int n = static_cast<int>(poly.size());
    std::vector<float> weights(n, 0.0f);
    if (n == 0) return weights;

    std::vector<glm::vec2> s(n);
    std::vector<float> r(n);

    for (int i = 0; i < n; ++i) {
        s[i] = poly[i] - p;
        r[i] = glm::length(s[i]);
        if (r[i] < 1e-5f) {
            weights[i] = 1.0f;
            return weights;
        }
    }

    for (int i = 0; i < n; ++i) {
        int iNext = (i + 1) % n;
        float cross = s[i].x * s[iNext].y - s[i].y * s[iNext].x;
        float dot = glm::dot(s[i], s[iNext]);
        if (std::abs(cross) < 1e-5f * r[i] * r[iNext] && dot < 0.0f) {
            float t = r[i] / (r[i] + r[iNext]);
            weights[i] = 1.0f - t;
            weights[iNext] = t;
            return weights;
        }
    }

    std::vector<float> tanHalf(n);
    for (int i = 0; i < n; ++i) {
        int iNext = (i + 1) % n;
        float cross = s[i].x * s[iNext].y - s[i].y * s[iNext].x;
        float dot = glm::dot(s[i], s[iNext]);
        float denom = r[i] * r[iNext] + dot;
        if (std::abs(denom) < 1e-7f) {
            tanHalf[i] = 0.0f;
        } else {
            tanHalf[i] = cross / denom;
        }
    }

    float sumW = 0.0f;
    for (int i = 0; i < n; ++i) {
        int iPrev = (i + n - 1) % n;
        float w = (tanHalf[iPrev] + tanHalf[i]) / r[i];
        weights[i] = w;
        sumW += w;
    }

    if (std::abs(sumW) > 1e-8f) {
        for (int i = 0; i < n; ++i) {
            weights[i] /= sumW;
        }
    } else {
        float invN = 1.0f / static_cast<float>(n);
        for (int i = 0; i < n; ++i) weights[i] = invN;
    }

    return weights;
}

static bool inCircumcircle2D(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
    float ax = a.x - p.x, ay = a.y - p.y;
    float bx = b.x - p.x, by = b.y - p.y;
    float cx = c.x - p.x, cy = c.y - p.y;

    float asq = ax * ax + ay * ay;
    float bsq = bx * bx + by * by;
    float csq = cx * cx + cy * cy;

    float det = asq * (bx * cy - cx * by)
              - bsq * (ax * cy - cx * ay)
              + csq * (ax * by - bx * ay);

    float orient = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (orient < 0.0f) det = -det;

    return det > 1e-6f;
}

static bool lineSegmentsIntersect2D(
    const glm::vec2& a1, const glm::vec2& a2,
    const glm::vec2& b1, const glm::vec2& b2
) {
    auto ccw = [](const glm::vec2& p1, const glm::vec2& p2, const glm::vec2& p3) {
        return (p3.y - p1.y) * (p2.x - p1.x) > (p2.y - p1.y) * (p3.x - p1.x);
    };
    if (glm::distance2(a1, b1) < 1e-5f || glm::distance2(a1, b2) < 1e-5f ||
        glm::distance2(a2, b1) < 1e-5f || glm::distance2(a2, b2) < 1e-5f) {
        return false;
    }
    return (ccw(a1, b1, b2) != ccw(a2, b1, b2)) && (ccw(a1, a2, b1) != ccw(a1, a2, b2));
}

static bool isTriValidForPoly(
    const glm::vec2& pA, const glm::vec2& pB, const glm::vec2& pC,
    const std::vector<glm::vec2>& poly2D
) {
    glm::vec2 centroid = (pA + pB + pC) / 3.0f;
    if (!pointInPolygon2D(centroid, poly2D)) return false;

    glm::vec2 mAB = (pA + pB) * 0.5f;
    glm::vec2 mBC = (pB + pC) * 0.5f;
    glm::vec2 mCA = (pC + pA) * 0.5f;

    if (!pointInPolygon2D(mAB, poly2D) && minDistToPoly2D(mAB, poly2D) > 0.5f) return false;
    if (!pointInPolygon2D(mBC, poly2D) && minDistToPoly2D(mBC, poly2D) > 0.5f) return false;
    if (!pointInPolygon2D(mCA, poly2D) && minDistToPoly2D(mCA, poly2D) > 0.5f) return false;

    int n = static_cast<int>(poly2D.size());
    for (int i = 0; i < n; ++i) {
        glm::vec2 b1 = poly2D[i];
        glm::vec2 b2 = poly2D[(i + 1) % n];

        if (lineSegmentsIntersect2D(pA, pB, b1, b2)) return false;
        if (lineSegmentsIntersect2D(pB, pC, b1, b2)) return false;
        if (lineSegmentsIntersect2D(pC, pA, b1, b2)) return false;
    }

    return true;
}

static std::vector<CapTri> bowyerWatsonDelaunay(
    const std::vector<glm::vec2>& points,
    const std::vector<glm::vec2>& poly2D
) {
    std::vector<CapTri> tris;
    int numPts = static_cast<int>(points.size());
    if (numPts < 3) return tris;

    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (const auto& p : points) {
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    }

    float dx = maxX - minX;
    float dy = maxY - minY;
    float deltaMax = std::max(dx, dy) * 10.0f;
    float midX = (minX + maxX) * 0.5f;
    float midY = (minY + maxY) * 0.5f;

    std::vector<glm::vec2> allPts = points;
    int s0 = numPts;
    int s1 = numPts + 1;
    int s2 = numPts + 2;

    allPts.push_back(glm::vec2(midX - 2.0f * deltaMax, midY - deltaMax));
    allPts.push_back(glm::vec2(midX + 2.0f * deltaMax, midY - deltaMax));
    allPts.push_back(glm::vec2(midX, midY + 2.0f * deltaMax));

    std::vector<CapTri> dtList;
    dtList.push_back({s0, s1, s2});

    for (int i = 0; i < numPts; ++i) {
        glm::vec2 p = allPts[i];
        std::vector<bool> isBad(dtList.size(), false);

        for (size_t t = 0; t < dtList.size(); ++t) {
            const auto& tri = dtList[t];
            if (inCircumcircle2D(p, allPts[tri.a], allPts[tri.b], allPts[tri.c])) {
                isBad[t] = true;
            }
        }

        struct Edge { int u, v; };
        std::vector<Edge> boundaryEdges;

        for (size_t t = 0; t < dtList.size(); ++t) {
            if (!isBad[t]) continue;
            const auto& tri = dtList[t];
            Edge edges[3] = { {tri.a, tri.b}, {tri.b, tri.c}, {tri.c, tri.a} };
            for (int e = 0; e < 3; ++e) {
                bool shared = false;
                for (size_t t2 = 0; t2 < dtList.size(); ++t2) {
                    if (t == t2 || !isBad[t2]) continue;
                    const auto& tri2 = dtList[t2];
                    Edge edges2[3] = { {tri2.a, tri2.b}, {tri2.b, tri2.c}, {tri2.c, tri2.a} };
                    for (int e2 = 0; e2 < 3; ++e2) {
                        if ((edges[e].u == edges2[e2].u && edges[e].v == edges2[e2].v) ||
                            (edges[e].u == edges2[e2].v && edges[e].v == edges2[e2].u)) {
                            shared = true;
                            break;
                        }
                    }
                    if (shared) break;
                }
                if (!shared) {
                    boundaryEdges.push_back(edges[e]);
                }
            }
        }

        std::vector<CapTri> nextDtList;
        for (size_t t = 0; t < dtList.size(); ++t) {
            if (!isBad[t]) nextDtList.push_back(dtList[t]);
        }
        dtList = std::move(nextDtList);

        for (const auto& edge : boundaryEdges) {
            dtList.push_back({edge.u, edge.v, i});
        }
    }

    for (const auto& tri : dtList) {
        if (tri.a >= numPts || tri.b >= numPts || tri.c >= numPts) continue;

        if (isTriValidForPoly(allPts[tri.a], allPts[tri.b], allPts[tri.c], poly2D)) {
            tris.push_back(tri);
        }
    }

    return tris;
}

static float findEdgeIntersectionParameter2D(
    const glm::vec2& sA,
    const glm::vec2& sB,
    const std::vector<glm::vec2>& poly,
    glm::vec2& outIntersectPt
) {
    glm::vec2 dS = sB - sA;
    float bestT = -1.0f;
    float minT = 1e9f;
    glm::vec2 bestPt = (sA + sB) * 0.5f;
    int n = static_cast<int>(poly.size());

    for (int i = 0; i < n; ++i) {
        glm::vec2 p1 = poly[i];
        glm::vec2 p2 = poly[(i + 1) % n];
        glm::vec2 dP = p2 - p1;

        float denom = dS.x * dP.y - dS.y * dP.x;
        if (std::abs(denom) < 1e-6f) continue;

        glm::vec2 dAP = p1 - sA;
        float t = (dAP.x * dP.y - dAP.y * dP.x) / denom;
        float s = (dAP.x * dS.y - dAP.y * dS.x) / denom;

        if (t >= 0.0f && t <= 1.0f && s >= 0.0f && s <= 1.0f) {
            if (t < minT) {
                minT = t;
                bestT = t;
                bestPt = p1 + s * dP;
            }
        }
    }

    if (bestT >= 0.0f) {
        outIntersectPt = bestPt;
        return bestT;
    }
    outIntersectPt = (sA + sB) * 0.5f;
    return 0.5f;
}

bool TrimTool::execute(
    Mesh* mesh,
    const Camera& camera,
    const std::vector<glm::vec2>& lassoPoints,
    bool isAlt,
    bool useSym,
    SymmetryMode symMode,
    const std::vector<glm::vec3>& symScales,
    const TrimConfig& config
) {
    if (!mesh || mesh->nbVerts == 0 || lassoPoints.size() < 3) {
        sculpt_log("[TrimTool] Invalid input: mesh=%p, lassoPoints=%zu\n", (void*)mesh, lassoPoints.size());
        return false;
    }

    sculpt_log("[TrimTool] Starting Trim operation. Mesh verts=%d, faces=%d, lasso points=%zu, alt=%d, useSym=%d\n",
               mesh->nbVerts, mesh->nbFaces, lassoPoints.size(), isAlt ? 1 : 0, useSym ? 1 : 0);

    // 1. Filter micro-jitter from lasso points
    std::vector<glm::vec2> poly;
    poly.reserve(lassoPoints.size());
    for (const auto& pt : lassoPoints) {
        if (poly.empty() || glm::distance2(pt, poly.back()) >= 4.0f) {
            poly.push_back(pt);
        }
    }

    if (poly.size() < 3) {
        sculpt_log("[TrimTool] Lasso points filtered to less than 3 points. Skipping.\n");
        return false;
    }

    float polyMinX = 1e9f, polyMinY = 1e9f, polyMaxX = -1e9f, polyMaxY = -1e9f;
    for (const auto& pt : poly) {
        polyMinX = std::min(polyMinX, pt.x);
        polyMinY = std::min(polyMinY, pt.y);
        polyMaxX = std::max(polyMaxX, pt.x);
        polyMaxY = std::max(polyMaxY, pt.y);
    }
    sculpt_log("[TrimTool] Lasso 2D AABB: min=(%.1f, %.1f), max=(%.1f, %.1f), Viewport=(%d x %d)\n",
               polyMinX, polyMinY, polyMaxX, polyMaxY, camera.getWidth(), camera.getHeight());

    int oldNbVerts = mesh->nbVerts;
    int oldNbFaces = mesh->nbFaces;

    std::vector<glm::vec3> passes = { glm::vec3(1.0f) };
    if (useSym && !symScales.empty()) {
        for (const auto& s : symScales) passes.push_back(s);
    }

    glm::mat4 viewProj = camera.getProjMatrix() * camera.getViewMatrix();

    // 2. Classify vertices (inside / outside of lasso volume)
    std::vector<uint8_t> removeVert(oldNbVerts, 0);
    int removedVertCount = 0;
    int culledBehindCamCount = 0;
    int insideAabbCount = 0;
    int insidePolyCount = 0;

    float meshProjMinX = 1e9f, meshProjMinY = 1e9f, meshProjMaxX = -1e9f, meshProjMaxY = -1e9f;

    for (int i = 0; i < oldNbVerts; ++i) {
        if (!mesh->vertVisible.empty() && i < (int)mesh->vertVisible.size() && !mesh->vertVisible[i]) {
            removeVert[i] = isAlt ? 1 : 0;
            continue;
        }

        glm::vec3 vLocal(mesh->verts[i * 3], mesh->verts[i * 3 + 1], mesh->verts[i * 3 + 2]);
        bool isInsideAnyPass = false;

        for (const auto& sScale : passes) {
            bool isSymPass = (sScale != glm::vec3(1.0f));
            glm::vec3 vEval = isSymPass ? reflectPointSymmetry(vLocal, sScale, mesh, symMode) : vLocal;
            glm::vec3 vWorld = glm::vec3(mesh->matrix * glm::vec4(vEval, 1.0f));

            glm::vec4 clipPos = viewProj * glm::vec4(vWorld, 1.0f);
            if (clipPos.w <= 0.0001f) {
                culledBehindCamCount++;
                continue; // Behind camera
            }

            glm::vec3 vProj = camera.project(vWorld);
            meshProjMinX = std::min(meshProjMinX, vProj.x);
            meshProjMinY = std::min(meshProjMinY, vProj.y);
            meshProjMaxX = std::max(meshProjMaxX, vProj.x);
            meshProjMaxY = std::max(meshProjMaxY, vProj.y);

            if (vProj.x >= polyMinX && vProj.x <= polyMaxX && vProj.y >= polyMinY && vProj.y <= polyMaxY) {
                insideAabbCount++;
                if (pointInPolygon2D(glm::vec2(vProj.x, vProj.y), poly)) {
                    insidePolyCount++;
                    isInsideAnyPass = true;
                    break;
                }
            }
        }

        bool shouldDelete = isAlt ? !isInsideAnyPass : isInsideAnyPass;
        removeVert[i] = shouldDelete ? 1 : 0;
        if (shouldDelete) removedVertCount++;
    }

    sculpt_log("[TrimTool] Mesh Screen AABB: min=(%.1f, %.1f), max=(%.1f, %.1f)\n",
               meshProjMinX, meshProjMinY, meshProjMaxX, meshProjMaxY);
    sculpt_log("[TrimTool] Vertices inside Lasso AABB: %d, inside Lasso Polygon: %d\n",
               insideAabbCount, insidePolyCount);
    sculpt_log("[TrimTool] Vertex classification: %d removed, %d kept out of %d total (behind cam culled: %d).\n",
               removedVertCount, oldNbVerts - removedVertCount, oldNbVerts, culledBehindCamCount);

    // Print diagnostic samples of kept vs removed vertices
    int loggedRem = 0, loggedKeep = 0;
    for (int i = 0; i < oldNbVerts && (loggedRem < 5 || loggedKeep < 5); ++i) {
        glm::vec3 vLocal(mesh->verts[i * 3], mesh->verts[i * 3 + 1], mesh->verts[i * 3 + 2]);
        glm::vec3 vWorld = glm::vec3(mesh->matrix * glm::vec4(vLocal, 1.0f));
        glm::vec3 vProj = camera.project(vWorld);
        if (removeVert[i] && loggedRem < 5) {
            sculpt_log("[TrimTool] REMOVED sample vert #%d: world=(%.2f, %.2f, %.2f) proj=(%.1f, %.1f, z=%.3f)\n",
                       i, vWorld.x, vWorld.y, vWorld.z, vProj.x, vProj.y, vProj.z);
            loggedRem++;
        } else if (!removeVert[i] && loggedKeep < 5) {
            sculpt_log("[TrimTool] KEPT sample vert #%d: world=(%.2f, %.2f, %.2f) proj=(%.1f, %.1f, z=%.3f)\n",
                       i, vWorld.x, vWorld.y, vWorld.z, vProj.x, vProj.y, vProj.z);
            loggedKeep++;
        }
    }

    if (removedVertCount == 0) {
        sculpt_log("[TrimTool] No vertices marked for trim. Skipping.\n");
        return false;
    }
    if (removedVertCount == oldNbVerts) {
        sculpt_log("[TrimTool] Entire mesh trimmed out.\n");
        mesh->verts.clear();
        mesh->normals.clear();
        mesh->colors.clear();
        mesh->materials.clear();
        mesh->faces.clear();
        mesh->faceGroups.clear();
        mesh->vrfStartCount.clear();
        mesh->nbVerts = 0;
        mesh->nbFaces = 0;
        mesh->postInit();
        mesh->isDirty = true;
        mesh->isVertexDirty = true;
        return true;
    }

    // Dynamic output vertex buffers
    std::vector<float> outVerts = mesh->verts;
    std::vector<float> outNormals = mesh->normals;
    std::vector<float> outColors = mesh->colors;
    std::vector<float> outMaterials = mesh->materials;

    std::map<std::pair<uint32_t, uint32_t>, uint32_t> splitMap;

    auto getOrCreateCutVert = [&](uint32_t keptIdx, uint32_t remIdx) -> uint32_t {
        uint32_t minV = std::min(keptIdx, remIdx);
        uint32_t maxV = std::max(keptIdx, remIdx);
        auto key = std::make_pair(minV, maxV);
        auto it = splitMap.find(key);
        if (it != splitMap.end()) {
            return it->second;
        }

        glm::vec3 uLocal(outVerts[keptIdx * 3], outVerts[keptIdx * 3 + 1], outVerts[keptIdx * 3 + 2]);
        glm::vec3 vLocal(outVerts[remIdx * 3], outVerts[remIdx * 3 + 1], outVerts[remIdx * 3 + 2]);

        glm::vec3 uWorld = glm::vec3(mesh->matrix * glm::vec4(uLocal, 1.0f));
        glm::vec3 vWorld = glm::vec3(mesh->matrix * glm::vec4(vLocal, 1.0f));

        glm::vec3 uProj = camera.project(uWorld);
        glm::vec3 vProj = camera.project(vWorld);
        glm::vec2 sA(uProj.x, uProj.y);
        glm::vec2 sB(vProj.x, vProj.y);
        glm::vec2 intersectPt;
        float t = findEdgeIntersectionParameter2D(sA, sB, poly, intersectPt);
        t = std::clamp(t, 0.001f, 0.999f);

        float zDepth = (1.0f - t) * uProj.z + t * vProj.z;
        float safeZ = std::clamp(zDepth, 0.0001f, 0.9999f);

        glm::vec3 wCut = camera.unproject(intersectPt.x, intersectPt.y, safeZ);
        glm::vec3 newLocal = glm::vec3(glm::inverse(mesh->matrix) * glm::vec4(wCut, 1.0f));

        uint32_t newIdx = static_cast<uint32_t>(outVerts.size() / 3);
        outVerts.push_back(newLocal.x);
        outVerts.push_back(newLocal.y);
        outVerts.push_back(newLocal.z);

        if (!outNormals.empty()) {
            glm::vec3 nu(outNormals[keptIdx * 3], outNormals[keptIdx * 3 + 1], outNormals[keptIdx * 3 + 2]);
            glm::vec3 nv(outNormals[remIdx * 3], outNormals[remIdx * 3 + 1], outNormals[remIdx * 3 + 2]);
            glm::vec3 nNew = glm::normalize((1.0f - t) * nu + t * nv);
            outNormals.push_back(nNew.x);
            outNormals.push_back(nNew.y);
            outNormals.push_back(nNew.z);
        }
        if (!outColors.empty()) {
            glm::vec3 cu(outColors[keptIdx * 3], outColors[keptIdx * 3 + 1], outColors[keptIdx * 3 + 2]);
            glm::vec3 cv(outColors[remIdx * 3], outColors[remIdx * 3 + 1], outColors[remIdx * 3 + 2]);
            glm::vec3 cNew = (1.0f - t) * cu + t * cv;
            outColors.push_back(cNew.x);
            outColors.push_back(cNew.y);
            outColors.push_back(cNew.z);
        }
        if (!outMaterials.empty()) {
            glm::vec3 mu(outMaterials[keptIdx * 3], outMaterials[keptIdx * 3 + 1], outMaterials[keptIdx * 3 + 2]);
            glm::vec3 mv(outMaterials[remIdx * 3], outMaterials[remIdx * 3 + 1], outMaterials[remIdx * 3 + 2]);
            glm::vec3 mNew = (1.0f - t) * mu + t * mv;
            outMaterials.push_back(mNew.x);
            outMaterials.push_back(mNew.y);
            outMaterials.push_back(mNew.z);
        }

        splitMap[key] = newIdx;
        return newIdx;
    };

    std::vector<uint32_t> keptFaces;
    std::vector<uint32_t> keptFaceGroups;

    for (int f = 0; f < oldNbFaces; ++f) {
        if (!mesh->faceVisible.empty() && !mesh->faceVisible[f]) continue;

        uint32_t v0 = mesh->faces[f * 4];
        uint32_t v1 = mesh->faces[f * 4 + 1];
        uint32_t v2 = mesh->faces[f * 4 + 2];
        uint32_t v3 = mesh->faces[f * 4 + 3];

        if (v0 >= (uint32_t)oldNbVerts || v1 >= (uint32_t)oldNbVerts || v2 >= (uint32_t)oldNbVerts) continue;

        uint32_t fg = mesh->faceGroups.empty() ? 0 : mesh->faceGroups[f];
        bool isQuad = (v3 != 0xffffffff && v3 < (uint32_t)oldNbVerts);

        int r0 = removeVert[v0] ? 1 : 0;
        int r1 = removeVert[v1] ? 1 : 0;
        int r2 = removeVert[v2] ? 1 : 0;
        int r3 = isQuad ? (removeVert[v3] ? 1 : 0) : 0;

        int totalRem = r0 + r1 + r2 + (isQuad ? r3 : 0);

        if (totalRem == 0) {
            // Face is completely kept. Preserve original quad/triangle topology intact!
            keptFaces.push_back(v0);
            keptFaces.push_back(v1);
            keptFaces.push_back(v2);
            keptFaces.push_back(isQuad ? v3 : 0xffffffff);
            keptFaceGroups.push_back(fg);
            continue;
        }

        int expectedVerts = isQuad ? 4 : 3;
        if (totalRem == expectedVerts) {
            // Face is completely removed.
            continue;
        }

        // Face is intersected by cut boundary -> split into triangles and cut each triangle
        std::vector<std::vector<uint32_t>> tris;
        tris.push_back({v0, v1, v2});
        if (isQuad) {
            tris.push_back({v0, v2, v3});
        }

        for (const auto& tri : tris) {
            uint32_t a = tri[0], b = tri[1], c = tri[2];
            int remA = removeVert[a] ? 1 : 0;
            int remB = removeVert[b] ? 1 : 0;
            int remC = removeVert[c] ? 1 : 0;
            int numRem = remA + remB + remC;

            if (numRem == 0) {
                keptFaces.push_back(a);
                keptFaces.push_back(b);
                keptFaces.push_back(c);
                keptFaces.push_back(0xffffffff);
                keptFaceGroups.push_back(fg);
            } else if (numRem == 3) {
                continue;
            } else if (numRem == 1) {
                // 1 vertex removed, 2 kept
                uint32_t r = a, k1 = b, k2 = c;
                if (remB) { r = b; k1 = c; k2 = a; }
                else if (remC) { r = c; k1 = a; k2 = b; }

                uint32_t c1 = getOrCreateCutVert(k1, r);
                uint32_t c2 = getOrCreateCutVert(k2, r);

                // Kept quad (k1, k2, c2, c1) split into 2 tris
                keptFaces.push_back(k1); keptFaces.push_back(k2); keptFaces.push_back(c2); keptFaces.push_back(0xffffffff);
                keptFaceGroups.push_back(fg);

                keptFaces.push_back(k1); keptFaces.push_back(c2); keptFaces.push_back(c1); keptFaces.push_back(0xffffffff);
                keptFaceGroups.push_back(fg);
            } else if (numRem == 2) {
                // 2 vertices removed, 1 kept
                uint32_t k = a, r1 = b, r2 = c;
                if (!remB) { k = b; r1 = c; r2 = a; }
                else if (!remC) { k = c; r1 = a; r2 = b; }

                uint32_t c1 = getOrCreateCutVert(k, r1);
                uint32_t c2 = getOrCreateCutVert(k, r2);

                keptFaces.push_back(k); keptFaces.push_back(c1); keptFaces.push_back(c2); keptFaces.push_back(0xffffffff);
                keptFaceGroups.push_back(fg);
            }
        }
    }

    // 3. Hole filling
    if (config.fillHole) {
        float targetCellSize = computeAverageMeshEdgeScreenLength(outVerts, keptFaces, camera, mesh->matrix);
        sculpt_log("[TrimTool] Hole filling target cell size (screen edge length): %.2f px\n", targetCellSize);

        int numKeptFaces = static_cast<int>(keptFaces.size() / 4);
        std::map<std::pair<uint32_t, uint32_t>, int> edgeCount;
        std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>> halfEdgeMap;

        for (int f = 0; f < numKeptFaces; ++f) {
            uint32_t v0 = keptFaces[f * 4];
            uint32_t v1 = keptFaces[f * 4 + 1];
            uint32_t v2 = keptFaces[f * 4 + 2];
            uint32_t v3 = keptFaces[f * 4 + 3];

            uint32_t verts[4] = {v0, v1, v2, v3};
            int count = (v3 == 0xffffffff) ? 3 : 4;

            for (int i = 0; i < count; ++i) {
                uint32_t a = verts[i];
                uint32_t b = verts[(i + 1) % count];
                uint32_t minV = std::min(a, b);
                uint32_t maxV = std::max(a, b);
                edgeCount[{minV, maxV}]++;
                halfEdgeMap[{minV, maxV}] = {a, b};
            }
        }

        std::unordered_multimap<uint32_t, uint32_t> capNextMap;
        std::vector<std::pair<uint32_t, uint32_t>> capHalfEdges;

        for (const auto& kv : edgeCount) {
            if (kv.second == 1) {
                auto dirEdge = halfEdgeMap[kv.first];
                capNextMap.insert({dirEdge.second, dirEdge.first});
                capHalfEdges.push_back({dirEdge.second, dirEdge.first});
            }
        }

        std::set<std::pair<uint32_t, uint32_t>> visitedHE;
        glm::mat4 invMeshMat = glm::inverse(mesh->matrix);

        for (const auto& startHE : capHalfEdges) {
            if (visitedHE.count(startHE)) continue;

            std::vector<uint32_t> loop;
            uint32_t uStart = startHE.first;
            uint32_t curr = uStart;
            uint32_t nextV = startHE.second;

            loop.push_back(curr);

            bool closed = false;
            while (true) {
                visitedHE.insert({curr, nextV});
                loop.push_back(nextV);
                curr = nextV;

                if (curr == uStart) {
                    loop.pop_back();
                    closed = true;
                    break;
                }

                auto range = capNextMap.equal_range(curr);
                uint32_t candidate = 0xffffffff;
                for (auto it = range.first; it != range.second; ++it) {
                    if (!visitedHE.count({curr, it->second})) {
                        candidate = it->second;
                        break;
                    }
                }

                if (candidate == 0xffffffff) break;
                nextV = candidate;
            }

            if (!closed || loop.size() < 3) continue;

            // 1. Clean boundary loop and project to screen space & depth
            std::vector<uint32_t> cleanLoop;
            for (uint32_t vid : loop) {
                if (cleanLoop.empty() || cleanLoop.back() != vid) {
                    cleanLoop.push_back(vid);
                }
            }
            if (cleanLoop.size() > 2 && cleanLoop.front() == cleanLoop.back()) {
                cleanLoop.pop_back();
            }
            if (cleanLoop.size() < 3) continue;

            int k = static_cast<int>(cleanLoop.size());
            std::vector<glm::vec3> poly3D(k);
            std::vector<glm::vec2> screenLoop(k);
            std::vector<float> depthLoop(k);

            float loopPerimeter = 0.0f;
            for (int i = 0; i < k; ++i) {
                uint32_t vid = cleanLoop[i];
                poly3D[i] = glm::vec3(outVerts[vid * 3], outVerts[vid * 3 + 1], outVerts[vid * 3 + 2]);
                glm::vec3 wPos = glm::vec3(mesh->matrix * glm::vec4(poly3D[i], 1.0f));
                glm::vec3 proj = camera.project(wPos);
                screenLoop[i] = glm::vec2(proj.x, proj.y);
                depthLoop[i] = proj.z;
            }

            for (int i = 0; i < k; ++i) {
                loopPerimeter += glm::distance(screenLoop[i], screenLoop[(i + 1) % k]);
            }
            float avgBoundaryEdgeLen = loopPerimeter / static_cast<float>(k);
            float gridStep = std::clamp(avgBoundaryEdgeLen * 1.5f, 4.0f, targetCellSize);

            // 2. Perform watertight Ear-Clipping of boundary loop (guaranteed N-2 triangles)
            std::vector<CapTri> capTris = earClipPolygon2D(screenLoop);

            // 3. Generate adaptive interior grid points
            std::vector<glm::vec2> interiorGrid = generateQuincunxGrid(screenLoop, gridStep);

            // 4. Create 3D interior vertices for each 2D grid point via MVC interpolation
            std::vector<uint32_t> interiorMeshVids;
            interiorMeshVids.reserve(interiorGrid.size());

            for (const auto& p2D : interiorGrid) {
                std::vector<float> weights = computeMeanValueWeights(p2D, screenLoop);
                float z = 0.0f;
                for (int i = 0; i < k; ++i) {
                    z += weights[i] * depthLoop[i];
                }
                float safeZ = std::clamp(z, 0.0001f, 0.9999f);

                glm::vec3 wPos = camera.unproject(p2D.x, p2D.y, safeZ);
                glm::vec3 lPos = glm::vec3(invMeshMat * glm::vec4(wPos, 1.0f));

                uint32_t newVid = static_cast<uint32_t>(outVerts.size() / 3);
                outVerts.push_back(lPos.x);
                outVerts.push_back(lPos.y);
                outVerts.push_back(lPos.z);

                if (!outNormals.empty()) {
                    glm::vec3 nSum(0.0f);
                    for (int i = 0; i < k; ++i) {
                        uint32_t bVid = cleanLoop[i];
                        nSum += weights[i] * glm::vec3(outNormals[bVid * 3], outNormals[bVid * 3 + 1], outNormals[bVid * 3 + 2]);
                    }
                    if (glm::length2(nSum) > 1e-5f) nSum = glm::normalize(nSum);
                    else nSum = glm::vec3(0.0f, 0.0f, 1.0f);
                    outNormals.push_back(nSum.x); outNormals.push_back(nSum.y); outNormals.push_back(nSum.z);
                }
                if (!outColors.empty()) {
                    glm::vec3 cSum(0.0f);
                    for (int i = 0; i < k; ++i) {
                        uint32_t bVid = cleanLoop[i];
                        cSum += weights[i] * glm::vec3(outColors[bVid * 3], outColors[bVid * 3 + 1], outColors[bVid * 3 + 2]);
                    }
                    outColors.push_back(cSum.x); outColors.push_back(cSum.y); outColors.push_back(cSum.z);
                }
                if (!outMaterials.empty()) {
                    glm::vec3 mSum(0.0f);
                    for (int i = 0; i < k; ++i) {
                        uint32_t bVid = cleanLoop[i];
                        mSum += weights[i] * glm::vec3(outMaterials[bVid * 3], outMaterials[bVid * 3 + 1], outMaterials[bVid * 3 + 2]);
                    }
                    outMaterials.push_back(mSum.x); outMaterials.push_back(mSum.y); outMaterials.push_back(mSum.z);
                }

                interiorMeshVids.push_back(newVid);
            }

            // 5. Insert interior grid points into cap triangles by splitting
            std::vector<glm::vec2> allPts2D = screenLoop;
            allPts2D.insert(allPts2D.end(), interiorGrid.begin(), interiorGrid.end());

            auto pointInTriangle2D = [](const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
                auto cross = [](const glm::vec2& p1, const glm::vec2& p2, const glm::vec2& p3) {
                    return (p2.x - p1.x) * (p3.y - p1.y) - (p2.y - p1.y) * (p3.x - p1.x);
                };
                float c1 = cross(a, b, p);
                float c2 = cross(b, c, p);
                float c3 = cross(c, a, p);
                bool has_neg = (c1 < -1e-4f) || (c2 < -1e-4f) || (c3 < -1e-4f);
                bool has_pos = (c1 > 1e-4f) || (c2 > 1e-4f) || (c3 > 1e-4f);
                return !(has_neg && has_pos);
            };

            for (size_t g = 0; g < interiorGrid.size(); ++g) {
                int gIdx = k + static_cast<int>(g);
                glm::vec2 gPt = interiorGrid[g];
                int targetTri = -1;
                for (size_t t = 0; t < capTris.size(); ++t) {
                    if (pointInTriangle2D(gPt, allPts2D[capTris[t].a], allPts2D[capTris[t].b], allPts2D[capTris[t].c])) {
                        targetTri = static_cast<int>(t);
                        break;
                    }
                }

                if (targetTri != -1) {
                    CapTri oldT = capTris[targetTri];
                    capTris[targetTri] = {oldT.a, oldT.b, gIdx};
                    capTris.push_back({oldT.b, oldT.c, gIdx});
                    capTris.push_back({oldT.c, oldT.a, gIdx});
                }
            }

            // 6. Laplacian Smoothing on interior 3D vertices
            int numInterior = static_cast<int>(interiorMeshVids.size());
            if (numInterior > 0) {
                std::vector<std::vector<uint32_t>> adj(numInterior);
                for (const auto& tri : capTris) {
                    uint32_t tVerts[3] = {
                        (tri.a < k) ? cleanLoop[tri.a] : interiorMeshVids[tri.a - k],
                        (tri.b < k) ? cleanLoop[tri.b] : interiorMeshVids[tri.b - k],
                        (tri.c < k) ? cleanLoop[tri.c] : interiorMeshVids[tri.c - k]
                    };

                    int tIndices[3] = {tri.a, tri.b, tri.c};
                    for (int e = 0; e < 3; ++e) {
                        int uIdx = tIndices[e];
                        int vIdx = tIndices[(e + 1) % 3];
                        if (uIdx >= k) {
                            adj[uIdx - k].push_back(tVerts[(e + 1) % 3]);
                        }
                        if (vIdx >= k) {
                            adj[vIdx - k].push_back(tVerts[e]);
                        }
                    }
                }

                for (int iter = 0; iter < 4; ++iter) {
                    for (int i = 0; i < numInterior; ++i) {
                        if (adj[i].empty()) continue;
                        glm::vec3 avgPos(0.0f);
                        for (uint32_t nVid : adj[i]) {
                            avgPos += glm::vec3(outVerts[nVid * 3], outVerts[nVid * 3 + 1], outVerts[nVid * 3 + 2]);
                        }
                        avgPos /= static_cast<float>(adj[i].size());
                        uint32_t inVid = interiorMeshVids[i];
                        outVerts[inVid * 3]     = 0.5f * outVerts[inVid * 3]     + 0.5f * avgPos.x;
                        outVerts[inVid * 3 + 1] = 0.5f * outVerts[inVid * 3 + 1] + 0.5f * avgPos.y;
                        outVerts[inVid * 3 + 2] = 0.5f * outVerts[inVid * 3 + 2] + 0.5f * avgPos.z;
                    }
                }
            }

            auto getMeshVid = [&](int idx) -> uint32_t {
                if (idx < k) return cleanLoop[idx];
                return interiorMeshVids[idx - k];
            };

            for (const auto& tri : capTris) {
                uint32_t v0 = getMeshVid(tri.a);
                uint32_t v1 = getMeshVid(tri.b);
                uint32_t v2 = getMeshVid(tri.c);

                keptFaces.push_back(v0);
                keptFaces.push_back(v1);
                keptFaces.push_back(v2);
                keptFaces.push_back(0xffffffff);
                keptFaceGroups.push_back(0);
            }

            sculpt_log("[TrimTool] Screen-space hole closed: boundary loop size=%d, interior verts added=%zu, cap tris=%zu\n",
                       k, interiorMeshVids.size(), capTris.size());
        }
    }

    // 4. Recompact mesh
    std::vector<int32_t> vertMap(outVerts.size() / 3, -1);
    int compactVertCount = 0;
    for (uint32_t vid : keptFaces) {
        if (vid != 0xffffffff && vertMap[vid] == -1) {
            vertMap[vid] = compactVertCount++;
        }
    }

    int keptFacesCount = static_cast<int>(keptFaces.size() / 4);

    sculpt_log("[TrimTool] Recompacting mesh: %d verts kept (from %zu total), %d faces kept (from %d orig tris/quads).\n",
               compactVertCount, outVerts.size() / 3, keptFacesCount, oldNbFaces);

    if (compactVertCount == 0 || keptFacesCount == 0) {
        sculpt_log("[TrimTool] Entire mesh trimmed out.\n");
        mesh->verts.clear();
        mesh->normals.clear();
        mesh->colors.clear();
        mesh->materials.clear();
        mesh->faces.clear();
        mesh->faceGroups.clear();
        mesh->vrfStartCount.clear();
        mesh->nbVerts = 0;
        mesh->nbFaces = 0;
        mesh->postInit();
        mesh->isDirty = true;
        mesh->isVertexDirty = true;
        return true;
    }

    std::vector<float> finalVerts(compactVertCount * 3);
    std::vector<float> finalNormals(outNormals.empty() ? 0 : compactVertCount * 3);
    std::vector<float> finalColors(outColors.empty() ? 0 : compactVertCount * 3);
    std::vector<float> finalMaterials(outMaterials.empty() ? 0 : compactVertCount * 3);

    for (size_t i = 0; i < vertMap.size(); ++i) {
        int newIdx = vertMap[i];
        if (newIdx == -1) continue;

        finalVerts[newIdx * 3]     = outVerts[i * 3];
        finalVerts[newIdx * 3 + 1] = outVerts[i * 3 + 1];
        finalVerts[newIdx * 3 + 2] = outVerts[i * 3 + 2];

        if (!outNormals.empty()) {
            finalNormals[newIdx * 3]     = outNormals[i * 3];
            finalNormals[newIdx * 3 + 1] = outNormals[i * 3 + 1];
            finalNormals[newIdx * 3 + 2] = outNormals[i * 3 + 2];
        }
        if (!outColors.empty()) {
            finalColors[newIdx * 3]     = outColors[i * 3];
            finalColors[newIdx * 3 + 1] = outColors[i * 3 + 1];
            finalColors[newIdx * 3 + 2] = outColors[i * 3 + 2];
        }
        if (!outMaterials.empty()) {
            finalMaterials[newIdx * 3]     = outMaterials[i * 3];
            finalMaterials[newIdx * 3 + 1] = outMaterials[i * 3 + 1];
            finalMaterials[newIdx * 3 + 2] = outMaterials[i * 3 + 2];
        }
    }

    std::vector<uint32_t> finalFaces(keptFaces.size());
    for (size_t i = 0; i < keptFaces.size(); ++i) {
        uint32_t vid = keptFaces[i];
        finalFaces[i] = (vid == 0xffffffff) ? 0xffffffff : static_cast<uint32_t>(vertMap[vid]);
    }

    mesh->verts = std::move(finalVerts);
    mesh->normals = std::move(finalNormals);
    mesh->colors = std::move(finalColors);
    mesh->materials = std::move(finalMaterials);
    mesh->faces = std::move(finalFaces);
    mesh->faceGroups = std::move(keptFaceGroups);
    mesh->vrfStartCount.clear(); // Force topology rebuild in postInit()
    mesh->nbVerts = compactVertCount;
    mesh->nbFaces = keptFacesCount;

    // Reset visibility arrays to prevent stale data from hiding faces in renderer
    mesh->vertVisible.assign(compactVertCount, 1);
    mesh->faceVisible.assign(keptFacesCount, 1);

    // Rebuild topology, normals, face boxes, and octree
    mesh->postInit();

    mesh->isDirty = true;
    mesh->isVertexDirty = true;
    mesh->invalidateLocalRadius();

    sculpt_log("[TrimTool] Trim operation completed successfully. New mesh: verts=%d, faces=%d\n",
               mesh->nbVerts, mesh->nbFaces);

    return true;
}

