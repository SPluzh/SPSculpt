#include "retopo/QuadDrawMeshBuilder.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>
#include "common/Constants.h"

namespace retopo {

static float cross2D(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

static bool SegmentsIntersect2D(const glm::vec2& p1, const glm::vec2& p2, const glm::vec2& p3, const glm::vec2& p4) {
    auto ccw = [](const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) -> float {
        return (c.y - a.y) * (b.x - a.x) - (b.y - a.y) * (c.x - a.x);
    };
    float c1 = ccw(p1, p3, p4);
    float c2 = ccw(p2, p3, p4);
    float c3 = ccw(p1, p2, p3);
    float c4 = ccw(p1, p2, p4);
    return ((c1 > 0.0f && c2 < 0.0f) || (c1 < 0.0f && c2 > 0.0f)) &&
           ((c3 > 0.0f && c4 < 0.0f) || (c3 < 0.0f && c4 > 0.0f));
}

QuadDrawMeshBuilder::QuadDrawMeshBuilder() = default;
QuadDrawMeshBuilder::~QuadDrawMeshBuilder() = default;

void QuadDrawMeshBuilder::NotifyMeshUpdated(Mesh* mesh) {
    if (!mesh) return;
    mesh->nbVerts = (int)(mesh->verts.size() / 3);
    mesh->nbFaces = (int)(mesh->faces.size() / 4);
    mesh->isDirty = true;
    mesh->isTopologyDirty = true;
    mesh->isVertexDirty = true;
    mesh->bumpVertsGeneration();
    mesh->initTopology();
    mesh->postInit();
    InvalidateEdgeCache();
}

Mesh* QuadDrawMeshBuilder::GetOrCreateRetopoMesh(Scene& scene, Mesh* targetMesh) {
    Mesh* active = scene.getSelected();
    if (active && active != targetMesh) {
        return active;
    }

    for (Mesh* m : scene.getMeshes()) {
        if (m && m != targetMesh && m->outlinerName == "QuadDraw_Retopo") {
            scene.setOrUnsetMesh(m, false);
            return m;
        }
    }

    Mesh* newMesh = new Mesh();
    newMesh->outlinerName = "QuadDraw_Retopo";
    if (targetMesh) {
        newMesh->matrix = targetMesh->matrix;
    }
    newMesh->postInit();
    scene.addMesh(newMesh);
    scene.setOrUnsetMesh(newMesh, false);
    return newMesh;
}

int32_t QuadDrawMeshBuilder::AddVertex(Mesh* mesh, const glm::vec3& worldPos) {
    if (!mesh) return -1;
    int32_t newIdx = (int32_t)(mesh->verts.size() / 3);

    glm::vec3 localPos = worldPos;
    if (mesh->matrix != glm::mat4(1.0f)) {
        glm::mat4 invMat = glm::inverse(mesh->matrix);
        localPos = glm::vec3(invMat * glm::vec4(worldPos, 1.0f));
    }

    mesh->verts.push_back(localPos.x);
    mesh->verts.push_back(localPos.y);
    mesh->verts.push_back(localPos.z);

    if (mesh->colors.size() == (size_t)newIdx * 3) {
        mesh->colors.push_back(1.0f); mesh->colors.push_back(1.0f); mesh->colors.push_back(1.0f);
    }
    if (mesh->materials.size() == (size_t)newIdx * 3) {
        mesh->materials.push_back(0.5f); mesh->materials.push_back(0.0f); mesh->materials.push_back(1.0f);
    }
    if (mesh->vertVisible.size() == (size_t)newIdx) {
        mesh->vertVisible.push_back(1);
    }

    NotifyMeshUpdated(mesh);
    return newIdx;
}

bool QuadDrawMeshBuilder::AddQuad(
    Mesh* mesh, int32_t a, int32_t b, int32_t c, int32_t d, const glm::vec3& targetNormal
) {
    if (!mesh) return false;
    int32_t ptCount = (int32_t)(mesh->verts.size() / 3);
    if (a < 0 || a >= ptCount || b < 0 || b >= ptCount ||
        c < 0 || c >= ptCount || d < 0 || d >= ptCount) return false;

    if (a == b || b == c || c == d || d == a || a == c || b == d) return false;

    int32_t polyCount = (int32_t)(mesh->faces.size() / 4);

    // Check duplicate or 3+ shared vertices
    for (int32_t i = 0; i < polyCount; ++i) {
        uint32_t pa = mesh->faces[i * 4 + 0];
        uint32_t pb = mesh->faces[i * 4 + 1];
        uint32_t pc = mesh->faces[i * 4 + 2];
        uint32_t pd = mesh->faces[i * 4 + 3];

        int match = 0;
        if (pa == (uint32_t)a || pa == (uint32_t)b || pa == (uint32_t)c || pa == (uint32_t)d) match++;
        if (pb == (uint32_t)a || pb == (uint32_t)b || pb == (uint32_t)c || pb == (uint32_t)d) match++;
        if (pc == (uint32_t)a || pc == (uint32_t)b || pc == (uint32_t)c || pc == (uint32_t)d) match++;
        if (pd != TRI_INDEX) {
            if (pd == (uint32_t)a || pd == (uint32_t)b || pd == (uint32_t)c || pd == (uint32_t)d) match++;
        }
        if (match >= 3) return false;
    }

    auto countEdgePolys = [&](int32_t u, int32_t v) -> int32_t {
        int32_t cnt = 0;
        for (int32_t i = 0; i < polyCount; ++i) {
            if (PolygonHasEdge(mesh->faces, i, u, v)) cnt++;
        }
        return cnt;
    };

    if (countEdgePolys(a, b) >= 2 || countEdgePolys(b, c) >= 2 ||
        countEdgePolys(c, d) >= 2 || countEdgePolys(d, a) >= 2) return false;

    if (countEdgePolys(a, c) > 0 || countEdgePolys(b, d) > 0) return false;

    auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
        glm::vec3 p(mesh->verts[idx * 3], mesh->verts[idx * 3 + 1], mesh->verts[idx * 3 + 2]);
        return (mesh->matrix != glm::mat4(1.0f)) ? glm::vec3(mesh->matrix * glm::vec4(p, 1.0f)) : p;
    };

    glm::vec3 pA = getVertWorld(a);
    glm::vec3 pB = getVertWorld(b);
    glm::vec3 pC = getVertWorld(c);
    glm::vec3 polyNormal = glm::cross(pB - pA, pC - pA);
    float pLen = glm::length(polyNormal);
    if (pLen > 1e-6f) polyNormal /= pLen;

    if (glm::dot(polyNormal, targetNormal) < 0.0f) {
        std::swap(b, d);
    }

    mesh->faces.push_back((uint32_t)a);
    mesh->faces.push_back((uint32_t)b);
    mesh->faces.push_back((uint32_t)c);
    mesh->faces.push_back((uint32_t)d);

    if (mesh->faceGroups.size() == (size_t)polyCount) {
        mesh->faceGroups.push_back(1);
    }

    NotifyMeshUpdated(mesh);
    return true;
}

bool QuadDrawMeshBuilder::AddTriangle(
    Mesh* mesh, int32_t a, int32_t b, int32_t c, const glm::vec3& targetNormal
) {
    if (!mesh) return false;
    int32_t ptCount = (int32_t)(mesh->verts.size() / 3);
    if (a < 0 || a >= ptCount || b < 0 || b >= ptCount || c < 0 || c >= ptCount) return false;
    if (a == b || b == c || c == a) return false;

    auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
        glm::vec3 p(mesh->verts[idx * 3], mesh->verts[idx * 3 + 1], mesh->verts[idx * 3 + 2]);
        return (mesh->matrix != glm::mat4(1.0f)) ? glm::vec3(mesh->matrix * glm::vec4(p, 1.0f)) : p;
    };

    glm::vec3 pA = getVertWorld(a);
    glm::vec3 pB = getVertWorld(b);
    glm::vec3 pC = getVertWorld(c);
    glm::vec3 polyNormal = glm::cross(pB - pA, pC - pA);
    float pLen = glm::length(polyNormal);
    if (pLen > 1e-6f) polyNormal /= pLen;

    if (glm::dot(polyNormal, targetNormal) < 0.0f) {
        std::swap(b, c);
    }

    mesh->faces.push_back((uint32_t)a);
    mesh->faces.push_back((uint32_t)b);
    mesh->faces.push_back((uint32_t)c);
    mesh->faces.push_back(TRI_INDEX);

    int32_t polyCount = (int32_t)(mesh->faces.size() / 4) - 1;
    if (mesh->faceGroups.size() == (size_t)polyCount) {
        mesh->faceGroups.push_back(1);
    }

    NotifyMeshUpdated(mesh);
    return true;
}

