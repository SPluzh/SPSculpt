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
        for (int f = 0; f < mesh.nbFaces; ++f) {
            int id = f * 4;
            int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
            for (int k = 0; k < numV; ++k) {
                if (selVerts.count(mesh.faces[id + k])) {
                    selMgr.selectFace((uint32_t)f, true);
                    break;
                }
            }
        }
        selMgr.setMode(ComponentMode::Face);
        return extrudeFaces(mesh, selMgr, offsetMagnitude);
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

    // 3. Compute extrusion direction for each vertex
    std::unordered_map<uint32_t, glm::vec3> vertNormals;
    for (uint32_t f : selFaces) {
        int id = f * 4;
        glm::vec3 fn(0.0f, 1.0f, 0.0f);
        if (mesh.faceNormals.size() >= (size_t)(f + 1) * 3) {
            fn = glm::vec3(mesh.faceNormals[f * 3], mesh.faceNormals[f * 3 + 1], mesh.faceNormals[f * 3 + 2]);
        }
        if (glm::length(fn) < 1e-4f) fn = glm::vec3(0.0f, 1.0f, 0.0f);

        int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;
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

        mesh.faces.push_back(va);
        mesh.faces.push_back(vb);
        mesh.faces.push_back(vb_ext);
        mesh.faces.push_back(va_ext);
        mesh.faceGroups.push_back(sideGid);
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

    std::unordered_map<uint32_t, uint32_t> oldToNew;
    for (uint32_t v : uniqueVerts) {
        if (v >= (uint32_t)mesh.nbVerts) continue;
        uint32_t newV = (uint32_t)mesh.nbVerts++;
        oldToNew[v] = newV;

        glm::vec3 pos(mesh.verts[v * 3], mesh.verts[v * 3 + 1], mesh.verts[v * 3 + 2]);
        glm::vec3 n(0.0f, 1.0f, 0.0f);
        if (mesh.normals.size() >= (size_t)(v + 1) * 3) {
            n = glm::vec3(mesh.normals[v * 3], mesh.normals[v * 3 + 1], mesh.normals[v * 3 + 2]);
        }
        if (glm::length(n) < 1e-4f) n = glm::vec3(0.0f, 1.0f, 0.0f);
        glm::vec3 newPos = pos + n * offsetMagnitude;

        mesh.verts.push_back(newPos.x);
        mesh.verts.push_back(newPos.y);
        mesh.verts.push_back(newPos.z);

        mesh.normals.push_back(n.x);
        mesh.normals.push_back(n.y);
        mesh.normals.push_back(n.z);

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
    for (uint32_t e : selEdges) {
        if (e >= edgeTop.size()) continue;
        uint32_t va = edgeTop[e].v0;
        uint32_t vb = edgeTop[e].v1;
        if (!oldToNew.count(va) || !oldToNew.count(vb)) continue;

        // If edge has an incident face f0, ensure bottom edge is opposite to f0 for 2-manifold consistency
        if (edgeTop[e].f0 >= 0) {
            int id0 = edgeTop[e].f0 * 4;
            int numV0 = (mesh.faces[id0 + 3] != 0xffffffff) ? 4 : 3;
            for (int k = 0; k < numV0; ++k) {
                if (mesh.faces[id0 + k] == va && mesh.faces[id0 + ((k + 1) % numV0)] == vb) {
                    std::swap(va, vb);
                    break;
                }
            }
        }

        uint32_t va_ext = oldToNew[va];
        uint32_t vb_ext = oldToNew[vb];

        glm::vec3 pva(mesh.verts[va * 3], mesh.verts[va * 3 + 1], mesh.verts[va * 3 + 2]);
        glm::vec3 pvb(mesh.verts[vb * 3], mesh.verts[vb * 3 + 1], mesh.verts[vb * 3 + 2]);
        glm::vec3 pva_ext(mesh.verts[va_ext * 3], mesh.verts[va_ext * 3 + 1], mesh.verts[va_ext * 3 + 2]);

        // Quad normal: (pvb - pva) x (pva_ext - pva)
        glm::vec3 nQuad = glm::cross(pvb - pva, pva_ext - pva);
        glm::vec3 nRef(0.0f);
        if (edgeTop[e].f0 >= 0 && (size_t)edgeTop[e].f0 * 3 + 2 < mesh.faceNormals.size()) {
            nRef += glm::vec3(mesh.faceNormals[edgeTop[e].f0 * 3],
                              mesh.faceNormals[edgeTop[e].f0 * 3 + 1],
                              mesh.faceNormals[edgeTop[e].f0 * 3 + 2]);
        }
        if (glm::length(nRef) < 1e-4f) {
            for (uint32_t v : {va, vb}) {
                if ((size_t)v * 3 + 2 < mesh.normals.size()) {
                    nRef += glm::vec3(mesh.normals[v * 3], mesh.normals[v * 3 + 1], mesh.normals[v * 3 + 2]);
                }
            }
        }

        uint32_t q0 = va, q1 = vb, q2 = vb_ext, q3 = va_ext;
        if (glm::length(nQuad) > 1e-6f && glm::length(nRef) > 1e-4f && glm::dot(nQuad, nRef) < 0.0f) {
            q0 = va; q1 = va_ext; q2 = vb_ext; q3 = vb;
        }

        mesh.faces.push_back(q0);
        mesh.faces.push_back(q1);
        mesh.faces.push_back(q2);
        mesh.faces.push_back(q3);
        mesh.faceGroups.push_back(gid);
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

    selMgr.clear();
    selMgr.invalidateTopologyCache();
    return true;
}

bool ComponentOps::bevelSelection(Mesh& mesh, SelectionManager& selMgr, float width, float heightOffset) {
    if (selMgr.getMode() == ComponentMode::Face || selMgr.getSelectedFaceCount() > 0) {
        return bevelFaces(mesh, selMgr, width, heightOffset);
    } else if (selMgr.getMode() == ComponentMode::Edge) {
        return bevelEdges(mesh, selMgr, width);
    }
    return false;
}

bool ComponentOps::bevelFaces(Mesh& mesh, SelectionManager& selMgr, float width, float heightOffset) {
    const auto& selFaces = selMgr.getSelectedFaces();
    if (selFaces.empty() || mesh.nbFaces == 0 || mesh.nbVerts == 0) return false;

    float t = std::clamp(width, 0.02f, 0.95f);
    uint32_t rimGid = mesh.getNextFreeGroupID();
    std::unordered_set<uint32_t> newSelectedFaces;

    for (uint32_t f : selFaces) {
        if (f >= (uint32_t)mesh.nbFaces) continue;
        int id = f * 4;
        int numV = (mesh.faces[id + 3] != 0xffffffff) ? 4 : 3;

        glm::vec3 center(0.0f);
        for (int k = 0; k < numV; ++k) {
            uint32_t v = mesh.faces[id + k];
            center += glm::vec3(mesh.verts[v * 3], mesh.verts[v * 3 + 1], mesh.verts[v * 3 + 2]);
        }
        center /= (float)numV;

        glm::vec3 normal(0.0f, 1.0f, 0.0f);
        if (mesh.faceNormals.size() >= (size_t)(f + 1) * 3) {
            normal = glm::vec3(mesh.faceNormals[f * 3], mesh.faceNormals[f * 3 + 1], mesh.faceNormals[f * 3 + 2]);
        }
        if (glm::length(normal) < 1e-4f) normal = glm::vec3(0.0f, 1.0f, 0.0f);

        std::vector<uint32_t> innerVerts(numV);
        for (int k = 0; k < numV; ++k) {
            uint32_t origV = mesh.faces[id + k];
            glm::vec3 origPos(mesh.verts[origV * 3], mesh.verts[origV * 3 + 1], mesh.verts[origV * 3 + 2]);
            glm::vec3 innerPos = origPos + (center - origPos) * t + normal * heightOffset;

            uint32_t newV = (uint32_t)mesh.nbVerts++;
            innerVerts[k] = newV;

            mesh.verts.push_back(innerPos.x);
            mesh.verts.push_back(innerPos.y);
            mesh.verts.push_back(innerPos.z);

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
        }

        std::vector<uint32_t> origVerts(numV);
        for (int k = 0; k < numV; ++k) {
            origVerts[k] = mesh.faces[id + k];
        }

        for (int k = 0; k < numV; ++k) {
            mesh.faces[id + k] = innerVerts[k];
        }
        newSelectedFaces.insert(f);

        for (int k = 0; k < numV; ++k) {
            int kn = (k + 1) % numV;
            uint32_t va = origVerts[k];
            uint32_t vb = origVerts[kn];
            uint32_t va_in = innerVerts[k];
            uint32_t vb_in = innerVerts[kn];

            mesh.faces.push_back(va);
            mesh.faces.push_back(vb);
            mesh.faces.push_back(vb_in);
            mesh.faces.push_back(va_in);
            mesh.faceGroups.push_back(rimGid);
            mesh.nbFaces++;
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

bool ComponentOps::bevelEdges(Mesh& mesh, SelectionManager& selMgr, float width) {
    const auto& selEdges = selMgr.getSelectedEdges();
    if (selEdges.empty() || mesh.nbEdges == 0 || mesh.nbVerts == 0) return false;

    selMgr.ensureEdgeTopology(mesh);
    const auto& edgeTop = selMgr.getEdgeTopology(mesh);

    float w = std::clamp(width, 0.01f, 0.5f);
    uint32_t bevelGid = mesh.getNextFreeGroupID();

    for (uint32_t e : selEdges) {
        if (e >= edgeTop.size()) continue;
        const auto& top = edgeTop[e];
        if (top.f0 < 0) continue;

        uint32_t va = top.v0;
        uint32_t vb = top.v1;
        if (va >= (uint32_t)mesh.nbVerts || vb >= (uint32_t)mesh.nbVerts) continue;

        glm::vec3 posA(mesh.verts[va * 3], mesh.verts[va * 3 + 1], mesh.verts[va * 3 + 2]);
        glm::vec3 posB(mesh.verts[vb * 3], mesh.verts[vb * 3 + 1], mesh.verts[vb * 3 + 2]);
        glm::vec3 mid = (posA + posB) * 0.5f;

        glm::vec3 c0(0.0f);
        int id0 = top.f0 * 4;
        int numV0 = (mesh.faces[id0 + 3] != 0xffffffff) ? 4 : 3;
        for (int k = 0; k < numV0; ++k) {
            uint32_t v = mesh.faces[id0 + k];
            c0 += glm::vec3(mesh.verts[v * 3], mesh.verts[v * 3 + 1], mesh.verts[v * 3 + 2]);
        }
        c0 /= (float)numV0;

        glm::vec3 dir0 = glm::normalize(c0 - mid);
        uint32_t newVa0 = (uint32_t)mesh.nbVerts++;
        uint32_t newVb0 = (uint32_t)mesh.nbVerts++;

        glm::vec3 pVa0 = posA + dir0 * (glm::length(c0 - mid) * w);
        glm::vec3 pVb0 = posB + dir0 * (glm::length(c0 - mid) * w);

        mesh.verts.push_back(pVa0.x); mesh.verts.push_back(pVa0.y); mesh.verts.push_back(pVa0.z);
        mesh.normals.push_back(0.0f); mesh.normals.push_back(1.0f); mesh.normals.push_back(0.0f);
        mesh.colors.push_back(1.0f); mesh.colors.push_back(1.0f); mesh.colors.push_back(1.0f);
        mesh.materials.push_back(0.5f); mesh.materials.push_back(0.0f); mesh.materials.push_back(1.0f);

        mesh.verts.push_back(pVb0.x); mesh.verts.push_back(pVb0.y); mesh.verts.push_back(pVb0.z);
        mesh.normals.push_back(0.0f); mesh.normals.push_back(1.0f); mesh.normals.push_back(0.0f);
        mesh.colors.push_back(1.0f); mesh.colors.push_back(1.0f); mesh.colors.push_back(1.0f);
        mesh.materials.push_back(0.5f); mesh.materials.push_back(0.0f); mesh.materials.push_back(1.0f);

        if (top.f1 >= 0) {
            glm::vec3 c1(0.0f);
            int id1 = top.f1 * 4;
            int numV1 = (mesh.faces[id1 + 3] != 0xffffffff) ? 4 : 3;
            for (int k = 0; k < numV1; ++k) {
                uint32_t v = mesh.faces[id1 + k];
                c1 += glm::vec3(mesh.verts[v * 3], mesh.verts[v * 3 + 1], mesh.verts[v * 3 + 2]);
            }
            c1 /= (float)numV1;
            glm::vec3 dir1 = glm::normalize(c1 - mid);

            uint32_t newVa1 = (uint32_t)mesh.nbVerts++;
            uint32_t newVb1 = (uint32_t)mesh.nbVerts++;

            glm::vec3 pVa1 = posA + dir1 * (glm::length(c1 - mid) * w);
            glm::vec3 pVb1 = posB + dir1 * (glm::length(c1 - mid) * w);

            mesh.verts.push_back(pVa1.x); mesh.verts.push_back(pVa1.y); mesh.verts.push_back(pVa1.z);
            mesh.normals.push_back(0.0f); mesh.normals.push_back(1.0f); mesh.normals.push_back(0.0f);
            mesh.colors.push_back(1.0f); mesh.colors.push_back(1.0f); mesh.colors.push_back(1.0f);
            mesh.materials.push_back(0.5f); mesh.materials.push_back(0.0f); mesh.materials.push_back(1.0f);

            mesh.verts.push_back(pVb1.x); mesh.verts.push_back(pVb1.y); mesh.verts.push_back(pVb1.z);
            mesh.normals.push_back(0.0f); mesh.normals.push_back(1.0f); mesh.normals.push_back(0.0f);
            mesh.colors.push_back(1.0f); mesh.colors.push_back(1.0f); mesh.colors.push_back(1.0f);
            mesh.materials.push_back(0.5f); mesh.materials.push_back(0.0f); mesh.materials.push_back(1.0f);

            // Replace edge in f0
            for (int k = 0; k < numV0; ++k) {
                if (mesh.faces[id0 + k] == va && mesh.faces[id0 + ((k + 1) % numV0)] == vb) {
                    mesh.faces[id0 + k] = newVa0;
                    mesh.faces[id0 + ((k + 1) % numV0)] = newVb0;
                } else if (mesh.faces[id0 + k] == vb && mesh.faces[id0 + ((k + 1) % numV0)] == va) {
                    mesh.faces[id0 + k] = newVb0;
                    mesh.faces[id0 + ((k + 1) % numV0)] = newVa0;
                }
            }

            // Replace edge in f1
            for (int k = 0; k < numV1; ++k) {
                if (mesh.faces[id1 + k] == va && mesh.faces[id1 + ((k + 1) % numV1)] == vb) {
                    mesh.faces[id1 + k] = newVa1;
                    mesh.faces[id1 + ((k + 1) % numV1)] = newVb1;
                } else if (mesh.faces[id1 + k] == vb && mesh.faces[id1 + ((k + 1) % numV1)] == va) {
                    mesh.faces[id1 + k] = newVb1;
                    mesh.faces[id1 + ((k + 1) % numV1)] = newVa1;
                }
            }

            // Add bevel quad bridging newVa0, newVb0, newVb1, newVa1
            // Check geometric normal against chamfer normal (incident face normals sum)
            glm::vec3 nQuad = glm::cross(pVb0 - pVa0, pVa1 - pVa0) + glm::cross(pVa1 - pVa0, pVb1 - pVa0);
            glm::vec3 nChamfer(0.0f);
            if ((size_t)top.f0 * 3 + 2 < mesh.faceNormals.size()) {
                nChamfer += glm::vec3(mesh.faceNormals[top.f0 * 3], mesh.faceNormals[top.f0 * 3 + 1], mesh.faceNormals[top.f0 * 3 + 2]);
            }
            if ((size_t)top.f1 * 3 + 2 < mesh.faceNormals.size()) {
                nChamfer += glm::vec3(mesh.faceNormals[top.f1 * 3], mesh.faceNormals[top.f1 * 3 + 1], mesh.faceNormals[top.f1 * 3 + 2]);
            }
            if (glm::length(nChamfer) < 1e-4f) {
                for (uint32_t v : {va, vb}) {
                    if ((size_t)v * 3 + 2 < mesh.normals.size()) {
                        nChamfer += glm::vec3(mesh.normals[v * 3], mesh.normals[v * 3 + 1], mesh.normals[v * 3 + 2]);
                    }
                }
            }

            uint32_t q0 = newVa0, q1 = newVb0, q2 = newVb1, q3 = newVa1;
            if (glm::length(nQuad) > 1e-6f && glm::length(nChamfer) > 1e-4f && glm::dot(nQuad, nChamfer) < 0.0f) {
                q0 = newVa0; q1 = newVa1; q2 = newVb1; q3 = newVb0;
            }

            mesh.faces.push_back(q0);
            mesh.faces.push_back(q1);
            mesh.faces.push_back(q2);
            mesh.faces.push_back(q3);
            mesh.faceGroups.push_back(bevelGid);
            mesh.nbFaces++;
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
