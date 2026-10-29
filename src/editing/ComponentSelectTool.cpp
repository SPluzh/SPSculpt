#include "editing/ComponentSelectTool.h"
#include "editing/SculptManager.h"
#include "editing/ComponentOps.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "mesh/Mesh.h"
#include "imgui.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cmath>
#include <limits>

static bool rayTriangleIntersect(
    const glm::vec3& orig, const glm::vec3& dir,
    const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
    float& t
) {
    const float EPSILON = 1e-7f;
    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;
    glm::vec3 pvec = glm::cross(dir, edge2);
    float det = glm::dot(edge1, pvec);
    if (std::abs(det) < EPSILON) return false;
    float invDet = 1.0f / det;
    glm::vec3 tvec = orig - v0;
    float u = glm::dot(tvec, pvec) * invDet;
    if (u < 0.0f || u > 1.0f) return false;
    glm::vec3 qvec = glm::cross(tvec, edge1);
    float v = glm::dot(dir, qvec) * invDet;
    if (v < 0.0f || u + v > 1.0f) return false;
    t = glm::dot(edge2, qvec) * invDet;
    return t > EPSILON;
}

static float distToSegment2D(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b) {
    glm::vec2 ab = b - a;
    float l2 = glm::dot(ab, ab);
    if (l2 < 1e-6f) return glm::distance(p, a);
    float t = std::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f);
    return glm::distance(p, a + t * ab);
}

ComponentSelectTool::ComponentSelectTool(SculptManager& sm)
    : m_sculpt(sm)
    , m_lastClickTime(std::chrono::steady_clock::now())
{
}

SelectionManager& ComponentSelectTool::getSelectionManager() {
    return m_sculpt.getSelectionManager();
}

const SelectionManager& ComponentSelectTool::getSelectionManager() const {
    return m_sculpt.getSelectionManager();
}

void ComponentSelectTool::onActivate(Scene& scene) {
    Mesh* mesh = scene.getSelected();
    if (mesh) {
        getSelectionManager().ensureEdgeTopology(*mesh);
    }
    m_hoveredVert = 0xffffffff;
    m_hoveredEdge = 0xffffffff;
    m_hoveredFace = 0xffffffff;
    m_isBoxSelecting = false;
}

void ComponentSelectTool::onDeactivate() {
    if (m_isBevelActive) {
        m_isBevelActive = false;
        m_isBevelDragging = false;
        m_bevelBackupMesh.reset();
        m_bevelSavedVerts.clear();
        m_bevelSavedEdges.clear();
        m_bevelSavedFaces.clear();
    }
    m_hoveredVert = 0xffffffff;
    m_hoveredEdge = 0xffffffff;
    m_hoveredFace = 0xffffffff;
    m_isBoxSelecting = false;
}

bool ComponentSelectTool::startInteractiveBevel(Scene& scene, float initWidth, int initSegments, BevelType initType, float initHeight) {
    Mesh* mesh = scene.getSelected();
    if (!mesh || mesh->nbVerts == 0 || mesh->nbFaces == 0) return false;

    auto& selMgr = getSelectionManager();
    if (!selMgr.hasSelection()) return false;

    // Backup current mesh geometry, topology and selection state
    m_bevelBackupMesh = std::make_unique<Mesh>();
    m_bevelBackupMesh->copyDataFrom(*mesh);

    m_bevelSavedMode = selMgr.getMode();
    m_bevelSavedVerts = selMgr.getSelectedVertices();
    m_bevelSavedEdges = selMgr.getSelectedEdges();
    m_bevelSavedFaces = selMgr.getSelectedFaces();

    m_bevelMeshRadius = mesh->computeLocalRadius();
    if (m_bevelMeshRadius < 0.001f) m_bevelMeshRadius = 1.0f;

    m_bevelWidth = std::clamp(initWidth, 0.005f, 0.99f);
    m_bevelSegments = std::clamp(initSegments, 1, 10);
    m_bevelType = initType;
    m_bevelHeight = initHeight;

    m_isBevelActive = true;
    m_isBevelDragging = false;

    // Perform initial live preview
    updateBevelLive(scene, false);
    return true;
}

void ComponentSelectTool::setBevelParams(Scene& scene, float width, int segments, BevelType type, float height) {
    if (!m_isBevelActive) return;
    m_bevelWidth = std::clamp(width, 0.005f, 0.99f);
    m_bevelSegments = std::clamp(segments, 1, 10);
    m_bevelType = type;
    m_bevelHeight = height;
    updateBevelLive(scene, false);
}

void ComponentSelectTool::updateBevelLive(Scene& scene, bool finalApply) {
    if (!m_isBevelActive || !m_bevelBackupMesh) return;
    Mesh* mesh = scene.getSelected();
    if (!mesh) return;

    // 1. Restore mesh from backup
    mesh->copyDataFrom(*m_bevelBackupMesh);

    // 2. Restore selection state into selection manager
    auto& selMgr = getSelectionManager();
    selMgr.clear();
    selMgr.setMode(m_bevelSavedMode);
    for (uint32_t v : m_bevelSavedVerts) selMgr.selectVertex(v, true);
    for (uint32_t e : m_bevelSavedEdges) selMgr.selectEdge(e, true);
    for (uint32_t f : m_bevelSavedFaces) selMgr.selectFace(f, true);

    // 3. Apply bevel operation
    ComponentOps::bevelSelection(*mesh, selMgr, m_bevelWidth, m_bevelSegments, m_bevelType, m_bevelHeight);

    // 4. Update mesh state (fast update during live drag, full BVH build on final apply)
    mesh->postInit(finalApply);
    mesh->invalidateDynamicSubdiv();
    mesh->isDirty = true;
    mesh->isTopologyDirty = true;
    selMgr.invalidateTopologyCache();
}

