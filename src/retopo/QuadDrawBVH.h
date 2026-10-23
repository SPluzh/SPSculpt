#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include "mesh/Mesh.h"
#include "common/Constants.h"
#include "common/Logger.h"

namespace retopo {

static constexpr float BVH_MAXRANGE = 1e30f;

struct BVHAABB {
    glm::vec3 minBound;
    glm::vec3 maxBound;

    BVHAABB()
        : minBound(BVH_MAXRANGE, BVH_MAXRANGE, BVH_MAXRANGE),
          maxBound(-BVH_MAXRANGE, -BVH_MAXRANGE, -BVH_MAXRANGE) {}

    BVHAABB(const glm::vec3& mn, const glm::vec3& mx)
        : minBound(mn), maxBound(mx) {}

    void Expand(const glm::vec3& p) {
        minBound.x = std::min(minBound.x, p.x);
        minBound.y = std::min(minBound.y, p.y);
        minBound.z = std::min(minBound.z, p.z);
        maxBound.x = std::max(maxBound.x, p.x);
        maxBound.y = std::max(maxBound.y, p.y);
        maxBound.z = std::max(maxBound.z, p.z);
    }

    void Expand(const BVHAABB& b) {
        minBound.x = std::min(minBound.x, b.minBound.x);
        minBound.y = std::min(minBound.y, b.minBound.y);
        minBound.z = std::min(minBound.z, b.minBound.z);
        maxBound.x = std::max(maxBound.x, b.maxBound.x);
        maxBound.y = std::max(maxBound.y, b.maxBound.y);
        maxBound.z = std::max(maxBound.z, b.maxBound.z);
    }

    float SqrDistanceToPoint(const glm::vec3& p) const {
        float sqDist = 0.0f;
        if (p.x < minBound.x) { float d = minBound.x - p.x; sqDist += d * d; }
        else if (p.x > maxBound.x) { float d = p.x - maxBound.x; sqDist += d * d; }

        if (p.y < minBound.y) { float d = minBound.y - p.y; sqDist += d * d; }
        else if (p.y > maxBound.y) { float d = p.y - maxBound.y; sqDist += d * d; }

        if (p.z < minBound.z) { float d = minBound.z - p.z; sqDist += d * d; }
        else if (p.z > maxBound.z) { float d = p.z - maxBound.z; sqDist += d * d; }

        return sqDist;
    }

    bool IntersectRay(const glm::vec3& orig, const glm::vec3& dir, float& tMinOut, float& tMaxOut) const {
        float tmin = -BVH_MAXRANGE;
        float tmax = BVH_MAXRANGE;

        for (int i = 0; i < 3; ++i) {
            float o = orig[i];
            float d = dir[i];
            float mn = minBound[i];
            float mx = maxBound[i];

            if (std::abs(d) < 1e-9f) {
                if (o < mn || o > mx) return false;
            } else {
                float invD = 1.0f / d;
                float t1 = (mn - o) * invD;
                float t2 = (mx - o) * invD;
                if (t1 > t2) std::swap(t1, t2);
                tmin = std::max(tmin, t1);
                tmax = std::min(tmax, t2);
                if (tmin > tmax) return false;
            }
        }
        tMinOut = tmin;
        tMaxOut = tmax;
        return true;
    }
};

struct BVHTriangle {
    glm::vec3 v0;
    glm::vec3 v1;
    glm::vec3 v2;
};

struct BVHBuildPrim {
    BVHAABB aabb;
    glm::vec3 centroid;
    int32_t triIndex = 0;
};

// Compact node: exactly 64 bytes (1 CPU cache line)
struct alignas(64) BVHNode {
    BVHAABB bbox;        // 24 bytes (2 x glm::vec3)
    int32_t leftChild = -1; // 4 bytes: if triCount == 0, leftChild and leftChild + 1
    int32_t firstTri = -1;  // 4 bytes: if triCount > 0, start into _triangles
    int32_t triCount = 0;   // 4 bytes: 0 for internal, > 0 for leaf
    int32_t pad[7] = {0};   // 28 bytes padding to fill 64 bytes
};

class QuadDrawBVH {
public:
    QuadDrawBVH() = default;

    void Clear() {
        m_triangles.clear();
        m_nodes.clear();
        m_nodeCount = 0;
        m_isBuilt = false;
        m_sourceMesh = nullptr;
        m_meshVertGen = 0;
    }

