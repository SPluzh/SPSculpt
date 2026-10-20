#include "retopo/QuadDrawMeshBuilder.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
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
            if (match >= 4) return false;
        } else {
            if (match >= 3) return false;
        }
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
    if (!mesh || vSource == vTarget || vSource < 0 || vTarget < 0) return false;
    return WeldVerticesMultiple(mesh, {{vSource, vTarget}}, pinnedVertices);
}

bool QuadDrawMeshBuilder::WeldVerticesMultiple(
    Mesh* mesh,
    const std::vector<std::pair<int32_t, int32_t>>& welds,
    std::unordered_set<uint32_t>* pinnedVertices,
    std::vector<int32_t>* outRemap
) {
    if (!mesh || welds.empty()) return false;
    int32_t ptCount = (int32_t)(mesh->verts.size() / 3);
    int32_t polyCount = (int32_t)(mesh->faces.size() / 4);

    std::unordered_map<int32_t, int32_t> remap;
    auto findRoot = [&](int32_t x) -> int32_t {
        int32_t r = x;
        while (remap.count(r)) {
            r = remap[r];
        }
        int32_t curr = x;
        while (remap.count(curr)) {
            int32_t nxt = remap[curr];
            remap[curr] = r;
            curr = nxt;
        }
        return r;
    };

    for (const auto& w : welds) {
        int32_t src = w.first;
        int32_t tgt = w.second;
        if (src < 0 || src >= ptCount || tgt < 0 || tgt >= ptCount) continue;
        if (src == tgt) continue;
        int32_t rSrc = findRoot(src);
        int32_t rTgt = findRoot(tgt);
        if (rSrc != rTgt) {
            remap[rSrc] = rTgt;
        }
    }

    if (remap.empty()) return false;

    // 1. Remap all polygon vertices
    for (int32_t i = 0; i < polyCount; ++i) {
        for (int k = 0; k < 4; ++k) {
            int32_t v = (int32_t)mesh->faces[i * 4 + k];
            if (v != (int32_t)TRI_INDEX && remap.count(v)) {
                mesh->faces[i * 4 + k] = (uint32_t)findRoot(v);
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
            if (a == b || b == c || a == c) continue;
        }

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

    if (pinnedVertices) {
        for (const auto& kv : remap) {
            int32_t src = kv.first;
            int32_t tgt = findRoot(src);
            if (pinnedVertices->count((uint32_t)src)) {
                pinnedVertices->insert((uint32_t)tgt);
            }
        }
    }

    // 3. Delete all unique welded sources in descending order
    std::vector<int32_t> toDelete;
    for (const auto& kv : remap) {
        toDelete.push_back(kv.first);
    }
    std::sort(toDelete.begin(), toDelete.end(), std::greater<int32_t>());
    toDelete.erase(std::unique(toDelete.begin(), toDelete.end()), toDelete.end());

    std::vector<int32_t> currentMap(ptCount);
    for (int32_t i = 0; i < ptCount; ++i) {
        currentMap[i] = remap.count(i) ? findRoot(i) : i;
    }

    for (int32_t delIdx : toDelete) {
        DeleteVertex(mesh, delIdx, pinnedVertices);
        for (int32_t& m : currentMap) {
            if (m > delIdx) {
                m--;
            } else if (m == delIdx) {
                m = -1;
            }
        }
    }

    if (outRemap) {
        *outRemap = std::move(currentMap);
    }

    NotifyMeshUpdated(mesh);
    return true;
}

bool QuadDrawMeshBuilder::WeldVerticesPair(
    Mesh* mesh, int32_t src0, int32_t tgt0, int32_t src1, int32_t tgt1, std::unordered_set<uint32_t>* pinnedVertices
) {
    std::vector<std::pair<int32_t, int32_t>> welds;
    if (src0 != -1 && tgt0 != -1 && src0 != tgt0) welds.push_back({src0, tgt0});
    if (src1 != -1 && tgt1 != -1 && src1 != tgt1) welds.push_back({src1, tgt1});
    if (welds.empty()) return false;
    return WeldVerticesMultiple(mesh, welds, pinnedVertices);
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

bool QuadDrawMeshBuilder::DeleteEdges(
    Mesh* mesh, const std::vector<std::pair<int32_t, int32_t>>& edges, std::unordered_set<uint32_t>* pinnedVertices
) {
    if (!mesh || edges.empty()) return false;
    int32_t polyCount = (int32_t)(mesh->faces.size() / 4);

    std::unordered_set<int32_t> adjPolys;
    for (const auto& e : edges) {
        for (int32_t i = 0; i < polyCount; ++i) {
            if (PolygonHasEdge(mesh->faces, i, e.first, e.second)) {
                adjPolys.insert(i);
            }
        }
    }

    if (!adjPolys.empty()) {
        std::vector<int32_t> polyList(adjPolys.begin(), adjPolys.end());
        return DeletePolygons(mesh, polyList, pinnedVertices);
    }

    // Fallback if edges were unconnected: delete their vertices
    std::unordered_set<int32_t> verts;
    for (const auto& e : edges) {
        verts.insert(e.first);
        verts.insert(e.second);
    }
    std::vector<int32_t> vertList(verts.begin(), verts.end());
    std::sort(vertList.begin(), vertList.end(), std::greater<int32_t>());
    for (int32_t v : vertList) {
        DeleteVertex(mesh, v, pinnedVertices);
    }
    return true;
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
    if (!retopo || startV0 == startV1 || retopo->nbFaces == 0 || retopo->nbVerts == 0) return result;

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

    struct EdgeKey {
        int32_t u, v;
        bool operator<(const EdgeKey& o) const {
            if (u != o.u) return u < o.u;
            return v < o.v;
        }
    };
    auto makeKey = [](int32_t a, int32_t b) -> EdgeKey {
        return { std::min(a, b), std::max(a, b) };
    };

    std::map<EdgeKey, std::vector<int32_t>> edgeToQuads;
    for (int32_t p = 0; p < polyCount; ++p) {
        if (retopo->faces[p * 4 + 3] == TRI_INDEX) continue;
        uint32_t f[4] = {
            retopo->faces[p * 4 + 0],
            retopo->faces[p * 4 + 1],
            retopo->faces[p * 4 + 2],
            retopo->faces[p * 4 + 3]
        };
        for (int k = 0; k < 4; ++k) {
            int32_t u = (int32_t)f[k];
            int32_t v = (int32_t)f[(k + 1) % 4];
            edgeToQuads[makeKey(u, v)].push_back(p);
        }
    }

    EdgeKey startKey = makeKey(startV0, startV1);
    auto itStart = edgeToQuads.find(startKey);
    if (itStart == edgeToQuads.end() || itStart->second.empty()) {
        return result;
    }

    std::vector<int32_t> incidentPolys = itStart->second;
    if (hintPoly != -1) {
        auto itHint = std::find(incidentPolys.begin(), incidentPolys.end(), hintPoly);
        if (itHint != incidentPolys.end() && itHint != incidentPolys.begin()) {
            std::swap(incidentPolys[0], *itHint);
        }
    }

    std::map<EdgeKey, int32_t> edgeCutMap;

    auto getOrCreateEdgeCut = [&](int32_t u, int32_t v, float paramFromU) -> int32_t {
        EdgeKey ek = makeKey(u, v);
        auto it = edgeCutMap.find(ek);
        if (it != edgeCutMap.end()) {
            return it->second;
        }
        CutPoint cp;
        cp.v0 = ek.u;
        cp.v1 = ek.v;
        float tNorm = (u == ek.u) ? paramFromU : (1.0f - paramFromU);
        cp.t = std::clamp(tNorm, 0.02f, 0.98f);
        glm::vec3 rawPos = glm::mix(getVertWorld(cp.v0), getVertWorld(cp.v1), cp.t);
        SnapResult snap = snapper.SnapClosestPoint(targetMesh, rawPos, 100.0f);
        cp.worldPos = snap.valid ? snap.worldPos : rawPos;
        int32_t idx = (int32_t)result.cutPoints.size();
        result.cutPoints.push_back(cp);
        edgeCutMap[ek] = idx;
        return idx;
    };

    getOrCreateEdgeCut(startV0, startV1, t);

    std::unordered_set<int32_t> visitedPolys;

    auto traceDirection = [&](int32_t initPoly, int32_t initU, int32_t initV, float initT) {
        int32_t curPoly = initPoly;
        int32_t curU = initU;
        int32_t curV = initV;
        float curT = initT;
        int maxSteps = polyCount + 2;

        while (curPoly != -1 && --maxSteps >= 0) {
            if (visitedPolys.count(curPoly)) {
                result.isClosed = true;
                break;
            }
            if ((size_t)curPoly * 4 + 3 >= retopo->faces.size()) break;
            if (retopo->faces[curPoly * 4 + 3] == TRI_INDEX) break;

            visitedPolys.insert(curPoly);

            uint32_t f[4] = {
                retopo->faces[curPoly * 4 + 0],
                retopo->faces[curPoly * 4 + 1],
                retopo->faces[curPoly * 4 + 2],
                retopo->faces[curPoly * 4 + 3]
            };

            int kEntry = -1;
            bool entryReversed = false;
            for (int k = 0; k < 4; ++k) {
                int32_t a = (int32_t)f[k];
                int32_t b = (int32_t)f[(k + 1) % 4];
                if (a == curU && b == curV) {
                    kEntry = k;
                    entryReversed = false;
                    break;
                } else if (a == curV && b == curU) {
                    kEntry = k;
                    entryReversed = true;
                    break;
                }
            }
            if (kEntry == -1) break;

            int32_t q0 = (int32_t)f[kEntry];
            int32_t q1 = (int32_t)f[(kEntry + 1) % 4];
            int32_t q2 = (int32_t)f[(kEntry + 2) % 4];
            int32_t q3 = (int32_t)f[(kEntry + 3) % 4];

            float tAlongQ0Q1 = entryReversed ? (1.0f - curT) : curT;

            int32_t cutPtA = getOrCreateEdgeCut(q0, q1, tAlongQ0Q1);
            int32_t cutPtB = getOrCreateEdgeCut(q3, q2, tAlongQ0Q1);

            QuadSplitInfo qs;
            qs.polyIndex = curPoly;
            qs.cutPtA = cutPtA;
            qs.cutPtB = cutPtB;
            qs.q[0] = q0;
            qs.q[1] = q1;
            qs.q[2] = q2;
            qs.q[3] = q3;
            result.quadSplits.push_back(qs);

            CutSegment seg;
            seg.p0 = result.cutPoints[cutPtA].worldPos;
            seg.p1 = result.cutPoints[cutPtB].worldPos;
            seg.polyIndex = curPoly;
            result.cutSegments.push_back(seg);

            EdgeKey exitKey = makeKey(q3, q2);
            int32_t nextPoly = -1;
            auto itExit = edgeToQuads.find(exitKey);
            if (itExit != edgeToQuads.end()) {
                for (int32_t pCand : itExit->second) {
                    if (pCand != curPoly) {
                        nextPoly = pCand;
                        break;
                    }
                }
            }

            if (nextPoly == -1) {
                break;
            }

            curPoly = nextPoly;
            curU = q3;
            curV = q2;
            curT = tAlongQ0Q1;
        }
    };

    traceDirection(incidentPolys[0], startV0, startV1, t);

    if (!result.isClosed && incidentPolys.size() > 1 && !visitedPolys.count(incidentPolys[1])) {
        traceDirection(incidentPolys[1], startV0, startV1, t);
    }

    result.valid = !result.cutSegments.empty();
    return result;
}

bool QuadDrawMeshBuilder::ApplyEdgeLoopCut(Mesh* retopo, const EdgeCutResult& cutResult) {
    if (!retopo || !cutResult.valid || cutResult.quadSplits.empty()) return false;
    if (retopo->nbFaces == 0 || retopo->nbVerts == 0) return false;

    std::vector<int32_t> newVertIndices(cutResult.cutPoints.size());
    for (size_t i = 0; i < cutResult.cutPoints.size(); ++i) {
        newVertIndices[i] = AddVertex(retopo, cutResult.cutPoints[i].worldPos);
    }

    for (const auto& qs : cutResult.quadSplits) {
        if (qs.cutPtA < 0 || qs.cutPtA >= (int32_t)newVertIndices.size()) continue;
        if (qs.cutPtB < 0 || qs.cutPtB >= (int32_t)newVertIndices.size()) continue;

        int32_t newVA = newVertIndices[qs.cutPtA];
        int32_t newVB = newVertIndices[qs.cutPtB];
        if (newVA == -1 || newVB == -1) continue;

        int32_t p = qs.polyIndex;
        if (p < 0 || (size_t)p * 4 + 3 >= retopo->faces.size()) continue;

        uint32_t q1[4] = { (uint32_t)qs.q[0], (uint32_t)newVA, (uint32_t)newVB, (uint32_t)qs.q[3] };
        uint32_t q2[4] = { (uint32_t)newVA, (uint32_t)qs.q[1], (uint32_t)qs.q[2], (uint32_t)newVB };

        retopo->faces[p * 4 + 0] = q1[0];
        retopo->faces[p * 4 + 1] = q1[1];
        retopo->faces[p * 4 + 2] = q1[2];
        retopo->faces[p * 4 + 3] = q1[3];

        uint32_t grp = (p < (int32_t)retopo->faceGroups.size()) ? retopo->faceGroups[p] : 1;
        retopo->faces.push_back(q2[0]);
        retopo->faces.push_back(q2[1]);
        retopo->faces.push_back(q2[2]);
        retopo->faces.push_back(q2[3]);
        if (!retopo->faceGroups.empty()) {
            retopo->faceGroups.push_back(grp);
        }
    }

    NotifyMeshUpdated(retopo);
    return true;
}

// Helper for finding edge intersections with a 2D screen line segment
struct ScreenLineEdgeHit {
    float s = 0.0f; // Parameter along line [0, 1]
    int32_t edgeV0 = -1;
    int32_t edgeV1 = -1;
    float edgeT = 0.5f; // Parameter along edge [0, 1]
    glm::vec3 worldPos{0.0f};
    std::vector<int32_t> polys; // Adjacent polygons sharing this edge
};

static std::vector<ScreenLineEdgeHit> FindScreenLineEdgeIntersections(
    const Camera& camera, const Mesh* retopo, const Mesh* targetMesh, SurfaceSnapper& snapper,
    const glm::vec2& screenP0, const glm::vec2& screenP1, bool cutThrough = false
) {
    std::vector<ScreenLineEdgeHit> intersections;
    if (!retopo || retopo->nbFaces == 0 || retopo->nbVerts == 0) return intersections;

    float lineLen = glm::distance(screenP0, screenP1);
    if (lineLen < 2.0f) return intersections;

    auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
        glm::vec3 p(retopo->verts[idx * 3], retopo->verts[idx * 3 + 1], retopo->verts[idx * 3 + 2]);
        return (retopo->matrix != glm::mat4(1.0f)) ? glm::vec3(retopo->matrix * glm::vec4(p, 1.0f)) : p;
    };

    int32_t polyCount = (int32_t)(retopo->faces.size() / 4);
    int32_t vertCount = (int32_t)(retopo->verts.size() / 3);

    struct EdgeKey {
        int32_t u, v;
        bool operator<(const EdgeKey& o) const {
            if (u != o.u) return u < o.u;
            return v < o.v;
        }
    };
    std::map<EdgeKey, std::vector<int32_t>> edgeToPolys;
    for (int32_t p = 0; p < polyCount; ++p) {
        uint32_t f[4] = {
            retopo->faces[p * 4 + 0],
            retopo->faces[p * 4 + 1],
            retopo->faces[p * 4 + 2],
            retopo->faces[p * 4 + 3]
        };
        int kMax = (f[3] != TRI_INDEX) ? 4 : 3;
        for (int k = 0; k < kMax; ++k) {
            int32_t u = (int32_t)f[k];
            int32_t v = (int32_t)f[(k + 1) % kMax];
            if (u >= vertCount || v >= vertCount || u == v) continue;
            int32_t e0 = std::min(u, v);
            int32_t e1 = std::max(u, v);
            edgeToPolys[{e0, e1}].push_back(p);
        }
    }

    glm::vec2 dL = screenP1 - screenP0;
    glm::vec3 camPos = camera.computePosition();

    for (const auto& kv : edgeToPolys) {
        int32_t u = kv.first.u;
        int32_t v = kv.first.v;
        glm::vec3 wU = getVertWorld(u);
        glm::vec3 wV = getVertWorld(v);

        glm::vec3 sU = camera.project(wU);
        glm::vec3 sV = camera.project(wV);
        if (sU.z <= 0.0f || sV.z <= 0.0f) continue;

        if (!cutThrough) {
            bool anyFront = false;
            for (int32_t pIdx : kv.second) {
                uint32_t f[4] = {
                    retopo->faces[pIdx * 4 + 0],
                    retopo->faces[pIdx * 4 + 1],
                    retopo->faces[pIdx * 4 + 2],
                    retopo->faces[pIdx * 4 + 3]
                };
                if (f[0] >= (uint32_t)vertCount || f[1] >= (uint32_t)vertCount || f[2] >= (uint32_t)vertCount) continue;
                glm::vec3 p0 = getVertWorld(f[0]);
                glm::vec3 p1 = getVertWorld(f[1]);
                glm::vec3 p2 = getVertWorld(f[2]);
                glm::vec3 faceNorm = glm::cross(p1 - p0, p2 - p0);
                glm::vec3 viewDir = ((p0 + p1 + p2) * (1.0f / 3.0f)) - camPos;
                if (glm::dot(faceNorm, viewDir) < 0.0f) {
                    anyFront = true;
                    break;
                }
            }
            if (!anyFront) continue;
        }

        glm::vec2 pU(sU.x, sU.y);
        glm::vec2 pV(sV.x, sV.y);
        glm::vec2 dE = pV - pU;

        float denom = dL.x * dE.y - dL.y * dE.x;
        if (std::abs(denom) < 1e-6f) continue;

        glm::vec2 d0 = pU - screenP0;
        float s = (d0.x * dE.y - d0.y * dE.x) / denom;
        float t = (d0.x * dL.y - d0.y * dL.x) / denom;

        if (s >= 0.0f && s <= 1.0f && t >= 0.0f && t <= 1.0f) {
            ScreenLineEdgeHit hit;
            hit.s = s;
            hit.edgeV0 = u;
            hit.edgeV1 = v;
            hit.edgeT = t;
            glm::vec3 rawWorld = glm::mix(wU, wV, t);
            SnapResult snap = snapper.SnapClosestPoint(targetMesh, rawWorld, 100.0f);
            hit.worldPos = snap.valid ? snap.worldPos : rawWorld;
            hit.polys = kv.second;
            intersections.push_back(hit);
        }
    }

    std::sort(intersections.begin(), intersections.end(), [](const auto& a, const auto& b) {
        return a.s < b.s;
    });

    std::vector<ScreenLineEdgeHit> uniqueHits;
    for (const auto& hit : intersections) {
        if (!uniqueHits.empty()) {
            const auto& prev = uniqueHits.back();
            if (prev.edgeV0 == hit.edgeV0 && prev.edgeV1 == hit.edgeV1) continue;
            if (std::abs(prev.s - hit.s) < 1e-4f) continue;
        }
        uniqueHits.push_back(hit);
    }

    return uniqueHits;
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

    if (points.size() < 2) {
        result.valid = !result.previewSegments.empty();
        return result;
    }

    auto pointInPoly = [&](const MultiCutPoint& pt, int32_t pIdx) -> bool {
        if (pIdx < 0 || (size_t)pIdx * 4 + 3 >= retopo->faces.size()) return false;
        if (pt.type == MultiCutSnapType::Vertex) {
            return PolygonHasVertex(retopo->faces, pIdx, pt.vertexIdx);
        }
        if (pt.type == MultiCutSnapType::Edge) {
            return PolygonHasEdge(retopo->faces, pIdx, pt.edgeV0, pt.edgeV1);
        }
        if (pt.type == MultiCutSnapType::Face) {
            return pt.polyIndex == pIdx;
        }
        return false;
    };

    int32_t polyCount = (int32_t)(retopo->faces.size() / 4);

    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const auto& pA = points[i];
        const auto& pB = points[i + 1];

        // 1. Direct shared polygon cut
        int32_t sharedPoly = -1;
        for (int32_t p = 0; p < polyCount; ++p) {
            if (pointInPoly(pA, p) && pointInPoly(pB, p)) {
                sharedPoly = p;
                break;
            }
        }

        if (sharedPoly != -1) {
            bool sameVert = (pA.type == MultiCutSnapType::Vertex && pB.type == MultiCutSnapType::Vertex && pA.vertexIdx == pB.vertexIdx);
            bool sameEdge = (pA.type == MultiCutSnapType::Edge && pB.type == MultiCutSnapType::Edge &&
                std::min(pA.edgeV0, pA.edgeV1) == std::min(pB.edgeV0, pB.edgeV1) &&
                std::max(pA.edgeV0, pA.edgeV1) == std::max(pB.edgeV0, pB.edgeV1));
            if (!sameVert && !sameEdge) {
                PolygonCut cut;
                cut.polyIndex = sharedPoly;
                cut.end0IsVertex = (pA.type == MultiCutSnapType::Vertex);
                cut.end0Vertex = pA.vertexIdx;
                cut.end0EdgeV0 = pA.edgeV0;
                cut.end0EdgeV1 = pA.edgeV1;
                cut.end0EdgeT = pA.edgeT;
                cut.end0WorldPos = pA.worldPos;

                cut.end1IsVertex = (pB.type == MultiCutSnapType::Vertex);
                cut.end1Vertex = pB.vertexIdx;
                cut.end1EdgeV0 = pB.edgeV0;
                cut.end1EdgeV1 = pB.edgeV1;
                cut.end1EdgeT = pB.edgeT;
                cut.end1WorldPos = pB.worldPos;

                result.cuts.push_back(cut);
            }
        } else {
            // 2. Multi-polygon screen line tracing
            glm::vec3 sA = camera.project(pA.worldPos);
            glm::vec3 sB = camera.project(pB.worldPos);
            if (sA.z > 0.0f && sB.z > 0.0f) {
                auto hits = FindScreenLineEdgeIntersections(
                    camera, retopo, targetMesh, snapper, glm::vec2(sA), glm::vec2(sB), false
                );

                struct TraceNode {
                    bool isVertex;
                    int32_t vertIdx;
                    int32_t edgeV0, edgeV1;
                    float edgeT;
                    glm::vec3 worldPos;
                    std::vector<int32_t> polys;
                };

                std::vector<TraceNode> nodes;
                TraceNode nodeA;
                nodeA.isVertex = (pA.type == MultiCutSnapType::Vertex);
                nodeA.vertIdx = pA.vertexIdx;
                nodeA.edgeV0 = pA.edgeV0; nodeA.edgeV1 = pA.edgeV1; nodeA.edgeT = pA.edgeT;
                nodeA.worldPos = pA.worldPos;
                for (int32_t p = 0; p < polyCount; ++p) {
                    if (pointInPoly(pA, p)) nodeA.polys.push_back(p);
                }
                nodes.push_back(nodeA);

                for (const auto& h : hits) {
                    if (h.s > 0.03f && h.s < 0.97f) {
                        TraceNode n;
                        n.isVertex = false;
                        n.vertIdx = -1;
                        n.edgeV0 = h.edgeV0; n.edgeV1 = h.edgeV1; n.edgeT = h.edgeT;
                        n.worldPos = h.worldPos;
                        n.polys = h.polys;
                        nodes.push_back(n);
                    }
                }

                TraceNode nodeB;
                nodeB.isVertex = (pB.type == MultiCutSnapType::Vertex);
                nodeB.vertIdx = pB.vertexIdx;
                nodeB.edgeV0 = pB.edgeV0; nodeB.edgeV1 = pB.edgeV1; nodeB.edgeT = pB.edgeT;
                nodeB.worldPos = pB.worldPos;
                for (int32_t p = 0; p < polyCount; ++p) {
                    if (pointInPoly(pB, p)) nodeB.polys.push_back(p);
                }
                nodes.push_back(nodeB);

                for (size_t n = 0; n + 1 < nodes.size(); ++n) {
                    const auto& n0 = nodes[n];
                    const auto& n1 = nodes[n + 1];

                    int32_t commonPoly = -1;
                    for (int32_t p0 : n0.polys) {
                        for (int32_t p1 : n1.polys) {
                            if (p0 == p1) {
                                commonPoly = p0;
                                break;
                            }
                        }
                        if (commonPoly != -1) break;
                    }

                    if (commonPoly != -1) {
                        PolygonCut cut;
                        cut.polyIndex = commonPoly;
                        cut.end0IsVertex = n0.isVertex;
                        cut.end0Vertex = n0.vertIdx;
                        cut.end0EdgeV0 = n0.edgeV0; cut.end0EdgeV1 = n0.edgeV1; cut.end0EdgeT = n0.edgeT;
                        cut.end0WorldPos = n0.worldPos;

                        cut.end1IsVertex = n1.isVertex;
                        cut.end1Vertex = n1.vertIdx;
                        cut.end1EdgeV0 = n1.edgeV0; cut.end1EdgeV1 = n1.edgeV1; cut.end1EdgeT = n1.edgeT;
                        cut.end1WorldPos = n1.worldPos;

                        result.cuts.push_back(cut);
                    }
                }
            }
        }
    }

    result.valid = !result.cuts.empty() || !result.previewSegments.empty();
    return result;
}

MultiCutResult QuadDrawMeshBuilder::BuildSliceCut(
    const Camera& camera, const Mesh* retopo, const Mesh* targetMesh, SurfaceSnapper& snapper,
    const glm::vec2& screenP0, const glm::vec2& screenP1, bool cutThrough, bool cutInfinite
) {
    MultiCutResult result;
    if (!retopo || retopo->nbFaces == 0 || retopo->nbVerts == 0) return result;

    auto hits = FindScreenLineEdgeIntersections(camera, retopo, targetMesh, snapper, screenP0, screenP1, cutThrough);
    if (hits.size() < 2) return result;

    for (size_t i = 0; i + 1 < hits.size(); ++i) {
        const auto& hA = hits[i];
        const auto& hB = hits[i + 1];

        int32_t sharedPoly = -1;
        for (int32_t pA : hA.polys) {
            for (int32_t pB : hB.polys) {
                if (pA == pB) {
                    sharedPoly = pA;
                    break;
                }
            }
            if (sharedPoly != -1) break;
        }

        if (sharedPoly != -1) {
            PolygonCut cut;
            cut.polyIndex = sharedPoly;
            cut.end0IsVertex = false;
            cut.end0Vertex = -1;
            cut.end0EdgeV0 = hA.edgeV0;
            cut.end0EdgeV1 = hA.edgeV1;
            cut.end0EdgeT = hA.edgeT;
            cut.end0WorldPos = hA.worldPos;

            cut.end1IsVertex = false;
            cut.end1Vertex = -1;
            cut.end1EdgeV0 = hB.edgeV0;
            cut.end1EdgeV1 = hB.edgeV1;
            cut.end1EdgeT = hB.edgeT;
            cut.end1WorldPos = hB.worldPos;

            result.cuts.push_back(cut);

            MultiCutSliceSegment seg;
            seg.p0 = hA.worldPos;
            seg.p1 = hB.worldPos;
            seg.polyIndex = sharedPoly;
            result.previewSegments.push_back(seg);
        }
    }

    result.valid = !result.cuts.empty();
    return result;
}

bool QuadDrawMeshBuilder::ApplyPolygonCuts(
    Mesh* retopo, const Mesh* targetMesh, SurfaceSnapper& snapper, const std::vector<PolygonCut>& cuts
) {
    if (!retopo || cuts.empty() || retopo->nbFaces == 0) return false;

    int32_t originalPolyCount = (int32_t)(retopo->faces.size() / 4);

    struct EdgeKey {
        int32_t u, v;
        bool operator<(const EdgeKey& o) const {
            if (u != o.u) return u < o.u;
            return v < o.v;
        }
    };
    std::map<EdgeKey, int32_t> edgeNewVerts;

    auto getOrCreateCutVertex = [&](bool isVertex, int32_t vertIdx, int32_t edgeV0, int32_t edgeV1, float edgeT, const glm::vec3& worldPos) -> int32_t {
        if (isVertex) {
            if (vertIdx >= 0 && vertIdx < (int32_t)(retopo->verts.size() / 3)) {
                return vertIdx;
            }
            return -1;
        }

        if (edgeV0 < 0 || edgeV1 < 0 || edgeV0 == edgeV1) {
            glm::vec3 pos = worldPos;
            SnapResult snap = snapper.SnapClosestPoint(targetMesh, pos, 100.0f);
            if (snap.valid) pos = snap.worldPos;
            return AddVertex(retopo, pos);
        }

        if (edgeT <= 0.005f) return edgeV0;
        if (edgeT >= 0.995f) return edgeV1;

        int32_t e0 = std::min(edgeV0, edgeV1);
        int32_t e1 = std::max(edgeV0, edgeV1);
        auto it = edgeNewVerts.find({e0, e1});
        if (it != edgeNewVerts.end()) {
            return it->second;
        }

        glm::vec3 pos = worldPos;
        SnapResult snap = snapper.SnapClosestPoint(targetMesh, pos, 100.0f);
        if (snap.valid) pos = snap.worldPos;
        int32_t newV = AddVertex(retopo, pos);
        edgeNewVerts[{e0, e1}] = newV;
        return newV;
    };

    std::unordered_set<int32_t> processedPolys;
    bool anyApplied = false;

    for (const auto& cut : cuts) {
        int32_t p = cut.polyIndex;
        if (p < 0 || p >= originalPolyCount || (size_t)p * 4 + 3 >= retopo->faces.size()) continue;
        if (processedPolys.count(p)) continue;

        int32_t vA = getOrCreateCutVertex(cut.end0IsVertex, cut.end0Vertex, cut.end0EdgeV0, cut.end0EdgeV1, cut.end0EdgeT, cut.end0WorldPos);
        int32_t vB = getOrCreateCutVertex(cut.end1IsVertex, cut.end1Vertex, cut.end1EdgeV0, cut.end1EdgeV1, cut.end1EdgeT, cut.end1WorldPos);

        if (vA < 0 || vB < 0 || vA == vB) continue;

        uint32_t f[4] = {
            retopo->faces[p * 4 + 0],
            retopo->faces[p * 4 + 1],
            retopo->faces[p * 4 + 2],
            retopo->faces[p * 4 + 3]
        };
        bool isQuad = (f[3] != TRI_INDEX);
        int N = isQuad ? 4 : 3;
        std::vector<int32_t> origVerts;
        for (int i = 0; i < N; ++i) origVerts.push_back((int32_t)f[i]);

        bool vAIsOrig = false, vBIsOrig = false;
        for (int v : origVerts) {
            if (v == vA) vAIsOrig = true;
            if (v == vB) vBIsOrig = true;
        }

        std::vector<int32_t> cycle;
        for (int k = 0; k < N; ++k) {
            cycle.push_back(origVerts[k]);
            int32_t u = origVerts[k];
            int32_t v = origVerts[(k + 1) % N];
            int32_t eMin = std::min(u, v);
            int32_t eMax = std::max(u, v);

            bool vAOnEdge = false;
            if (!vAIsOrig) {
                if ((cut.end0EdgeV0 == u && cut.end0EdgeV1 == v) || (cut.end0EdgeV0 == v && cut.end0EdgeV1 == u)) {
                    vAOnEdge = true;
                } else if (edgeNewVerts.count({eMin, eMax}) && edgeNewVerts[{eMin, eMax}] == vA) {
                    vAOnEdge = true;
                }
            }

            bool vBOnEdge = false;
            if (!vBIsOrig) {
                if ((cut.end1EdgeV0 == u && cut.end1EdgeV1 == v) || (cut.end1EdgeV0 == v && cut.end1EdgeV1 == u)) {
                    vBOnEdge = true;
                } else if (edgeNewVerts.count({eMin, eMax}) && edgeNewVerts[{eMin, eMax}] == vB) {
                    vBOnEdge = true;
                }
            }

            if (vAOnEdge && vBOnEdge) {
                float tA = cut.end0EdgeT;
                float tB = cut.end1EdgeT;
                if (cut.end0EdgeV0 == v && cut.end0EdgeV1 == u) tA = 1.0f - tA;
                if (cut.end1EdgeV0 == v && cut.end1EdgeV1 == u) tB = 1.0f - tB;
                if (tA < tB) {
                    cycle.push_back(vA);
                    cycle.push_back(vB);
                } else {
                    cycle.push_back(vB);
                    cycle.push_back(vA);
                }
            } else if (vAOnEdge) {
                cycle.push_back(vA);
            } else if (vBOnEdge) {
                cycle.push_back(vB);
            }
        }

        int idxA = -1, idxB = -1;
        for (size_t i = 0; i < cycle.size(); ++i) {
            if (cycle[i] == vA && idxA == -1) idxA = (int)i;
            if (cycle[i] == vB && idxB == -1) idxB = (int)i;
        }

        if (idxA == -1 || idxB == -1 || idxA == idxB) continue;

        int M = (int)cycle.size();
        std::vector<int32_t> poly1;
        int cur = idxA;
        while (cur != idxB) {
            poly1.push_back(cycle[cur]);
            cur = (cur + 1) % M;
        }
        poly1.push_back(cycle[idxB]);

        std::vector<int32_t> poly2;
        cur = idxB;
        while (cur != idxA) {
            poly2.push_back(cycle[cur]);
            cur = (cur + 1) % M;
        }
        poly2.push_back(cycle[idxA]);

        if (poly1.size() < 3 || poly2.size() < 3) continue;

        auto convertToFaces = [&](const std::vector<int32_t>& pVerts) -> std::vector<std::vector<uint32_t>> {
            std::vector<std::vector<uint32_t>> res;
            if (pVerts.size() == 3) {
                res.push_back({ (uint32_t)pVerts[0], (uint32_t)pVerts[1], (uint32_t)pVerts[2], TRI_INDEX });
            } else if (pVerts.size() == 4) {
                res.push_back({ (uint32_t)pVerts[0], (uint32_t)pVerts[1], (uint32_t)pVerts[2], (uint32_t)pVerts[3] });
            } else if (pVerts.size() == 5) {
                auto getVertPos = [&](int32_t vIdx) -> glm::vec3 {
                    return glm::vec3(retopo->verts[vIdx * 3], retopo->verts[vIdx * 3 + 1], retopo->verts[vIdx * 3 + 2]);
                };
                float d02 = glm::distance(getVertPos(pVerts[0]), getVertPos(pVerts[2]));
                float d24 = glm::distance(getVertPos(pVerts[2]), getVertPos(pVerts[4]));
                if (d02 < d24) {
                    res.push_back({ (uint32_t)pVerts[0], (uint32_t)pVerts[1], (uint32_t)pVerts[2], TRI_INDEX });
                    res.push_back({ (uint32_t)pVerts[0], (uint32_t)pVerts[2], (uint32_t)pVerts[3], (uint32_t)pVerts[4] });
                } else {
                    res.push_back({ (uint32_t)pVerts[0], (uint32_t)pVerts[1], (uint32_t)pVerts[2], (uint32_t)pVerts[4] });
                    res.push_back({ (uint32_t)pVerts[2], (uint32_t)pVerts[3], (uint32_t)pVerts[4], TRI_INDEX });
                }
            } else {
                for (size_t t = 1; t + 1 < pVerts.size(); ++t) {
                    res.push_back({ (uint32_t)pVerts[0], (uint32_t)pVerts[t], (uint32_t)pVerts[t + 1], TRI_INDEX });
                }
            }
            return res;
        };

        auto facesA = convertToFaces(poly1);
        auto facesB = convertToFaces(poly2);
        std::vector<std::vector<uint32_t>> allNewFaces;
        allNewFaces.insert(allNewFaces.end(), facesA.begin(), facesA.end());
        allNewFaces.insert(allNewFaces.end(), facesB.begin(), facesB.end());

        if (allNewFaces.size() >= 2) {
            retopo->faces[p * 4 + 0] = allNewFaces[0][0];
            retopo->faces[p * 4 + 1] = allNewFaces[0][1];
            retopo->faces[p * 4 + 2] = allNewFaces[0][2];
            retopo->faces[p * 4 + 3] = allNewFaces[0][3];

            uint32_t grp = (p < (int32_t)retopo->faceGroups.size()) ? retopo->faceGroups[p] : 1;
            for (size_t k = 1; k < allNewFaces.size(); ++k) {
                retopo->faces.push_back(allNewFaces[k][0]);
                retopo->faces.push_back(allNewFaces[k][1]);
                retopo->faces.push_back(allNewFaces[k][2]);
                retopo->faces.push_back(allNewFaces[k][3]);
                if (!retopo->faceGroups.empty()) {
                    retopo->faceGroups.push_back(grp);
                }
            }

            processedPolys.insert(p);
            anyApplied = true;
        }
    }

    if (anyApplied) {
        NotifyMeshUpdated(retopo);
        return true;
    }
    return false;
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

    if (!AddQuad(mesh, q0, q1, q2, q3, targetNormal)) {
        DeleteVertex(mesh, newV1);
        DeleteVertex(mesh, newV0);
        return result;
    }

    result.valid = true;
    result.newV0 = newV0;
    result.newV1 = newV1;
    result.newPoly = (int32_t)(mesh->faces.size() / 4) - 1;
    return result;
}

ExtrudeEdgesResult QuadDrawMeshBuilder::ExtrudeEdges(
    Mesh* mesh, const std::vector<std::pair<int32_t, int32_t>>& edges, const glm::vec3& targetNormal
) {
    ExtrudeEdgesResult result;
    if (!mesh || edges.empty()) return result;

    int32_t ptCount = (int32_t)(mesh->verts.size() / 3);
    int32_t polyCount = (int32_t)(mesh->faces.size() / 4);

    auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
        glm::vec3 p(mesh->verts[idx * 3], mesh->verts[idx * 3 + 1], mesh->verts[idx * 3 + 2]);
        return (mesh->matrix != glm::mat4(1.0f)) ? glm::vec3(mesh->matrix * glm::vec4(p, 1.0f)) : p;
    };

    // 1. Filter valid border edges and deduplicate
    std::vector<std::pair<int32_t, int32_t>> validEdges;
    std::unordered_set<uint64_t> seenKeys;
    auto makeKey = [](int32_t a, int32_t b) -> uint64_t {
        uint32_t u = (uint32_t)std::min(a, b);
        uint32_t v = (uint32_t)std::max(a, b);
        return ((uint64_t)u << 32) | (uint64_t)v;
    };

    for (const auto& e : edges) {
        int32_t v0 = e.first;
        int32_t v1 = e.second;
        if (v0 < 0 || v0 >= ptCount || v1 < 0 || v1 >= ptCount || v0 == v1) continue;
        uint64_t key = makeKey(v0, v1);
        if (seenKeys.count(key)) continue;

        int32_t count = 0;
        for (int32_t p = 0; p < polyCount; ++p) {
            if (PolygonHasEdge(mesh->faces, p, v0, v1)) count++;
        }
        if (count >= 2) continue; // Only border edges can be extruded

        seenKeys.insert(key);
        validEdges.push_back({v0, v1});
    }

    if (validEdges.empty()) return result;

    // 2. Identify unique vertices among valid edges
    std::vector<int32_t> uniqueOrigVerts;
    std::unordered_set<int32_t> seenVerts;
    for (const auto& e : validEdges) {
        if (!seenVerts.count(e.first)) {
            seenVerts.insert(e.first);
            uniqueOrigVerts.push_back(e.first);
        }
        if (!seenVerts.count(e.second)) {
            seenVerts.insert(e.second);
            uniqueOrigVerts.push_back(e.second);
        }
    }

    // 3. Add new vertices at initial positions
    for (int32_t v : uniqueOrigVerts) {
        glm::vec3 pos = getVertWorld(v);
        int32_t newV = AddVertex(mesh, pos);
        if (newV == -1) {
            for (int32_t added : result.newVertices) {
                DeleteVertex(mesh, added);
            }
            return ExtrudeEdgesResult();
        }
        result.origToNew[v] = newV;
        result.newVertices.push_back(newV);
    }

    // 4. Create quad for each edge
    for (const auto& e : validEdges) {
        int32_t v0 = e.first;
        int32_t v1 = e.second;
        int32_t newV0 = result.origToNew[v0];
        int32_t newV1 = result.origToNew[v1];

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
            result.newPolygons.push_back((int32_t)(mesh->faces.size() / 4) - 1);
            result.newFrontEdges.push_back({std::min(newV0, newV1), std::max(newV0, newV1)});
        }
    }

    if (result.newPolygons.empty()) {
        for (int32_t added : result.newVertices) {
            DeleteVertex(mesh, added);
        }
        return ExtrudeEdgesResult();
    }

    result.valid = true;
    NotifyMeshUpdated(mesh);
    return result;
}