void ComponentSelectTool::applyInteractiveBevel(Scene& scene) {
    if (!m_isBevelActive) return;
    Mesh* mesh = scene.getSelected();
    if (mesh && m_bevelBackupMesh) {
        updateBevelLive(scene, true);
        scene.pushHistoryState();
    }
    m_isBevelActive = false;
    m_isBevelDragging = false;
    m_bevelBackupMesh.reset();
    m_bevelSavedVerts.clear();
    m_bevelSavedEdges.clear();
    m_bevelSavedFaces.clear();
}

void ComponentSelectTool::cancelInteractiveBevel(Scene& scene) {
    if (!m_isBevelActive) return;
    Mesh* mesh = scene.getSelected();
    if (mesh && m_bevelBackupMesh) {
        mesh->copyDataFrom(*m_bevelBackupMesh);
        auto& selMgr = getSelectionManager();
        selMgr.clear();
        selMgr.setMode(m_bevelSavedMode);
        for (uint32_t v : m_bevelSavedVerts) selMgr.selectVertex(v, true);
        for (uint32_t e : m_bevelSavedEdges) selMgr.selectEdge(e, true);
        for (uint32_t f : m_bevelSavedFaces) selMgr.selectFace(f, true);

        mesh->postInit(true);
        mesh->invalidateDynamicSubdiv();
        mesh->isDirty = true;
        mesh->isTopologyDirty = true;
        selMgr.invalidateTopologyCache();
    }
    m_isBevelActive = false;
    m_isBevelDragging = false;
    m_bevelBackupMesh.reset();
    m_bevelSavedVerts.clear();
    m_bevelSavedEdges.clear();
    m_bevelSavedFaces.clear();
}

bool ComponentSelectTool::handleKeyDown(Scene& scene, int keycode) {
    if (m_isBevelActive) {
        if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER || keycode == SDLK_SPACE) {
            applyInteractiveBevel(scene);
            return true;
        } else if (keycode == SDLK_ESCAPE) {
            cancelInteractiveBevel(scene);
            return true;
        } else if (keycode == SDLK_TAB || keycode == SDLK_s) {
            m_bevelType = (m_bevelType == BevelType::Chamfer) ? BevelType::Solid : BevelType::Chamfer;
            updateBevelLive(scene, false);
            return true;
        } else if (keycode == SDLK_UP || keycode == SDLK_RIGHT || keycode == SDLK_PLUS || keycode == SDLK_KP_PLUS || keycode == SDLK_EQUALS) {
            m_bevelSegments = std::clamp(m_bevelSegments + 1, 1, 10);
            updateBevelLive(scene, false);
            return true;
        } else if (keycode == SDLK_DOWN || keycode == SDLK_LEFT || keycode == SDLK_MINUS || keycode == SDLK_KP_MINUS) {
            m_bevelSegments = std::clamp(m_bevelSegments - 1, 1, 10);
            updateBevelLive(scene, false);
            return true;
        }
    }
    return false;
}

bool ComponentSelectTool::handleMouseWheel(Scene& scene, int wheelY) {
    if (!m_isBevelActive) return false;
    if (wheelY > 0) {
        m_bevelSegments = std::clamp(m_bevelSegments + 1, 1, 10);
    } else if (wheelY < 0) {
        m_bevelSegments = std::clamp(m_bevelSegments - 1, 1, 10);
    }
    updateBevelLive(scene, false);
    return true;
}

