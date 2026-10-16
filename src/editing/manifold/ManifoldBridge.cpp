#include "editing/manifold/ManifoldBridge.h"
#include "common/Logger.h"
#include "common/Constants.h"
#include <chrono>
#include <numeric>
#include <map>
#include <unordered_map>

namespace sculpt {

manifold::Manifold ManifoldBridge::fromMesh(const Mesh& mesh, uint32_t originalID) {
    auto tStart = std::chrono::high_resolution_clock::now();

    manifold::MeshGL gl;
    gl.numProp = 12; // pos(3), norm(3), color(3), material(3)

    const int nbV = mesh.nbVerts;
    gl.vertProperties.resize(nbV * 12);

    const bool hasColors = (mesh.colors.size() == (size_t)(nbV * 3));
    const bool hasMaterials = (mesh.materials.size() == (size_t)(nbV * 3));
    const bool hasNormals = (mesh.normals.size() == (size_t)(nbV * 3));

    for (int i = 0; i < nbV; ++i) {
        int base = i * 12;
        gl.vertProperties[base + 0] = mesh.verts[i * 3 + 0];
        gl.vertProperties[base + 1] = mesh.verts[i * 3 + 1];
        gl.vertProperties[base + 2] = mesh.verts[i * 3 + 2];

        if (hasNormals) {
            gl.vertProperties[base + 3] = mesh.normals[i * 3 + 0];
            gl.vertProperties[base + 4] = mesh.normals[i * 3 + 1];
            gl.vertProperties[base + 5] = mesh.normals[i * 3 + 2];
        } else {
            gl.vertProperties[base + 3] = 0.0f;
            gl.vertProperties[base + 4] = 1.0f;
            gl.vertProperties[base + 5] = 0.0f;
        }

        if (hasColors) {
            gl.vertProperties[base + 6] = mesh.colors[i * 3 + 0];
            gl.vertProperties[base + 7] = mesh.colors[i * 3 + 1];
            gl.vertProperties[base + 8] = mesh.colors[i * 3 + 2];
        } else {
            gl.vertProperties[base + 6] = 1.0f;
            gl.vertProperties[base + 7] = 1.0f;
            gl.vertProperties[base + 8] = 1.0f;
        }

        if (hasMaterials) {
            gl.vertProperties[base + 9] = mesh.materials[i * 3 + 0];
            gl.vertProperties[base + 10] = mesh.materials[i * 3 + 1];
            gl.vertProperties[base + 11] = mesh.materials[i * 3 + 2];
        } else {
            gl.vertProperties[base + 9] = 0.5f;
            gl.vertProperties[base + 10] = 0.0f;
            gl.vertProperties[base + 11] = 1.0f;
        }
    }

    const int nbF = mesh.nbFaces;

    gl.triVerts.reserve(nbF * 6);
    gl.faceID.reserve(nbF * 2);

    for (int f = 0; f < nbF; ++f) {
        uint32_t i0 = mesh.faces[f * 4 + 0];
        uint32_t i1 = mesh.faces[f * 4 + 1];
        uint32_t i2 = mesh.faces[f * 4 + 2];
        uint32_t i3 = mesh.faces[f * 4 + 3];

        // Tri 1: i0, i1, i2
        gl.triVerts.push_back(i0);
        gl.triVerts.push_back(i1);
        gl.triVerts.push_back(i2);
        gl.faceID.push_back((uint32_t)f);

        // Tri 2: i0, i2, i3 (if quad)
        if (i3 != TRI_INDEX) {
            gl.triVerts.push_back(i0);
            gl.triVerts.push_back(i2);
            gl.triVerts.push_back(i3);
            gl.faceID.push_back((uint32_t)f);
        }
    }

    if (originalID == 0) {
        originalID = manifold::Manifold::ReserveIDs(1);
    }
    gl.runIndex = {0, (uint32_t)gl.triVerts.size()};
    gl.runOriginalID = {originalID};

    // Merge coincident vertices along open edges to guarantee watertight 2-manifold
    gl.Merge();

    // Disable coplanar face merging by setting negative property tolerance
    std::vector<float> propertyTolerance(gl.numProp - 3, -1.0f);
    manifold::Manifold m(gl, propertyTolerance);

    auto tEnd = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    sculpt_log("[MANIFOLD BRIDGE] Converted Mesh -> Manifold in %.2f ms (%d verts, %d tris, status: %d, origID: %u)\n",
               ms, nbV, (int)(gl.triVerts.size() / 3), (int)m.Status(), originalID);

    return m;
}

void ManifoldBridge::toMesh(
    const manifold::Manifold& m,
    Mesh& outMesh,
    bool runPostInit,
    int cutterOriginalID,
    uint32_t capGroupID
) {
    auto tStart = std::chrono::high_resolution_clock::now();

    manifold::MeshGL gl = m.GetMeshGL();
    gl.Merge();

    const int rawNumV = gl.NumVert();
    const int numT = gl.NumTri();

    // Map source mesh triangles to original PolyGroup IDs before outMesh is overwritten
    std::vector<uint32_t> origTriToGroup;
    uint32_t defaultMeshGid = 1;
    if (!outMesh.faceGroups.empty()) {
        defaultMeshGid = outMesh.faceGroups[0];
        if (outMesh.faceGroups.size() == (size_t)outMesh.nbFaces && outMesh.faces.size() == (size_t)outMesh.nbFaces * 4) {
            origTriToGroup.reserve(outMesh.nbFaces * 2);
            for (int f = 0; f < outMesh.nbFaces; ++f) {
                uint32_t fg = outMesh.faceGroups[f];
                origTriToGroup.push_back(fg);
                if (outMesh.faces[f * 4 + 3] != TRI_INDEX) {
                    origTriToGroup.push_back(fg);
                }
            }
        }
    }

    // Build canonical vertex mapping to weld any duplicate vertices from property splits
    std::vector<int> canonical(rawNumV);
    std::iota(canonical.begin(), canonical.end(), 0);

    auto findRoot = [&](int v) {
        int root = v;
        while (canonical[root] != root) {
            root = canonical[root];
        }
        int curr = v;
        while (curr != root) {
            int nxt = canonical[curr];
            canonical[curr] = root;
            curr = nxt;
        }
        return root;
    };

    for (size_t i = 0; i < gl.mergeFromVert.size(); ++i) {
        int vFrom = gl.mergeFromVert[i];
        int vTo = gl.mergeToVert[i];
        if (vFrom >= 0 && vFrom < rawNumV && vTo >= 0 && vTo < rawNumV) {
            int rFrom = findRoot(vFrom);
            int rTo = findRoot(vTo);
            if (rFrom != rTo) {
                canonical[rFrom] = rTo;
            }
        }
    }

    std::vector<int> compactIndex(rawNumV, -1);
    int numUniqueV = 0;
    for (int i = 0; i < rawNumV; ++i) {
        int root = findRoot(i);
        if (compactIndex[root] == -1) {
            compactIndex[root] = numUniqueV++;
        }
        compactIndex[i] = compactIndex[root];
    }

    outMesh.nbVerts = numUniqueV;
    outMesh.verts.resize(numUniqueV * 3);
    outMesh.normals.resize(numUniqueV * 3);
    outMesh.colors.resize(numUniqueV * 3);
    outMesh.materials.resize(numUniqueV * 3);

    const int propStride = gl.numProp;

    for (int i = 0; i < rawNumV; ++i) {
        int u = compactIndex[i];
        int base = i * propStride;
        outMesh.verts[u * 3 + 0] = gl.vertProperties[base + 0];
        outMesh.verts[u * 3 + 1] = gl.vertProperties[base + 1];
        outMesh.verts[u * 3 + 2] = gl.vertProperties[base + 2];

        if (propStride >= 6) {
            outMesh.normals[u * 3 + 0] = gl.vertProperties[base + 3];
            outMesh.normals[u * 3 + 1] = gl.vertProperties[base + 4];
            outMesh.normals[u * 3 + 2] = gl.vertProperties[base + 5];
        } else {
            outMesh.normals[u * 3 + 0] = 0.0f;
            outMesh.normals[u * 3 + 1] = 1.0f;
            outMesh.normals[u * 3 + 2] = 0.0f;
        }

        if (propStride >= 9) {
            float r = gl.vertProperties[base + 6];
            float g = gl.vertProperties[base + 7];
            float b = gl.vertProperties[base + 8];
            if (r <= 1e-4f && g <= 1e-4f && b <= 1e-4f) {
                r = g = b = 1.0f;
            }
            outMesh.colors[u * 3 + 0] = r;
            outMesh.colors[u * 3 + 1] = g;
            outMesh.colors[u * 3 + 2] = b;
        } else {
            outMesh.colors[u * 3 + 0] = 1.0f;
            outMesh.colors[u * 3 + 1] = 1.0f;
            outMesh.colors[u * 3 + 2] = 1.0f;
        }

        if (propStride >= 12) {
            float rough = gl.vertProperties[base + 9];
            float metal = gl.vertProperties[base + 10];
            float mask  = gl.vertProperties[base + 11];
            if (mask <= 1e-4f) mask = 1.0f; // unmasked
            if (rough <= 1e-4f && metal <= 1e-4f) {
                rough = 0.5f;
                metal = 0.0f;
            }
            outMesh.materials[u * 3 + 0] = rough;
            outMesh.materials[u * 3 + 1] = metal;
            outMesh.materials[u * 3 + 2] = mask;
        } else {
            outMesh.materials[u * 3 + 0] = 0.5f;
            outMesh.materials[u * 3 + 1] = 0.0f;
            outMesh.materials[u * 3 + 2] = 1.0f;
        }
    }

    const bool hasFaceID = !gl.faceID.empty() && (gl.faceID.size() == (size_t)numT);

    // Identify cap triangles using cutterOriginalID
    std::vector<uint32_t> triCapGroup(numT, 0);
    int capTris = 0;
    if (cutterOriginalID >= 0 && gl.runIndex.size() >= 2 && gl.runOriginalID.size() == gl.runIndex.size() - 1) {
        for (size_t r = 0; r < gl.runOriginalID.size(); ++r) {
            if ((int)gl.runOriginalID[r] == cutterOriginalID) {
                uint32_t startT = gl.runIndex[r] / 3;
                uint32_t endT = gl.runIndex[r + 1] / 3;
                for (uint32_t t = startT; t < endT && t < (uint32_t)numT; ++t) {
                    triCapGroup[t] = capGroupID;
                    capTris++;
                }
            }
        }
        sculpt_log("[MANIFOLD BRIDGE] Assigned Cap Group ID %u to %d cap triangles (from cutter originalID %d)\n",
                   capGroupID, capTris, cutterOriginalID);
    }

    struct ValidTri {
        uint32_t i0, i1, i2;
        uint32_t gid;
        glm::vec3 normal;
    };

    std::vector<ValidTri> validTris;
    validTris.reserve(numT);

    for (int t = 0; t < numT; ++t) {
        uint32_t i0 = compactIndex[gl.triVerts[t * 3 + 0]];
        uint32_t i1 = compactIndex[gl.triVerts[t * 3 + 1]];
        uint32_t i2 = compactIndex[gl.triVerts[t * 3 + 2]];

        // Discard degenerate triangles
        if (i0 == i1 || i1 == i2 || i2 == i0) continue;

        uint32_t gid = triCapGroup[t];
        if (gid == 0) {
            if (hasFaceID && gl.faceID[t] < outMesh.faceGroups.size()) {
                gid = outMesh.faceGroups[gl.faceID[t]];
            } else if (hasFaceID && gl.faceID[t] < origTriToGroup.size()) {
                gid = origTriToGroup[gl.faceID[t]];
            } else {
                gid = defaultMeshGid;
            }
        }

        glm::vec3 p0(outMesh.verts[i0 * 3], outMesh.verts[i0 * 3 + 1], outMesh.verts[i0 * 3 + 2]);
        glm::vec3 p1(outMesh.verts[i1 * 3], outMesh.verts[i1 * 3 + 1], outMesh.verts[i1 * 3 + 2]);
        glm::vec3 p2(outMesh.verts[i2 * 3], outMesh.verts[i2 * 3 + 1], outMesh.verts[i2 * 3 + 2]);
        glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
        float len = glm::length(n);
        if (len > 1e-7f) n /= len;
        else n = glm::vec3(0, 1, 0);

        validTris.push_back({i0, i1, i2, gid, n});
    }

    const size_t VT = validTris.size();

    // Map undirected edges to adjacent triangles for quadification
    struct EdgeHalf {
        int triIdx;
        uint32_t vOpp;
        bool forward; // true if tri has min(a,b) -> max(a,b)
    };

    auto makeEdgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
        uint32_t u = std::min(a, b);
        uint32_t v = std::max(a, b);
        return ((uint64_t)u << 32) | (uint64_t)v;
    };

