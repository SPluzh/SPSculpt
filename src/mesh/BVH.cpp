#include "mesh/BVH.h"
#include "mesh/Mesh.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <cstring>
#include <atomic>
#include <omp.h>

namespace {

inline glm::vec3 safeInvDir(const glm::vec3& dir) {
    return glm::vec3(
        std::abs(dir.x) > 1e-9f ? 1.0f / dir.x : (dir.x >= 0.0f ? 1e9f : -1e9f),
        std::abs(dir.y) > 1e-9f ? 1.0f / dir.y : (dir.y >= 0.0f ? 1e9f : -1e9f),
        std::abs(dir.z) > 1e-9f ? 1.0f / dir.z : (dir.z >= 0.0f ? 1e9f : -1e9f)
    );
}

inline bool rayBoxIntersect(
    const glm::vec3& boxMin, const glm::vec3& boxMax,
    const glm::vec3& origin, const glm::vec3& invDir, float tMax) {
    glm::vec3 t0 = (boxMin - origin) * invDir;
    glm::vec3 t1 = (boxMax - origin) * invDir;
    glm::vec3 tmin = glm::min(t0, t1);
    glm::vec3 tmax = glm::max(t0, t1);
    float enter = std::max(std::max(tmin.x, tmin.y), std::max(tmin.z, 0.0f));
    float exit = std::min(std::min(tmax.x, tmax.y), std::min(tmax.z, tMax));
    return enter <= exit;
}

inline bool rayBoxDistance(
    const glm::vec3& boxMin, const glm::vec3& boxMax,
    const glm::vec3& origin, const glm::vec3& invDir,
    float tMax, float& outEnter) {
    glm::vec3 t0 = (boxMin - origin) * invDir;
    glm::vec3 t1 = (boxMax - origin) * invDir;
    glm::vec3 tmin = glm::min(t0, t1);
    glm::vec3 tmax = glm::max(t0, t1);
    float enter = std::max(std::max(tmin.x, tmin.y), std::max(tmin.z, 0.0f));
    float exit = std::min(std::min(tmax.x, tmax.y), std::min(tmax.z, tMax));
    if (enter <= exit) {
        outEnter = enter;
        return true;
    }
    return false;
}

// Möller–Trumbore two-sided ray-triangle intersection
inline bool rayTriangle(
    const glm::vec3& orig, const glm::vec3& dir,
    const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
    float& t) {
    const float EPSILON = 1e-7f;
    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;
    glm::vec3 h = glm::cross(dir, edge2);
    float a = glm::dot(edge1, h);
    if (std::abs(a) < EPSILON) return false;
    float f = 1.0f / a;
    glm::vec3 s = orig - v0;
    float u = f * glm::dot(s, h);
    if (u < 0.0f || u > 1.0f) return false;
    glm::vec3 q = glm::cross(s, edge1);
    float v = f * glm::dot(dir, q);
    if (v < 0.0f || u + v > 1.0f) return false;
    float hitT = f * glm::dot(edge2, q);
    if (hitT > EPSILON) {
        t = hitT;
        return true;
    }
    return false;
}

inline bool sphereBoxIntersect(
    const glm::vec3& boxMin, const glm::vec3& boxMax,
    const glm::vec3& center, float radiusSquared) {
    float dx = 0.0f;
    if (center.x < boxMin.x) dx = boxMin.x - center.x;
    else if (center.x > boxMax.x) dx = center.x - boxMax.x;
    float d2 = dx * dx;
    if (d2 > radiusSquared) return false;

    float dy = 0.0f;
    if (center.y < boxMin.y) dy = boxMin.y - center.y;
    else if (center.y > boxMax.y) dy = center.y - boxMax.y;
    d2 += dy * dy;
    if (d2 > radiusSquared) return false;

    float dz = 0.0f;
    if (center.z < boxMin.z) dz = boxMin.z - center.z;
    else if (center.z > boxMax.z) dz = center.z - boxMax.z;
    d2 += dz * dz;
    return d2 <= radiusSquared;
}

inline bool boxFullyInSphere(
    const glm::vec3& boxMin, const glm::vec3& boxMax,
    const glm::vec3& center, float radiusSquared) {
    float dx = std::max(std::abs(boxMin.x - center.x), std::abs(boxMax.x - center.x));
    float d2 = dx * dx;
    if (d2 > radiusSquared) return false;

    float dy = std::max(std::abs(boxMin.y - center.y), std::abs(boxMax.y - center.y));
    d2 += dy * dy;
    if (d2 > radiusSquared) return false;

    float dz = std::max(std::abs(boxMin.z - center.z), std::abs(boxMax.z - center.z));
    d2 += dz * dz;
    return d2 <= radiusSquared;
}

inline bool isFaceVisible(const Mesh& mesh, uint32_t faceId) {
    if (!mesh.faceVisible.empty() && faceId < mesh.faceVisible.size() && !mesh.faceVisible[faceId]) {
        return false;
    }
    if (!mesh.vertVisible.empty()) {
        uint32_t idx = faceId * 4;
        uint32_t v0 = mesh.faces[idx + 0];
        uint32_t v1 = mesh.faces[idx + 1];
        uint32_t v2 = mesh.faces[idx + 2];
        uint32_t v3 = mesh.faces[idx + 3];

        if (v0 < mesh.vertVisible.size() && !mesh.vertVisible[v0]) return false;
        if (v1 < mesh.vertVisible.size() && !mesh.vertVisible[v1]) return false;
        if (v2 < mesh.vertVisible.size() && !mesh.vertVisible[v2]) return false;
        if (v3 != 0xFFFFFFFF && v3 < mesh.vertVisible.size() && !mesh.vertVisible[v3]) return false;
    }
    return true;
}

} // namespace

void BVH::clear() {
    m_nodes.clear();
    m_parents.clear();
    m_primitiveIndices.clear();
    m_faceToLeaf.clear();
    m_leafCount = 0;
    m_editsSinceBuild = 0;
    m_scratch[0].clear();
    m_scratch[1].clear();
    m_leafVisitedFlags.clear();
    m_leafVisitedEpoch = 0;
    m_nodeVisitedFlags.clear();
    m_nodeVisitedEpoch = 0;
    m_dirtyLeaves.clear();
    m_dirtyInnerNodes.clear();
    lastCollectCellsMs = 0.0;
    lastCandidateVertsMs = 0.0;
    lastFilterVertsMs = 0.0;
    m_lastPickStats[0] = BVHPickStats{};
    m_lastPickStats[1] = BVHPickStats{};
    m_lastRefitStats = BVHRefitStats{};
}