void ComponentSelectTool::updateHover(Scene& scene, const Camera& camera, float mouseX, float mouseY) {
    m_hoveredVert = 0xffffffff;
    m_hoveredEdge = 0xffffffff;
    m_hoveredFace = 0xffffffff;

    Mesh* mesh = scene.getSelected();
    if (!mesh || mesh->nbFaces == 0 || mesh->nbVerts == 0) return;

    Ray ray = camera.getRay(mouseX, mouseY);
    glm::mat4 invMatrix = glm::inverse(mesh->matrix);
    glm::vec3 localRayOrigin = glm::vec3(invMatrix * glm::vec4(ray.origin, 1.0f));
    glm::vec3 localRayDir = glm::normalize(glm::vec3(invMatrix * glm::vec4(ray.dir, 0.0f)));

    std::vector<uint32_t> candidateFaces = mesh->octree.collectIntersectRay(
        localRayOrigin.x, localRayOrigin.y, localRayOrigin.z,
        localRayDir.x, localRayDir.y, localRayDir.z
    );

    float localMinT = std::numeric_limits<float>::infinity();
    uint32_t localFaceId = 0xffffffff;

    auto testFace = [&](uint32_t faceId) {
        if (faceId >= (uint32_t)mesh->nbFaces) return;
        uint32_t v0Id = mesh->faces[faceId * 4];
        uint32_t v1Id = mesh->faces[faceId * 4 + 1];
        uint32_t v2Id = mesh->faces[faceId * 4 + 2];
        uint32_t v3Id = mesh->faces[faceId * 4 + 3];

        if (!mesh->faceVisible.empty() && !mesh->faceVisible[faceId]) return;
        if (!mesh->vertVisible.empty()) {
            if (v0Id >= (uint32_t)mesh->nbVerts || !mesh->vertVisible[v0Id]) return;
            if (v1Id >= (uint32_t)mesh->nbVerts || !mesh->vertVisible[v1Id]) return;
            if (v2Id >= (uint32_t)mesh->nbVerts || !mesh->vertVisible[v2Id]) return;
            if (v3Id != 0xffffffff && (v3Id >= (uint32_t)mesh->nbVerts || !mesh->vertVisible[v3Id])) return;
        }

        glm::vec3 v0(mesh->verts[v0Id * 3], mesh->verts[v0Id * 3 + 1], mesh->verts[v0Id * 3 + 2]);
        glm::vec3 v1(mesh->verts[v1Id * 3], mesh->verts[v1Id * 3 + 1], mesh->verts[v1Id * 3 + 2]);
        glm::vec3 v2(mesh->verts[v2Id * 3], mesh->verts[v2Id * 3 + 1], mesh->verts[v2Id * 3 + 2]);

        float t;
        if (rayTriangleIntersect(localRayOrigin, localRayDir, v0, v1, v2, t)) {
            if (t < localMinT) {
                localMinT = t;
                localFaceId = faceId;
            }
        }

        if (v3Id != 0xffffffff) {
            glm::vec3 v3(mesh->verts[v3Id * 3], mesh->verts[v3Id * 3 + 1], mesh->verts[v3Id * 3 + 2]);
            if (rayTriangleIntersect(localRayOrigin, localRayDir, v0, v2, v3, t)) {
                if (t < localMinT) {
                    localMinT = t;
                    localFaceId = faceId;
                }
            }
        }
    };

    for (uint32_t faceId : candidateFaces) {
        testFace(faceId);
    }

    if (localFaceId == 0xffffffff && mesh->nbFaces <= 4000) {
        for (int f = 0; f < mesh->nbFaces; ++f) {
            testFace((uint32_t)f);
        }
    }

    ComponentMode mode = getSelectionManager().getMode();
    if (mode == ComponentMode::Face) {
        m_hoveredFace = localFaceId;
        return;
    }

    glm::vec2 mousePos(mouseX, mouseY);
    const glm::mat4& m2w = mesh->matrix;

    float hitWorldDepth = std::numeric_limits<float>::infinity();
    if (localFaceId != 0xffffffff) {
        glm::vec3 hitLocal = localRayOrigin + localRayDir * localMinT;
        glm::vec3 hitWorld = glm::vec3(m2w * glm::vec4(hitLocal, 1.0f));
        hitWorldDepth = glm::dot(hitWorld - ray.origin, ray.dir);
    }

    if (mode == ComponentMode::Vertex) {
        float bestDist = 24.0f; // pixel radius threshold
        uint32_t bestVert = 0xffffffff;
        float bestDepth = std::numeric_limits<float>::infinity();
        float occlusionTolerance = (hitWorldDepth < 1e8f) ? std::max(hitWorldDepth * 0.05f, 0.05f) : 1e8f;

        // 1. If ray hit a face, test that face's vertices first
        if (localFaceId != 0xffffffff) {
            int numV = (mesh->faces[localFaceId * 4 + 3] != 0xffffffff) ? 4 : 3;
            for (int k = 0; k < numV; ++k) {
                uint32_t vIdx = mesh->faces[localFaceId * 4 + k];
                if (vIdx >= (uint32_t)mesh->nbVerts) continue;
                if (!mesh->vertVisible.empty() && !mesh->vertVisible[vIdx]) continue;

                glm::vec3 vLocal(mesh->verts[vIdx * 3], mesh->verts[vIdx * 3 + 1], mesh->verts[vIdx * 3 + 2]);
                glm::vec3 vWorld = glm::vec3(m2w * glm::vec4(vLocal, 1.0f));
                float depth = glm::dot(vWorld - ray.origin, ray.dir);
                if (depth <= 0.0f) continue;

                glm::vec3 vScreen = camera.project(vWorld);
                if (vScreen.z <= 0.0f) continue;

                float d = glm::distance(mousePos, glm::vec2(vScreen.x, vScreen.y));
                if (d < bestDist) {
                    bestDist = d;
                    bestVert = vIdx;
                    bestDepth = depth;
                }
            }
        }

        // 2. Also test all mesh vertices so boundary/silhouette vertices are never missed
        for (int i = 0; i < mesh->nbVerts; ++i) {
            if (!mesh->vertVisible.empty() && !mesh->vertVisible[i]) continue;

            glm::vec3 vLocal(mesh->verts[i * 3], mesh->verts[i * 3 + 1], mesh->verts[i * 3 + 2]);
            glm::vec3 vWorld = glm::vec3(m2w * glm::vec4(vLocal, 1.0f));
            float depth = glm::dot(vWorld - ray.origin, ray.dir);
            if (depth <= 0.0f) continue;
            if (hitWorldDepth < 1e8f && depth > hitWorldDepth + occlusionTolerance) {
                continue; // Occluded behind solid front face
            }

            glm::vec3 vScreen = camera.project(vWorld);
            if (vScreen.z <= 0.0f) continue;

            float d = glm::distance(mousePos, glm::vec2(vScreen.x, vScreen.y));
            if (d < bestDist) {
                bestDist = d;
                bestVert = (uint32_t)i;
                bestDepth = depth;
            } else if (d <= 24.0f && depth + occlusionTolerance < bestDepth && d < bestDist + 6.0f) {
                // Foreground vertex priority over background
                bestDist = d;
                bestVert = (uint32_t)i;
                bestDepth = depth;
            }
        }
        m_hoveredVert = bestVert;
    } else if (mode == ComponentMode::Edge) {
        getSelectionManager().ensureEdgeTopology(*mesh);
        const auto& edgeTop = getSelectionManager().getEdgeTopology(*mesh);

        float bestDist = 18.0f; // pixel edge distance threshold
        uint32_t bestEdge = 0xffffffff;
        float bestDepth = std::numeric_limits<float>::infinity();
        float occlusionTolerance = (hitWorldDepth < 1e8f) ? std::max(hitWorldDepth * 0.05f, 0.05f) : 1e8f;

        // 1. If ray hit a face, check that face's edges first
        if (localFaceId != 0xffffffff && mesh->faceEdges.size() >= (size_t)(mesh->nbFaces * 4)) {
            int numV = (mesh->faces[localFaceId * 4 + 3] != 0xffffffff) ? 4 : 3;
            for (int k = 0; k < numV; ++k) {
                uint32_t eIdx = mesh->faceEdges[localFaceId * 4 + k];
                if (eIdx >= edgeTop.size()) continue;
                uint32_t va = edgeTop[eIdx].v0;
                uint32_t vb = edgeTop[eIdx].v1;
                if (va >= (uint32_t)mesh->nbVerts || vb >= (uint32_t)mesh->nbVerts) continue;
                if (!mesh->vertVisible.empty() && (!mesh->vertVisible[va] || !mesh->vertVisible[vb])) continue;

                glm::vec3 p0Local(mesh->verts[va * 3], mesh->verts[va * 3 + 1], mesh->verts[va * 3 + 2]);
                glm::vec3 p1Local(mesh->verts[vb * 3], mesh->verts[vb * 3 + 1], mesh->verts[vb * 3 + 2]);
                glm::vec3 p0World = glm::vec3(m2w * glm::vec4(p0Local, 1.0f));
                glm::vec3 p1World = glm::vec3(m2w * glm::vec4(p1Local, 1.0f));
                glm::vec3 s0 = camera.project(p0World);
                glm::vec3 s1 = camera.project(p1World);
                if (s0.z <= 0.0f || s1.z <= 0.0f) continue;

                float d = distToSegment2D(mousePos, glm::vec2(s0.x, s0.y), glm::vec2(s1.x, s1.y));
                if (d < bestDist) {
                    bestDist = d;
                    bestEdge = eIdx;
                    bestDepth = glm::dot(((p0World + p1World) * 0.5f) - ray.origin, ray.dir);
                }
            }
        }

        // 2. Also test all mesh edges (boundary edges, hole borders, silhouette edges)
        for (size_t e = 0; e < edgeTop.size(); ++e) {
            uint32_t va = edgeTop[e].v0;
            uint32_t vb = edgeTop[e].v1;
            if (va >= (uint32_t)mesh->nbVerts || vb >= (uint32_t)mesh->nbVerts) continue;
            if (!mesh->vertVisible.empty() && (!mesh->vertVisible[va] || !mesh->vertVisible[vb])) continue;

            glm::vec3 p0Local(mesh->verts[va * 3], mesh->verts[va * 3 + 1], mesh->verts[va * 3 + 2]);
            glm::vec3 p1Local(mesh->verts[vb * 3], mesh->verts[vb * 3 + 1], mesh->verts[vb * 3 + 2]);
            glm::vec3 p0World = glm::vec3(m2w * glm::vec4(p0Local, 1.0f));
            glm::vec3 p1World = glm::vec3(m2w * glm::vec4(p1Local, 1.0f));

            glm::vec3 midWorld = (p0World + p1World) * 0.5f;
            float depth = glm::dot(midWorld - ray.origin, ray.dir);
            if (depth <= 0.0f) continue;
            if (hitWorldDepth < 1e8f && depth > hitWorldDepth + occlusionTolerance) {
                continue; // Occluded behind solid front face
            }

            glm::vec3 s0 = camera.project(p0World);
            glm::vec3 s1 = camera.project(p1World);
            if (s0.z <= 0.0f || s1.z <= 0.0f) continue;

            float minSx = std::min(s0.x, s1.x) - bestDist;
            float maxSx = std::max(s0.x, s1.x) + bestDist;
            float minSy = std::min(s0.y, s1.y) - bestDist;
            float maxSy = std::max(s0.y, s1.y) + bestDist;
            if (mouseX < minSx || mouseX > maxSx || mouseY < minSy || mouseY > maxSy) {
                continue;
            }

            float d = distToSegment2D(mousePos, glm::vec2(s0.x, s0.y), glm::vec2(s1.x, s1.y));
            if (d < bestDist) {
                bestDist = d;
                bestEdge = (uint32_t)e;
                bestDepth = depth;
            } else if (d <= 18.0f && depth + occlusionTolerance < bestDepth && d < bestDist + 4.0f) {
                bestDist = d;
                bestEdge = (uint32_t)e;
                bestDepth = depth;
            }
        }
        m_hoveredEdge = bestEdge;
    }
}

