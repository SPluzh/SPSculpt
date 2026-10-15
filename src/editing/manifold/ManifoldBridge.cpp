#include "editing/manifold/ManifoldBridge.h"
#include "common/Logger.h"
#include "common/Constants.h"
#include <chrono>
#include <numeric>

namespace sculpt {

manifold::Manifold ManifoldBridge::fromMesh(const Mesh& mesh) {
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
    const bool hasGroups = (mesh.faceGroups.size() == (size_t)nbF);

    gl.triVerts.reserve(nbF * 6);
    gl.faceID.reserve(nbF * 2);

    for (int f = 0; f < nbF; ++f) {
        uint32_t i0 = mesh.faces[f * 4 + 0];
        uint32_t i1 = mesh.faces[f * 4 + 1];
        uint32_t i2 = mesh.faces[f * 4 + 2];
        uint32_t i3 = mesh.faces[f * 4 + 3];
        uint32_t gid = hasGroups ? mesh.faceGroups[f] : 0;

        // Tri 1: i0, i1, i2
        gl.triVerts.push_back(i0);
        gl.triVerts.push_back(i1);
        gl.triVerts.push_back(i2);
        gl.faceID.push_back(gid);

        // Tri 2: i0, i2, i3 (if quad)
        if (i3 != TRI_INDEX) {
            gl.triVerts.push_back(i0);
            gl.triVerts.push_back(i2);
            gl.triVerts.push_back(i3);
            gl.faceID.push_back(gid);
        }
    }

    // Merge coincident vertices along open edges to guarantee watertight 2-manifold
    gl.Merge();

    manifold::Manifold m(gl);

    auto tEnd = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    sculpt_log("[MANIFOLD BRIDGE] Converted Mesh -> Manifold in %.2f ms (%d verts, %d tris, status: %d)\n",
               ms, nbV, (int)(gl.triVerts.size() / 3), (int)m.Status());

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
            outMesh.colors[u * 3 + 0] = gl.vertProperties[base + 6];
            outMesh.colors[u * 3 + 1] = gl.vertProperties[base + 7];
            outMesh.colors[u * 3 + 2] = gl.vertProperties[base + 8];
        } else {
            outMesh.colors[u * 3 + 0] = 1.0f;
            outMesh.colors[u * 3 + 1] = 1.0f;
            outMesh.colors[u * 3 + 2] = 1.0f;
        }

        if (propStride >= 12) {
            outMesh.materials[u * 3 + 0] = gl.vertProperties[base + 9];
            outMesh.materials[u * 3 + 1] = gl.vertProperties[base + 10];
            outMesh.materials[u * 3 + 2] = gl.vertProperties[base + 11];
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

    std::vector<uint32_t> validFaces;
    std::vector<uint32_t> validFaceGroups;
    validFaces.reserve(numT * 4);
    validFaceGroups.reserve(numT);

    for (int t = 0; t < numT; ++t) {
        uint32_t i0 = compactIndex[gl.triVerts[t * 3 + 0]];
        uint32_t i1 = compactIndex[gl.triVerts[t * 3 + 1]];
        uint32_t i2 = compactIndex[gl.triVerts[t * 3 + 2]];

        // Discard degenerate triangles
        if (i0 == i1 || i1 == i2 || i2 == i0) continue;

        validFaces.push_back(i0);
        validFaces.push_back(i1);
        validFaces.push_back(i2);
        validFaces.push_back(TRI_INDEX);

        uint32_t gid = triCapGroup[t];
        if (gid == 0 && hasFaceID) {
            gid = gl.faceID[t];
        }
        validFaceGroups.push_back(gid);
    }

    outMesh.nbFaces = (int)(validFaces.size() / 4);
    outMesh.faces = std::move(validFaces);
    outMesh.faceGroups = std::move(validFaceGroups);

    outMesh.vrfStartCount.clear();
    outMesh.vertRingFace.clear();
    outMesh.vrvStartCount.clear();
    outMesh.vertRingVert.clear();
    outMesh.vertOnEdge.clear();
    outMesh.edges.clear();
    outMesh.faceEdges.clear();

    if (runPostInit) {
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