bool QuadDrawMeshBuilder::SetVertexPosition(Mesh* mesh, int32_t index, const glm::vec3& worldPos) {
    if (!mesh || index < 0 || (size_t)index * 3 + 2 >= mesh->verts.size()) return false;

    glm::vec3 localPos = worldPos;
    if (mesh->matrix != glm::mat4(1.0f)) {
        glm::mat4 invMat = glm::inverse(mesh->matrix);
        localPos = glm::vec3(invMat * glm::vec4(worldPos, 1.0f));
    }

    mesh->verts[index * 3 + 0] = localPos.x;
    mesh->verts[index * 3 + 1] = localPos.y;
    mesh->verts[index * 3 + 2] = localPos.z;

    NotifyMeshUpdated(mesh);
    return true;
}

bool QuadDrawMeshBuilder::WeldVertices(
    Mesh* mesh, int32_t vSource, int32_t vTarget, std::unordered_set<uint32_t>* pinnedVertices
) {
    if (!mesh || vSource == vTarget) return false;
    int32_t ptCount = (int32_t)(mesh->verts.size() / 3);
    int32_t polyCount = (int32_t)(mesh->faces.size() / 4);
    if (vSource < 0 || vSource >= ptCount || vTarget < 0 || vTarget >= ptCount) return false;

    // 1. Remap all polygon vertex indices from vSource to vTarget
    for (int32_t i = 0; i < polyCount; ++i) {
        for (int k = 0; k < 4; ++k) {
            if (mesh->faces[i * 4 + k] == (uint32_t)vSource) {
                mesh->faces[i * 4 + k] = (uint32_t)vTarget;
            }
        }
    }

    // 2. Clean degenerate polygons
    std::vector<uint32_t> validFaces;
    validFaces.reserve(mesh->faces.size());

    for (int32_t i = 0; i < polyCount; ++i) {
        uint32_t a = mesh->faces[i * 4 + 0];
        uint32_t b = mesh->faces[i * 4 + 1];
        uint32_t c = mesh->faces[i * 4 + 2];
        uint32_t d = mesh->faces[i * 4 + 3];

        if (d != TRI_INDEX) {
            if (a == b) { a = b; b = c; c = d; d = TRI_INDEX; }
            else if (b == c) { b = c; c = d; d = TRI_INDEX; }
            else if (c == d) { d = TRI_INDEX; }
            else if (d == a) { d = TRI_INDEX; }
        }

        if (d == TRI_INDEX) {
            if (a == b || b == c || a == c) continue; // Degenerate triangle collapsed to edge/point
        }

        // Avoid duplicate faces
        bool isDuplicate = false;
        size_t numValid = validFaces.size() / 4;
        for (size_t j = 0; j < numValid; ++j) {
            if (validFaces[j * 4 + 0] == a && validFaces[j * 4 + 1] == b &&
                validFaces[j * 4 + 2] == c && validFaces[j * 4 + 3] == d) {
                isDuplicate = true;
                break;
            }
        }
        if (!isDuplicate) {
            validFaces.push_back(a);
            validFaces.push_back(b);
            validFaces.push_back(c);
            validFaces.push_back(d);
        }
    }
    mesh->faces = std::move(validFaces);

    if (pinnedVertices && pinnedVertices->count((uint32_t)vSource)) {
        pinnedVertices->insert((uint32_t)vTarget);
    }

    return DeleteVertex(mesh, vSource, pinnedVertices);
}

bool QuadDrawMeshBuilder::DeleteVertex(
    Mesh* mesh, int32_t ptIndex, std::unordered_set<uint32_t>* pinnedVertices
) {
    if (!mesh) return false;
    int32_t ptCount = (int32_t)(mesh->verts.size() / 3);
    int32_t polyCount = (int32_t)(mesh->faces.size() / 4);
    if (ptIndex < 0 || ptIndex >= ptCount) return false;

    // Remaining polygons without ptIndex
    std::vector<uint32_t> remainingFaces;
    remainingFaces.reserve(mesh->faces.size());
    for (int32_t i = 0; i < polyCount; ++i) {
        uint32_t a = mesh->faces[i * 4 + 0];
        uint32_t b = mesh->faces[i * 4 + 1];
        uint32_t c = mesh->faces[i * 4 + 2];
        uint32_t d = mesh->faces[i * 4 + 3];

        if (a == (uint32_t)ptIndex || b == (uint32_t)ptIndex ||
            c == (uint32_t)ptIndex || (d != TRI_INDEX && d == (uint32_t)ptIndex)) {
            continue;
        }
        remainingFaces.push_back(a);
        remainingFaces.push_back(b);
        remainingFaces.push_back(c);
        remainingFaces.push_back(d);
    }

    std::vector<int32_t> oldUseCount(ptCount, 0);
    for (uint32_t idx : mesh->faces) {
        if (idx != TRI_INDEX && idx < (uint32_t)ptCount) oldUseCount[idx]++;
    }

    std::vector<int32_t> newUseCount(ptCount, 0);
    for (uint32_t idx : remainingFaces) {
        if (idx != TRI_INDEX && idx < (uint32_t)ptCount) newUseCount[idx]++;
    }

    std::vector<float> newVerts;
    std::vector<float> newColors;
    std::vector<float> newMaterials;
    std::vector<uint8_t> newVertVisible;
    std::vector<int32_t> oldToNew(ptCount, -1);
    std::unordered_set<uint32_t> newPinned;

    for (int32_t i = 0; i < ptCount; ++i) {
        if (i == ptIndex) {
            oldToNew[i] = -1;
        } else if (oldUseCount[i] == 0) {
            // Unconnected before deletion: preserve it!
            oldToNew[i] = (int32_t)(newVerts.size() / 3);
            newVerts.push_back(mesh->verts[i * 3 + 0]);
            newVerts.push_back(mesh->verts[i * 3 + 1]);
            newVerts.push_back(mesh->verts[i * 3 + 2]);
            if (mesh->colors.size() == (size_t)ptCount * 3) {
                newColors.push_back(mesh->colors[i * 3 + 0]);
                newColors.push_back(mesh->colors[i * 3 + 1]);
                newColors.push_back(mesh->colors[i * 3 + 2]);
            }
            if (mesh->materials.size() == (size_t)ptCount * 3) {
                newMaterials.push_back(mesh->materials[i * 3 + 0]);
                newMaterials.push_back(mesh->materials[i * 3 + 1]);
                newMaterials.push_back(mesh->materials[i * 3 + 2]);
            }
            if (mesh->vertVisible.size() == (size_t)ptCount) {
                newVertVisible.push_back(mesh->vertVisible[i]);
            }
            if (pinnedVertices && pinnedVertices->count((uint32_t)i)) {
                newPinned.insert((uint32_t)oldToNew[i]);
            }
        } else if (newUseCount[i] > 0) {
            oldToNew[i] = (int32_t)(newVerts.size() / 3);
            newVerts.push_back(mesh->verts[i * 3 + 0]);
            newVerts.push_back(mesh->verts[i * 3 + 1]);
            newVerts.push_back(mesh->verts[i * 3 + 2]);
            if (mesh->colors.size() == (size_t)ptCount * 3) {
                newColors.push_back(mesh->colors[i * 3 + 0]);
                newColors.push_back(mesh->colors[i * 3 + 1]);
                newColors.push_back(mesh->colors[i * 3 + 2]);
            }
            if (mesh->materials.size() == (size_t)ptCount * 3) {
                newMaterials.push_back(mesh->materials[i * 3 + 0]);
                newMaterials.push_back(mesh->materials[i * 3 + 1]);
                newMaterials.push_back(mesh->materials[i * 3 + 2]);
            }
            if (mesh->vertVisible.size() == (size_t)ptCount) {
                newVertVisible.push_back(mesh->vertVisible[i]);
            }
            if (pinnedVertices && pinnedVertices->count((uint32_t)i)) {
                newPinned.insert((uint32_t)oldToNew[i]);
            }
        } else {
            oldToNew[i] = -1;
        }
    }

    for (uint32_t& idx : remainingFaces) {
        if (idx != TRI_INDEX) {
            idx = (uint32_t)oldToNew[idx];
        }
    }

    mesh->verts = std::move(newVerts);
    if (!newColors.empty()) mesh->colors = std::move(newColors);
    if (!newMaterials.empty()) mesh->materials = std::move(newMaterials);
    if (!newVertVisible.empty()) mesh->vertVisible = std::move(newVertVisible);
    mesh->faces = std::move(remainingFaces);
    if (pinnedVertices) *pinnedVertices = std::move(newPinned);

    NotifyMeshUpdated(mesh);
    return true;
}