bool ComponentSelectTool::handleMouseDown(
    Scene& scene, const Camera& camera,
    float mouseX, float mouseY,
    bool ctrl, bool shift, int button
) {
    if (m_isBevelActive) {
        if (button == 1) {
            m_isBevelDragging = true;
            m_bevelDragStartMouse = glm::vec2(mouseX, mouseY);
            m_bevelDragStartWidth = m_bevelWidth;
            m_bevelDragStartHeight = m_bevelHeight;
            return true;
        } else if (button == 3) {
            cancelInteractiveBevel(scene);
            return true;
        }
        return false;
    }

    if (button != 1) return false; // Left mouse button only

    Mesh* mesh = scene.getSelected();
    if (!mesh) return false;

    updateHover(scene, camera, mouseX, mouseY);
    auto& selMgr = getSelectionManager();
    ComponentMode mode = selMgr.getMode();

    auto now = std::chrono::steady_clock::now();
    double msSinceLast = std::chrono::duration<double, std::milli>(now - m_lastClickTime).count();

    // Double click detection for Edge Loop
    if (mode == ComponentMode::Edge && m_hoveredEdge != 0xffffffff) {
        if (m_hoveredEdge == m_lastClickedEdge && msSinceLast < 350.0) {
            selMgr.selectEdgeLoop(*mesh, m_hoveredEdge, shift);
            m_lastClickedEdge = 0xffffffff;
            return true;
        }
    }

    if (mode == ComponentMode::Vertex) {
        if (m_hoveredVert != 0xffffffff) {
            if (shift) selMgr.selectVertex(m_hoveredVert, true);
            else if (ctrl) selMgr.deselectVertex(m_hoveredVert);
            else selMgr.toggleVertex(m_hoveredVert);
            return true;
        }
    } else if (mode == ComponentMode::Edge) {
        if (m_hoveredEdge != 0xffffffff) {
            m_lastClickedEdge = m_hoveredEdge;
            m_lastClickTime = now;
            if (shift) selMgr.selectEdge(m_hoveredEdge, true);
            else if (ctrl) selMgr.deselectEdge(m_hoveredEdge);
            else selMgr.toggleEdge(m_hoveredEdge);
            return true;
        }
    } else if (mode == ComponentMode::Face) {
        if (m_hoveredFace != 0xffffffff) {
            if (shift) selMgr.selectFace(m_hoveredFace, true);
            else if (ctrl) selMgr.deselectFace(m_hoveredFace);
            else selMgr.toggleFace(m_hoveredFace);
            return true;
        }
    }

    // Clicked in empty space -> Start box select
    m_isBoxSelecting = true;
    m_boxStart = glm::vec2(mouseX, mouseY);
    m_boxCurrent = m_boxStart;
    return true;
}

