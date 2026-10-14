#include "sculpt/RelaxEngine.h"
#include "mesh/Mesh.h"
#include "editing/SculptManager.h"

#include <cmath>
#include <algorithm>
#include <vector>
#include <limits>
#include <glm/gtc/constants.hpp>

namespace {

static float distSqPointToSegment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b) {
    glm::vec3 ab = b - a;
    float lenSq = glm::dot(ab, ab);
    if (lenSq < 1e-8f) {
        glm::vec3 d = p - a;
        return glm::dot(d, d);
    }
    float t = glm::clamp(glm::dot(p - a, ab) / lenSq, 0.0f, 1.0f);
    glm::vec3 proj = a + ab * t;
    glm::vec3 d = p - proj;
    return glm::dot(d, d);
}

} // namespace

glm::vec3 RelaxEngine::closestPointOnTriangle(
    const glm::vec3& p,
    const glm::vec3& a,
    const glm::vec3& b,
    const glm::vec3& c
) {
    glm::vec3 ab = b - a;
    glm::vec3 ac = c - a;
    glm::vec3 ap = p - a;
    float d1 = glm::dot(ab, ap);
    float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    glm::vec3 bp = p - b;
    float d3 = glm::dot(ab, bp);
    float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        return a + ab * v;
    }

    glm::vec3 cp = p - c;
    float d5 = glm::dot(ab, cp);
    float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        return a + ac * w;
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + (c - b) * w;
    }

    float denomSum = va + vb + vc;
    if (denomSum <= 1e-12f) return a;
    float denom = 1.0f / denomSum;
    float v = vb * denom;
    float w = vc * denom;
    return a + ab * v + ac * w;
}

RelaxEngine::RelaxEngine() = default;

RelaxEngine::~RelaxEngine() {
    endStroke();
}

bool RelaxEngine::beginStroke(
    Mesh* mesh,
    int algorithm,
    bool preserveCreases,
    float creaseAngleDeg
) {
    m_strokeActive = true;
    m_cachedAlgorithm = algorithm;
    return ensureMeshTopology(mesh, preserveCreases, creaseAngleDeg);
}

void RelaxEngine::endStroke() {
    m_strokeActive = false;
    m_cachedMesh = nullptr;
    m_initialPositions.clear();
    m_boundaryNeighbors.clear();
    m_creaseNeighbors.clear();
    m_boundaryEdges.clear();
}