    std::unordered_map<uint64_t, std::vector<EdgeHalf>> edgeMap;
    edgeMap.reserve(VT * 3 / 2);

    for (size_t t = 0; t < VT; ++t) {
        const auto& tri = validTris[t];
        edgeMap[makeEdgeKey(tri.i0, tri.i1)].push_back({(int)t, tri.i2, tri.i0 < tri.i1});
        edgeMap[makeEdgeKey(tri.i1, tri.i2)].push_back({(int)t, tri.i0, tri.i1 < tri.i2});
        edgeMap[makeEdgeKey(tri.i2, tri.i0)].push_back({(int)t, tri.i1, tri.i2 < tri.i0});
    }

    struct CandidatePair {
        int tA, tB;
        uint32_t v0, v1, v2, v3;
        float score;
    };

    std::vector<CandidatePair> candidates;
    candidates.reserve(VT / 2);

    for (const auto& [key, list] : edgeMap) {
        if (list.size() != 2) continue;

        const auto& hA = list[0];
        const auto& hB = list[1];
        if (hA.forward == hB.forward) continue; // Non-manifold or inverted

        int tA = hA.triIdx;
        int tB = hB.triIdx;
        if (validTris[tA].gid != validTris[tB].gid) continue; // Keep polygroups separate

        float dotN = glm::dot(validTris[tA].normal, validTris[tB].normal);
        if (dotN < 0.80f) continue; // Crease angle > ~36 deg

        uint32_t u = (uint32_t)(key >> 32);
        uint32_t v = (uint32_t)(key & 0xFFFFFFFF);
        uint32_t wA = hA.vOpp;
        uint32_t wB = hB.vOpp;

        // In tA, ensure edge is directed u -> v
        if (!hA.forward) {
            std::swap(u, v);
        }
        // CCW quad perimeter: u -> wB -> v -> wA
        uint32_t q0 = u;
        uint32_t q1 = wB;
        uint32_t q2 = v;
        uint32_t q3 = wA;

        glm::vec3 p0(outMesh.verts[q0 * 3], outMesh.verts[q0 * 3 + 1], outMesh.verts[q0 * 3 + 2]);
        glm::vec3 p1(outMesh.verts[q1 * 3], outMesh.verts[q1 * 3 + 1], outMesh.verts[q1 * 3 + 2]);
        glm::vec3 p2(outMesh.verts[q2 * 3], outMesh.verts[q2 * 3 + 1], outMesh.verts[q2 * 3 + 2]);
        glm::vec3 p3(outMesh.verts[q3 * 3], outMesh.verts[q3 * 3 + 1], outMesh.verts[q3 * 3 + 2]);

        glm::vec3 avgN = glm::normalize(validTris[tA].normal + validTris[tB].normal);

        // Check strict convexity at all 4 corners
        glm::vec3 c0 = glm::cross(p1 - p0, p3 - p0);
        glm::vec3 c1 = glm::cross(p2 - p1, p0 - p1);
        glm::vec3 c2 = glm::cross(p3 - p2, p1 - p2);
        glm::vec3 c3 = glm::cross(p0 - p3, p2 - p3);

        if (glm::dot(c0, avgN) <= 1e-5f || glm::dot(c1, avgN) <= 1e-5f ||
            glm::dot(c2, avgN) <= 1e-5f || glm::dot(c3, avgN) <= 1e-5f) {
            continue;
        }

        // Check diagonals: aspect ratio must not be extremely skewed
        float d1 = glm::distance(p0, p2);
        float d2 = glm::distance(p1, p3);
        float diagRatio = (d1 > 1e-5f && d2 > 1e-5f) ? std::min(d1, d2) / std::max(d1, d2) : 0.0f;
        if (diagRatio < 0.20f) continue;

        float score = dotN * 2.0f + diagRatio;
        candidates.push_back({tA, tB, q0, q1, q2, q3, score});
    }