bool QuadDrawMeshBuilder::CanExtrudeVertex(
    const Mesh* mesh, int32_t vCorner, int32_t* outNeighA, int32_t* outNeighB
) {
    if (!mesh || vCorner < 0) return false;
    int32_t ptCount = (int32_t)(mesh->verts.size() / 3);
    int32_t polyCount = (int32_t)(mesh->faces.size() / 4);
    if (vCorner >= ptCount || polyCount == 0) return false;

    // Find all unique neighbors of vCorner
    std::vector<int32_t> neighbors;
    for (int32_t i = 0; i < polyCount; ++i) {
        uint32_t a = mesh->faces[i * 4 + 0];
        uint32_t b = mesh->faces[i * 4 + 1];
        uint32_t c = mesh->faces[i * 4 + 2];
        uint32_t d = mesh->faces[i * 4 + 3];
        bool isQuad = (d != TRI_INDEX);

        uint32_t v = (uint32_t)vCorner;
        if (a == v) { neighbors.push_back((int32_t)b); neighbors.push_back((int32_t)(isQuad ? d : c)); }
        else if (b == v) { neighbors.push_back((int32_t)c); neighbors.push_back((int32_t)a); }
        else if (c == v) { neighbors.push_back((int32_t)(isQuad ? d : a)); neighbors.push_back((int32_t)b); }
        else if (isQuad && d == v) { neighbors.push_back((int32_t)a); neighbors.push_back((int32_t)c); }
    }

    if (neighbors.empty()) return false;

    std::sort(neighbors.begin(), neighbors.end());
    neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());

    // Boundary edges are those with exactly 1 adjacent polygon
    std::vector<int32_t> boundaryNeighbors;
    for (int32_t n : neighbors) {
        int32_t edgeCnt = 0;
        for (int32_t i = 0; i < polyCount; ++i) {
            if (PolygonHasEdge(mesh->faces, i, vCorner, n)) edgeCnt++;
        }
        if (edgeCnt == 1) {
            boundaryNeighbors.push_back(n);
        }
    }

    if (boundaryNeighbors.size() < 2) return false;

    auto countEdgeFaces = [&](int32_t u, int32_t v) -> int32_t {
        int32_t cnt = 0;
        for (int32_t i = 0; i < polyCount; ++i) {
            if (PolygonHasEdge(mesh->faces, i, u, v)) cnt++;
        }
        return cnt;
    };

    // Find a pair (nA, nB) such that edge (nA, nB) does NOT already exist in the mesh
    int32_t bestA = -1;
    int32_t bestB = -1;

    for (size_t i = 0; i < boundaryNeighbors.size(); ++i) {
        for (size_t j = i + 1; j < boundaryNeighbors.size(); ++j) {
            int32_t nA = boundaryNeighbors[i];
            int32_t nB = boundaryNeighbors[j];
            if (countEdgeFaces(nA, nB) == 0) {
                bestA = nA;
                bestB = nB;
                break;
            }
        }
        if (bestA != -1) break;
    }

    if (bestA == -1) return false;

    if (outNeighA) *outNeighA = bestA;
    if (outNeighB) *outNeighB = bestB;
    return true;
}