bool RelaxEngine::ensureMeshTopology(
    Mesh* mesh,
    bool preserveCreases,
    float creaseAngleDeg
) {
    if (!mesh) return false;

    int ptCount = mesh->nbVerts;
    int polyCount = mesh->nbFaces;
    if (ptCount < 3 || polyCount == 0) return false;

    if (m_cachedMesh == mesh && (int)m_initialPositions.size() == ptCount &&
        m_cachedPreserveCreases == preserveCreases &&
        std::abs(m_cachedCreaseAngleDeg - creaseAngleDeg) < 0.01f) {
        return true;
    }

    m_cachedMesh = mesh;
    m_cachedPreserveCreases = preserveCreases;
    m_cachedCreaseAngleDeg = creaseAngleDeg;

    const float* pts = mesh->verts.data();

    // Cache initial positions for surface reprojection
    m_initialPositions.resize(ptCount);
    for (int i = 0; i < ptCount; ++i) {
        m_initialPositions[i] = glm::vec3(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]);
    }

    // Calculate unit polygon normals for crease dihedral angle detection
    std::vector<glm::vec3> polyUnitNormals(polyCount);
    for (int p = 0; p < polyCount; ++p) {
        uint32_t a = mesh->faces[p * 4 + 0];
        uint32_t b = mesh->faces[p * 4 + 1];
        uint32_t c = mesh->faces[p * 4 + 2];
        uint32_t d = mesh->faces[p * 4 + 3];

        glm::vec3 A(pts[a * 3], pts[a * 3 + 1], pts[a * 3 + 2]);
        glm::vec3 B(pts[b * 3], pts[b * 3 + 1], pts[b * 3 + 2]);
        glm::vec3 C(pts[c * 3], pts[c * 3 + 1], pts[c * 3 + 2]);
        glm::vec3 N = glm::cross(B - A, C - A);
        if (d != 0xffffffff) {
            glm::vec3 D(pts[d * 3], pts[d * 3 + 1], pts[d * 3 + 2]);
            N += glm::cross(C - A, D - A);
        }
        float len = glm::length(N);
        if (len > 1e-8f) {
            polyUnitNormals[p] = N / len;
        } else {
            polyUnitNormals[p] = glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }

    struct RawEdge {
        uint32_t u;
        uint32_t v;
        uint32_t polyIndex;

        bool operator<(const RawEdge& other) const {
            if (u != other.u) return u < other.u;
            if (v != other.v) return v < other.v;
            return polyIndex < other.polyIndex;
        }
    };

    std::vector<RawEdge> rawEdges;
    rawEdges.reserve((size_t)polyCount * 4);

    auto addRaw = [&](uint32_t pIdx, uint32_t a, uint32_t b) {
        RawEdge re;
        re.u = std::min(a, b);
        re.v = std::max(a, b);
        re.polyIndex = pIdx;
        rawEdges.push_back(re);
    };

    for (int p = 0; p < polyCount; ++p) {
        uint32_t a = mesh->faces[p * 4 + 0];
        uint32_t b = mesh->faces[p * 4 + 1];
        uint32_t c = mesh->faces[p * 4 + 2];
        uint32_t d = mesh->faces[p * 4 + 3];

        addRaw(p, a, b);
        addRaw(p, b, c);
        if (d != 0xffffffff) {
            addRaw(p, c, d);
            addRaw(p, d, a);
        } else {
            addRaw(p, c, a);
        }
    }

    std::sort(rawEdges.begin(), rawEdges.end());

    m_boundaryNeighbors.assign(ptCount, std::vector<uint32_t>());
    m_creaseNeighbors.assign(ptCount, std::vector<uint32_t>());
    m_boundaryEdges.clear();

    float cosCreaseThreshold = std::cos(glm::radians(creaseAngleDeg));

    size_t edgeCount = rawEdges.size();
    size_t i = 0;
    while (i < edgeCount) {
        size_t j = i + 1;
        while (j < edgeCount && rawEdges[j].u == rawEdges[i].u && rawEdges[j].v == rawEdges[i].v) {
            ++j;
        }

        uint32_t u = rawEdges[i].u;
        uint32_t v = rawEdges[i].v;
        size_t count = j - i;

        if (u < (uint32_t)ptCount && v < (uint32_t)ptCount) {
            if (count == 1) {
                // Boundary edge
                m_boundaryNeighbors[u].push_back(v);
                m_boundaryNeighbors[v].push_back(u);
                m_boundaryEdges.push_back({ u, v });
            } else if (count == 2 && preserveCreases) {
                uint32_t p1 = rawEdges[i].polyIndex;
                uint32_t p2 = rawEdges[i + 1].polyIndex;
                float dotVal = glm::clamp(glm::dot(polyUnitNormals[p1], polyUnitNormals[p2]), -1.0f, 1.0f);
                if (dotVal < cosCreaseThreshold) {
                    // Dihedral angle exceeds threshold -> Crease edge
                    m_creaseNeighbors[u].push_back(v);
                    m_creaseNeighbors[v].push_back(u);
                }
            }
        }

        i = j;
    }

    return true;
}

bool RelaxEngine::isCursorNearBorder(
    const Mesh* mesh,
    const glm::vec3& center,
    float brushRadius
) const {
    if (!mesh || m_boundaryEdges.empty() || brushRadius <= 0.0f) {
        return false;
    }

    const float* pts = mesh->verts.data();
    float minBorderDistSq = std::numeric_limits<float>::max();

    for (const auto& be : m_boundaryEdges) {
        if (be.u >= (uint32_t)mesh->nbVerts || be.v >= (uint32_t)mesh->nbVerts) continue;

        glm::vec3 sA(pts[be.u * 3], pts[be.u * 3 + 1], pts[be.u * 3 + 2]);
        glm::vec3 sB(pts[be.v * 3], pts[be.v * 3 + 1], pts[be.v * 3 + 2]);

        float dSq = distSqPointToSegment(center, sA, sB);
        if (dSq < minBorderDistSq) {
            minBorderDistSq = dSq;
        }
    }

    float radiusSq = brushRadius * brushRadius;
    if (minBorderDistSq <= radiusSq) {
        float thresh = brushRadius * 0.4f;
        if (minBorderDistSq <= thresh * thresh) {
            return true;
        }
    }

    return false;
}

