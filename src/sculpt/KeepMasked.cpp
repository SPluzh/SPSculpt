#include "sculpt/KeepMasked.h"
#include "common/Constants.h"
#include "common/Logger.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace sculpt {

std::vector<uint8_t> getKeepMaskedTriangles(
    const float* materials,
    int nbVerts,
    const uint32_t* tris,
    int nbTris
) {
    if (!materials || nbVerts <= 0 || !tris || nbTris <= 0) return {};
    std::vector<uint8_t> keep(nbTris, 0);
    for (int t = 0; t < nbTris; ++t) {
        uint32_t v0 = tris[t * 3];
        uint32_t v1 = tris[t * 3 + 1];
        uint32_t v2 = tris[t * 3 + 2];
        if (v0 < (uint32_t)nbVerts && v1 < (uint32_t)nbVerts && v2 < (uint32_t)nbVerts) {
            bool m0 = materials[v0 * 3 + 2] < kKeepMaskedThreshold;
            bool m1 = materials[v1 * 3 + 2] < kKeepMaskedThreshold;
            bool m2 = materials[v2 * 3 + 2] < kKeepMaskedThreshold;
            if (m0 && m1 && m2) {
                keep[t] = 1;
            }
        }
    }
    return keep;
}

namespace {

struct SimpleMesh {
    std::vector<glm::vec3> positions;
    std::vector<glm::uvec3> triangles;
    std::vector<glm::vec3> colors;
    std::vector<glm::vec3> materials; // roughness, metalness, mask
    std::vector<uint32_t> polygroups;
    std::vector<uint32_t> quadPartners;

    size_t vertexCount() const { return positions.size(); }
    size_t triangleCount() const { return triangles.size(); }
};

inline uint64_t directedKey(uint32_t a, uint32_t b) {
    return (uint64_t(a) << 32) | uint64_t(b);
}

inline uint64_t undirectedKey(uint32_t a, uint32_t b) {
    return (a < b) ? ((uint64_t(a) << 32) | uint64_t(b)) : ((uint64_t(b) << 32) | uint64_t(a));
}

// Closed loops of open edges of `tris`, oriented in the direction each triangle runs along it.
// Returns false if open edges form pinches (loops meeting at a vertex or open multiple times).
bool boundaryLoops(const std::vector<glm::uvec3>& tris, std::vector<std::vector<uint32_t>>& loops,
                   std::vector<uint32_t>* pinches = nullptr) {
    loops.clear();
    std::unordered_map<uint64_t, int> used;
    used.reserve(tris.size() * 3);
    for (const glm::uvec3& t : tris) {
        for (int e = 0; e < 3; ++e) {
            if (t[e] != t[(e + 1) % 3]) {
                ++used[directedKey(t[e], t[(e + 1) % 3])];
            }
        }
    }
    std::unordered_map<uint32_t, uint32_t> next;
    std::unordered_map<uint32_t, int> outs, ins;
    bool ok = true;
    for (const auto& [key, n] : used) {
        const uint32_t a = uint32_t(key >> 32), b = uint32_t(key & 0xffffffffu);
        const auto back = used.find(directedKey(b, a));
        const int open = n - (back == used.end() ? 0 : back->second);
        if (open <= 0) continue;
        if (open > 1) {
            ok = false;
            if (pinches) pinches->push_back(a);
        }
        next.emplace(a, b);
        ++outs[a];
        ++ins[b];
    }
    for (const auto& [v, n] : outs) {
        if (n > 1 || ins[v] > 1) {
            ok = false;
            if (pinches) pinches->push_back(v);
        }
    }
    if (!ok) return false;

    std::unordered_set<uint32_t> seen;
    std::vector<uint32_t> starts;
    starts.reserve(next.size());
    for (const auto& [a, b] : next) starts.push_back(a);
    std::sort(starts.begin(), starts.end()); // Deterministic order across platforms
    for (uint32_t start : starts) {
        if (seen.count(start)) continue;
        std::vector<uint32_t> loop;
        uint32_t v = start;
        do {
            if (!seen.insert(v).second) return false;
            loop.push_back(v);
            const auto it = next.find(v);
            if (it == next.end()) return false;
            v = it->second;
        } while (v != start);
        if (loop.size() < 3) return false;
        loops.push_back(std::move(loop));
    }
    return true;
}

// Spatial hash grid nearest point search: for each query point, returns the value of the nearest point in `points`
std::vector<float> nearestValues(const std::vector<glm::vec3>& points, const std::vector<float>& values,
                                 const std::vector<glm::vec3>& query) {
    if (points.empty() || query.empty()) return std::vector<float>(query.size(), -1.0f);

    glm::vec3 boxMin(std::numeric_limits<float>::max());
    glm::vec3 boxMax(-std::numeric_limits<float>::max());
    for (const auto& p : points) {
        boxMin = glm::min(boxMin, p);
        boxMax = glm::max(boxMax, p);
    }
    const glm::vec3 ext = boxMax - boxMin;
    const float longest = std::max({ext.x, ext.y, ext.z, 1e-6f});
    const float cell = std::max(longest / std::max(1.0f, std::cbrt(float(points.size()))), longest * 1e-4f);
    constexpr int kOffset = 1 << 20;

    auto cellOf = [&](const glm::vec3& p) {
        const glm::vec3 f = glm::floor((p - boxMin) / cell);
        auto axis = [](float v) {
            return std::isfinite(v) ? int(std::clamp(v, -float(kOffset), float(kOffset))) : 0;
        };
        return glm::ivec3(axis(f.x), axis(f.y), axis(f.z));
    };

    auto keyOf = [](const glm::ivec3& c) {
        return (uint64_t(uint32_t(c.x + kOffset) & 0x1fffffu) << 42) |
               (uint64_t(uint32_t(c.y + kOffset) & 0x1fffffu) << 21) |
               uint64_t(uint32_t(c.z + kOffset) & 0x1fffffu);
    };

    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    grid.reserve(points.size());
    for (uint32_t v = 0; v < (uint32_t)points.size(); ++v) {
        grid[keyOf(cellOf(points[v]))].push_back(v);
    }
    const int gridCells = int(std::ceil(longest / cell)) + 2;

    std::vector<float> out(query.size(), -1.0f);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int i = 0; i < (int)query.size(); ++i) {
        const glm::vec3 p = query[i];
        const glm::ivec3 c = cellOf(p);
        const glm::vec3 outside = glm::max(glm::max(boxMin - p, p - boxMax), glm::vec3(0.0f));
        const int maxRing = gridCells + int(std::ceil(glm::length(outside) / cell));
        float best = std::numeric_limits<float>::max();
        uint32_t bestV = 0;

        for (int r = 0; r <= maxRing; ++r) {
            for (int dz = -r; dz <= r; ++dz) {
                for (int dy = -r; dy <= r; ++dy) {
                    for (int dx = -r; dx <= r; ++dx) {
                        if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != r) continue;
                        auto it = grid.find(keyOf(c + glm::ivec3(dx, dy, dz)));
                        if (it == grid.end()) continue;
                        for (uint32_t v : it->second) {
                            const glm::vec3 d = points[v] - p;
                            const float d2 = glm::dot(d, d);
                            if (d2 < best) {
                                best = d2;
                                bestV = v;
                            }
                        }
                    }
                }
            }
            if (best < std::numeric_limits<float>::max() && std::sqrt(best) <= float(r) * cell) break;
        }
        if (bestV < values.size()) {
            out[i] = values[bestV];
        }
    }
    return out;
}