bool QuadDrawMeshBuilder::DeleteVertices(
    Mesh* mesh, const std::vector<int32_t>& ptIndices, std::unordered_set<uint32_t>* pinnedVertices
) {
    if (!mesh || ptIndices.empty()) return false;
    for (int32_t idx : ptIndices) {
        DeleteVertex(mesh, idx, pinnedVertices);
    }
    return true;
}

bool QuadDrawMeshBuilder::DeleteEdge(
    Mesh* mesh, int32_t v0, int32_t v1, std::unordered_set<uint32_t>* pinnedVertices
) {
    if (!mesh || v0 == v1) return false;
    int32_t polyCount = (int32_t)(mesh->faces.size() / 4);

    std::vector<int32_t> adjPolys;
    for (int32_t i = 0; i < polyCount; ++i) {
        if (PolygonHasEdge(mesh->faces, i, v0, v1)) {
            adjPolys.push_back(i);
        }
    }

    if (adjPolys.empty()) return false;
    return DeletePolygons(mesh, adjPolys, pinnedVertices);
}

bool QuadDrawMeshBuilder::DeleteSingleEdge(
    Mesh* mesh, int32_t v0, int32_t v1, std::unordered_set<uint32_t>* pinnedVertices
) {
    return DeleteEdge(mesh, v0, v1, pinnedVertices);
}

bool QuadDrawMeshBuilder::DeletePolygon(
    Mesh* mesh, int32_t polyIndex, std::unordered_set<uint32_t>* pinnedVertices
) {
    if (!mesh || polyIndex < 0 || (size_t)polyIndex * 4 + 3 >= mesh->faces.size()) return false;
    std::vector<int32_t> polys = { polyIndex };
    return DeletePolygons(mesh, polys, pinnedVertices);
}

bool QuadDrawMeshBuilder::DeletePolygons(
    Mesh* mesh, const std::vector<int32_t>& polyIndices, std::unordered_set<uint32_t>* pinnedVertices
) {
    if (!mesh || polyIndices.empty()) return false;
    int32_t ptCount = (int32_t)(mesh->verts.size() / 3);
    int32_t polyCount = (int32_t)(mesh->faces.size() / 4);

    std::unordered_set<int32_t> toDelete(polyIndices.begin(), polyIndices.end());

    std::vector<uint32_t> remainingFaces;
    remainingFaces.reserve(mesh->faces.size());

    for (int32_t i = 0; i < polyCount; ++i) {
        if (toDelete.count(i)) continue;
        remainingFaces.push_back(mesh->faces[i * 4 + 0]);
        remainingFaces.push_back(mesh->faces[i * 4 + 1]);
        remainingFaces.push_back(mesh->faces[i * 4 + 2]);
        remainingFaces.push_back(mesh->faces[i * 4 + 3]);
    }

    std::vector<int32_t> oldUseCount(ptCount, 0);
    for (uint32_t idx : mesh->faces) {
        if (idx != TRI_INDEX && idx < (uint32_t)ptCount) oldUseCount[idx]++;
    }

    std::vector<int32_t> newUseCount(ptCount, 0);
    for (uint32_t idx : remainingFaces) {
        if (idx != TRI_INDEX && idx < (uint32_t)ptCount) newUseCount[idx]++;
    }

    std::vector<float> newVerts;
    std::vector<int32_t> oldToNew(ptCount, -1);
    std::unordered_set<uint32_t> newPinned;

    for (int32_t i = 0; i < ptCount; ++i) {
        if (oldUseCount[i] == 0) {
            oldToNew[i] = (int32_t)(newVerts.size() / 3);
            newVerts.push_back(mesh->verts[i * 3 + 0]);
            newVerts.push_back(mesh->verts[i * 3 + 1]);
            newVerts.push_back(mesh->verts[i * 3 + 2]);
            if (pinnedVertices && pinnedVertices->count((uint32_t)i)) newPinned.insert((uint32_t)oldToNew[i]);
        } else if (newUseCount[i] > 0) {
            oldToNew[i] = (int32_t)(newVerts.size() / 3);
            newVerts.push_back(mesh->verts[i * 3 + 0]);
            newVerts.push_back(mesh->verts[i * 3 + 1]);
            newVerts.push_back(mesh->verts[i * 3 + 2]);
            if (pinnedVertices && pinnedVertices->count((uint32_t)i)) newPinned.insert((uint32_t)oldToNew[i]);
        }
    }

    for (uint32_t& idx : remainingFaces) {
        if (idx != TRI_INDEX) idx = (uint32_t)oldToNew[idx];
    }

    mesh->verts = std::move(newVerts);
    mesh->faces = std::move(remainingFaces);
    if (pinnedVertices) *pinnedVertices = std::move(newPinned);

    NotifyMeshUpdated(mesh);
    return true;
}

int32_t QuadDrawMeshBuilder::DeleteAllUnconnectedPoints(
    Mesh* mesh, std::unordered_set<uint32_t>* pinnedVertices
) {
    if (!mesh || mesh->nbVerts == 0) return 0;
    int32_t ptCount = (int32_t)(mesh->verts.size() / 3);
    std::vector<int32_t> useCount(ptCount, 0);

    for (uint32_t idx : mesh->faces) {
        if (idx != TRI_INDEX && idx < (uint32_t)ptCount) {
            useCount[idx]++;
        }
    }

    int32_t deletedCount = 0;
    std::vector<float> newVerts;
    std::vector<int32_t> oldToNew(ptCount, -1);
    std::unordered_set<uint32_t> newPinned;

    for (int32_t i = 0; i < ptCount; ++i) {
        if (useCount[i] > 0) {
            oldToNew[i] = (int32_t)(newVerts.size() / 3);
            newVerts.push_back(mesh->verts[i * 3 + 0]);
            newVerts.push_back(mesh->verts[i * 3 + 1]);
            newVerts.push_back(mesh->verts[i * 3 + 2]);
            if (pinnedVertices && pinnedVertices->count((uint32_t)i)) {
                newPinned.insert((uint32_t)oldToNew[i]);
            }
        } else {
            deletedCount++;
        }
    }

    if (deletedCount > 0) {
        for (uint32_t& idx : mesh->faces) {
            if (idx != TRI_INDEX) idx = (uint32_t)oldToNew[idx];
        }
        mesh->verts = std::move(newVerts);
        if (pinnedVertices) *pinnedVertices = std::move(newPinned);
        NotifyMeshUpdated(mesh);
    }

    return deletedCount;
}

EdgeLoopResult QuadDrawMeshBuilder::FindEdgeLoop(const Mesh* mesh, int32_t startV0, int32_t startV1) {
    EdgeLoopResult result;
    if (!mesh || startV0 == startV1) return result;

    int32_t polyCount = (int32_t)(mesh->faces.size() / 4);
    int32_t ptCount = (int32_t)(mesh->verts.size() / 3);

    auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
        glm::vec3 p(mesh->verts[idx * 3], mesh->verts[idx * 3 + 1], mesh->verts[idx * 3 + 2]);
        return (mesh->matrix != glm::mat4(1.0f)) ? glm::vec3(mesh->matrix * glm::vec4(p, 1.0f)) : p;
    };

    std::vector<LoopEdge> edges;
    std::unordered_set<uint64_t> visitedEdges;
    auto makeEdgeKey = [](int32_t a, int32_t b) -> uint64_t {
        if (a > b) std::swap(a, b);
        return ((uint64_t)(uint32_t)a << 32) | (uint64_t)(uint32_t)b;
    };

    auto traceDir = [&](int32_t curV0, int32_t curV1, bool forward) {
        int32_t prevPoly = -1;
        while (true) {
            uint64_t key = makeEdgeKey(curV0, curV1);
            if (visitedEdges.count(key)) break;
            visitedEdges.insert(key);

            LoopEdge le;
            le.v0 = curV0;
            le.v1 = curV1;
            le.worldPos0 = getVertWorld(curV0);
            le.worldPos1 = getVertWorld(curV1);
            if (forward) edges.push_back(le);
            else edges.insert(edges.begin(), le);

            // Find incident quad excluding prevPoly
            int32_t nextPoly = -1;
            for (int32_t p = 0; p < polyCount; ++p) {
                if (p == prevPoly) continue;
                if (mesh->faces[p * 4 + 3] == TRI_INDEX) continue; // Only step across quads
                if (PolygonHasEdge(mesh->faces, p, curV0, curV1)) {
                    nextPoly = p;
                    break;
                }
            }
            if (nextPoly == -1) break;

            // Across nextPoly, find opposite edge
            uint32_t q[4] = {
                mesh->faces[nextPoly * 4 + 0],
                mesh->faces[nextPoly * 4 + 1],
                mesh->faces[nextPoly * 4 + 2],
                mesh->faces[nextPoly * 4 + 3]
            };
            int32_t oppV0 = -1, oppV1 = -1;
            if ((q[0] == (uint32_t)curV0 && q[1] == (uint32_t)curV1) || (q[0] == (uint32_t)curV1 && q[1] == (uint32_t)curV0)) {
                oppV0 = q[3]; oppV1 = q[2];
            } else if ((q[1] == (uint32_t)curV0 && q[2] == (uint32_t)curV1) || (q[1] == (uint32_t)curV1 && q[2] == (uint32_t)curV0)) {
                oppV0 = q[0]; oppV1 = q[3];
            } else if ((q[2] == (uint32_t)curV0 && q[3] == (uint32_t)curV1) || (q[2] == (uint32_t)curV1 && q[3] == (uint32_t)curV0)) {
                oppV0 = q[1]; oppV1 = q[0];
            } else if ((q[3] == (uint32_t)curV0 && q[0] == (uint32_t)curV1) || (q[3] == (uint32_t)curV1 && q[0] == (uint32_t)curV0)) {
                oppV0 = q[2]; oppV1 = q[1];
            }
            if (oppV0 == -1 || oppV1 == -1) break;

            prevPoly = nextPoly;
            curV0 = oppV0;
            curV1 = oppV1;
        }
    };

    traceDir(startV0, startV1, true);
    result.edges = edges;
    return result;
}