bool ComponentSelectTool::handleMouseMove(
    Scene& scene, const Camera& camera,
    float mouseX, float mouseY,
    bool ctrl, bool shift
) {
    (void)ctrl; (void)shift;
    if (m_isBevelActive) {
        if (m_isBevelDragging) {
            float dx = mouseX - m_bevelDragStartMouse.x;
            float dy = mouseY - m_bevelDragStartMouse.y;
            float deltaW = (dx / 250.0f);
            m_bevelWidth = std::clamp(m_bevelDragStartWidth + deltaW, 0.005f, 0.99f);

            float deltaH = (-dy / 200.0f) * m_bevelMeshRadius;
            m_bevelHeight = m_bevelDragStartHeight + deltaH;

            updateBevelLive(scene, false);
            return true;
        }
        return false;
    }

    if (m_isBoxSelecting) {
        m_boxCurrent = glm::vec2(mouseX, mouseY);
        return true;
    }

    updateHover(scene, camera, mouseX, mouseY);
    return false;
}

bool ComponentSelectTool::handleMouseUp(Scene& scene, const Camera& camera, int button) {
    if (m_isBevelActive) {
        if (button == 1 && m_isBevelDragging) {
            m_isBevelDragging = false;
            return true;
        }
        return false;
    }

    if (button != 1) return false;

    if (m_isBoxSelecting) {
        float d = glm::distance(m_boxStart, m_boxCurrent);
        bool ctrl = (ImGui::GetIO().KeyCtrl);
        bool shift = (ImGui::GetIO().KeyShift);

        if (d > 6.0f) {
            finishBoxSelect(scene, camera, ctrl, shift);
        } else if (!shift && !ctrl) {
            // Clicked empty space without dragging -> Clear selection
            getSelectionManager().clear();
        }
        m_isBoxSelecting = false;
        return true;
    }
    return false;
}