ExtrudeVertexResult QuadDrawMeshBuilder::ExtrudeVertex(
    Mesh* mesh, int32_t vCorner, const glm::vec3& targetNormal, const glm::vec3& initialPos
) {
    ExtrudeVertexResult result;
    if (!mesh) return result;

    int32_t nA = -1;
    int32_t nB = -1;
    if (!CanExtrudeVertex(mesh, vCorner, &nA, &nB)) {
        return result;
    }

    int32_t newV = AddVertex(mesh, initialPos);
    if (newV == -1) return result;

    if (!AddQuad(mesh, vCorner, nA, newV, nB, targetNormal)) {
        DeleteVertex(mesh, newV);
        return result;
    }

    result.valid = true;
    result.newVertex = newV;
    result.newPoly = (int32_t)(mesh->faces.size() / 4) - 1;
    result.neighA = nA;
    result.neighB = nB;
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
            SnapResult snap = snapper.SnapClosestPoint(target, smoothed, 100.0f);
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

BorderChain QuadDrawMeshBuilder::FindBorderChain(const Mesh* mesh, int32_t startV0, int32_t startV1) {
    BorderChain result;
    if (!mesh || startV0 == startV1 || startV0 < 0 || startV1 < 0) return result;

    int32_t ptCount = (int32_t)(mesh->verts.size() / 3);
    int32_t polyCount = (int32_t)(mesh->faces.size() / 4);
    if (startV0 >= ptCount || startV1 >= ptCount) return result;

    auto countEdgePolys = [&](int32_t u, int32_t v) -> int32_t {
        int32_t cnt = 0;
        for (int32_t i = 0; i < polyCount; ++i) {
            if (PolygonHasEdge(mesh->faces, i, u, v)) cnt++;
        }
        return cnt;
    };

    if (countEdgePolys(startV0, startV1) != 1) {
        return result;
    }

    auto makeEdgeKey = [](int32_t a, int32_t b) -> uint64_t {
        uint32_t u = (uint32_t)std::min(a, b);
        uint32_t v = (uint32_t)std::max(a, b);
        return ((uint64_t)u << 32) | (uint64_t)v;
    };

    auto getIncidentBorderEdges = [&](int32_t v, int32_t excludeNeighbor) -> std::vector<int32_t> {
        std::vector<int32_t> neighbors;
        for (int32_t p = 0; p < polyCount; ++p) {
            if (!PolygonHasVertex(mesh->faces, p, v)) continue;
            uint32_t q[4] = {
                mesh->faces[p * 4 + 0],
                mesh->faces[p * 4 + 1],
                mesh->faces[p * 4 + 2],
                mesh->faces[p * 4 + 3]
            };
            int numE = (q[3] != TRI_INDEX) ? 4 : 3;
            for (int e = 0; e < numE; ++e) {
                int32_t a = (int32_t)q[e];
                int32_t b = (int32_t)q[(e + 1) % numE];
                int32_t other = -1;
                if (a == v) other = b;
                else if (b == v) other = a;
                if (other != -1 && other != excludeNeighbor) {
                    if (countEdgePolys(v, other) == 1) {
                        if (std::find(neighbors.begin(), neighbors.end(), other) == neighbors.end()) {
                            neighbors.push_back(other);
                        }
                    }
                }
            }
        }
        return neighbors;
    };

    std::unordered_set<uint64_t> visitedEdges;
    visitedEdges.insert(makeEdgeKey(startV0, startV1));

    // Trace forward from startV1
    std::vector<int32_t> forwardVerts;
    std::vector<std::pair<int32_t, int32_t>> forwardEdges;
    int32_t curr = startV1;
    int32_t prev = startV0;
    bool isClosed = false;

    while (true) {
        auto nexts = getIncidentBorderEdges(curr, prev);
        if (nexts.empty()) break;
        int32_t next = nexts[0];
        uint64_t key = makeEdgeKey(curr, next);
        if (visitedEdges.count(key)) {
            if (next == startV0) {
                isClosed = true;
            }
            break;
        }
        visitedEdges.insert(key);
        forwardEdges.push_back({curr, next});
        forwardVerts.push_back(next);
        prev = curr;
        curr = next;
    }

    // Trace backward from startV0 (only if not closed)
    std::vector<int32_t> backwardVerts;
    std::vector<std::pair<int32_t, int32_t>> backwardEdges;
    if (!isClosed) {
        curr = startV0;
        prev = startV1;
        while (true) {
            auto nexts = getIncidentBorderEdges(curr, prev);
            if (nexts.empty()) break;
            int32_t next = nexts[0];
            uint64_t key = makeEdgeKey(curr, next);
            if (visitedEdges.count(key)) break;
            visitedEdges.insert(key);
            backwardEdges.push_back({next, curr});
            backwardVerts.push_back(next);
            prev = curr;
            curr = next;
        }
    }

    std::reverse(backwardVerts.begin(), backwardVerts.end());
    std::reverse(backwardEdges.begin(), backwardEdges.end());

    result.vertices = backwardVerts;
    result.vertices.push_back(startV0);
    result.vertices.push_back(startV1);
    result.vertices.insert(result.vertices.end(), forwardVerts.begin(), forwardVerts.end());

    result.edges = backwardEdges;
    result.edges.push_back({startV0, startV1});
    result.edges.insert(result.edges.end(), forwardEdges.begin(), forwardEdges.end());
    result.isClosed = isClosed;

    return result;
}

std::vector<BorderChain> QuadDrawMeshBuilder::ExtractBorderChains(
    const Mesh* mesh, const std::vector<std::pair<int32_t, int32_t>>& edges
) {
    std::vector<BorderChain> chains;
    if (!mesh || edges.empty()) return chains;

    int32_t ptCount = (int32_t)(mesh->verts.size() / 3);
    int32_t polyCount = (int32_t)(mesh->faces.size() / 4);

    auto countEdgePolys = [&](int32_t u, int32_t v) -> int32_t {
        int32_t cnt = 0;
        for (int32_t i = 0; i < polyCount; ++i) {
            if (PolygonHasEdge(mesh->faces, i, u, v)) cnt++;
        }
        return cnt;
    };

    auto makeKey = [](int32_t a, int32_t b) -> uint64_t {
        uint32_t u = (uint32_t)std::min(a, b);
        uint32_t v = (uint32_t)std::max(a, b);
        return ((uint64_t)u << 32) | (uint64_t)v;
    };

    std::vector<std::pair<int32_t, int32_t>> validEdges;
    std::unordered_set<uint64_t> seenKeys;
    for (const auto& e : edges) {
        int32_t u = e.first;
        int32_t v = e.second;
        if (u < 0 || u >= ptCount || v < 0 || v >= ptCount || u == v) continue;
        uint64_t k = makeKey(u, v);
        if (seenKeys.count(k)) continue;
        if (countEdgePolys(u, v) >= 2) continue;
        seenKeys.insert(k);
        validEdges.push_back({u, v});
    }

    if (validEdges.empty()) return chains;

    std::unordered_map<int32_t, std::vector<int32_t>> adj;
    std::unordered_set<uint64_t> unvisitedEdges;
    for (const auto& e : validEdges) {
        adj[e.first].push_back(e.second);
        adj[e.second].push_back(e.first);
        unvisitedEdges.insert(makeKey(e.first, e.second));
    }

    while (!unvisitedEdges.empty()) {
        uint64_t seedKey = *unvisitedEdges.begin();
        int32_t seedU = (int32_t)(seedKey >> 32);
        int32_t seedV = (int32_t)(seedKey & 0xFFFFFFFF);

        std::unordered_set<int32_t> compVerts;
        std::vector<int32_t> queue = { seedU };
        compVerts.insert(seedU);
        size_t qHead = 0;
        while (qHead < queue.size()) {
            int32_t curr = queue[qHead++];
            for (int32_t neigh : adj[curr]) {
                if (unvisitedEdges.count(makeKey(curr, neigh))) {
                    if (!compVerts.count(neigh)) {
                        compVerts.insert(neigh);
                        queue.push_back(neigh);
                    }
                }
            }
        }

        std::vector<int32_t> endpoints;
        for (int32_t v : compVerts) {
            int deg = 0;
            for (int32_t neigh : adj[v]) {
                if (unvisitedEdges.count(makeKey(v, neigh))) deg++;
            }
            if (deg == 1) {
                endpoints.push_back(v);
            }
        }

        BorderChain chain;
        if (!endpoints.empty()) {
            int32_t curr = endpoints[0];
            int32_t prev = -1;
            chain.vertices.push_back(curr);
            while (true) {
                int32_t next = -1;
                for (int32_t neigh : adj[curr]) {
                    if (neigh != prev && unvisitedEdges.count(makeKey(curr, neigh))) {
                        next = neigh;
                        break;
                    }
                }
                if (next == -1) break;
                unvisitedEdges.erase(makeKey(curr, next));
                chain.edges.push_back({curr, next});
                chain.vertices.push_back(next);
                prev = curr;
                curr = next;
            }
            chain.isClosed = false;
        } else {
            int32_t curr = seedU;
            int32_t prev = -1;
            chain.vertices.push_back(curr);
            while (true) {
                int32_t next = -1;
                for (int32_t neigh : adj[curr]) {
                    if (neigh != prev && unvisitedEdges.count(makeKey(curr, neigh))) {
                        next = neigh;
                        break;
                    }
                }
                if (next == -1) break;
                unvisitedEdges.erase(makeKey(curr, next));
                chain.edges.push_back({curr, next});
                chain.vertices.push_back(next);
                if (next == seedU) {
                    chain.isClosed = true;
                    break;
                }
                prev = curr;
                curr = next;
            }
        }

        if (!chain.edges.empty()) {
            chains.push_back(chain);
        }
    }

    return chains;
}

StrokeExtrudeResult QuadDrawMeshBuilder::ExtrudeEdgesToStroke(
    Mesh* mesh,
    const Mesh* targetMesh,
    SurfaceSnapper& snapper,
    const BorderChain& chain,
    const std::vector<glm::vec3>& strokeWorldPoints,
    float weldRadiusPixels,
    const Camera* camera
) {
    StrokeExtrudeResult result;
    if (!mesh || chain.edges.empty() || strokeWorldPoints.size() < 2) return result;

    int32_t N = (int32_t)chain.edges.size();
    if (chain.vertices.size() < (size_t)(N + 1)) return result;

    auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
        glm::vec3 p(mesh->verts[idx * 3], mesh->verts[idx * 3 + 1], mesh->verts[idx * 3 + 2]);
        return (mesh->matrix != glm::mat4(1.0f)) ? glm::vec3(mesh->matrix * glm::vec4(p, 1.0f)) : p;
    };

    std::vector<glm::vec3> stroke;
    stroke.push_back(strokeWorldPoints[0]);
    for (size_t i = 1; i < strokeWorldPoints.size(); ++i) {
        if (glm::distance(strokeWorldPoints[i], stroke.back()) > 1e-4f) {
            stroke.push_back(strokeWorldPoints[i]);
        }
    }
    if (stroke.size() < 2) return result;

    glm::vec3 v0Pos = getVertWorld(chain.vertices.front());
    glm::vec3 vNPos = getVertWorld(chain.vertices.back());
    float dNormal = glm::distance(stroke.front(), v0Pos) + glm::distance(stroke.back(), vNPos);
    float dReversed = glm::distance(stroke.front(), vNPos) + glm::distance(stroke.back(), v0Pos);

    if (dReversed < dNormal && !chain.isClosed) {
        std::reverse(stroke.begin(), stroke.end());
    }

    std::vector<float> cumChain(N + 1, 0.0f);
    for (int32_t i = 1; i <= N; ++i) {
        cumChain[i] = cumChain[i - 1] + glm::distance(
            getVertWorld(chain.vertices[i]),
            getVertWorld(chain.vertices[i - 1])
        );
    }
    float totalChainLen = cumChain[N];

    std::vector<float> cumStroke(stroke.size(), 0.0f);
    for (size_t i = 1; i < stroke.size(); ++i) {
        cumStroke[i] = cumStroke[i - 1] + glm::distance(stroke[i], stroke[i - 1]);
    }
    float totalStrokeLen = cumStroke.back();
    if (totalStrokeLen < 1e-5f) return result;

    std::vector<glm::vec3> resampledPts(N + 1);
    for (int32_t i = 0; i <= N; ++i) {
        float t = (totalChainLen > 1e-5f) ? (cumChain[i] / totalChainLen) : ((float)i / (float)N);
        float targetDist = t * totalStrokeLen;

        size_t seg = 0;
        while (seg + 1 < cumStroke.size() && cumStroke[seg + 1] < targetDist) {
            seg++;
        }
        if (seg + 1 >= cumStroke.size()) {
            resampledPts[i] = stroke.back();
        } else {
            float segLen = cumStroke[seg + 1] - cumStroke[seg];
            float alpha = (segLen > 1e-6f) ? ((targetDist - cumStroke[seg]) / segLen) : 0.0f;
            alpha = std::clamp(alpha, 0.0f, 1.0f);
            resampledPts[i] = glm::mix(stroke[seg], stroke[seg + 1], alpha);
        }

        if (targetMesh && targetMesh->nbFaces > 0) {
            SnapResult snap = snapper.SnapClosestPoint(targetMesh, resampledPts[i], 500.0f);
            if (snap.valid) {
                resampledPts[i] = snap.worldPos;
            }
        }
    }

    std::vector<int32_t> newVertIndices(N + 1, -1);
    for (int32_t i = 0; i <= N; ++i) {
        if (chain.isClosed && i == N) {
            newVertIndices[N] = newVertIndices[0];
            continue;
        }
        int32_t nv = AddVertex(mesh, resampledPts[i]);
        if (nv == -1) {
            for (int32_t added : result.newVertices) {
                DeleteVertex(mesh, added);
            }
            return StrokeExtrudeResult();
        }
        newVertIndices[i] = nv;
        result.newVertices.push_back(nv);
    }

    for (int32_t i = 0; i < N; ++i) {
        int32_t u = chain.vertices[i];
        int32_t v = chain.vertices[i + 1];
        int32_t nu = newVertIndices[i];
        int32_t nv = newVertIndices[i + 1];

        glm::vec3 quadMid = (getVertWorld(u) + getVertWorld(v) + resampledPts[i] + resampledPts[i + 1]) * 0.25f;
        glm::vec3 targetNorm(0, 1, 0);
        if (targetMesh && targetMesh->nbFaces > 0) {
            SnapResult snapMid = snapper.SnapClosestPoint(targetMesh, quadMid, 500.0f);
            if (snapMid.valid) targetNorm = snapMid.normal;
        }

        if (AddQuad(mesh, u, v, nv, nu, targetNorm)) {
            result.newPolygons.push_back((int32_t)(mesh->faces.size() / 4) - 1);
            result.newFrontEdges.push_back({std::min(nu, nv), std::max(nu, nv)});
        }
    }

    if (result.newPolygons.empty()) {
        for (int32_t added : result.newVertices) {
            DeleteVertex(mesh, added);
        }
        return StrokeExtrudeResult();
    }

    if (camera && weldRadiusPixels > 0.0f && !chain.isClosed) {
        std::unordered_set<int32_t> excluded;
        for (int32_t v : chain.vertices) excluded.insert(v);
        for (int32_t nv : newVertIndices) excluded.insert(nv);

        std::vector<std::pair<int32_t, int32_t>> welds;

        glm::vec3 sStart = camera->project(resampledPts.front());
        if (sStart.z > 0.0f && sStart.z < 1.0f) {
            int32_t w = snapper.FindNearestRetopoVertex(*camera, mesh, sStart.x, sStart.y, weldRadiusPixels, newVertIndices[0], targetMesh);
            if (w != -1 && !excluded.count(w)) {
                welds.push_back({newVertIndices[0], w});
            }
        }

        glm::vec3 sEnd = camera->project(resampledPts.back());
        if (sEnd.z > 0.0f && sEnd.z < 1.0f) {
            int32_t w = snapper.FindNearestRetopoVertex(*camera, mesh, sEnd.x, sEnd.y, weldRadiusPixels, newVertIndices[N], targetMesh);
            if (w != -1 && !excluded.count(w)) {
                welds.push_back({newVertIndices[N], w});
            }
        }

        if (!welds.empty()) {
            std::vector<int32_t> remap;
            WeldVerticesMultiple(mesh, welds, nullptr, &remap);
            if (!remap.empty()) {
                for (auto& fe : result.newFrontEdges) {
                    if (fe.first >= 0 && fe.first < (int32_t)remap.size() && remap[fe.first] != -1) {
                        fe.first = remap[fe.first];
                    }
                    if (fe.second >= 0 && fe.second < (int32_t)remap.size() && remap[fe.second] != -1) {
                        fe.second = remap[fe.second];
                    }
                    if (fe.first > fe.second) std::swap(fe.first, fe.second);
                }
            }
        }
    }

    result.valid = true;
    NotifyMeshUpdated(mesh);
    return result;
}