std::vector<int32_t> QuadDrawMeshBuilder::FindPolygonLoop(
    const Mesh* mesh, int32_t startPoly, int32_t enterV0, int32_t enterV1
) {
    std::vector<int32_t> loop;
    if (!mesh || startPoly < 0 || (size_t)startPoly * 4 + 3 >= mesh->faces.size()) return loop;
    if (mesh->faces[startPoly * 4 + 3] == TRI_INDEX) {
        loop.push_back(startPoly);
        return loop;
    }

    int32_t polyCount = (int32_t)(mesh->faces.size() / 4);
    std::unordered_set<int32_t> visited;

    int32_t curPoly = startPoly;
    int32_t curV0 = enterV0;
    int32_t curV1 = enterV1;

    while (curPoly != -1 && !visited.count(curPoly)) {
        visited.insert(curPoly);
        loop.push_back(curPoly);

        if (mesh->faces[curPoly * 4 + 3] == TRI_INDEX) break;

        uint32_t q[4] = {
            mesh->faces[curPoly * 4 + 0],
            mesh->faces[curPoly * 4 + 1],
            mesh->faces[curPoly * 4 + 2],
            mesh->faces[curPoly * 4 + 3]
        };

        int32_t oppV0 = -1, oppV1 = -1;
        if ((q[0] == (uint32_t)curV0 && q[1] == (uint32_t)curV1) || (q[0] == (uint32_t)curV1 && q[1] == (uint32_t)curV0)) {
            oppV0 = q[2]; oppV1 = q[3];
        } else if ((q[1] == (uint32_t)curV0 && q[2] == (uint32_t)curV1) || (q[1] == (uint32_t)curV1 && q[2] == (uint32_t)curV0)) {
            oppV0 = q[3]; oppV1 = q[0];
        } else if ((q[2] == (uint32_t)curV0 && q[3] == (uint32_t)curV1) || (q[2] == (uint32_t)curV1 && q[3] == (uint32_t)curV0)) {
            oppV0 = q[0]; oppV1 = q[1];
        } else if ((q[3] == (uint32_t)curV0 && q[0] == (uint32_t)curV1) || (q[3] == (uint32_t)curV1 && q[0] == (uint32_t)curV0)) {
            oppV0 = q[1]; oppV1 = q[2];
        } else {
            break;
        }

        int32_t nextPoly = -1;
        for (int32_t p = 0; p < polyCount; ++p) {
            if (p == curPoly) continue;
            if (PolygonHasEdge(mesh->faces, p, oppV0, oppV1)) {
                nextPoly = p;
                break;
            }
        }
        curPoly = nextPoly;
        curV0 = oppV0;
        curV1 = oppV1;
    }

    return loop;
}

std::vector<int32_t> QuadDrawMeshBuilder::FindVertexLoop(
    const Camera& camera, const Mesh* mesh, int32_t startV, float screenX, float screenY, std::vector<LoopEdge>* outEdges
) {
    std::vector<int32_t> loop;
    if (!mesh || startV < 0 || (size_t)startV * 3 + 2 >= mesh->verts.size()) return loop;
    loop.push_back(startV);
    return loop;
}

bool QuadDrawMeshBuilder::DeleteEdgeLoop(
    Mesh* mesh, const std::vector<LoopEdge>& loopEdges, std::unordered_set<uint32_t>* pinnedVertices
) {
    if (!mesh || loopEdges.empty()) return false;
    std::vector<int32_t> polysToDelete;
    int32_t polyCount = (int32_t)(mesh->faces.size() / 4);

    for (const auto& le : loopEdges) {
        for (int32_t p = 0; p < polyCount; ++p) {
            if (PolygonHasEdge(mesh->faces, p, le.v0, le.v1)) {
                polysToDelete.push_back(p);
            }
        }
    }
    return DeletePolygons(mesh, polysToDelete, pinnedVertices);
}

int32_t QuadDrawMeshBuilder::FindPolygonUnderScreen(
    const Camera& camera, const Mesh* mesh, float screenX, float screenY,
    const Mesh* targetMesh, SurfaceSnapper* snapper,
    float* outAvgZ, const SnapResult* precomputedTargetSnap
) {
    if (!mesh || mesh->nbFaces == 0 || mesh->nbVerts == 0) return -1;

    const int numFaces = mesh->nbFaces;
    const int numVerts = mesh->nbVerts;
    const auto& faces = mesh->faces;
    const auto& verts = mesh->verts;
    const glm::mat4& mat = mesh->matrix;
    const bool hasTransform = (mat != glm::mat4(1.0f));

    auto getVertWorld = [&](uint32_t idx) -> glm::vec3 {
        glm::vec3 p(verts[idx * 3], verts[idx * 3 + 1], verts[idx * 3 + 2]);
        return hasTransform ? glm::vec3(mat * glm::vec4(p, 1.0f)) : p;
    };

    glm::vec2 cursor(screenX, screenY);
    int32_t bestPoly = -1;
    float bestZ = 1e30f;

    for (int f = 0; f < numFaces; ++f) {
        if (!mesh->faceVisible.empty() && !mesh->faceVisible[f]) continue;

        uint32_t fv[4] = { faces[f * 4 + 0], faces[f * 4 + 1], faces[f * 4 + 2], faces[f * 4 + 3] };
        if (fv[0] >= (uint32_t)numVerts || fv[1] >= (uint32_t)numVerts || fv[2] >= (uint32_t)numVerts) continue;

        glm::vec3 s0 = camera.project(getVertWorld(fv[0]));
        glm::vec3 s1 = camera.project(getVertWorld(fv[1]));
        glm::vec3 s2 = camera.project(getVertWorld(fv[2]));
        if (s0.z <= 0.0f || s1.z <= 0.0f || s2.z <= 0.0f) continue;

        bool isQuad = (fv[3] != TRI_INDEX && fv[3] < (uint32_t)numVerts);
        glm::vec3 s3(0.0f);
        if (isQuad) {
            s3 = camera.project(getVertWorld(fv[3]));
            if (s3.z <= 0.0f) continue;
        }

        // 2D inside test
        bool inside = false;
        float zVal = (s0.z + s1.z + s2.z) * (1.0f / 3.0f);

        auto ptInTri = [](const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) -> bool {
            float d1 = cross2D(a, b, p);
            float d2 = cross2D(b, c, p);
            float d3 = cross2D(c, a, p);
            bool hasNeg = (d1 < 0.0f) || (d2 < 0.0f) || (d3 < 0.0f);
            bool hasPos = (d1 > 0.0f) || (d2 > 0.0f) || (d3 > 0.0f);
            return !(hasNeg && hasPos);
        };

        if (ptInTri(cursor, glm::vec2(s0), glm::vec2(s1), glm::vec2(s2))) {
            inside = true;
        } else if (isQuad) {
            zVal = (s0.z + s1.z + s2.z + s3.z) * 0.25f;
            if (ptInTri(cursor, glm::vec2(s0), glm::vec2(s2), glm::vec2(s3))) {
                inside = true;
            }
        }

        if (inside && zVal < bestZ) {
            bestZ = zVal;
            bestPoly = f;
        }
    }

    if (outAvgZ && bestPoly != -1) *outAvgZ = bestZ;
    return bestPoly;
}

