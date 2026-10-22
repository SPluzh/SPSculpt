#include "retopo/SurfaceSnapper.h"
#include "common/Logger.h"
#include <algorithm>
#include <cmath>

namespace retopo {

SurfaceSnapper::SurfaceSnapper() = default;
SurfaceSnapper::~SurfaceSnapper() = default;

void SurfaceSnapper::ClearCache() {
    m_cachedRaycast.target = nullptr;
    m_cachedRaycast.screenX = -1e30f;
    m_cachedRaycast.screenY = -1e30f;
    m_cachedRaycast.vertGen = 0;
    m_cachedRaycast.result = SnapResult();
    m_bvh.Clear();
}

SnapResult SurfaceSnapper::RaycastSurface(
    const Camera& camera, const Mesh* targetMesh, float screenX, float screenY
) {
    SnapResult result;
    if (!targetMesh || targetMesh->nbFaces == 0 || targetMesh->nbVerts == 0)
        return result;

    if (m_bvh.NeedsRebuild(targetMesh)) {
        if (!m_bvh.Build(targetMesh)) {
            return result;
        }
    }

    Ray ray = camera.getRay(screenX, screenY);
    glm::vec3 hitPoint{0.0f};
    glm::vec3 hitNormal{0.0f, 1.0f, 0.0f};

    if (m_bvh.Raycast(ray.origin, ray.dir, hitPoint, hitNormal, 1e30f, false)) {
        result.valid = true;
        result.mode = SnapMode::TargetSurface;
        result.worldPos = hitPoint;
        result.normal = hitNormal;
        result.screenPos = camera.project(hitPoint);
    }

    return result;
}

SnapResult SurfaceSnapper::ProjectPointAlongNormal(
    const Mesh* targetMesh, const glm::vec3& worldPoint, const glm::vec3& worldNormal, float maxDist
) {
    SnapResult result;
    if (!targetMesh || targetMesh->nbFaces == 0 || targetMesh->nbVerts == 0)
        return result;

    float normLen = glm::length(worldNormal);
    if (normLen < 1e-4f) return SnapClosestPoint(targetMesh, worldPoint, maxDist);
    glm::vec3 norm = worldNormal / normLen;

    if (m_bvh.NeedsRebuild(targetMesh)) {
        if (!m_bvh.Build(targetMesh)) return result;
    }

    float offset = std::min(100.0f, maxDist * 0.5f);
    if (offset < 1.0f) offset = 1.0f;

    // Ray 1: Pass-through ray along normal through worldPoint
    glm::vec3 rayStart = worldPoint + norm * offset;
    glm::vec3 hitPt{0.0f};
    glm::vec3 hitNorm{0.0f, 1.0f, 0.0f};

    if (m_bvh.Raycast(rayStart, -norm, hitPt, hitNorm, offset * 2.0f, false)) {
        result.valid = true;
        result.mode = SnapMode::TargetSurface;
        result.worldPos = hitPt;
        result.normal = hitNorm;
        return result;
    }

    // Ray 2: Bidirectional ray along normal from worldPoint
    if (m_bvh.Raycast(worldPoint, norm, hitPt, hitNorm, maxDist, true)) {
        result.valid = true;
        result.mode = SnapMode::TargetSurface;
        result.worldPos = hitPt;
        result.normal = hitNorm;
        return result;
    }

    // Fallback: Closest point on mesh within maxDist
    return SnapClosestPoint(targetMesh, worldPoint, maxDist);
}

SnapResult SurfaceSnapper::SnapClosestPoint(
    const Mesh* targetMesh, const glm::vec3& worldPoint, float maxDist
) {
    SnapResult result;
    if (!targetMesh || targetMesh->nbFaces == 0 || targetMesh->nbVerts == 0)
        return result;

    if (m_bvh.NeedsRebuild(targetMesh)) {
        if (!m_bvh.Build(targetMesh)) return result;
    }

    glm::vec3 closestPt{0.0f};
    glm::vec3 closestNorm{0.0f, 1.0f, 0.0f};
    if (m_bvh.FindClosestPoint(worldPoint, closestPt, closestNorm, maxDist)) {
        result.valid = true;
        result.mode = SnapMode::TargetSurface;
        result.worldPos = closestPt;
        result.normal = closestNorm;
        return result;
    }

    return result;
}

int32_t SurfaceSnapper::FindNearestRetopoVertex(
    const Camera& camera, const Mesh* retopoMesh,
    float screenX, float screenY, float maxRadiusPixels,
    int32_t excludeIndex, const Mesh* targetMesh,
    const SnapResult* precomputedTargetSnap,
    const glm::vec3* sourceWorldPos,
    float maxWorldDistance
) {
    if (!retopoMesh || retopoMesh->nbVerts == 0) return -1;

    const int ptCount = retopoMesh->nbVerts;
    const int numFaces = retopoMesh->nbFaces;
    const auto& faces = retopoMesh->faces;
    const auto& verts = retopoMesh->verts;
    const glm::mat4& mat = retopoMesh->matrix;
    const bool hasTransform = (mat != glm::mat4(1.0f));

    auto getVertWorld = [&](uint32_t idx) -> glm::vec3 {
        glm::vec3 p(verts[idx * 3 + 0], verts[idx * 3 + 1], verts[idx * 3 + 2]);
        return hasTransform ? glm::vec3(mat * glm::vec4(p, 1.0f)) : p;
    };

    glm::vec3 camPos = camera.computePosition();
    bool isOrtho = camera.isOrthographic();
    glm::vec3 orthoDir = isOrtho ? glm::vec3(camera.getViewMatrix()[0][2], camera.getViewMatrix()[1][2], camera.getViewMatrix()[2][2]) : glm::vec3(0.0f);

    // Precompute vertex face counts & front-facing face counts to cull backface vertices
    std::vector<int32_t> vertPolyCount(ptCount, 0);
    std::vector<int32_t> vertFrontFacingCount(ptCount, 0);

    for (int f = 0; f < numFaces; ++f) {
        if (!retopoMesh->faceVisible.empty() && !retopoMesh->faceVisible[f]) continue;

        uint32_t q[4] = { faces[f * 4 + 0], faces[f * 4 + 1], faces[f * 4 + 2], faces[f * 4 + 3] };
        int numE = (q[3] != TRI_INDEX) ? 4 : 3;

        glm::vec3 p0 = getVertWorld(q[0]);
        glm::vec3 p1 = getVertWorld(q[1]);
        glm::vec3 p2 = getVertWorld(q[2]);
        glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
        float lenN = glm::length(n);
        if (lenN < 1e-8f && numE == 4) {
            n = glm::cross(p2 - p0, getVertWorld(q[3]) - p0);
            lenN = glm::length(n);
        }

        glm::vec3 center = (numE == 4) ? (p0 + p1 + p2 + getVertWorld(q[3])) * 0.25f : (p0 + p1 + p2) * 0.333333f;
        glm::vec3 viewVec = isOrtho ? orthoDir : (camPos - center);

        bool isFront = (lenN < 1e-8f) || (glm::dot(n, viewVec) > 0.0f);

        for (int e = 0; e < numE; ++e) {
            uint32_t vi = q[e];
            if (vi < (uint32_t)ptCount) {
                vertPolyCount[vi]++;
                if (isFront) {
                    vertFrontFacingCount[vi]++;
                }
            }
        }
    }

    // If sourceWorldPos is provided, compute world distance threshold corresponding to maxRadiusPixels
    float threshold3D = maxWorldDistance;
    glm::vec3 srcScreen(0.0f);
    if (sourceWorldPos) {
        srcScreen = camera.project(*sourceWorldPos);
        if (threshold3D <= 0.0f) {
            glm::vec3 pOff = camera.unproject(srcScreen.x + maxRadiusPixels * 2.5f, srcScreen.y, srcScreen.z);
            threshold3D = glm::distance(pOff, *sourceWorldPos);
            if (threshold3D < 0.001f) threshold3D = 50.0f;
        }
    }

    float bestDistSq = maxRadiusPixels * maxRadiusPixels;
    float bestZ = 2.0f;
    int32_t bestIdx = -1;

    for (int i = 0; i < ptCount; ++i) {
        if (i == excludeIndex) continue;
        if (!retopoMesh->vertVisible.empty() && !retopoMesh->vertVisible[i]) continue;

        // 1. Backface check on retopo mesh: if vertex has faces and none are front-facing, it's on the back side
        if (vertPolyCount[i] > 0 && vertFrontFacingCount[i] == 0) {
            continue;
        }

        glm::vec3 worldPos = getVertWorld(i);

        // 2. Isolated vertex (no faces yet): check against targetMesh surface normal
        if (vertPolyCount[i] == 0 && targetMesh && targetMesh->nbFaces > 0) {
            SnapResult sc = const_cast<SurfaceSnapper*>(this)->SnapClosestPoint(targetMesh, worldPos, 200.0f);
            if (sc.valid) {
                glm::vec3 viewVec = isOrtho ? orthoDir : (camPos - worldPos);
                if (glm::dot(sc.normal, viewVec) <= 0.0f) {
                    continue; // Back-facing on targetMesh surface
                }
            }
        }

        glm::vec3 sPos = camera.project(worldPos);
        if (sPos.z < 0.0f || sPos.z > 1.0f) continue; // Behind camera or outside depth range

        float dx = sPos.x - screenX;
        float dy = sPos.y - screenY;
        float dSq = dx * dx + dy * dy;

        if (dSq > bestDistSq) continue;

        // 3. Occlusion check with targetMesh
        if (targetMesh && targetMesh->nbFaces > 0) {
            SnapResult tSnap;
            if (precomputedTargetSnap && precomputedTargetSnap->valid &&
                std::abs(precomputedTargetSnap->screenPos.x - sPos.x) < 5.0f &&
                std::abs(precomputedTargetSnap->screenPos.y - sPos.y) < 5.0f) {
                tSnap = *precomputedTargetSnap;
            } else {
                tSnap = const_cast<SurfaceSnapper*>(this)->RaycastSurface(camera, targetMesh, sPos.x, sPos.y);
            }

            if (tSnap.valid) {
                float tZ = tSnap.screenPos.z;
                if (sPos.z > tZ + 0.005f) {
                    continue; // Occluded by targetMesh!
                }
            } else {
                // Secondary occlusion check: direct ray from camera to worldPos
                glm::vec3 toVert = worldPos - camPos;
                float distToVert = glm::length(toVert);
                if (distToVert > 1e-4f) {
                    glm::vec3 rayDir = toVert / distToVert;
                    glm::vec3 hitPt{0.0f}, hitN{0.0f};
                    if (m_bvh.Raycast(camPos, rayDir, hitPt, hitN, distToVert - 0.02f, false)) {
                        continue; // Occluded by targetMesh!
                    }
                }
            }
        }

        // 4. Source point proximity and depth check (when dragging/tweaking)
        if (sourceWorldPos) {
            // Candidate vertex cannot be noticeably behind the moving source vertex in depth
            if (sPos.z > srcScreen.z + 0.015f) {
                continue;
            }

            // 3D world distance check
            float dist3D = glm::distance(worldPos, *sourceWorldPos);
            if (dist3D > threshold3D) {
                continue;
            }
        }

        // 5. Depth-prioritized selection: front vertices always take precedence over deeper ones
        bool isBetter = false;
        if (bestIdx == -1) {
            isBetter = true;
        } else if (sPos.z < bestZ - 0.01f) {
            // Significantly closer in depth to camera
            isBetter = true;
        } else if (std::abs(sPos.z - bestZ) <= 0.01f) {
            // Same depth layer, pick closer in screen space
            if (dSq < bestDistSq) {
                isBetter = true;
            }
        }

        if (isBetter) {
            bestDistSq = dSq;
            bestZ = sPos.z;
            bestIdx = i;
        }
    }

    return bestIdx;
}

EdgeHit SurfaceSnapper::FindNearestRetopoEdge(
    const Camera& camera, const Mesh* retopoMesh,
    float screenX, float screenY, float maxRadiusPixels,
    const Mesh* targetMesh,
    const SnapResult* precomputedTargetSnap,
    bool borderOnly
) {
    EdgeHit result;
    if (!retopoMesh || retopoMesh->nbFaces == 0 || retopoMesh->nbVerts == 0) return result;

    const int numFaces = retopoMesh->nbFaces;
    const int numVerts = retopoMesh->nbVerts;
    const auto& faces = retopoMesh->faces;
    const auto& verts = retopoMesh->verts;
    const glm::mat4& mat = retopoMesh->matrix;
    const bool hasTransform = (mat != glm::mat4(1.0f));

    auto getVertWorld = [&](uint32_t idx) -> glm::vec3 {
        glm::vec3 p(verts[idx * 3], verts[idx * 3 + 1], verts[idx * 3 + 2]);
        return hasTransform ? glm::vec3(mat * glm::vec4(p, 1.0f)) : p;
    };

    glm::vec3 camPos = camera.computePosition();
    bool isOrtho = camera.isOrthographic();
    glm::vec3 orthoDir = isOrtho ? glm::vec3(camera.getViewMatrix()[0][2], camera.getViewMatrix()[1][2], camera.getViewMatrix()[2][2]) : glm::vec3(0.0f);

    // Precompute edge face counts & front-facing counts to filter backface edges
    std::unordered_map<uint64_t, int32_t> edgeFaceCounts;
    std::unordered_map<uint64_t, int32_t> edgeFrontFacingCounts;

    for (int f = 0; f < numFaces; ++f) {
        if (!retopoMesh->faceVisible.empty() && !retopoMesh->faceVisible[f]) continue;
        uint32_t fv[4] = { faces[f * 4 + 0], faces[f * 4 + 1], faces[f * 4 + 2], faces[f * 4 + 3] };
        int numE = (fv[3] != TRI_INDEX) ? 4 : 3;

        glm::vec3 p0 = getVertWorld(fv[0]);
        glm::vec3 p1 = getVertWorld(fv[1]);
        glm::vec3 p2 = getVertWorld(fv[2]);
        glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
        glm::vec3 center = (numE == 4) ? (p0 + p1 + p2 + getVertWorld(fv[3])) * 0.25f : (p0 + p1 + p2) * 0.333333f;
        glm::vec3 viewVec = isOrtho ? orthoDir : (camPos - center);
        bool isFront = (glm::dot(n, viewVec) > 0.0f);

        for (int e = 0; e < numE; ++e) {
            uint32_t eu = fv[e];
            uint32_t ev = fv[(e + 1) % numE];
            if (eu >= (uint32_t)numVerts || ev >= (uint32_t)numVerts) continue;
            uint64_t k = (static_cast<uint64_t>(std::min(eu, ev)) << 32) | static_cast<uint64_t>(std::max(eu, ev));
            edgeFaceCounts[k]++;
            if (isFront) {
                edgeFrontFacingCounts[k]++;
            }
        }
    }

    float bestDist = maxRadiusPixels;
    float bestZ = 2.0f;
    std::unordered_set<uint64_t> visitedEdges;

    for (int f = 0; f < numFaces; ++f) {
        if (!retopoMesh->faceVisible.empty() && !retopoMesh->faceVisible[f]) continue;

        uint32_t fv[4] = {
            faces[f * 4 + 0],
            faces[f * 4 + 1],
            faces[f * 4 + 2],
            faces[f * 4 + 3]
        };

        int numEdges = (fv[3] != TRI_INDEX) ? 4 : 3;
        for (int e = 0; e < numEdges; ++e) {
            uint32_t u = fv[e];
            uint32_t v = fv[(e + 1) % numEdges];
            if (u >= (uint32_t)numVerts || v >= (uint32_t)numVerts) continue;

            uint64_t edgeKey = (static_cast<uint64_t>(std::min(u, v)) << 32) | static_cast<uint64_t>(std::max(u, v));
            if (!visitedEdges.insert(edgeKey).second) continue; // Each unique edge checked once

            // Backface culling: skip edge if all incident faces face away
            auto ffIt = edgeFrontFacingCounts.find(edgeKey);
            if (ffIt == edgeFrontFacingCounts.end() || ffIt->second == 0) {
                continue;
            }

            if (borderOnly) {
                auto it = edgeFaceCounts.find(edgeKey);
                if (it != edgeFaceCounts.end() && it->second > 1) {
                    continue; // Skip interior edges!
                }
            }

            glm::vec3 w0 = getVertWorld(u);
            glm::vec3 w1 = getVertWorld(v);

            glm::vec3 s0 = camera.project(w0);
            glm::vec3 s1 = camera.project(w1);
            if (s0.z < 0.0f || s0.z > 1.0f || s1.z < 0.0f || s1.z > 1.0f) continue;

            glm::vec2 p0(s0.x, s0.y);
            glm::vec2 p1(s1.x, s1.y);
            glm::vec2 seg = p1 - p0;
            float segLenSq = glm::dot(seg, seg);

            float t = 0.5f;
            float d = 1e30f;
            if (segLenSq < 1e-6f) {
                glm::vec2 diff = glm::vec2(screenX, screenY) - p0;
                d = glm::length(diff);
                t = 0.5f;
            } else {
                glm::vec2 pt(screenX, screenY);
                t = glm::dot(pt - p0, seg) / segLenSq;
                t = std::clamp(t, 0.0f, 1.0f);
                glm::vec2 proj = p0 + t * seg;
                d = glm::length(pt - proj);
            }

            if (d < bestDist) {
                // Occlusion check at mid-edge
                glm::vec3 edgeWorldMid = glm::mix(w0, w1, t);
                glm::vec3 edgeScreenMid = camera.project(edgeWorldMid);

                if (targetMesh && targetMesh->nbFaces > 0) {
                    SnapResult tSnap;
                    if (precomputedTargetSnap && precomputedTargetSnap->valid &&
                        std::abs(precomputedTargetSnap->screenPos.x - edgeScreenMid.x) < 5.0f &&
                        std::abs(precomputedTargetSnap->screenPos.y - edgeScreenMid.y) < 5.0f) {
                        tSnap = *precomputedTargetSnap;
                    } else {
                        tSnap = const_cast<SurfaceSnapper*>(this)->RaycastSurface(camera, targetMesh, edgeScreenMid.x, edgeScreenMid.y);
                    }

                    if (tSnap.valid) {
                        float tZ = tSnap.screenPos.z;
                        if (edgeScreenMid.z > tZ + 0.005f) {
                            continue; // Edge is occluded on back of mesh
                        }
                    } else {
                        glm::vec3 toEdge = edgeWorldMid - camPos;
                        float distToEdge = glm::length(toEdge);
                        if (distToEdge > 1e-4f) {
                            glm::vec3 rayDir = toEdge / distToEdge;
                            glm::vec3 hitPt{0.0f}, hitN{0.0f};
                            if (m_bvh.Raycast(camPos, rayDir, hitPt, hitN, distToEdge - 0.02f, false)) {
                                continue; // Occluded by targetMesh!
                            }
                        }
                    }
                }

                bool isBetter = false;
                if (!result.valid) {
                    isBetter = true;
                } else if (edgeScreenMid.z < bestZ - 0.01f) {
                    isBetter = true;
                } else if (std::abs(edgeScreenMid.z - bestZ) <= 0.01f) {
                    if (d < bestDist) {
                        isBetter = true;
                    }
                }

                if (isBetter) {
                    bestDist = d;
                    bestZ = edgeScreenMid.z;
                    result.valid = true;
                    result.v0 = (int32_t)u;
                    result.v1 = (int32_t)v;
                    result.worldPos0 = w0;
                    result.worldPos1 = w1;
                    result.t = t;
                    result.dist = d;
                    result.polyIndex = f;
                }
            }
        }
    }

    return result;
}

SnapResult SurfaceSnapper::Snap(
    const Camera& camera, const Mesh* targetMesh, const Mesh* retopoMesh,
    float screenX, float screenY, float snapRadiusPixels
) {
    SnapResult targetSnap = RaycastSurface(camera, targetMesh, screenX, screenY);

    if (retopoMesh && retopoMesh->nbVerts > 0) {
        int32_t nearV = FindNearestRetopoVertex(
            camera, retopoMesh, screenX, screenY, snapRadiusPixels, -1, targetMesh, &targetSnap,
            targetSnap.valid ? &targetSnap.worldPos : nullptr
        );
        if (nearV != -1) {
            SnapResult res;
            res.valid = true;
            res.mode = SnapMode::RetopoVertex;
            res.retopoVertexIndex = nearV;

            glm::vec3 localPos(
                retopoMesh->verts[nearV * 3 + 0],
                retopoMesh->verts[nearV * 3 + 1],
                retopoMesh->verts[nearV * 3 + 2]
            );
            res.worldPos = (retopoMesh->matrix != glm::mat4(1.0f))
                ? glm::vec3(retopoMesh->matrix * glm::vec4(localPos, 1.0f))
                : localPos;
            res.screenPos = camera.project(res.worldPos);
            return res;
        }
    }

    return targetSnap;
}

} // namespace retopo