    bool IsBuilt() const { return m_isBuilt; }
    int32_t GetTriangleCount() const { return (int32_t)m_triangles.size(); }
    int32_t GetNodeCount() const { return m_nodeCount; }
    const Mesh* GetSourceMesh() const { return m_sourceMesh; }

    static glm::vec3 ClosestPointOnTriangle(
        const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
        glm::vec3& outNormal
    ) {
        glm::vec3 ab = b - a;
        glm::vec3 ac = c - a;
        glm::vec3 ap = p - a;

        float d1 = glm::dot(ab, ap);
        float d2 = glm::dot(ac, ap);
        if (d1 <= 0.0f && d2 <= 0.0f) {
            outNormal = glm::cross(ab, ac);
            return a;
        }

        glm::vec3 bp = p - b;
        float d3 = glm::dot(ab, bp);
        float d4 = glm::dot(ac, bp);
        if (d3 >= 0.0f && d4 <= d3) {
            outNormal = glm::cross(ab, ac);
            return b;
        }

        float vc = d1 * d4 - d3 * d2;
        if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
            float v = d1 / (d1 - d3);
            outNormal = glm::cross(ab, ac);
            return a + ab * v;
        }

        glm::vec3 cp = p - c;
        float d5 = glm::dot(ab, cp);
        float d6 = glm::dot(ac, cp);
        if (d6 >= 0.0f && d5 <= d6) {
            outNormal = glm::cross(ab, ac);
            return c;
        }

        float vb = d5 * d2 - d1 * d6;
        if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
            float w = d2 / (d2 - d6);
            outNormal = glm::cross(ab, ac);
            return a + ac * w;
        }