// Extracts kept triangles and their unique vertices
SimpleMesh keptPiece(const SimpleMesh& source, const std::vector<uint8_t>& keep, std::vector<uint32_t>& sourceVertex) {
    const size_t nt = source.triangleCount();
    std::vector<uint32_t> remap(source.vertexCount(), ~0u), triRemap(nt, ~0u);
    sourceVertex.clear();
    SimpleMesh k;

    for (uint32_t t = 0; t < nt; ++t) {
        if (!keep[t]) continue;
        glm::uvec3 tri = source.triangles[t];
        for (int c = 0; c < 3; ++c) {
            uint32_t& r = remap[tri[c]];
            if (r == ~0u) {
                r = uint32_t(sourceVertex.size());
                sourceVertex.push_back(tri[c]);
            }
            tri[c] = r;
        }
        triRemap[t] = uint32_t(k.triangles.size());
        k.triangles.push_back(tri);
    }
    for (uint32_t v : sourceVertex) {
        k.positions.push_back(source.positions[v]);
        if (v < source.colors.size()) k.colors.push_back(source.colors[v]);
        if (v < source.materials.size()) k.materials.push_back(source.materials[v]);
    }
    for (uint32_t t = 0; t < nt; ++t) {
        if (!keep[t]) continue;
        if (t < source.polygroups.size()) k.polygroups.push_back(source.polygroups[t]);
        if (t < source.quadPartners.size()) {
            const uint32_t u = source.quadPartners[t];
            k.quadPartners.push_back(u < nt && triRemap[u] != ~0u ? triRemap[u] : ~0u);
        }
    }
    return k;
}

// Extracts stayed triangles from remeshed mesh
SimpleMesh extractTriangles(const SimpleMesh& mesh, const std::vector<uint8_t>& stay) {
    const size_t nt = mesh.triangleCount();
    std::vector<uint32_t> remap(mesh.vertexCount(), ~0u), triRemap(nt, ~0u);
    SimpleMesh out;
    std::vector<uint32_t> meshVertex;

    for (uint32_t t = 0; t < nt; ++t) {
        if (!stay[t]) continue;
        glm::uvec3 tri = mesh.triangles[t];
        for (int c = 0; c < 3; ++c) {
            uint32_t& r = remap[tri[c]];
            if (r == ~0u) {
                r = uint32_t(meshVertex.size());
                meshVertex.push_back(tri[c]);
            }
            tri[c] = r;
        }
        triRemap[t] = uint32_t(out.triangles.size());
        out.triangles.push_back(tri);
    }
    for (uint32_t v : meshVertex) {
        out.positions.push_back(mesh.positions[v]);
        if (v < mesh.colors.size()) out.colors.push_back(mesh.colors[v]);
        if (v < mesh.materials.size()) out.materials.push_back(mesh.materials[v]);
    }
    for (uint32_t t = 0; t < nt; ++t) {
        if (!stay[t]) continue;
        if (t < mesh.polygroups.size()) out.polygroups.push_back(mesh.polygroups[t]);
        if (t < mesh.quadPartners.size()) {
            const uint32_t u = mesh.quadPartners[t];
            out.quadPartners.push_back(u < nt && triRemap[u] != ~0u ? triRemap[u] : ~0u);
        }
    }
    return out;
}