void BVH::computeLeafAABB(const Mesh& mesh, BVHNode& node, uint32_t nodeIdx, bool usePrecomputedBoxes, bool assignFaceToLeaf) {
    glm::vec3 bmin(std::numeric_limits<float>::infinity());
    glm::vec3 bmax(-std::numeric_limits<float>::infinity());

    const bool hasFaceBoxes = usePrecomputedBoxes && (mesh.faceBoxes.size() >= (size_t)mesh.nbFaces * 6);
    const float* const verts = mesh.verts.data();
    const uint32_t* const faces = mesh.faces.data();
    const size_t nbVerts = (size_t)mesh.nbVerts;
    const uint32_t* const primIndices = m_primitiveIndices.data();
    const uint32_t first = node.firstIndex;
    const uint32_t count = node.count;

    if (assignFaceToLeaf) {
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t faceId = primIndices[first + i];
            if (faceId < m_faceToLeaf.size()) {
                m_faceToLeaf[faceId] = nodeIdx;
            }
        }
    }

    if (hasFaceBoxes) {
        const float* const fb = mesh.faceBoxes.data();
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t faceId = primIndices[first + i];
            uint32_t idx = faceId * 6;
            bmin = glm::min(bmin, glm::vec3(fb[idx + 0], fb[idx + 1], fb[idx + 2]));
            bmax = glm::max(bmax, glm::vec3(fb[idx + 3], fb[idx + 4], fb[idx + 5]));
        }
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t faceId = primIndices[first + i];
            uint32_t idx = faceId * 4;
            for (int k = 0; k < 4; ++k) {
                uint32_t v = faces[idx + k];
                if (v == 0xFFFFFFFF || (size_t)v >= nbVerts) continue;
                glm::vec3 p(verts[v * 3 + 0], verts[v * 3 + 1], verts[v * 3 + 2]);
                bmin = glm::min(bmin, p);
                bmax = glm::max(bmax, p);
            }
        }
    }
    node.aabbMin = bmin;
    node.aabbMax = bmax;
}