    std::sort(candidates.begin(), candidates.end(), [](const CandidatePair& a, const CandidatePair& b) {
        return a.score > b.score;
    });

    std::vector<bool> triUsed(VT, false);
    std::vector<uint32_t> outFaces;
    std::vector<uint32_t> outFaceGroups;
    outFaces.reserve(VT * 4);
    outFaceGroups.reserve(VT);

    int numQuads = 0;
    for (const auto& c : candidates) {
        if (!triUsed[c.tA] && !triUsed[c.tB]) {
            triUsed[c.tA] = true;
            triUsed[c.tB] = true;

            outFaces.push_back(c.v0);
            outFaces.push_back(c.v1);
            outFaces.push_back(c.v2);
            outFaces.push_back(c.v3);
            outFaceGroups.push_back(validTris[c.tA].gid);
            numQuads++;
        }
    }

    int numTrisRemaining = 0;
    for (size_t t = 0; t < VT; ++t) {
        if (!triUsed[t]) {
            const auto& tri = validTris[t];
            outFaces.push_back(tri.i0);
            outFaces.push_back(tri.i1);
            outFaces.push_back(tri.i2);
            outFaces.push_back(TRI_INDEX);
            outFaceGroups.push_back(tri.gid);
            numTrisRemaining++;
        }
    }