struct StitchFailure {
    enum Kind { None, KeptPinch, OpenEdge, Cut, Unmatched, Join } kind = None;
    std::vector<uint32_t> pinches;
    std::vector<uint32_t> tooSmall;
};

// Tests whether quadrilateral (p0, p1, p2, p3) in 3D is strictly convex and reasonably planar
bool isConvexPlanarQuad(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3) {
    glm::vec3 n0 = glm::cross(p1 - p0, p3 - p0);
    glm::vec3 n1 = glm::cross(p2 - p1, p0 - p1);
    glm::vec3 n2 = glm::cross(p3 - p2, p1 - p2);
    glm::vec3 n3 = glm::cross(p0 - p3, p2 - p3);

    float len0 = glm::length(n0);
    float len1 = glm::length(n1);
    float len2 = glm::length(n2);
    float len3 = glm::length(n3);
    if (len0 < 1e-6f || len1 < 1e-6f || len2 < 1e-6f || len3 < 1e-6f) return false;

    n0 /= len0; n1 /= len1; n2 /= len2; n3 /= len3;

    // All corner normals must face the same direction (convexity)
    if (glm::dot(n0, n1) <= 0.2f || glm::dot(n0, n2) <= 0.2f || glm::dot(n0, n3) <= 0.2f) return false;

    // Diagonal length check
    float d1 = glm::distance(p0, p2);
    float d2 = glm::distance(p1, p3);
    if (d1 < 1e-5f || d2 < 1e-5f) return false;
    float diagRatio = std::min(d1, d2) / std::max(d1, d2);
    if (diagRatio < 0.25f) return false;

    return true;
}