TubeSliceResult QuadDrawMeshBuilder::SliceTargetMeshForTube(
    const Camera& camera, const Mesh* targetMesh, SurfaceSnapper& snapper,
    const glm::vec2& screenP0, const glm::vec2& screenP1, int32_t radialSegments
) {
    TubeSliceResult result;
    if (!targetMesh || targetMesh->nbFaces == 0 || radialSegments < 3) return result;

    float scrDist = glm::distance(screenP0, screenP1);
    if (scrDist < 6.0f) return result;

    if (!snapper.getBVH().IsBuilt() || snapper.getBVH().GetSourceMesh() != targetMesh) {
        snapper.getBVH().Build(targetMesh);
    }
    if (!snapper.getBVH().IsBuilt()) return result;

    // 1. Densely sample along the screen line to find where it crosses targetMesh
    const int sampleCount = std::clamp((int)(scrDist / 4.0f), 20, 80);
    std::vector<glm::vec3> hits;
    for (int i = 0; i <= sampleCount; ++i) {
        float t = (float)i / (float)sampleCount;
        glm::vec2 sp = glm::mix(screenP0, screenP1, t);
        SnapResult snap = snapper.RaycastSurface(camera, targetMesh, sp.x, sp.y);
        if (snap.valid) {
            hits.push_back(snap.worldPos);
        }
    }

    // Fallback: check snap with a small radius around midpoint if direct raycasts barely missed
    if (hits.empty()) {
        glm::vec2 midScreen = (screenP0 + screenP1) * 0.5f;
        SnapResult snap = snapper.SnapClosestPoint(targetMesh, camera.unproject(midScreen.x, midScreen.y, 0.5f), 100.0f);
        if (snap.valid) hits.push_back(snap.worldPos);
    }

    if (hits.empty()) return result;

    glm::vec3 hitAvg(0.0f);
    for (const auto& h : hits) hitAvg += h;
    hitAvg /= (float)hits.size();

    // 2. Define cutting plane: exactly containing the screen stroke line and camera view
    Ray r0 = camera.getRay(screenP0.x, screenP0.y);
    Ray r1 = camera.getRay(screenP1.x, screenP1.y);

    glm::vec3 planeNormal(0.0f);
    glm::vec3 planePoint(0.0f);

    if (camera.isOrthographic()) {
        glm::vec3 u0 = camera.unproject(screenP0.x, screenP0.y, 0.0f);
        glm::vec3 u1 = camera.unproject(screenP1.x, screenP1.y, 0.0f);
        glm::vec3 strokeDir = u1 - u0;
        glm::vec3 camDir = camera.unproject(screenP0.x, screenP0.y, 1.0f) - u0;
        if (glm::length(camDir) > 1e-5f) camDir = glm::normalize(camDir);
        else camDir = r0.dir;
        planeNormal = glm::cross(strokeDir, camDir);
        float pLen = glm::length(planeNormal);
        if (pLen > 1e-6f) planeNormal /= pLen;
        else planeNormal = glm::vec3(0.0f, 1.0f, 0.0f);
        planePoint = u0;
    } else {
        glm::vec3 eye = camera.computePosition();
        planeNormal = glm::cross(r1.dir, r0.dir);
        float pLen = glm::length(planeNormal);
        if (pLen > 1e-6f) {
            planeNormal /= pLen;
        } else {
            Ray rMid = camera.getRay((screenP0.x + screenP1.x) * 0.5f, (screenP0.y + screenP1.y) * 0.5f);
            glm::vec3 u0 = camera.unproject(screenP0.x, screenP0.y, 0.5f);
            glm::vec3 u1 = camera.unproject(screenP1.x, screenP1.y, 0.5f);
            planeNormal = glm::cross(u1 - u0, rMid.dir);
            float l2 = glm::length(planeNormal);
            if (l2 > 1e-6f) planeNormal /= l2;
            else planeNormal = glm::vec3(0.0f, 1.0f, 0.0f);
        }
        planePoint = eye;
    }

    float planeD = -glm::dot(planeNormal, planePoint);

    // 3. Query all triangles from BVH that intersect the cutting plane (maxRadius=0 to avoid clipping back of limb!)
    std::vector<BVHTriangle> tris;
    if (!snapper.getBVH().QueryPlaneTriangles(planeNormal, planeD, hitAvg, 0.0f, tris) || tris.empty()) {
        return result;
    }

    // 4. Intersect triangles with plane to produce 3D segments
    struct PlaneSegment {
        glm::vec3 p0;
        glm::vec3 p1;
    };
    std::vector<PlaneSegment> segments;
    segments.reserve(tris.size());

    for (const auto& tri : tris) {
        float d0 = glm::dot(planeNormal, tri.v0) + planeD;
        float d1 = glm::dot(planeNormal, tri.v1) + planeD;
        float d2 = glm::dot(planeNormal, tri.v2) + planeD;

        if (std::abs(d0) < 1e-7f) d0 = (d0 >= 0.0f) ? 1e-7f : -1e-7f;
        if (std::abs(d1) < 1e-7f) d1 = (d1 >= 0.0f) ? 1e-7f : -1e-7f;
        if (std::abs(d2) < 1e-7f) d2 = (d2 >= 0.0f) ? 1e-7f : -1e-7f;

        bool s0 = d0 > 0.0f;
        bool s1 = d1 > 0.0f;
        bool s2 = d2 > 0.0f;

        if ((s0 == s1) && (s1 == s2)) continue;

        glm::vec3 pts[2];
        int numPts = 0;

        if (s0 != s1 && numPts < 2) {
            float t = d0 / (d0 - d1);
            pts[numPts++] = tri.v0 + t * (tri.v1 - tri.v0);
        }
        if (s1 != s2 && numPts < 2) {
            float t = d1 / (d1 - d2);
            pts[numPts++] = tri.v1 + t * (tri.v2 - tri.v1);
        }
        if (s2 != s0 && numPts < 2) {
            float t = d2 / (d2 - d0);
            pts[numPts++] = tri.v2 + t * (tri.v0 - tri.v2);
        }

        if (numPts == 2) {
            glm::vec3 diff = pts[0] - pts[1];
            if (glm::dot(diff, diff) > 1e-10f) {
                segments.push_back({pts[0], pts[1]});
            }
        }
    }

    if (segments.empty()) return result;

    // 5. Chain segments into loops with bidirectional expansion
    float totalSegLen = 0.0f;
    for (const auto& s : segments) totalSegLen += glm::distance(s.p0, s.p1);
    float avgSegLen = totalSegLen / (float)segments.size();
    float eps = std::max(avgSegLen * 0.8f, 1e-3f);
    float epsSq = eps * eps;

    std::vector<std::vector<glm::vec3>> loops;
    std::vector<bool> used(segments.size(), false);

    for (size_t i = 0; i < segments.size(); ++i) {
        if (used[i]) continue;
        used[i] = true;

        std::deque<glm::vec3> chain;
        chain.push_back(segments[i].p0);
        chain.push_back(segments[i].p1);

        // Expand forward
        bool canExpand = true;
        while (canExpand) {
            glm::vec3 tip = chain.back();
            glm::vec3 toStart = tip - chain.front();
            if (chain.size() >= 3 && glm::dot(toStart, toStart) < epsSq * 4.0f) {
                break;
            }

            int bestNext = -1;
            bool flipNext = false;
            float bestDistSq = epsSq * 9.0f;

            for (size_t j = 0; j < segments.size(); ++j) {
                if (used[j]) continue;
                glm::vec3 d0 = tip - segments[j].p0;
                float dist0 = glm::dot(d0, d0);
                if (dist0 < bestDistSq) {
                    bestDistSq = dist0;
                    bestNext = (int)j;
                    flipNext = false;
                }
                glm::vec3 d1 = tip - segments[j].p1;
                float dist1 = glm::dot(d1, d1);
                if (dist1 < bestDistSq) {
                    bestDistSq = dist1;
                    bestNext = (int)j;
                    flipNext = true;
                }
            }

            if (bestNext != -1) {
                used[bestNext] = true;
                chain.push_back(flipNext ? segments[bestNext].p0 : segments[bestNext].p1);
            } else {
                canExpand = false;
            }
        }

        // Expand backward
        canExpand = true;
        while (canExpand) {
            glm::vec3 tip = chain.front();
            glm::vec3 toBack = tip - chain.back();
            if (chain.size() >= 3 && glm::dot(toBack, toBack) < epsSq * 4.0f) {
                break;
            }

            int bestNext = -1;
            bool flipNext = false;
            float bestDistSq = epsSq * 9.0f;

            for (size_t j = 0; j < segments.size(); ++j) {
                if (used[j]) continue;
                glm::vec3 d0 = tip - segments[j].p1;
                float dist0 = glm::dot(d0, d0);
                if (dist0 < bestDistSq) {
                    bestDistSq = dist0;
                    bestNext = (int)j;
                    flipNext = false;
                }
                glm::vec3 d1 = tip - segments[j].p0;
                float dist1 = glm::dot(d1, d1);
                if (dist1 < bestDistSq) {
                    bestDistSq = dist1;
                    bestNext = (int)j;
                    flipNext = true;
                }
            }

            if (bestNext != -1) {
                used[bestNext] = true;
                chain.push_front(flipNext ? segments[bestNext].p1 : segments[bestNext].p0);
            } else {
                canExpand = false;
            }
        }

        float gap = glm::distance(chain.front(), chain.back());
        float chainPerim = 0.0f;
        for (size_t k = 0; k + 1 < chain.size(); ++k) {
            chainPerim += glm::distance(chain[k], chain[k + 1]);
        }

        bool isClosed = (gap < eps * 3.0f);
        if (!isClosed && chain.size() >= 6 && chainPerim > 1e-3f && (gap < chainPerim * 0.45f)) {
            isClosed = true;
        }

        if (isClosed && chain.size() >= 4) {
            std::vector<glm::vec3> loop(chain.begin(), chain.end());
            loops.push_back(std::move(loop));
        }
    }

    if (loops.empty()) return result;

    // 6. Select the loop whose center is closest to hitAvg
    int bestLoopIdx = -1;
    float bestLoopDist = 1e30f;

    for (size_t k = 0; k < loops.size(); ++k) {
        const auto& lp = loops[k];
        glm::vec3 c(0.0f);
        float perim = 0.0f;
        for (size_t v = 0; v < lp.size(); ++v) {
            c += lp[v];
            perim += glm::distance(lp[v], lp[(v + 1) % lp.size()]);
        }
        c /= (float)lp.size();
        if (perim < 1e-3f) continue;

        float dist = glm::distance(c, hitAvg);
        if (dist < bestLoopDist) {
            bestLoopDist = dist;
            bestLoopIdx = (int)k;
        }
    }

    if (bestLoopIdx == -1) return result;
    const auto& bestLoop = loops[bestLoopIdx];

    // 7. Resample loop into N equidistant points
    const int K = (int)bestLoop.size();
    std::vector<float> cumDist(K + 1, 0.0f);
    for (int i = 0; i < K; ++i) {
        cumDist[i + 1] = cumDist[i] + glm::distance(bestLoop[i], bestLoop[(i + 1) % K]);
    }
    float totalPerim = cumDist[K];
    if (totalPerim < 1e-4f) return result;

    const int N = radialSegments;
    std::vector<glm::vec3> resampled(N);

    for (int j = 0; j < N; ++j) {
        float targetDist = totalPerim * ((float)j / (float)N);
        int seg = 0;
        while (seg < K - 1 && cumDist[seg + 1] < targetDist) {
            seg++;
        }
        float segLen = cumDist[seg + 1] - cumDist[seg];
        float alpha = (segLen > 1e-6f) ? ((targetDist - cumDist[seg]) / segLen) : 0.0f;
        glm::vec3 pt = glm::mix(bestLoop[seg], bestLoop[(seg + 1) % K], std::clamp(alpha, 0.0f, 1.0f));

        SnapResult snap = snapper.SnapClosestPoint(targetMesh, pt, 500.0f);
        if (snap.valid) pt = snap.worldPos;
        resampled[j] = pt;
    }

    // 8. Compute center and orient counter-clockwise around planeNormal
    glm::vec3 loopCenter(0.0f);
    for (const auto& p : resampled) loopCenter += p;
    loopCenter /= (float)N;

    glm::vec3 loopNorm(0.0f);
    for (int j = 0; j < N; ++j) {
        loopNorm += glm::cross(resampled[j] - loopCenter, resampled[(j + 1) % N] - loopCenter);
    }
    if (glm::dot(loopNorm, planeNormal) < 0.0f) {
        std::reverse(resampled.begin() + 1, resampled.end());
    }

    // 9. Align start vertex with screenP0
    glm::vec3 u0 = camera.unproject(screenP0.x, screenP0.y, 0.5f);
    int startIdx = 0;
    float bestStartDist = 1e30f;
    for (int j = 0; j < N; ++j) {
        float d = glm::distance(resampled[j], u0);
        if (d < bestStartDist) {
            bestStartDist = d;
            startIdx = j;
        }
    }
    if (startIdx > 0) {
        std::rotate(resampled.begin(), resampled.begin() + startIdx, resampled.end());
    }

    result.valid = true;
    result.worldPoints = std::move(resampled);
    result.center = loopCenter;
    result.normal = planeNormal;
    result.radius = totalPerim / (2.0f * 3.14159265f);
    return result;
}