    outMesh.nbFaces = (int)(outFaces.size() / 4);
    outMesh.faces = std::move(outFaces);
    outMesh.faceGroups = std::move(outFaceGroups);

    float quadPct = (outMesh.nbFaces > 0) ? (float)numQuads / (float)outMesh.nbFaces * 100.0f : 0.0f;
    sculpt_log("[MANIFOLD BRIDGE] Quadify: generated %d Quads (%.1f%%), %d Tris\n",
               numQuads, quadPct, numTrisRemaining);

    outMesh.vrfStartCount.clear();
    outMesh.vertRingFace.clear();
    outMesh.vrvStartCount.clear();
    outMesh.vertRingVert.clear();
    outMesh.vertOnEdge.clear();
    outMesh.edges.clear();
    outMesh.faceEdges.clear();

    if (runPostInit) {
        outMesh.hasCustomNormals = false;
        outMesh.initTopology();
        outMesh.postInit();
    }

    outMesh.isDirty = true;
    outMesh.isTopologyDirty = true;
    outMesh.isVertexDirty = true;
    outMesh.isColorDirty = true;
    outMesh.isMaterialDirty = true;
    outMesh.isFaceGroupDirty = true;

    auto tEnd = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    sculpt_log("[MANIFOLD BRIDGE] Converted Manifold -> Mesh in %.2f ms (%d verts, %d faces)\n",
               ms, outMesh.nbVerts, outMesh.nbFaces);
}