void ComponentSelectTool::finishBoxSelect(Scene& scene, const Camera& camera, bool ctrl, bool shift) {
    Mesh* mesh = scene.getSelected();
    if (!mesh) return;

    float minX = std::min(m_boxStart.x, m_boxCurrent.x);
    float maxX = std::max(m_boxStart.x, m_boxCurrent.x);
    float minY = std::min(m_boxStart.y, m_boxCurrent.y);
    float maxY = std::max(m_boxStart.y, m_boxCurrent.y);

    auto& selMgr = getSelectionManager();
    ComponentMode mode = selMgr.getMode();

    if (!shift && !ctrl) {
        selMgr.clearMode(mode);
    }

    auto inBox = [&](float x, float y) {
        return x >= minX && x <= maxX && y >= minY && y <= maxY;
    };

    if (mode == ComponentMode::Vertex) {
        for (int i = 0; i < mesh->nbVerts; ++i) {
            if (!mesh->vertVisible.empty() && !mesh->vertVisible[i]) continue;
            glm::vec3 vLocal(mesh->verts[i * 3], mesh->verts[i * 3 + 1], mesh->verts[i * 3 + 2]);
            glm::vec3 vWorld = glm::vec3(mesh->matrix * glm::vec4(vLocal, 1.0f));
            glm::vec3 s = camera.project(vWorld);
            if (s.z > 0.0f && inBox(s.x, s.y)) {
                if (ctrl) selMgr.deselectVertex((uint32_t)i);
                else selMgr.selectVertex((uint32_t)i, true);
            }
        }
    } else if (mode == ComponentMode::Edge) {
        selMgr.ensureEdgeTopology(*mesh);
        const auto& edgeTop = selMgr.getEdgeTopology(*mesh);
        for (size_t e = 0; e < edgeTop.size(); ++e) {
            uint32_t va = edgeTop[e].v0;
            uint32_t vb = edgeTop[e].v1;
            if (va >= (uint32_t)mesh->nbVerts || vb >= (uint32_t)mesh->nbVerts) continue;
            if (!mesh->vertVisible.empty() && (!mesh->vertVisible[va] || !mesh->vertVisible[vb])) continue;

            glm::vec3 v0(mesh->verts[va * 3], mesh->verts[va * 3 + 1], mesh->verts[va * 3 + 2]);
            glm::vec3 v1(mesh->verts[vb * 3], mesh->verts[vb * 3 + 1], mesh->verts[vb * 3 + 2]);
            glm::vec3 mid = (v0 + v1) * 0.5f;
            glm::vec3 midWorld = glm::vec3(mesh->matrix * glm::vec4(mid, 1.0f));
            glm::vec3 s = camera.project(midWorld);
            glm::vec3 s0 = camera.project(glm::vec3(mesh->matrix * glm::vec4(v0, 1.0f)));
            glm::vec3 s1 = camera.project(glm::vec3(mesh->matrix * glm::vec4(v1, 1.0f)));
            if (s.z > 0.0f && (inBox(s.x, s.y) || (inBox(s0.x, s0.y) && inBox(s1.x, s1.y)))) {
                if (ctrl) selMgr.deselectEdge((uint32_t)e);
                else selMgr.selectEdge((uint32_t)e, true);
            }
        }
    } else if (mode == ComponentMode::Face) {
        for (int f = 0; f < mesh->nbFaces; ++f) {
            if (!mesh->faceVisible.empty() && !mesh->faceVisible[f]) continue;
            int id = f * 4;
            int numV = (mesh->faces[id + 3] != 0xffffffff) ? 4 : 3;
            glm::vec3 centerLocal(0.0f);
            for (int k = 0; k < numV; ++k) {
                uint32_t v = mesh->faces[id + k];
                centerLocal += glm::vec3(mesh->verts[v * 3], mesh->verts[v * 3 + 1], mesh->verts[v * 3 + 2]);
            }
            centerLocal /= (float)numV;
            glm::vec3 centerWorld = glm::vec3(mesh->matrix * glm::vec4(centerLocal, 1.0f));
            glm::vec3 s = camera.project(centerWorld);
            if (s.z > 0.0f && inBox(s.x, s.y)) {
                if (ctrl) selMgr.deselectFace((uint32_t)f);
                else selMgr.selectFace((uint32_t)f, true);
            }
        }
    }
}