        float va = d3 * d6 - d5 * d4;
        if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
            float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            outNormal = glm::cross(ab, ac);
            return b + (c - b) * w;
        }

        float denom = 1.0f / (va + vb + vc);
        float v = vb * denom;
        float w = vc * denom;
        outNormal = glm::cross(ab, ac);
        return a + ab * v + ac * w;
    }

    static bool RayTriangleIntersect(
        const glm::vec3& orig, const glm::vec3& dir,
        const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
        float& outT, glm::vec3& outNormal, bool bidirectional
    ) {
        const float eps = 1e-12f;
        glm::vec3 edge1 = v1 - v0;
        glm::vec3 edge2 = v2 - v0;
        glm::vec3 pvec = glm::cross(dir, edge2);
        float det = glm::dot(edge1, pvec);

        if (std::abs(det) < eps) return false;

        float invDet = 1.0f / det;
        glm::vec3 tvec = orig - v0;
        float u = glm::dot(tvec, pvec) * invDet;
        if (u < -1e-4f || u > 1.0001f) return false;

        glm::vec3 qvec = glm::cross(tvec, edge1);
        float v = glm::dot(dir, qvec) * invDet;
        if (v < -1e-4f || u + v > 1.0001f) return false;

        float t = glm::dot(edge2, qvec) * invDet;
        if (!bidirectional && t < 1e-5f) return false;

        outT = t;
        outNormal = glm::cross(edge1, edge2);
        float lenSq = glm::dot(outNormal, outNormal);
        if (lenSq > 1e-12f) {
            outNormal = glm::normalize(outNormal);
        }

        if (glm::dot(outNormal, dir) > 0.0f) {
            outNormal = -outNormal;
        }

        return true;
    }

    bool Build(const Mesh* targetMesh) {
        Clear();
        if (!targetMesh || targetMesh->nbFaces <= 0 || targetMesh->nbVerts <= 0 ||
            targetMesh->faces.size() < (size_t)targetMesh->nbFaces * 4 ||
            targetMesh->verts.size() < (size_t)targetMesh->nbVerts * 3) {
            return false;
        }

        const int numFaces = targetMesh->nbFaces;
        const int numVerts = targetMesh->nbVerts;
        const auto& faces = targetMesh->faces;
        const auto& verts = targetMesh->verts;
        const glm::mat4 mat = targetMesh->matrix * targetMesh->editMatrix;
        const bool hasTransform = (mat != glm::mat4(1.0f));

        int totalEstTris = 0;
        for (int i = 0; i < numFaces; ++i) {
            totalEstTris += (faces[i * 4 + 3] == TRI_INDEX) ? 1 : 2;
        }
        if (totalEstTris <= 0) return false;

        std::vector<BVHTriangle> srcTriangles(totalEstTris);
        std::vector<BVHBuildPrim> buildPrims(totalEstTris);

        auto getVertWorld = [&](uint32_t idx) -> glm::vec3 {
            glm::vec3 p(verts[idx * 3], verts[idx * 3 + 1], verts[idx * 3 + 2]);
            return hasTransform ? glm::vec3(mat * glm::vec4(p, 1.0f)) : p;
        };

        int triIdx = 0;
        BVHAABB rootBbox;
        BVHAABB rootCentroidBbox;

        for (int i = 0; i < numFaces; ++i) {
            uint32_t a = faces[i * 4 + 0];
            uint32_t b = faces[i * 4 + 1];
            uint32_t c = faces[i * 4 + 2];
            uint32_t d = faces[i * 4 + 3];

            if (a >= (uint32_t)numVerts || b >= (uint32_t)numVerts || c >= (uint32_t)numVerts)
                continue;

            glm::vec3 v0 = getVertWorld(a);
            glm::vec3 v1 = getVertWorld(b);
            glm::vec3 v2 = getVertWorld(c);

            // First triangle (a, b, c)
            {
                BVHTriangle& tri = srcTriangles[triIdx];
                tri.v0 = v0;
                tri.v1 = v1;
                tri.v2 = v2;

                BVHBuildPrim& prim = buildPrims[triIdx];
                prim.triIndex = triIdx;
                prim.centroid = (v0 + v1 + v2) * (1.0f / 3.0f);
                prim.aabb.minBound = prim.aabb.maxBound = v0;
                prim.aabb.Expand(v1);
                prim.aabb.Expand(v2);

                rootBbox.Expand(prim.aabb);
                rootCentroidBbox.Expand(prim.centroid);

                triIdx++;
            }

            // Second triangle (a, c, d) if quad
            if (d != TRI_INDEX && d < (uint32_t)numVerts) {
                glm::vec3 v3 = getVertWorld(d);

                BVHTriangle& tri = srcTriangles[triIdx];
                tri.v0 = v0;
                tri.v1 = v2;
                tri.v2 = v3;

                BVHBuildPrim& prim = buildPrims[triIdx];
                prim.triIndex = triIdx;
                prim.centroid = (v0 + v2 + v3) * (1.0f / 3.0f);
                prim.aabb.minBound = prim.aabb.maxBound = v0;
                prim.aabb.Expand(v2);
                prim.aabb.Expand(v3);

                rootBbox.Expand(prim.aabb);
                rootCentroidBbox.Expand(prim.centroid);

                triIdx++;
            }
        }

        int totalTris = triIdx;
        if (totalTris <= 0) return false;

        srcTriangles.resize(totalTris);
        buildPrims.resize(totalTris);

        int maxNodes = totalTris * 2 + 1;
        m_nodes.resize(maxNodes);
        m_nodeCount = 1;

        SubdivideNode(0, 0, totalTris, 0, rootBbox, rootCentroidBbox, buildPrims);

        m_nodes.resize(m_nodeCount);

        m_triangles.resize(totalTris);
        for (int i = 0; i < totalTris; ++i) {
            m_triangles[i] = srcTriangles[buildPrims[i].triIndex];
        }

        m_isBuilt = true;
        m_sourceMesh = targetMesh;
        m_meshVertGen = targetMesh->getVertsGeneration();
        sculpt_log("[QuadDrawBVH] Built BVH for '%s': %d triangles, %d nodes, bounds [(%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f)]\n",
            targetMesh->outlinerName.c_str(), totalTris, m_nodeCount,
            rootBbox.minBound.x, rootBbox.minBound.y, rootBbox.minBound.z,
            rootBbox.maxBound.x, rootBbox.maxBound.y, rootBbox.maxBound.z);
        return true;
    }

    bool NeedsRebuild(const Mesh* targetMesh) const {
        if (!m_isBuilt || m_sourceMesh != targetMesh || !targetMesh) return true;
        if (m_meshVertGen != targetMesh->getVertsGeneration()) return true;
        return false;
    }

    bool FindClosestPoint(
        const glm::vec3& queryPoint, glm::vec3& outClosestPoint, glm::vec3& outNormal,
        float maxDist = BVH_MAXRANGE
    ) const {
        if (!m_isBuilt || m_nodeCount == 0 || m_triangles.empty()) return false;

        float bestDistSq = (maxDist < BVH_MAXRANGE) ? (maxDist * maxDist) : (BVH_MAXRANGE * BVH_MAXRANGE);
        glm::vec3 bestPoint(0.0f);
        glm::vec3 bestNormal(0.0f, 1.0f, 0.0f);
        bool found = false;

        int32_t stack[64];
        int32_t stackPtr = 0;
        stack[stackPtr++] = 0;

        while (stackPtr > 0) {
            int32_t nodeIdx = stack[--stackPtr];
            const BVHNode& node = m_nodes[nodeIdx];

            if (node.bbox.SqrDistanceToPoint(queryPoint) >= bestDistSq) continue;

            if (node.triCount > 0) {
                for (int32_t i = 0; i < node.triCount; ++i) {
                    const BVHTriangle& tri = m_triangles[node.firstTri + i];
                    glm::vec3 n;
                    glm::vec3 pt = ClosestPointOnTriangle(queryPoint, tri.v0, tri.v1, tri.v2, n);
                    glm::vec3 diff = queryPoint - pt;
                    float dSq = glm::dot(diff, diff);
                    if (dSq < bestDistSq) {
                        bestDistSq = dSq;
                        bestPoint = pt;
                        bestNormal = n;
                        found = true;
                    }
                }
            } else {
                int32_t leftIdx = node.leftChild;
                int32_t rightIdx = node.leftChild + 1;

                float distLeft = m_nodes[leftIdx].bbox.SqrDistanceToPoint(queryPoint);
                float distRight = m_nodes[rightIdx].bbox.SqrDistanceToPoint(queryPoint);

                if (distLeft < distRight) {
                    if (distRight < bestDistSq && stackPtr < 63) stack[stackPtr++] = rightIdx;
                    if (distLeft < bestDistSq && stackPtr < 63) stack[stackPtr++] = leftIdx;
                } else {
                    if (distLeft < bestDistSq && stackPtr < 63) stack[stackPtr++] = leftIdx;
                    if (distRight < bestDistSq && stackPtr < 63) stack[stackPtr++] = rightIdx;
                }
            }
        }

        if (found) {
            outClosestPoint = bestPoint;
            float nLenSq = glm::dot(bestNormal, bestNormal);
            outNormal = (nLenSq > 1e-10f) ? glm::normalize(bestNormal) : glm::vec3(0.0f, 1.0f, 0.0f);
            return true;
        }
        return false;
    }

    bool FindNearestVertex(const glm::vec3& queryPoint, glm::vec3& outVertex, float maxDist = BVH_MAXRANGE) const {
        if (!m_isBuilt || m_nodeCount == 0 || m_triangles.empty()) return false;

        float bestDistSq = (maxDist < BVH_MAXRANGE) ? (maxDist * maxDist) : (BVH_MAXRANGE * BVH_MAXRANGE);
        glm::vec3 bestVert(0.0f);
        bool found = false;

        int32_t stack[64];
        int32_t stackPtr = 0;
        stack[stackPtr++] = 0;

        while (stackPtr > 0) {
            int32_t nodeIdx = stack[--stackPtr];
            const BVHNode& node = m_nodes[nodeIdx];

            if (node.bbox.SqrDistanceToPoint(queryPoint) >= bestDistSq) continue;

            if (node.triCount > 0) {
                for (int32_t i = 0; i < node.triCount; ++i) {
                    const BVHTriangle& tri = m_triangles[node.firstTri + i];

                    float d0 = glm::dot(queryPoint - tri.v0, queryPoint - tri.v0);
                    if (d0 < bestDistSq) { bestDistSq = d0; bestVert = tri.v0; found = true; }

                    float d1 = glm::dot(queryPoint - tri.v1, queryPoint - tri.v1);
                    if (d1 < bestDistSq) { bestDistSq = d1; bestVert = tri.v1; found = true; }

                    float d2 = glm::dot(queryPoint - tri.v2, queryPoint - tri.v2);
                    if (d2 < bestDistSq) { bestDistSq = d2; bestVert = tri.v2; found = true; }
                }
            } else {
                int32_t leftIdx = node.leftChild;
                int32_t rightIdx = node.leftChild + 1;

                float distLeft = m_nodes[leftIdx].bbox.SqrDistanceToPoint(queryPoint);
                float distRight = m_nodes[rightIdx].bbox.SqrDistanceToPoint(queryPoint);

                if (distLeft < distRight) {
                    if (distRight < bestDistSq && stackPtr < 63) stack[stackPtr++] = rightIdx;
                    if (distLeft < bestDistSq && stackPtr < 63) stack[stackPtr++] = leftIdx;
                } else {
                    if (distLeft < bestDistSq && stackPtr < 63) stack[stackPtr++] = leftIdx;
                    if (distRight < bestDistSq && stackPtr < 63) stack[stackPtr++] = rightIdx;
                }
            }
        }

        if (found) {
            outVertex = bestVert;
            return true;
        }
        return false;
    }

    bool Raycast(
        const glm::vec3& orig, const glm::vec3& dir,
        glm::vec3& outHitPoint, glm::vec3& outNormal,
        float maxDist = BVH_MAXRANGE, bool bidirectional = false
    ) const {
        if (!m_isBuilt || m_nodeCount == 0 || m_triangles.empty()) return false;

        float bestDist = (maxDist > 0.0f && maxDist < BVH_MAXRANGE) ? maxDist : BVH_MAXRANGE;
        glm::vec3 bestPoint(0.0f);
        glm::vec3 bestNormal(0.0f, 1.0f, 0.0f);
        bool found = false;

        float dirLenSq = glm::dot(dir, dir);
        if (dirLenSq < 1e-12f) return false;
        glm::vec3 rayDir = glm::normalize(dir);

        int32_t stack[64];
        int32_t stackPtr = 0;
        stack[stackPtr++] = 0;

        while (stackPtr > 0) {
            int32_t nodeIdx = stack[--stackPtr];
            const BVHNode& node = m_nodes[nodeIdx];

            float tmin, tmax;
            if (!node.bbox.IntersectRay(orig, rayDir, tmin, tmax)) continue;

            if (!bidirectional && tmax < 0.0f) continue;
            if (!bidirectional && tmin > bestDist) continue;

            if (bidirectional) {
                float boxMinAbsDist = 0.0f;
                if (tmin > 0.0f) boxMinAbsDist = tmin;
                else if (tmax < 0.0f) boxMinAbsDist = -tmax;
                if (boxMinAbsDist > bestDist) continue;
            }

            if (node.triCount > 0) {
                for (int32_t i = 0; i < node.triCount; ++i) {
                    const BVHTriangle& tri = m_triangles[node.firstTri + i];
                    float hitT;
                    glm::vec3 triN;
                    if (RayTriangleIntersect(orig, rayDir, tri.v0, tri.v1, tri.v2, hitT, triN, bidirectional)) {
                        float absDist = std::abs(hitT);
                        if (absDist < bestDist) {
                            bestDist = absDist;
                            bestPoint = orig + rayDir * hitT;
                            bestNormal = triN;
                            found = true;
                        }
                    }
                }
            } else {
                int32_t leftIdx = node.leftChild;
                int32_t rightIdx = node.leftChild + 1;

                float tminL, tmaxL, tminR, tmaxR;
                bool hitL = m_nodes[leftIdx].bbox.IntersectRay(orig, rayDir, tminL, tmaxL);
                bool hitR = m_nodes[rightIdx].bbox.IntersectRay(orig, rayDir, tminR, tmaxR);

                if (hitL && hitR) {
                    float distL = (tminL > 0.0f) ? tminL : 0.0f;
                    float distR = (tminR > 0.0f) ? tminR : 0.0f;
                    if (distL < distR) {
                        if (stackPtr < 63) stack[stackPtr++] = rightIdx;
                        if (stackPtr < 63) stack[stackPtr++] = leftIdx;
                    } else {
                        if (stackPtr < 63) stack[stackPtr++] = leftIdx;
                        if (stackPtr < 63) stack[stackPtr++] = rightIdx;
                    }
                } else if (hitL) {
                    if (stackPtr < 63) stack[stackPtr++] = leftIdx;
                } else if (hitR) {
                    if (stackPtr < 63) stack[stackPtr++] = rightIdx;
                }
            }
        }

        if (found) {
            outHitPoint = bestPoint;
            outNormal = bestNormal;
            return true;
        }
        return false;
    }

    bool QueryPlaneTriangles(
        const glm::vec3& planeNormal, float planeD,
        const glm::vec3& sphereCenter, float maxRadius,
        std::vector<BVHTriangle>& outTriangles
    ) const {
        if (!m_isBuilt || m_nodeCount == 0 || m_triangles.empty()) return false;

        float maxRadiusSq = maxRadius * maxRadius;
        glm::vec3 pNorm = glm::normalize(planeNormal);

        int32_t stack[64];
        int32_t stackPtr = 0;
        stack[stackPtr++] = 0;

        while (stackPtr > 0) {
            int32_t currIdx = stack[--stackPtr];
            const auto& node = m_nodes[currIdx];

            if (maxRadius > 0.0f) {
                if (node.bbox.SqrDistanceToPoint(sphereCenter) > maxRadiusSq) {
                    continue;
                }
            }

            glm::vec3 bCenter = (node.bbox.minBound + node.bbox.maxBound) * 0.5f;
            glm::vec3 bExt = (node.bbox.maxBound - node.bbox.minBound) * 0.5f;
            float r = bExt.x * std::abs(pNorm.x) + bExt.y * std::abs(pNorm.y) + bExt.z * std::abs(pNorm.z);
            float dist = glm::dot(pNorm, bCenter) + planeD;
            if (std::abs(dist) > r) {
                continue;
            }

            if (node.triCount > 0) {
                int32_t endTri = node.firstTri + node.triCount;
                for (int32_t t = node.firstTri; t < endTri; ++t) {
                    outTriangles.push_back(m_triangles[t]);
                }
            } else {
                int32_t leftIdx = node.leftChild;
                int32_t rightIdx = node.leftChild + 1;
                if (stackPtr < 63) stack[stackPtr++] = leftIdx;
                if (stackPtr < 63) stack[stackPtr++] = rightIdx;
            }
        }
        return !outTriangles.empty();
    }

