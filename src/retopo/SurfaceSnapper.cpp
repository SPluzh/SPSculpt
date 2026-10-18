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
    if (normLen < 1e-4f) return result;
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
    const SnapResult* precomputedTargetSnap
) {
    if (!retopoMesh || retopoMesh->nbVerts == 0) return -1;

    const int ptCount = retopoMesh->nbVerts;
    const auto& verts = retopoMesh->verts;
    const glm::mat4& mat = retopoMesh->matrix;
    const bool hasTransform = (mat != glm::mat4(1.0f));

    float bestDistSq = maxRadiusPixels * maxRadiusPixels;
    int32_t bestIdx = -1;

    glm::vec3 camPos = camera.computePosition();
    bool isOrtho = camera.isOrthographic();

    for (int i = 0; i < ptCount; ++i) {
        if (i == excludeIndex) continue;
        if (!retopoMesh->vertVisible.empty() && !retopoMesh->vertVisible[i]) continue;

        glm::vec3 localPos(verts[i * 3 + 0], verts[i * 3 + 1], verts[i * 3 + 2]);
        glm::vec3 worldPos = hasTransform ? glm::vec3(mat * glm::vec4(localPos, 1.0f)) : localPos;

        glm::vec3 sPos = camera.project(worldPos);
        if (sPos.z < 0.0f || sPos.z > 1.0f) continue; // Behind camera or outside depth range

        float dx = sPos.x - screenX;
        float dy = sPos.y - screenY;
        float dSq = dx * dx + dy * dy;

        if (dSq <= bestDistSq) {
            // Occlusion check with targetMesh
            if (targetMesh && targetMesh->nbFaces > 0) {
                SnapResult tSnap;
                if (precomputedTargetSnap && precomputedTargetSnap->valid &&
                    std::abs(precomputedTargetSnap->screenPos.x - sPos.x) < 5.0f &&
                    std::abs(precomputedTargetSnap->screenPos.y - sPos.y) < 5.0f) {
                    tSnap = *precomputedTargetSnap;
                } else {
                    tSnap = RaycastSurface(camera, targetMesh, sPos.x, sPos.y);
                }

                if (tSnap.valid) {
                    float tZ = tSnap.screenPos.z;
                    if (sPos.z > tZ + 0.005f) {
                        continue; // Occluded by targetMesh!
                    }
                }
            }

            bestDistSq = dSq;
            bestIdx = i;
        }
    }

    return bestIdx;
}

EdgeHit SurfaceSnapper::FindNearestRetopoEdge(
    const Camera& camera, const Mesh* retopoMesh,
    float screenX, float screenY, float maxRadiusPixels,
    const Mesh* targetMesh,
    const SnapResult* precomputedTargetSnap
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

    float bestDist = maxRadiusPixels;

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
            if (u > v) continue; // Each undirected edge checked once

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
                        tSnap = RaycastSurface(camera, targetMesh, edgeScreenMid.x, edgeScreenMid.y);
                    }

                    if (tSnap.valid) {
                        float tZ = tSnap.screenPos.z;
                        if (edgeScreenMid.z > tZ + 0.005f) {
                            continue; // Edge is occluded
                        }
                    }
                }

                bestDist = d;
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

    return result;
}

SnapResult SurfaceSnapper::Snap(
    const Camera& camera, const Mesh* targetMesh, const Mesh* retopoMesh,
    float screenX, float screenY, float snapRadiusPixels
) {
    SnapResult targetSnap = RaycastSurface(camera, targetMesh, screenX, screenY);

    if (retopoMesh && retopoMesh->nbVerts > 0) {
        int32_t nearV = FindNearestRetopoVertex(
            camera, retopoMesh, screenX, screenY, snapRadiusPixels, -1, targetMesh, &targetSnap
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