QuadPreview QuadDrawMeshBuilder::FindPotentialQuad(
    const Camera& camera, const Mesh* retopo, const glm::vec3& targetNormal,
    float screenX, float screenY, const Mesh* targetMesh, SurfaceSnapper* snapper
) {
    QuadPreview result;
    result.valid = false;
    if (!retopo || retopo->nbVerts < 4) return result;

    if (FindPolygonUnderScreen(camera, retopo, screenX, screenY, targetMesh, snapper) != -1) {
        return result;
    }

    const int ptCount = retopo->nbVerts;
    const int polyCount = retopo->nbFaces;
    const auto& faces = retopo->faces;
    const auto& verts = retopo->verts;
    const glm::mat4& mat = retopo->matrix;
    const bool hasTransform = (mat != glm::mat4(1.0f));

    auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
        glm::vec3 p(verts[idx * 3], verts[idx * 3 + 1], verts[idx * 3 + 2]);
        return hasTransform ? glm::vec3(mat * glm::vec4(p, 1.0f)) : p;
    };

    std::vector<glm::vec3> allScreenPts(ptCount);
    std::vector<bool> isFrontVertex(ptCount, false);

    for (int i = 0; i < ptCount; ++i) {
        if (!retopo->vertVisible.empty() && !retopo->vertVisible[i]) continue;
        glm::vec3 s = camera.project(getVertWorld(i));
        allScreenPts[i] = s;
        if (s.z > 0.0f) isFrontVertex[i] = true;
    }

    struct CandidatePt {
        int32_t index;
        glm::vec3 worldPos;
        glm::vec3 screenPos;
        float distSq;
    };

    std::vector<CandidatePt> candidates;
    const float maxSearchRadius = 600.0f;
    const float maxRadiusSq = maxSearchRadius * maxSearchRadius;

    for (int i = 0; i < ptCount; ++i) {
        if (!isFrontVertex[i]) continue;
        const glm::vec3& sPos = allScreenPts[i];
        float dx = sPos.x - screenX;
        if (std::abs(dx) > maxSearchRadius) continue;
        float dy = sPos.y - screenY;
        if (std::abs(dy) > maxSearchRadius) continue;

        float dSq = dx * dx + dy * dy;
        if (dSq <= maxRadiusSq) {
            CandidatePt cp;
            cp.index = i;
            cp.worldPos = getVertWorld(i);
            cp.screenPos = sPos;
            cp.distSq = dSq;
            candidates.push_back(cp);
        }
    }

    if (candidates.size() < 4) return result;

    std::sort(candidates.begin(), candidates.end(), [](const CandidatePt& a, const CandidatePt& b) {
        return a.distSq < b.distSq;
    });

    int numCand = (int)std::min(candidates.size(), (size_t)8);

    auto hasExistingPolygon4 = [&](int32_t a, int32_t b, int32_t c, int32_t d) -> bool {
        for (int p = 0; p < polyCount; ++p) {
            uint32_t pa = faces[p * 4 + 0];
            uint32_t pb = faces[p * 4 + 1];
            uint32_t pc = faces[p * 4 + 2];
            uint32_t pd = faces[p * 4 + 3];
            int matchCount = 0;
            if (pa == (uint32_t)a || pa == (uint32_t)b || pa == (uint32_t)c || pa == (uint32_t)d) matchCount++;
            if (pb == (uint32_t)a || pb == (uint32_t)b || pb == (uint32_t)c || pb == (uint32_t)d) matchCount++;
            if (pc == (uint32_t)a || pc == (uint32_t)b || pc == (uint32_t)c || pc == (uint32_t)d) matchCount++;
            if (pd != TRI_INDEX) {
                if (pd == (uint32_t)a || pd == (uint32_t)b || pd == (uint32_t)c || pd == (uint32_t)d) matchCount++;
                if (matchCount == 4) return true;
            } else {
                if (matchCount == 3) return true;
            }
        }
        return false;
    };

    auto sharesThreeOrMoreWithExisting = [&](int32_t a, int32_t b, int32_t c, int32_t d) -> bool {
        for (int p = 0; p < polyCount; ++p) {
            uint32_t pa = faces[p * 4 + 0];
            uint32_t pb = faces[p * 4 + 1];
            uint32_t pc = faces[p * 4 + 2];
            uint32_t pd = faces[p * 4 + 3];
            int matchCount = 0;
            if (pa == (uint32_t)a || pa == (uint32_t)b || pa == (uint32_t)c || pa == (uint32_t)d) matchCount++;
            if (pb == (uint32_t)a || pb == (uint32_t)b || pb == (uint32_t)c || pb == (uint32_t)d) matchCount++;
            if (pc == (uint32_t)a || pc == (uint32_t)b || pc == (uint32_t)c || pc == (uint32_t)d) matchCount++;
            if (pd != TRI_INDEX) {
                if (pd == (uint32_t)a || pd == (uint32_t)b || pd == (uint32_t)c || pd == (uint32_t)d) matchCount++;
            }
            if (matchCount >= 3) return true;
        }
        return false;
    };

    auto countEdgePolys = [&](int32_t u, int32_t v) -> int32_t {
        int32_t count = 0;
        for (int p = 0; p < polyCount; ++p) {
            if (PolygonHasEdge(faces, p, u, v)) count++;
        }
        return count;
    };

    float bestScore = 1e30f;
    glm::vec2 cursor(screenX, screenY);

    for (int i0 = 0; i0 < numCand - 3; ++i0) {
        for (int i1 = i0 + 1; i1 < numCand - 2; ++i1) {
            for (int i2 = i1 + 1; i2 < numCand - 1; ++i2) {
                for (int i3 = i2 + 1; i3 < numCand; ++i3) {
                    const CandidatePt& c0 = candidates[i0];
                    const CandidatePt& c1 = candidates[i1];
                    const CandidatePt& c2 = candidates[i2];
                    const CandidatePt& c3 = candidates[i3];

                    float minX = std::min({c0.screenPos.x, c1.screenPos.x, c2.screenPos.x, c3.screenPos.x});
                    float maxX = std::max({c0.screenPos.x, c1.screenPos.x, c2.screenPos.x, c3.screenPos.x});
                    if (screenX < minX || screenX > maxX) continue;

                    float minY = std::min({c0.screenPos.y, c1.screenPos.y, c2.screenPos.y, c3.screenPos.y});
                    float maxY = std::max({c0.screenPos.y, c1.screenPos.y, c2.screenPos.y, c3.screenPos.y});
                    if (screenY < minY || screenY > maxY) continue;

                    CandidatePt quadPts[4] = { c0, c1, c2, c3 };
                    glm::vec2 centroid = (glm::vec2(c0.screenPos) + glm::vec2(c1.screenPos) +
                                          glm::vec2(c2.screenPos) + glm::vec2(c3.screenPos)) * 0.25f;

                    float angles[4];
                    for (int k = 0; k < 4; ++k) {
                        angles[k] = std::atan2(quadPts[k].screenPos.y - centroid.y, quadPts[k].screenPos.x - centroid.x);
                    }

                    for (int a = 0; a < 3; ++a) {
                        for (int b = a + 1; b < 4; ++b) {
                            if (angles[a] > angles[b]) {
                                std::swap(angles[a], angles[b]);
                                std::swap(quadPts[a], quadPts[b]);
                            }
                        }
                    }

                    float cp0 = cross2D(glm::vec2(quadPts[0].screenPos), glm::vec2(quadPts[1].screenPos), glm::vec2(quadPts[2].screenPos));
                    float cp1 = cross2D(glm::vec2(quadPts[1].screenPos), glm::vec2(quadPts[2].screenPos), glm::vec2(quadPts[3].screenPos));
                    float cp2 = cross2D(glm::vec2(quadPts[2].screenPos), glm::vec2(quadPts[3].screenPos), glm::vec2(quadPts[0].screenPos));
                    float cp3 = cross2D(glm::vec2(quadPts[3].screenPos), glm::vec2(quadPts[0].screenPos), glm::vec2(quadPts[1].screenPos));

                    bool allPos = (cp0 > 1.0f && cp1 > 1.0f && cp2 > 1.0f && cp3 > 1.0f);
                    bool allNeg = (cp0 < -1.0f && cp1 < -1.0f && cp2 < -1.0f && cp3 < -1.0f);
                    if (!allPos && !allNeg) continue;

                    float sign = allPos ? 1.0f : -1.0f;
                    float d0 = cross2D(glm::vec2(quadPts[0].screenPos), glm::vec2(quadPts[1].screenPos), cursor) * sign;
                    float d1 = cross2D(glm::vec2(quadPts[1].screenPos), glm::vec2(quadPts[2].screenPos), cursor) * sign;
                    float d2 = cross2D(glm::vec2(quadPts[2].screenPos), glm::vec2(quadPts[3].screenPos), cursor) * sign;
                    float d3 = cross2D(glm::vec2(quadPts[3].screenPos), glm::vec2(quadPts[0].screenPos), cursor) * sign;

                    if (d0 < 0.0f || d1 < 0.0f || d2 < 0.0f || d3 < 0.0f) continue;

                    if (hasExistingPolygon4(c0.index, c1.index, c2.index, c3.index)) continue;
                    if (sharesThreeOrMoreWithExisting(c0.index, c1.index, c2.index, c3.index)) continue;

                    if (countEdgePolys(quadPts[0].index, quadPts[2].index) > 0 ||
                        countEdgePolys(quadPts[1].index, quadPts[3].index) > 0) continue;

                    if (countEdgePolys(quadPts[0].index, quadPts[1].index) >= 2 ||
                        countEdgePolys(quadPts[1].index, quadPts[2].index) >= 2 ||
                        countEdgePolys(quadPts[2].index, quadPts[3].index) >= 2 ||
                        countEdgePolys(quadPts[3].index, quadPts[0].index) >= 2) continue;

                    glm::vec3 diag02 = quadPts[2].worldPos - quadPts[0].worldPos;
                    glm::vec3 diag13 = quadPts[3].worldPos - quadPts[1].worldPos;
                    glm::vec3 geomNormal = glm::cross(diag02, diag13);
                    float gnLen = glm::length(geomNormal);
                    if (gnLen > 1e-6f) geomNormal /= gnLen;

                    if (glm::dot(geomNormal, targetNormal) < 0.0f) {
                        std::swap(quadPts[1], quadPts[3]);
                        geomNormal = -geomNormal;
                    }

                    if (glm::dot(geomNormal, targetNormal) < 0.01f) continue;

                    float perimeter = glm::length(glm::vec2(quadPts[1].screenPos) - glm::vec2(quadPts[0].screenPos)) +
                                      glm::length(glm::vec2(quadPts[2].screenPos) - glm::vec2(quadPts[1].screenPos)) +
                                      glm::length(glm::vec2(quadPts[3].screenPos) - glm::vec2(quadPts[2].screenPos)) +
                                      glm::length(glm::vec2(quadPts[0].screenPos) - glm::vec2(quadPts[3].screenPos));
                    float distToCentroid = glm::length(centroid - cursor);
                    float score = perimeter + 2.0f * distToCentroid;

                    if (score < bestScore) {
                        bestScore = score;
                        result.valid = true;
                        result.v[0] = quadPts[0].index;
                        result.v[1] = quadPts[1].index;
                        result.v[2] = quadPts[2].index;
                        result.v[3] = quadPts[3].index;
                        result.worldPositions[0] = quadPts[0].worldPos;
                        result.worldPositions[1] = quadPts[1].worldPos;
                        result.worldPositions[2] = quadPts[2].worldPos;
                        result.worldPositions[3] = quadPts[3].worldPos;
                        result.screenPositions[0] = quadPts[0].screenPos;
                        result.screenPositions[1] = quadPts[1].screenPos;
                        result.screenPositions[2] = quadPts[2].screenPos;
                        result.screenPositions[3] = quadPts[3].screenPos;
                        result.normal = geomNormal;
                    }
                }
            }
        }
    }

    return result;
}