void ComponentSelectTool::renderOverlay(Scene& scene, const Camera& camera, float vW, float vH) {
    (void)vW; (void)vH;
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    if (!drawList) return;

    Mesh* mesh = scene.getSelected();
    if (!mesh || mesh->nbVerts == 0) return;

    auto& selMgr = getSelectionManager();
    ComponentMode mode = selMgr.getMode();

    // 1. Draw Box Select Marquee
    if (m_isBoxSelecting) {
        float minX = std::min(m_boxStart.x, m_boxCurrent.x);
        float maxX = std::max(m_boxStart.x, m_boxCurrent.x);
        float minY = std::min(m_boxStart.y, m_boxCurrent.y);
        float maxY = std::max(m_boxStart.y, m_boxCurrent.y);

        drawList->AddRectFilled(ImVec2(minX, minY), ImVec2(maxX, maxY), IM_COL32(255, 200, 0, 35));
        drawList->AddRect(ImVec2(minX, minY), ImVec2(maxX, maxY), IM_COL32(255, 220, 50, 220), 0.0f, 0, 1.5f);
    }

    // 2. Draw Selected Faces
    if (!selMgr.getSelectedFaces().empty()) {
        for (uint32_t f : selMgr.getSelectedFaces()) {
            if (f >= (uint32_t)mesh->nbFaces) continue;
            int id = f * 4;
            int numV = (mesh->faces[id + 3] != 0xffffffff) ? 4 : 3;
            ImVec2 poly[4];
            bool allInFront = true;
            for (int k = 0; k < numV; ++k) {
                uint32_t v = mesh->faces[id + k];
                glm::vec3 posLocal(mesh->verts[v * 3], mesh->verts[v * 3 + 1], mesh->verts[v * 3 + 2]);
                glm::vec3 posWorld = glm::vec3(mesh->matrix * glm::vec4(posLocal, 1.0f));
                glm::vec3 s = camera.project(posWorld);
                if (s.z < 0.0f) { allInFront = false; break; }
                poly[k] = ImVec2(s.x, s.y);
            }
            if (allInFront) {
                drawList->AddConvexPolyFilled(poly, numV, IM_COL32(255, 180, 0, 90));
                drawList->AddPolyline(poly, numV, IM_COL32(255, 220, 50, 230), ImDrawFlags_Closed, 1.8f);
            }
        }
    }

    // 3. Draw Selected Edges
    if (!selMgr.getSelectedEdges().empty()) {
        selMgr.ensureEdgeTopology(*mesh);
        const auto& edgeTop = selMgr.getEdgeTopology(*mesh);
        for (uint32_t e : selMgr.getSelectedEdges()) {
            if (e >= edgeTop.size()) continue;
            uint32_t va = edgeTop[e].v0;
            uint32_t vb = edgeTop[e].v1;
            if (va >= (uint32_t)mesh->nbVerts || vb >= (uint32_t)mesh->nbVerts) continue;

            glm::vec3 v0(mesh->verts[va * 3], mesh->verts[va * 3 + 1], mesh->verts[va * 3 + 2]);
            glm::vec3 v1(mesh->verts[vb * 3], mesh->verts[vb * 3 + 1], mesh->verts[vb * 3 + 2]);
            glm::vec3 w0 = glm::vec3(mesh->matrix * glm::vec4(v0, 1.0f));
            glm::vec3 w1 = glm::vec3(mesh->matrix * glm::vec4(v1, 1.0f));
            glm::vec3 s0 = camera.project(w0);
            glm::vec3 s1 = camera.project(w1);
            if (s0.z > 0.0f && s1.z > 0.0f) {
                drawList->AddLine(ImVec2(s0.x, s0.y), ImVec2(s1.x, s1.y), IM_COL32(255, 140, 0, 255), 2.5f);
            }
        }
    }

    // 4. Draw Selected Vertices
    if (!selMgr.getSelectedVertices().empty()) {
        for (uint32_t v : selMgr.getSelectedVertices()) {
            if (v >= (uint32_t)mesh->nbVerts) continue;
            glm::vec3 vLocal(mesh->verts[v * 3], mesh->verts[v * 3 + 1], mesh->verts[v * 3 + 2]);
            glm::vec3 w = glm::vec3(mesh->matrix * glm::vec4(vLocal, 1.0f));
            glm::vec3 s = camera.project(w);
            if (s.z > 0.0f) {
                drawList->AddCircleFilled(ImVec2(s.x, s.y), 4.5f, IM_COL32(255, 215, 0, 255));
                drawList->AddCircle(ImVec2(s.x, s.y), 4.5f, IM_COL32(30, 30, 30, 240), 0, 1.5f);
            }
        }
    }

    // 5. Draw Hovered Element Highlight
    if (mode == ComponentMode::Vertex && m_hoveredVert != 0xffffffff && m_hoveredVert < (uint32_t)mesh->nbVerts) {
        glm::vec3 vLocal(mesh->verts[m_hoveredVert * 3], mesh->verts[m_hoveredVert * 3 + 1], mesh->verts[m_hoveredVert * 3 + 2]);
        glm::vec3 w = glm::vec3(mesh->matrix * glm::vec4(vLocal, 1.0f));
        glm::vec3 s = camera.project(w);
        if (s.z > 0.0f) {
            drawList->AddCircleFilled(ImVec2(s.x, s.y), 6.0f, IM_COL32(50, 255, 120, 255));
            drawList->AddCircle(ImVec2(s.x, s.y), 6.0f, IM_COL32(0, 0, 0, 255), 0, 1.5f);
        }
    } else if (mode == ComponentMode::Edge && m_hoveredEdge != 0xffffffff) {
        selMgr.ensureEdgeTopology(*mesh);
        const auto& edgeTop = selMgr.getEdgeTopology(*mesh);
        if (m_hoveredEdge < edgeTop.size()) {
            uint32_t va = edgeTop[m_hoveredEdge].v0;
            uint32_t vb = edgeTop[m_hoveredEdge].v1;
            if (va < (uint32_t)mesh->nbVerts && vb < (uint32_t)mesh->nbVerts) {
                glm::vec3 v0(mesh->verts[va * 3], mesh->verts[va * 3 + 1], mesh->verts[va * 3 + 2]);
                glm::vec3 v1(mesh->verts[vb * 3], mesh->verts[vb * 3 + 1], mesh->verts[vb * 3 + 2]);
                glm::vec3 s0 = camera.project(glm::vec3(mesh->matrix * glm::vec4(v0, 1.0f)));
                glm::vec3 s1 = camera.project(glm::vec3(mesh->matrix * glm::vec4(v1, 1.0f)));
                if (s0.z > 0.0f && s1.z > 0.0f) {
                    drawList->AddLine(ImVec2(s0.x, s0.y), ImVec2(s1.x, s1.y), IM_COL32(50, 255, 120, 255), 3.5f);
                }
            }
        }
    } else if (mode == ComponentMode::Face && m_hoveredFace != 0xffffffff && m_hoveredFace < (uint32_t)mesh->nbFaces) {
        int id = m_hoveredFace * 4;
        int numV = (mesh->faces[id + 3] != 0xffffffff) ? 4 : 3;
        ImVec2 poly[4];
        bool allInFront = true;
        for (int k = 0; k < numV; ++k) {
            uint32_t v = mesh->faces[id + k];
            glm::vec3 posLocal(mesh->verts[v * 3], mesh->verts[v * 3 + 1], mesh->verts[v * 3 + 2]);
            glm::vec3 posWorld = glm::vec3(mesh->matrix * glm::vec4(posLocal, 1.0f));
            glm::vec3 s = camera.project(posWorld);
            if (s.z < 0.0f) { allInFront = false; break; }
            poly[k] = ImVec2(s.x, s.y);
        }
        if (allInFront) {
            drawList->AddConvexPolyFilled(poly, numV, IM_COL32(50, 255, 120, 110));
            drawList->AddPolyline(poly, numV, IM_COL32(50, 255, 120, 255), ImDrawFlags_Closed, 2.5f);
        }
    }

    // 6. Interactive Bevel Viewport HUD Badge
    if (m_isBevelActive) {
        const char* typeStr = (m_bevelType == BevelType::Solid) ? "SOLID" : "CHAMFER";
        char buf[128];
        std::snprintf(buf, sizeof(buf), "BEVEL [%s]: Size: %.2f | Segments: %d", typeStr, m_bevelWidth, m_bevelSegments);
        const char* hint = "[LMB Drag] Size | [Wheel] Segments | [Tab/S] Mode | [Enter] Apply | [Esc] Cancel";

        ImVec2 textSz1 = ImGui::CalcTextSize(buf);
        ImVec2 textSz2 = ImGui::CalcTextSize(hint);
        float boxW = std::max(textSz1.x, textSz2.x) + 32.0f;
        float boxH = textSz1.y + textSz2.y + 22.0f;

        float centerX = vW * 0.5f;
        float topY = 45.0f;

        ImVec2 rMin(centerX - boxW * 0.5f, topY);
        ImVec2 rMax(centerX + boxW * 0.5f, topY + boxH);

        drawList->AddRectFilled(ImVec2(rMin.x + 2, rMin.y + 2), ImVec2(rMax.x + 2, rMax.y + 2), IM_COL32(0, 0, 0, 120), 6.0f);
        drawList->AddRectFilled(rMin, rMax, IM_COL32(20, 24, 32, 235), 6.0f);
        drawList->AddRect(rMin, rMax, (m_bevelType == BevelType::Solid) ? IM_COL32(255, 170, 50, 255) : IM_COL32(60, 150, 255, 255), 6.0f, 0, 1.8f);

        drawList->AddText(ImVec2(centerX - textSz1.x * 0.5f, topY + 7.0f), (m_bevelType == BevelType::Solid) ? IM_COL32(255, 200, 100, 255) : IM_COL32(100, 210, 255, 255), buf);
        drawList->AddText(ImVec2(centerX - textSz2.x * 0.5f, topY + 9.0f + textSz1.y), IM_COL32(220, 220, 220, 230), hint);
    }
}