void BVH::build(const Mesh& mesh) {
    clear();

    const int nbFaces = mesh.nbFaces;
    if (nbFaces <= 0 || mesh.nbVerts <= 0) return;

    m_primitiveIndices.resize(nbFaces);
    std::iota(m_primitiveIndices.begin(), m_primitiveIndices.end(), 0);
    m_faceToLeaf.assign(nbFaces, 0xFFFFFFFF);

    const bool hasCenters = mesh.faceCenters.size() >= (size_t)nbFaces * 3;
    const bool hasFaceBoxes = mesh.faceBoxes.size() >= (size_t)nbFaces * 6;
    const float* const verts = mesh.verts.data();
    const uint32_t* const faces = mesh.faces.data();

    std::vector<glm::vec3> fallbackCentroids;
    const glm::vec3* centroids = nullptr;
    if (hasCenters) {
        centroids = reinterpret_cast<const glm::vec3*>(mesh.faceCenters.data());
    } else {
        fallbackCentroids.resize(nbFaces);
        #pragma omp parallel for schedule(static) if(nbFaces > 2000)
        for (int i = 0; i < nbFaces; ++i) {
            uint32_t idx = i * 4;
            uint32_t v0 = faces[idx + 0];
            uint32_t v1 = faces[idx + 1];
            uint32_t v2 = faces[idx + 2];
            uint32_t v3 = faces[idx + 3];

            glm::vec3 p0(verts[v0 * 3 + 0], verts[v0 * 3 + 1], verts[v0 * 3 + 2]);
            glm::vec3 p1(verts[v1 * 3 + 0], verts[v1 * 3 + 1], verts[v1 * 3 + 2]);
            glm::vec3 p2(verts[v2 * 3 + 0], verts[v2 * 3 + 1], verts[v2 * 3 + 2]);

            if (v3 != 0xFFFFFFFF) {
                glm::vec3 p3(verts[v3 * 3 + 0], verts[v3 * 3 + 1], verts[v3 * 3 + 2]);
                fallbackCentroids[i] = (p0 + p1 + p2 + p3) * 0.25f;
            } else {
                fallbackCentroids[i] = (p0 + p1 + p2) * (1.0f / 3.0f);
            }
        }
        centroids = fallbackCentroids.data();
    }

    int maxThreads = omp_get_max_threads();
    bool inParallel = omp_in_parallel();

    // Fast sequential build path for small meshes or single thread
    if (nbFaces < 16384 || maxThreads <= 1 || inParallel) {
        m_nodes.reserve(2 * (nbFaces / kLeafSize + 1));
        m_parents.reserve(2 * (nbFaces / kLeafSize + 1));

        m_nodes.push_back(BVHNode{
            glm::vec3(std::numeric_limits<float>::infinity()),
            0,
            glm::vec3(-std::numeric_limits<float>::infinity()),
            (uint32_t)nbFaces
        });
        m_parents.push_back(0);

        auto splitNode = [&](uint32_t ni) -> bool {
            const uint32_t first = m_nodes[ni].firstIndex;
            const uint32_t count = m_nodes[ni].count;
            if (count <= kLeafSize) return false;

            glm::vec3 cmin(std::numeric_limits<float>::infinity());
            glm::vec3 cmax(-std::numeric_limits<float>::infinity());
            for (uint32_t i = first; i < first + count; ++i) {
                const glm::vec3& c = centroids[m_primitiveIndices[i]];
                cmin = glm::min(cmin, c);
                cmax = glm::max(cmax, c);
            }

            glm::vec3 ext = cmax - cmin;
            int axis = 0;
            if (ext.y > ext.x && ext.y > ext.z) axis = 1;
            else if (ext.z > ext.x && ext.z > ext.y) axis = 2;

            if (ext[axis] <= 1e-7f) return false;

            uint32_t mid = first + count / 2;
            std::nth_element(
                m_primitiveIndices.begin() + first,
                m_primitiveIndices.begin() + mid,
                m_primitiveIndices.begin() + first + count,
                [&](uint32_t a, uint32_t b) {
                    return centroids[a][axis] < centroids[b][axis];
                }
            );

            uint32_t leftChild = (uint32_t)m_nodes.size();
            m_nodes.push_back(BVHNode{
                glm::vec3(std::numeric_limits<float>::infinity()),
                first,
                glm::vec3(-std::numeric_limits<float>::infinity()),
                mid - first
            });
            m_nodes.push_back(BVHNode{
                glm::vec3(std::numeric_limits<float>::infinity()),
                mid,
                glm::vec3(-std::numeric_limits<float>::infinity()),
                first + count - mid
            });
            m_parents.push_back(ni);
            m_parents.push_back(ni);

            m_nodes[ni].firstIndex = leftChild;
            m_nodes[ni].count = 0;
            return true;
        };

        std::vector<uint32_t> stack;
        stack.reserve(64);
        stack.push_back(0);

        while (!stack.empty()) {
            uint32_t ni = stack.back();
            stack.pop_back();
            if (splitNode(ni)) {
                stack.push_back(m_nodes[ni].firstIndex + 1);
                stack.push_back(m_nodes[ni].firstIndex);
            } else {
                m_leafCount++;
            }
        }

        #pragma omp parallel for schedule(static) if(m_nodes.size() > 500)
        for (int i = 0; i < (int)m_nodes.size(); ++i) {
            if (m_nodes[i].isLeaf()) {
                computeLeafAABB(mesh, m_nodes[i], (uint32_t)i, false, true);
            }
        }

        for (int i = (int)m_nodes.size() - 1; i >= 0; --i) {
            BVHNode& node = m_nodes[i];
            if (!node.isLeaf()) {
                uint32_t left = node.firstIndex;
                uint32_t right = left + 1;
                node.aabbMin = glm::min(m_nodes[left].aabbMin, m_nodes[right].aabbMin);
                node.aabbMax = glm::max(m_nodes[left].aabbMax, m_nodes[right].aabbMax);
            }
        }
        return;
    }

    // 2-level OpenMP build for medium and large meshes
    const int targetSubtasks = std::max(64, maxThreads * 16);
    const uint32_t minTaskSize = 2048;

    m_nodes.reserve(std::max((size_t)1024, (size_t)targetSubtasks * 4));
    m_parents.reserve(std::max((size_t)1024, (size_t)targetSubtasks * 4));

    m_nodes.push_back(BVHNode{
        glm::vec3(std::numeric_limits<float>::infinity()),
        0,
        glm::vec3(-std::numeric_limits<float>::infinity()),
        (uint32_t)nbFaces
    });
    m_parents.push_back(0);

    std::vector<uint32_t> currentLevel;
    currentLevel.push_back(0);

    struct SplitTask {
        uint32_t parentIdx;
        uint32_t first;
        uint32_t count;
        uint32_t leftChild;
    };

    while (currentLevel.size() < (size_t)targetSubtasks) {
        std::vector<SplitTask> tasks;
        tasks.reserve(currentLevel.size());
        for (uint32_t ni : currentLevel) {
            if (m_nodes[ni].count > minTaskSize && m_nodes[ni].count > kLeafSize) {
                tasks.push_back({ ni, m_nodes[ni].firstIndex, m_nodes[ni].count, 0 });
            }
        }
        if (tasks.empty()) break;

        uint32_t baseChild = (uint32_t)m_nodes.size();
        size_t numTasks = tasks.size();
        m_nodes.resize(baseChild + numTasks * 2);
        m_parents.resize(baseChild + numTasks * 2);

        for (size_t s = 0; s < numTasks; ++s) {
            uint32_t lc = baseChild + (uint32_t)(s * 2);
            tasks[s].leftChild = lc;
            m_nodes[tasks[s].parentIdx].firstIndex = lc;
            m_nodes[tasks[s].parentIdx].count = 0; // mark as inner node
            m_parents[lc + 0] = tasks[s].parentIdx;
            m_parents[lc + 1] = tasks[s].parentIdx;
        }

        std::vector<uint32_t> nextLevel;
        nextLevel.reserve(currentLevel.size() * 2);
        for (uint32_t ni : currentLevel) {
            if (m_nodes[ni].isLeaf()) {
                nextLevel.push_back(ni);
            }
        }

        #pragma omp parallel for schedule(dynamic, 1)
        for (int s = 0; s < (int)numTasks; ++s) {
            const auto& t = tasks[s];
            glm::vec3 cmin(std::numeric_limits<float>::infinity());
            glm::vec3 cmax(-std::numeric_limits<float>::infinity());
            for (uint32_t i = t.first; i < t.first + t.count; ++i) {
                const glm::vec3& c = centroids[m_primitiveIndices[i]];
                cmin = glm::min(cmin, c);
                cmax = glm::max(cmax, c);
            }

            glm::vec3 ext = cmax - cmin;
            int axis = 0;
            if (ext.y > ext.x && ext.y > ext.z) axis = 1;
            else if (ext.z > ext.x && ext.z > ext.y) axis = 2;

            uint32_t mid = t.first + t.count / 2;
            if (ext[axis] > 1e-7f) {
                std::nth_element(
                    m_primitiveIndices.begin() + t.first,
                    m_primitiveIndices.begin() + mid,
                    m_primitiveIndices.begin() + t.first + t.count,
                    [&](uint32_t a, uint32_t b) {
                        return centroids[a][axis] < centroids[b][axis];
                    }
                );
            }

            m_nodes[t.leftChild + 0] = BVHNode{
                glm::vec3(std::numeric_limits<float>::infinity()),
                t.first,
                glm::vec3(-std::numeric_limits<float>::infinity()),
                mid - t.first
            };
            m_nodes[t.leftChild + 1] = BVHNode{
                glm::vec3(std::numeric_limits<float>::infinity()),
                mid,
                glm::vec3(-std::numeric_limits<float>::infinity()),
                t.first + t.count - mid
            };
        }

        for (size_t s = 0; s < numTasks; ++s) {
            nextLevel.push_back(tasks[s].leftChild + 0);
            nextLevel.push_back(tasks[s].leftChild + 1);
        }
        currentLevel = std::move(nextLevel);
    }

    // Collect top-level leaves as subtasks
    std::vector<uint32_t> subtaskNodes;
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i].isLeaf()) {
            subtaskNodes.push_back((uint32_t)i);
        }
    }

    const size_t numSubtasks = subtaskNodes.size();
    struct SubtreeResult {
        std::vector<BVHNode> nodes;
        std::vector<uint32_t> parents;
        size_t leafCount = 0;
    };
    std::vector<SubtreeResult> subtrees(numSubtasks);

    const size_t nbVerts = (size_t)mesh.nbVerts;

    #pragma omp parallel for schedule(dynamic, 1)
    for (int t = 0; t < (int)numSubtasks; ++t) {
        uint32_t topNodeIdx = subtaskNodes[t];
        const uint32_t tFirst = m_nodes[topNodeIdx].firstIndex;
        const uint32_t tCount = m_nodes[topNodeIdx].count;
        auto& sub = subtrees[t];

        if (tCount <= kLeafSize) {
            sub.leafCount = 1;
            continue;
        }

        // Split top node into children (local nodes 0 and 1)
        glm::vec3 cmin(std::numeric_limits<float>::infinity());
        glm::vec3 cmax(-std::numeric_limits<float>::infinity());
        for (uint32_t i = tFirst; i < tFirst + tCount; ++i) {
            const glm::vec3& c = centroids[m_primitiveIndices[i]];
            cmin = glm::min(cmin, c);
            cmax = glm::max(cmax, c);
        }

        glm::vec3 ext = cmax - cmin;
        int axis = 0;
        if (ext.y > ext.x && ext.y > ext.z) axis = 1;
        else if (ext.z > ext.x && ext.z > ext.y) axis = 2;

        if (ext[axis] <= 1e-7f) {
            sub.leafCount = 1;
            continue;
        }

        uint32_t mid = tFirst + tCount / 2;
        std::nth_element(
            m_primitiveIndices.begin() + tFirst,
            m_primitiveIndices.begin() + mid,
            m_primitiveIndices.begin() + tFirst + tCount,
            [&](uint32_t a, uint32_t b) {
                return centroids[a][axis] < centroids[b][axis];
            }
        );

        sub.nodes.reserve(2 * (tCount / kLeafSize + 2));
        sub.parents.reserve(2 * (tCount / kLeafSize + 2));

        sub.nodes.push_back(BVHNode{
            glm::vec3(std::numeric_limits<float>::infinity()),
            tFirst,
            glm::vec3(-std::numeric_limits<float>::infinity()),
            mid - tFirst
        });
        sub.nodes.push_back(BVHNode{
            glm::vec3(std::numeric_limits<float>::infinity()),
            mid,
            glm::vec3(-std::numeric_limits<float>::infinity()),
            tFirst + tCount - mid
        });
        sub.parents.push_back(0xFFFFFFFF);
        sub.parents.push_back(0xFFFFFFFF);

        std::vector<uint32_t> localStack;
        localStack.reserve(64);
        localStack.push_back(1);
        localStack.push_back(0);

        while (!localStack.empty()) {
            uint32_t lni = localStack.back();
            localStack.pop_back();

            const uint32_t first = sub.nodes[lni].firstIndex;
            const uint32_t count = sub.nodes[lni].count;

            if (count <= kLeafSize) {
                sub.leafCount++;
                glm::vec3 bmin(std::numeric_limits<float>::infinity());
                glm::vec3 bmax(-std::numeric_limits<float>::infinity());
                if (hasFaceBoxes) {
                    const float* const fb = mesh.faceBoxes.data();
                    for (uint32_t i = first; i < first + count; ++i) {
                        uint32_t idx = m_primitiveIndices[i] * 6;
                        bmin = glm::min(bmin, glm::vec3(fb[idx + 0], fb[idx + 1], fb[idx + 2]));
                        bmax = glm::max(bmax, glm::vec3(fb[idx + 3], fb[idx + 4], fb[idx + 5]));
                    }
                } else {
                    for (uint32_t i = first; i < first + count; ++i) {
                        uint32_t faceId = m_primitiveIndices[i];
                        uint32_t idx = faceId * 4;
                        for (int k = 0; k < 4; ++k) {
                            uint32_t v = faces[idx + k];
                            if (v == 0xFFFFFFFF || (size_t)v >= nbVerts) continue;
                            glm::vec3 p(verts[v * 3 + 0], verts[v * 3 + 1], verts[v * 3 + 2]);
                            bmin = glm::min(bmin, p);
                            bmax = glm::max(bmax, p);
                        }
                    }
                }
                sub.nodes[lni].aabbMin = bmin;
                sub.nodes[lni].aabbMax = bmax;
                continue;
            }

            glm::vec3 lcmin(std::numeric_limits<float>::infinity());
            glm::vec3 lcmax(-std::numeric_limits<float>::infinity());
            for (uint32_t i = first; i < first + count; ++i) {
                const glm::vec3& c = centroids[m_primitiveIndices[i]];
                lcmin = glm::min(lcmin, c);
                lcmax = glm::max(lcmax, c);
            }

            glm::vec3 lext = lcmax - lcmin;
            int laxis = 0;
            if (lext.y > lext.x && lext.y > lext.z) laxis = 1;
            else if (lext.z > lext.x && lext.z > lext.y) laxis = 2;

            if (lext[laxis] <= 1e-7f) {
                sub.leafCount++;
                glm::vec3 bmin(std::numeric_limits<float>::infinity());
                glm::vec3 bmax(-std::numeric_limits<float>::infinity());
                if (hasFaceBoxes) {
                    const float* const fb = mesh.faceBoxes.data();
                    for (uint32_t i = first; i < first + count; ++i) {
                        uint32_t idx = m_primitiveIndices[i] * 6;
                        bmin = glm::min(bmin, glm::vec3(fb[idx + 0], fb[idx + 1], fb[idx + 2]));
                        bmax = glm::max(bmax, glm::vec3(fb[idx + 3], fb[idx + 4], fb[idx + 5]));
                    }
                } else {
                    for (uint32_t i = first; i < first + count; ++i) {
                        uint32_t faceId = m_primitiveIndices[i];
                        uint32_t idx = faceId * 4;
                        for (int k = 0; k < 4; ++k) {
                            uint32_t v = faces[idx + k];
                            if (v == 0xFFFFFFFF || (size_t)v >= nbVerts) continue;
                            glm::vec3 p(verts[v * 3 + 0], verts[v * 3 + 1], verts[v * 3 + 2]);
                            bmin = glm::min(bmin, p);
                            bmax = glm::max(bmax, p);
                        }
                    }
                }
                sub.nodes[lni].aabbMin = bmin;
                sub.nodes[lni].aabbMax = bmax;
                continue;
            }

            uint32_t lmid = first + count / 2;
            std::nth_element(
                m_primitiveIndices.begin() + first,
                m_primitiveIndices.begin() + lmid,
                m_primitiveIndices.begin() + first + count,
                [&](uint32_t a, uint32_t b) {
                    return centroids[a][laxis] < centroids[b][laxis];
                }
            );

            uint32_t leftChild = (uint32_t)sub.nodes.size();
            sub.nodes.push_back(BVHNode{
                glm::vec3(std::numeric_limits<float>::infinity()),
                first,
                glm::vec3(-std::numeric_limits<float>::infinity()),
                lmid - first
            });
            sub.nodes.push_back(BVHNode{
                glm::vec3(std::numeric_limits<float>::infinity()),
                lmid,
                glm::vec3(-std::numeric_limits<float>::infinity()),
                first + count - lmid
            });
            sub.parents.push_back(lni);
            sub.parents.push_back(lni);

            sub.nodes[lni].firstIndex = leftChild;
            sub.nodes[lni].count = 0;

            localStack.push_back(leftChild + 1);
            localStack.push_back(leftChild);
        }

        // Local bottom-up inner AABB computation
        for (int i = (int)sub.nodes.size() - 1; i >= 0; --i) {
            if (!sub.nodes[i].isLeaf()) {
                uint32_t left = sub.nodes[i].firstIndex;
                uint32_t right = left + 1;
                sub.nodes[i].aabbMin = glm::min(sub.nodes[left].aabbMin, sub.nodes[right].aabbMin);
                sub.nodes[i].aabbMax = glm::max(sub.nodes[left].aabbMax, sub.nodes[right].aabbMax);
            }
        }
    }

    // Step D: Merge subtrees into m_nodes with topological invariant (child > parent)
    const uint32_t topCount = (uint32_t)m_nodes.size();
    std::vector<uint32_t> subOffsets(numSubtasks + 1);
    subOffsets[0] = topCount;
    for (size_t t = 0; t < numSubtasks; ++t) {
        subOffsets[t + 1] = subOffsets[t] + (uint32_t)subtrees[t].nodes.size();
        m_leafCount += subtrees[t].leafCount;
    }
    const uint32_t totalNodes = subOffsets.back();
    m_nodes.resize(totalNodes);
    m_parents.resize(totalNodes);

    #pragma omp parallel for schedule(static)
    for (int t = 0; t < (int)numSubtasks; ++t) {
        uint32_t topNodeIdx = subtaskNodes[t];
        if (subtrees[t].nodes.empty()) {
            computeLeafAABB(mesh, m_nodes[topNodeIdx], topNodeIdx, hasFaceBoxes, true);
        } else {
            uint32_t base = subOffsets[t];
            uint32_t subSize = (uint32_t)subtrees[t].nodes.size();

            m_nodes[topNodeIdx].firstIndex = base;
            m_nodes[topNodeIdx].count = 0; // marked as inner
            m_nodes[topNodeIdx].aabbMin = glm::min(subtrees[t].nodes[0].aabbMin, subtrees[t].nodes[1].aabbMin);
            m_nodes[topNodeIdx].aabbMax = glm::max(subtrees[t].nodes[0].aabbMax, subtrees[t].nodes[1].aabbMax);

            std::memcpy(&m_nodes[base], subtrees[t].nodes.data(), subSize * sizeof(BVHNode));

            m_parents[base + 0] = topNodeIdx;
            m_parents[base + 1] = topNodeIdx;

            for (uint32_t k = 0; k < subSize; ++k) {
                if (k >= 2) {
                    m_parents[base + k] = subtrees[t].parents[k] + base;
                }
                if (!m_nodes[base + k].isLeaf()) {
                    m_nodes[base + k].firstIndex += base;
                } else {
                    uint32_t fStart = m_nodes[base + k].firstIndex;
                    uint32_t fCount = m_nodes[base + k].count;
                    uint32_t leafNodeIdx = base + k;
                    for (uint32_t fi = 0; fi < fCount; ++fi) {
                        m_faceToLeaf[m_primitiveIndices[fStart + fi]] = leafNodeIdx;
                    }
                }
            }
        }
    }

    // Step E: Final bottom-up pass on top-level nodes
    for (int i = (int)topCount - 1; i >= 0; --i) {
        if (!m_nodes[i].isLeaf()) {
            uint32_t left = m_nodes[i].firstIndex;
            uint32_t right = left + 1;
            m_nodes[i].aabbMin = glm::min(m_nodes[left].aabbMin, m_nodes[right].aabbMin);
            m_nodes[i].aabbMax = glm::max(m_nodes[left].aabbMax, m_nodes[right].aabbMax);
        }
    }
}