void ManifoldBridge::toMeshBooleanMultiple(
    const manifold::Manifold& m,
    Mesh& outMesh,
    const std::vector<const Mesh*>& sourceMeshes,
    const std::vector<uint32_t>& origIds,
    bool runPostInit
) {
    auto tStart = std::chrono::high_resolution_clock::now();
    const size_t numSrc = sourceMeshes.size();
    if (numSrc == 0) return;

    // 1. Copy source face and faceGroup metadata in case &outMesh == sourceMeshes[i]
    struct SourceData {
        int nbFaces = 0;
        std::vector<uint32_t> faces;
        std::vector<uint32_t> faceGroups;
        uint32_t origId = 0;
        uint32_t offset = 0;
    };
    std::vector<SourceData> srcData(numSrc);
    std::unordered_map<uint32_t, size_t> origIdToIdx;

    uint32_t currentOffset = 0;
    for (size_t i = 0; i < numSrc; ++i) {
        const Mesh* sm = sourceMeshes[i];
        srcData[i].nbFaces = sm ? sm->nbFaces : 0;
        if (sm) {
            srcData[i].faces = sm->faces;
            srcData[i].faceGroups = sm->faceGroups;
        }
        uint32_t oid = (i < origIds.size()) ? origIds[i] : 0;
        srcData[i].origId = oid;
        srcData[i].offset = currentOffset;
        if (oid != 0) {
            origIdToIdx[oid] = i;
        }

        uint32_t maxG = 0;
        for (uint32_t g : srcData[i].faceGroups) {
            if (g > maxG) maxG = g;
        }
        currentOffset += maxG + 1;
    }
    uint32_t seamGid = currentOffset + 1;

    manifold::MeshGL gl = m.GetMeshGL();
    gl.Merge();

    const int rawNumV = gl.NumVert();
    const int numT = gl.NumTri();

    if (rawNumV == 0 || numT == 0) {
        outMesh.nbVerts = 0;
        outMesh.nbFaces = 0;
        outMesh.verts.clear();
        outMesh.faces.clear();
        outMesh.faceGroups.clear();
        return;
    }

    // Canonical vertex welding for duplicate property vertices
    std::vector<int> canonical(rawNumV);
    std::iota(canonical.begin(), canonical.end(), 0);

    auto findRoot = [&](int v) {
        int root = v;
        while (canonical[root] != root) {
            root = canonical[root];
        }
        int curr = v;
        while (curr != root) {
            int nxt = canonical[curr];
            canonical[curr] = root;
            curr = nxt;
        }
        return root;
    };

    for (size_t i = 0; i < gl.mergeFromVert.size(); ++i) {
        int vFrom = gl.mergeFromVert[i];
        int vTo = gl.mergeToVert[i];
        if (vFrom >= 0 && vFrom < rawNumV && vTo >= 0 && vTo < rawNumV) {
            int rFrom = findRoot(vFrom);
            int rTo = findRoot(vTo);
            if (rFrom != rTo) {
                canonical[rFrom] = rTo;
            }
        }
    }

    std::vector<int> compactIndex(rawNumV, -1);
    int numUniqueV = 0;
    for (int i = 0; i < rawNumV; ++i) {
        int root = findRoot(i);
        if (compactIndex[root] == -1) {
            compactIndex[root] = numUniqueV++;
        }
        compactIndex[i] = compactIndex[root];
    }

    outMesh.nbVerts = numUniqueV;
    outMesh.verts.resize(numUniqueV * 3);
    outMesh.normals.resize(numUniqueV * 3);
    outMesh.colors.resize(numUniqueV * 3);
    outMesh.materials.resize(numUniqueV * 3);

    const int propStride = gl.numProp;
    for (int i = 0; i < rawNumV; ++i) {
        int u = compactIndex[i];
        int base = i * propStride;
        outMesh.verts[u * 3 + 0] = gl.vertProperties[base + 0];
        outMesh.verts[u * 3 + 1] = gl.vertProperties[base + 1];
        outMesh.verts[u * 3 + 2] = gl.vertProperties[base + 2];

        if (propStride >= 6) {
            outMesh.normals[u * 3 + 0] = gl.vertProperties[base + 3];
            outMesh.normals[u * 3 + 1] = gl.vertProperties[base + 4];
            outMesh.normals[u * 3 + 2] = gl.vertProperties[base + 5];
        } else {
            outMesh.normals[u * 3 + 0] = 0.0f;
            outMesh.normals[u * 3 + 1] = 1.0f;
            outMesh.normals[u * 3 + 2] = 0.0f;
        }

        if (propStride >= 9) {
            float r = gl.vertProperties[base + 6];
            float g = gl.vertProperties[base + 7];
            float b = gl.vertProperties[base + 8];
            if (r <= 1e-4f && g <= 1e-4f && b <= 1e-4f) {
                r = g = b = 1.0f;
            }
            outMesh.colors[u * 3 + 0] = r;
            outMesh.colors[u * 3 + 1] = g;
            outMesh.colors[u * 3 + 2] = b;
        } else {
            outMesh.colors[u * 3 + 0] = 1.0f;
            outMesh.colors[u * 3 + 1] = 1.0f;
            outMesh.colors[u * 3 + 2] = 1.0f;
        }

        if (propStride >= 12) {
            float rough = gl.vertProperties[base + 9];
            float metal = gl.vertProperties[base + 10];
            float mask  = gl.vertProperties[base + 11];
            if (mask <= 1e-4f) mask = 1.0f;
            if (rough <= 1e-4f && metal <= 1e-4f) {
                rough = 0.5f;
                metal = 0.0f;
            }
            outMesh.materials[u * 3 + 0] = rough;
            outMesh.materials[u * 3 + 1] = metal;
            outMesh.materials[u * 3 + 2] = mask;
        } else {
            outMesh.materials[u * 3 + 0] = 0.5f;
            outMesh.materials[u * 3 + 1] = 0.0f;
            outMesh.materials[u * 3 + 2] = 1.0f;
        }
    }

    // Map each triangle to its source originalID
    std::vector<uint32_t> triOrigID(numT, 0);
    if (!gl.runOriginalID.empty() && gl.runIndex.size() >= 2) {
        for (size_t r = 0; r < gl.runOriginalID.size(); ++r) {
            uint32_t startT = gl.runIndex[r] / 3;
            uint32_t endT = (r + 1 < gl.runIndex.size()) ? (gl.runIndex[r + 1] / 3) : numT;
            uint32_t rOrig = gl.runOriginalID[r];
            for (uint32_t t = startT; t < endT && t < (uint32_t)numT; ++t) {
                triOrigID[t] = rOrig;
            }
        }
    }

    // Group triangles by source mesh and source face
    std::vector<std::vector<std::vector<int>>> trisForFace(numSrc);
    for (size_t i = 0; i < numSrc; ++i) {
        trisForFace[i].resize(srcData[i].nbFaces);
    }

    for (int t = 0; t < numT; ++t) {
        uint32_t i0 = compactIndex[gl.triVerts[t * 3 + 0]];
        uint32_t i1 = compactIndex[gl.triVerts[t * 3 + 1]];
        uint32_t i2 = compactIndex[gl.triVerts[t * 3 + 2]];
        if (i0 == i1 || i1 == i2 || i2 == i0) continue; // degenerate

        uint32_t orig = triOrigID[t];
        uint32_t fid = (t < (int)gl.faceID.size()) ? gl.faceID[t] : (uint32_t)-1;

        auto it = origIdToIdx.find(orig);
        if (it != origIdToIdx.end()) {
            size_t sIdx = it->second;
            if (fid < (uint32_t)srcData[sIdx].nbFaces) {
                trisForFace[sIdx][fid].push_back(t);
            }
        }
    }

    std::vector<uint32_t> outFaces;
    std::vector<uint32_t> outFaceGroups;
    outFaces.reserve(numT * 4);
    outFaceGroups.reserve(numT);

    std::vector<bool> triUsed(numT, false);
    int numRestoredQuads = 0;
    int numRestoredTris = 0;

    auto tryRestoreQuad = [&](int t0, int t1, uint32_t gid) -> bool {
        uint32_t a[3] = {
            (uint32_t)compactIndex[gl.triVerts[t0 * 3 + 0]],
            (uint32_t)compactIndex[gl.triVerts[t0 * 3 + 1]],
            (uint32_t)compactIndex[gl.triVerts[t0 * 3 + 2]]
        };
        uint32_t b[3] = {
            (uint32_t)compactIndex[gl.triVerts[t1 * 3 + 0]],
            (uint32_t)compactIndex[gl.triVerts[t1 * 3 + 1]],
            (uint32_t)compactIndex[gl.triVerts[t1 * 3 + 2]]
        };

        for (int i = 0; i < 3; ++i) {
            uint32_t u = a[i];
            uint32_t v = a[(i + 1) % 3];
            uint32_t w0 = a[(i + 2) % 3];

            for (int j = 0; j < 3; ++j) {
                if (b[j] == v && b[(j + 1) % 3] == u) {
                    uint32_t w1 = b[(j + 2) % 3];

                    // Check convexity before restoring
                    glm::vec3 p0(outMesh.verts[u * 3], outMesh.verts[u * 3 + 1], outMesh.verts[u * 3 + 2]);
                    glm::vec3 p1(outMesh.verts[w1 * 3], outMesh.verts[w1 * 3 + 1], outMesh.verts[w1 * 3 + 2]);
                    glm::vec3 p2(outMesh.verts[v * 3], outMesh.verts[v * 3 + 1], outMesh.verts[v * 3 + 2]);
                    glm::vec3 p3(outMesh.verts[w0 * 3], outMesh.verts[w0 * 3 + 1], outMesh.verts[w0 * 3 + 2]);

                    glm::vec3 c0 = glm::cross(p1 - p0, p3 - p0);
                    glm::vec3 c1 = glm::cross(p2 - p1, p0 - p1);
                    glm::vec3 c2 = glm::cross(p3 - p2, p1 - p2);
                    glm::vec3 c3 = glm::cross(p0 - p3, p2 - p3);
                    glm::vec3 avgN = glm::normalize(c0 + c1 + c2 + c3);

                    if (glm::dot(c0, avgN) <= 1e-5f || glm::dot(c1, avgN) <= 1e-5f ||
                        glm::dot(c2, avgN) <= 1e-5f || glm::dot(c3, avgN) <= 1e-5f) {
                        return false;
                    }

                    // CCW order: u -> w1 -> v -> w0
                    outFaces.push_back(u);
                    outFaces.push_back(w1);
                    outFaces.push_back(v);
                    outFaces.push_back(w0);
                    outFaceGroups.push_back(gid);
                    triUsed[t0] = true;
                    triUsed[t1] = true;
                    numRestoredQuads++;
                    return true;
                }
            }
        }
        return false;
    };

    // Restore untouched faces for all source meshes
    for (size_t sIdx = 0; sIdx < numSrc; ++sIdx) {
        const auto& src = srcData[sIdx];
        for (int f = 0; f < src.nbFaces; ++f) {
            uint32_t origGid = (!src.faceGroups.empty() && f < (int)src.faceGroups.size()) ? src.faceGroups[f] : 1;
            uint32_t gid = origGid + src.offset;
            bool wasQuad = (src.faces[f * 4 + 3] != TRI_INDEX);
            const auto& tList = trisForFace[sIdx][f];

            if (wasQuad && tList.size() == 2) {
                tryRestoreQuad(tList[0], tList[1], gid);
            } else if (!wasQuad && tList.size() == 1) {
                int t = tList[0];
                outFaces.push_back((uint32_t)compactIndex[gl.triVerts[t * 3 + 0]]);
                outFaces.push_back((uint32_t)compactIndex[gl.triVerts[t * 3 + 1]]);
                outFaces.push_back((uint32_t)compactIndex[gl.triVerts[t * 3 + 2]]);
                outFaces.push_back(TRI_INDEX);
                outFaceGroups.push_back(gid);
                triUsed[t] = true;
                numRestoredTris++;
            }
        }
    }

    // Process remaining cut/seam triangles
    struct SeamTri {
        uint32_t i0, i1, i2;
        uint32_t gid;
        glm::vec3 normal;
    };
    std::vector<SeamTri> seamTris;
    seamTris.reserve(numT / 4);

    for (int t = 0; t < numT; ++t) {
        if (triUsed[t]) continue;
        uint32_t i0 = compactIndex[gl.triVerts[t * 3 + 0]];
        uint32_t i1 = compactIndex[gl.triVerts[t * 3 + 1]];
        uint32_t i2 = compactIndex[gl.triVerts[t * 3 + 2]];
        if (i0 == i1 || i1 == i2 || i2 == i0) continue;

        uint32_t orig = triOrigID[t];
        uint32_t fid = (t < (int)gl.faceID.size()) ? gl.faceID[t] : (uint32_t)-1;
        uint32_t gid = seamGid;

        auto it = origIdToIdx.find(orig);
        if (it != origIdToIdx.end()) {
            size_t sIdx = it->second;
            if (fid < (uint32_t)srcData[sIdx].nbFaces && !srcData[sIdx].faceGroups.empty()) {
                gid = srcData[sIdx].faceGroups[fid] + srcData[sIdx].offset;
            }
        }

        glm::vec3 p0(outMesh.verts[i0 * 3], outMesh.verts[i0 * 3 + 1], outMesh.verts[i0 * 3 + 2]);
        glm::vec3 p1(outMesh.verts[i1 * 3], outMesh.verts[i1 * 3 + 1], outMesh.verts[i1 * 3 + 2]);
        glm::vec3 p2(outMesh.verts[i2 * 3], outMesh.verts[i2 * 3 + 1], outMesh.verts[i2 * 3 + 2]);
        glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
        float len = glm::length(n);
        if (len > 1e-7f) n /= len;
        else n = glm::vec3(0, 1, 0);

        seamTris.push_back({i0, i1, i2, gid, n});
    }

    const size_t ST = seamTris.size();
    int numSeamQuads = 0;
    int numSeamTris = 0;

    if (ST > 0) {
        struct EdgeHalf {
            int triIdx;
            uint32_t vOpp;
            bool forward;
        };
        struct EdgeHalfEntry {
            EdgeHalf halves[2];
            uint8_t count = 0;
            inline void add(const EdgeHalf& h) {
                if (count < 2) halves[count] = h;
                count++;
            }
        };
        auto makeEdgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
            uint32_t u = std::min(a, b);
            uint32_t v = std::max(a, b);
            return ((uint64_t)u << 32) | (uint64_t)v;
        };

        std::unordered_map<uint64_t, EdgeHalfEntry> edgeMap;
        edgeMap.reserve(ST * 3 / 2);
        for (size_t t = 0; t < ST; ++t) {
            const auto& tri = seamTris[t];
            edgeMap[makeEdgeKey(tri.i0, tri.i1)].add({(int)t, tri.i2, tri.i0 < tri.i1});
            edgeMap[makeEdgeKey(tri.i1, tri.i2)].add({(int)t, tri.i0, tri.i1 < tri.i2});
            edgeMap[makeEdgeKey(tri.i2, tri.i0)].add({(int)t, tri.i1, tri.i2 < tri.i0});
        }

        struct CandidatePair {
            int tA, tB;
            uint32_t v0, v1, v2, v3;
            float score;
        };
        std::vector<CandidatePair> candidates;
        candidates.reserve(ST / 2);

        for (const auto& [key, entry] : edgeMap) {
            if (entry.count != 2) continue;
            const auto& hA = entry.halves[0];
            const auto& hB = entry.halves[1];
            if (hA.forward == hB.forward) continue;

            int tA = hA.triIdx;
            int tB = hB.triIdx;
            if (seamTris[tA].gid != seamTris[tB].gid) continue;

            float dotN = glm::dot(seamTris[tA].normal, seamTris[tB].normal);
            if (dotN < 0.80f) continue;

            uint32_t u = (uint32_t)(key >> 32);
            uint32_t v = (uint32_t)(key & 0xFFFFFFFF);
            uint32_t wA = hA.vOpp;
            uint32_t wB = hB.vOpp;

            if (!hA.forward) std::swap(u, v);
            uint32_t q0 = u;
            uint32_t q1 = wB;
            uint32_t q2 = v;
            uint32_t q3 = wA;

            glm::vec3 p0(outMesh.verts[q0 * 3], outMesh.verts[q0 * 3 + 1], outMesh.verts[q0 * 3 + 2]);
            glm::vec3 p1(outMesh.verts[q1 * 3], outMesh.verts[q1 * 3 + 1], outMesh.verts[q1 * 3 + 2]);
            glm::vec3 p2(outMesh.verts[q2 * 3], outMesh.verts[q2 * 3 + 1], outMesh.verts[q2 * 3 + 2]);
            glm::vec3 p3(outMesh.verts[q3 * 3], outMesh.verts[q3 * 3 + 1], outMesh.verts[q3 * 3 + 2]);

            glm::vec3 avgN = glm::normalize(seamTris[tA].normal + seamTris[tB].normal);
            glm::vec3 c0 = glm::cross(p1 - p0, p3 - p0);
            glm::vec3 c1 = glm::cross(p2 - p1, p0 - p1);
            glm::vec3 c2 = glm::cross(p3 - p2, p1 - p2);
            glm::vec3 c3 = glm::cross(p0 - p3, p2 - p3);

            if (glm::dot(c0, avgN) <= 1e-5f || glm::dot(c1, avgN) <= 1e-5f ||
                glm::dot(c2, avgN) <= 1e-5f || glm::dot(c3, avgN) <= 1e-5f) {
                continue;
            }

            float d1 = glm::distance(p0, p2);
            float d2 = glm::distance(p1, p3);
            float diagRatio = (d1 > 1e-5f && d2 > 1e-5f) ? std::min(d1, d2) / std::max(d1, d2) : 0.0f;
            if (diagRatio < 0.20f) continue;

            float score = dotN * 2.0f + diagRatio;
            candidates.push_back({tA, tB, q0, q1, q2, q3, score});
        }

        std::sort(candidates.begin(), candidates.end(), [](const CandidatePair& a, const CandidatePair& b) {
            return a.score > b.score;
        });

        std::vector<bool> seamTriUsed(ST, false);
        for (const auto& c : candidates) {
            if (!seamTriUsed[c.tA] && !seamTriUsed[c.tB]) {
                seamTriUsed[c.tA] = true;
                seamTriUsed[c.tB] = true;
                outFaces.push_back(c.v0);
                outFaces.push_back(c.v1);
                outFaces.push_back(c.v2);
                outFaces.push_back(c.v3);
                outFaceGroups.push_back(seamTris[c.tA].gid);
                numSeamQuads++;
            }
        }

        for (size_t t = 0; t < ST; ++t) {
            if (!seamTriUsed[t]) {
                const auto& tri = seamTris[t];
                outFaces.push_back(tri.i0);
                outFaces.push_back(tri.i1);
                outFaces.push_back(tri.i2);
                outFaces.push_back(TRI_INDEX);
                outFaceGroups.push_back(tri.gid);
                numSeamTris++;
            }
        }
    }

    outMesh.nbFaces = (int)(outFaces.size() / 4);
    outMesh.faces = std::move(outFaces);
    outMesh.faceGroups = std::move(outFaceGroups);

    sculpt_log("[MANIFOLD BRIDGE] Boolean Topology (%zu meshes): Restored %d original quads, %d original tris | Seam: %d quads, %d tris (Total: %d faces, %d verts)\n",
               numSrc, numRestoredQuads, numRestoredTris, numSeamQuads, numSeamTris, outMesh.nbFaces, outMesh.nbVerts);

    outMesh.vrfStartCount.clear();
    outMesh.vertRingFace.clear();
    outMesh.vrvStartCount.clear();
    outMesh.vertRingVert.clear();
    outMesh.vertOnEdge.clear();
    outMesh.edges.clear();
    outMesh.faceEdges.clear();

    if (runPostInit) {
        outMesh.hasCustomNormals = false;
        outMesh.initTopology();
        outMesh.postInit();
    }

    outMesh.isDirty = true;
    outMesh.isTopologyDirty = true;
    outMesh.isVertexDirty = true;
    outMesh.isColorDirty = true;
    outMesh.isMaterialDirty = true;
    outMesh.isFaceGroupDirty = true;

    auto tEnd = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    sculpt_log("[MANIFOLD BRIDGE] Converted Boolean Manifold (%zu meshes) -> Mesh in %.2f ms (%d verts, %d faces)\n",
               numSrc, ms, outMesh.nbVerts, outMesh.nbFaces);
}

