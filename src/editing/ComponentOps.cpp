#include "editing/ComponentOps.h"
#include "editing/SelectionManager.h"
#include "mesh/Mesh.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <limits>

// Helper to hash an undirected edge
static inline uint64_t makeEdgeKey(uint32_t a, uint32_t b) {
    uint32_t mn = std::min(a, b);
    uint32_t mx = std::max(a, b);
    return (uint64_t(mn) << 32) | uint64_t(mx);
}

bool ComponentOps::deleteSelection(Mesh& mesh, SelectionManager& selMgr) {
    if (mesh.nbFaces == 0 || mesh.nbVerts == 0) return false;
    if (!selMgr.hasSelection()) return false;

    ComponentMode mode = selMgr.getMode();
    std::unordered_set<uint32_t> facesToDelete;

    if (mode == ComponentMode::Face) {
        facesToDelete = selMgr.getSelectedFaces();
    } else if (mode == ComponentMode::Vertex) {
        const auto& selVerts = selMgr.getSelectedVertices();
        for (int f = 0; f < mesh.nbFaces; ++f) {
            int id = f * 4;
            int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
            for (int k = 0; k < numV; ++k) {
                if (selVerts.count(mesh.faces[id + k])) {
                    facesToDelete.insert(f);
                    break;
                }
            }
        }
    } else if (mode == ComponentMode::Edge) {
        selMgr.ensureEdgeTopology(mesh);
        const auto& edgeTop = selMgr.getEdgeTopology(mesh);
        const auto& selEdges = selMgr.getSelectedEdges();
        for (uint32_t e : selEdges) {
            if (e < edgeTop.size()) {
                if (edgeTop[e].f0 >= 0) facesToDelete.insert((uint32_t)edgeTop[e].f0);
                if (edgeTop[e].f1 >= 0) facesToDelete.insert((uint32_t)edgeTop[e].f1);
            }
        }
    }

    if (facesToDelete.empty()) return false;

    // Collect surviving face indices
    std::vector<uint32_t> survivingFaceIndices;
    survivingFaceIndices.reserve(mesh.nbFaces - facesToDelete.size());
    for (int f = 0; f < mesh.nbFaces; ++f) {
        if (!facesToDelete.count(f)) {
            survivingFaceIndices.push_back((uint32_t)f);
        }
    }

    if (survivingFaceIndices.empty()) {
        mesh.verts.clear();
        mesh.normals.clear();
        mesh.colors.clear();
        mesh.materials.clear();
        mesh.faces.clear();
        mesh.faceGroups.clear();
        mesh.nbVerts = 0;
        mesh.nbFaces = 0;
        mesh.nbEdges = 0;
        mesh.vrfStartCount.clear();
        mesh.vertRingFace.clear();
        mesh.vrvStartCount.clear();
        mesh.vertRingVert.clear();
        mesh.faceEdges.clear();
        mesh.edges.clear();
        mesh.edgeCreases.clear();
        mesh.vertVisible.clear();
        mesh.faceVisible.clear();
        mesh.isTopologyDirty = true;
        mesh.postInit(true);
        mesh.invalidateDynamicSubdiv();
        mesh.isDirty = true;
        mesh.isTopologyDirty = true;
        selMgr.clear();
        selMgr.invalidateTopologyCache();
        return true;
    }

    // Remap vertices to eliminate unreferenced orphans
    std::vector<int> vertRemap(mesh.nbVerts, -1);
    int newNbVerts = 0;
    for (uint32_t f : survivingFaceIndices) {
        int id = f * 4;
        int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
        for (int k = 0; k < numV; ++k) {
            uint32_t v = mesh.faces[id + k];
            if (v < (uint32_t)mesh.nbVerts && vertRemap[v] == -1) {
                vertRemap[v] = newNbVerts++;
            }
        }
    }

    std::vector<float> newVerts(newNbVerts * 3);
    std::vector<float> newNormals(newNbVerts * 3);
    std::vector<float> newColors(newNbVerts * 3, 1.0f);
    std::vector<float> newMaterials(newNbVerts * 3);

    for (int oldV = 0; oldV < mesh.nbVerts; ++oldV) {
        int nv = vertRemap[oldV];
        if (nv != -1) {
            newVerts[nv * 3]     = mesh.verts[oldV * 3];
            newVerts[nv * 3 + 1] = mesh.verts[oldV * 3 + 1];
            newVerts[nv * 3 + 2] = mesh.verts[oldV * 3 + 2];

            if (mesh.normals.size() == (size_t)mesh.nbVerts * 3) {
                newNormals[nv * 3]     = mesh.normals[oldV * 3];
                newNormals[nv * 3 + 1] = mesh.normals[oldV * 3 + 1];
                newNormals[nv * 3 + 2] = mesh.normals[oldV * 3 + 2];
            }
            if (mesh.colors.size() == (size_t)mesh.nbVerts * 3) {
                newColors[nv * 3]     = mesh.colors[oldV * 3];
                newColors[nv * 3 + 1] = mesh.colors[oldV * 3 + 1];
                newColors[nv * 3 + 2] = mesh.colors[oldV * 3 + 2];
            }
            if (mesh.materials.size() == (size_t)mesh.nbVerts * 3) {
                newMaterials[nv * 3]     = mesh.materials[oldV * 3];
                newMaterials[nv * 3 + 1] = mesh.materials[oldV * 3 + 1];
                newMaterials[nv * 3 + 2] = mesh.materials[oldV * 3 + 2];
            } else {
                newMaterials[nv * 3] = 0.5f; newMaterials[nv * 3 + 1] = 0.0f; newMaterials[nv * 3 + 2] = 1.0f;
            }
        }
    }

    std::vector<uint32_t> newFaces;
    std::vector<uint32_t> newFaceGroups;
    newFaces.reserve(survivingFaceIndices.size() * 4);
    newFaceGroups.reserve(survivingFaceIndices.size());

    for (uint32_t f : survivingFaceIndices) {
        int id = f * 4;
        uint32_t v0 = mesh.faces[id];
        uint32_t v1 = mesh.faces[id + 1];
        uint32_t v2 = mesh.faces[id + 2];
        int nv0 = (v0 < (uint32_t)mesh.nbVerts) ? vertRemap[v0] : -1;
        int nv1 = (v1 < (uint32_t)mesh.nbVerts) ? vertRemap[v1] : -1;
        int nv2 = (v2 < (uint32_t)mesh.nbVerts) ? vertRemap[v2] : -1;
        if (nv0 == -1 || nv1 == -1 || nv2 == -1) continue;

        newFaces.push_back((uint32_t)nv0);
        newFaces.push_back((uint32_t)nv1);
        newFaces.push_back((uint32_t)nv2);
        uint32_t v3 = mesh.faces[id + 3];
        if (v3 != 0xffffffff && (int)v3 < mesh.nbVerts && vertRemap[v3] != -1) {
            newFaces.push_back((uint32_t)vertRemap[v3]);
        } else {
            newFaces.push_back(0xffffffff);
        }
        if (mesh.faceGroups.size() == (size_t)mesh.nbFaces) {
            newFaceGroups.push_back(mesh.faceGroups[f]);
        } else {
            newFaceGroups.push_back(0);
        }
    }

    mesh.verts = std::move(newVerts);
    mesh.normals = std::move(newNormals);
    mesh.colors = std::move(newColors);
    mesh.materials = std::move(newMaterials);
    mesh.faces = std::move(newFaces);
    mesh.faceGroups = std::move(newFaceGroups);
    mesh.nbVerts = newNbVerts;
    mesh.nbFaces = (int)(mesh.faces.size() / 4);

    mesh.vrfStartCount.clear();
    mesh.vertRingFace.clear();
    mesh.vrvStartCount.clear();
    mesh.vertRingVert.clear();
    mesh.faceEdges.clear();
    mesh.edges.clear();
    mesh.edgeCreases.clear();
    mesh.vertVisible.assign(mesh.nbVerts, 1);
    mesh.faceVisible.assign(mesh.nbFaces, 1);
    mesh.isTopologyDirty = true;
    mesh.hasCustomNormals = false;

    mesh.postInit(true);
    mesh.invalidateDynamicSubdiv();
    mesh.isDirty = true;
    mesh.isTopologyDirty = true;

    selMgr.clear();
    selMgr.invalidateTopologyCache();
    return true;
}

bool ComponentOps::extrudeSelection(Mesh& mesh, SelectionManager& selMgr, float offsetMagnitude) {
    if (selMgr.getMode() == ComponentMode::Face || selMgr.getSelectedFaceCount() > 0) {
        return extrudeFaces(mesh, selMgr, offsetMagnitude);
    } else if (selMgr.getMode() == ComponentMode::Edge) {
        return extrudeEdges(mesh, selMgr, offsetMagnitude);
    } else if (selMgr.getMode() == ComponentMode::Vertex) {
        const auto& selVerts = selMgr.getSelectedVertices();
        if (selVerts.empty()) return false;

        // Check if any full faces are formed by selected vertices
        bool foundFullFace = false;
        for (int f = 0; f < mesh.nbFaces; ++f) {
            int id = f * 4;
            int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
            bool allIn = true;
            for (int k = 0; k < numV; ++k) {
                if (!selVerts.count(mesh.faces[id + k])) {
                    allIn = false;
                    break;
                }
            }
            if (allIn) {
                selMgr.selectFace((uint32_t)f, true);
                foundFullFace = true;
            }
        }
        if (foundFullFace) {
            selMgr.setMode(ComponentMode::Face);
            return extrudeFaces(mesh, selMgr, offsetMagnitude);
        }

        // If no full face, check if edges are formed by selected vertices
        selMgr.ensureEdgeTopology(mesh);
        const auto& edgeTop = selMgr.getEdgeTopology(mesh);
        bool foundEdge = false;
        for (size_t e = 0; e < edgeTop.size(); ++e) {
            if (selVerts.count(edgeTop[e].v0) && selVerts.count(edgeTop[e].v1)) {
                selMgr.selectEdge((uint32_t)e, true);
                foundEdge = true;
            }
        }
        if (foundEdge) {
            selMgr.setMode(ComponentMode::Edge);
            return extrudeEdges(mesh, selMgr, offsetMagnitude);
        }
    }
    return false;
}