bool BVH::raycast(const Mesh& mesh, const glm::vec3& origin, const glm::vec3& dir, RayHit& hit) const {
    if (m_nodes.empty() || mesh.nbFaces <= 0 || mesh.nbVerts <= 0) return false;

    glm::vec3 invDir = safeInvDir(dir);
    float closestT = hit.t;
    bool found = false;

    struct TraversalEntry {
        uint32_t nodeIdx;
        float dist;
    };
    TraversalEntry stack[64];
    int stackPtr = 0;

    float rootDist = 0.0f;
    if (!rayBoxDistance(m_nodes[0].aabbMin, m_nodes[0].aabbMax, origin, invDir, closestT, rootDist)) {
        return false;
    }
    stack[stackPtr++] = {0, rootDist};

    const float* const verts = mesh.verts.data();
    const uint32_t* const faces = mesh.faces.data();
    const size_t nbVerts = (size_t)mesh.nbVerts;

    while (stackPtr > 0) {
        TraversalEntry current = stack[--stackPtr];
        if (current.dist >= closestT) continue;

        const BVHNode& node = m_nodes[current.nodeIdx];
        if (node.isLeaf()) {
            for (uint32_t i = 0; i < node.count; ++i) {
                uint32_t faceId = m_primitiveIndices[node.firstIndex + i];
                if (!isFaceVisible(mesh, faceId)) continue;

                uint32_t idx = faceId * 4;
                uint32_t v0 = faces[idx + 0];
                uint32_t v1 = faces[idx + 1];
                uint32_t v2 = faces[idx + 2];
                uint32_t v3 = faces[idx + 3];

                if (v0 >= nbVerts || v1 >= nbVerts || v2 >= nbVerts) continue;

                glm::vec3 p0(verts[v0 * 3], verts[v0 * 3 + 1], verts[v0 * 3 + 2]);
                glm::vec3 p1(verts[v1 * 3], verts[v1 * 3 + 1], verts[v1 * 3 + 2]);
                glm::vec3 p2(verts[v2 * 3], verts[v2 * 3 + 1], verts[v2 * 3 + 2]);

                float t = 0.0f;
                if (rayTriangle(origin, dir, p0, p1, p2, t) && t < closestT) {
                    closestT = t;
                    hit.t = t;
                    hit.faceId = faceId;
                    hit.position = origin + dir * t;
                    glm::vec3 geoNorm = glm::cross(p1 - p0, p2 - p0);
                    float normLen = glm::length(geoNorm);
                    hit.normal = normLen > 1e-9f ? (geoNorm / normLen) : glm::vec3(0.0f, 1.0f, 0.0f);
                    found = true;
                }

                if (v3 != 0xFFFFFFFF && (size_t)v3 < nbVerts) {
                    glm::vec3 p3(verts[v3 * 3], verts[v3 * 3 + 1], verts[v3 * 3 + 2]);
                    if (rayTriangle(origin, dir, p0, p2, p3, t) && t < closestT) {
                        closestT = t;
                        hit.t = t;
                        hit.faceId = faceId;
                        hit.position = origin + dir * t;
                        glm::vec3 geoNorm = glm::cross(p2 - p0, p3 - p0);
                        float normLen = glm::length(geoNorm);
                        hit.normal = normLen > 1e-9f ? (geoNorm / normLen) : glm::vec3(0.0f, 1.0f, 0.0f);
                        found = true;
                    }
                }
            }
        } else {
            uint32_t left = node.firstIndex;
            uint32_t right = left + 1;
            float distL = 0.0f, distR = 0.0f;
            bool hitL = rayBoxDistance(m_nodes[left].aabbMin, m_nodes[left].aabbMax, origin, invDir, closestT, distL);
            bool hitR = rayBoxDistance(m_nodes[right].aabbMin, m_nodes[right].aabbMax, origin, invDir, closestT, distR);

            if (hitL && hitR) {
                if (distL < distR) {
                    stack[stackPtr++] = {right, distR};
                    stack[stackPtr++] = {left, distL};
                } else {
                    stack[stackPtr++] = {left, distL};
                    stack[stackPtr++] = {right, distR};
                }
            } else if (hitL) {
                stack[stackPtr++] = {left, distL};
            } else if (hitR) {
                stack[stackPtr++] = {right, distR};
            }
        }
    }
    return found;
}