float QuadDrawMeshBuilder::ComputeEdgeParam(
    const Camera& camera, const Mesh* mesh, int32_t v0, int32_t v1, float screenX, float screenY
) {
    if (!mesh) return 0.5f;
    auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
        glm::vec3 p(mesh->verts[idx * 3], mesh->verts[idx * 3 + 1], mesh->verts[idx * 3 + 2]);
        return (mesh->matrix != glm::mat4(1.0f)) ? glm::vec3(mesh->matrix * glm::vec4(p, 1.0f)) : p;
    };
    glm::vec3 s0 = camera.project(getVertWorld(v0));
    glm::vec3 s1 = camera.project(getVertWorld(v1));
    glm::vec2 p0(s0.x, s0.y);
    glm::vec2 p1(s1.x, s1.y);
    glm::vec2 seg = p1 - p0;
    float l2 = glm::dot(seg, seg);
    if (l2 < 1e-6f) return 0.5f;
    float t = glm::dot(glm::vec2(screenX, screenY) - p0, seg) / l2;
    return std::clamp(t, 0.0f, 1.0f);
}

EdgeHit QuadDrawMeshBuilder::FindClosestEdgeOfPolygon(
    const Camera& camera, const Mesh* mesh, int32_t polyIdx, float screenX, float screenY
) {
    EdgeHit hit;
    if (!mesh || polyIdx < 0 || (size_t)polyIdx * 4 + 3 >= mesh->faces.size()) return hit;

    auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
        glm::vec3 p(mesh->verts[idx * 3], mesh->verts[idx * 3 + 1], mesh->verts[idx * 3 + 2]);
        return (mesh->matrix != glm::mat4(1.0f)) ? glm::vec3(mesh->matrix * glm::vec4(p, 1.0f)) : p;
    };

    uint32_t q[4] = {
        mesh->faces[polyIdx * 4 + 0],
        mesh->faces[polyIdx * 4 + 1],
        mesh->faces[polyIdx * 4 + 2],
        mesh->faces[polyIdx * 4 + 3]
    };
    int numEdges = (q[3] != TRI_INDEX) ? 4 : 3;

    float bestDist = 1e30f;
    for (int e = 0; e < numEdges; ++e) {
        int32_t u = q[e];
        int32_t v = q[(e + 1) % numEdges];
        glm::vec3 w0 = getVertWorld(u);
        glm::vec3 w1 = getVertWorld(v);
        glm::vec3 s0 = camera.project(w0);
        glm::vec3 s1 = camera.project(w1);
        if (s0.z <= 0.0f || s1.z <= 0.0f) continue;

        glm::vec2 p0(s0.x, s0.y);
        glm::vec2 p1(s1.x, s1.y);
        glm::vec2 seg = p1 - p0;
        float l2 = glm::dot(seg, seg);
        float t = 0.5f;
        float d = 1e30f;
        if (l2 < 1e-6f) {
            d = glm::length(glm::vec2(screenX, screenY) - p0);
        } else {
            t = std::clamp(glm::dot(glm::vec2(screenX, screenY) - p0, seg) / l2, 0.0f, 1.0f);
            d = glm::length(glm::vec2(screenX, screenY) - (p0 + t * seg));
        }

        if (d < bestDist) {
            bestDist = d;
            hit.valid = true;
            hit.v0 = u;
            hit.v1 = v;
            hit.worldPos0 = w0;
            hit.worldPos1 = w1;
            hit.t = t;
            hit.dist = d;
            hit.polyIndex = polyIdx;
        }
    }
    return hit;
}