void ComponentSelectTool::renderGui(Scene& scene) {
    Mesh* mesh = scene.getSelected();
    auto& selMgr = getSelectionManager();
    ComponentMode currentMode = selMgr.getMode();

    ImGui::TextDisabled("COMPONENT SELECTION");
    ImGui::Spacing();

    // Mode Buttons: [ Points ] [ Edges ] [ Faces ]
    float availW = ImGui::GetContentRegionAvail().x;
    float btnW = (availW - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;

    auto pushActiveStyle = [](bool active) {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.7f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
        }
    };

    pushActiveStyle(currentMode == ComponentMode::Vertex);
    if (ImGui::Button("Points", ImVec2(btnW, 28.0f))) {
        selMgr.setMode(ComponentMode::Vertex);
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    pushActiveStyle(currentMode == ComponentMode::Edge);
    if (ImGui::Button("Edges", ImVec2(btnW, 28.0f))) {
        selMgr.setMode(ComponentMode::Edge);
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    pushActiveStyle(currentMode == ComponentMode::Face);
    if (ImGui::Button("Faces", ImVec2(btnW, 28.0f))) {
        selMgr.setMode(ComponentMode::Face);
    }
    ImGui::PopStyleColor(3);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Loop selection
    if (currentMode == ComponentMode::Edge) {
        if (ImGui::Button("Select Edge Loop", ImVec2(-1, 26.0f))) {
            if (mesh) {
                selMgr.expandCurrentEdgeLoops(*mesh);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Expand current edge selection along quad edge loops (or double-click an edge)");
        }
        ImGui::Spacing();
    }

    // Helper Action Buttons: [ All ] [ Clear ] [ Invert ]
    if (ImGui::Button("Select All", ImVec2(btnW, 24.0f))) {
        if (mesh) selMgr.selectAll(*mesh);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(btnW, 24.0f))) {
        selMgr.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Invert", ImVec2(btnW, 24.0f))) {
        if (mesh) selMgr.invertSelection(*mesh);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Info Stats
    size_t nV = selMgr.getSelectedVertexCount();
    size_t nE = selMgr.getSelectedEdgeCount();
    size_t nF = selMgr.getSelectedFaceCount();
    ImGui::Text("Selected: %zu pts, %zu edges, %zu faces", nV, nE, nF);
}