TubeBuildResult QuadDrawMeshBuilder::BuildTubeRing(
    Mesh* retopoMesh, const Mesh* targetMesh, SurfaceSnapper& snapper,
    const std::vector<glm::vec3>& newRingPoints,
    const std::vector<int32_t>& previousLoopVertices,
    int32_t spans
) {
    TubeBuildResult result;
    if (!retopoMesh || newRingPoints.empty()) return result;

    const int N = (int)newRingPoints.size();
    if (spans < 1) spans = 1;

    bool hasValidPrev = (!previousLoopVertices.empty() && (int)previousLoopVertices.size() == N);
    if (hasValidPrev) {
        int32_t ptCount = (int32_t)(retopoMesh->verts.size() / 3);
        for (int32_t v : previousLoopVertices) {
            if (v < 0 || v >= ptCount) {
                hasValidPrev = false;
                break;
            }
        }
    }

    auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
        glm::vec3 p(retopoMesh->verts[idx * 3], retopoMesh->verts[idx * 3 + 1], retopoMesh->verts[idx * 3 + 2]);
        return (retopoMesh->matrix != glm::mat4(1.0f)) ? glm::vec3(retopoMesh->matrix * glm::vec4(p, 1.0f)) : p;
    };

    if (!hasValidPrev) {
        result.newLoopVertices.resize(N);
        for (int j = 0; j < N; ++j) {
            result.newLoopVertices[j] = AddVertex(retopoMesh, newRingPoints[j]);
        }
        result.valid = true;
        result.quadsCreated = 0;
        NotifyMeshUpdated(retopoMesh);
        return result;
    }

    std::vector<glm::vec3> prevPos(N);
    for (int j = 0; j < N; ++j) {
        prevPos[j] = getVertWorld(previousLoopVertices[j]);
    }

    int bestShift = 0;
    int bestDir = 1;
    float minCost = 1e30f;

    for (int dir : {1, -1}) {
        for (int s = 0; s < N; ++s) {
            float cost = 0.0f;
            for (int j = 0; j < N; ++j) {
                int qIdx = (dir * j + s + N * 100) % N;
                glm::vec3 diff = prevPos[j] - newRingPoints[qIdx];
                cost += glm::dot(diff, diff);
            }
            if (cost < minCost) {
                minCost = cost;
                bestShift = s;
                bestDir = dir;
            }
        }
    }

    std::vector<glm::vec3> alignedNewPoints(N);
    for (int j = 0; j < N; ++j) {
        int qIdx = (bestDir * j + bestShift + N * 100) % N;
        alignedNewPoints[j] = newRingPoints[qIdx];
    }

    std::vector<int32_t> currentRing = previousLoopVertices;
    int totalQuads = 0;

    for (int step = 1; step <= spans; ++step) {
        float t = (float)step / (float)spans;
        std::vector<int32_t> nextRing(N);

        for (int j = 0; j < N; ++j) {
            glm::vec3 pt = glm::mix(prevPos[j], alignedNewPoints[j], t);
            if (step < spans) {
                SnapResult snap = snapper.SnapClosestPoint(targetMesh, pt, 500.0f);
                if (snap.valid) pt = snap.worldPos;
            } else {
                pt = alignedNewPoints[j];
            }
            nextRing[j] = AddVertex(retopoMesh, pt);
        }

        for (int j = 0; j < N; ++j) {
            int jNext = (j + 1) % N;
            int32_t a = currentRing[j];
            int32_t b = currentRing[jNext];
            int32_t c = nextRing[jNext];
            int32_t d = nextRing[j];

            glm::vec3 quadMid = (getVertWorld(a) + getVertWorld(c)) * 0.5f;
            SnapResult snap = snapper.SnapClosestPoint(targetMesh, quadMid, 500.0f);
            glm::vec3 targetN = snap.valid ? snap.normal : glm::vec3(0.0f, 1.0f, 0.0f);

            if (AddQuad(retopoMesh, a, b, c, d, targetN)) {
                totalQuads++;
            }
        }

        currentRing = nextRing;
    }

    result.valid = true;
    result.newLoopVertices = currentRing;
    result.quadsCreated = totalQuads;
    InvalidateEdgeCache();
    NotifyMeshUpdated(retopoMesh);
    return result;
}