EdgeCutResult QuadDrawMeshBuilder::FindEdgeLoopCut(
    const Mesh* retopo, const Mesh* targetMesh, SurfaceSnapper& snapper,
    const Camera& camera, int32_t startV0, int32_t startV1, float startT, int32_t hintPoly
) {
    EdgeCutResult result;
    if (!retopo || startV0 == startV1) return result;

    float t = std::clamp(startT, 0.02f, 0.98f);
    result.paramT = t;
    result.primaryV0 = startV0;
    result.primaryV1 = startV1;
    result.primaryPoly = hintPoly;

    auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
        glm::vec3 p(retopo->verts[idx * 3], retopo->verts[idx * 3 + 1], retopo->verts[idx * 3 + 2]);
        return (retopo->matrix != glm::mat4(1.0f)) ? glm::vec3(retopo->matrix * glm::vec4(p, 1.0f)) : p;
    };

    int32_t polyCount = (int32_t)(retopo->faces.size() / 4);

    // Initial cut point
    CutPoint cp;
    cp.v0 = std::min(startV0, startV1);
    cp.v1 = std::max(startV0, startV1);
    cp.t = (startV0 < startV1) ? t : (1.0f - t);
    glm::vec3 rawPos = glm::mix(getVertWorld(cp.v0), getVertWorld(cp.v1), cp.t);
    SnapResult snap = snapper.ProjectPointAlongNormal(targetMesh, rawPos, glm::vec3(0, 1, 0), 100.0f);
    cp.worldPos = snap.valid ? snap.worldPos : rawPos;
    result.cutPoints.push_back(cp);

    // Trace quads
    int32_t curPoly = hintPoly;
    if (curPoly == -1) {
        for (int p = 0; p < polyCount; ++p) {
            if (PolygonHasEdge(retopo->faces, p, startV0, startV1)) {
                curPoly = p;
                break;
            }
        }
    }

    if (curPoly != -1 && retopo->faces[curPoly * 4 + 3] != TRI_INDEX) {
        uint32_t q[4] = {
            retopo->faces[curPoly * 4 + 0],
            retopo->faces[curPoly * 4 + 1],
            retopo->faces[curPoly * 4 + 2],
            retopo->faces[curPoly * 4 + 3]
        };

        // Opposite edge across quad
        int32_t oppV0 = -1, oppV1 = -1;
        if ((q[0] == (uint32_t)startV0 && q[1] == (uint32_t)startV1) || (q[0] == (uint32_t)startV1 && q[1] == (uint32_t)startV0)) {
            oppV0 = q[3]; oppV1 = q[2];
        } else if ((q[1] == (uint32_t)startV0 && q[2] == (uint32_t)startV1) || (q[1] == (uint32_t)startV1 && q[2] == (uint32_t)startV0)) {
            oppV0 = q[0]; oppV1 = q[3];
        } else if ((q[2] == (uint32_t)startV0 && q[3] == (uint32_t)startV1) || (q[2] == (uint32_t)startV1 && q[3] == (uint32_t)startV0)) {
            oppV0 = q[1]; oppV1 = q[0];
        } else if ((q[3] == (uint32_t)startV0 && q[0] == (uint32_t)startV1) || (q[3] == (uint32_t)startV1 && q[0] == (uint32_t)startV0)) {
            oppV0 = q[2]; oppV1 = q[1];
        }

        if (oppV0 != -1 && oppV1 != -1) {
            CutPoint cpOpp;
            cpOpp.v0 = std::min(oppV0, oppV1);
            cpOpp.v1 = std::max(oppV0, oppV1);
            cpOpp.t = t;
            glm::vec3 rawOpp = glm::mix(getVertWorld(cpOpp.v0), getVertWorld(cpOpp.v1), cpOpp.t);
            SnapResult snapOpp = snapper.ProjectPointAlongNormal(targetMesh, rawOpp, glm::vec3(0, 1, 0), 100.0f);
            cpOpp.worldPos = snapOpp.valid ? snapOpp.worldPos : rawOpp;
            result.cutPoints.push_back(cpOpp);

            CutSegment seg;
            seg.p0 = cp.worldPos;
            seg.p1 = cpOpp.worldPos;
            seg.polyIndex = curPoly;
            result.cutSegments.push_back(seg);

            QuadSplitInfo qs;
            qs.polyIndex = curPoly;
            qs.cutPtA = 0;
            qs.cutPtB = 1;
            qs.q[0] = q[0]; qs.q[1] = q[1]; qs.q[2] = q[2]; qs.q[3] = q[3];
            result.quadSplits.push_back(qs);
        }
    }

    result.valid = !result.cutSegments.empty();
    return result;
}

bool QuadDrawMeshBuilder::ApplyEdgeLoopCut(Mesh* retopo, const EdgeCutResult& cutResult) {
    if (!retopo || !cutResult.valid || cutResult.quadSplits.empty()) return false;

    // Create vertices for cut points
    std::vector<int32_t> newVertIndices(cutResult.cutPoints.size());
    for (size_t i = 0; i < cutResult.cutPoints.size(); ++i) {
        newVertIndices[i] = AddVertex(retopo, cutResult.cutPoints[i].worldPos);
    }

    for (const auto& qs : cutResult.quadSplits) {
        int32_t newVA = newVertIndices[qs.cutPtA];
        int32_t newVB = newVertIndices[qs.cutPtB];
        if (newVA == -1 || newVB == -1) continue;

        // Split quad into two quads
        DeletePolygon(retopo, qs.polyIndex);
        AddQuad(retopo, qs.q[0], newVA, newVB, qs.q[3], glm::vec3(0, 1, 0));
        AddQuad(retopo, newVA, qs.q[1], qs.q[2], newVB, glm::vec3(0, 1, 0));
    }

    NotifyMeshUpdated(retopo);
    return true;
}

MultiCutResult QuadDrawMeshBuilder::BuildMultiCutFromPoints(
    const Camera& camera, const Mesh* retopo, const Mesh* targetMesh, SurfaceSnapper& snapper,
    const std::vector<MultiCutPoint>& points, const MultiCutPoint* candidateHover
) {
    MultiCutResult result;
    if (!retopo || points.empty()) return result;

    for (size_t i = 0; i + 1 < points.size(); ++i) {
        MultiCutSliceSegment seg;
        seg.p0 = points[i].worldPos;
        seg.p1 = points[i + 1].worldPos;
        result.previewSegments.push_back(seg);
    }

    if (candidateHover && !points.empty()) {
        MultiCutSliceSegment seg;
        seg.p0 = points.back().worldPos;
        seg.p1 = candidateHover->worldPos;
        result.previewSegments.push_back(seg);
    }

    result.valid = !result.previewSegments.empty();
    return result;
}

MultiCutResult QuadDrawMeshBuilder::BuildSliceCut(
    const Camera& camera, const Mesh* retopo, const Mesh* targetMesh, SurfaceSnapper& snapper,
    const glm::vec2& screenP0, const glm::vec2& screenP1, bool cutThrough, bool cutInfinite
) {
    MultiCutResult result;
    if (!retopo || retopo->nbFaces == 0) return result;

    MultiCutSliceSegment seg;
    seg.p0 = camera.unproject(screenP0.x, screenP0.y, 0.5f);
    seg.p1 = camera.unproject(screenP1.x, screenP1.y, 0.5f);
    result.previewSegments.push_back(seg);
    result.valid = true;
    return result;
}

bool QuadDrawMeshBuilder::ApplyPolygonCuts(
    Mesh* retopo, const Mesh* targetMesh, SurfaceSnapper& snapper, const std::vector<PolygonCut>& cuts
) {
    if (!retopo || cuts.empty()) return false;
    NotifyMeshUpdated(retopo);
    return true;
}

ExtrudeEdgeResult QuadDrawMeshBuilder::ExtrudeEdge(
    Mesh* mesh, int32_t v0, int32_t v1, const glm::vec3& pos0, const glm::vec3& pos1, const glm::vec3& targetNormal
) {
    ExtrudeEdgeResult result;
    if (!mesh || v0 == v1) return result;
    int32_t ptCount = (int32_t)(mesh->verts.size() / 3);
    int32_t polyCount = (int32_t)(mesh->faces.size() / 4);
    if (v0 < 0 || v0 >= ptCount || v1 < 0 || v1 >= ptCount) return result;

    int32_t edgePolyCount = 0;
    bool adjReversed = false;

    for (int32_t i = 0; i < polyCount; ++i) {
        if (PolygonHasEdge(mesh->faces, i, v0, v1)) {
            edgePolyCount++;
            uint32_t a = mesh->faces[i * 4 + 0];
            uint32_t b = mesh->faces[i * 4 + 1];
            uint32_t c = mesh->faces[i * 4 + 2];
            uint32_t d = mesh->faces[i * 4 + 3];
            bool isQuad = (d != TRI_INDEX);

            if ((a == (uint32_t)v1 && b == (uint32_t)v0) ||
                (b == (uint32_t)v1 && c == (uint32_t)v0) ||
                (isQuad && c == (uint32_t)v1 && d == (uint32_t)v0) ||
                (isQuad && d == (uint32_t)v1 && a == (uint32_t)v0) ||
                (!isQuad && c == (uint32_t)v1 && a == (uint32_t)v0)) {
                adjReversed = true;
            }
        }
    }

    if (edgePolyCount >= 2) return result; // Non-border edge

    int32_t newV0 = AddVertex(mesh, pos0);
    if (newV0 == -1) return result;
    int32_t newV1 = AddVertex(mesh, pos1);
    if (newV1 == -1) {
        DeleteVertex(mesh, newV0);
        return result;
    }

    int32_t q0, q1, q2, q3;
    if (edgePolyCount == 1) {
        if (adjReversed) {
            q0 = v0; q1 = v1; q2 = newV1; q3 = newV0;
        } else {
            q0 = v1; q1 = v0; q2 = newV0; q3 = newV1;
        }
    } else {
        q0 = v0; q1 = v1; q2 = newV1; q3 = newV0;
    }

    if (AddQuad(mesh, q0, q1, q2, q3, targetNormal)) {
        result.valid = true;
        result.newV0 = newV0;
        result.newV1 = newV1;
        result.newPoly = (int32_t)(mesh->faces.size() / 4) - 1;
    }

    return result;
}