void BVH::collectIntersectRay(const glm::vec3& origin, const glm::vec3& dir, std::vector<uint32_t>& outFaces) const {
    outFaces.clear();
    if (m_nodes.empty()) return;

    glm::vec3 invDir = safeInvDir(dir);
    uint32_t stack[64];
    int stackPtr = 0;
    stack[stackPtr++] = 0;

    const float inf = std::numeric_limits<float>::infinity();

    while (stackPtr > 0) {
        uint32_t nodeIdx = stack[--stackPtr];
        const BVHNode& node = m_nodes[nodeIdx];

        if (!rayBoxIntersect(node.aabbMin, node.aabbMax, origin, invDir, inf)) {
            continue;
        }

        if (node.isLeaf()) {
            for (uint32_t i = 0; i < node.count; ++i) {
                outFaces.push_back(m_primitiveIndices[node.firstIndex + i]);
            }
        } else {
            stack[stackPtr++] = node.firstIndex + 1;
            stack[stackPtr++] = node.firstIndex;
        }
    }
}

void BVH::pickVerticesInSphere(
    const Mesh& mesh, const glm::vec3& center, float radiusSquared,
    std::vector<uint32_t>& outVertices, const uint8_t* vertVisible,
    int scratchIndex) const {

    int sIdx = scratchIndex & 1;
    QueryScratch& sc = m_scratch[sIdx];
    outVertices.clear();
    BVHPickStats stats;

    if (m_nodes.empty() || mesh.nbVerts <= 0 || mesh.nbFaces <= 0) {
        m_lastPickStats[sIdx] = stats;
        if (sIdx == 0) {
            lastCollectCellsMs = lastCandidateVertsMs = lastFilterVertsMs = 0.0;
        }
        return;
    }

    auto t0 = std::chrono::high_resolution_clock::now();

    sc.candidateLeaves.clear();
    uint32_t totalCandidateFaces = 0;

    uint32_t stack[64];
    int stackPtr = 0;
    stack[stackPtr++] = 0;

    while (stackPtr > 0) {
        uint32_t nodeIdx = stack[--stackPtr];
        const BVHNode& node = m_nodes[nodeIdx];
        if (!sphereBoxIntersect(node.aabbMin, node.aabbMax, center, radiusSquared)) {
            continue;
        }
        if (node.isLeaf()) {
            sc.candidateLeaves.push_back(nodeIdx);
            totalCandidateFaces += node.count;
        } else {
            stack[stackPtr++] = node.firstIndex;
            stack[stackPtr++] = node.firstIndex + 1;
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    stats.collectCellsMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats.candLeaves = (uint32_t)sc.candidateLeaves.size();
    stats.candFaces = totalCandidateFaces;

    if (sc.candidateLeaves.empty()) {
        m_lastPickStats[sIdx] = stats;
        if (sIdx == 0) {
            lastCollectCellsMs = stats.collectCellsMs;
            lastCandidateVertsMs = 0.0;
            lastFilterVertsMs = 0.0;
        }
        return;
    }

    if (sc.visitedFlags.size() < (size_t)mesh.nbVerts) {
        sc.visitedFlags.assign(mesh.nbVerts, 0);
    }
    sc.visitedEpoch++;
    if (sc.visitedEpoch == 0) {
        std::fill(sc.visitedFlags.begin(), sc.visitedFlags.end(), 0);
        sc.visitedEpoch = 1;
    }
    auto tInitDone = std::chrono::high_resolution_clock::now();
    stats.visitedInitMs = std::chrono::duration<double, std::milli>(tInitDone - t1).count();

    const uint32_t curEpoch = sc.visitedEpoch;
    uint32_t* const vFlags = sc.visitedFlags.data();
    const uint32_t limitVerts = (uint32_t)mesh.nbVerts;
    const float* const verts = mesh.verts.data();
    const uint32_t* const faces = mesh.faces.data();
    const uint32_t* const primIndices = m_primitiveIndices.data();

    const int maxThreads = omp_get_max_threads();
    const bool inParallel = omp_in_parallel();

    if (totalCandidateFaces < 65536 || inParallel || maxThreads <= 1) {
        stats.usedMT = false;
        stats.numThreads = 1;
        size_t estCount = std::min((size_t)mesh.nbVerts, (size_t)totalCandidateFaces * 2);
        if (outVertices.capacity() < estCount) {
            outVertices.reserve(estCount);
        }
        for (uint32_t nodeIdx : sc.candidateLeaves) {
            const BVHNode& node = m_nodes[nodeIdx];
            const uint32_t* const leafPrims = primIndices + node.firstIndex;
            const uint32_t count = node.count;

            for (uint32_t i = 0; i < count; ++i) {
                uint32_t faceId = leafPrims[i];
                const uint32_t* const fv = faces + faceId * 4;
                for (int k = 0; k < 4; ++k) {
                    uint32_t v = fv[k];
                    if (v >= limitVerts || v == 0xFFFFFFFF) continue;
                    if (vFlags[v] == curEpoch) continue;
                    vFlags[v] = curEpoch;

                    if (vertVisible && vertVisible[v] == 0) continue;

                    const float* const vp = verts + v * 3;
                    float dx = center.x - vp[0];
                    float dx2 = dx * dx;
                    if (dx2 > radiusSquared) continue;
                    float dy = center.y - vp[1];
                    float dy2 = dy * dy;
                    if (dx2 + dy2 > radiusSquared) continue;
                    float dz = center.z - vp[2];
                    if (dx2 + dy2 + dz * dz <= radiusSquared) {
                        outVertices.push_back(v);
                    }
                }
            }
        }
        auto tPhase2End = std::chrono::high_resolution_clock::now();
        stats.filterVertsMs = std::chrono::duration<double, std::milli>(tPhase2End - tInitDone).count();
        stats.mergeMs = 0.0;
    } else {
        stats.usedMT = true;
        stats.numThreads = maxThreads;
        if ((int)sc.threadPicked.size() < maxThreads) {
            sc.threadPicked.resize(maxThreads);
        }
        const size_t numCandLeaves = sc.candidateLeaves.size();

        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            auto& localPicked = sc.threadPicked[tid];
            localPicked.clear();
            size_t estCount = (totalCandidateFaces * 2 / maxThreads) + 64;
            if (localPicked.capacity() < estCount) {
                localPicked.reserve(estCount);
            }

            #pragma omp for schedule(static)
            for (int64_t c = 0; c < (int64_t)numCandLeaves; ++c) {
                uint32_t nodeIdx = sc.candidateLeaves[c];
                const BVHNode& node = m_nodes[nodeIdx];
                const uint32_t* const leafPrims = primIndices + node.firstIndex;
                const uint32_t count = node.count;

                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t faceId = leafPrims[i];
                    const uint32_t* const fv = faces + faceId * 4;
                    for (int k = 0; k < 4; ++k) {
                        uint32_t v = fv[k];
                        if (v >= limitVerts || v == 0xFFFFFFFF) continue;
                        if (vFlags[v] == curEpoch) continue;

                        const float* const vp = verts + v * 3;
                        float dx = center.x - vp[0];
                        float dx2 = dx * dx;
                        if (dx2 > radiusSquared) continue;
                        float dy = center.y - vp[1];
                        float dy2 = dy * dy;
                        if (dx2 + dy2 > radiusSquared) continue;
                        float dz = center.z - vp[2];
                        if (dx2 + dy2 + dz * dz > radiusSquared) continue;

                        std::atomic_ref<uint32_t> ref(vFlags[v]);
                        if (ref.exchange(curEpoch, std::memory_order_relaxed) != curEpoch) {
                            if (vertVisible && vertVisible[v] == 0) continue;
                            localPicked.push_back(v);
                        }
                    }
                }
            }
        }
        auto tMTLoopEnd = std::chrono::high_resolution_clock::now();
        stats.filterVertsMs = std::chrono::duration<double, std::milli>(tMTLoopEnd - tInitDone).count();

        size_t totalFound = 0;
        for (int t = 0; t < maxThreads; ++t) {
            totalFound += sc.threadPicked[t].size();
        }
        outVertices.resize(totalFound);
        size_t offset = 0;
        for (int t = 0; t < maxThreads; ++t) {
            size_t count = sc.threadPicked[t].size();
            if (count > 0) {
                std::memcpy(outVertices.data() + offset, sc.threadPicked[t].data(), count * sizeof(uint32_t));
                offset += count;
            }
        }
        auto tMergeEnd = std::chrono::high_resolution_clock::now();
        stats.mergeMs = std::chrono::duration<double, std::milli>(tMergeEnd - tMTLoopEnd).count();
    }

    stats.outVerts = (uint32_t)outVertices.size();
    m_lastPickStats[sIdx] = stats;
    if (sIdx == 0) {
        lastCollectCellsMs = stats.collectCellsMs;
        lastCandidateVertsMs = stats.visitedInitMs;
        lastFilterVertsMs = stats.filterVertsMs + stats.mergeMs;
    }
}