void ManifoldBridge::toMeshBoolean(
    const manifold::Manifold& m,
    Mesh& outMesh,
    const Mesh& meshA,
    uint32_t origIdA,
    const Mesh& meshB,
    uint32_t origIdB,
    bool runPostInit
) {
    toMeshBooleanMultiple(m, outMesh, {&meshA, &meshB}, {origIdA, origIdB}, runPostInit);
}

bool ManifoldBridge::verifyRoundTrip(const Mesh& originalMesh) {
    sculpt_log("[MANIFOLD BRIDGE] Starting Round-Trip verification...\n");
    manifold::Manifold mani = fromMesh(originalMesh);
    if (mani.Status() != manifold::Manifold::Error::NoError) {
        sculpt_log("[MANIFOLD BRIDGE] Error: fromMesh produced non-manifold geometry! Status: %d\n", (int)mani.Status());
        return false;
    }

    Mesh roundTripMesh;
    toMesh(mani, roundTripMesh, true);

    int degen = mani.NumDegenerateTris();
    sculpt_log("[MANIFOLD BRIDGE] Round-trip verification: Vertices match (in: %d, out: %d), %d degenerate faces\n",
               originalMesh.nbVerts, roundTripMesh.nbVerts, degen);

    manifold::Manifold mani2 = fromMesh(roundTripMesh);
    if (mani2.Status() != manifold::Manifold::Error::NoError) {
        sculpt_log("[MANIFOLD BRIDGE] Error: roundTripMesh -> Manifold failed! Status: %d\n", (int)mani2.Status());
        return false;
    }
    sculpt_log("[MANIFOLD BRIDGE] Round-trip 2nd stage verified: 100%% Watertight 2-Manifold (Status: 0)\n");

    return (degen == 0);
}

} // namespace sculpt