TubeSegmentResult QuadDrawMeshBuilder::GenerateTubeBetweenPoints(
    const Mesh* targetMesh, SurfaceSnapper& snapper,
    const glm::vec3& startWorldPos, const glm::vec3& endWorldPos,
    int32_t radialSegments, int32_t spans
) {
    return GenerateTubeFromPoints(targetMesh, snapper, {startWorldPos, endWorldPos}, radialSegments, spans);
}

TubeSegmentResult QuadDrawMeshBuilder::GenerateTubeFromPoints(
    const Mesh* targetMesh, SurfaceSnapper& snapper,
    const std::vector<glm::vec3>& centerlinePoints,
    int32_t radialSegments, int32_t spansPerSegment
) {
    TubeSegmentResult result;
    if (!targetMesh || targetMesh->nbFaces == 0 || centerlinePoints.size() < 2 || radialSegments < 3 || spansPerSegment < 1) {
        return result;
    }

    if (!snapper.getBVH().IsBuilt() || snapper.getBVH().GetSourceMesh() != targetMesh) {
        snapper.getBVH().Build(targetMesh);
    }
    if (!snapper.getBVH().IsBuilt()) return result;

    int numPoints = (int)centerlinePoints.size();
    int numSegments = numPoints - 1;
    int totalRings = numSegments * spansPerSegment + 1;

    result.radialSegments = radialSegments;
    result.spans = spansPerSegment;
    result.rings.resize(totalRings);

    // 1. Segment directions and lengths
    std::vector<glm::vec3> segDirs(numSegments);
    std::vector<float> segLens(numSegments);
    for (int s = 0; s < numSegments; ++s) {
        glm::vec3 v = centerlinePoints[s + 1] - centerlinePoints[s];
        float len = glm::length(v);
        segLens[s] = len;
        segDirs[s] = (len > 1e-4f) ? (v / len) : glm::vec3(0.0f, 1.0f, 0.0f);
    }

    // 2. Joint normals (mitered / bisector at corners)
    std::vector<glm::vec3> jointNormals(numPoints);
    jointNormals[0] = segDirs[0];
    jointNormals[numPoints - 1] = segDirs[numSegments - 1];
    for (int i = 1; i < numPoints - 1; ++i) {
        glm::vec3 bisect = segDirs[i - 1] + segDirs[i];
        float bLen = glm::length(bisect);
        jointNormals[i] = (bLen > 1e-4f) ? (bisect / bLen) : segDirs[i];
    }

    // 3. Construct Rotation-Minimizing Frames (RMF / Bishop frame) across segments
    // to guarantee straight longitudinal edges like taut strings without twisting
    std::vector<glm::vec3> uSeg(numSegments);
    std::vector<glm::vec3> vSeg(numSegments);

    glm::vec3 initialUp(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(segDirs[0], initialUp)) > 0.9f) {
        initialUp = glm::vec3(1.0f, 0.0f, 0.0f);
    }
    uSeg[0] = glm::normalize(glm::cross(segDirs[0], initialUp));
    vSeg[0] = glm::normalize(glm::cross(segDirs[0], uSeg[0]));

    for (int s = 1; s < numSegments; ++s) {
        glm::vec3 vPrev = segDirs[s - 1];
        glm::vec3 vCur = segDirs[s];
        glm::vec3 axis = glm::cross(vPrev, vCur);
        float axisLen = glm::length(axis);
        if (axisLen > 1e-5f) {
            axis /= axisLen;
            float dotVal = std::clamp(glm::dot(vPrev, vCur), -1.0f, 1.0f);
            float angle = std::acos(dotVal);
            glm::mat4 rot = glm::rotate(glm::mat4(1.0f), angle, axis);
            uSeg[s] = glm::normalize(glm::vec3(rot * glm::vec4(uSeg[s - 1], 0.0f)));
            vSeg[s] = glm::normalize(glm::cross(vCur, uSeg[s]));
        } else {
            if (glm::dot(vPrev, vCur) < 0.0f) {
                uSeg[s] = -uSeg[s - 1];
                vSeg[s] = -vSeg[s - 1];
            } else {
                uSeg[s] = uSeg[s - 1];
                vSeg[s] = vSeg[s - 1];
            }
        }
    }

    float estRadius = 1.0f;
    for (int s = 0; s < numSegments; ++s) {
        if (segLens[s] > 1e-3f) {
            estRadius = segLens[s] * 0.15f;
            break;
        }
    }

    // 4. Generate rings with strictly consistent radial angles
    for (int ringIdx = 0; ringIdx < totalRings; ++ringIdx) {
        int s = 0;
        float t = 0.0f;
        if (ringIdx == totalRings - 1) {
            s = numSegments - 1;
            t = 1.0f;
        } else {
            s = ringIdx / spansPerSegment;
            int k = ringIdx % spansPerSegment;
            t = (float)k / (float)spansPerSegment;
        }

        glm::vec3 curAxisPt = glm::mix(centerlinePoints[s], centerlinePoints[s + 1], t);

        // Interpolated cutting plane normal along segment
        glm::vec3 planeNormal = glm::mix(jointNormals[s], jointNormals[s + 1], t);
        float pNormLen = glm::length(planeNormal);
        planeNormal = (pNormLen > 1e-4f) ? (planeNormal / pNormLen) : segDirs[s];
        float planeD = -glm::dot(planeNormal, curAxisPt);

        // Frame in this slice plane projected from the segment's reference frame
        glm::vec3 uRing = uSeg[s] - glm::dot(uSeg[s], planeNormal) * planeNormal;
        float uLen = glm::length(uRing);
        if (uLen > 1e-4f) {
            uRing /= uLen;
        } else {
            glm::vec3 fallbackUp = (std::abs(planeNormal.y) < 0.9f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            uRing = glm::normalize(glm::cross(planeNormal, fallbackUp));
        }
        glm::vec3 vRing = glm::normalize(glm::cross(planeNormal, uRing));

        // Find candidate surface point near axis point
        SnapResult refSnap = snapper.SnapClosestPoint(targetMesh, curAxisPt, 500.0f);
        glm::vec3 refPt = refSnap.valid ? refSnap.worldPos : curAxisPt;

        std::vector<BVHTriangle> tris;
        snapper.getBVH().QueryPlaneTriangles(planeNormal, planeD, refPt, 0.0f, tris);

        bool loopFound = false;
        std::vector<glm::vec3> loop;

        if (!tris.empty()) {
            struct PlaneSegment { glm::vec3 p0, p1; };
            std::vector<PlaneSegment> segments;
            segments.reserve(tris.size());

            for (const auto& tri : tris) {
                float d0 = glm::dot(planeNormal, tri.v0) + planeD;
                float d1 = glm::dot(planeNormal, tri.v1) + planeD;
                float d2 = glm::dot(planeNormal, tri.v2) + planeD;

                if (std::abs(d0) < 1e-7f) d0 = (d0 >= 0.0f) ? 1e-7f : -1e-7f;
                if (std::abs(d1) < 1e-7f) d1 = (d1 >= 0.0f) ? 1e-7f : -1e-7f;
                if (std::abs(d2) < 1e-7f) d2 = (d2 >= 0.0f) ? 1e-7f : -1e-7f;

                if ((d0 > 0.0f == d1 > 0.0f) && (d1 > 0.0f == d2 > 0.0f)) continue;

                glm::vec3 pts[2];
                int numPts = 0;
                if ((d0 > 0.0f) != (d1 > 0.0f) && numPts < 2) {
                    float a = d0 / (d0 - d1);
                    pts[numPts++] = tri.v0 + a * (tri.v1 - tri.v0);
                }
                if ((d1 > 0.0f) != (d2 > 0.0f) && numPts < 2) {
                    float a = d1 / (d1 - d2);
                    pts[numPts++] = tri.v1 + a * (tri.v2 - tri.v1);
                }
                if ((d2 > 0.0f) != (d0 > 0.0f) && numPts < 2) {
                    float a = d2 / (d2 - d0);
                    pts[numPts++] = tri.v2 + a * (tri.v0 - tri.v2);
                }

                if (numPts == 2 && glm::distance(pts[0], pts[1]) > 1e-6f) {
                    segments.push_back({pts[0], pts[1]});
                }
            }

            if (!segments.empty()) {
                float totalLen = 0.0f;
                for (const auto& sm : segments) totalLen += glm::distance(sm.p0, sm.p1);
                float eps = std::max((totalLen / (float)segments.size()) * 0.8f, 1e-3f);
                float epsSq = eps * eps;

                std::vector<std::vector<glm::vec3>> loops;
                std::vector<bool> used(segments.size(), false);

                for (size_t i = 0; i < segments.size(); ++i) {
                    if (used[i]) continue;
                    used[i] = true;
                    std::deque<glm::vec3> chain;
                    chain.push_back(segments[i].p0);
                    chain.push_back(segments[i].p1);

                    // Expand forward
                    bool expand = true;
                    while (expand) {
                        glm::vec3 tip = chain.back();
                        glm::vec3 toStart = tip - chain.front();
                        if (chain.size() >= 3 && glm::dot(toStart, toStart) < epsSq * 4.0f) break;

                        int bestIdx = -1;
                        bool flip = false;
                        float bestD = epsSq * 9.0f;
                        for (size_t j = 0; j < segments.size(); ++j) {
                            if (used[j]) continue;
                            glm::vec3 d0 = tip - segments[j].p0;
                            float dist0 = glm::dot(d0, d0);
                            if (dist0 < bestD) { bestD = dist0; bestIdx = (int)j; flip = false; }
                            glm::vec3 d1 = tip - segments[j].p1;
                            float dist1 = glm::dot(d1, d1);
                            if (dist1 < bestD) { bestD = dist1; bestIdx = (int)j; flip = true; }
                        }
                        if (bestIdx != -1) {
                            used[bestIdx] = true;
                            chain.push_back(flip ? segments[bestIdx].p0 : segments[bestIdx].p1);
                        } else {
                            expand = false;
                        }
                    }

                    // Expand backward
                    expand = true;
                    while (expand) {
                        glm::vec3 tip = chain.front();
                        glm::vec3 toEnd = tip - chain.back();
                        if (chain.size() >= 3 && glm::dot(toEnd, toEnd) < epsSq * 4.0f) break;

                        int bestIdx = -1;
                        bool flip = false;
                        float bestD = epsSq * 9.0f;
                        for (size_t j = 0; j < segments.size(); ++j) {
                            if (used[j]) continue;
                            glm::vec3 d0 = tip - segments[j].p1;
                            float dist0 = glm::dot(d0, d0);
                            if (dist0 < bestD) { bestD = dist0; bestIdx = (int)j; flip = false; }
                            glm::vec3 d1 = tip - segments[j].p0;
                            float dist1 = glm::dot(d1, d1);
                            if (dist1 < bestD) { bestD = dist1; bestIdx = (int)j; flip = true; }
                        }
                        if (bestIdx != -1) {
                            used[bestIdx] = true;
                            chain.push_front(flip ? segments[bestIdx].p1 : segments[bestIdx].p0);
                        } else {
                            expand = false;
                        }
                    }

                    float gap = glm::distance(chain.front(), chain.back());
                    float perim = 0.0f;
                    for (size_t k = 0; k + 1 < chain.size(); ++k) perim += glm::distance(chain[k], chain[k + 1]);

                    if ((gap < eps * 3.0f || (chain.size() >= 6 && gap < perim * 0.45f)) && chain.size() >= 4) {
                        loops.emplace_back(chain.begin(), chain.end());
                    }
                }

                if (!loops.empty()) {
                    int bestLoop = -1;
                    float minLoopDist = 1e30f;
                    for (size_t l = 0; l < loops.size(); ++l) {
                        glm::vec3 lCent(0.0f);
                        for (const auto& p : loops[l]) lCent += p;
                        lCent /= (float)loops[l].size();

                        float d = glm::distance(lCent, refPt);
                        for (const auto& p : loops[l]) {
                            d = std::min(d, glm::distance(p, refPt));
                        }
                        if (d < minLoopDist) {
                            minLoopDist = d;
                            bestLoop = (int)l;
                        }
                    }

                    if (bestLoop >= 0) {
                        loop = std::move(loops[bestLoop]);
                        loopFound = true;
                    }
                }
            }
        }

        std::vector<glm::vec3> ringPts(radialSegments);

        if (loopFound && loop.size() >= 3) {
            glm::vec3 cLoop(0.0f);
            for (const auto& p : loop) cLoop += p;
            cLoop /= (float)loop.size();

            const int K = (int)loop.size();
            std::vector<glm::vec2> loop2D(K);
            for (int k = 0; k < K; ++k) {
                glm::vec3 dP = loop[k] - cLoop;
                loop2D[k] = glm::vec2(glm::dot(dP, uRing), glm::dot(dP, vRing));
            }

            float accumRadius = 0.0f;
            int validRadiusCount = 0;

            for (int j = 0; j < radialSegments; ++j) {
                float angle = 2.0f * 3.141592653589793f * ((float)j / (float)radialSegments);
                glm::vec2 rayDir(std::cos(angle), std::sin(angle));

                float bestDiff = 1e9f;
                glm::vec3 bestPt(0.0f);
                bool hit = false;

                for (int k = 0; k < K; ++k) {
                    glm::vec2 A = loop2D[k];
                    glm::vec2 B = loop2D[(k + 1) % K];
                    glm::vec2 delta = B - A;
                    float denom = rayDir.x * delta.y - rayDir.y * delta.x;
                    if (std::abs(denom) > 1e-6f) {
                        float alpha = (rayDir.x * A.y - rayDir.y * A.x) / denom;
                        if (alpha >= -1e-4f && alpha <= 1.0f + 1e-4f) {
                            float lambda = (A.x * B.y - A.y * B.x) / denom;
                            if (lambda > 0.001f) {
                                float diff = std::abs(lambda - estRadius);
                                if (!hit || diff < bestDiff) {
                                    bestDiff = diff;
                                    float clampedAlpha = std::clamp(alpha, 0.0f, 1.0f);
                                    bestPt = glm::mix(loop[k], loop[(k + 1) % K], clampedAlpha);
                                    hit = true;
                                }
                            }
                        }
                    }
                }

                if (hit) {
                    SnapResult snap = snapper.SnapClosestPoint(targetMesh, bestPt, 200.0f);
                    ringPts[j] = snap.valid ? snap.worldPos : bestPt;
                    accumRadius += glm::distance(ringPts[j], cLoop);
                    validRadiusCount++;
                } else {
                    glm::vec3 rawPt = cLoop + (rayDir.x * uRing + rayDir.y * vRing) * estRadius;
                    SnapResult snap = snapper.SnapClosestPoint(targetMesh, rawPt, 500.0f);
                    ringPts[j] = snap.valid ? snap.worldPos : rawPt;
                }
            }

            if (validRadiusCount > 0) {
                estRadius = glm::mix(estRadius, accumRadius / (float)validRadiusCount, 0.25f);
            }
        } else {
            // Fallback: Radial circle in the slice plane snapped to target surface
            for (int j = 0; j < radialSegments; ++j) {
                float angle = 2.0f * 3.141592653589793f * ((float)j / (float)radialSegments);
                glm::vec3 rawPt = curAxisPt + (std::cos(angle) * uRing + std::sin(angle) * vRing) * estRadius;
                SnapResult snap = snapper.SnapClosestPoint(targetMesh, rawPt, 500.0f);
                ringPts[j] = snap.valid ? snap.worldPos : rawPt;
            }
        }

        result.rings[ringIdx] = std::move(ringPts);
    }

    result.valid = true;
    result.quadsCount = (totalRings - 1) * radialSegments;
    return result;
}