bool ComponentOps::extrudeFaces(Mesh& mesh, SelectionManager& selMgr, float offsetMagnitude) {
    const auto& selFaces = selMgr.getSelectedFaces();
    if (selFaces.empty() || mesh.nbFaces == 0 || mesh.nbVerts == 0) return false;

    // 1. Identify boundary directed edges of the selected face patch
    struct DirectedEdge {
        uint32_t v0;
        uint32_t v1;
        uint32_t faceIdx;
    };
    std::vector<DirectedEdge> allEdges;
    std::unordered_map<uint64_t, int> edgeUsage;

    for (uint32_t f : selFaces) {
        if (f >= (uint32_t)mesh.nbFaces) continue;
        int id = f * 4;
        int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
        for (int k = 0; k < numV; ++k) {
            uint32_t va = mesh.faces[id + k];
            uint32_t vb = mesh.faces[id + ((k + 1) % numV)];
            allEdges.push_back({va, vb, f});
            edgeUsage[makeEdgeKey(va, vb)]++;
        }
    }

    std::vector<DirectedEdge> boundaryEdges;
    for (const auto& de : allEdges) {
        if (edgeUsage[makeEdgeKey(de.v0, de.v1)] == 1) {
            boundaryEdges.push_back(de);
        }
    }

    // 2. Collect unique vertices in selected faces
    std::unordered_set<uint32_t> selVerts;
    for (uint32_t f : selFaces) {
        int id = f * 4;
        int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
        for (int k = 0; k < numV; ++k) {
            selVerts.insert(mesh.faces[id + k]);
        }
    }

    // 3. Compute extrusion direction for each vertex directly from geometry
    std::unordered_map<uint32_t, glm::vec3> vertNormals;
    for (uint32_t f : selFaces) {
        if (f >= (uint32_t)mesh.nbFaces) continue;
        int id = f * 4;
        int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;

        glm::vec3 p0(mesh.verts[mesh.faces[id] * 3], mesh.verts[mesh.faces[id] * 3 + 1], mesh.verts[mesh.faces[id] * 3 + 2]);
        glm::vec3 p1(mesh.verts[mesh.faces[id + 1] * 3], mesh.verts[mesh.faces[id + 1] * 3 + 1], mesh.verts[mesh.faces[id + 1] * 3 + 2]);
        glm::vec3 p2(mesh.verts[mesh.faces[id + 2] * 3], mesh.verts[mesh.faces[id + 2] * 3 + 1], mesh.verts[mesh.faces[id + 2] * 3 + 2]);
        glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
        if (numV == 4) {
            glm::vec3 p3(mesh.verts[mesh.faces[id + 3] * 3], mesh.verts[mesh.faces[id + 3] * 3 + 1], mesh.verts[mesh.faces[id + 3] * 3 + 2]);
            fn += glm::cross(p2 - p0, p3 - p0);
        }
        if (glm::length(fn) > 1e-6f) fn = glm::normalize(fn);
        else fn = glm::vec3(0.0f, 1.0f, 0.0f);

        for (int k = 0; k < numV; ++k) {
            vertNormals[mesh.faces[id + k]] += fn;
        }
    }
    for (auto& kv : vertNormals) {
        if (glm::length(kv.second) > 1e-5f) {
            kv.second = glm::normalize(kv.second);
        } else {
            kv.second = glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }

    if (std::abs(offsetMagnitude) < 1e-5f) {
        float rad = mesh.computeLocalRadius();
        offsetMagnitude = (rad > 0.01f) ? rad * 0.05f : 0.5f;
    }

    // Check if selected faces form an isolated open sheet/plane (no unselected faces sharing boundary edges)
    bool hasUnselectedNeighbor = false;
    for (int f = 0; f < mesh.nbFaces; ++f) {
        if (selFaces.count((uint32_t)f)) continue;
        int id = f * 4;
        int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
        for (int k = 0; k < numV; ++k) {
            uint32_t va = mesh.faces[id + k];
            uint32_t vb = mesh.faces[id + ((k + 1) % numV)];
            if (edgeUsage.count(makeEdgeKey(va, vb))) {
                hasUnselectedNeighbor = true;
                break;
            }
        }
        if (hasUnselectedNeighbor) break;
    }

    // If it's an isolated open surface (e.g. flat plane), preserve bottom faces to keep it a closed watertight 3D prism
    std::vector<uint32_t> bottomFaces;
    std::vector<uint32_t> bottomFaceGroups;
    if (!hasUnselectedNeighbor) {
        uint32_t botGid = mesh.getNextFreeGroupID();
        for (uint32_t f : selFaces) {
            int id = f * 4;
            int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
            if (numV == 3) {
                bottomFaces.push_back(mesh.faces[id + 0]);
                bottomFaces.push_back(mesh.faces[id + 2]);
                bottomFaces.push_back(mesh.faces[id + 1]);
                bottomFaces.push_back(0xffffffff);
            } else {
                bottomFaces.push_back(mesh.faces[id + 0]);
                bottomFaces.push_back(mesh.faces[id + 3]);
                bottomFaces.push_back(mesh.faces[id + 2]);
                bottomFaces.push_back(mesh.faces[id + 1]);
            }
            bottomFaceGroups.push_back(botGid);
        }
    }

    // 4. Duplicate vertices for the extruded cap
    std::unordered_map<uint32_t, uint32_t> oldToNew;
    int origNbVerts = mesh.nbVerts;

    mesh.verts.reserve((origNbVerts + selVerts.size()) * 3);
    mesh.normals.reserve((origNbVerts + selVerts.size()) * 3);
    mesh.colors.reserve((origNbVerts + selVerts.size()) * 3);
    mesh.materials.reserve((origNbVerts + selVerts.size()) * 3);

    for (uint32_t oldV : selVerts) {
        uint32_t newV = (uint32_t)mesh.nbVerts++;
        oldToNew[oldV] = newV;

        glm::vec3 origPos(mesh.verts[oldV * 3], mesh.verts[oldV * 3 + 1], mesh.verts[oldV * 3 + 2]);
        glm::vec3 n = vertNormals[oldV];
        glm::vec3 newPos = origPos + n * offsetMagnitude;

        mesh.verts.push_back(newPos.x);
        mesh.verts.push_back(newPos.y);
        mesh.verts.push_back(newPos.z);

        mesh.normals.push_back(n.x);
        mesh.normals.push_back(n.y);
        mesh.normals.push_back(n.z);

        if (mesh.colors.size() >= (size_t)(oldV + 1) * 3) {
            mesh.colors.push_back(mesh.colors[oldV * 3]);
            mesh.colors.push_back(mesh.colors[oldV * 3 + 1]);
            mesh.colors.push_back(mesh.colors[oldV * 3 + 2]);
        } else {
            mesh.colors.push_back(1.0f); mesh.colors.push_back(1.0f); mesh.colors.push_back(1.0f);
        }

        if (mesh.materials.size() >= (size_t)(oldV + 1) * 3) {
            mesh.materials.push_back(mesh.materials[oldV * 3]);
            mesh.materials.push_back(mesh.materials[oldV * 3 + 1]);
            mesh.materials.push_back(mesh.materials[oldV * 3 + 2]);
        } else {
            mesh.materials.push_back(0.5f); mesh.materials.push_back(0.0f); mesh.materials.push_back(1.0f);
        }
    }

    // 5. Update cap faces: map them to the new extruded vertices
    for (uint32_t f : selFaces) {
        int id = f * 4;
        int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
        for (int k = 0; k < numV; ++k) {
            mesh.faces[id + k] = oldToNew[mesh.faces[id + k]];
        }
    }

    // 6. Generate side quad walls for each boundary edge
    uint32_t sideGid = mesh.getNextFreeGroupID();
    for (const auto& be : boundaryEdges) {
        uint32_t va = be.v0;
        uint32_t vb = be.v1;
        uint32_t va_ext = oldToNew[va];
        uint32_t vb_ext = oldToNew[vb];

        if (offsetMagnitude >= 0.0f) {
            mesh.faces.push_back(va);
            mesh.faces.push_back(vb);
            mesh.faces.push_back(vb_ext);
            mesh.faces.push_back(va_ext);
        } else {
            // Negative offset: reverse side quad winding so normals point outward into cavity
            mesh.faces.push_back(va);
            mesh.faces.push_back(va_ext);
            mesh.faces.push_back(vb_ext);
            mesh.faces.push_back(vb);
        }
        mesh.faceGroups.push_back(sideGid);
        mesh.nbFaces++;
    }

    // Append bottom faces if capped
    for (size_t i = 0; i < bottomFaces.size(); i += 4) {
        mesh.faces.push_back(bottomFaces[i]);
        mesh.faces.push_back(bottomFaces[i + 1]);
        mesh.faces.push_back(bottomFaces[i + 2]);
        mesh.faces.push_back(bottomFaces[i + 3]);
        mesh.faceGroups.push_back(bottomFaceGroups[i / 4]);
        mesh.nbFaces++;
    }

    mesh.vrfStartCount.clear();
    mesh.vertRingFace.clear();
    mesh.vrvStartCount.clear();
    mesh.vertRingVert.clear();
    mesh.faceEdges.clear();
    mesh.edges.clear();
    mesh.edgeCreases.clear();
    mesh.vertVisible.assign(mesh.nbVerts, 1);
    mesh.faceVisible.assign(mesh.nbFaces, 1);
    mesh.isTopologyDirty = true;
    mesh.hasCustomNormals = false;

    mesh.postInit(true);
    mesh.invalidateDynamicSubdiv();
    mesh.isDirty = true;
    mesh.isTopologyDirty = true;

    selMgr.invalidateTopologyCache();
    return true;
}

bool ComponentOps::extrudeEdges(Mesh& mesh, SelectionManager& selMgr, float offsetMagnitude) {
    const auto& selEdges = selMgr.getSelectedEdges();
    if (selEdges.empty() || mesh.nbEdges == 0 || mesh.nbVerts == 0) return false;

    selMgr.ensureEdgeTopology(mesh);
    const auto& edgeTop = selMgr.getEdgeTopology(mesh);

    if (std::abs(offsetMagnitude) < 1e-5f) {
        float rad = mesh.computeLocalRadius();
        offsetMagnitude = (rad > 0.01f) ? rad * 0.05f : 0.5f;
    }

    std::unordered_set<uint32_t> uniqueVerts;
    for (uint32_t e : selEdges) {
        if (e < edgeTop.size()) {
            uniqueVerts.insert(edgeTop[e].v0);
            uniqueVerts.insert(edgeTop[e].v1);
        }
    }

    // Compute outward extrusion direction along surface tangent for each vertex
    std::unordered_map<uint32_t, glm::vec3> vertDirections;
    for (uint32_t e : selEdges) {
        if (e >= edgeTop.size()) continue;
        const auto& top = edgeTop[e];
        uint32_t va = top.v0;
        uint32_t vb = top.v1;
        if (va >= (uint32_t)mesh.nbVerts || vb >= (uint32_t)mesh.nbVerts) continue;

        glm::vec3 pa(mesh.verts[va * 3], mesh.verts[va * 3 + 1], mesh.verts[va * 3 + 2]);
        glm::vec3 pb(mesh.verts[vb * 3], mesh.verts[vb * 3 + 1], mesh.verts[vb * 3 + 2]);
        glm::vec3 edgeVec = pb - pa;
        if (glm::length(edgeVec) < 1e-6f) continue;
        edgeVec = glm::normalize(edgeVec);

        glm::vec3 faceNorm(0.0f, 1.0f, 0.0f);
        if (top.f0 >= 0 && (size_t)top.f0 * 4 + 2 < mesh.faces.size()) {
            int id0 = top.f0 * 4;
            int numV0 = (mesh.faces[id0 + 3] != 0xffffffff) ? 4 : 3;
            glm::vec3 p0(mesh.verts[mesh.faces[id0] * 3], mesh.verts[mesh.faces[id0] * 3 + 1], mesh.verts[mesh.faces[id0] * 3 + 2]);
            glm::vec3 p1(mesh.verts[mesh.faces[id0 + 1] * 3], mesh.verts[mesh.faces[id0 + 1] * 3 + 1], mesh.verts[mesh.faces[id0 + 1] * 3 + 2]);
            glm::vec3 p2(mesh.verts[mesh.faces[id0 + 2] * 3], mesh.verts[mesh.faces[id0 + 2] * 3 + 1], mesh.verts[mesh.faces[id0 + 2] * 3 + 2]);
            glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
            if (numV0 == 4) {
                glm::vec3 p3(mesh.verts[mesh.faces[id0 + 3] * 3], mesh.verts[mesh.faces[id0 + 3] * 3 + 1], mesh.verts[mesh.faces[id0 + 3] * 3 + 2]);
                fn += glm::cross(p2 - p0, p3 - p0);
            }
            if (glm::length(fn) > 1e-6f) faceNorm = glm::normalize(fn);
        }

        bool f0HasVaToVb = false;
        if (top.f0 >= 0) {
            int id0 = top.f0 * 4;
            int numV0 = (mesh.faces[id0 + 3] != 0xffffffff) ? 4 : 3;
            for (int k = 0; k < numV0; ++k) {
                if (mesh.faces[id0 + k] == va && mesh.faces[id0 + ((k + 1) % numV0)] == vb) {
                    f0HasVaToVb = true;
                    break;
                }
            }
        }

        // Outward tangent: edgeVec x faceNorm (or faceNorm x edgeVec) pointing away from face interior
        glm::vec3 outward = f0HasVaToVb ? glm::cross(edgeVec, faceNorm) : glm::cross(faceNorm, edgeVec);
        if (glm::length(outward) > 1e-5f) outward = glm::normalize(outward);
        else outward = faceNorm;

        vertDirections[va] += outward;
        vertDirections[vb] += outward;
    }

    for (auto& kv : vertDirections) {
        if (glm::length(kv.second) > 1e-5f) {
            kv.second = glm::normalize(kv.second);
        } else {
            uint32_t v = kv.first;
            glm::vec3 n(0.0f, 1.0f, 0.0f);
            if (mesh.normals.size() >= (size_t)(v + 1) * 3) {
                n = glm::vec3(mesh.normals[v * 3], mesh.normals[v * 3 + 1], mesh.normals[v * 3 + 2]);
            }
            kv.second = (glm::length(n) > 1e-4f) ? glm::normalize(n) : glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }

    // Duplicate vertices
    std::unordered_map<uint32_t, uint32_t> oldToNew;
    for (uint32_t v : uniqueVerts) {
        if (v >= (uint32_t)mesh.nbVerts) continue;
        uint32_t newV = (uint32_t)mesh.nbVerts++;
        oldToNew[v] = newV;

        glm::vec3 pos(mesh.verts[v * 3], mesh.verts[v * 3 + 1], mesh.verts[v * 3 + 2]);
        glm::vec3 dir = vertDirections.count(v) ? vertDirections[v] : glm::vec3(0.0f, 1.0f, 0.0f);
        glm::vec3 newPos = pos + dir * offsetMagnitude;

        mesh.verts.push_back(newPos.x);
        mesh.verts.push_back(newPos.y);
        mesh.verts.push_back(newPos.z);

        mesh.normals.push_back(dir.x);
        mesh.normals.push_back(dir.y);
        mesh.normals.push_back(dir.z);

        if (mesh.colors.size() >= (size_t)(v + 1) * 3) {
            mesh.colors.push_back(mesh.colors[v * 3]);
            mesh.colors.push_back(mesh.colors[v * 3 + 1]);
            mesh.colors.push_back(mesh.colors[v * 3 + 2]);
        } else {
            mesh.colors.push_back(1.0f); mesh.colors.push_back(1.0f); mesh.colors.push_back(1.0f);
        }

        if (mesh.materials.size() >= (size_t)(v + 1) * 3) {
            mesh.materials.push_back(mesh.materials[v * 3]);
            mesh.materials.push_back(mesh.materials[v * 3 + 1]);
            mesh.materials.push_back(mesh.materials[v * 3 + 2]);
        } else {
            mesh.materials.push_back(0.5f); mesh.materials.push_back(0.0f); mesh.materials.push_back(1.0f);
        }
    }

    uint32_t gid = mesh.getNextFreeGroupID();
    std::vector<std::pair<uint32_t, uint32_t>> newOuterEdges;

    for (uint32_t e : selEdges) {
        if (e >= edgeTop.size()) continue;
        uint32_t va = edgeTop[e].v0;
        uint32_t vb = edgeTop[e].v1;
        if (!oldToNew.count(va) || !oldToNew.count(vb)) continue;

        bool f0HasVaToVb = false;
        if (edgeTop[e].f0 >= 0) {
            int id0 = edgeTop[e].f0 * 4;
            int numV0 = (mesh.faces[id0 + 3] != 0xffffffff) ? 4 : 3;
            for (int k = 0; k < numV0; ++k) {
                if (mesh.faces[id0 + k] == va && mesh.faces[id0 + ((k + 1) % numV0)] == vb) {
                    f0HasVaToVb = true;
                    break;
                }
            }
        }

        // Bottom edge must be opposite to f0 for 2-manifold continuity
        uint32_t b0 = f0HasVaToVb ? vb : va;
        uint32_t b1 = f0HasVaToVb ? va : vb;
        uint32_t b0_ext = oldToNew[b0];
        uint32_t b1_ext = oldToNew[b1];

        // 2-Manifold quad: b0 -> b1 -> b1_ext -> b0_ext
        mesh.faces.push_back(b0);
        mesh.faces.push_back(b1);
        mesh.faces.push_back(b1_ext);
        mesh.faces.push_back(b0_ext);
        mesh.faceGroups.push_back(gid);
        mesh.nbFaces++;

        newOuterEdges.push_back({std::min(b0_ext, b1_ext), std::max(b0_ext, b1_ext)});
    }

    mesh.vrfStartCount.clear();
    mesh.vertRingFace.clear();
    mesh.vrvStartCount.clear();
    mesh.vertRingVert.clear();
    mesh.faceEdges.clear();
    mesh.edges.clear();
    mesh.edgeCreases.clear();
    mesh.vertVisible.assign(mesh.nbVerts, 1);
    mesh.faceVisible.assign(mesh.nbFaces, 1);
    mesh.isTopologyDirty = true;
    mesh.hasCustomNormals = false;

    mesh.postInit(true);
    mesh.invalidateDynamicSubdiv();
    mesh.isDirty = true;
    mesh.isTopologyDirty = true;

    // Select the newly extruded outer edges so the user can immediately transform or extrude them again!
    selMgr.clear();
    selMgr.setMode(ComponentMode::Edge);
    selMgr.ensureEdgeTopology(mesh);
    const auto& newTop = selMgr.getEdgeTopology(mesh);
    std::unordered_map<uint64_t, uint32_t> edgeLookup;
    for (size_t i = 0; i < newTop.size(); ++i) {
        edgeLookup[makeEdgeKey(newTop[i].v0, newTop[i].v1)] = (uint32_t)i;
    }
    for (const auto& pr : newOuterEdges) {
        uint64_t key = makeEdgeKey(pr.first, pr.second);
        auto it = edgeLookup.find(key);
        if (it != edgeLookup.end()) {
            selMgr.selectEdge(it->second, true);
        }
    }

    selMgr.invalidateTopologyCache();
    return true;
}

bool ComponentOps::bevelSelection(Mesh& mesh, SelectionManager& selMgr, float width, int segments, BevelType type, float heightOffset) {
    if (selMgr.getMode() == ComponentMode::Face || selMgr.getSelectedFaceCount() > 0) {
        return bevelFaces(mesh, selMgr, width, segments, type, heightOffset);
    } else if (selMgr.getMode() == ComponentMode::Edge) {
        return bevelEdges(mesh, selMgr, width, segments, type, heightOffset);
    }
    return false;
}

bool ComponentOps::bevelFaces(Mesh& mesh, SelectionManager& selMgr, float width, int segments, BevelType type, float heightOffset) {
    const auto& selFaces = selMgr.getSelectedFaces();
    if (selFaces.empty() || mesh.nbFaces == 0 || mesh.nbVerts == 0) return false;

    int S = std::clamp(segments, 1, 10);
    float t = std::clamp(width, 0.001f, 0.99f);
    float effHeight = (type == BevelType::Solid) ? 0.0f : heightOffset;
    uint32_t rimGid = mesh.getNextFreeGroupID();
    std::unordered_set<uint32_t> newSelectedFaces;

    std::unordered_map<uint64_t, int> edgeUsage;
    for (uint32_t f : selFaces) {
        if (f >= (uint32_t)mesh.nbFaces) continue;
        int id = f * 4;
        int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
        for (int k = 0; k < numV; ++k) {
            uint32_t va = mesh.faces[id + k];
            uint32_t vb = mesh.faces[id + ((k + 1) % numV)];
            edgeUsage[makeEdgeKey(va, vb)]++;
        }
    }

    // Compute global characteristic edge length across all selected faces for uniform insetting
    float globalMinEdgeLen = 1e9f;
    float globalAvgEdgeLen = 0.0f;
    int globalEdgeCount = 0;
    for (uint32_t f : selFaces) {
        if (f >= (uint32_t)mesh.nbFaces) continue;
        int id = f * 4;
        int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
        for (int k = 0; k < numV; ++k) {
            uint32_t va = mesh.faces[id + k];
            uint32_t vb = mesh.faces[id + ((k + 1) % numV)];
            glm::vec3 pa(mesh.verts[va * 3], mesh.verts[va * 3 + 1], mesh.verts[va * 3 + 2]);
            glm::vec3 pb(mesh.verts[vb * 3], mesh.verts[vb * 3 + 1], mesh.verts[vb * 3 + 2]);
            float l = glm::length(pb - pa);
            if (l > 1e-6f) {
                globalMinEdgeLen = std::min(globalMinEdgeLen, l);
                globalAvgEdgeLen += l;
                globalEdgeCount++;
            }
        }
    }
    if (globalMinEdgeLen > 1e8f || globalMinEdgeLen < 1e-6f) globalMinEdgeLen = 1.0f;
    if (globalEdgeCount > 0) globalAvgEdgeLen /= (float)globalEdgeCount;
    else globalAvgEdgeLen = globalMinEdgeLen;

    float refLen = std::min(globalAvgEdgeLen, globalMinEdgeLen * 2.0f);
    float globalInsetDist = std::clamp(t * refLen * 0.5f, 0.0001f, globalMinEdgeLen * 0.48f);

    auto createNewVert = [&](uint32_t origV, const glm::vec3& newPos, const glm::vec3& normal) -> uint32_t {
        uint32_t newV = (uint32_t)mesh.nbVerts++;
        mesh.verts.push_back(newPos.x);
        mesh.verts.push_back(newPos.y);
        mesh.verts.push_back(newPos.z);

        mesh.normals.push_back(normal.x);
        mesh.normals.push_back(normal.y);
        mesh.normals.push_back(normal.z);

        if (mesh.colors.size() >= (size_t)(origV + 1) * 3) {
            mesh.colors.push_back(mesh.colors[origV * 3]);
            mesh.colors.push_back(mesh.colors[origV * 3 + 1]);
            mesh.colors.push_back(mesh.colors[origV * 3 + 2]);
        } else {
            mesh.colors.push_back(1.0f); mesh.colors.push_back(1.0f); mesh.colors.push_back(1.0f);
        }

        if (mesh.materials.size() >= (size_t)(origV + 1) * 3) {
            mesh.materials.push_back(mesh.materials[origV * 3]);
            mesh.materials.push_back(mesh.materials[origV * 3 + 1]);
            mesh.materials.push_back(mesh.materials[origV * 3 + 2]);
        } else {
            mesh.materials.push_back(0.5f); mesh.materials.push_back(0.0f); mesh.materials.push_back(1.0f);
        }
        return newV;
    };

    for (uint32_t f : selFaces) {
        if (f >= (uint32_t)mesh.nbFaces) continue;
        int id = f * 4;
        int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;

        // Compute face normal directly from vertex positions
        glm::vec3 p0(mesh.verts[mesh.faces[id] * 3], mesh.verts[mesh.faces[id] * 3 + 1], mesh.verts[mesh.faces[id] * 3 + 2]);
        glm::vec3 p1(mesh.verts[mesh.faces[id + 1] * 3], mesh.verts[mesh.faces[id + 1] * 3 + 1], mesh.verts[mesh.faces[id + 1] * 3 + 2]);
        glm::vec3 p2(mesh.verts[mesh.faces[id + 2] * 3], mesh.verts[mesh.faces[id + 2] * 3 + 1], mesh.verts[mesh.faces[id + 2] * 3 + 2]);
        glm::vec3 normal = glm::cross(p1 - p0, p2 - p0);
        if (numV == 4) {
            glm::vec3 p3(mesh.verts[mesh.faces[id + 3] * 3], mesh.verts[mesh.faces[id + 3] * 3 + 1], mesh.verts[mesh.faces[id + 3] * 3 + 2]);
            normal += glm::cross(p2 - p0, p3 - p0);
        }
        if (glm::length(normal) > 1e-6f) normal = glm::normalize(normal);
        else normal = glm::vec3(0.0f, 1.0f, 0.0f);

        // Find minimum edge length of this face to safely prevent self-intersection
        float minFEdgeLen = 1e9f;
        for (int k = 0; k < numV; ++k) {
            uint32_t va = mesh.faces[id + k];
            uint32_t vb = mesh.faces[id + ((k + 1) % numV)];
            glm::vec3 pa(mesh.verts[va * 3], mesh.verts[va * 3 + 1], mesh.verts[va * 3 + 2]);
            glm::vec3 pb(mesh.verts[vb * 3], mesh.verts[vb * 3 + 1], mesh.verts[vb * 3 + 2]);
            float l = glm::length(pb - pa);
            if (l > 1e-6f) minFEdgeLen = std::min(minFEdgeLen, l);
        }
        if (minFEdgeLen > 1e8f || minFEdgeLen < 1e-6f) minFEdgeLen = 1.0f;
        float insetDist = std::min(minFEdgeLen * 0.48f, globalInsetDist);

        std::vector<uint32_t> origVerts(numV);
        std::vector<glm::vec3> origPos(numV);
        std::vector<glm::vec3> targetInnerPos(numV);

        for (int k = 0; k < numV; ++k) {
            uint32_t origV = mesh.faces[id + k];
            origVerts[k] = origV;
            uint32_t origVPrev = mesh.faces[id + ((k - 1 + numV) % numV)];
            uint32_t origVNext = mesh.faces[id + ((k + 1) % numV)];

            glm::vec3 posV(mesh.verts[origV * 3], mesh.verts[origV * 3 + 1], mesh.verts[origV * 3 + 2]);
            glm::vec3 posPrev(mesh.verts[origVPrev * 3], mesh.verts[origVPrev * 3 + 1], mesh.verts[origVPrev * 3 + 2]);
            glm::vec3 posNext(mesh.verts[origVNext * 3], mesh.verts[origVNext * 3 + 1], mesh.verts[origVNext * 3 + 2]);
            origPos[k] = posV;

            glm::vec3 uPrev = glm::cross(normal, posV - posPrev);
            if (glm::length(uPrev) > 1e-6f) uPrev = glm::normalize(uPrev);
            glm::vec3 uNext = glm::cross(normal, posNext - posV);
            if (glm::length(uNext) > 1e-6f) uNext = glm::normalize(uNext);

            glm::vec3 fCenter(0.0f);
            for (int j = 0; j < numV; ++j) {
                uint32_t fv = mesh.faces[id + j];
                fCenter += glm::vec3(mesh.verts[fv * 3], mesh.verts[fv * 3 + 1], mesh.verts[fv * 3 + 2]);
            }
            fCenter /= (float)numV;
            glm::vec3 toCenter = fCenter - posV;
            if (glm::length(toCenter) > 1e-6f) {
                toCenter = glm::normalize(toCenter);
                if (glm::dot(uPrev, toCenter) < -1e-4f) uPrev = -uPrev;
                if (glm::dot(uNext, toCenter) < -1e-4f) uNext = -uNext;
            }

            glm::vec3 shift(0.0f);
            glm::vec3 bisector = uPrev + uNext;
            float bisLen = glm::length(bisector);
            if (bisLen > 1e-4f) {
                bisector /= bisLen;
                float cosHalf = std::max(0.15f, glm::dot(bisector, uPrev));
                float shiftLen = std::min(insetDist / cosHalf, insetDist * 2.5f);
                shift = bisector * shiftLen;
            } else {
                shift = uPrev * insetDist;
            }

            targetInnerPos[k] = posV + shift + normal * effHeight;
        }

        std::vector<std::vector<uint32_t>> rings(S + 1, std::vector<uint32_t>(numV));
        for (int k = 0; k < numV; ++k) {
            rings[0][k] = origVerts[k];
        }

        for (int s = 1; s <= S; ++s) {
            float frac = (float)s / (float)S;
            for (int k = 0; k < numV; ++k) {
                glm::vec3 p = (1.0f - frac) * origPos[k] + frac * targetInnerPos[k];
                rings[s][k] = createNewVert(origVerts[k], p, normal);
            }
        }

        for (int k = 0; k < numV; ++k) {
            mesh.faces[id + k] = rings[S][k];
        }
        newSelectedFaces.insert(f);

        for (int s = 0; s < S; ++s) {
            for (int k = 0; k < numV; ++k) {
                int kn = (k + 1) % numV;
                uint32_t va = origVerts[k];
                uint32_t vb = origVerts[kn];
                if (edgeUsage[makeEdgeKey(va, vb)] > 1) {
                    continue;
                }
                uint32_t r0_a = rings[s][k];
                uint32_t r0_b = rings[s][kn];
                uint32_t r1_b = rings[s + 1][kn];
                uint32_t r1_a = rings[s + 1][k];

                mesh.faces.push_back(r0_a);
                mesh.faces.push_back(r0_b);
                mesh.faces.push_back(r1_b);
                mesh.faces.push_back(r1_a);
                mesh.faceGroups.push_back(rimGid);
                mesh.nbFaces++;
            }
        }
    }

    mesh.vrfStartCount.clear();
    mesh.vertRingFace.clear();
    mesh.vrvStartCount.clear();
    mesh.vertRingVert.clear();
    mesh.faceEdges.clear();
    mesh.edges.clear();
    mesh.edgeCreases.clear();
    mesh.vertVisible.assign(mesh.nbVerts, 1);
    mesh.faceVisible.assign(mesh.nbFaces, 1);
    mesh.isTopologyDirty = true;
    mesh.hasCustomNormals = false;

    mesh.postInit(true);
    mesh.invalidateDynamicSubdiv();
    mesh.isDirty = true;
    mesh.isTopologyDirty = true;

    selMgr.invalidateTopologyCache();
    return true;
}

bool ComponentOps::bevelEdges(Mesh& mesh, SelectionManager& selMgr, float width, int segments, BevelType type, float heightOffset) {
    const auto& selEdges = selMgr.getSelectedEdges();
    if (selEdges.empty() || mesh.nbEdges == 0 || mesh.nbVerts == 0) return false;

    int S = std::clamp(segments, 1, 10);

    selMgr.ensureEdgeTopology(mesh);
    const auto& edgeTop = selMgr.getEdgeTopology(mesh);

    // 1. Gather valid selected edges
    std::unordered_set<uint32_t> selEdgeSet;
    for (uint32_t e : selEdges) {
        if (e < edgeTop.size() && edgeTop[e].f0 >= 0) {
            selEdgeSet.insert(e);
        }
    }
    if (selEdgeSet.empty()) return false;

    // 2. Build edge lookup from (minV, maxV) -> edge index in edgeTop
    std::unordered_map<uint64_t, uint32_t> edgeLookup;
    for (size_t e = 0; e < edgeTop.size(); ++e) {
        edgeLookup[makeEdgeKey(edgeTop[e].v0, edgeTop[e].v1)] = (uint32_t)e;
    }

    // 3. Count how many selected beveled edges meet at each vertex: k(v)
    std::unordered_map<uint32_t, std::vector<uint32_t>> vertToSelEdges;
    for (uint32_t e : selEdgeSet) {
        vertToSelEdges[edgeTop[e].v0].push_back(e);
        vertToSelEdges[edgeTop[e].v1].push_back(e);
    }

    // 4. Precalculate face centers and face normals
    std::vector<glm::vec3> faceNormals(mesh.nbFaces, glm::vec3(0.0f, 1.0f, 0.0f));
    std::vector<glm::vec3> faceCenters(mesh.nbFaces, glm::vec3(0.0f));

    for (int f = 0; f < mesh.nbFaces; ++f) {
        int id = f * 4;
        int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
        glm::vec3 c(0.0f);
        for (int k = 0; k < numV; ++k) {
            uint32_t v = mesh.faces[id + k];
            if (v < (uint32_t)mesh.nbVerts) {
                c += glm::vec3(mesh.verts[v * 3], mesh.verts[v * 3 + 1], mesh.verts[v * 3 + 2]);
            }
        }
        faceCenters[f] = c / (float)numV;

        uint32_t v0 = mesh.faces[id];
        uint32_t v1 = mesh.faces[id + 1];
        uint32_t v2 = mesh.faces[id + 2];
        if (v0 < (uint32_t)mesh.nbVerts && v1 < (uint32_t)mesh.nbVerts && v2 < (uint32_t)mesh.nbVerts) {
            glm::vec3 p0(mesh.verts[v0 * 3], mesh.verts[v0 * 3 + 1], mesh.verts[v0 * 3 + 2]);
            glm::vec3 p1(mesh.verts[v1 * 3], mesh.verts[v1 * 3 + 1], mesh.verts[v1 * 3 + 2]);
            glm::vec3 p2(mesh.verts[v2 * 3], mesh.verts[v2 * 3 + 1], mesh.verts[v2 * 3 + 2]);
            glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
            if (numV == 4) {
                uint32_t v3 = mesh.faces[id + 3];
                if (v3 < (uint32_t)mesh.nbVerts) {
                    glm::vec3 p3(mesh.verts[v3 * 3], mesh.verts[v3 * 3 + 1], mesh.verts[v3 * 3 + 2]);
                    fn += glm::cross(p2 - p0, p3 - p0);
                }
            }
            if (glm::length(fn) > 1e-6f) faceNormals[f] = glm::normalize(fn);
        }
    }

    // 5. Collect all affected faces and compute uniform metric bevel distance across them
    std::unordered_set<int> affectedFaces;
    for (uint32_t e : selEdgeSet) {
        if (edgeTop[e].f0 >= 0) affectedFaces.insert(edgeTop[e].f0);
        if (edgeTop[e].f1 >= 0) affectedFaces.insert(edgeTop[e].f1);
    }

    float minEdgeLen = 1e9f;
    float avgEdgeLen = 0.0f;
    int edgeCount = 0;

    for (int f : affectedFaces) {
        int id = f * 4;
        int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
        for (int k = 0; k < numV; ++k) {
            uint32_t va = mesh.faces[id + k];
            uint32_t vb = mesh.faces[id + ((k + 1) % numV)];
            glm::vec3 pa(mesh.verts[va * 3], mesh.verts[va * 3 + 1], mesh.verts[va * 3 + 2]);
            glm::vec3 pb(mesh.verts[vb * 3], mesh.verts[vb * 3 + 1], mesh.verts[vb * 3 + 2]);
            float l = glm::length(pb - pa);
            if (l > 1e-6f) {
                minEdgeLen = std::min(minEdgeLen, l);
                avgEdgeLen += l;
                edgeCount++;
            }
        }
    }
    if (minEdgeLen > 1e8f || minEdgeLen < 1e-6f) minEdgeLen = 1.0f;
    if (edgeCount > 0) avgEdgeLen /= (float)edgeCount;
    else avgEdgeLen = minEdgeLen;

    float refLen = std::min(avgEdgeLen, minEdgeLen * 2.0f);
    float w = std::clamp(width, 0.001f, 0.99f);
    float baseDist = std::clamp(w * refLen * 0.5f, 0.0001f, minEdgeLen * 0.48f);

    uint32_t bevelGid = mesh.getNextFreeGroupID();

    auto createNewVertWithNormal = [&](uint32_t origV, const glm::vec3& newPos, const glm::vec3& newNorm) -> uint32_t {
        uint32_t newV = (uint32_t)mesh.nbVerts++;
        mesh.verts.push_back(newPos.x);
        mesh.verts.push_back(newPos.y);
        mesh.verts.push_back(newPos.z);

        mesh.normals.push_back(newNorm.x);
        mesh.normals.push_back(newNorm.y);
        mesh.normals.push_back(newNorm.z);

        if (mesh.colors.size() >= (size_t)(origV + 1) * 3) {
            mesh.colors.push_back(mesh.colors[origV * 3]);
            mesh.colors.push_back(mesh.colors[origV * 3 + 1]);
            mesh.colors.push_back(mesh.colors[origV * 3 + 2]);
        } else {
            mesh.colors.push_back(1.0f); mesh.colors.push_back(1.0f); mesh.colors.push_back(1.0f);
        }

        if (mesh.materials.size() >= (size_t)(origV + 1) * 3) {
            mesh.materials.push_back(mesh.materials[origV * 3]);
            mesh.materials.push_back(mesh.materials[origV * 3 + 1]);
            mesh.materials.push_back(mesh.materials[origV * 3 + 2]);
        } else {
            mesh.materials.push_back(0.5f); mesh.materials.push_back(0.0f); mesh.materials.push_back(1.0f);
        }
        return newV;
    };

    auto createNewVert = [&](uint32_t origV, const glm::vec3& newPos) -> uint32_t {
        glm::vec3 n(0.0f, 1.0f, 0.0f);
        if (mesh.normals.size() >= (size_t)(origV + 1) * 3) {
            n = glm::vec3(mesh.normals[origV * 3], mesh.normals[origV * 3 + 1], mesh.normals[origV * 3 + 2]);
        }
        return createNewVertWithNormal(origV, newPos, n);
    };

    // Map corner (f, k) -> new displaced vertex index
    // Also map (v, f) -> new displaced vertex index for fast quad building
    std::unordered_map<uint64_t, uint32_t> faceCornerNewV;
    std::unordered_map<uint64_t, uint32_t> vertFaceToNewV;
    std::unordered_map<uint64_t, uint32_t> unbeveledEdgeCornerNewV;

    for (int f : affectedFaces) {
        int id = f * 4;
        int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
        const glm::vec3& nF = faceNormals[f];

        for (int k = 0; k < numV; ++k) {
            uint32_t v = mesh.faces[id + k];
            uint32_t vPrev = mesh.faces[id + ((k - 1 + numV) % numV)];
            uint32_t vNext = mesh.faces[id + ((k + 1) % numV)];

            uint64_t kPrev = makeEdgeKey(vPrev, v);
            uint64_t kNext = makeEdgeKey(v, vNext);

            bool prevBevel = (edgeLookup.count(kPrev) && selEdgeSet.count(edgeLookup[kPrev]));
            bool nextBevel = (edgeLookup.count(kNext) && selEdgeSet.count(edgeLookup[kNext]));

            uint64_t cornerKey = (uint64_t(f) << 32) | uint64_t(k);
            uint64_t vfKey = (uint64_t(v) << 32) | uint64_t((uint32_t)f);

            if (!prevBevel && !nextBevel) {
                faceCornerNewV[cornerKey] = v;
                vertFaceToNewV[vfKey] = v;
                continue;
            }

            glm::vec3 posV(mesh.verts[v * 3], mesh.verts[v * 3 + 1], mesh.verts[v * 3 + 2]);
            glm::vec3 posPrev(mesh.verts[vPrev * 3], mesh.verts[vPrev * 3 + 1], mesh.verts[vPrev * 3 + 2]);
            glm::vec3 posNext(mesh.verts[vNext * 3], mesh.verts[vNext * 3 + 1], mesh.verts[vNext * 3 + 2]);

            glm::vec3 uPrev(0.0f);
            if (prevBevel) {
                glm::vec3 ePrev = posV - posPrev;
                uPrev = glm::cross(nF, ePrev);
                if (glm::length(uPrev) > 1e-6f) uPrev = glm::normalize(uPrev);
            }

            glm::vec3 uNext(0.0f);
            if (nextBevel) {
                glm::vec3 eNext = posNext - posV;
                uNext = glm::cross(nF, eNext);
                if (glm::length(uNext) > 1e-6f) uNext = glm::normalize(uNext);
            }

            glm::vec3 toCenter = faceCenters[f] - posV;
            if (glm::length(toCenter) > 1e-6f) {
                toCenter = glm::normalize(toCenter);
                if (prevBevel && glm::dot(uPrev, toCenter) < -1e-4f) uPrev = -uPrev;
                if (nextBevel && glm::dot(uNext, toCenter) < -1e-4f) uNext = -uNext;
            }

            glm::vec3 shift(0.0f);
            if (prevBevel && nextBevel) {
                glm::vec3 bisector = uPrev + uNext;
                float bisLen = glm::length(bisector);
                if (bisLen > 1e-4f) {
                    bisector /= bisLen;
                    float cosHalf = std::max(0.15f, glm::dot(bisector, uPrev));
                    float shiftLen = std::min(baseDist / cosHalf, baseDist * 2.5f);
                    shift = bisector * shiftLen;
                } else {
                    shift = uPrev * baseDist;
                }
            } else if (nextBevel) {
                shift = uNext * baseDist;
            } else if (prevBevel) {
                shift = uPrev * baseDist;
            }

            glm::vec3 newPos = posV + shift + nF * ((type == BevelType::Solid) ? 0.0f : heightOffset);

            uint32_t newV = 0xffffffff;
            if (prevBevel && nextBevel) {
                newV = createNewVert(v, newPos);
            } else if (prevBevel && !nextBevel) {
                uint64_t unKey = (uint64_t(v) << 32) | uint64_t(vNext);
                auto it = unbeveledEdgeCornerNewV.find(unKey);
                if (it != unbeveledEdgeCornerNewV.end()) {
                    newV = it->second;
                } else {
                    newV = createNewVert(v, newPos);
                    unbeveledEdgeCornerNewV[unKey] = newV;
                }
            } else if (!prevBevel && nextBevel) {
                uint64_t unKey = (uint64_t(v) << 32) | uint64_t(vPrev);
                auto it = unbeveledEdgeCornerNewV.find(unKey);
                if (it != unbeveledEdgeCornerNewV.end()) {
                    newV = it->second;
                } else {
                    newV = createNewVert(v, newPos);
                    unbeveledEdgeCornerNewV[unKey] = newV;
                }
            } else {
                newV = createNewVert(v, newPos);
            }

            faceCornerNewV[cornerKey] = newV;
            vertFaceToNewV[vfKey] = newV;
        }
    }

    std::vector<uint32_t> newBevelFaces;

    if (type == BevelType::Chamfer) {
        // Record whether f0 has va -> vb for each edge before modifying mesh.faces
        std::unordered_map<uint32_t, bool> edgeF0DirVaToVb;
        for (uint32_t e : selEdgeSet) {
            uint32_t va = edgeTop[e].v0;
            uint32_t vb = edgeTop[e].v1;
            int f0 = edgeTop[e].f0;
            int id0 = f0 * 4;
            int numV0 = (mesh.faces[id0 + 3] != 0xffffffff) ? 4 : 3;

            bool dir = false;
            for (int k = 0; k < numV0; ++k) {
                if (mesh.faces[id0 + k] == va && mesh.faces[id0 + ((k + 1) % numV0)] == vb) {
                    dir = true;
                    break;
                }
            }
            edgeF0DirVaToVb[e] = dir;
        }

        // Update affected faces with inset corner vertices
        for (int f : affectedFaces) {
            int id = f * 4;
            int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
            for (int k = 0; k < numV; ++k) {
                uint64_t cornerKey = (uint64_t(f) << 32) | uint64_t(k);
                auto it = faceCornerNewV.find(cornerKey);
                if (it != faceCornerNewV.end()) {
                    mesh.faces[id + k] = it->second;
                }
            }
        }

        // Shared profile caching to ensure adjacent edges dividing the same face pair share exact profile vertices
        struct ProfileKey {
            uint32_t v;
            int f0, f1;
            bool operator==(const ProfileKey& o) const {
                return v == o.v && std::min(f0, f1) == std::min(o.f0, o.f1) && std::max(f0, f1) == std::max(o.f0, o.f1);
            }
        };
        struct ProfileKeyHash {
            size_t operator()(const ProfileKey& k) const {
                size_t h = std::hash<uint32_t>()(k.v);
                int a = std::min(k.f0, k.f1);
                int b = std::max(k.f0, k.f1);
                h ^= std::hash<int>()(a) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<int>()(b) + 0x9e3779b9 + (h << 6) + (h >> 2);
                return h;
            }
        };
        std::unordered_map<ProfileKey, std::vector<uint32_t>, ProfileKeyHash> sharedProfiles;

        // Generate chamfer quad strips (with S segments) along each beveled edge
        std::unordered_map<uint32_t, std::vector<uint32_t>> edgePolyA;
        std::unordered_map<uint32_t, std::vector<uint32_t>> edgePolyB;

        for (uint32_t e : selEdgeSet) {
            const auto& top = edgeTop[e];
            uint32_t va = top.v0;
            uint32_t vb = top.v1;
            int f0 = top.f0;
            int f1 = top.f1;

            uint64_t keyVa0 = (uint64_t(va) << 32) | uint64_t((uint32_t)f0);
            uint64_t keyVb0 = (uint64_t(vb) << 32) | uint64_t((uint32_t)f0);
            uint32_t va0 = vertFaceToNewV.count(keyVa0) ? vertFaceToNewV[keyVa0] : va;
            uint32_t vb0 = vertFaceToNewV.count(keyVb0) ? vertFaceToNewV[keyVb0] : vb;

            uint32_t va1 = va;
            uint32_t vb1 = vb;
            if (f1 >= 0) {
                uint64_t keyVa1 = (uint64_t(va) << 32) | uint64_t((uint32_t)f1);
                uint64_t keyVb1 = (uint64_t(vb) << 32) | uint64_t((uint32_t)f1);
                if (vertFaceToNewV.count(keyVa1)) va1 = vertFaceToNewV[keyVa1];
                if (vertFaceToNewV.count(keyVb1)) vb1 = vertFaceToNewV[keyVb1];
            }

            std::vector<uint32_t> polyA(S + 1);
            std::vector<uint32_t> polyB(S + 1);
            polyA[0] = va0; polyA[S] = va1;
            polyB[0] = vb0; polyB[S] = vb1;

            ProfileKey keyA{va, f0, f1};
            auto itA = sharedProfiles.find(keyA);
            if (itA != sharedProfiles.end()) {
                if (itA->first.f0 == f0) {
                    polyA = itA->second;
                } else {
                    polyA = itA->second;
                    std::reverse(polyA.begin(), polyA.end());
                }
            } else {
                if (S > 1) {
                    glm::vec3 pa0(mesh.verts[va0 * 3], mesh.verts[va0 * 3 + 1], mesh.verts[va0 * 3 + 2]);
                    glm::vec3 pa1(mesh.verts[va1 * 3], mesh.verts[va1 * 3 + 1], mesh.verts[va1 * 3 + 2]);
                    glm::vec3 origPa(mesh.verts[va * 3], mesh.verts[va * 3 + 1], mesh.verts[va * 3 + 2]);
                    glm::vec3 na0 = faceNormals[f0];
                    glm::vec3 na1 = (f1 >= 0) ? faceNormals[f1] : na0;

                    for (int s = 1; s < S; ++s) {
                        float frac = (float)s / (float)S;
                        float w0 = (1.0f - frac) * (1.0f - frac);
                        float wCtrl = 2.0f * (1.0f - frac) * frac;
                        float w1 = frac * frac;

                        glm::vec3 posA = w0 * pa0 + wCtrl * origPa + w1 * pa1;
                        glm::vec3 normA = glm::normalize((1.0f - frac) * na0 + frac * na1);
                        polyA[s] = createNewVertWithNormal(va, posA, normA);
                    }
                }
                sharedProfiles[keyA] = polyA;
            }

            ProfileKey keyB{vb, f0, f1};
            auto itB = sharedProfiles.find(keyB);
            if (itB != sharedProfiles.end()) {
                if (itB->first.f0 == f0) {
                    polyB = itB->second;
                } else {
                    polyB = itB->second;
                    std::reverse(polyB.begin(), polyB.end());
                }
            } else {
                if (S > 1) {
                    glm::vec3 pb0(mesh.verts[vb0 * 3], mesh.verts[vb0 * 3 + 1], mesh.verts[vb0 * 3 + 2]);
                    glm::vec3 pb1(mesh.verts[vb1 * 3], mesh.verts[vb1 * 3 + 1], mesh.verts[vb1 * 3 + 2]);
                    glm::vec3 origPb(mesh.verts[vb * 3], mesh.verts[vb * 3 + 1], mesh.verts[vb * 3 + 2]);
                    glm::vec3 na0 = faceNormals[f0];
                    glm::vec3 na1 = (f1 >= 0) ? faceNormals[f1] : na0;

                    for (int s = 1; s < S; ++s) {
                        float frac = (float)s / (float)S;
                        float w0 = (1.0f - frac) * (1.0f - frac);
                        float wCtrl = 2.0f * (1.0f - frac) * frac;
                        float w1 = frac * frac;

                        glm::vec3 posB = w0 * pb0 + wCtrl * origPb + w1 * pb1;
                        glm::vec3 normB = glm::normalize((1.0f - frac) * na0 + frac * na1);
                        polyB[s] = createNewVertWithNormal(vb, posB, normB);
                    }
                }
                sharedProfiles[keyB] = polyB;
            }

            edgePolyA[e] = polyA;
            edgePolyB[e] = polyB;

            bool f0HasVaToVb = edgeF0DirVaToVb[e];

            for (int s = 0; s < S; ++s) {
                uint32_t a0 = polyA[s];
                uint32_t a1 = polyA[s + 1];
                uint32_t b0 = polyB[s];
                uint32_t b1 = polyB[s + 1];

                uint32_t q0, q1, q2, q3;
                if (f0HasVaToVb) {
                    q0 = b0; q1 = a0; q2 = a1; q3 = b1;
                } else {
                    q0 = a0; q1 = b0; q2 = b1; q3 = a1;
                }

                mesh.faces.push_back(q0);
                mesh.faces.push_back(q1);
                mesh.faces.push_back(q2);
                mesh.faces.push_back(q3);
                mesh.faceGroups.push_back(bevelGid);
                newBevelFaces.push_back((uint32_t)mesh.nbFaces++);
            }
        }

        // Generate End-Caps (k == 1) and Watertight Corner Caps (k >= 2) with full multi-segment welding
        for (const auto& kv : vertToSelEdges) {
            uint32_t v = kv.first;
            const auto& incEdges = kv.second;
            int k = (int)incEdges.size();

            glm::vec3 origNorm(0.0f, 1.0f, 0.0f);
            if (mesh.normals.size() >= (size_t)(v + 1) * 3) {
                origNorm = glm::vec3(mesh.normals[v * 3], mesh.normals[v * 3 + 1], mesh.normals[v * 3 + 2]);
            }
            if (glm::length(origNorm) < 1e-4f) origNorm = glm::vec3(0.0f, 1.0f, 0.0f);
            else origNorm = glm::normalize(origNorm);

            if (k == 1) {
                uint32_t e = incEdges[0];
                const auto& top = edgeTop[e];
                int f0 = top.f0;
                int f1 = top.f1;
                if (f1 < 0) continue;

                const auto& poly = (v == top.v0) ? edgePolyA[e] : edgePolyB[e];
                if (poly.size() < 2) continue;

                glm::vec3 outN = origNorm;
                if (glm::length(faceNormals[f0] + faceNormals[f1]) > 1e-4f) {
                    outN = glm::normalize(faceNormals[f0] + faceNormals[f1]);
                }

                for (int s = 0; s < S; ++s) {
                    uint32_t v0 = poly[s];
                    uint32_t v1 = poly[s + 1];
                    if (v0 == v || v1 == v || v0 == v1) continue;

                    glm::vec3 p0(mesh.verts[v0 * 3], mesh.verts[v0 * 3 + 1], mesh.verts[v0 * 3 + 2]);
                    glm::vec3 p1(mesh.verts[v1 * 3], mesh.verts[v1 * 3 + 1], mesh.verts[v1 * 3 + 2]);
                    glm::vec3 pv(mesh.verts[v * 3], mesh.verts[v * 3 + 1], mesh.verts[v * 3 + 2]);
                    glm::vec3 triN = glm::cross(p1 - p0, pv - p0);

                    if (glm::dot(triN, outN) < 0.0f) {
                        std::swap(v0, v1);
                    }

                    mesh.faces.push_back(v0);
                    mesh.faces.push_back(v1);
                    mesh.faces.push_back(v);
                    mesh.faces.push_back(0xffffffff);
                    mesh.faceGroups.push_back(bevelGid);
                    mesh.nbFaces++;
                }
            } else if (k == 2) {
                uint32_t eA = incEdges[0];
                uint32_t eB = incEdges[1];
                const auto& topA = edgeTop[eA];
                const auto& topB = edgeTop[eB];

                const auto& polyA_raw = (v == topA.v0) ? edgePolyA[eA] : edgePolyB[eA];
                const auto& polyB_raw = (v == topB.v0) ? edgePolyA[eB] : edgePolyB[eB];

                int fA0 = topA.f0, fA1 = topA.f1;
                int fB0 = topB.f0, fB1 = topB.f1;

                bool bothShared = (std::min(fA0, fA1) == std::min(fB0, fB1) && std::max(fA0, fA1) == std::max(fB0, fB1));
                if (bothShared) {
                    // Continuous edge division across the same face pair, sharedProfiles already welded them seamlessly
                    continue;
                }

                int fShared = -1;
                if (fA0 >= 0 && (fA0 == fB0 || fA0 == fB1)) fShared = fA0;
                else if (fA1 >= 0 && (fA1 == fB0 || fA1 == fB1)) fShared = fA1;

                if (fShared >= 0) {
                    std::vector<uint32_t> profA = polyA_raw;
                    if (fShared == fA1) {
                        std::reverse(profA.begin(), profA.end());
                    }

                    std::vector<uint32_t> profB = polyB_raw;
                    if (fShared == fB1) {
                        std::reverse(profB.begin(), profB.end());
                    }

                    // 1. Apex triangle at s = 0: [profA[0], profA[1], profB[1]]
                    uint32_t vApex = profA[0];
                    uint32_t a1 = profA[1];
                    uint32_t b1 = profB[1];
                    if (a1 != b1 && a1 != vApex && b1 != vApex) {
                        glm::vec3 pApex(mesh.verts[vApex * 3], mesh.verts[vApex * 3 + 1], mesh.verts[vApex * 3 + 2]);
                        glm::vec3 pa1(mesh.verts[a1 * 3], mesh.verts[a1 * 3 + 1], mesh.verts[a1 * 3 + 2]);
                        glm::vec3 pb1(mesh.verts[b1 * 3], mesh.verts[b1 * 3 + 1], mesh.verts[b1 * 3 + 2]);
                        glm::vec3 triN = glm::cross(pa1 - pApex, pb1 - pApex);
                        if (glm::dot(triN, origNorm) < 0.0f) std::swap(a1, b1);

                        mesh.faces.push_back(vApex);
                        mesh.faces.push_back(a1);
                        mesh.faces.push_back(b1);
                        mesh.faces.push_back(0xffffffff);
                        mesh.faceGroups.push_back(bevelGid);
                        mesh.nbFaces++;
                    }

                    // 2. Turning Quads for s = 1..S-1: [profA[s], profA[s+1], profB[s+1], profB[s]]
                    for (int s = 1; s < S; ++s) {
                        uint32_t as0 = profA[s];
                        uint32_t as1 = profA[s + 1];
                        uint32_t bs0 = profB[s];
                        uint32_t bs1 = profB[s + 1];
                        if (as0 == bs0 && as1 == bs1) continue;

                        glm::vec3 p0(mesh.verts[as0 * 3], mesh.verts[as0 * 3 + 1], mesh.verts[as0 * 3 + 2]);
                        glm::vec3 p1(mesh.verts[as1 * 3], mesh.verts[as1 * 3 + 1], mesh.verts[as1 * 3 + 2]);
                        glm::vec3 p2(mesh.verts[bs1 * 3], mesh.verts[bs1 * 3 + 1], mesh.verts[bs1 * 3 + 2]);
                        glm::vec3 quadN = glm::cross(p1 - p0, p2 - p0);

                        uint32_t q0 = as0, q1 = as1, q2 = bs1, q3 = bs0;
                        if (glm::dot(quadN, origNorm) < 0.0f) {
                            std::swap(q1, q3);
                        }

                        if (q1 == q2) {
                            mesh.faces.push_back(q0);
                            mesh.faces.push_back(q1);
                            mesh.faces.push_back(q3);
                            mesh.faces.push_back(0xffffffff);
                        } else if (q0 == q3) {
                            mesh.faces.push_back(q0);
                            mesh.faces.push_back(q1);
                            mesh.faces.push_back(q2);
                            mesh.faces.push_back(0xffffffff);
                        } else {
                            mesh.faces.push_back(q0);
                            mesh.faces.push_back(q1);
                            mesh.faces.push_back(q2);
                            mesh.faces.push_back(q3);
                        }
                        mesh.faceGroups.push_back(bevelGid);
                        mesh.nbFaces++;
                    }

                    // 3. Bottom triangle connecting profA[S], profB[S] and original vertex v
                    uint32_t endA = profA[S];
                    uint32_t endB = profB[S];
                    if (endA != endB && endA != v && endB != v) {
                        glm::vec3 pa(mesh.verts[endA * 3], mesh.verts[endA * 3 + 1], mesh.verts[endA * 3 + 2]);
                        glm::vec3 pb(mesh.verts[endB * 3], mesh.verts[endB * 3 + 1], mesh.verts[endB * 3 + 2]);
                        glm::vec3 pv(mesh.verts[v * 3], mesh.verts[v * 3 + 1], mesh.verts[v * 3 + 2]);
                        glm::vec3 triN = glm::cross(pb - pa, pv - pa);
                        uint32_t t0 = endA, t1 = endB, t2 = v;
                        if (glm::dot(triN, origNorm) < 0.0f) std::swap(t0, t1);

                        mesh.faces.push_back(t0);
                        mesh.faces.push_back(t1);
                        mesh.faces.push_back(t2);
                        mesh.faces.push_back(0xffffffff);
                        mesh.faceGroups.push_back(bevelGid);
                        mesh.nbFaces++;
                    }
                }
            } else if (k >= 3) {
                // Form the continuous cyclic loop of all profile segments around corner vertex v
                std::vector<uint32_t> loopVerts;
                std::vector<bool> edgeUsed(incEdges.size(), false);

                const auto& firstTop = edgeTop[incEdges[0]];
                const auto& firstPoly = (v == firstTop.v0) ? edgePolyA[incEdges[0]] : edgePolyB[incEdges[0]];
                int currFace = firstTop.f0;
                int nextFace = firstTop.f1;
                edgeUsed[0] = true;

                for (int s = 0; s < S; ++s) {
                    loopVerts.push_back(firstPoly[s]);
                }
                currFace = nextFace;

                for (size_t step = 1; step < incEdges.size(); ++step) {
                    int nextEdgeIdx = -1;
                    bool forward = true;
                    for (size_t j = 0; j < incEdges.size(); ++j) {
                        if (edgeUsed[j]) continue;
                        const auto& topJ = edgeTop[incEdges[j]];
                        if (topJ.f0 == currFace) {
                            nextEdgeIdx = (int)j;
                            forward = true;
                            break;
                        } else if (topJ.f1 == currFace) {
                            nextEdgeIdx = (int)j;
                            forward = false;
                            break;
                        }
                    }

                    if (nextEdgeIdx < 0) {
                        break;
                    }

                    edgeUsed[nextEdgeIdx] = true;
                    const auto& topJ = edgeTop[incEdges[nextEdgeIdx]];
                    const auto& polyJ = (v == topJ.v0) ? edgePolyA[incEdges[nextEdgeIdx]] : edgePolyB[incEdges[nextEdgeIdx]];

                    if (forward) {
                        for (int s = 0; s < S; ++s) {
                            loopVerts.push_back(polyJ[s]);
                        }
                        currFace = topJ.f1;
                    } else {
                        for (int s = S; s > 0; --s) {
                            loopVerts.push_back(polyJ[s]);
                        }
                        currFace = topJ.f0;
                    }
                }

                // Clean duplicate consecutive vertices if any
                std::vector<uint32_t> cleanLoop;
                for (uint32_t lv : loopVerts) {
                    if (cleanLoop.empty() || cleanLoop.back() != lv) {
                        cleanLoop.push_back(lv);
                    }
                }
                if (cleanLoop.size() > 1 && cleanLoop.front() == cleanLoop.back()) {
                    cleanLoop.pop_back();
                }

                // Fallback for open or non-manifold topology
                if (cleanLoop.size() < 3) {
                    cleanLoop.clear();
                    for (uint32_t e : incEdges) {
                        const auto& poly = (v == edgeTop[e].v0) ? edgePolyA[e] : edgePolyB[e];
                        for (uint32_t pv : poly) {
                            if (std::find(cleanLoop.begin(), cleanLoop.end(), pv) == cleanLoop.end()) {
                                cleanLoop.push_back(pv);
                            }
                        }
                    }
                    if (cleanLoop.size() >= 3) {
                        glm::vec3 tangent = glm::vec3(1, 0, 0);
                        if (std::abs(glm::dot(origNorm, tangent)) > 0.9f) tangent = glm::vec3(0, 1, 0);
                        tangent = glm::normalize(tangent - origNorm * glm::dot(origNorm, tangent));
                        glm::vec3 bitangent = glm::cross(origNorm, tangent);
                        glm::vec3 pv(mesh.verts[v * 3], mesh.verts[v * 3 + 1], mesh.verts[v * 3 + 2]);

                        std::sort(cleanLoop.begin(), cleanLoop.end(), [&](uint32_t a, uint32_t b) {
                            glm::vec3 pa(mesh.verts[a * 3] - pv.x, mesh.verts[a * 3 + 1] - pv.y, mesh.verts[a * 3 + 2] - pv.z);
                            glm::vec3 pb(mesh.verts[b * 3] - pv.x, mesh.verts[b * 3 + 1] - pv.y, mesh.verts[b * 3 + 2] - pv.z);
                            float angA = std::atan2(glm::dot(pa, bitangent), glm::dot(pa, tangent));
                            float angB = std::atan2(glm::dot(pb, bitangent), glm::dot(pb, tangent));
                            return angA < angB;
                        });
                    }
                }

                size_t N = cleanLoop.size();
                if (N == 3) {
                    uint32_t a = cleanLoop[0];
                    uint32_t b = cleanLoop[1];
                    uint32_t c = cleanLoop[2];
                    glm::vec3 pa(mesh.verts[a * 3], mesh.verts[a * 3 + 1], mesh.verts[a * 3 + 2]);
                    glm::vec3 pb(mesh.verts[b * 3], mesh.verts[b * 3 + 1], mesh.verts[b * 3 + 2]);
                    glm::vec3 pc(mesh.verts[c * 3], mesh.verts[c * 3 + 1], mesh.verts[c * 3 + 2]);
                    glm::vec3 triN = glm::cross(pb - pa, pc - pa);
                    if (glm::dot(triN, origNorm) < 0.0f) std::swap(b, c);

                    mesh.faces.push_back(a);
                    mesh.faces.push_back(b);
                    mesh.faces.push_back(c);
                    mesh.faces.push_back(0xffffffff);
                    mesh.faceGroups.push_back(bevelGid);
                    mesh.nbFaces++;
                } else if (N == 4) {
                    uint32_t a = cleanLoop[0];
                    uint32_t b = cleanLoop[1];
                    uint32_t c = cleanLoop[2];
                    uint32_t d = cleanLoop[3];
                    glm::vec3 pa(mesh.verts[a * 3], mesh.verts[a * 3 + 1], mesh.verts[a * 3 + 2]);
                    glm::vec3 pb(mesh.verts[b * 3], mesh.verts[b * 3 + 1], mesh.verts[b * 3 + 2]);
                    glm::vec3 pc(mesh.verts[c * 3], mesh.verts[c * 3 + 1], mesh.verts[c * 3 + 2]);
                    glm::vec3 quadN = glm::cross(pb - pa, pc - pa);
                    if (glm::dot(quadN, origNorm) < 0.0f) {
                        std::swap(b, d);
                    }

                    mesh.faces.push_back(a);
                    mesh.faces.push_back(b);
                    mesh.faces.push_back(c);
                    mesh.faces.push_back(d);
                    mesh.faceGroups.push_back(bevelGid);
                    mesh.nbFaces++;
                } else if (N > 4) {
                    glm::vec3 pAvg(0.0f);
                    for (uint32_t lv : cleanLoop) {
                        pAvg += glm::vec3(mesh.verts[lv * 3], mesh.verts[lv * 3 + 1], mesh.verts[lv * 3 + 2]);
                    }
                    pAvg /= (float)N;
                    glm::vec3 pOrig(mesh.verts[v * 3], mesh.verts[v * 3 + 1], mesh.verts[v * 3 + 2]);
                    glm::vec3 posC = 0.5f * pAvg + 0.5f * pOrig;

                    uint32_t cVert = createNewVertWithNormal(v, posC, origNorm);

                    for (size_t i = 0; i < N; ++i) {
                        uint32_t va = cleanLoop[i];
                        uint32_t vb = cleanLoop[(i + 1) % N];
                        if (va == vb) continue;

                        glm::vec3 pa(mesh.verts[va * 3], mesh.verts[va * 3 + 1], mesh.verts[va * 3 + 2]);
                        glm::vec3 pb(mesh.verts[vb * 3], mesh.verts[vb * 3 + 1], mesh.verts[vb * 3 + 2]);
                        glm::vec3 triN = glm::cross(pa - posC, pb - posC);

                        uint32_t f0 = cVert;
                        uint32_t f1 = va;
                        uint32_t f2 = vb;
                        if (glm::dot(triN, origNorm) < 0.0f) {
                            std::swap(f1, f2);
                        }

                        mesh.faces.push_back(f0);
                        mesh.faces.push_back(f1);
                        mesh.faces.push_back(f2);
                        mesh.faces.push_back(0xffffffff);
                        mesh.faceGroups.push_back(bevelGid);
                        mesh.nbFaces++;
                    }
                }
            }
        }
    } else {
        // Solid Mode: Keep original edges in the center as sharp crests,
        // and insert parallel holding strips of S segments into each affected face.
        for (int f : affectedFaces) {
            int id = f * 4;
            int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;

            std::vector<uint32_t> origVerts(numV);
            std::vector<uint32_t> innerVerts(numV);
            for (int k = 0; k < numV; ++k) {
                uint32_t v = mesh.faces[id + k];
                origVerts[k] = v;
                uint64_t vfKey = (uint64_t(v) << 32) | uint64_t((uint32_t)f);
                innerVerts[k] = vertFaceToNewV.count(vfKey) ? vertFaceToNewV[vfKey] : v;
            }

            // Inset the interior of face f to innerVerts
            for (int k = 0; k < numV; ++k) {
                mesh.faces[id + k] = innerVerts[k];
            }

            // Pre-generate corner profile vertices along each radial ray (origVerts[k] -> innerVerts[k])
            // so adjacent bevel strips share the exact same profile vertices at common corners.
            std::vector<std::vector<uint32_t>> cornerProfiles(numV, std::vector<uint32_t>(S + 1));
            for (int k = 0; k < numV; ++k) {
                uint32_t va = origVerts[k];
                uint32_t va_in = innerVerts[k];
                cornerProfiles[k][0] = va;
                cornerProfiles[k][S] = va_in;
                if (S > 1) {
                    if (va == va_in) {
                        for (int s = 1; s < S; ++s) {
                            cornerProfiles[k][s] = va;
                        }
                    } else {
                        glm::vec3 pa0(mesh.verts[va * 3], mesh.verts[va * 3 + 1], mesh.verts[va * 3 + 2]);
                        glm::vec3 pa1(mesh.verts[va_in * 3], mesh.verts[va_in * 3 + 1], mesh.verts[va_in * 3 + 2]);
                        for (int s = 1; s < S; ++s) {
                            float t = (float)s / (float)S;
                            glm::vec3 posA = (1.0f - t) * pa0 + t * pa1;
                            cornerProfiles[k][s] = createNewVert(va, posA);
                        }
                    }
                }
            }

            // Create supporting rim quads ONLY for selected bevel edges on face f.
            // Non-beveled edges must not generate rim faces: their subdivision is already
            // naturally formed by the inner face and adjacent bevel quad boundaries.
            for (int k = 0; k < numV; ++k) {
                int kn = (k + 1) % numV;
                uint32_t va = origVerts[k];
                uint32_t vb = origVerts[kn];

                uint64_t edgeKey = makeEdgeKey(va, vb);
                bool isBevelEdge = (edgeLookup.count(edgeKey) && selEdgeSet.count(edgeLookup[edgeKey]));
                if (!isBevelEdge) {
                    uint32_t va_in = innerVerts[k];
                    uint32_t vb_in = innerVerts[kn];
                    if (va == va_in && vb == vb_in) {
                        continue;
                    }

                    if (S == 1) {
                        if (va == va_in) {
                            mesh.faces.push_back(va);
                            mesh.faces.push_back(vb);
                            mesh.faces.push_back(vb_in);
                            mesh.faces.push_back(0xffffffff);
                            mesh.faceGroups.push_back(bevelGid);
                            newBevelFaces.push_back((uint32_t)mesh.nbFaces++);
                        } else if (vb == vb_in) {
                            mesh.faces.push_back(va);
                            mesh.faces.push_back(vb);
                            mesh.faces.push_back(va_in);
                            mesh.faces.push_back(0xffffffff);
                            mesh.faceGroups.push_back(bevelGid);
                            newBevelFaces.push_back((uint32_t)mesh.nbFaces++);
                        } else {
                            mesh.faces.push_back(va);
                            mesh.faces.push_back(vb);
                            mesh.faces.push_back(vb_in);
                            mesh.faces.push_back(va_in);
                            mesh.faceGroups.push_back(bevelGid);
                            newBevelFaces.push_back((uint32_t)mesh.nbFaces++);
                        }
                    } else {
                        const auto& rA = cornerProfiles[k];
                        const auto& rB = cornerProfiles[kn];
                        for (int s = 0; s < S; ++s) {
                            uint32_t q0 = rA[s];
                            uint32_t q1 = rB[s];
                            uint32_t q2 = rB[s + 1];
                            uint32_t q3 = rA[s + 1];

                            if (q0 == q3 && q1 == q2) continue;
                            if (q0 == q3) {
                                mesh.faces.push_back(q0);
                                mesh.faces.push_back(q1);
                                mesh.faces.push_back(q2);
                                mesh.faces.push_back(0xffffffff);
                            } else if (q1 == q2) {
                                mesh.faces.push_back(q0);
                                mesh.faces.push_back(q1);
                                mesh.faces.push_back(q3);
                                mesh.faces.push_back(0xffffffff);
                            } else {
                                mesh.faces.push_back(q0);
                                mesh.faces.push_back(q1);
                                mesh.faces.push_back(q2);
                                mesh.faces.push_back(q3);
                            }
                            mesh.faceGroups.push_back(bevelGid);
                            newBevelFaces.push_back((uint32_t)mesh.nbFaces++);
                        }
                    }
                    continue;
                }

                const auto& rA = cornerProfiles[k];
                const auto& rB = cornerProfiles[kn];

                for (int s = 0; s < S; ++s) {
                    uint32_t q0 = rA[s];
                    uint32_t q1 = rB[s];
                    uint32_t q2 = rB[s + 1];
                    uint32_t q3 = rA[s + 1];

                    if (q0 == q3 && q1 == q2) continue;
                    if (q0 == q3) {
                        mesh.faces.push_back(q0);
                        mesh.faces.push_back(q1);
                        mesh.faces.push_back(q2);
                        mesh.faces.push_back(0xffffffff);
                    } else if (q1 == q2) {
                        mesh.faces.push_back(q0);
                        mesh.faces.push_back(q1);
                        mesh.faces.push_back(q3);
                        mesh.faces.push_back(0xffffffff);
                    } else {
                        mesh.faces.push_back(q0);
                        mesh.faces.push_back(q1);
                        mesh.faces.push_back(q2);
                        mesh.faces.push_back(q3);
                    }
                    mesh.faceGroups.push_back(bevelGid);
                    newBevelFaces.push_back((uint32_t)mesh.nbFaces++);
                }
            }
        }
    }

    mesh.vrfStartCount.clear();
    mesh.vertRingFace.clear();
    mesh.vrvStartCount.clear();
    mesh.vertRingVert.clear();
    mesh.faceEdges.clear();
    mesh.edges.clear();
    mesh.edgeCreases.clear();
    mesh.vertVisible.assign(mesh.nbVerts, 1);
    mesh.faceVisible.assign(mesh.nbFaces, 1);
    mesh.isTopologyDirty = true;
    mesh.hasCustomNormals = false;

    mesh.postInit(true);
    mesh.invalidateDynamicSubdiv();
    mesh.isDirty = true;
    mesh.isTopologyDirty = true;

    selMgr.clear();
    selMgr.setMode(ComponentMode::Face);
    for (uint32_t f : newBevelFaces) {
        selMgr.selectFace(f, true);
    }
    selMgr.invalidateTopologyCache();
    return true;
}

// -------------------------------------------------------------
// BRIDGE TOOLS
// -------------------------------------------------------------

bool ComponentOps::bridgeSelection(Mesh& mesh, SelectionManager& selMgr) {
    if (selMgr.getMode() == ComponentMode::Face || selMgr.getSelectedFaceCount() == 2) {
        return bridgeFaces(mesh, selMgr);
    } else if (selMgr.getMode() == ComponentMode::Edge || selMgr.getSelectedEdgeCount() >= 2) {
        return bridgeEdges(mesh, selMgr);
    }
    return false;
}

bool ComponentOps::bridgeFaces(Mesh& mesh, SelectionManager& selMgr) {
    const auto& selFaces = selMgr.getSelectedFaces();
    if (selFaces.size() != 2) return false;

    auto it = selFaces.begin();
    uint32_t fA = *it++;
    uint32_t fB = *it;

    if (fA >= (uint32_t)mesh.nbFaces || fB >= (uint32_t)mesh.nbFaces) return false;

    int idA = fA * 4;
    int idB = fB * 4;
    int numVA = (mesh.faces[idA + 3] != 0xffffffff) ? 4 : 3;
    int numVB = (mesh.faces[idB + 3] != 0xffffffff) ? 4 : 3;

    if (numVA != numVB) return false;
    int N = numVA;

    std::vector<uint32_t> vA(N);
    std::vector<uint32_t> vB(N);
    for (int k = 0; k < N; ++k) {
        vA[k] = mesh.faces[idA + k];
        vB[k] = mesh.faces[idB + k];
    }

    // Find best cyclic offset s and direction to minimize bridge twist
    int bestS = 0;
    bool bestReverse = true;
    float bestDistSq = std::numeric_limits<float>::max();

    for (int s = 0; s < N; ++s) {
        // Test reversed: (s - k + N) % N
        float distSqRev = 0.0f;
        for (int k = 0; k < N; ++k) {
            uint32_t a = vA[k];
            uint32_t b = vB[(s - k + N) % N];
            glm::vec3 pa(mesh.verts[a * 3], mesh.verts[a * 3 + 1], mesh.verts[a * 3 + 2]);
            glm::vec3 pb(mesh.verts[b * 3], mesh.verts[b * 3 + 1], mesh.verts[b * 3 + 2]);
            distSqRev += glm::dot(pa - pb, pa - pb);
        }
        if (distSqRev < bestDistSq) {
            bestDistSq = distSqRev;
            bestS = s;
            bestReverse = true;
        }

        // Test forward: (s + k) % N
        float distSqFwd = 0.0f;
        for (int k = 0; k < N; ++k) {
            uint32_t a = vA[k];
            uint32_t b = vB[(s + k) % N];
            glm::vec3 pa(mesh.verts[a * 3], mesh.verts[a * 3 + 1], mesh.verts[a * 3 + 2]);
            glm::vec3 pb(mesh.verts[b * 3], mesh.verts[b * 3 + 1], mesh.verts[b * 3 + 2]);
            distSqFwd += glm::dot(pa - pb, pa - pb);
        }
        if (distSqFwd < bestDistSq) {
            bestDistSq = distSqFwd;
            bestS = s;
            bestReverse = false;
        }
    }

    glm::vec3 centerA(0.0f);
    glm::vec3 centerB(0.0f);
    for (int k = 0; k < N; ++k) {
        centerA += glm::vec3(mesh.verts[vA[k] * 3], mesh.verts[vA[k] * 3 + 1], mesh.verts[vA[k] * 3 + 2]);
        centerB += glm::vec3(mesh.verts[vB[k] * 3], mesh.verts[vB[k] * 3 + 1], mesh.verts[vB[k] * 3 + 2]);
    }
    centerA /= (float)N;
    centerB /= (float)N;
    glm::vec3 centerMid = (centerA + centerB) * 0.5f;

    uint32_t bridgeGid = mesh.getNextFreeGroupID();
    std::vector<std::vector<uint32_t>> bridgeQuads;

    for (int k = 0; k < N; ++k) {
        int kNext = (k + 1) % N;
        uint32_t a0 = vA[k];
        uint32_t a1 = vA[kNext];
        uint32_t b0 = bestReverse ? vB[(bestS - k + N) % N] : vB[(bestS + k) % N];
        uint32_t b1 = bestReverse ? vB[(bestS - kNext + N) % N] : vB[(bestS + kNext) % N];

        glm::vec3 pa0(mesh.verts[a0 * 3], mesh.verts[a0 * 3 + 1], mesh.verts[a0 * 3 + 2]);
        glm::vec3 pa1(mesh.verts[a1 * 3], mesh.verts[a1 * 3 + 1], mesh.verts[a1 * 3 + 2]);
        glm::vec3 pb0(mesh.verts[b0 * 3], mesh.verts[b0 * 3 + 1], mesh.verts[b0 * 3 + 2]);
        glm::vec3 pb1(mesh.verts[b1 * 3], mesh.verts[b1 * 3 + 1], mesh.verts[b1 * 3 + 2]);

        // Quad normal
        glm::vec3 nQuad = glm::cross(pa1 - pa0, pb1 - pa0) + glm::cross(pb1 - pa0, pb0 - pa0);

        // Outward radial direction from tunnel centerline to quad center
        glm::vec3 quadCenter = (pa0 + pa1 + pb0 + pb1) * 0.25f;
        glm::vec3 outwardDir = quadCenter - centerMid;

        if (glm::length(outwardDir) < 1e-4f) {
            outwardDir = glm::vec3(0.0f);
            for (uint32_t v : {a0, a1, b0, b1}) {
                if ((size_t)v * 3 + 2 < mesh.normals.size()) {
                    outwardDir += glm::vec3(mesh.normals[v * 3], mesh.normals[v * 3 + 1], mesh.normals[v * 3 + 2]);
                }
            }
        }

        if (glm::length(nQuad) > 1e-6f && glm::length(outwardDir) > 1e-4f) {
            if (glm::dot(nQuad, outwardDir) < 0.0f) {
                // Inverted normal! Reverse quad winding: a0 -> b0 -> b1 -> a1
                bridgeQuads.push_back({a0, b0, b1, a1});
                continue;
            }
        }

        // Quad: a0 -> a1 -> b1 -> b0
        bridgeQuads.push_back({a0, a1, b1, b0});
    }

    // Replace fA with bridgeQuads[0]
    mesh.faces[idA]     = bridgeQuads[0][0];
    mesh.faces[idA + 1] = bridgeQuads[0][1];
    mesh.faces[idA + 2] = bridgeQuads[0][2];
    mesh.faces[idA + 3] = bridgeQuads[0][3];
    mesh.faceGroups[fA] = bridgeGid;

    // Replace fB with bridgeQuads[1]
    mesh.faces[idB]     = bridgeQuads[1][0];
    mesh.faces[idB + 1] = bridgeQuads[1][1];
    mesh.faces[idB + 2] = bridgeQuads[1][2];
    mesh.faces[idB + 3] = bridgeQuads[1][3];
    mesh.faceGroups[fB] = bridgeGid;

    std::unordered_set<uint32_t> newSelectedFaces = {fA, fB};

    // Append remaining quads (indices 2 to N-1)
    for (size_t i = 2; i < bridgeQuads.size(); ++i) {
        uint32_t newF = (uint32_t)mesh.nbFaces++;
        mesh.faces.push_back(bridgeQuads[i][0]);
        mesh.faces.push_back(bridgeQuads[i][1]);
        mesh.faces.push_back(bridgeQuads[i][2]);
        mesh.faces.push_back(bridgeQuads[i][3]);
        mesh.faceGroups.push_back(bridgeGid);
        newSelectedFaces.insert(newF);
    }

    mesh.vrfStartCount.clear();
    mesh.vertRingFace.clear();
    mesh.vrvStartCount.clear();
    mesh.vertRingVert.clear();
    mesh.faceEdges.clear();
    mesh.edges.clear();
    mesh.edgeCreases.clear();
    mesh.vertVisible.assign(mesh.nbVerts, 1);
    mesh.faceVisible.assign(mesh.nbFaces, 1);
    mesh.isTopologyDirty = true;
    mesh.hasCustomNormals = false;

    mesh.postInit(true);
    mesh.invalidateDynamicSubdiv();
    mesh.isDirty = true;
    mesh.isTopologyDirty = true;

    selMgr.clear();
    selMgr.setMode(ComponentMode::Face);
    for (uint32_t f : newSelectedFaces) {
        selMgr.selectFace(f, true);
    }
    selMgr.invalidateTopologyCache();
    return true;
}

bool ComponentOps::bridgeEdges(Mesh& mesh, SelectionManager& selMgr) {
    const auto& selEdges = selMgr.getSelectedEdges();
    if (selEdges.size() < 2) return false;

    selMgr.ensureEdgeTopology(mesh);
    const auto& edgeTop = selMgr.getEdgeTopology(mesh);

    std::vector<uint32_t> edgesList(selEdges.begin(), selEdges.end());
    uint32_t bridgeGid = mesh.getNextFreeGroupID();
    std::unordered_set<uint32_t> newSelectedFaces;

    std::vector<std::pair<uint32_t, uint32_t>> edgePairs;
    if (edgesList.size() == 2) {
        edgePairs.push_back({edgesList[0], edgesList[1]});
    } else {
        // Greedy minimum distance pairing between edge midpoints
        std::vector<bool> edgePaired(edgesList.size(), false);
        for (size_t i = 0; i < edgesList.size(); ++i) {
            if (edgePaired[i]) continue;
            uint32_t eA = edgesList[i];
            if (eA >= edgeTop.size()) continue;

            glm::vec3 midA = (glm::vec3(mesh.verts[edgeTop[eA].v0 * 3], mesh.verts[edgeTop[eA].v0 * 3 + 1], mesh.verts[edgeTop[eA].v0 * 3 + 2]) +
                              glm::vec3(mesh.verts[edgeTop[eA].v1 * 3], mesh.verts[edgeTop[eA].v1 * 3 + 1], mesh.verts[edgeTop[eA].v1 * 3 + 2])) * 0.5f;

            float bestD = std::numeric_limits<float>::max();
            int bestJ = -1;

            for (size_t j = i + 1; j < edgesList.size(); ++j) {
                if (edgePaired[j]) continue;
                uint32_t eB = edgesList[j];
                if (eB >= edgeTop.size()) continue;

                glm::vec3 midB = (glm::vec3(mesh.verts[edgeTop[eB].v0 * 3], mesh.verts[edgeTop[eB].v0 * 3 + 1], mesh.verts[edgeTop[eB].v0 * 3 + 2]) +
                                  glm::vec3(mesh.verts[edgeTop[eB].v1 * 3], mesh.verts[edgeTop[eB].v1 * 3 + 1], mesh.verts[edgeTop[eB].v1 * 3 + 2])) * 0.5f;

                float d = glm::distance(midA, midB);
                if (d < bestD) {
                    bestD = d;
                    bestJ = (int)j;
                }
            }

            if (bestJ != -1) {
                edgePaired[i] = true;
                edgePaired[bestJ] = true;
                edgePairs.push_back({eA, edgesList[bestJ]});
            }
        }
    }

    for (const auto& pair : edgePairs) {
        uint32_t eA = pair.first;
        uint32_t eB = pair.second;
        if (eA >= edgeTop.size() || eB >= edgeTop.size()) continue;

        uint32_t a0 = edgeTop[eA].v0;
        uint32_t a1 = edgeTop[eA].v1;
        uint32_t b0 = edgeTop[eB].v0;
        uint32_t b1 = edgeTop[eB].v1;

        if (a0 >= (uint32_t)mesh.nbVerts || a1 >= (uint32_t)mesh.nbVerts ||
            b0 >= (uint32_t)mesh.nbVerts || b1 >= (uint32_t)mesh.nbVerts) continue;

        // Determine correct direction for edge A based on incident face f0:
        // For 2-manifold orientation, the bridge quad must traverse edge A in the OPPOSITE direction to f0.
        if (edgeTop[eA].f0 >= 0) {
            int id0 = edgeTop[eA].f0 * 4;
            int numV0 = (mesh.faces[id0 + 3] != 0xffffffff) ? 4 : 3;
            for (int k = 0; k < numV0; ++k) {
                if (mesh.faces[id0 + k] == a0 && mesh.faces[id0 + ((k + 1) % numV0)] == a1) {
                    // f0 uses a0 -> a1. Therefore new quad must use a1 -> a0.
                    std::swap(a0, a1);
                    break;
                }
            }
        } else if (edgeTop[eB].f0 >= 0) {
            // Edge A has no face, but edge B does: orient edge B opposite to its incident face
            int id0 = edgeTop[eB].f0 * 4;
            int numV0 = (mesh.faces[id0 + 3] != 0xffffffff) ? 4 : 3;
            for (int k = 0; k < numV0; ++k) {
                if (mesh.faces[id0 + k] == b0 && mesh.faces[id0 + ((k + 1) % numV0)] == b1) {
                    std::swap(b0, b1);
                    break;
                }
            }
        }

        glm::vec3 pa0(mesh.verts[a0 * 3], mesh.verts[a0 * 3 + 1], mesh.verts[a0 * 3 + 2]);
        glm::vec3 pa1(mesh.verts[a1 * 3], mesh.verts[a1 * 3 + 1], mesh.verts[a1 * 3 + 2]);
        glm::vec3 pb0(mesh.verts[b0 * 3], mesh.verts[b0 * 3 + 1], mesh.verts[b0 * 3 + 2]);
        glm::vec3 pb1(mesh.verts[b1 * 3], mesh.verts[b1 * 3 + 1], mesh.verts[b1 * 3 + 2]);

        // Connect a0 to pb0 and a1 to pb1 minimizing diagonal twist
        float dDirect = glm::distance(pa0, pb0) + glm::distance(pa1, pb1);
        float dCross  = glm::distance(pa0, pb1) + glm::distance(pa1, pb0);

        if (dCross < dDirect) {
            std::swap(b0, b1);
            pb0 = glm::vec3(mesh.verts[b0 * 3], mesh.verts[b0 * 3 + 1], mesh.verts[b0 * 3 + 2]);
            pb1 = glm::vec3(mesh.verts[b1 * 3], mesh.verts[b1 * 3 + 1], mesh.verts[b1 * 3 + 2]);
        }

        // Candidate quad: a0 -> a1 -> b1 -> b0
        glm::vec3 nQuad = glm::cross(pa1 - pa0, pb1 - pa0) + glm::cross(pb1 - pa0, pb0 - pa0);

        // Compute reference outward normal from incident faces and vertex normals
        glm::vec3 nRef(0.0f);
        if (edgeTop[eA].f0 >= 0 && (size_t)edgeTop[eA].f0 * 3 + 2 < mesh.faceNormals.size()) {
            nRef += glm::vec3(mesh.faceNormals[edgeTop[eA].f0 * 3],
                              mesh.faceNormals[edgeTop[eA].f0 * 3 + 1],
                              mesh.faceNormals[edgeTop[eA].f0 * 3 + 2]);
        }
        if (edgeTop[eB].f0 >= 0 && (size_t)edgeTop[eB].f0 * 3 + 2 < mesh.faceNormals.size()) {
            nRef += glm::vec3(mesh.faceNormals[edgeTop[eB].f0 * 3],
                              mesh.faceNormals[edgeTop[eB].f0 * 3 + 1],
                              mesh.faceNormals[edgeTop[eB].f0 * 3 + 2]);
        }
        if (glm::length(nRef) < 1e-4f) {
            for (uint32_t v : {a0, a1, b0, b1}) {
                if ((size_t)v * 3 + 2 < mesh.normals.size()) {
                    nRef += glm::vec3(mesh.normals[v * 3], mesh.normals[v * 3 + 1], mesh.normals[v * 3 + 2]);
                }
            }
        }

        uint32_t q0 = a0, q1 = a1, q2 = b1, q3 = b0;
        if (glm::length(nQuad) > 1e-6f && glm::length(nRef) > 1e-4f && glm::dot(nQuad, nRef) < 0.0f) {
            // Flip quad winding to match outward reference normal
            q0 = a0;
            q1 = b0;
            q2 = b1;
            q3 = a1;
        }

        uint32_t newF = (uint32_t)mesh.nbFaces++;
        mesh.faces.push_back(q0);
        mesh.faces.push_back(q1);
        mesh.faces.push_back(q2);
        mesh.faces.push_back(q3);
        mesh.faceGroups.push_back(bridgeGid);
        newSelectedFaces.insert(newF);
    }

    if (newSelectedFaces.empty()) return false;

    mesh.vrfStartCount.clear();
    mesh.vertRingFace.clear();
    mesh.vrvStartCount.clear();
    mesh.vertRingVert.clear();
    mesh.faceEdges.clear();
    mesh.edges.clear();
    mesh.edgeCreases.clear();
    mesh.vertVisible.assign(mesh.nbVerts, 1);
    mesh.faceVisible.assign(mesh.nbFaces, 1);
    mesh.isTopologyDirty = true;
    mesh.hasCustomNormals = false;

    mesh.postInit(true);
    mesh.invalidateDynamicSubdiv();
    mesh.isDirty = true;
    mesh.isTopologyDirty = true;

    selMgr.clear();
    selMgr.setMode(ComponentMode::Face);
    for (uint32_t f : newSelectedFaces) {
        selMgr.selectFace(f, true);
    }
    selMgr.invalidateTopologyCache();
    return true;
}

// -------------------------------------------------------------
// MERGE / COLLAPSE TOOLS
// -------------------------------------------------------------

bool ComponentOps::mergeToCenter(Mesh& mesh, SelectionManager& selMgr) {
    if (mesh.nbFaces == 0 || mesh.nbVerts == 0) return false;
    auto effVerts = selMgr.getEffectiveVertexIndices(mesh);
    if (effVerts.size() < 2) return false;

    std::unordered_set<uint32_t> selVertSet(effVerts.begin(), effVerts.end());
    uint32_t targetV = effVerts[0];

    glm::vec3 centroid(0.0f);
    glm::vec3 avgNormal(0.0f);
    for (uint32_t v : effVerts) {
        centroid += glm::vec3(mesh.verts[v * 3], mesh.verts[v * 3 + 1], mesh.verts[v * 3 + 2]);
        if (mesh.normals.size() >= (size_t)(v + 1) * 3) {
            avgNormal += glm::vec3(mesh.normals[v * 3], mesh.normals[v * 3 + 1], mesh.normals[v * 3 + 2]);
        }
    }
    centroid /= (float)effVerts.size();
    if (glm::length(avgNormal) > 1e-4f) avgNormal = glm::normalize(avgNormal);

    mesh.verts[targetV * 3]     = centroid.x;
    mesh.verts[targetV * 3 + 1] = centroid.y;
    mesh.verts[targetV * 3 + 2] = centroid.z;

    if (mesh.normals.size() >= (size_t)(targetV + 1) * 3) {
        mesh.normals[targetV * 3]     = avgNormal.x;
        mesh.normals[targetV * 3 + 1] = avgNormal.y;
        mesh.normals[targetV * 3 + 2] = avgNormal.z;
    }

    // Replace all other selected vertices with targetV in all faces
    std::vector<uint32_t> newFaces;
    std::vector<uint32_t> newFaceGroups;

    for (int f = 0; f < mesh.nbFaces; ++f) {
        int id = f * 4;
        int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;

        std::vector<uint32_t> faceV;
        for (int k = 0; k < numV; ++k) {
            uint32_t v = mesh.faces[id + k];
            if (selVertSet.count(v)) {
                v = targetV;
            }
            if (faceV.empty() || faceV.back() != v) {
                faceV.push_back(v);
            }
        }
        if (faceV.size() > 1 && faceV.front() == faceV.back()) {
            faceV.pop_back();
        }

        if (faceV.size() == 3) {
            newFaces.push_back(faceV[0]);
            newFaces.push_back(faceV[1]);
            newFaces.push_back(faceV[2]);
            newFaces.push_back(0xffffffff);
            newFaceGroups.push_back(mesh.faceGroups.size() == (size_t)mesh.nbFaces ? mesh.faceGroups[f] : 0);
        } else if (faceV.size() == 4) {
            newFaces.push_back(faceV[0]);
            newFaces.push_back(faceV[1]);
            newFaces.push_back(faceV[2]);
            newFaces.push_back(faceV[3]);
            newFaceGroups.push_back(mesh.faceGroups.size() == (size_t)mesh.nbFaces ? mesh.faceGroups[f] : 0);
        }
    }

    mesh.faces = std::move(newFaces);
    mesh.faceGroups = std::move(newFaceGroups);
    mesh.nbFaces = (int)mesh.faces.size() / 4;

    // Compact vertices to remove unused vertices
    std::vector<int> vertRemap(mesh.nbVerts, -1);
    int newNbVerts = 0;
    for (int i = 0; i < mesh.nbFaces * 4; ++i) {
        uint32_t v = mesh.faces[i];
        if (v != 0xffffffff && (int)v < mesh.nbVerts && vertRemap[v] == -1) {
            vertRemap[v] = newNbVerts++;
        }
    }

    std::vector<float> newVerts(newNbVerts * 3);
    std::vector<float> newNormals(newNbVerts * 3);
    std::vector<float> newColors(newNbVerts * 3, 1.0f);
    std::vector<float> newMaterials(newNbVerts * 3);

    for (int oldV = 0; oldV < mesh.nbVerts; ++oldV) {
        int nv = vertRemap[oldV];
        if (nv != -1) {
            newVerts[nv * 3]     = mesh.verts[oldV * 3];
            newVerts[nv * 3 + 1] = mesh.verts[oldV * 3 + 1];
            newVerts[nv * 3 + 2] = mesh.verts[oldV * 3 + 2];
            if (mesh.normals.size() == (size_t)mesh.nbVerts * 3) {
                newNormals[nv * 3]     = mesh.normals[oldV * 3];
                newNormals[nv * 3 + 1] = mesh.normals[oldV * 3 + 1];
                newNormals[nv * 3 + 2] = mesh.normals[oldV * 3 + 2];
            }
            if (mesh.colors.size() == (size_t)mesh.nbVerts * 3) {
                newColors[nv * 3]     = mesh.colors[oldV * 3];
                newColors[nv * 3 + 1] = mesh.colors[oldV * 3 + 1];
                newColors[nv * 3 + 2] = mesh.colors[oldV * 3 + 2];
            }
            if (mesh.materials.size() == (size_t)mesh.nbVerts * 3) {
                newMaterials[nv * 3]     = mesh.materials[oldV * 3];
                newMaterials[nv * 3 + 1] = mesh.materials[oldV * 3 + 1];
                newMaterials[nv * 3 + 2] = mesh.materials[oldV * 3 + 2];
            } else {
                newMaterials[nv * 3] = 0.5f; newMaterials[nv * 3 + 1] = 0.0f; newMaterials[nv * 3 + 2] = 1.0f;
            }
        }
    }

    for (size_t i = 0; i < mesh.faces.size(); ++i) {
        if (mesh.faces[i] != 0xffffffff) {
            mesh.faces[i] = (uint32_t)vertRemap[mesh.faces[i]];
        }
    }

    mesh.verts = std::move(newVerts);
    mesh.normals = std::move(newNormals);
    mesh.colors = std::move(newColors);
    mesh.materials = std::move(newMaterials);
    mesh.nbVerts = newNbVerts;

    mesh.vrfStartCount.clear();
    mesh.vertRingFace.clear();
    mesh.vrvStartCount.clear();
    mesh.vertRingVert.clear();
    mesh.faceEdges.clear();
    mesh.edges.clear();
    mesh.edgeCreases.clear();
    mesh.vertVisible.assign(mesh.nbVerts, 1);
    mesh.faceVisible.assign(mesh.nbFaces, 1);
    mesh.isTopologyDirty = true;
    mesh.hasCustomNormals = false;

    mesh.postInit(true);
    mesh.invalidateDynamicSubdiv();
    mesh.isDirty = true;
    mesh.isTopologyDirty = true;

    selMgr.clear();
    selMgr.setMode(ComponentMode::Vertex);
    if (vertRemap[targetV] != -1) {
        selMgr.selectVertex((uint32_t)vertRemap[targetV]);
    }
    selMgr.invalidateTopologyCache();
    return true;
}

bool ComponentOps::collapseEdges(Mesh& mesh, SelectionManager& selMgr) {
    if (mesh.nbFaces == 0 || mesh.nbVerts == 0) return false;

    // For edges, merge the endpoints of each selected edge to their midpoint
    if (selMgr.getMode() == ComponentMode::Edge) {
        selMgr.ensureEdgeTopology(mesh);
        const auto& edgeTop = selMgr.getEdgeTopology(mesh);
        const auto& selEdges = selMgr.getSelectedEdges();
        if (selEdges.empty()) return false;

        // Collect all vertices in the selected edges
        std::unordered_set<uint32_t> edgeVerts;
        for (uint32_t e : selEdges) {
            if (e < edgeTop.size()) {
                edgeVerts.insert(edgeTop[e].v0);
                edgeVerts.insert(edgeTop[e].v1);
            }
        }
        if (edgeVerts.size() < 2) return false;

        // Perform merge
        return mergeToCenter(mesh, selMgr);
    } else {
        // Vertex or face collapse: merge to center
        return mergeToCenter(mesh, selMgr);
    }
}

// -------------------------------------------------------------
// CLOSE HOLES TOOL
// -------------------------------------------------------------

bool ComponentOps::closeHoles(Mesh& mesh, SelectionManager& selMgr) {
    if (mesh.nbFaces == 0 || mesh.nbVerts == 0) return false;

    selMgr.ensureEdgeTopology(mesh);
    const auto& edgeTop = selMgr.getEdgeTopology(mesh);

    // 1. Identify all boundary directed half-edges
    // top.f0 >= 0 && top.f1 == -1 means edge is only in top.f0
    struct BoundaryHalfEdge {
        uint32_t v0;
        uint32_t v1;
        uint32_t edgeIdx;
    };
    std::unordered_map<uint32_t, std::vector<BoundaryHalfEdge>> outgoingHalfEdges;
    std::vector<BoundaryHalfEdge> startEdges;

    for (size_t e = 0; e < edgeTop.size(); ++e) {
        const auto& top = edgeTop[e];
        if (top.f0 >= 0 && top.f1 == -1) {
            uint32_t va = top.v0;
            uint32_t vb = top.v1;
            int id = top.f0 * 4;
            int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
            bool f0UsesVaToVb = false;
            for (int k = 0; k < numV; ++k) {
                if (mesh.faces[id + k] == va && mesh.faces[id + ((k + 1) % numV)] == vb) {
                    f0UsesVaToVb = true;
                    break;
                }
            }
            uint32_t bStart = f0UsesVaToVb ? vb : va;
            uint32_t bEnd   = f0UsesVaToVb ? va : vb;
            BoundaryHalfEdge bhe{bStart, bEnd, (uint32_t)e};
            outgoingHalfEdges[bStart].push_back(bhe);
            startEdges.push_back(bhe);
        }
    }

    if (startEdges.empty()) {
        return false;
    }

    // 2. Trace connected boundary loops by edge
    std::vector<bool> edgeUsed(edgeTop.size(), false);
    std::vector<std::vector<uint32_t>> loops;

    for (const auto& startHe : startEdges) {
        if (edgeUsed[startHe.edgeIdx]) continue;

        std::vector<uint32_t> loop;
        uint32_t cur = startHe.v0;
        uint32_t target = cur;
        bool closed = false;

        loop.push_back(cur);
        edgeUsed[startHe.edgeIdx] = true;
        cur = startHe.v1;

        while (cur != target && loop.size() <= (size_t)mesh.nbVerts) {
            loop.push_back(cur);
            bool found = false;
            if (outgoingHalfEdges.count(cur)) {
                for (const auto& he : outgoingHalfEdges[cur]) {
                    if (!edgeUsed[he.edgeIdx]) {
                        edgeUsed[he.edgeIdx] = true;
                        cur = he.v1;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) break;
            if (cur == target) {
                closed = true;
                break;
            }
        }
        if (cur == target) closed = true;

        if (closed && loop.size() >= 3) {
            loops.push_back(std::move(loop));
        }
    }

    if (loops.empty()) return false;

    // Filter loops if user has active selection
    std::vector<std::vector<uint32_t>> loopsToFill;
    if (selMgr.hasSelection()) {
        auto effVerts = selMgr.getEffectiveVertexIndices(mesh);
        std::unordered_set<uint32_t> selSet(effVerts.begin(), effVerts.end());
        for (auto& l : loops) {
            bool touches = false;
            for (uint32_t v : l) {
                if (selSet.count(v)) { touches = true; break; }
            }
            if (touches) loopsToFill.push_back(std::move(l));
        }
    }
    if (loopsToFill.empty()) {
        loopsToFill = std::move(loops);
    }

    uint32_t capGid = mesh.getNextFreeGroupID();
    std::unordered_set<uint32_t> newCapFaces;

    for (auto& loop : loopsToFill) {
        size_t k = loop.size();
        if (k < 3) continue;

        // Compute Newell's 3D polygon normal and average surrounding vertex/face normal
        glm::vec3 polyNormal(0.0f);
        glm::vec3 centroid(0.0f);
        glm::vec3 avgNormal(0.0f);

        for (size_t i = 0; i < k; ++i) {
            size_t nextI = (i + 1) % k;
            uint32_t vCur = loop[i];
            uint32_t vNext = loop[nextI];
            glm::vec3 pCur(mesh.verts[vCur * 3], mesh.verts[vCur * 3 + 1], mesh.verts[vCur * 3 + 2]);
            glm::vec3 pNext(mesh.verts[vNext * 3], mesh.verts[vNext * 3 + 1], mesh.verts[vNext * 3 + 2]);

            // Newell's method for arbitrary 3D polygon normal
            polyNormal.x += (pCur.y - pNext.y) * (pCur.z + pNext.z);
            polyNormal.y += (pCur.z - pNext.z) * (pCur.x + pNext.x);
            polyNormal.z += (pCur.x - pNext.x) * (pCur.y + pNext.y);

            centroid += pCur;
            if (mesh.normals.size() >= (size_t)(vCur + 1) * 3) {
                avgNormal += glm::vec3(mesh.normals[vCur * 3], mesh.normals[vCur * 3 + 1], mesh.normals[vCur * 3 + 2]);
            }
        }
        centroid /= (float)k;
        if (glm::length(avgNormal) > 1e-4f) avgNormal = glm::normalize(avgNormal);
        else avgNormal = glm::vec3(0.0f, 1.0f, 0.0f);

        // Verify orientation: does the loop winding produce a normal that aligns with the surrounding surface?
        if (glm::length(polyNormal) > 1e-6f) {
            polyNormal = glm::normalize(polyNormal);
            if (glm::dot(polyNormal, avgNormal) < 0.0f) {
                // Inverted! Reverse the loop vertices so that the cap normal points outward with the mesh!
                std::reverse(loop.begin(), loop.end());
            }
        }

        if (k == 3) {
            uint32_t newF = (uint32_t)mesh.nbFaces++;
            mesh.faces.push_back(loop[0]);
            mesh.faces.push_back(loop[1]);
            mesh.faces.push_back(loop[2]);
            mesh.faces.push_back(0xffffffff);
            mesh.faceGroups.push_back(capGid);
            newCapFaces.insert(newF);
        } else if (k == 4) {
            uint32_t newF = (uint32_t)mesh.nbFaces++;
            mesh.faces.push_back(loop[0]);
            mesh.faces.push_back(loop[1]);
            mesh.faces.push_back(loop[2]);
            mesh.faces.push_back(loop[3]);
            mesh.faceGroups.push_back(capGid);
            newCapFaces.insert(newF);
        } else {
            // Triangle fan to centroid
            uint32_t cIdx = (uint32_t)mesh.nbVerts++;
            mesh.verts.push_back(centroid.x);
            mesh.verts.push_back(centroid.y);
            mesh.verts.push_back(centroid.z);

            mesh.normals.push_back(avgNormal.x);
            mesh.normals.push_back(avgNormal.y);
            mesh.normals.push_back(avgNormal.z);

            mesh.colors.push_back(1.0f); mesh.colors.push_back(1.0f); mesh.colors.push_back(1.0f);
            mesh.materials.push_back(0.5f); mesh.materials.push_back(0.0f); mesh.materials.push_back(1.0f);

            for (size_t i = 0; i < k; ++i) {
                size_t nextI = (i + 1) % k;
                uint32_t newF = (uint32_t)mesh.nbFaces++;
                mesh.faces.push_back(loop[i]);
                mesh.faces.push_back(loop[nextI]);
                mesh.faces.push_back(cIdx);
                mesh.faces.push_back(0xffffffff);
                mesh.faceGroups.push_back(capGid);
                newCapFaces.insert(newF);
            }
        }
    }

    mesh.vrfStartCount.clear();
    mesh.vertRingFace.clear();
    mesh.vrvStartCount.clear();
    mesh.vertRingVert.clear();
    mesh.faceEdges.clear();
    mesh.edges.clear();
    mesh.edgeCreases.clear();
    mesh.vertVisible.assign(mesh.nbVerts, 1);
    mesh.faceVisible.assign(mesh.nbFaces, 1);
    mesh.isTopologyDirty = true;
    mesh.hasCustomNormals = false;

    mesh.postInit(true);
    mesh.invalidateDynamicSubdiv();
    mesh.isDirty = true;
    mesh.isTopologyDirty = true;

    selMgr.clear();
    selMgr.setMode(ComponentMode::Face);
    for (uint32_t f : newCapFaces) {
        selMgr.selectFace(f, true);
    }
    selMgr.invalidateTopologyCache();
    return true;
}