// Single stitch attempt. Returns true if stitch succeeded and passed all manifold/watertight checks.
bool stitchOnce(const SimpleMesh& source, const std::vector<uint8_t>& keep, const SimpleMesh& remeshed,
                RemeshResult& out, std::string& error, StitchFailure& why, int cutRings) {
    std::vector<uint32_t> keptSource;
    const SimpleMesh kept = keptPiece(source, keep, keptSource);

    // 1. Edge loops of kept piece
    std::vector<std::vector<uint32_t>> keptLoops;
    {
        std::vector<uint32_t> pinches;
        if (!boundaryLoops(kept.triangles, keptLoops, &pinches)) {
            error = "The masked area's edge touches itself; smooth or grow the mask a little";
            why.kind = StitchFailure::KeptPinch;
            for (uint32_t v : pinches) why.pinches.push_back(keptSource[v]);
            return false;
        }
    }

    // Filter out loops on original source open edges
    {
        std::unordered_set<uint64_t> sourceEdges;
        sourceEdges.reserve(source.triangles.size() * 3);
        for (const glm::uvec3& t : source.triangles) {
            for (int e = 0; e < 3; ++e) {
                sourceEdges.insert(directedKey(t[e], t[(e + 1) % 3]));
            }
        }
        std::vector<std::vector<uint32_t>> inner;
        for (auto& loop : keptLoops) {
            size_t open = 0;
            for (size_t i = 0; i < loop.size(); ++i) {
                const uint32_t a = keptSource[loop[i]], b = keptSource[loop[(i + 1) % loop.size()]];
                open += !sourceEdges.count(directedKey(b, a));
            }
            if (open == loop.size()) continue; // Pre-existing boundary loop in source
            if (open > 0) {
                error = "The masked area reaches an open edge of the mesh; unmask the strip along the open edge";
                why.kind = StitchFailure::OpenEdge;
                return false;
            }
            inner.push_back(std::move(loop));
        }
        keptLoops = std::move(inner);
    }

    // 2. Cut remeshed surface away over the masked area and next to it
    std::vector<float> inKept(source.vertexCount(), 0.0f);
    for (uint32_t v : keptSource) inKept[v] = 1.0f;
    const std::vector<float> nearKept = nearestValues(source.positions, inKept, remeshed.positions);
    const size_t rt = remeshed.triangleCount();
    std::vector<uint8_t> stay(rt, 1);
    for (size_t t = 0; t < rt; ++t) {
        const glm::uvec3& tri = remeshed.triangles[t];
        if (nearKept[tri.x] > 0.5f || nearKept[tri.y] > 0.5f || nearKept[tri.z] > 0.5f) stay[t] = 0;
    }

    auto dropHalfQuads = [&] {
        if (remeshed.quadPartners.size() != rt) return;
        for (size_t t = 0; t < rt; ++t) {
            const uint32_t u = remeshed.quadPartners[t];
            if (u < rt && !stay[u]) stay[t] = 0;
        }
    };
    dropHalfQuads();

    for (int ring = 0; ring < cutRings; ++ring) {
        std::vector<uint8_t> cutCorner(remeshed.vertexCount(), 0);
        for (size_t t = 0; t < rt; ++t) {
            if (!stay[t]) {
                for (int c = 0; c < 3; ++c) cutCorner[remeshed.triangles[t][c]] = 1;
            }
        }
        for (size_t t = 0; t < rt; ++t) {
            const glm::uvec3& tri = remeshed.triangles[t];
            if (cutCorner[tri.x] || cutCorner[tri.y] || cutCorner[tri.z]) stay[t] = 0;
        }
        dropHalfQuads();
    }

    // Pinch removal along cut boundary
    for (int pass = 0; pass < 32; ++pass) {
        std::vector<glm::uvec3> stayed;
        for (size_t t = 0; t < rt; ++t) {
            if (stay[t]) stayed.push_back(remeshed.triangles[t]);
        }
        std::vector<std::vector<uint32_t>> loops;
        std::vector<uint32_t> pinches;
        if (boundaryLoops(stayed, loops, &pinches) || pinches.empty()) break;
        const std::unordered_set<uint32_t> pinch(pinches.begin(), pinches.end());
        for (size_t t = 0; t < rt; ++t) {
            const glm::uvec3& tri = remeshed.triangles[t];
            if (pinch.count(tri.x) || pinch.count(tri.y) || pinch.count(tri.z)) stay[t] = 0;
        }
        dropHalfQuads();
    }

    // 3. Match each kept piece loop with corresponding cut loop in remeshed surface
    SimpleMesh rest;
    std::vector<std::vector<uint32_t>> restLoops;
    std::vector<int> match(keptLoops.size(), -1);

    for (int attempt = 0; attempt < 2; ++attempt) {
        rest = extractTriangles(remeshed, stay);
        if (!boundaryLoops(rest.triangles, restLoops)) {
            error = "Couldn't cut the remeshed surface cleanly around the mask";
            why.kind = StitchFailure::Cut;
            return false;
        }
        std::vector<glm::vec3> keptPoints, restPoints;
        std::vector<float> keptLoopId;
        for (size_t l = 0; l < keptLoops.size(); ++l) {
            for (uint32_t v : keptLoops[l]) {
                keptPoints.push_back(kept.positions[v]);
                keptLoopId.push_back(float(l));
            }
        }
        for (const auto& loop : restLoops) {
            for (uint32_t v : loop) restPoints.push_back(rest.positions[v]);
        }
        const std::vector<float> nearestLoop =
            keptPoints.empty() ? std::vector<float>(restPoints.size(), -1.0f) : nearestValues(keptPoints, keptLoopId, restPoints);

        std::vector<int> votedFor(restLoops.size(), -1);
        std::vector<uint8_t> voted(keptLoops.size(), 0);
        size_t at = 0;
        for (size_t r = 0; r < restLoops.size(); ++r) {
            std::map<int, size_t> votes;
            for (size_t i = 0; i < restLoops[r].size(); ++i) {
                const int id = int(nearestLoop[at++]);
                ++votes[id];
                if (id >= 0 && size_t(id) < voted.size()) voted[size_t(id)] = 1;
            }
            size_t best = 0;
            for (const auto& [id, n] : votes) {
                if (id >= 0 && n > best) {
                    best = n;
                    votedFor[r] = id;
                }
            }
        }
        std::fill(match.begin(), match.end(), -1);
        for (size_t r = 0; r < restLoops.size(); ++r) {
            const int k = votedFor[r];
            if (k >= 0 && (match[size_t(k)] < 0 || restLoops[r].size() > restLoops[size_t(match[size_t(k)])].size())) {
                match[size_t(k)] = int(r);
            }
        }
        std::vector<uint8_t> matched(restLoops.size(), 0);
        for (int r : match) {
            if (r >= 0) matched[size_t(r)] = 1;
        }
        if (std::find(match.begin(), match.end(), -1) != match.end()) {
            error = "Part of the mask is too thin for this resolution";
            why.kind = StitchFailure::Unmatched;
            for (size_t l = 0; l < keptLoops.size(); ++l) {
                if (match[l] < 0 && !voted[l]) why.tooSmall.push_back(keptSource[keptLoops[l][0]]);
            }
            return false;
        }
        if (std::find(matched.begin(), matched.end(), 0) == matched.end()) break;
        if (attempt == 1) {
            error = "Couldn't match the remeshed surface to the mask's edge";
            why.kind = StitchFailure::Unmatched;
            return false;
        }

        // Drop stray pieces of remeshed surface inside masked area
        std::vector<uint32_t> parent(rest.vertexCount());
        for (uint32_t v = 0; v < parent.size(); ++v) parent[v] = v;
        auto find = [&](uint32_t v) {
            while (parent[v] != v) v = parent[v] = parent[parent[v]];
            return v;
        };
        for (const glm::uvec3& t : rest.triangles) {
            parent[find(t.y)] = find(t.x);
            parent[find(t.z)] = find(t.x);
        }
        std::unordered_map<uint32_t, size_t> pieceTris;
        for (const glm::uvec3& t : rest.triangles) ++pieceTris[find(t.x)];
        std::unordered_set<uint32_t> neededPieces, strayPieces;
        for (size_t r = 0; r < restLoops.size(); ++r) {
            (matched[r] ? neededPieces : strayPieces).insert(find(restLoops[r][0]));
        }
        for (uint32_t p : strayPieces) {
            if (neededPieces.count(p) || pieceTris[p] * 10 > rest.triangleCount()) {
                error = "Couldn't match the remeshed surface to the mask's edge";
                why.kind = StitchFailure::Unmatched;
                return false;
            }
        }
        size_t rIdx = 0;
        for (size_t t = 0; t < rt; ++t) {
            if (stay[t] && strayPieces.count(find(rest.triangles[rIdx++].x))) stay[t] = 0;
        }
    }

    // 4. Merge kept & rest vertices and construct zipper bridge ribbon
    const uint32_t offset = uint32_t(kept.positions.size());
    std::vector<glm::vec3> mergedPositions;
    std::vector<glm::vec3> mergedColors;
    std::vector<glm::vec3> mergedMaterials;
    mergedPositions.reserve(kept.positions.size() + rest.positions.size());
    mergedColors.reserve(kept.colors.size() + rest.colors.size());
    mergedMaterials.reserve(kept.materials.size() + rest.materials.size());

    mergedPositions.insert(mergedPositions.end(), kept.positions.begin(), kept.positions.end());
    mergedPositions.insert(mergedPositions.end(), rest.positions.begin(), rest.positions.end());

    mergedColors.insert(mergedColors.end(), kept.colors.begin(), kept.colors.end());
    mergedColors.insert(mergedColors.end(), rest.colors.begin(), rest.colors.end());

    mergedMaterials.insert(mergedMaterials.end(), kept.materials.begin(), kept.materials.end());
    mergedMaterials.insert(mergedMaterials.end(), rest.materials.begin(), rest.materials.end());

    uint32_t bridgeGroup = !rest.polygroups.empty() ? rest.polygroups[0] : (!kept.polygroups.empty() ? kept.polygroups[0] : 0u);

    std::vector<glm::uvec3> bridge;
    for (size_t l = 0; l < keptLoops.size(); ++l) {
        const auto& P = keptLoops[l];
        const auto& Q = restLoops[size_t(match[l])];
        size_t j0 = 0;
        float bestD = std::numeric_limits<float>::max();
        for (size_t j = 0; j < Q.size(); ++j) {
            const float d = glm::distance(kept.positions[P[0]], rest.positions[Q[j]]);
            if (d < bestD) {
                bestD = d;
                j0 = j;
            }
        }
        const size_t n = P.size(), m = Q.size();
        auto p = [&](size_t i) { return P[i % n]; };
        auto s = [&](size_t k) { return Q[(j0 + m - (k % m)) % m] + offset; };
        auto pos = [&](uint32_t v) { return mergedPositions[v]; };

        size_t i = 0, k = 0;
        while (i < n || k < m) {
            const bool advanceP =
                k >= m || (i < n && glm::distance(pos(p(i + 1)), pos(s(k))) <= glm::distance(pos(p(i)), pos(s(k + 1))));
            if (advanceP) {
                bridge.emplace_back(p(i + 1), p(i), s(k));
                ++i;
            } else {
                bridge.emplace_back(s(k), s(k + 1), p(i));
                ++k;
            }
        }
    }

    // 5. Build RemeshResult with quads preservation and quadification of bridge
    RemeshResult res;
    res.vertices.reserve(mergedPositions.size() * 3);
    for (const auto& p : mergedPositions) {
        res.vertices.push_back(p.x);
        res.vertices.push_back(p.y);
        res.vertices.push_back(p.z);
    }
    res.colors.reserve(mergedColors.size() * 3);
    for (const auto& c : mergedColors) {
        res.colors.push_back(c.x);
        res.colors.push_back(c.y);
        res.colors.push_back(c.z);
    }
    res.materials.reserve(mergedMaterials.size() * 3);
    for (const auto& m : mergedMaterials) {
        res.materials.push_back(m.x);
        res.materials.push_back(m.y);
        res.materials.push_back(m.z);
    }

    auto emitQuad = [&](uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3, uint32_t fg) {
        res.faces.push_back(v0);
        res.faces.push_back(v1);
        res.faces.push_back(v2);
        res.faces.push_back(v3);
        res.faceGroups.push_back(fg);
    };

    auto emitTri = [&](uint32_t v0, uint32_t v1, uint32_t v2, uint32_t fg) {
        res.faces.push_back(v0);
        res.faces.push_back(v1);
        res.faces.push_back(v2);
        res.faces.push_back(TRI_INDEX);
        res.faceGroups.push_back(fg);
    };

    // A. Kept faces
    std::vector<uint8_t> keptFaceEmitted(kept.triangles.size(), 0);
    for (size_t t = 0; t < kept.triangles.size(); ++t) {
        if (keptFaceEmitted[t]) continue;
        uint32_t u = (t < kept.quadPartners.size()) ? kept.quadPartners[t] : ~0u;
        uint32_t fg = (t < kept.polygroups.size()) ? kept.polygroups[t] : 0u;
        if (u < kept.triangles.size() && !keptFaceEmitted[u]) {
            // Reconstruct quad from triangle pair: t = (v0, v1, v2), u = (v0, v2, v3)
            const auto& tA = kept.triangles[t];
            const auto& tB = kept.triangles[u];
            emitQuad(tA.x, tA.y, tA.z, tB.z, fg);
            keptFaceEmitted[t] = 1;
            keptFaceEmitted[u] = 1;
        } else {
            const auto& tA = kept.triangles[t];
            emitTri(tA.x, tA.y, tA.z, fg);
            keptFaceEmitted[t] = 1;
        }
    }

    // B. Rest faces (vertices shifted by offset)
    std::vector<uint8_t> restFaceEmitted(rest.triangles.size(), 0);
    for (size_t t = 0; t < rest.triangles.size(); ++t) {
        if (restFaceEmitted[t]) continue;
        uint32_t u = (t < rest.quadPartners.size()) ? rest.quadPartners[t] : ~0u;
        uint32_t fg = (t < rest.polygroups.size()) ? rest.polygroups[t] : 0u;
        if (u < rest.triangles.size() && !restFaceEmitted[u]) {
            const auto& tA = rest.triangles[t];
            const auto& tB = rest.triangles[u];
            emitQuad(tA.x + offset, tA.y + offset, tA.z + offset, tB.z + offset, fg);
            restFaceEmitted[t] = 1;
            restFaceEmitted[u] = 1;
        } else {
            const auto& tA = rest.triangles[t];
            emitTri(tA.x + offset, tA.y + offset, tA.z + offset, fg);
            restFaceEmitted[t] = 1;
        }
    }

    // C. Bridge faces with adjacent triangle quad pairing
    size_t b = 0;
    while (b < bridge.size()) {
        bool paired = false;
        if (b + 1 < bridge.size()) {
            const auto& T1 = bridge[b];
            const auto& T2 = bridge[b + 1];
            // Check if T1 and T2 share an edge in opposite directions:
            // T1 = (p(i+1), p(i), s(k)), T2 = (s(k), s(k+1), p(i+1)) -> shared edge (s(k), p(i+1))
            if (T1.x == T2.z && T1.z == T2.x) {
                uint32_t q0 = T1.y;
                uint32_t q1 = T1.z;
                uint32_t q2 = T2.y;
                uint32_t q3 = T1.x;
                if (isConvexPlanarQuad(mergedPositions[q0], mergedPositions[q1], mergedPositions[q2], mergedPositions[q3])) {
                    emitQuad(q0, q1, q2, q3, bridgeGroup);
                    paired = true;
                    b += 2;
                }
            } else if (T1.y == T2.x && T1.x == T2.z) {
                uint32_t q0 = T1.z;
                uint32_t q1 = T1.x;
                uint32_t q2 = T2.y;
                uint32_t q3 = T1.y;
                if (isConvexPlanarQuad(mergedPositions[q0], mergedPositions[q1], mergedPositions[q2], mergedPositions[q3])) {
                    emitQuad(q0, q1, q2, q3, bridgeGroup);
                    paired = true;
                    b += 2;
                }
            }
        }
        if (!paired) {
            emitTri(bridge[b].x, bridge[b].y, bridge[b].z, bridgeGroup);
            b += 1;
        }
    }

    // 6. Verify topological health (watertight & 2-manifold)
    std::unordered_map<uint64_t, int> edgeMap;
    edgeMap.reserve(res.faces.size());
    const size_t nbF = res.faces.size() / 4;
    for (size_t f = 0; f < nbF; ++f) {
        uint32_t v0 = res.faces[f * 4 + 0];
        uint32_t v1 = res.faces[f * 4 + 1];
        uint32_t v2 = res.faces[f * 4 + 2];
        uint32_t v3 = res.faces[f * 4 + 3];
        ++edgeMap[undirectedKey(v0, v1)];
        ++edgeMap[undirectedKey(v1, v2)];
        if (v3 != TRI_INDEX) {
            ++edgeMap[undirectedKey(v2, v3)];
            ++edgeMap[undirectedKey(v3, v0)];
        } else {
            ++edgeMap[undirectedKey(v2, v0)];
        }
    }

    int openBoundaryEdges = 0;
    int nonManifoldEdges = 0;
    for (const auto& [k, cnt] : edgeMap) {
        if (cnt == 1) ++openBoundaryEdges;
        else if (cnt > 2) ++nonManifoldEdges;
    }

    // Check if source mesh had open boundaries
    std::unordered_map<uint64_t, int> srcEdgeMap;
    for (const auto& t : source.triangles) {
        for (int e = 0; e < 3; ++e) {
            ++srcEdgeMap[undirectedKey(t[e], t[(e + 1) % 3])];
        }
    }
    int srcBoundary = 0;
    for (const auto& [k, cnt] : srcEdgeMap) {
        if (cnt == 1) ++srcBoundary;
    }

    if (nonManifoldEdges > 0 || (srcBoundary == 0 && openBoundaryEdges != 0)) {
        error = "Couldn't join the masked area to the remeshed surface cleanly";
        why.kind = StitchFailure::Join;
        return false;
    }

    out = std::move(res);
    return true;
}