void BVH::refit(const Mesh& mesh, const uint32_t* modifiedFaces, size_t count, bool usePrecomputedBoxes) {
    BVHRefitStats stats;
    stats.inputFaces = (uint32_t)count;
    stats.usedPrecomputed = usePrecomputedBoxes;

    if (m_nodes.empty() || !modifiedFaces || count == 0 || mesh.nbFaces <= 0 || mesh.nbVerts <= 0) {
        m_lastRefitStats = stats;
        return;
    }

    if (count > (size_t)mesh.nbFaces * 0.40f) {
        refitAll(mesh);
        m_lastRefitStats.inputFaces = (uint32_t)count;
        return;
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    m_editsSinceBuild += count;

    if (m_leafVisitedFlags.size() < m_nodes.size()) {
        m_leafVisitedFlags.assign(m_nodes.size(), 0);
    }
    m_leafVisitedEpoch++;
    if (m_leafVisitedEpoch == 0) {
        std::fill(m_leafVisitedFlags.begin(), m_leafVisitedFlags.end(), 0);
        m_leafVisitedEpoch = 1;
    }

    m_dirtyLeaves.clear();
    if (m_dirtyLeaves.capacity() < count / 4 + 16) {
        m_dirtyLeaves.reserve(count / 4 + 16);
    }

    for (size_t i = 0; i < count; ++i) {
        uint32_t faceId = modifiedFaces[i];
        if (faceId >= m_faceToLeaf.size()) continue;
        uint32_t leafIdx = m_faceToLeaf[faceId];
        if (leafIdx >= m_nodes.size()) continue;

        if (m_leafVisitedFlags[leafIdx] != m_leafVisitedEpoch) {
            m_leafVisitedFlags[leafIdx] = m_leafVisitedEpoch;
            m_dirtyLeaves.push_back(leafIdx);
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    stats.leafDedupMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Refit dirty leaves in parallel for large sets, sequential for small
    const size_t numDirty = m_dirtyLeaves.size();
    stats.dirtyLeaves = (uint32_t)numDirty;
    if (numDirty < 2048) {
        for (size_t i = 0; i < numDirty; ++i) {
            uint32_t leafIdx = m_dirtyLeaves[i];
            computeLeafAABB(mesh, m_nodes[leafIdx], leafIdx, usePrecomputedBoxes, false);
        }
    } else {
        #pragma omp parallel for schedule(static)
        for (int64_t i = 0; i < (int64_t)numDirty; ++i) {
            uint32_t leafIdx = m_dirtyLeaves[i];
            computeLeafAABB(mesh, m_nodes[leafIdx], leafIdx, usePrecomputedBoxes, false);
        }
    }
    auto t2 = std::chrono::high_resolution_clock::now();
    stats.leafAABBMs = std::chrono::duration<double, std::milli>(t2 - t1).count();

    // Propagate up to root using topological invariant (parentIdx < childIdx)
    if (m_nodeVisitedFlags.size() < m_nodes.size()) {
        m_nodeVisitedFlags.assign(m_nodes.size(), 0);
    }
    m_nodeVisitedEpoch++;
    if (m_nodeVisitedEpoch == 0) {
        std::fill(m_nodeVisitedFlags.begin(), m_nodeVisitedFlags.end(), 0);
        m_nodeVisitedEpoch = 1;
    }

    m_dirtyInnerNodes.clear();
    if (m_dirtyInnerNodes.capacity() < numDirty) {
        m_dirtyInnerNodes.reserve(numDirty);
    }

    for (uint32_t leafIdx : m_dirtyLeaves) {
        uint32_t curr = leafIdx;
        while (curr != 0) {
            uint32_t parent = m_parents[curr];
            if (m_nodeVisitedFlags[parent] == m_nodeVisitedEpoch) {
                break;
            }
            m_nodeVisitedFlags[parent] = m_nodeVisitedEpoch;
            m_dirtyInnerNodes.push_back(parent);
            curr = parent;
        }
    }

    std::sort(m_dirtyInnerNodes.begin(), m_dirtyInnerNodes.end(), std::greater<uint32_t>());
    stats.dirtyInnerNodes = (uint32_t)m_dirtyInnerNodes.size();
    auto t3 = std::chrono::high_resolution_clock::now();
    stats.parentGatherMs = std::chrono::duration<double, std::milli>(t3 - t2).count();

    for (uint32_t nodeIdx : m_dirtyInnerNodes) {
        BVHNode& node = m_nodes[nodeIdx];
        uint32_t left = node.firstIndex;
        uint32_t right = left + 1;
        node.aabbMin = glm::min(m_nodes[left].aabbMin, m_nodes[right].aabbMin);
        node.aabbMax = glm::max(m_nodes[left].aabbMax, m_nodes[right].aabbMax);
    }
    auto t4 = std::chrono::high_resolution_clock::now();
    stats.innerUpdateMs = std::chrono::duration<double, std::milli>(t4 - t3).count();

    m_lastRefitStats = stats;
}

void BVH::refitAll(const Mesh& mesh) {
    BVHRefitStats stats;
    stats.inputFaces = (uint32_t)mesh.nbFaces;
    stats.fellBackToRefitAll = true;
    stats.dirtyLeaves = (uint32_t)m_leafCount;
    stats.dirtyInnerNodes = (uint32_t)(m_nodes.size() > m_leafCount ? m_nodes.size() - m_leafCount : 0);

    if (m_nodes.empty() || mesh.nbFaces <= 0 || mesh.nbVerts <= 0) {
        m_lastRefitStats = stats;
        return;
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    #pragma omp parallel for schedule(static) if(m_nodes.size() > 500)
    for (int i = 0; i < (int)m_nodes.size(); ++i) {
        if (m_nodes[i].isLeaf()) {
            computeLeafAABB(mesh, m_nodes[i], (uint32_t)i, false, false);
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    stats.leafAABBMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    for (int i = (int)m_nodes.size() - 1; i >= 0; --i) {
        BVHNode& node = m_nodes[i];
        if (!node.isLeaf()) {
            uint32_t left = node.firstIndex;
            uint32_t right = left + 1;
            node.aabbMin = glm::min(m_nodes[left].aabbMin, m_nodes[right].aabbMin);
            node.aabbMax = glm::max(m_nodes[left].aabbMax, m_nodes[right].aabbMax);
        }
    }
    auto t2 = std::chrono::high_resolution_clock::now();
    stats.innerUpdateMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    m_lastRefitStats = stats;
}

void BVH::scale(float sx, float sy, float sz) {
    for (auto& node : m_nodes) {
        float x0 = node.aabbMin.x * sx, x1 = node.aabbMax.x * sx;
        node.aabbMin.x = std::min(x0, x1);
        node.aabbMax.x = std::max(x0, x1);

        float y0 = node.aabbMin.y * sy, y1 = node.aabbMax.y * sy;
        node.aabbMin.y = std::min(y0, y1);
        node.aabbMax.y = std::max(y0, y1);

        float z0 = node.aabbMin.z * sz, z1 = node.aabbMax.z * sz;
        node.aabbMin.z = std::min(z0, z1);
        node.aabbMax.z = std::max(z0, z1);
    }
}

bool BVH::isDegraded() const {
    return m_editsSinceBuild > m_primitiveIndices.size() * 2;
}

size_t BVH::memoryBytes() const {
    size_t bytes = sizeof(*this);
    bytes += m_nodes.capacity() * sizeof(BVHNode);
    bytes += m_parents.capacity() * sizeof(uint32_t);
    bytes += m_primitiveIndices.capacity() * sizeof(uint32_t);
    bytes += m_faceToLeaf.capacity() * sizeof(uint32_t);
    for (int i = 0; i < 2; ++i) {
        bytes += m_scratch[i].visitedFlags.capacity() * sizeof(uint32_t);
        bytes += m_scratch[i].candidateLeaves.capacity() * sizeof(uint32_t);
        for (const auto& tp : m_scratch[i].threadPicked) {
            bytes += tp.capacity() * sizeof(uint32_t);
        }
    }
    bytes += m_leafVisitedFlags.capacity() * sizeof(uint32_t);
    bytes += m_nodeVisitedFlags.capacity() * sizeof(uint32_t);
    bytes += m_dirtyLeaves.capacity() * sizeof(uint32_t);
    bytes += m_dirtyInnerNodes.capacity() * sizeof(uint32_t);
    return bytes;
}