private:
    void SubdivideNode(
        int32_t nodeIdx, int32_t first, int32_t count, int32_t depth,
        const BVHAABB& bbox, const BVHAABB& centroidBbox,
        std::vector<BVHBuildPrim>& buildPrims
    ) {
        m_nodes[nodeIdx].bbox = bbox;

        if (count <= 4 || depth >= 30 || centroidBbox.minBound == centroidBbox.maxBound) {
            m_nodes[nodeIdx].firstTri = first;
            m_nodes[nodeIdx].triCount = count;
            m_nodes[nodeIdx].leftChild = -1;
            return;
        }

        glm::vec3 extent = centroidBbox.maxBound - centroidBbox.minBound;
        int32_t axis = 0;
        if (extent.y > extent.x && extent.y > extent.z) axis = 1;
        else if (extent.z > extent.x && extent.z > extent.y) axis = 2;

        float minC = centroidBbox.minBound[axis];
        float maxC = centroidBbox.maxBound[axis];
        float splitPos = 0.5f * (minC + maxC);

        int32_t i = first;
        int32_t j = first + count - 1;
        while (i <= j) {
            float c = buildPrims[i].centroid[axis];
            if (c < splitPos) {
                i++;
            } else {
                std::swap(buildPrims[i], buildPrims[j]);
                j--;
            }
        }

        int32_t leftCount = i - first;
        if (leftCount == 0 || leftCount == count) {
            leftCount = count / 2;
            std::nth_element(
                &buildPrims[first],
                &buildPrims[first + leftCount],
                &buildPrims[first + count],
                [axis](const BVHBuildPrim& a, const BVHBuildPrim& b) {
                    return a.centroid[axis] < b.centroid[axis];
                }
            );
        }
        int32_t rightCount = count - leftCount;

        BVHAABB leftBbox, leftCentroidBbox;
        for (int32_t k = first; k < first + leftCount; ++k) {
            leftBbox.Expand(buildPrims[k].aabb);
            leftCentroidBbox.Expand(buildPrims[k].centroid);
        }

        BVHAABB rightBbox, rightCentroidBbox;
        for (int32_t k = first + leftCount; k < first + count; ++k) {
            rightBbox.Expand(buildPrims[k].aabb);
            rightCentroidBbox.Expand(buildPrims[k].centroid);
        }

        int32_t leftChildIdx = m_nodeCount;
        m_nodeCount += 2;

        m_nodes[nodeIdx].leftChild = leftChildIdx;
        m_nodes[nodeIdx].triCount = 0; // internal node

        SubdivideNode(leftChildIdx, first, leftCount, depth + 1, leftBbox, leftCentroidBbox, buildPrims);
        SubdivideNode(leftChildIdx + 1, first + leftCount, rightCount, depth + 1, rightBbox, rightCentroidBbox, buildPrims);
    }

    std::vector<BVHTriangle> m_triangles;
    std::vector<BVHNode> m_nodes;
    int32_t m_nodeCount = 0;
    bool m_isBuilt = false;
    const Mesh* m_sourceMesh = nullptr;
    uint64_t m_meshVertGen = 0;
};

} // namespace retopo