// Drops disconnected pieces of keep containing seeds
void dropPieces(const SimpleMesh& source, std::vector<uint8_t>& keep, const std::vector<uint32_t>& seeds) {
    std::vector<uint32_t> parent(source.vertexCount());
    for (uint32_t v = 0; v < parent.size(); ++v) parent[v] = v;
    auto find = [&](uint32_t v) {
        while (parent[v] != v) v = parent[v] = parent[parent[v]];
        return v;
    };
    for (size_t t = 0; t < keep.size(); ++t) {
        if (keep[t]) {
            const glm::uvec3& tri = source.triangles[t];
            parent[find(tri.y)] = find(tri.x);
            parent[find(tri.z)] = find(tri.x);
        }
    }
    std::unordered_set<uint32_t> drop;
    for (uint32_t v : seeds) drop.insert(find(v));
    for (size_t t = 0; t < keep.size(); ++t) {
        if (keep[t] && drop.count(find(source.triangles[t].x))) keep[t] = 0;
    }
}

// Morphological closing of the kept area by `rings` rings of vertices
void closeKept(const SimpleMesh& source, std::vector<uint8_t>& keep, int rings) {
    const size_t nv = source.vertexCount();
    std::vector<uint8_t> in(nv, 0), next(nv, 0);
    for (size_t t = 0; t < keep.size(); ++t) {
        if (keep[t]) {
            for (int c = 0; c < 3; ++c) in[source.triangles[t][c]] = 1;
        }
    }

    std::vector<std::vector<uint32_t>> adj(nv);
    for (const auto& t : source.triangles) {
        adj[t.x].push_back(t.y); adj[t.x].push_back(t.z);
        adj[t.y].push_back(t.x); adj[t.y].push_back(t.z);
        adj[t.z].push_back(t.x); adj[t.z].push_back(t.y);
    }

    for (int pass = 0; pass < 2 * rings; ++pass) {
        const bool grow = pass < rings;
        for (uint32_t v = 0; v < nv; ++v) {
            uint8_t x = in[v];
            for (uint32_t n : adj[v]) x = grow ? uint8_t(x | in[n]) : uint8_t(x & in[n]);
            next[v] = x;
        }
        in.swap(next);
    }
    for (size_t t = 0; t < keep.size(); ++t) {
        const glm::uvec3& tri = source.triangles[t];
        if (in[tri.x] && in[tri.y] && in[tri.z]) keep[t] = 1;
    }
}

} // namespace