bool QuadDrawMeshBuilder::IsBoundaryOrIsolatedVertex(const Mesh* mesh, int32_t ptIndex) {
    if (!mesh || ptIndex < 0 || (size_t)ptIndex * 3 + 2 >= mesh->verts.size()) return false;
    int32_t polyCount = (int32_t)(mesh->faces.size() / 4);

    std::vector<int32_t> neighbors;
    for (int32_t i = 0; i < polyCount; ++i) {
        uint32_t a = mesh->faces[i * 4 + 0];
        uint32_t b = mesh->faces[i * 4 + 1];
        uint32_t c = mesh->faces[i * 4 + 2];
        uint32_t d = mesh->faces[i * 4 + 3];
        bool isQuad = (d != TRI_INDEX);

        uint32_t v = (uint32_t)ptIndex;
        if (a == v) { neighbors.push_back(b); neighbors.push_back(isQuad ? d : c); }
        if (b == v) { neighbors.push_back(c); neighbors.push_back(a); }
        if (c == v) { neighbors.push_back(isQuad ? d : a); neighbors.push_back(b); }
        if (isQuad && d == v) { neighbors.push_back(a); neighbors.push_back(c); }
    }

    if (neighbors.empty()) return true; // Isolated

    std::sort(neighbors.begin(), neighbors.end());
    neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());

    for (int32_t n : neighbors) {
        int32_t edgeCnt = 0;
        for (int32_t i = 0; i < polyCount; ++i) {
            if (PolygonHasEdge(mesh->faces, i, ptIndex, n)) edgeCnt++;
        }
        if (edgeCnt == 1) return true; // Boundary edge found
    }

    return false;
}

bool QuadDrawMeshBuilder::IsCursorNearBorder(
    const Mesh* retopo, const Camera& camera, float screenX, float screenY,
    float brushRadius, const Mesh* target, SurfaceSnapper* snapper, bool visibleOnly
) {
    if (!retopo || retopo->nbVerts == 0) return false;
    int32_t ptCount = (int32_t)(retopo->verts.size() / 3);
    float rSq = brushRadius * brushRadius;

    auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
        glm::vec3 p(retopo->verts[idx * 3], retopo->verts[idx * 3 + 1], retopo->verts[idx * 3 + 2]);
        return (retopo->matrix != glm::mat4(1.0f)) ? glm::vec3(retopo->matrix * glm::vec4(p, 1.0f)) : p;
    };

    for (int i = 0; i < ptCount; ++i) {
        if (!IsBoundaryOrIsolatedVertex(retopo, i)) continue;
        glm::vec3 s = camera.project(getVertWorld(i));
        if (s.z <= 0.0f) continue;
        float dSq = (s.x - screenX) * (s.x - screenX) + (s.y - screenY) * (s.y - screenY);
        if (dSq <= rSq) return true;
    }
    return false;
}

bool QuadDrawMeshBuilder::RelaxVertices(
    Mesh* retopo, const Mesh* target, SurfaceSnapper& snapper,
    const Camera& camera, float screenX, float screenY, float brushRadius, float strength,
    bool lockBorder, bool lockInterior, bool visibleOnly,
    const std::unordered_set<uint32_t>* pinnedVertices
) {
    if (!retopo || retopo->nbVerts == 0 || strength <= 0.001f) return false;

    const int ptCount = (int32_t)(retopo->verts.size() / 3);
    const int polyCount = (int32_t)(retopo->faces.size() / 4);
    float rSq = brushRadius * brushRadius;

    auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
        glm::vec3 p(retopo->verts[idx * 3], retopo->verts[idx * 3 + 1], retopo->verts[idx * 3 + 2]);
        return (retopo->matrix != glm::mat4(1.0f)) ? glm::vec3(retopo->matrix * glm::vec4(p, 1.0f)) : p;
    };

    // Find affected vertices and their weights
    std::vector<float> weights(ptCount, 0.0f);
    bool anyAffected = false;

    for (int i = 0; i < ptCount; ++i) {
        if (pinnedVertices && pinnedVertices->count((uint32_t)i)) continue;

        bool isBorder = IsBoundaryOrIsolatedVertex(retopo, i);
        if (lockBorder && isBorder) continue;
        if (lockInterior && !isBorder) continue;

        glm::vec3 s = camera.project(getVertWorld(i));
        if (s.z <= 0.0f) continue;

        float dSq = (s.x - screenX) * (s.x - screenX) + (s.y - screenY) * (s.y - screenY);
        if (dSq <= rSq) {
            float dist = std::sqrt(dSq);
            float w = (1.0f - dist / brushRadius) * strength;
            weights[i] = std::clamp(w, 0.0f, 1.0f);
            anyAffected = true;
        }
    }

    if (!anyAffected) return false;

    // Build neighbors for affected vertices
    std::vector<std::vector<int32_t>> neighbors(ptCount);
    for (int p = 0; p < polyCount; ++p) {
        uint32_t q[4] = {
            retopo->faces[p * 4 + 0],
            retopo->faces[p * 4 + 1],
            retopo->faces[p * 4 + 2],
            retopo->faces[p * 4 + 3]
        };
        int numE = (q[3] != TRI_INDEX) ? 4 : 3;
        for (int e = 0; e < numE; ++e) {
            uint32_t u = q[e];
            uint32_t v = q[(e + 1) % numE];
            neighbors[u].push_back((int32_t)v);
            neighbors[v].push_back((int32_t)u);
        }
    }

    // Laplacian step
    for (int i = 0; i < ptCount; ++i) {
        if (weights[i] <= 0.0f || neighbors[i].empty()) continue;

        glm::vec3 centroid(0.0f);
        int validNeighs = 0;
        for (int32_t n : neighbors[i]) {
            centroid += getVertWorld(n);
            validNeighs++;
        }
        if (validNeighs == 0) continue;
        centroid /= (float)validNeighs;

        glm::vec3 origPos = getVertWorld(i);
        glm::vec3 smoothed = glm::mix(origPos, centroid, weights[i]);

        // Snap back to reference target surface
        if (target && target->nbFaces > 0) {
            SnapResult snap = snapper.ProjectPointAlongNormal(target, smoothed, glm::vec3(0, 1, 0), 100.0f);
            if (snap.valid) smoothed = snap.worldPos;
        }

        SetVertexPosition(retopo, i, smoothed);
    }

    NotifyMeshUpdated(retopo);
    return true;
}

bool QuadDrawMeshBuilder::CollectGrabVertices(
    const Mesh* retopo, const Mesh* target, SurfaceSnapper& snapper,
    const Camera& camera, float screenX, float screenY, float brushRadius, float intensity,
    int32_t falloffType, bool visibleOnly,
    std::vector<GrabVertexInfo>& outVertices, glm::vec3& outGrabCenterWorld,
    const std::unordered_set<uint32_t>* pinnedVertices
) {
    outVertices.clear();
    if (!retopo || retopo->nbVerts == 0) return false;

    const int ptCount = (int32_t)(retopo->verts.size() / 3);
    float rSq = brushRadius * brushRadius;

    auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
        glm::vec3 p(retopo->verts[idx * 3], retopo->verts[idx * 3 + 1], retopo->verts[idx * 3 + 2]);
        return (retopo->matrix != glm::mat4(1.0f)) ? glm::vec3(retopo->matrix * glm::vec4(p, 1.0f)) : p;
    };

    glm::vec3 sumWorld(0.0f);
    float totalW = 0.0f;

    for (int i = 0; i < ptCount; ++i) {
        if (pinnedVertices && pinnedVertices->count((uint32_t)i)) continue;

        glm::vec3 wPos = getVertWorld(i);
        glm::vec3 sPos = camera.project(wPos);
        if (sPos.z <= 0.0f) continue;

        float dx = sPos.x - screenX;
        float dy = sPos.y - screenY;
        float dSq = dx * dx + dy * dy;

        if (dSq <= rSq) {
            float dist = std::sqrt(dSq);
            float normDist = dist / brushRadius;
            float w = 1.0f - normDist;

            // Falloff types: 0=Hermite, 1=Linear, 2=Dome, 3=Spike
            if (falloffType == 0) {
                w = w * w * (3.0f - 2.0f * w);
            } else if (falloffType == 2) {
                w = std::sqrt(std::max(0.0f, 1.0f - normDist * normDist));
            } else if (falloffType == 3) {
                w = w * w * w;
            }
            w = std::clamp(w * intensity, 0.0f, 1.0f);

            GrabVertexInfo gv;
            gv.index = i;
            gv.initLocalPos = glm::vec3(retopo->verts[i * 3], retopo->verts[i * 3 + 1], retopo->verts[i * 3 + 2]);
            gv.initWorldPos = wPos;
            gv.initScreenPos = sPos;
            gv.weight = w;

            outVertices.push_back(gv);
            sumWorld += wPos * w;
            totalW += w;
        }
    }

    if (totalW > 1e-6f) {
        outGrabCenterWorld = sumWorld / totalW;
    } else {
        outGrabCenterWorld = camera.unproject(screenX, screenY, 0.5f);
    }

    return !outVertices.empty();
}

} // namespace retopo