TubeBuildResult QuadDrawMeshBuilder::CommitTubeMesh(
    Mesh* retopoMesh, const Mesh* targetMesh, SurfaceSnapper& snapper,
    const TubeSegmentResult& tubeGeom
) {
    TubeBuildResult result;
    if (!retopoMesh || !tubeGeom.valid || tubeGeom.rings.empty()) return result;

    int numRings = (int)tubeGeom.rings.size();
    int N = tubeGeom.radialSegments;
    if (numRings < 2 || N < 3) return result;

    std::vector<std::vector<int32_t>> ringVertIndices(numRings);
    for (int r = 0; r < numRings; ++r) {
        ringVertIndices[r].resize(N);
        for (int j = 0; j < N; ++j) {
            ringVertIndices[r][j] = AddVertex(retopoMesh, tubeGeom.rings[r][j]);
        }
    }

    int totalQuads = 0;
    for (int r = 0; r + 1 < numRings; ++r) {
        for (int j = 0; j < N; ++j) {
            int jNext = (j + 1) % N;
            int32_t a = ringVertIndices[r][j];
            int32_t b = ringVertIndices[r][jNext];
            int32_t c = ringVertIndices[r + 1][jNext];
            int32_t d = ringVertIndices[r + 1][j];

            glm::vec3 quadMid = (tubeGeom.rings[r][j] + tubeGeom.rings[r + 1][jNext]) * 0.5f;
            SnapResult snap = snapper.SnapClosestPoint(targetMesh, quadMid, 500.0f);
            glm::vec3 targetN = snap.valid ? snap.normal : glm::vec3(0.0f, 1.0f, 0.0f);

            if (AddQuad(retopoMesh, a, b, c, d, targetN)) {
                totalQuads++;
            }
        }
    }

    result.valid = true;
    result.newLoopVertices = ringVertIndices.back();
    result.quadsCreated = totalQuads;

    InvalidateEdgeCache();
    NotifyMeshUpdated(retopoMesh);
    return result;
}

} // namespace retopo