bool stitchKeptTriangles(
    const float* srcVerts, int srcNbVerts,
    const uint32_t* srcTris, int srcNbTris,
    const float* srcColors,
    const float* srcMaterials,
    const uint32_t* srcFaceGroups,
    const std::vector<uint8_t>& keepIn,
    const RemeshResult& remeshed,
    RemeshResult& out,
    std::string& error,
    std::string* note
) {
    if (!srcVerts || srcNbVerts <= 0 || !srcTris || srcNbTris <= 0) {
        error = "Invalid source mesh";
        return false;
    }
    if (keepIn.size() != (size_t)srcNbTris || std::find(keepIn.begin(), keepIn.end(), 1) == keepIn.end()) {
        error = "Nothing is masked";
        return false;
    }

    // Convert source mesh to SimpleMesh
    SimpleMesh source;
    source.positions.resize(srcNbVerts);
    for (int i = 0; i < srcNbVerts; ++i) {
        source.positions[i] = glm::vec3(srcVerts[i * 3], srcVerts[i * 3 + 1], srcVerts[i * 3 + 2]);
    }
    source.triangles.resize(srcNbTris);
    for (int i = 0; i < srcNbTris; ++i) {
        source.triangles[i] = glm::uvec3(srcTris[i * 3], srcTris[i * 3 + 1], srcTris[i * 3 + 2]);
    }
    if (srcColors) {
        source.colors.resize(srcNbVerts);
        for (int i = 0; i < srcNbVerts; ++i) {
            source.colors[i] = glm::vec3(srcColors[i * 3], srcColors[i * 3 + 1], srcColors[i * 3 + 2]);
        }
    } else {
        source.colors.assign(srcNbVerts, glm::vec3(0.72f, 0.52f, 0.45f));
    }
    if (srcMaterials) {
        source.materials.resize(srcNbVerts);
        for (int i = 0; i < srcNbVerts; ++i) {
            source.materials[i] = glm::vec3(srcMaterials[i * 3], srcMaterials[i * 3 + 1], srcMaterials[i * 3 + 2]);
        }
    } else {
        source.materials.assign(srcNbVerts, glm::vec3(0.5f, 0.0f, 1.0f));
    }
    if (srcFaceGroups) {
        source.polygroups.assign(srcFaceGroups, srcFaceGroups + srcNbTris);
    } else {
        source.polygroups.assign(srcNbTris, 0u);
    }
    // Quad partner detection in source: adjacent triangle pairs sharing (v0, v2)
    source.quadPartners.assign(srcNbTris, ~0u);
    for (int t = 0; t + 1 < srcNbTris; ++t) {
        if (srcTris[t * 3 + 0] == srcTris[(t + 1) * 3 + 0] &&
            srcTris[t * 3 + 2] == srcTris[(t + 1) * 3 + 1]) {
            source.quadPartners[t] = t + 1;
            source.quadPartners[t + 1] = t;
            ++t; // Advance past pair
        }
    }

    // Convert remeshed RemeshResult to SimpleMesh
    SimpleMesh remMesh;
    const size_t remNbV = remeshed.vertices.size() / 3;
    remMesh.positions.resize(remNbV);
    for (size_t i = 0; i < remNbV; ++i) {
        remMesh.positions[i] = glm::vec3(remeshed.vertices[i * 3], remeshed.vertices[i * 3 + 1], remeshed.vertices[i * 3 + 2]);
    }
    if (!remeshed.colors.empty()) {
        remMesh.colors.resize(remNbV);
        for (size_t i = 0; i < remNbV; ++i) {
            remMesh.colors[i] = glm::vec3(remeshed.colors[i * 3], remeshed.colors[i * 3 + 1], remeshed.colors[i * 3 + 2]);
        }
    } else {
        remMesh.colors.assign(remNbV, glm::vec3(0.72f, 0.52f, 0.45f));
    }
    if (!remeshed.materials.empty()) {
        remMesh.materials.resize(remNbV);
        for (size_t i = 0; i < remNbV; ++i) {
            remMesh.materials[i] = glm::vec3(remeshed.materials[i * 3], remeshed.materials[i * 3 + 1], remeshed.materials[i * 3 + 2]);
        }
    } else {
        remMesh.materials.assign(remNbV, glm::vec3(0.5f, 0.0f, 1.0f));
    }

    const size_t remNbF = remeshed.faces.size() / 4;
    for (size_t f = 0; f < remNbF; ++f) {
        uint32_t v0 = remeshed.faces[f * 4 + 0];
        uint32_t v1 = remeshed.faces[f * 4 + 1];
        uint32_t v2 = remeshed.faces[f * 4 + 2];
        uint32_t v3 = remeshed.faces[f * 4 + 3];
        uint32_t fg = f < remeshed.faceGroups.size() ? remeshed.faceGroups[f] : 0u;

        if (v3 == TRI_INDEX) {
            remMesh.triangles.emplace_back(v0, v1, v2);
            remMesh.polygroups.push_back(fg);
            remMesh.quadPartners.push_back(~0u);
        } else {
            uint32_t idxA = uint32_t(remMesh.triangles.size());
            uint32_t idxB = idxA + 1;
            remMesh.triangles.emplace_back(v0, v1, v2);
            remMesh.triangles.emplace_back(v0, v2, v3);
            remMesh.polygroups.push_back(fg);
            remMesh.polygroups.push_back(fg);
            remMesh.quadPartners.push_back(idxB);
            remMesh.quadPartners.push_back(idxA);
        }
    }

    // Self-healing attempt loop (up to 24 iterations, matching SculptForge)
    std::vector<uint8_t> keep = keepIn;
    StitchFailure first;
    size_t pinchFixes = 0;
    int rings = 0, cutRings = 0;

    for (int attempt = 0; attempt < 24; ++attempt) {
        StitchFailure why;
        std::string err;
        if (stitchOnce(source, keep, remMesh, out, err, why, cutRings)) {
            if (note) {
                size_t added = 0, removed = 0;
                for (size_t t = 0; t < (size_t)srcNbTris; ++t) {
                    added += keep[t] && !keepIn[t];
                    removed += !keep[t] && keepIn[t];
                }
                note->clear();
                if (added || removed) {
                    *note = "the mask's edge was tidied to fit the new surface (";
                    if (added) *note += std::to_string(added) + " triangles in narrow gaps kept";
                    if (added && removed) *note += ", ";
                    if (removed) *note += std::to_string(removed) + " triangles at tight corners or small spots remeshed";
                    *note += ")";
                }
            }
            return true;
        }
        if (first.kind == StitchFailure::None) {
            first = why;
            error = err;
        }
        bool changed = false;
        if (why.kind == StitchFailure::KeptPinch && !why.pinches.empty() && pinchFixes < 8) {
            const std::unordered_set<uint32_t> pinch(why.pinches.begin(), why.pinches.end());
            for (size_t t = 0; t < (size_t)srcNbTris; ++t) {
                const glm::uvec3& tri = source.triangles[t];
                if (keep[t] && (pinch.count(tri.x) || pinch.count(tri.y) || pinch.count(tri.z))) {
                    keep[t] = 0;
                    changed = true;
                }
            }
            ++pinchFixes;
        } else if (why.kind == StitchFailure::Unmatched && !why.tooSmall.empty()) {
            dropPieces(source, keep, why.tooSmall);
            changed = true;
        } else if ((why.kind == StitchFailure::Join || why.kind == StitchFailure::Cut) && cutRings < 2) {
            ++cutRings;
            changed = true;
        } else if (why.kind != StitchFailure::OpenEdge && rings < 4) {
            rings = rings ? rings * 2 : 1;
            const std::vector<uint8_t> before = keep;
            closeKept(source, keep, rings);
            changed = keep != before;
        }
        if (!changed) break;
        if (std::find(keep.begin(), keep.end(), 1) == keep.end()) break;
        if (std::find(keep.begin(), keep.end(), 0) == keep.end()) break;
    }

    switch (first.kind) {
    case StitchFailure::KeptPinch:
        error = "The masked area's edge touches itself (" + std::to_string(first.pinches.size()) +
                (first.pinches.size() == 1 ? " place" : " places") + "); blur or grow the mask, then try again";
        break;
    case StitchFailure::OpenEdge:
        break;
    case StitchFailure::Unmatched:
        error = "Two masked areas are too close together or one is too small for this resolution; grow the mask or increase remesh resolution";
        break;
    default:
        error = "Couldn't join the masked area to the remeshed surface cleanly; blur or grow the mask, then try again";
        break;
    }
    return false;
}

} // namespace sculpt