int RelaxEngine::relaxVertices(
    Mesh* mesh,
    const std::vector<uint32_t>& pickedVertices,
    const glm::vec3& center,
    float radius,
    float intensity,
    const BrushSettings& settings
) {
    if (!mesh || pickedVertices.empty() || radius <= 0.0f || intensity <= 0.0f) {
        return 0;
    }

    int ptCount = mesh->nbVerts;
    int polyCount = mesh->nbFaces;
    if (ptCount < 3 || polyCount == 0) return 0;

    if (!ensureMeshTopology(mesh, settings.relaxPreserveCreases, settings.relaxCreaseAngle)) {
        return 0;
    }

    // 1. Determine locking mode
    bool lockBorder = false;
    bool lockInterior = false;

    if (settings.relaxMode == RELAX_MODE_INTERIOR) {
        lockBorder = true;
        lockInterior = false;
    } else if (settings.relaxMode == RELAX_MODE_BORDER) {
        lockBorder = false;
        lockInterior = true;
    } else if (settings.relaxMode == RELAX_MODE_ALL) {
        lockBorder = settings.relaxLockBorder;
        lockInterior = settings.relaxLockInterior;
    } else { // RELAX_MODE_AUTOLOCK
        bool startOnBorder = isCursorNearBorder(mesh, center, radius);
        if (startOnBorder) {
            lockBorder = false;
            lockInterior = true;
        } else {
            lockBorder = true;
            lockInterior = false;
        }
    }

    float* pts = mesh->verts.data();
    const float* materials = mesh->materials.data();
    float radiusSq = radius * radius;

    // 2. Filter affected vertices and compute per-vertex weights
    std::vector<uint32_t> affectedVertices;
    std::vector<float> vertexWeights;
    affectedVertices.reserve(pickedVertices.size());
    vertexWeights.reserve(pickedVertices.size());

    float p = (1.0f - settings.focalShift) / 2.0f;
    float pSq = p * p;

    for (uint32_t id : pickedVertices) {
        if (id >= (uint32_t)ptCount) continue;

        int ind = id * 3;
        float vx = pts[ind];
        float vy = pts[ind + 1];
        float vz = pts[ind + 2];

        float dx = vx - center.x;
        float dy = vy - center.y;
        float dz = vz - center.z;
        float distSq = (dx * dx + dy * dy + dz * dz) / radiusSq;
        if (distSq >= 1.0f) continue;

        float fallOff = 1.0f;
        if (distSq < pSq) {
            fallOff = 1.0f;
        } else if (p >= 1.0f) {
            continue;
        } else {
            float dist = std::sqrt(distSq);
            float d = (dist - p) / (1.0f - p);
            float d2 = d * d;
            fallOff = 3.0f * d2 * d2 - 4.0f * d2 * d + 1.0f;
        }

        float maskWeight = (materials && (size_t)(ind + 2) < mesh->materials.size()) ? materials[ind + 2] : 1.0f;
        float w = std::min(1.0f, std::max(0.0f, intensity * fallOff * maskWeight));
        if (w <= 0.0001f) continue;

        affectedVertices.push_back(id);
        vertexWeights.push_back(w);
    }

    if (affectedVertices.empty()) return 0;

    // 3. Multi-pass smoothing iterations
    int clampedIters = std::clamp(settings.relaxIterations, 1, 20);
    int algorithm = settings.relaxAlgorithm;
    bool preserveCreases = settings.relaxPreserveCreases;

    std::vector<glm::vec3> nextPositions(affectedVertices.size());

    for (int iter = 0; iter < clampedIters; ++iter) {
        for (size_t ai = 0; ai < affectedVertices.size(); ++ai) {
            uint32_t idx = affectedVertices[ai];
            float w = vertexWeights[ai];

            glm::vec3 currentPos(pts[idx * 3], pts[idx * 3 + 1], pts[idx * 3 + 2]);

            const auto& bNeighbors = m_boundaryNeighbors[idx];
            const auto& cNeighbors = m_creaseNeighbors[idx];

            bool isBoundary = !bNeighbors.empty();
            bool isCrease = (preserveCreases && !cNeighbors.empty() && !isBoundary);

            // =================================================================
            // 1. Boundary Vertex
            // =================================================================
            if (isBoundary) {
                if (lockBorder) {
                    nextPositions[ai] = currentPos;
                    continue;
                }

                if (bNeighbors.size() == 2) {
                    uint32_t nb0 = bNeighbors[0];
                    uint32_t nb1 = bNeighbors[1];
                    glm::vec3 pA(pts[nb0 * 3], pts[nb0 * 3 + 1], pts[nb0 * 3 + 2]);
                    glm::vec3 pB(pts[nb1 * 3], pts[nb1 * 3 + 1], pts[nb1 * 3 + 2]);
                    glm::vec3 targetPos = (pA + pB) * 0.5f;

                    if (algorithm == RELAX_ALGO_TANGENTIAL || algorithm == RELAX_ALGO_PROJECT) {
                        glm::vec3 segDir = pB - pA;
                        float segLen = glm::length(segDir);
                        if (segLen > 1e-6f) {
                            segDir /= segLen;
                            glm::vec3 delta = targetPos - currentPos;
                            targetPos = currentPos + segDir * glm::dot(delta, segDir);
                        }
                    }

                    glm::vec3 candPos = currentPos + (targetPos - currentPos) * w;

                    if (algorithm == RELAX_ALGO_PROJECT && (int)m_initialPositions.size() == ptCount) {
                        glm::vec3 iA = m_initialPositions[nb0];
                        glm::vec3 iB = m_initialPositions[nb1];
                        glm::vec3 ab = iB - iA;
                        float abLenSq = glm::dot(ab, ab);
                        if (abLenSq > 1e-8f) {
                            float t = glm::clamp(glm::dot(candPos - iA, ab) / abLenSq, 0.0f, 1.0f);
                            candPos = iA + ab * t;
                        }
                    }
                    nextPositions[ai] = candPos;
                } else {
                    // Boundary corner (1 or >2 boundary edges) -> keep fixed
                    nextPositions[ai] = currentPos;
                }
                continue;
            }

            // =================================================================
            // 2. Crease / Feature Vertex
            // =================================================================
            if (isCrease) {
                if (lockInterior) {
                    nextPositions[ai] = currentPos;
                    continue;
                }

                if (cNeighbors.size() == 2) {
                    uint32_t nb0 = cNeighbors[0];
                    uint32_t nb1 = cNeighbors[1];
                    glm::vec3 pA(pts[nb0 * 3], pts[nb0 * 3 + 1], pts[nb0 * 3 + 2]);
                    glm::vec3 pB(pts[nb1 * 3], pts[nb1 * 3 + 1], pts[nb1 * 3 + 2]);
                    glm::vec3 targetPos = (pA + pB) * 0.5f;

                    glm::vec3 segDir = pB - pA;
                    float segLen = glm::length(segDir);
                    if (segLen > 1e-6f) {
                        segDir /= segLen;
                        glm::vec3 delta = targetPos - currentPos;
                        targetPos = currentPos + segDir * glm::dot(delta, segDir);
                    }

                    glm::vec3 candPos = currentPos + (targetPos - currentPos) * w;

                    if (algorithm == RELAX_ALGO_PROJECT && (int)m_initialPositions.size() == ptCount) {
                        glm::vec3 iA = m_initialPositions[nb0];
                        glm::vec3 iB = m_initialPositions[nb1];
                        glm::vec3 ab = iB - iA;
                        float abLenSq = glm::dot(ab, ab);
                        if (abLenSq > 1e-8f) {
                            float t = glm::clamp(glm::dot(candPos - iA, ab) / abLenSq, 0.0f, 1.0f);
                            candPos = iA + ab * t;
                        }
                    }
                    nextPositions[ai] = candPos;
                } else {
                    // Crease junction / end -> keep fixed
                    nextPositions[ai] = currentPos;
                }
                continue;
            }

            // =================================================================
            // 3. Smooth Interior Vertex
            // =================================================================
            if (lockInterior) {
                nextPositions[ai] = currentPos;
                continue;
            }

            uint32_t vStart = mesh->vrvStartCount[idx * 2];
            uint32_t vCount = mesh->vrvStartCount[idx * 2 + 1];
            if (vCount == 0) {
                nextPositions[ai] = currentPos;
                continue;
            }

            glm::vec3 sum(0.0f);
            for (uint32_t ni = 0; ni < vCount; ++ni) {
                uint32_t neighborIdx = mesh->vertRingVert[vStart + ni];
                sum += glm::vec3(pts[neighborIdx * 3], pts[neighborIdx * 3 + 1], pts[neighborIdx * 3 + 2]);
            }
            glm::vec3 centroid = sum / (float)vCount;
            glm::vec3 disp = centroid - currentPos;
            glm::vec3 targetPos = centroid;

            if (algorithm == RELAX_ALGO_TANGENTIAL || algorithm == RELAX_ALGO_PROJECT) {
                // Calculate current vertex normal from incident polygons
                glm::vec3 vNormal(0.0f);
                uint32_t fStart = mesh->vrfStartCount[idx * 2];
                uint32_t fCount = mesh->vrfStartCount[idx * 2 + 1];

                for (uint32_t fi = 0; fi < fCount; ++fi) {
                    uint32_t polyIdx = mesh->vertRingFace[fStart + fi];
                    uint32_t pa = mesh->faces[polyIdx * 4 + 0];
                    uint32_t pb = mesh->faces[polyIdx * 4 + 1];
                    uint32_t pc = mesh->faces[polyIdx * 4 + 2];
                    uint32_t pd = mesh->faces[polyIdx * 4 + 3];

                    glm::vec3 A(pts[pa * 3], pts[pa * 3 + 1], pts[pa * 3 + 2]);
                    glm::vec3 B(pts[pb * 3], pts[pb * 3 + 1], pts[pb * 3 + 2]);
                    glm::vec3 C(pts[pc * 3], pts[pc * 3 + 1], pts[pc * 3 + 2]);
                    glm::vec3 N = glm::cross(B - A, C - A);
                    if (pd != 0xffffffff) {
                        glm::vec3 D(pts[pd * 3], pts[pd * 3 + 1], pts[pd * 3 + 2]);
                        N += glm::cross(C - A, D - A);
                    }
                    vNormal += N;
                }

                float normLen = glm::length(vNormal);
                if (normLen > 1e-6f) {
                    vNormal /= normLen;
                    float normalDisp = glm::dot(disp, vNormal);
                    glm::vec3 tangentialDisp = disp - vNormal * normalDisp;
                    targetPos = currentPos + tangentialDisp;
                }
            }

            glm::vec3 candPos = currentPos + (targetPos - currentPos) * w;

            // =================================================================
            // 4. Project to Initial Surface (RELAX_ALGO_PROJECT)
            // =================================================================
            if (algorithm == RELAX_ALGO_PROJECT && (int)m_initialPositions.size() == ptCount) {
                float minDistSq = std::numeric_limits<float>::max();
                glm::vec3 bestPoint = candPos;

                auto testPoly = [&](uint32_t pIdx) {
                    if (pIdx >= (uint32_t)polyCount) return;
                    uint32_t pa = mesh->faces[pIdx * 4 + 0];
                    uint32_t pb = mesh->faces[pIdx * 4 + 1];
                    uint32_t pc = mesh->faces[pIdx * 4 + 2];
                    uint32_t pd = mesh->faces[pIdx * 4 + 3];

                    glm::vec3 a0 = m_initialPositions[pa];
                    glm::vec3 b0 = m_initialPositions[pb];
                    glm::vec3 c0 = m_initialPositions[pc];

                    glm::vec3 q1 = closestPointOnTriangle(candPos, a0, b0, c0);
                    glm::vec3 diff1 = candPos - q1;
                    float d1Sq = glm::dot(diff1, diff1);
                    if (d1Sq < minDistSq) {
                        minDistSq = d1Sq;
                        bestPoint = q1;
                    }

                    if (pd != 0xffffffff) {
                        glm::vec3 d0 = m_initialPositions[pd];
                        glm::vec3 q2 = closestPointOnTriangle(candPos, a0, c0, d0);
                        glm::vec3 diff2 = candPos - q2;
                        float d2Sq = glm::dot(diff2, diff2);
                        if (d2Sq < minDistSq) {
                            minDistSq = d2Sq;
                            bestPoint = q2;
                        }
                    }
                };

                // Test 1-ring incident polygons
                uint32_t fStart = mesh->vrfStartCount[idx * 2];
                uint32_t fCount = mesh->vrfStartCount[idx * 2 + 1];
                for (uint32_t fi = 0; fi < fCount; ++fi) {
                    testPoly(mesh->vertRingFace[fStart + fi]);
                }

                // Also test 2-ring incident polygons from direct neighbors
                for (uint32_t ni = 0; ni < vCount; ++ni) {
                    uint32_t neighborIdx = mesh->vertRingVert[vStart + ni];
                    uint32_t nfStart = mesh->vrfStartCount[neighborIdx * 2];
                    uint32_t nfCount = mesh->vrfStartCount[neighborIdx * 2 + 1];
                    for (uint32_t nfi = 0; nfi < nfCount; ++nfi) {
                        testPoly(mesh->vertRingFace[nfStart + nfi]);
                    }
                }

                candPos = bestPoint;
            }

            nextPositions[ai] = candPos;
        }

        // Apply iteration results to pts
        for (size_t ai = 0; ai < affectedVertices.size(); ++ai) {
            uint32_t idx = affectedVertices[ai];
            pts[idx * 3]     = nextPositions[ai].x;
            pts[idx * 3 + 1] = nextPositions[ai].y;
            pts[idx * 3 + 2] = nextPositions[ai].z;
        }
    }

    return (int)affectedVertices.size();
}
