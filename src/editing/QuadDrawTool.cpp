#include "editing/QuadDrawTool.h"
#include "editing/SculptManager.h"
#include "editing/undo/UndoManager.h"
#include "common/Logger.h"
#include <SDL2/SDL.h>
#include <cmath>
#include <algorithm>

namespace retopo {

QuadDrawTool::QuadDrawTool(SculptManager& sm)
    : m_sculpt(sm) {
}

QuadDrawTool::~QuadDrawTool() = default;

Mesh* QuadDrawTool::getTargetMesh() {
    if (m_targetMesh && m_scene) {
        const auto& meshes = m_scene->getMeshes();
        if (std::find(meshes.begin(), meshes.end(), m_targetMesh) == meshes.end()) {
            m_targetMesh = nullptr;
            m_snapper.ClearCache();
        }
    }
    return m_targetMesh;
}

void QuadDrawTool::setTargetMesh(Mesh* target) {
    if (m_targetMesh != target) {
        m_targetMesh = target;
        sculpt_log("[QuadDrawTool] Set target mesh: %s\n", target ? target->outlinerName.c_str() : "None");
        if (m_targetMesh) {
            m_snapper.getBVH().Build(m_targetMesh);
        } else {
            m_snapper.ClearCache();
        }
    }
}

Mesh* QuadDrawTool::getRetopoMesh() {
    if (m_retopoMesh && m_scene) {
        const auto& meshes = m_scene->getMeshes();
        if (std::find(meshes.begin(), meshes.end(), m_retopoMesh) == meshes.end()) {
            m_retopoMesh = nullptr;
        }
    }
    return m_retopoMesh;
}

void QuadDrawTool::setRetopoMesh(Mesh* retopo) {
    if (m_retopoMesh != retopo) {
        m_retopoMesh = retopo;
        sculpt_log("[QuadDrawTool] Set retopo mesh: %s\n", retopo ? retopo->outlinerName.c_str() : "None");
    }
}

void QuadDrawTool::ensureTargetAndRetopo(Scene& scene, bool createIfMissing) {
    m_scene = &scene;
    const auto& meshes = scene.getMeshes();

    // 1. Verify or find target mesh
    bool targetValid = false;
    if (m_targetMesh) {
        for (Mesh* m : meshes) {
            if (m == m_targetMesh && m->nbFaces > 0) {
                targetValid = true;
                break;
            }
        }
    }

    if (!targetValid) {
        m_targetMesh = nullptr;
        m_snapper.ClearCache();
        Mesh* sel = scene.getSelected();
        if (sel && sel->outlinerName != "QuadDraw_Retopo" && sel->nbFaces > 0) {
            m_targetMesh = sel;
        } else {
            for (Mesh* m : meshes) {
                if (m && m->outlinerName != "QuadDraw_Retopo" && m->nbFaces > 0) {
                    m_targetMesh = m;
                    break;
                }
            }
        }
        if (m_targetMesh) {
            sculpt_log("[QuadDrawTool] Auto-detected target mesh '%s' (%d faces, %d verts)\n",
                m_targetMesh->outlinerName.c_str(), m_targetMesh->nbFaces, m_targetMesh->nbVerts);
        } else {
            sculpt_log("[QuadDrawTool] WARNING: No valid target mesh with faces in scene!\n");
        }
    }

    if (m_targetMesh && m_snapper.getBVH().NeedsRebuild(m_targetMesh)) {
        m_snapper.getBVH().Build(m_targetMesh);
    }

    // 2. Verify or find retopo mesh
    bool retopoValid = false;
    if (m_retopoMesh) {
        for (Mesh* m : meshes) {
            if (m == m_retopoMesh) {
                retopoValid = true;
                break;
            }
        }
    }

    if (!retopoValid) {
        m_retopoMesh = nullptr;
        for (Mesh* m : meshes) {
            if (m && m != m_targetMesh && m->outlinerName == "QuadDraw_Retopo") {
                m_retopoMesh = m;
                retopoValid = true;
                break;
            }
        }

        if (!retopoValid && createIfMissing) {
            m_retopoMesh = m_builder.GetOrCreateRetopoMesh(scene, m_targetMesh);
            sculpt_log("[QuadDrawTool] Auto-attached created retopo mesh '%s'\n",
                m_retopoMesh ? m_retopoMesh->outlinerName.c_str() : "None");
        } else if (retopoValid) {
            sculpt_log("[QuadDrawTool] Reconnected to existing retopo mesh '%s' (%d verts, %d faces)\n",
                m_retopoMesh->outlinerName.c_str(), m_retopoMesh->nbVerts, m_retopoMesh->nbFaces);
        }
    }
}

void QuadDrawTool::onSceneTopologyRestored(Scene& scene) {
    m_scene = &scene;
    m_dragMode = DragMode::None;
    m_hoverQuad.valid = false;
    m_hoverEdge.valid = false;
    m_hoverCut.valid = false;
    m_hoverEdgeLoop.edges.clear();
    m_hoverPolyLoop.clear();
    m_hoverVertex = -1;
    m_weldTargetVertex = -1;
    m_extrudeWeldTarget0 = -1;
    m_extrudeWeldTarget1 = -1;
    m_extrudeVertexWeldTarget = -1;
    m_multiCutPoints.clear();
    m_grabbedVerts.clear();
    m_extrudedVerts.clear();
    m_extrudedFrontEdges.clear();
    m_snapper.ClearCache();

    ensureTargetAndRetopo(scene, false);

    if (m_retopoMesh) {
        std::unordered_set<uint32_t> validPins;
        for (uint32_t v : m_pinnedVertices) {
            if (v < (uint32_t)m_retopoMesh->nbVerts) {
                validPins.insert(v);
            }
        }
        m_pinnedVertices = std::move(validPins);
        pruneSelectedEdges();

        bool loopValid = true;
        for (int32_t v : m_lastTubeSliceLoop) {
            if (v < 0 || v >= m_retopoMesh->nbVerts) {
                loopValid = false;
                break;
            }
        }
        if (!loopValid) m_lastTubeSliceLoop.clear();
    } else {
        m_pinnedVertices.clear();
        m_selectedEdges.clear();
        m_lastTubeSliceLoop.clear();
    }
}

void QuadDrawTool::notifyMeshDeleted(Mesh* mesh) {
    if (m_targetMesh == mesh) {
        m_targetMesh = nullptr;
        m_snapper.ClearCache();
    }
    if (m_retopoMesh == mesh) {
        m_retopoMesh = nullptr;
        m_pinnedVertices.clear();
        m_selectedEdges.clear();
        m_lastTubeSliceLoop.clear();
        resetTubeCurve();
    }
    m_dragMode = DragMode::None;
    m_hoverQuad.valid = false;
    m_hoverEdge.valid = false;
    m_hoverCut.valid = false;
    m_hoverEdgeLoop.edges.clear();
    m_hoverPolyLoop.clear();
    m_hoverVertex = -1;
    m_hoverPoly = -1;
    m_weldTargetVertex = -1;
    m_extrudeWeldTarget0 = -1;
    m_extrudeWeldTarget1 = -1;
    m_extrudeVertexWeldTarget = -1;
    m_multiCutPoints.clear();
    m_grabbedVerts.clear();
    m_extrudedVerts.clear();
    m_extrudedFrontEdges.clear();
    m_tweakVerts.clear();
    m_tweakSource = TweakSource::None;
    m_tweakPoly = -1;
    m_tweakEdgeV0 = -1;
    m_tweakEdgeV1 = -1;
}

void QuadDrawTool::onActivate(Scene& scene) {
    m_scene = &scene;
    sculpt_log("[QuadDrawTool] onActivate called\n");
    ensureTargetAndRetopo(scene, false);
}

void QuadDrawTool::onDeactivate() {
    m_dragMode = DragMode::None;
    m_hoverQuad.valid = false;
    m_hoverCut.valid = false;
    m_multiCutPoints.clear();
    m_extrudedVerts.clear();
    m_extrudedFrontEdges.clear();
    m_tweakVerts.clear();
    m_tweakSource = TweakSource::None;
    m_tweakPoly = -1;
    m_tweakEdgeV0 = -1;
    m_tweakEdgeV1 = -1;
    m_strokeScreenPoints.clear();
    m_strokeWorldPoints.clear();
    m_strokeQuadPreviews.clear();
    m_lastTubeSliceLoop.clear();
    m_tubeSlicePreviewResult.valid = false;
    resetTubeCurve();
}

void QuadDrawTool::snapAllPointsToTarget(Scene& scene) {
    if (!m_retopoMesh || !m_targetMesh || m_retopoMesh->nbVerts == 0) return;
    int32_t ptCount = (int32_t)(m_retopoMesh->verts.size() / 3);

    g_undoManager.pushTopologyChange(scene, "Snap All Points to Nearest Mesh", [&]() {
        for (int32_t i = 0; i < ptCount; ++i) {
            if (m_pinnedVertices.count((uint32_t)i)) continue;

            glm::vec3 localPos(
                m_retopoMesh->verts[i * 3 + 0],
                m_retopoMesh->verts[i * 3 + 1],
                m_retopoMesh->verts[i * 3 + 2]
            );
            glm::vec3 worldPos = (m_retopoMesh->matrix != glm::mat4(1.0f))
                ? glm::vec3(m_retopoMesh->matrix * glm::vec4(localPos, 1.0f))
                : localPos;

            SnapResult snap = m_snapper.SnapClosestPoint(m_targetMesh, worldPos, 500.0f);
            if (snap.valid) {
                m_builder.SetVertexPosition(m_retopoMesh, i, snap.worldPos);
            }
        }
        m_builder.NotifyMeshUpdated(m_retopoMesh);
    });
}

void QuadDrawTool::deleteAllUnconnectedPoints(Scene& scene) {
    if (!m_retopoMesh) return;
    g_undoManager.pushTopologyChange(scene, "Delete Unconnected Points", [&]() {
        m_builder.DeleteAllUnconnectedPoints(m_retopoMesh, &m_pinnedVertices);
    });
}

void QuadDrawTool::pinBorderVertices() {
    if (!m_retopoMesh) return;
    int32_t ptCount = (int32_t)(m_retopoMesh->verts.size() / 3);
    for (int32_t i = 0; i < ptCount; ++i) {
        if (m_builder.IsBoundaryOrIsolatedVertex(m_retopoMesh, i)) {
            m_pinnedVertices.insert((uint32_t)i);
        }
    }
}

void QuadDrawTool::pinAll() {
    if (!m_retopoMesh) return;
    int32_t ptCount = (int32_t)(m_retopoMesh->verts.size() / 3);
    for (int32_t i = 0; i < ptCount; ++i) {
        m_pinnedVertices.insert((uint32_t)i);
    }
}

void QuadDrawTool::unpinAll() {
    m_pinnedVertices.clear();
}

void QuadDrawTool::invertPins() {
    if (!m_retopoMesh) return;
    int32_t ptCount = (int32_t)(m_retopoMesh->verts.size() / 3);
    std::unordered_set<uint32_t> inverted;
    for (int32_t i = 0; i < ptCount; ++i) {
        if (!m_pinnedVertices.count((uint32_t)i)) {
            inverted.insert((uint32_t)i);
        }
    }
    m_pinnedVertices = std::move(inverted);
}

bool QuadDrawTool::isEdgeSelected(int32_t v0, int32_t v1) const {
    int32_t u = std::min(v0, v1);
    int32_t v = std::max(v0, v1);
    for (const auto& e : m_selectedEdges) {
        if (e.first == u && e.second == v) return true;
    }
    return false;
}

void QuadDrawTool::selectEdge(int32_t v0, int32_t v1) {
    int32_t u = std::min(v0, v1);
    int32_t v = std::max(v0, v1);
    if (!isEdgeSelected(u, v)) {
        m_selectedEdges.push_back({u, v});
    }
}

void QuadDrawTool::deselectEdge(int32_t v0, int32_t v1) {
    int32_t u = std::min(v0, v1);
    int32_t v = std::max(v0, v1);
    auto it = std::find_if(m_selectedEdges.begin(), m_selectedEdges.end(), [&](const std::pair<int32_t, int32_t>& e) {
        return e.first == u && e.second == v;
    });
    if (it != m_selectedEdges.end()) {
        m_selectedEdges.erase(it);
    }
}

void QuadDrawTool::toggleEdgeSelection(int32_t v0, int32_t v1) {
    if (isEdgeSelected(v0, v1)) deselectEdge(v0, v1);
    else selectEdge(v0, v1);
}

void QuadDrawTool::clearSelectedEdges() {
    m_selectedEdges.clear();
}

void QuadDrawTool::pruneSelectedEdges() {
    if (!m_retopoMesh || m_retopoMesh->nbVerts == 0) {
        m_selectedEdges.clear();
        return;
    }
    int32_t ptCount = m_retopoMesh->nbVerts;
    int32_t polyCount = m_retopoMesh->nbFaces;

    std::vector<std::pair<int32_t, int32_t>> valid;
    for (const auto& e : m_selectedEdges) {
        if (e.first >= 0 && e.first < ptCount && e.second >= 0 && e.second < ptCount && e.first != e.second) {
            bool exists = false;
            for (int32_t p = 0; p < polyCount; ++p) {
                if (PolygonHasEdge(m_retopoMesh->faces, p, e.first, e.second)) {
                    exists = true;
                    break;
                }
            }
            if (exists) valid.push_back(e);
        }
    }
    m_selectedEdges = std::move(valid);
}

void QuadDrawTool::deleteSelectedEdges(Scene& scene) {
    if (!m_retopoMesh || m_selectedEdges.empty()) return;
    g_undoManager.pushTopologyChange(scene, "Delete Selected Edges", [&]() {
        m_builder.DeleteEdges(m_retopoMesh, m_selectedEdges, &m_pinnedVertices);
    });
    m_selectedEdges.clear();
    m_hoverEdge.valid = false;
    m_hoverVertex = -1;
    m_hoverPoly = -1;
}

bool QuadDrawTool::isVertexInSelectedEdges(int32_t v) const {
    if (v == -1 || m_selectedEdges.empty()) return false;
    for (const auto& e : m_selectedEdges) {
        if (e.first == v || e.second == v) return true;
    }
    return false;
}

bool QuadDrawTool::isPolygonInSelectedEdges(int32_t polyIdx) const {
    if (polyIdx == -1 || !m_retopoMesh || polyIdx >= m_retopoMesh->nbFaces || m_selectedEdges.empty()) return false;
    for (const auto& e : m_selectedEdges) {
        if (PolygonHasEdge(m_retopoMesh->faces, polyIdx, e.first, e.second)) {
            return true;
        }
    }
    return false;
}

bool QuadDrawTool::isSelectionHovered() const {
    if (m_selectedEdges.empty()) return false;
    if (m_hoverEdge.valid && isEdgeSelected(m_hoverEdge.v0, m_hoverEdge.v1)) return true;
    if (m_hoverVertex != -1 && isVertexInSelectedEdges(m_hoverVertex)) return true;
    if (m_hoverPoly != -1 && isPolygonInSelectedEdges(m_hoverPoly)) return true;
    return false;
}

void QuadDrawTool::updateHoverState(
    const Camera& camera, float mouseX, float mouseY, bool ctrl, bool alt, bool shift
) {
    m_hoverSnap = m_snapper.RaycastSurface(camera, m_targetMesh, mouseX, mouseY);
    m_hoverVertex = -1;
    m_hoverPoly = -1;
    m_hoverEdge.valid = false;
    m_hoverQuad.valid = false;
    m_hoverCut.valid = false;
    m_hoverEdgeLoop.edges.clear();
    m_hoverPolyLoop.clear();
    m_weldTargetVertex = -1;
    m_extrudeWeldTarget0 = -1;
    m_extrudeWeldTarget1 = -1;
    m_extrudeVertexWeldTarget = -1;

    if (m_subTool == QuadDrawSubTool::TubeSlice) {
        return;
    }

    if (m_subTool == QuadDrawSubTool::TubeCurve) {
        m_tubeCurveHoveredPointIdx = -1;
        float bestDist = 18.0f;
        for (size_t i = 0; i < m_tubeCurvePoints.size(); ++i) {
            glm::vec3 sp = camera.project(m_tubeCurvePoints[i]);
            if (sp.z > 0.0f && sp.z < 1.0f) {
                float d = glm::distance(glm::vec2(mouseX, mouseY), glm::vec2(sp.x, sp.y));
                if (d < bestDist) {
                    bestDist = d;
                    m_tubeCurveHoveredPointIdx = (int32_t)i;
                }
            }
        }
        if (m_targetMesh) {
            SnapResult snap = m_snapper.RaycastSurface(camera, m_targetMesh, mouseX, mouseY);
            if (snap.valid) {
                m_tubeCurveCursorWorld = snap.worldPos;
            }
        }
        return;
    }

    if (!m_retopoMesh || m_retopoMesh->nbVerts == 0) return;

    // Check vertex hover
    m_hoverVertex = m_snapper.FindNearestRetopoVertex(
        camera, m_retopoMesh, mouseX, mouseY, m_weldRadiusPixels, -1, m_targetMesh, &m_hoverSnap
    );

    // Check edge hover
    bool borderOnly = (m_subTool == QuadDrawSubTool::Extrude && !ctrl) || (m_subTool == QuadDrawSubTool::Strip);
    float edgeRadius = (m_subTool == QuadDrawSubTool::Select || m_subTool == QuadDrawSubTool::Strip || m_subTool == QuadDrawSubTool::Tweak) ? 18.0f : 14.0f;
    m_hoverEdge = m_snapper.FindNearestRetopoEdge(
        camera, m_retopoMesh, mouseX, mouseY, edgeRadius, m_targetMesh, &m_hoverSnap, borderOnly
    );

    if (m_subTool != QuadDrawSubTool::Select && m_subTool != QuadDrawSubTool::Strip) {
        if (m_hoverVertex != -1 && m_hoverEdge.valid) {
            if (m_hoverVertex == m_hoverEdge.v0 || m_hoverVertex == m_hoverEdge.v1) {
                m_hoverEdge.valid = false;
            }
        }
    }

    // Check face hover (for Delete, Select, Tweak, and MultiCut tools)
    if ((m_subTool == QuadDrawSubTool::Delete || (ctrl && shift) || m_subTool == QuadDrawSubTool::Select || m_subTool == QuadDrawSubTool::Tweak || m_subTool == QuadDrawSubTool::MultiCut) && m_hoverVertex == -1 && !m_hoverEdge.valid) {
        m_hoverPoly = m_builder.FindPolygonUnderScreen(camera, m_retopoMesh, mouseX, mouseY, m_targetMesh, &m_snapper);
    }

    // Subtool specific hovers
    if (m_subTool == QuadDrawSubTool::CreatePolygon) {
        // Automatically find Quad Preview (No Shift key required!)
        glm::vec3 targetNorm = m_hoverSnap.valid ? m_hoverSnap.normal : glm::vec3(0, 1, 0);
        m_hoverQuad = m_builder.FindPotentialQuad(
            camera, m_retopoMesh, targetNorm, mouseX, mouseY, m_targetMesh, &m_snapper
        );
    } else if (m_subTool == QuadDrawSubTool::Knife) {
        if (m_hoverEdge.valid) {
            m_hoverCut = m_builder.FindEdgeLoopCut(
                m_retopoMesh, m_targetMesh, m_snapper, camera,
                m_hoverEdge.v0, m_hoverEdge.v1, m_hoverEdge.t, m_hoverEdge.polyIndex
            );
        }
    } else if (m_subTool == QuadDrawSubTool::MultiCut) {
        if (m_hoverEdge.valid && shift) {
            m_hoverEdge.t = std::clamp(std::round(m_hoverEdge.t * 4.0f) * 0.25f, 0.0f, 1.0f);
        }
    }

    if (ctrl && !shift) {
        if (m_hoverEdge.valid) {
            if (m_subTool == QuadDrawSubTool::Strip) {
                BorderChain bc = m_builder.FindBorderChain(m_retopoMesh, m_hoverEdge.v0, m_hoverEdge.v1);
                m_hoverEdgeLoop.edges.clear();
                auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
                    glm::vec3 p(m_retopoMesh->verts[idx * 3], m_retopoMesh->verts[idx * 3 + 1], m_retopoMesh->verts[idx * 3 + 2]);
                    return (m_retopoMesh->matrix != glm::mat4(1.0f)) ? glm::vec3(m_retopoMesh->matrix * glm::vec4(p, 1.0f)) : p;
                };
                for (const auto& e : bc.edges) {
                    LoopEdge le;
                    le.v0 = e.first; le.v1 = e.second;
                    le.worldPos0 = getVertWorld(e.first);
                    le.worldPos1 = getVertWorld(e.second);
                    m_hoverEdgeLoop.edges.push_back(le);
                }
            } else {
                m_hoverEdgeLoop = m_builder.FindEdgeLoop(m_retopoMesh, m_hoverEdge.v0, m_hoverEdge.v1);
            }
        }
    }
}

bool QuadDrawTool::handleMouseDown(
    Scene& scene, const Camera& camera, float mouseX, float mouseY, bool ctrl, bool alt, bool shift, int button
) {
    m_dragStartX = mouseX;
    m_dragStartY = mouseY;
    m_lastMouseX = mouseX;
    m_lastMouseY = mouseY;

    ensureTargetAndRetopo(scene, false);
    updateHoverState(camera, mouseX, mouseY, ctrl, alt, shift);

    sculpt_log("[QuadDrawTool] handleMouseDown at (%.1f, %.1f) btn=%d [ctrl=%d alt=%d shift=%d] | target=%s retopo=%s subTool=%d hoverSnap=%d (%.2f,%.2f,%.2f) hoverV=%d hoverE=%d hoverQuad=%d\n",
        mouseX, mouseY, button, ctrl ? 1 : 0, alt ? 1 : 0, shift ? 1 : 0,
        m_targetMesh ? m_targetMesh->outlinerName.c_str() : "NULL",
        m_retopoMesh ? m_retopoMesh->outlinerName.c_str() : "NULL",
        (int)m_subTool,
        m_hoverSnap.valid ? 1 : 0,
        m_hoverSnap.worldPos.x, m_hoverSnap.worldPos.y, m_hoverSnap.worldPos.z,
        m_hoverVertex,
        m_hoverEdge.valid ? 1 : 0,
        m_hoverQuad.valid ? 1 : 0);

    // MMB or Shift+MMB: interactive brush resize
    if (button == SDL_BUTTON_MIDDLE) {
        m_dragMode = DragMode::BrushResize;
        return true;
    }

    if (button == SDL_BUTTON_RIGHT) {
        if (m_subTool == QuadDrawSubTool::MultiCut) {
            if (m_multiCutPoints.size() >= 2) {
                commitMultiCut(scene);
            } else {
                m_multiCutPoints.clear();
            }
            return true;
        }
        return false;
    }

    if (button != SDL_BUTTON_LEFT) return false;

    // 0. Tube Slice Mode (LMB drag across mesh to slice and build quad tube)
    if (m_subTool == QuadDrawSubTool::TubeSlice) {
        m_dragMode = DragMode::TubeSliceDrag;
        m_tubeSliceStartScreen = glm::vec2(mouseX, mouseY);
        m_tubeSliceEndScreen = m_tubeSliceStartScreen;
        m_tubeSlicePreviewResult.valid = false;
        return true;
    }

    // 0. Tube Curve Mode (Click to add points, drag to move, Alt/Shift+drag end handle to extrude)
    if (m_subTool == QuadDrawSubTool::TubeCurve) {
        ensureTargetAndRetopo(scene, true);

        // Check if clicking on an existing handle
        m_tubeCurveWeldTargetPointIdx = -1;
        if (m_tubeCurveHoveredPointIdx >= 0 && m_tubeCurveHoveredPointIdx < (int32_t)m_tubeCurvePoints.size()) {
            if ((alt || shift) && (m_tubeCurveHoveredPointIdx == 0 || m_tubeCurveHoveredPointIdx == (int32_t)m_tubeCurvePoints.size() - 1)) {
                SnapResult snap = m_snapper.RaycastSurface(camera, m_targetMesh, mouseX, mouseY);
                glm::vec3 worldPos = snap.valid ? snap.worldPos : camera.unproject(mouseX, mouseY, 0.95f);

                if (m_tubeCurveHoveredPointIdx == 0) {
                    m_tubeCurvePoints.insert(m_tubeCurvePoints.begin(), worldPos);
                    m_tubeCurveDraggingPointIdx = 0;
                } else {
                    m_tubeCurvePoints.push_back(worldPos);
                    m_tubeCurveDraggingPointIdx = (int32_t)m_tubeCurvePoints.size() - 1;
                }
                m_tubeCurveExtrudeDrag = true;
                m_dragMode = DragMode::TubeCurveDrag;
                recalculateTubeCurvePreview();
                return true;
            }

            m_dragMode = DragMode::TubeCurveDrag;
            m_tubeCurveDraggingPointIdx = m_tubeCurveHoveredPointIdx;
            m_tubeCurveExtrudeDrag = false;
            return true;
        }

        // Clicking on mesh: append new point to tube
        SnapResult snap = m_snapper.RaycastSurface(camera, m_targetMesh, mouseX, mouseY);
        glm::vec3 worldPos = snap.valid ? snap.worldPos : camera.unproject(mouseX, mouseY, 0.95f);

        m_tubeCurvePoints.push_back(worldPos);
        m_tubeCurveCursorWorld = worldPos;
        m_tubeCurveDraggingPointIdx = (int32_t)m_tubeCurvePoints.size() - 1;
        m_dragMode = DragMode::TubeCurveDrag;
        m_tubeCurveExtrudeDrag = false;
        if (m_tubeCurvePoints.size() >= 2) {
            recalculateTubeCurvePreview();
        }
        return true;
    }

    // 0. Select Mode (Click to select edge/face, Alt+Click to deselect)
    if (m_subTool == QuadDrawSubTool::Select) {
        if (m_hoverEdge.valid) {
            if (ctrl) {
                EdgeLoopResult loop = m_builder.FindEdgeLoop(m_retopoMesh, m_hoverEdge.v0, m_hoverEdge.v1);
                if (!loop.edges.empty()) {
                    for (const auto& le : loop.edges) {
                        if (alt) deselectEdge(le.v0, le.v1);
                        else selectEdge(le.v0, le.v1);
                    }
                } else {
                    if (alt) deselectEdge(m_hoverEdge.v0, m_hoverEdge.v1);
                    else selectEdge(m_hoverEdge.v0, m_hoverEdge.v1);
                }
            } else {
                if (alt) {
                    deselectEdge(m_hoverEdge.v0, m_hoverEdge.v1);
                    sculpt_log("[QuadDrawTool] Deselected edge (%d, %d). Remaining: %zu\n",
                        m_hoverEdge.v0, m_hoverEdge.v1, m_selectedEdges.size());
                } else {
                    selectEdge(m_hoverEdge.v0, m_hoverEdge.v1);
                    sculpt_log("[QuadDrawTool] Selected edge (%d, %d). Total: %zu\n",
                        m_hoverEdge.v0, m_hoverEdge.v1, m_selectedEdges.size());
                }
            }
            updateHoverState(camera, mouseX, mouseY, ctrl, alt, shift);
            return true;
        } else if (m_hoverPoly != -1 && m_retopoMesh && m_hoverPoly < m_retopoMesh->nbFaces) {
            // Click inside a face: select/deselect all edges of this face
            uint32_t q[4] = {
                m_retopoMesh->faces[m_hoverPoly * 4 + 0],
                m_retopoMesh->faces[m_hoverPoly * 4 + 1],
                m_retopoMesh->faces[m_hoverPoly * 4 + 2],
                m_retopoMesh->faces[m_hoverPoly * 4 + 3]
            };
            int numE = (q[3] != TRI_INDEX) ? 4 : 3;
            for (int e = 0; e < numE; ++e) {
                int32_t u = (int32_t)q[e];
                int32_t v = (int32_t)q[(e + 1) % numE];
                if (alt) deselectEdge(u, v);
                else selectEdge(u, v);
            }
            updateHoverState(camera, mouseX, mouseY, ctrl, alt, shift);
            return true;
        }
        return false;
    }

    // 0.5. Strip Mode (Click to toggle/select edge, Ctrl+Click for chain, Drag to draw stroke)
    if (m_subTool == QuadDrawSubTool::Strip) {
        if (m_hoverEdge.valid) {
            if (ctrl) {
                BorderChain bc = m_builder.FindBorderChain(m_retopoMesh, m_hoverEdge.v0, m_hoverEdge.v1);
                if (!bc.edges.empty()) {
                    for (const auto& e : bc.edges) {
                        if (alt) deselectEdge(e.first, e.second);
                        else selectEdge(e.first, e.second);
                    }
                } else {
                    if (alt) deselectEdge(m_hoverEdge.v0, m_hoverEdge.v1);
                    else selectEdge(m_hoverEdge.v0, m_hoverEdge.v1);
                }
                updateHoverState(camera, mouseX, mouseY, ctrl, alt, shift);
                return true;
            } else if (alt) {
                deselectEdge(m_hoverEdge.v0, m_hoverEdge.v1);
                updateHoverState(camera, mouseX, mouseY, ctrl, alt, shift);
                return true;
            } else {
                m_edgeClickCandidate = true;
                m_candidateEdge = m_hoverEdge;
                m_strokeScreenPoints.clear();
                m_strokeWorldPoints.clear();
                m_strokeQuadPreviews.clear();
                m_strokeScreenPoints.push_back(glm::vec2(mouseX, mouseY));
                glm::vec3 worldPt = m_hoverSnap.valid ? m_hoverSnap.worldPos : camera.unproject(mouseX, mouseY, 0.95f);
                m_strokeWorldPoints.push_back(worldPt);
                return true;
            }
        } else {
            m_edgeClickCandidate = false;
            m_dragMode = DragMode::StrokeDrag;
            m_strokeScreenPoints.clear();
            m_strokeWorldPoints.clear();
            m_strokeQuadPreviews.clear();
            m_strokeScreenPoints.push_back(glm::vec2(mouseX, mouseY));
            glm::vec3 worldPt = m_hoverSnap.valid ? m_hoverSnap.worldPos : camera.unproject(mouseX, mouseY, 0.95f);
            m_strokeWorldPoints.push_back(worldPt);
            return true;
        }
    }

    // 1. Create Polygon Mode (Click to create quad)
    if (m_subTool == QuadDrawSubTool::CreatePolygon) {
        if (m_hoverQuad.valid) {
            ensureTargetAndRetopo(scene, true);
            if (!m_retopoMesh) return false;
            g_undoManager.pushTopologyChange(scene, "Add Quad", [&]() {
                m_builder.AddQuad(
                    m_retopoMesh,
                    m_hoverQuad.v[0], m_hoverQuad.v[1], m_hoverQuad.v[2], m_hoverQuad.v[3],
                    m_hoverQuad.normal
                );
                sculpt_log("[QuadDrawTool] Created Quad: (%d, %d, %d, %d)\n",
                    m_hoverQuad.v[0], m_hoverQuad.v[1], m_hoverQuad.v[2], m_hoverQuad.v[3]);
            });
            m_hoverQuad.valid = false;
            updateHoverState(camera, mouseX, mouseY, ctrl, alt, shift);
            return true;
        } else if (m_hoverSnap.valid) {
            // Also allow placing a point if clicking on surface where 4 points are not formed yet
            ensureTargetAndRetopo(scene, true);
            if (!m_retopoMesh) return false;
            g_undoManager.pushTopologyChange(scene, "Place Retopo Vertex", [&]() {
                int32_t newIdx = m_builder.AddVertex(m_retopoMesh, m_hoverSnap.worldPos);
                sculpt_log("[QuadDrawTool] Placed vertex #%d at world (%.2f, %.2f, %.2f) in retopo mesh '%s'\n",
                    newIdx, m_hoverSnap.worldPos.x, m_hoverSnap.worldPos.y, m_hoverSnap.worldPos.z,
                    m_retopoMesh ? m_retopoMesh->outlinerName.c_str() : "NULL");
            });
            updateHoverState(camera, mouseX, mouseY, ctrl, alt, shift);
            return true;
        }
        return false;
    }

    // 2. Delete Mode or Ctrl+Shift+LMB: deletion
    if (m_subTool == QuadDrawSubTool::Delete || (ctrl && shift)) {
        if (!m_selectedEdges.empty()) {
            bool hitSelection = false;
            if (m_hoverEdge.valid && isEdgeSelected(m_hoverEdge.v0, m_hoverEdge.v1)) {
                hitSelection = true;
            } else if (m_hoverVertex != -1) {
                for (const auto& e : m_selectedEdges) {
                    if (e.first == m_hoverVertex || e.second == m_hoverVertex) {
                        hitSelection = true;
                        break;
                    }
                }
            } else if (m_hoverPoly != -1 && m_hoverPoly < m_retopoMesh->nbFaces) {
                for (const auto& e : m_selectedEdges) {
                    if (PolygonHasEdge(m_retopoMesh->faces, m_hoverPoly, e.first, e.second)) {
                        hitSelection = true;
                        break;
                    }
                }
            }
            if (hitSelection) {
                deleteSelectedEdges(scene);
                updateHoverState(camera, mouseX, mouseY, ctrl, alt, shift);
                return true;
            }
        }

        if (m_hoverVertex != -1) {
            g_undoManager.pushTopologyChange(scene, "Delete Vertex", [&]() {
                m_builder.DeleteVertex(m_retopoMesh, m_hoverVertex, &m_pinnedVertices);
            });
            m_hoverVertex = -1;
            pruneSelectedEdges();
            updateHoverState(camera, mouseX, mouseY, ctrl, alt, shift);
            return true;
        } else if (m_hoverEdge.valid) {
            g_undoManager.pushTopologyChange(scene, "Delete Edge", [&]() {
                m_builder.DeleteEdge(m_retopoMesh, m_hoverEdge.v0, m_hoverEdge.v1, &m_pinnedVertices);
            });
            m_hoverEdge.valid = false;
            pruneSelectedEdges();
            updateHoverState(camera, mouseX, mouseY, ctrl, alt, shift);
            return true;
        } else if (m_hoverPoly != -1 && m_hoverPoly < m_retopoMesh->nbFaces) {
            g_undoManager.pushTopologyChange(scene, "Delete Polygon", [&]() {
                m_builder.DeletePolygon(m_retopoMesh, m_hoverPoly, &m_pinnedVertices);
            });
            m_hoverPoly = -1;
            pruneSelectedEdges();
            updateHoverState(camera, mouseX, mouseY, ctrl, alt, shift);
            return true;
        }
        return false;
    }

    // 3. Pin Mode: paint pins
    if (m_subTool == QuadDrawSubTool::Pin) {
        m_dragMode = DragMode::PinDrag;
        handleMouseMove(scene, camera, mouseX, mouseY, ctrl, alt, shift);
        return true;
    }

    // 4. Relax (Hold Shift in general or Relax brush)
    if (shift && m_subTool != QuadDrawSubTool::CreatePolygon) {
        m_dragMode = DragMode::RelaxDrag;
        m_builder.RelaxVertices(
            m_retopoMesh, m_targetMesh, m_snapper, camera,
            mouseX, mouseY, m_brushRadius, m_brushStrength,
            m_relaxLockBorder, m_relaxLockInterior, true, &m_pinnedVertices
        );
        return true;
    }

    // 5. Grab Mode
    if (m_subTool == QuadDrawSubTool::Grab) {
        m_dragMode = DragMode::GrabDrag;
        m_builder.CollectGrabVertices(
            m_retopoMesh, m_targetMesh, m_snapper, camera,
            mouseX, mouseY, m_brushRadius, m_brushStrength, m_brushFalloff,
            true, m_grabbedVerts, m_grabCenterWorld, &m_pinnedVertices
        );
        return true;
    }

    // 6. Knife Mode: insert edge loop
    if (m_subTool == QuadDrawSubTool::Knife) {
        if (m_hoverCut.valid) {
            g_undoManager.pushTopologyChange(scene, "Insert Edge Loop", [&]() {
                m_builder.ApplyEdgeLoopCut(m_retopoMesh, m_hoverCut);
            });
            m_hoverCut.valid = false;
            return true;
        }
        return false;
    }

    // 7. Multi-Cut Mode
    if (m_subTool == QuadDrawSubTool::MultiCut) {
        MultiCutPoint mcp;
        if (m_hoverVertex != -1) {
            mcp.type = MultiCutSnapType::Vertex;
            mcp.vertexIdx = m_hoverVertex;
            glm::vec3 p(
                m_retopoMesh->verts[m_hoverVertex * 3],
                m_retopoMesh->verts[m_hoverVertex * 3 + 1],
                m_retopoMesh->verts[m_hoverVertex * 3 + 2]
            );
            mcp.worldPos = (m_retopoMesh->matrix != glm::mat4(1.0f)) ? glm::vec3(m_retopoMesh->matrix * glm::vec4(p, 1.0f)) : p;
        } else if (m_hoverEdge.valid) {
            mcp.type = MultiCutSnapType::Edge;
            mcp.edgeV0 = m_hoverEdge.v0;
            mcp.edgeV1 = m_hoverEdge.v1;
            mcp.edgeT = m_hoverEdge.t;
            mcp.worldPos = glm::mix(m_hoverEdge.worldPos0, m_hoverEdge.worldPos1, m_hoverEdge.t);
        } else if (m_hoverPoly != -1) {
            mcp.type = MultiCutSnapType::Face;
            mcp.polyIndex = m_hoverPoly;
            mcp.worldPos = m_hoverSnap.valid ? m_hoverSnap.worldPos : camera.unproject(mouseX, mouseY, 0.5f);
        } else {
            m_dragMode = DragMode::SliceDrag;
            m_sliceStartScreen = glm::vec2(mouseX, mouseY);
            m_sliceEndScreen = m_sliceStartScreen;
            return true;
        }

        // Commit if user clicked the same vertex as the last point (double-click finish)
        if (!m_multiCutPoints.empty()) {
            const auto& last = m_multiCutPoints.back();
            if (last.type == MultiCutSnapType::Vertex && mcp.type == MultiCutSnapType::Vertex && last.vertexIdx == mcp.vertexIdx) {
                commitMultiCut(scene);
                return true;
            }
        }

        m_multiCutPoints.push_back(mcp);
        return true;
    }

    // 8. Tweak SubTool (Move components: Selection, Vertex, Edge, Polygon)
    if (m_subTool == QuadDrawSubTool::Tweak && m_retopoMesh) {
        auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
            glm::vec3 p(m_retopoMesh->verts[idx * 3], m_retopoMesh->verts[idx * 3 + 1], m_retopoMesh->verts[idx * 3 + 2]);
            return (m_retopoMesh->matrix != glm::mat4(1.0f)) ? glm::vec3(m_retopoMesh->matrix * glm::vec4(p, 1.0f)) : p;
        };

        // Priority 1: If selection exists and is hovered, tweak all selected edges!
        if (!m_selectedEdges.empty() && isSelectionHovered()) {
            std::vector<int32_t> vertIndices;
            std::unordered_set<int32_t> seen;
            for (const auto& e : m_selectedEdges) {
                if (e.first >= 0 && e.first < m_retopoMesh->nbVerts && !m_pinnedVertices.count(e.first)) {
                    if (seen.insert(e.first).second) vertIndices.push_back(e.first);
                }
                if (e.second >= 0 && e.second < m_retopoMesh->nbVerts && !m_pinnedVertices.count(e.second)) {
                    if (seen.insert(e.second).second) vertIndices.push_back(e.second);
                }
            }

            if (!vertIndices.empty()) {
                glm::vec3 center(0.0f);
                for (int32_t v : vertIndices) center += getVertWorld(v);
                center /= (float)vertIndices.size();

                m_dragStartWorldPos = m_hoverSnap.valid ? m_hoverSnap.worldPos : center;
                glm::vec3 sCenter = camera.project(m_dragStartWorldPos);
                m_dragEdgeScreenZ = (sCenter.z > 0.0f && sCenter.z < 1.0f) ? sCenter.z : 0.95f;

                m_tweakVerts.clear();
                for (int32_t v : vertIndices) {
                    TweakVertexInfo info;
                    info.vertexIdx = v;
                    info.initialWorldPos = getVertWorld(v);
                    info.weldTarget = -1;
                    m_tweakVerts.push_back(info);
                }

                m_tweakSource = TweakSource::Selection;
                m_dragMode = DragMode::TweakDrag;
                m_dragVertex = vertIndices[0];
                m_hoverEdge.valid = false;
                m_hoverVertex = -1;
                m_hoverPoly = -1;

                std::vector<uint32_t> affected(vertIndices.begin(), vertIndices.end());
                g_undoManager.beginSculptStroke(scene, m_retopoMesh->getID(), affected, false, false, "Tweak Move Selected Edges");
                return true;
            }
        }

        // Priority 2: Hovered Vertex
        if (m_hoverVertex != -1 && !m_pinnedVertices.count(m_hoverVertex)) {
            m_dragStartWorldPos = getVertWorld(m_hoverVertex);
            glm::vec3 sPos = camera.project(m_dragStartWorldPos);
            m_dragEdgeScreenZ = (sPos.z > 0.0f && sPos.z < 1.0f) ? sPos.z : 0.95f;

            m_tweakVerts.clear();
            m_tweakVerts.push_back({m_hoverVertex, m_dragStartWorldPos, -1});

            m_tweakSource = TweakSource::Vertex;
            m_dragMode = DragMode::TweakDrag;
            m_dragVertex = m_hoverVertex;
            m_hoverEdge.valid = false;
            m_hoverVertex = -1;
            m_hoverPoly = -1;

            std::vector<uint32_t> affected = { (uint32_t)m_hoverVertex };
            g_undoManager.beginSculptStroke(scene, m_retopoMesh->getID(), affected, false, false, "Tweak Move Vertex");
            return true;
        }

        // Priority 3: Hovered Edge
        if (m_hoverEdge.valid) {
            std::vector<int32_t> vertIndices;
            if (!m_pinnedVertices.count(m_hoverEdge.v0)) vertIndices.push_back(m_hoverEdge.v0);
            if (!m_pinnedVertices.count(m_hoverEdge.v1)) vertIndices.push_back(m_hoverEdge.v1);

            if (!vertIndices.empty()) {
                glm::vec3 edgeMid = (m_hoverEdge.worldPos0 + m_hoverEdge.worldPos1) * 0.5f;
                m_dragStartWorldPos = m_hoverSnap.valid ? m_hoverSnap.worldPos : edgeMid;
                glm::vec3 sMid = camera.project(m_dragStartWorldPos);
                m_dragEdgeScreenZ = (sMid.z > 0.0f && sMid.z < 1.0f) ? sMid.z : 0.95f;

                m_tweakVerts.clear();
                for (int32_t v : vertIndices) {
                    m_tweakVerts.push_back({v, getVertWorld(v), -1});
                }

                m_tweakSource = TweakSource::Edge;
                m_tweakEdgeV0 = m_hoverEdge.v0;
                m_tweakEdgeV1 = m_hoverEdge.v1;
                m_dragMode = DragMode::TweakDrag;
                m_dragVertex = vertIndices[0];
                m_dragEdgeV0 = m_hoverEdge.v0;
                m_dragEdgeV1 = m_hoverEdge.v1;
                m_hoverEdge.valid = false;
                m_hoverVertex = -1;
                m_hoverPoly = -1;

                std::vector<uint32_t> affected(vertIndices.begin(), vertIndices.end());
                g_undoManager.beginSculptStroke(scene, m_retopoMesh->getID(), affected, false, false, "Tweak Move Edge");
                return true;
            }
        }

        // Priority 4: Hovered Polygon
        if (m_hoverPoly != -1 && m_hoverPoly < m_retopoMesh->nbFaces) {
            uint32_t q[4] = {
                m_retopoMesh->faces[m_hoverPoly * 4 + 0],
                m_retopoMesh->faces[m_hoverPoly * 4 + 1],
                m_retopoMesh->faces[m_hoverPoly * 4 + 2],
                m_retopoMesh->faces[m_hoverPoly * 4 + 3]
            };
            int numV = (q[3] != TRI_INDEX) ? 4 : 3;

            std::vector<int32_t> vertIndices;
            glm::vec3 polyCenter(0.0f);
            for (int i = 0; i < numV; ++i) {
                int32_t v = (int32_t)q[i];
                polyCenter += getVertWorld(v);
                if (!m_pinnedVertices.count(v)) vertIndices.push_back(v);
            }
            if (numV > 0) polyCenter /= (float)numV;

            if (!vertIndices.empty()) {
                m_dragStartWorldPos = m_hoverSnap.valid ? m_hoverSnap.worldPos : polyCenter;
                glm::vec3 sCenter = camera.project(m_dragStartWorldPos);
                m_dragEdgeScreenZ = (sCenter.z > 0.0f && sCenter.z < 1.0f) ? sCenter.z : 0.95f;

                m_tweakVerts.clear();
                for (int32_t v : vertIndices) {
                    m_tweakVerts.push_back({v, getVertWorld(v), -1});
                }

                m_tweakSource = TweakSource::Polygon;
                m_tweakPoly = m_hoverPoly;
                m_dragMode = DragMode::TweakDrag;
                m_dragVertex = vertIndices[0];
                m_hoverEdge.valid = false;
                m_hoverVertex = -1;
                m_hoverPoly = -1;

                std::vector<uint32_t> affected(vertIndices.begin(), vertIndices.end());
                g_undoManager.beginSculptStroke(scene, m_retopoMesh->getID(), affected, false, false, "Tweak Move Polygon");
                return true;
            }
        }

        return false;
    }

    // 9. Extrude Mode
    if (m_hoverVertex != -1) {
        if (m_subTool == QuadDrawSubTool::Extrude && m_retopoMesh) {
            int32_t neighA = -1, neighB = -1;
            if (m_builder.CanExtrudeVertex(m_retopoMesh, m_hoverVertex, &neighA, &neighB)) {
                auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
                    glm::vec3 p(m_retopoMesh->verts[idx * 3], m_retopoMesh->verts[idx * 3 + 1], m_retopoMesh->verts[idx * 3 + 2]);
                    return (m_retopoMesh->matrix != glm::mat4(1.0f)) ? glm::vec3(m_retopoMesh->matrix * glm::vec4(p, 1.0f)) : p;
                };

                glm::vec3 pCorner = getVertWorld(m_hoverVertex);
                glm::vec3 pA = getVertWorld(neighA);
                glm::vec3 pB = getVertWorld(neighB);
                glm::vec3 initialPos = pA + pB - pCorner;

                if (m_targetMesh && m_targetMesh->nbFaces > 0) {
                    SnapResult sc = m_snapper.SnapClosestPoint(m_targetMesh, initialPos, 500.0f);
                    if (sc.valid) initialPos = sc.worldPos;
                }

                glm::vec3 targetNorm = m_hoverSnap.valid ? m_hoverSnap.normal : -camera.getRay(mouseX, mouseY).dir;
                if (glm::length(targetNorm) < 1e-4f) targetNorm = glm::vec3(0, 1, 0);

                m_extrudeCornerV = m_hoverVertex;
                m_extrudeCornerVA = neighA;
                m_extrudeCornerVB = neighB;
                m_extrudeVertexWeldTarget = -1;

                glm::vec3 sCorner = camera.project(initialPos);
                m_dragEdgeScreenZ = (sCorner.z > 0.0f && sCorner.z < 1.0f) ? sCorner.z : 0.95f;
                m_dragStartWorldPos = initialPos;

                g_undoManager.pushTopologyChange(scene, "Extrude Corner Vertex", [&]() {
                    ExtrudeVertexResult res = m_builder.ExtrudeVertex(
                        m_retopoMesh, m_hoverVertex, targetNorm, initialPos
                    );
                    if (res.valid) {
                        m_dragMode = DragMode::ExtrudeVertexDrag;
                        m_extrudedSingleV = res.newVertex;
                    } else {
                        m_dragMode = DragMode::None;
                    }
                });

                if (m_dragMode == DragMode::ExtrudeVertexDrag) {
                    m_hoverEdge.valid = false;
                    m_hoverVertex = -1;
                    m_hoverPoly = -1;
                    return true;
                }
            }
        }

        // Tweak Vertex Drag in Extrude mode
        auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
            glm::vec3 p(m_retopoMesh->verts[idx * 3], m_retopoMesh->verts[idx * 3 + 1], m_retopoMesh->verts[idx * 3 + 2]);
            return (m_retopoMesh->matrix != glm::mat4(1.0f)) ? glm::vec3(m_retopoMesh->matrix * glm::vec4(p, 1.0f)) : p;
        };
        m_dragStartWorldPos = getVertWorld(m_hoverVertex);
        glm::vec3 sPos = camera.project(m_dragStartWorldPos);
        m_dragEdgeScreenZ = (sPos.z > 0.0f && sPos.z < 1.0f) ? sPos.z : 0.95f;
        m_tweakVerts.clear();
        m_tweakVerts.push_back({m_hoverVertex, m_dragStartWorldPos, -1});
        m_tweakSource = TweakSource::Vertex;
        m_dragMode = DragMode::TweakDrag;
        m_dragVertex = m_hoverVertex;
        m_hoverEdge.valid = false;
        m_hoverVertex = -1;
        m_hoverPoly = -1;
        std::vector<uint32_t> affected = { (uint32_t)m_hoverVertex };
        g_undoManager.beginSculptStroke(scene, m_retopoMesh->getID(), affected, false, false, "Tweak Move Vertex");
        return true;
    }

    if (m_hoverEdge.valid && m_subTool == QuadDrawSubTool::Extrude) {
        // Border Edge Extrude Drag
        std::vector<std::pair<int32_t, int32_t>> edgesToExtrude;

        // If hovered edge is part of existing selection, extrude all selected edges!
        if (!m_selectedEdges.empty() && isEdgeSelected(m_hoverEdge.v0, m_hoverEdge.v1)) {
            edgesToExtrude = m_selectedEdges;
        } else {
            m_selectedEdges.clear();
            edgesToExtrude.push_back({std::min(m_hoverEdge.v0, m_hoverEdge.v1), std::max(m_hoverEdge.v0, m_hoverEdge.v1)});
        }

        m_dragEdgeV0 = m_hoverEdge.v0;
        m_dragEdgeV1 = m_hoverEdge.v1;
        m_dragEdgeInitialPos0 = m_hoverEdge.worldPos0;
        m_dragEdgeInitialPos1 = m_hoverEdge.worldPos1;

        glm::vec3 edgeMid = (m_dragEdgeInitialPos0 + m_dragEdgeInitialPos1) * 0.5f;
        glm::vec3 sMid = camera.project(edgeMid);
        m_dragEdgeScreenZ = (sMid.z > 0.0f && sMid.z < 1.0f) ? sMid.z : 0.95f;
        m_dragStartWorldPos = m_hoverSnap.valid ? m_hoverSnap.worldPos : edgeMid;

        glm::vec3 targetNorm = m_hoverSnap.valid ? m_hoverSnap.normal : glm::vec3(0, 1, 0);

        m_extrudedVerts.clear();
        m_extrudedFrontEdges.clear();

        auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
            glm::vec3 p(m_retopoMesh->verts[idx * 3], m_retopoMesh->verts[idx * 3 + 1], m_retopoMesh->verts[idx * 3 + 2]);
            return (m_retopoMesh->matrix != glm::mat4(1.0f)) ? glm::vec3(m_retopoMesh->matrix * glm::vec4(p, 1.0f)) : p;
        };

        const char* undoName = edgesToExtrude.size() > 1 ? "Extrude Selected Edges" : "Extrude Border Edge";
        g_undoManager.pushTopologyChange(scene, undoName, [&]() {
            ExtrudeEdgesResult res = m_builder.ExtrudeEdges(
                m_retopoMesh, edgesToExtrude, targetNorm
            );
            if (res.valid) {
                m_dragMode = DragMode::ExtrudeDrag;
                m_extrudedFrontEdges = res.newFrontEdges;
                for (const auto& kv : res.origToNew) {
                    ExtrudedVertexDragInfo info;
                    info.origV = kv.first;
                    info.newV = kv.second;
                    info.initialWorldPos = getVertWorld(kv.first);
                    info.weldTarget = -1;
                    m_extrudedVerts.push_back(info);
                }
                if (res.origToNew.count(m_dragEdgeV0)) m_extrudedV0 = res.origToNew[m_dragEdgeV0];
                if (res.origToNew.count(m_dragEdgeV1)) m_extrudedV1 = res.origToNew[m_dragEdgeV1];
            } else {
                m_dragMode = DragMode::None;
            }
        });
        if (m_dragMode == DragMode::ExtrudeDrag) {
            m_hoverEdge.valid = false;
            m_hoverVertex = -1;
            m_hoverPoly = -1;
            return true;
        } else {
            // Interior edge fallback: Tweak edge
            std::vector<int32_t> vertIndices;
            if (!m_pinnedVertices.count(m_hoverEdge.v0)) vertIndices.push_back(m_hoverEdge.v0);
            if (!m_pinnedVertices.count(m_hoverEdge.v1)) vertIndices.push_back(m_hoverEdge.v1);

            if (!vertIndices.empty()) {
                glm::vec3 edgeMid = (m_hoverEdge.worldPos0 + m_hoverEdge.worldPos1) * 0.5f;
                m_dragStartWorldPos = m_hoverSnap.valid ? m_hoverSnap.worldPos : edgeMid;
                glm::vec3 sMid = camera.project(m_dragStartWorldPos);
                m_dragEdgeScreenZ = (sMid.z > 0.0f && sMid.z < 1.0f) ? sMid.z : 0.95f;

                m_tweakVerts.clear();
                for (int32_t v : vertIndices) {
                    m_tweakVerts.push_back({v, getVertWorld(v), -1});
                }

                m_tweakSource = TweakSource::Edge;
                m_tweakEdgeV0 = m_hoverEdge.v0;
                m_tweakEdgeV1 = m_hoverEdge.v1;
                m_dragMode = DragMode::TweakDrag;
                m_dragVertex = vertIndices[0];
                m_dragEdgeV0 = m_hoverEdge.v0;
                m_dragEdgeV1 = m_hoverEdge.v1;
                m_hoverEdge.valid = false;
                m_hoverVertex = -1;
                m_hoverPoly = -1;

                std::vector<uint32_t> affected(vertIndices.begin(), vertIndices.end());
                g_undoManager.beginSculptStroke(scene, m_retopoMesh->getID(), affected, false, false, "Tweak Move Edge");
                return true;
            }
        }
    }

    // Click on empty target surface -> place vertex
    if (m_hoverSnap.valid && m_subTool == QuadDrawSubTool::Extrude) {
        ensureTargetAndRetopo(scene, true);
        if (!m_retopoMesh) return false;
        g_undoManager.pushTopologyChange(scene, "Place Retopo Vertex", [&]() {
            int32_t newIdx = m_builder.AddVertex(m_retopoMesh, m_hoverSnap.worldPos);
            sculpt_log("[QuadDrawTool] Placed vertex #%d at world (%.2f, %.2f, %.2f) in retopo mesh '%s' (total verts: %d)\n",
                newIdx, m_hoverSnap.worldPos.x, m_hoverSnap.worldPos.y, m_hoverSnap.worldPos.z,
                m_retopoMesh ? m_retopoMesh->outlinerName.c_str() : "NULL",
                m_retopoMesh ? m_retopoMesh->nbVerts : 0);
        });
        return true;
    }

    sculpt_log("[QuadDrawTool] Click did not trigger any action (hoverSnap.valid=%d, subTool=%d)\n",
        m_hoverSnap.valid ? 1 : 0, (int)m_subTool);
    return false;
}

bool QuadDrawTool::handleMouseMove(
    Scene& scene, const Camera& camera, float mouseX, float mouseY, bool ctrl, bool alt, bool shift
) {
    ensureTargetAndRetopo(scene, false);
    if (m_dragMode == DragMode::None) {
        updateHoverState(camera, mouseX, mouseY, ctrl, alt, shift);
        m_lastMouseX = mouseX;
        m_lastMouseY = mouseY;
        return false;
    }

    if (m_dragMode == DragMode::BrushResize) {
        m_brushRadius = std::clamp(m_brushRadius + (mouseX - m_lastMouseX) * 0.5f, 5.0f, 500.0f);
        m_lastMouseX = mouseX;
        m_lastMouseY = mouseY;
        return true;
    }

    if (m_dragMode == DragMode::PinDrag && m_retopoMesh) {
        int32_t ptCount = (int32_t)(m_retopoMesh->verts.size() / 3);
        float rSq = m_brushRadius * m_brushRadius;
        for (int32_t i = 0; i < ptCount; ++i) {
            glm::vec3 localPos(m_retopoMesh->verts[i * 3], m_retopoMesh->verts[i * 3 + 1], m_retopoMesh->verts[i * 3 + 2]);
            glm::vec3 wPos = (m_retopoMesh->matrix != glm::mat4(1.0f))
                ? glm::vec3(m_retopoMesh->matrix * glm::vec4(localPos, 1.0f))
                : localPos;
            glm::vec3 sPos = camera.project(wPos);
            if (sPos.z <= 0.0f) continue;
            float dSq = (sPos.x - mouseX) * (sPos.x - mouseX) + (sPos.y - mouseY) * (sPos.y - mouseY);
            if (dSq <= rSq) {
                if (ctrl) m_pinnedVertices.erase((uint32_t)i);
                else m_pinnedVertices.insert((uint32_t)i);
            }
        }
        m_lastMouseX = mouseX;
        m_lastMouseY = mouseY;
        return true;
    }

    if (m_dragMode == DragMode::RelaxDrag && m_retopoMesh) {
        m_builder.RelaxVertices(
            m_retopoMesh, m_targetMesh, m_snapper, camera,
            mouseX, mouseY, m_brushRadius, m_brushStrength * 0.5f,
            m_relaxLockBorder, m_relaxLockInterior, true, &m_pinnedVertices
        );
        m_lastMouseX = mouseX;
        m_lastMouseY = mouseY;
        return true;
    }

    if (m_dragMode == DragMode::GrabDrag && m_retopoMesh) {
        glm::vec3 newGrabCenter = camera.unproject(mouseX, mouseY, 0.5f);
        glm::vec3 delta = newGrabCenter - m_grabCenterWorld;

        for (const auto& gv : m_grabbedVerts) {
            glm::vec3 movedPos = gv.initWorldPos + delta * gv.weight;
            if (m_targetMesh && m_targetMesh->nbFaces > 0) {
                SnapResult snap = m_snapper.ProjectPointAlongNormal(m_targetMesh, movedPos, gv.initNormal, 100.0f);
                if (snap.valid) movedPos = snap.worldPos;
            }
            m_builder.SetVertexPosition(m_retopoMesh, gv.index, movedPos);
        }
        m_lastMouseX = mouseX;
        m_lastMouseY = mouseY;
        return true;
    }

    if (m_dragMode == DragMode::TweakDrag && m_retopoMesh && !m_tweakVerts.empty()) {
        SnapResult snap = m_snapper.RaycastSurface(camera, m_targetMesh, mouseX, mouseY);
        glm::vec3 targetPos = snap.valid ? snap.worldPos : camera.unproject(mouseX, mouseY, m_dragEdgeScreenZ);
        glm::vec3 offset = targetPos - m_dragStartWorldPos;

        std::unordered_set<int32_t> movingVerts;
        for (const auto& tv : m_tweakVerts) {
            movingVerts.insert(tv.vertexIdx);
        }

        // 1. Update vertex positions with target mesh snapping
        for (auto& tv : m_tweakVerts) {
            glm::vec3 newPos = tv.initialWorldPos + offset;

            if (m_targetMesh && m_targetMesh->nbFaces > 0) {
                if (m_tweakVerts.size() == 1 && snap.valid) {
                    newPos = snap.worldPos;
                } else {
                    glm::vec3 s = camera.project(newPos);
                    SnapResult snapV;
                    if (s.z > 0.0f && s.z < 1.0f) {
                        snapV = m_snapper.RaycastSurface(camera, m_targetMesh, s.x, s.y);
                    }
                    if (snapV.valid && glm::distance(snapV.worldPos, newPos) < 100.0f) {
                        newPos = snapV.worldPos;
                    } else {
                        SnapResult sc = m_snapper.SnapClosestPoint(m_targetMesh, newPos, 500.0f);
                        if (sc.valid) newPos = sc.worldPos;
                    }
                }
            }

            m_builder.SetVertexPosition(m_retopoMesh, tv.vertexIdx, newPos);
        }

        // 2. Check weld candidates
        if (m_tweakVerts.size() == 1) {
            m_weldTargetVertex = m_snapper.FindNearestRetopoVertex(
                camera, m_retopoMesh, mouseX, mouseY, m_weldRadiusPixels, m_tweakVerts[0].vertexIdx, m_targetMesh, snap.valid ? &snap : nullptr
            );
            m_tweakVerts[0].weldTarget = m_weldTargetVertex;
        } else {
            m_weldTargetVertex = -1;
            for (auto& tv : m_tweakVerts) {
                glm::vec3 curPos(
                    m_retopoMesh->verts[tv.vertexIdx * 3 + 0],
                    m_retopoMesh->verts[tv.vertexIdx * 3 + 1],
                    m_retopoMesh->verts[tv.vertexIdx * 3 + 2]
                );
                glm::vec3 sPos = camera.project(curPos);
                int32_t weld = m_snapper.FindNearestRetopoVertex(
                    camera, m_retopoMesh, sPos.x, sPos.y, m_weldRadiusPixels, tv.vertexIdx, m_targetMesh
                );
                if (weld != -1 && movingVerts.count(weld)) {
                    weld = -1;
                }
                tv.weldTarget = weld;
            }
        }

        m_lastMouseX = mouseX;
        m_lastMouseY = mouseY;
        return true;
    }

    if (m_dragMode == DragMode::ExtrudeVertexDrag && m_retopoMesh && m_extrudedSingleV != -1) {
        SnapResult snap = m_snapper.RaycastSurface(camera, m_targetMesh, mouseX, mouseY);
        glm::vec3 targetPos = snap.valid ? snap.worldPos : camera.unproject(mouseX, mouseY, m_dragEdgeScreenZ);

        if (!snap.valid && m_targetMesh && m_targetMesh->nbFaces > 0) {
            SnapResult sc = m_snapper.SnapClosestPoint(m_targetMesh, targetPos, 500.0f);
            if (sc.valid) targetPos = sc.worldPos;
        }

        m_builder.SetVertexPosition(m_retopoMesh, m_extrudedSingleV, targetPos);

        // Check weld candidate
        int32_t weld = m_snapper.FindNearestRetopoVertex(
            camera, m_retopoMesh, mouseX, mouseY, m_weldRadiusPixels, m_extrudedSingleV, m_targetMesh, snap.valid ? &snap : nullptr
        );

        if (weld == m_extrudeCornerV || weld == m_extrudeCornerVA || weld == m_extrudeCornerVB) {
            weld = -1;
        }

        m_extrudeVertexWeldTarget = weld;

        if (m_extrudeVertexWeldTarget != -1) {
            auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
                glm::vec3 p(m_retopoMesh->verts[idx * 3], m_retopoMesh->verts[idx * 3 + 1], m_retopoMesh->verts[idx * 3 + 2]);
                return (m_retopoMesh->matrix != glm::mat4(1.0f)) ? glm::vec3(m_retopoMesh->matrix * glm::vec4(p, 1.0f)) : p;
            };
            targetPos = getVertWorld(m_extrudeVertexWeldTarget);
            m_builder.SetVertexPosition(m_retopoMesh, m_extrudedSingleV, targetPos);
        }

        m_lastMouseX = mouseX;
        m_lastMouseY = mouseY;
        return true;
    }

    if (m_dragMode == DragMode::ExtrudeDrag && m_retopoMesh && !m_extrudedVerts.empty()) {
        SnapResult snap = m_snapper.RaycastSurface(camera, m_targetMesh, mouseX, mouseY);
        glm::vec3 targetPos = snap.valid ? snap.worldPos : camera.unproject(mouseX, mouseY, m_dragEdgeScreenZ);
        glm::vec3 offset = targetPos - m_dragStartWorldPos;

        std::unordered_set<int32_t> excludedWeldVerts;
        for (const auto& ev : m_extrudedVerts) {
            excludedWeldVerts.insert(ev.origV);
            excludedWeldVerts.insert(ev.newV);
        }

        // 1. Move vertices and snap to surface
        for (auto& ev : m_extrudedVerts) {
            glm::vec3 newPos = ev.initialWorldPos + offset;

            if (m_targetMesh && m_targetMesh->nbFaces > 0) {
                glm::vec3 s = camera.project(newPos);
                SnapResult snapV;
                if (s.z > 0.0f && s.z < 1.0f) {
                    snapV = m_snapper.RaycastSurface(camera, m_targetMesh, s.x, s.y);
                }
                if (snapV.valid && glm::distance(snapV.worldPos, newPos) < 100.0f) {
                    newPos = snapV.worldPos;
                } else {
                    SnapResult sc = m_snapper.SnapClosestPoint(m_targetMesh, newPos, 500.0f);
                    if (sc.valid) newPos = sc.worldPos;
                }
            }

            m_builder.SetVertexPosition(m_retopoMesh, ev.newV, newPos);
        }

        auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
            glm::vec3 p(m_retopoMesh->verts[idx * 3], m_retopoMesh->verts[idx * 3 + 1], m_retopoMesh->verts[idx * 3 + 2]);
            return (m_retopoMesh->matrix != glm::mat4(1.0f)) ? glm::vec3(m_retopoMesh->matrix * glm::vec4(p, 1.0f)) : p;
        };

        // 2. Check weld candidates
        std::unordered_map<int32_t, int> weldTargetToEvIdx;

        for (size_t i = 0; i < m_extrudedVerts.size(); ++i) {
            auto& ev = m_extrudedVerts[i];
            glm::vec3 curPos(
                m_retopoMesh->verts[ev.newV * 3 + 0],
                m_retopoMesh->verts[ev.newV * 3 + 1],
                m_retopoMesh->verts[ev.newV * 3 + 2]
            );
            glm::vec3 sPos = camera.project(curPos);

            int32_t weld = m_snapper.FindNearestRetopoVertex(
                camera, m_retopoMesh, sPos.x, sPos.y, m_weldRadiusPixels, ev.newV, m_targetMesh
            );

            if (weld != -1 && excludedWeldVerts.count(weld)) {
                weld = -1;
            }

            ev.weldTarget = weld;

            if (weld != -1) {
                auto it = weldTargetToEvIdx.find(weld);
                if (it != weldTargetToEvIdx.end()) {
                    int otherIdx = it->second;
                    glm::vec3 otherPos(
                        m_retopoMesh->verts[m_extrudedVerts[otherIdx].newV * 3 + 0],
                        m_retopoMesh->verts[m_extrudedVerts[otherIdx].newV * 3 + 1],
                        m_retopoMesh->verts[m_extrudedVerts[otherIdx].newV * 3 + 2]
                    );
                    glm::vec3 targetWorld = getVertWorld(weld);
                    if (glm::distance(curPos, targetWorld) < glm::distance(otherPos, targetWorld)) {
                        m_extrudedVerts[otherIdx].weldTarget = -1;
                        it->second = (int)i;
                    } else {
                        ev.weldTarget = -1;
                    }
                } else {
                    weldTargetToEvIdx[weld] = (int)i;
                }
            }
        }

        // 3. Magnetic visual snap to weld targets
        for (auto& ev : m_extrudedVerts) {
            if (ev.weldTarget != -1) {
                glm::vec3 targetWorld = getVertWorld(ev.weldTarget);
                m_builder.SetVertexPosition(m_retopoMesh, ev.newV, targetWorld);
            }
        }

        m_extrudeWeldTarget0 = -1;
        m_extrudeWeldTarget1 = -1;
        for (const auto& ev : m_extrudedVerts) {
            if (ev.newV == m_extrudedV0) m_extrudeWeldTarget0 = ev.weldTarget;
            if (ev.newV == m_extrudedV1) m_extrudeWeldTarget1 = ev.weldTarget;
        }

        m_lastMouseX = mouseX;
        m_lastMouseY = mouseY;
        return true;
    }

    if (m_dragMode == DragMode::SliceDrag) {
        m_sliceEndScreen = glm::vec2(mouseX, mouseY);
        m_lastMouseX = mouseX;
        m_lastMouseY = mouseY;
        return true;
    }

    if (m_dragMode == DragMode::TubeSliceDrag) {
        m_tubeSliceEndScreen = glm::vec2(mouseX, mouseY);
        if (glm::distance(m_tubeSliceStartScreen, m_tubeSliceEndScreen) >= 6.0f) {
            m_tubeSlicePreviewResult = m_builder.SliceTargetMeshForTube(
                camera, m_targetMesh, m_snapper,
                m_tubeSliceStartScreen, m_tubeSliceEndScreen, m_tubeSliceRadialSegments
            );
        } else {
            m_tubeSlicePreviewResult.valid = false;
        }
        m_lastMouseX = mouseX;
        m_lastMouseY = mouseY;
        return true;
    }

    if (m_subTool == QuadDrawSubTool::TubeCurve) {
        SnapResult snap = m_snapper.RaycastSurface(camera, m_targetMesh, mouseX, mouseY);
        glm::vec3 worldPt = snap.valid ? snap.worldPos : camera.unproject(mouseX, mouseY, 0.95f);
        m_tubeCurveCursorWorld = worldPt;

        if (m_dragMode == DragMode::TubeCurveDrag) {
            if (m_tubeCurveDraggingPointIdx >= 0 && m_tubeCurveDraggingPointIdx < (int32_t)m_tubeCurvePoints.size()) {
                // Check if dragging near another point for snapping / collapsing
                m_tubeCurveWeldTargetPointIdx = -1;
                float bestWeldDist = m_weldRadiusPixels * 1.5f;
                int32_t bestTargetIdx = -1;
                for (size_t i = 0; i < m_tubeCurvePoints.size(); ++i) {
                    if ((int32_t)i == m_tubeCurveDraggingPointIdx) continue;
                    glm::vec3 sp = camera.project(m_tubeCurvePoints[i]);
                    if (sp.z > 0.0f && sp.z < 1.0f) {
                        float d = glm::distance(glm::vec2(mouseX, mouseY), glm::vec2(sp.x, sp.y));
                        if (d < bestWeldDist) {
                            bestWeldDist = d;
                            bestTargetIdx = (int32_t)i;
                        }
                    }
                }

                if (bestTargetIdx >= 0) {
                    m_tubeCurveWeldTargetPointIdx = bestTargetIdx;
                    m_tubeCurvePoints[m_tubeCurveDraggingPointIdx] = m_tubeCurvePoints[bestTargetIdx];
                } else {
                    m_tubeCurvePoints[m_tubeCurveDraggingPointIdx] = worldPt;
                }

                if (m_tubeCurvePoints.size() >= 2) {
                    recalculateTubeCurvePreview();
                }
            }
            m_lastMouseX = mouseX;
            m_lastMouseY = mouseY;
            return true;
        }

        m_lastMouseX = mouseX;
        m_lastMouseY = mouseY;
        return true;
    }

    if (m_subTool == QuadDrawSubTool::Strip) {
        if (m_edgeClickCandidate) {
            float dist = glm::distance(glm::vec2(mouseX, mouseY), glm::vec2(m_dragStartX, m_dragStartY));
            if (dist > 8.0f) {
                m_edgeClickCandidate = false;
                m_dragMode = DragMode::StrokeDrag;
            }
        }

        if (m_dragMode == DragMode::StrokeDrag) {
            glm::vec2 curScreen(mouseX, mouseY);
            if (m_strokeScreenPoints.empty() || glm::distance(curScreen, m_strokeScreenPoints.back()) > 4.0f) {
                m_strokeScreenPoints.push_back(curScreen);
                SnapResult snap = m_snapper.RaycastSurface(camera, m_targetMesh, mouseX, mouseY);
                glm::vec3 worldPt = snap.valid ? snap.worldPos : camera.unproject(mouseX, mouseY, 0.95f);
                m_strokeWorldPoints.push_back(worldPt);
                updateStrokePreview(camera);
            }
            m_lastMouseX = mouseX;
            m_lastMouseY = mouseY;
            return true;
        }
    }

    m_lastMouseX = mouseX;
    m_lastMouseY = mouseY;
    return false;
}

bool QuadDrawTool::handleMouseUp(Scene& scene, const Camera& camera, int button) {
    if (m_subTool == QuadDrawSubTool::TubeSlice) {
        if (m_dragMode == DragMode::TubeSliceDrag) {
            TubeSliceResult slice;
            if (m_tubeSlicePreviewResult.valid && !m_tubeSlicePreviewResult.worldPoints.empty()) {
                slice = std::move(m_tubeSlicePreviewResult);
            } else if (glm::distance(m_tubeSliceStartScreen, m_tubeSliceEndScreen) >= 6.0f) {
                slice = m_builder.SliceTargetMeshForTube(
                    camera, m_targetMesh, m_snapper,
                    m_tubeSliceStartScreen, m_tubeSliceEndScreen, m_tubeSliceRadialSegments
                );
            }

            if (slice.valid && !slice.worldPoints.empty()) {
                ensureTargetAndRetopo(scene, true);
                if (m_retopoMesh) {
                    g_undoManager.pushTopologyChange(scene, "Tube Retopo Ring", [&]() {
                        TubeBuildResult bRes = m_builder.BuildTubeRing(
                            m_retopoMesh, m_targetMesh, m_snapper,
                            slice.worldPoints, m_lastTubeSliceLoop, m_tubeSliceSpans
                        );
                        if (bRes.valid) {
                            m_lastTubeSliceLoop = bRes.newLoopVertices;
                        }
                    });
                }
            }

            m_dragMode = DragMode::None;
            m_tubeSlicePreviewResult.valid = false;
            updateHoverState(camera, m_lastMouseX, m_lastMouseY, false, false, false);
            return true;
        }
    }

    if (m_subTool == QuadDrawSubTool::TubeCurve) {
        if (m_dragMode == DragMode::TubeCurveDrag) {
            // Check if points should collapse into each other
            if (m_tubeCurveWeldTargetPointIdx >= 0 && m_tubeCurveWeldTargetPointIdx < (int32_t)m_tubeCurvePoints.size() &&
                m_tubeCurveDraggingPointIdx >= 0 && m_tubeCurveDraggingPointIdx < (int32_t)m_tubeCurvePoints.size() &&
                m_tubeCurveDraggingPointIdx != m_tubeCurveWeldTargetPointIdx) {
                m_tubeCurvePoints.erase(m_tubeCurvePoints.begin() + m_tubeCurveDraggingPointIdx);
                m_tubeCurveWeldTargetPointIdx = -1;
                if (m_tubeCurvePoints.size() >= 2) {
                    recalculateTubeCurvePreview();
                } else {
                    m_tubeCurveGeomPreview.valid = false;
                }
            }
            m_dragMode = DragMode::None;
            m_tubeCurveDraggingPointIdx = -1;
            m_tubeCurveExtrudeDrag = false;
            m_tubeCurveWeldTargetPointIdx = -1;
            updateHoverState(camera, m_lastMouseX, m_lastMouseY, false, false, false);
            return true;
        }
    }

    if (m_subTool == QuadDrawSubTool::Strip) {
        if (m_edgeClickCandidate) {
            if (m_candidateEdge.valid) {
                toggleEdgeSelection(m_candidateEdge.v0, m_candidateEdge.v1);
            }
            cancelStroke();
            updateHoverState(camera, m_lastMouseX, m_lastMouseY, false, false, false);
            return true;
        }

        if (m_dragMode == DragMode::StrokeDrag) {
            if (m_strokeWorldPoints.size() >= 2) {
                commitStrokeExtrude(scene, camera);
            }
            cancelStroke();
            updateHoverState(camera, m_lastMouseX, m_lastMouseY, false, false, false);
            return true;
        }
    }

    if (m_dragMode == DragMode::None) return false;

    if (m_dragMode == DragMode::TweakDrag && m_retopoMesh && !m_tweakVerts.empty()) {
        std::vector<std::pair<int32_t, int32_t>> welds;
        for (const auto& tv : m_tweakVerts) {
            if (tv.weldTarget != -1 && tv.weldTarget != tv.vertexIdx) {
                welds.push_back({tv.vertexIdx, tv.weldTarget});
            }
        }

        if (!welds.empty()) {
            g_undoManager.cancelSculptStroke();
            g_undoManager.pushTopologyChange(scene, "Weld Vertices", [&]() {
                std::vector<int32_t> outRemap;
                m_builder.WeldVerticesMultiple(m_retopoMesh, welds, &m_pinnedVertices, &outRemap);
                if (!outRemap.empty() && !m_selectedEdges.empty()) {
                    std::vector<std::pair<int32_t, int32_t>> newSel;
                    for (const auto& e : m_selectedEdges) {
                        int32_t u = (e.first >= 0 && e.first < (int32_t)outRemap.size()) ? outRemap[e.first] : e.first;
                        int32_t v = (e.second >= 0 && e.second < (int32_t)outRemap.size()) ? outRemap[e.second] : e.second;
                        if (u != -1 && v != -1 && u != v) {
                            newSel.push_back({std::min(u, v), std::max(u, v)});
                        }
                    }
                    std::sort(newSel.begin(), newSel.end());
                    newSel.erase(std::unique(newSel.begin(), newSel.end()), newSel.end());
                    m_selectedEdges = std::move(newSel);
                }
            });
        } else {
            g_undoManager.endSculptStroke(scene);
        }
        m_tweakVerts.clear();
    } else if (m_dragMode == DragMode::ExtrudeDrag && m_retopoMesh && !m_extrudedVerts.empty()) {
        std::vector<std::pair<int32_t, int32_t>> welds;
        for (const auto& ev : m_extrudedVerts) {
            if (ev.weldTarget != -1 && ev.weldTarget != ev.newV) {
                welds.push_back({ev.newV, ev.weldTarget});
            }
        }

        std::vector<int32_t> outRemap;
        if (!welds.empty()) {
            g_undoManager.pushTopologyChange(scene, "Weld Extruded Vertices", [&]() {
                m_builder.WeldVerticesMultiple(m_retopoMesh, welds, &m_pinnedVertices, &outRemap);
            });
        }

        // Update m_selectedEdges to the new front edges
        m_selectedEdges.clear();
        for (const auto& fe : m_extrudedFrontEdges) {
            int32_t u = fe.first;
            int32_t v = fe.second;
            if (!outRemap.empty()) {
                if (u >= 0 && u < (int32_t)outRemap.size()) u = outRemap[u];
                if (v >= 0 && v < (int32_t)outRemap.size()) v = outRemap[v];
            }
            if (u != -1 && v != -1 && u != v) {
                selectEdge(u, v);
            }
        }
    } else if (m_dragMode == DragMode::ExtrudeVertexDrag && m_retopoMesh && m_extrudedSingleV != -1) {
        if (m_extrudeVertexWeldTarget != -1 && m_extrudeVertexWeldTarget != m_extrudedSingleV) {
            int32_t src = m_extrudedSingleV;
            int32_t tgt = m_extrudeVertexWeldTarget;
            g_undoManager.pushTopologyChange(scene, "Weld Extruded Vertex", [&]() {
                m_builder.WeldVertices(m_retopoMesh, src, tgt, &m_pinnedVertices);
            });
        }
    } else if (m_dragMode == DragMode::SliceDrag) {
        MultiCutResult slice = m_builder.BuildSliceCut(
            camera, m_retopoMesh, m_targetMesh, m_snapper, m_sliceStartScreen, m_sliceEndScreen
        );
        if (slice.valid && !slice.cuts.empty()) {
            g_undoManager.pushTopologyChange(scene, "Slice Cut", [&]() {
                m_builder.ApplyPolygonCuts(m_retopoMesh, m_targetMesh, m_snapper, slice.cuts);
            });
        }
    }

    m_dragMode = DragMode::None;
    m_dragVertex = -1;
    m_dragEdgeV0 = -1;
    m_dragEdgeV1 = -1;
    m_tweakSource = TweakSource::None;
    m_tweakPoly = -1;
    m_tweakEdgeV0 = -1;
    m_tweakEdgeV1 = -1;
    m_extrudedV0 = -1;
    m_extrudedV1 = -1;
    m_extrudedSingleV = -1;
    m_extrudeCornerV = -1;
    m_extrudeCornerVA = -1;
    m_extrudeCornerVB = -1;
    m_weldTargetVertex = -1;
    m_extrudeWeldTarget0 = -1;
    m_extrudeWeldTarget1 = -1;
    m_extrudeVertexWeldTarget = -1;
    m_tweakVerts.clear();
    m_grabbedVerts.clear();
    m_extrudedVerts.clear();
    m_extrudedFrontEdges.clear();
    updateHoverState(camera, m_lastMouseX, m_lastMouseY, false, false, false);
    return true;
}

void QuadDrawTool::commitMultiCut(Scene& scene) {
    if (!m_retopoMesh || m_multiCutPoints.size() < 2) return;
    MultiCutResult res = m_builder.BuildMultiCutFromPoints(
        scene.getCamera(), m_retopoMesh, m_targetMesh, m_snapper, m_multiCutPoints
    );
    if (res.valid && !res.cuts.empty()) {
        g_undoManager.pushTopologyChange(scene, "Apply Multi-Cut", [&]() {
            m_builder.ApplyPolygonCuts(m_retopoMesh, m_targetMesh, m_snapper, res.cuts);
        });
    }
    m_multiCutPoints.clear();
}

bool QuadDrawTool::handleKeyDown(Scene& scene, int keycode) {
    if (m_subTool == QuadDrawSubTool::TubeSlice) {
        if (keycode == SDLK_ESCAPE) {
            m_lastTubeSliceLoop.clear();
            m_dragMode = DragMode::None;
            m_tubeSlicePreviewResult.valid = false;
            return true;
        }
    }

    if (m_subTool == QuadDrawSubTool::TubeCurve) {
        if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER || keycode == SDLK_SPACE) {
            commitCurrentTubeCurve(scene);
            return true;
        }
        if (keycode == SDLK_ESCAPE) {
            resetTubeCurve();
            return true;
        }
        if (keycode == SDLK_BACKSPACE || keycode == SDLK_DELETE) {
            if (!m_tubeCurvePoints.empty()) {
                removeLastTubeCurvePoint();
                return true;
            }
        }
    }

    if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER || keycode == SDLK_SPACE) {
        commitMultiCut(scene);
        return true;
    }
    if (keycode == SDLK_DELETE || keycode == SDLK_BACKSPACE) {
        if (!m_multiCutPoints.empty()) {
            m_multiCutPoints.pop_back();
            return true;
        }
        if (!m_selectedEdges.empty()) {
            deleteSelectedEdges(scene);
            return true;
        }
    }
    if (keycode == SDLK_ESCAPE) {
        if (m_subTool == QuadDrawSubTool::TubeSlice) {
            m_lastTubeSliceLoop.clear();
            m_dragMode = DragMode::None;
            m_tubeSlicePreviewResult.valid = false;
            return true;
        }
        if (m_subTool == QuadDrawSubTool::TubeCurve) {
            resetTubeCurve();
            return true;
        }
        if (m_dragMode == DragMode::StrokeDrag || m_edgeClickCandidate) {
            cancelStroke();
            return true;
        }
        if (!m_selectedEdges.empty()) {
            m_selectedEdges.clear();
            return true;
        }
        m_multiCutPoints.clear();
        m_hoverQuad.valid = false;
        m_hoverCut.valid = false;
        m_dragMode = DragMode::None;
        return true;
    }
    return false;
}

void QuadDrawTool::renderOverlay(Scene& scene, const Camera& camera, float vW, float vH) {
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList) return;

    ensureTargetAndRetopo(scene, false);

    // 0. Draw Cursor Snap Indicator on target surface
    if (m_dragMode == DragMode::None && m_hoverSnap.valid && (m_subTool == QuadDrawSubTool::Extrude || m_subTool == QuadDrawSubTool::CreatePolygon)) {
        glm::vec3 sPos = camera.project(m_hoverSnap.worldPos);
        if (sPos.z >= 0.0f && sPos.z <= 1.0f) {
            ImVec2 c(sPos.x, sPos.y);
            drawList->AddCircleFilled(c, 4.0f, IM_COL32(30, 255, 120, 220));
            drawList->AddCircle(c, 9.0f, IM_COL32(30, 255, 120, 200), 16, 1.5f);
            drawList->AddLine(ImVec2(c.x - 14.0f, c.y), ImVec2(c.x + 14.0f, c.y), IM_COL32(255, 255, 255, 180), 1.0f);
            drawList->AddLine(ImVec2(c.x, c.y - 14.0f), ImVec2(c.x, c.y + 14.0f), IM_COL32(255, 255, 255, 180), 1.0f);
        }
    }

    // 1. Draw Quad Preview (Green polygon)
    if (m_dragMode == DragMode::None && m_hoverQuad.valid) {
        ImVec2 pts[4];
        for (int i = 0; i < 4; ++i) {
            pts[i] = ImVec2(m_hoverQuad.screenPositions[i].x, m_hoverQuad.screenPositions[i].y);
        }
        drawList->AddConvexPolyFilled(pts, 4, IM_COL32(40, 230, 90, 110));
        drawList->AddPolyline(pts, 4, IM_COL32(80, 255, 120, 240), ImDrawFlags_Closed, 2.5f);
    }

    // 2. Draw Retopo Mesh (Wireframe & Points)
    if (m_retopoMesh && m_retopoMesh->nbVerts > 0) {
        const int ptCount = m_retopoMesh->nbVerts;
        const int polyCount = m_retopoMesh->nbFaces;
        const auto& faces = m_retopoMesh->faces;
        const auto& verts = m_retopoMesh->verts;
        const glm::mat4& mat = m_retopoMesh->matrix;
        const bool hasTransform = (mat != glm::mat4(1.0f));

        auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
            glm::vec3 p(verts[idx * 3], verts[idx * 3 + 1], verts[idx * 3 + 2]);
            return hasTransform ? glm::vec3(mat * glm::vec4(p, 1.0f)) : p;
        };

        std::vector<glm::vec3> screenPts(ptCount);
        for (int i = 0; i < ptCount; ++i) {
            screenPts[i] = camera.project(getVertWorld(i));
        }

        // Draw Wireframe
        if (m_showWireframe) {
            ImU32 wireCol = ImGui::ColorConvertFloat4ToU32(m_wireColor);
            for (int f = 0; f < polyCount; ++f) {
                uint32_t q[4] = { faces[f * 4 + 0], faces[f * 4 + 1], faces[f * 4 + 2], faces[f * 4 + 3] };
                int numE = (q[3] != TRI_INDEX) ? 4 : 3;

                ImVec2 polyScreen[4];
                bool allFront = true;
                for (int e = 0; e < numE; ++e) {
                    if (screenPts[q[e]].z < 0.0f || screenPts[q[e]].z > 1.0f) { allFront = false; break; }
                    polyScreen[e] = ImVec2(screenPts[q[e]].x, screenPts[q[e]].y);
                }
                if (allFront) {
                    drawList->AddPolyline(polyScreen, numE, wireCol, ImDrawFlags_Closed, m_lineWidth);
                }
            }
        }

        // Draw Vertices
        for (int i = 0; i < ptCount; ++i) {
            if (screenPts[i].z < 0.0f || screenPts[i].z > 1.0f) continue;
            ImVec2 p(screenPts[i].x, screenPts[i].y);

            bool isExtrudeWeld = false;
            for (const auto& ev : m_extrudedVerts) {
                if (ev.weldTarget == i) { isExtrudeWeld = true; break; }
            }
            bool isTweakWeld = false;
            for (const auto& tv : m_tweakVerts) {
                if (tv.weldTarget == i) { isTweakWeld = true; break; }
            }

            if (i == m_weldTargetVertex || i == m_extrudeWeldTarget0 || i == m_extrudeWeldTarget1 || i == m_extrudeVertexWeldTarget || isExtrudeWeld || isTweakWeld) {
                // Weld Target highlight: Pulsing red
                drawList->AddCircleFilled(p, m_pointSize * 1.6f, IM_COL32(255, 30, 30, 240));
                drawList->AddCircle(p, m_pointSize * 2.2f, IM_COL32(255, 255, 255, 220), 16, 2.0f);
            } else if (i == m_hoverVertex) {
                if (m_subTool == QuadDrawSubTool::Extrude && m_builder.CanExtrudeVertex(m_retopoMesh, i)) {
                    // Extrudable corner vertex: Green highlight ring
                    drawList->AddCircleFilled(p, m_pointSize * 1.5f, IM_COL32(50, 255, 100, 255));
                    drawList->AddCircle(p, m_pointSize * 2.2f, IM_COL32(255, 255, 255, 220), 16, 2.0f);
                } else if (m_subTool == QuadDrawSubTool::Delete || (ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift)) {
                    // Hovered vertex in Delete mode: Red
                    drawList->AddCircleFilled(p, m_pointSize * 1.5f, IM_COL32(255, 40, 40, 255));
                    drawList->AddCircle(p, m_pointSize * 2.0f, IM_COL32(255, 255, 255, 200), 16, 1.5f);
                } else {
                    // Hovered vertex: Yellow
                    drawList->AddCircleFilled(p, m_pointSize * 1.4f, IM_COL32(255, 230, 40, 255));
                    drawList->AddCircle(p, m_pointSize * 1.8f, IM_COL32(0, 0, 0, 180), 16, 1.5f);
                }
            } else if (m_pinnedVertices.count((uint32_t)i)) {
                // Pinned vertex: Cyan
                drawList->AddCircleFilled(p, m_pointSize * 1.2f, IM_COL32(30, 220, 255, 240));
                drawList->AddCircle(p, m_pointSize * 1.4f, IM_COL32(0, 0, 0, 180), 16, 1.0f);
            } else {
                // Standard retopo vertex: Light grey / white
                drawList->AddCircleFilled(p, m_pointSize, IM_COL32(240, 240, 240, 220));
                drawList->AddCircle(p, m_pointSize, IM_COL32(40, 40, 40, 180), 12, 1.0f);
            }
        }

        const bool isDeleteMode = (m_subTool == QuadDrawSubTool::Delete || (ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift));

        // Draw Selected Edges
        if (!m_selectedEdges.empty()) {
            bool isExtrudeHover = (!isDeleteMode && m_hoverEdge.valid && m_subTool == QuadDrawSubTool::Extrude && isEdgeSelected(m_hoverEdge.v0, m_hoverEdge.v1));
            bool isTweakSelectionHover = (!isDeleteMode && m_subTool == QuadDrawSubTool::Tweak && isSelectionHovered());

            bool isDeleteHover = false;
            if (isDeleteMode) {
                if (m_hoverEdge.valid && isEdgeSelected(m_hoverEdge.v0, m_hoverEdge.v1)) {
                    isDeleteHover = true;
                } else if (m_hoverVertex != -1) {
                    for (const auto& e : m_selectedEdges) {
                        if (e.first == m_hoverVertex || e.second == m_hoverVertex) {
                            isDeleteHover = true;
                            break;
                        }
                    }
                } else if (m_hoverPoly != -1 && m_hoverPoly < polyCount) {
                    for (const auto& e : m_selectedEdges) {
                        if (PolygonHasEdge(faces, m_hoverPoly, e.first, e.second)) {
                            isDeleteHover = true;
                            break;
                        }
                    }
                }
            }

            ImU32 selColor = IM_COL32(255, 150, 0, 255);
            float selWidth = m_lineWidth * 2.8f;
            if (isDeleteMode) {
                if (isDeleteHover) {
                    selColor = IM_COL32(255, 30, 30, 255);
                    selWidth = m_lineWidth * 3.8f;
                } else {
                    selColor = IM_COL32(230, 60, 60, 240);
                    selWidth = m_lineWidth * 3.0f;
                }
            } else if (isExtrudeHover || isTweakSelectionHover) {
                selColor = IM_COL32(255, 255, 100, 255);
                selWidth = m_lineWidth * 3.6f;
            } else if (m_subTool == QuadDrawSubTool::Strip) {
                selColor = IM_COL32(255, 160, 20, 255);
                selWidth = m_lineWidth * 3.2f;
            }

            for (const auto& e : m_selectedEdges) {
                if (e.first < 0 || e.first >= ptCount || e.second < 0 || e.second >= ptCount) continue;
                glm::vec3 s0 = screenPts[e.first];
                glm::vec3 s1 = screenPts[e.second];
                if (s0.z > 0.0f && s0.z < 1.0f && s1.z > 0.0f && s1.z < 1.0f) {
                    drawList->AddLine(
                        ImVec2(s0.x, s0.y), ImVec2(s1.x, s1.y),
                        selColor, selWidth
                    );
                    drawList->AddCircleFilled(ImVec2(s0.x, s0.y), m_pointSize * 1.1f, selColor);
                    drawList->AddCircleFilled(ImVec2(s1.x, s1.y), m_pointSize * 1.1f, selColor);
                }
            }
        }

        // Draw Highlighted Face (in Select, Delete, or Tweak mode) - ONLY when not dragging
        if (m_dragMode == DragMode::None && m_hoverPoly != -1 && m_hoverPoly < polyCount && !m_hoverEdge.valid && m_hoverVertex == -1) {
            uint32_t q[4] = { faces[m_hoverPoly * 4 + 0], faces[m_hoverPoly * 4 + 1], faces[m_hoverPoly * 4 + 2], faces[m_hoverPoly * 4 + 3] };
            int numE = (q[3] != TRI_INDEX) ? 4 : 3;
            ImVec2 polyScreen[4];
            bool allFront = true;
            for (int e = 0; e < numE; ++e) {
                if (screenPts[q[e]].z < 0.0f || screenPts[q[e]].z > 1.0f) { allFront = false; break; }
                polyScreen[e] = ImVec2(screenPts[q[e]].x, screenPts[q[e]].y);
            }
            if (allFront) {
                if (m_subTool == QuadDrawSubTool::Select) {
                    ImU32 fillCol = ImGui::GetIO().KeyAlt ? IM_COL32(255, 60, 60, 80) : IM_COL32(50, 220, 255, 80);
                    ImU32 lineCol = ImGui::GetIO().KeyAlt ? IM_COL32(255, 80, 80, 220) : IM_COL32(80, 240, 255, 220);
                    drawList->AddConvexPolyFilled(polyScreen, numE, fillCol);
                    drawList->AddPolyline(polyScreen, numE, lineCol, ImDrawFlags_Closed, 2.0f);
                } else if (isDeleteMode) {
                    drawList->AddConvexPolyFilled(polyScreen, numE, IM_COL32(255, 40, 40, 90));
                    drawList->AddPolyline(polyScreen, numE, IM_COL32(255, 60, 60, 220), ImDrawFlags_Closed, 2.5f);
                } else if (m_subTool == QuadDrawSubTool::Tweak) {
                    ImU32 fillCol = IM_COL32(40, 190, 255, 75);
                    ImU32 lineCol = IM_COL32(70, 230, 255, 220);
                    drawList->AddConvexPolyFilled(polyScreen, numE, fillCol);
                    drawList->AddPolyline(polyScreen, numE, lineCol, ImDrawFlags_Closed, m_lineWidth * 2.0f);
                    for (int e = 0; e < numE; ++e) {
                        drawList->AddCircleFilled(polyScreen[e], m_pointSize * 1.1f, lineCol);
                    }
                }
            }
        }

        // Draw Highlighted Edge - ONLY when not dragging
        if (m_dragMode == DragMode::None && m_hoverEdge.valid && (m_subTool == QuadDrawSubTool::Extrude || m_subTool == QuadDrawSubTool::Select || m_subTool == QuadDrawSubTool::Strip || m_subTool == QuadDrawSubTool::Tweak || isDeleteMode)) {
            glm::vec3 s0;
            glm::vec3 s1;
            if (m_hoverEdge.v0 >= 0 && m_hoverEdge.v0 < ptCount && m_hoverEdge.v1 >= 0 && m_hoverEdge.v1 < ptCount) {
                s0 = screenPts[m_hoverEdge.v0];
                s1 = screenPts[m_hoverEdge.v1];
            } else {
                s0 = camera.project(m_hoverEdge.worldPos0);
                s1 = camera.project(m_hoverEdge.worldPos1);
            }
            if (s0.z > 0.0f && s1.z > 0.0f) {
                ImU32 edgeColor = IM_COL32(255, 230, 40, 255);
                if (m_subTool == QuadDrawSubTool::Select || m_subTool == QuadDrawSubTool::Strip) {
                    if (ImGui::GetIO().KeyAlt) {
                        edgeColor = IM_COL32(255, 60, 60, 255);
                    } else if (isEdgeSelected(m_hoverEdge.v0, m_hoverEdge.v1)) {
                        edgeColor = IM_COL32(255, 200, 50, 255);
                    } else {
                        edgeColor = IM_COL32(50, 230, 255, 255);
                    }
                } else if (isDeleteMode) {
                    edgeColor = IM_COL32(255, 40, 40, 255);
                } else if (m_subTool == QuadDrawSubTool::Tweak) {
                    if (isEdgeSelected(m_hoverEdge.v0, m_hoverEdge.v1)) {
                        edgeColor = IM_COL32(255, 255, 90, 255);
                    } else {
                        edgeColor = IM_COL32(60, 230, 255, 255);
                    }
                }
                drawList->AddLine(
                    ImVec2(s0.x, s0.y), ImVec2(s1.x, s1.y),
                    edgeColor, m_lineWidth * 2.8f
                );
                drawList->AddCircleFilled(ImVec2(s0.x, s0.y), m_pointSize * 1.1f, edgeColor);
                drawList->AddCircleFilled(ImVec2(s1.x, s1.y), m_pointSize * 1.1f, edgeColor);
            }
        }

        // Draw active moving components (edge or polygon) & weld lines during TweakDrag
        if (m_dragMode == DragMode::TweakDrag) {
            if (m_tweakSource == TweakSource::Edge) {
                int32_t u = m_tweakEdgeV0;
                int32_t v = m_tweakEdgeV1;
                if (u >= 0 && u < ptCount && v >= 0 && v < ptCount) {
                    glm::vec3 s0 = screenPts[u];
                    glm::vec3 s1 = screenPts[v];
                    if (s0.z > 0.0f && s1.z > 0.0f) {
                        ImU32 edgeColor = isEdgeSelected(u, v) ? IM_COL32(255, 255, 90, 255) : IM_COL32(60, 230, 255, 255);
                        drawList->AddLine(
                            ImVec2(s0.x, s0.y), ImVec2(s1.x, s1.y),
                            edgeColor, m_lineWidth * 3.0f
                        );
                        drawList->AddCircleFilled(ImVec2(s0.x, s0.y), m_pointSize * 1.1f, edgeColor);
                        drawList->AddCircleFilled(ImVec2(s1.x, s1.y), m_pointSize * 1.1f, edgeColor);
                    }
                }
            } else if (m_tweakSource == TweakSource::Polygon && m_tweakPoly >= 0 && m_tweakPoly < polyCount) {
                uint32_t q[4] = { faces[m_tweakPoly * 4 + 0], faces[m_tweakPoly * 4 + 1], faces[m_tweakPoly * 4 + 2], faces[m_tweakPoly * 4 + 3] };
                int numE = (q[3] != TRI_INDEX) ? 4 : 3;
                ImVec2 polyScreen[4];
                bool allFront = true;
                for (int e = 0; e < numE; ++e) {
                    int32_t vi = (int32_t)q[e];
                    if (vi < 0 || vi >= ptCount || screenPts[vi].z < 0.0f || screenPts[vi].z > 1.0f) {
                        allFront = false;
                        break;
                    }
                    polyScreen[e] = ImVec2(screenPts[vi].x, screenPts[vi].y);
                }
                if (allFront) {
                    ImU32 fillCol = IM_COL32(40, 190, 255, 75);
                    ImU32 lineCol = IM_COL32(70, 230, 255, 220);
                    drawList->AddConvexPolyFilled(polyScreen, numE, fillCol);
                    drawList->AddPolyline(polyScreen, numE, lineCol, ImDrawFlags_Closed, m_lineWidth * 2.0f);
                    for (int e = 0; e < numE; ++e) {
                        drawList->AddCircleFilled(polyScreen[e], m_pointSize * 1.1f, lineCol);
                    }
                }
            }

            for (const auto& tv : m_tweakVerts) {
                if (tv.weldTarget != -1 && tv.weldTarget < ptCount) {
                    glm::vec3 sSrc = screenPts[tv.vertexIdx];
                    glm::vec3 sTgt = screenPts[tv.weldTarget];
                    if (sSrc.z > 0.0f && sTgt.z > 0.0f) {
                        drawList->AddLine(
                            ImVec2(sSrc.x, sSrc.y), ImVec2(sTgt.x, sTgt.y),
                            IM_COL32(50, 255, 100, 255), 2.2f
                        );
                        drawList->AddCircleFilled(
                            ImVec2(sTgt.x, sTgt.y), m_pointSize * 1.6f,
                            IM_COL32(50, 255, 100, 255)
                        );
                    }
                }
                if (tv.vertexIdx >= 0 && tv.vertexIdx < ptCount) {
                    glm::vec3 s = screenPts[tv.vertexIdx];
                    if (s.z > 0.0f && s.z < 1.0f) {
                        drawList->AddCircleFilled(ImVec2(s.x, s.y), m_pointSize * 1.3f, IM_COL32(50, 220, 255, 255));
                        drawList->AddCircle(ImVec2(s.x, s.y), m_pointSize * 1.6f, IM_COL32(255, 255, 255, 200), 16, 1.5f);
                    }
                }
            }
        }

        // Draw Edge Loop Highlight - ONLY when not dragging
        if (m_dragMode == DragMode::None && !m_hoverEdgeLoop.edges.empty()) {
            for (const auto& le : m_hoverEdgeLoop.edges) {
                glm::vec3 s0 = camera.project(le.worldPos0);
                glm::vec3 s1 = camera.project(le.worldPos1);
                if (s0.z > 0.0f && s1.z > 0.0f) {
                    drawList->AddLine(
                        ImVec2(s0.x, s0.y), ImVec2(s1.x, s1.y),
                        IM_COL32(40, 190, 255, 255), m_lineWidth * 2.2f
                    );
                }
            }
        }

        // Draw Knife / Multi-Cut Cut Preview
        if (m_dragMode == DragMode::None && m_hoverCut.valid) {
            for (const auto& cs : m_hoverCut.cutSegments) {
                glm::vec3 s0 = camera.project(cs.p0);
                glm::vec3 s1 = camera.project(cs.p1);
                if (s0.z > 0.0f && s1.z > 0.0f) {
                    drawList->AddLine(
                        ImVec2(s0.x, s0.y), ImVec2(s1.x, s1.y),
                        IM_COL32(50, 255, 100, 255), 2.5f
                    );
                }
            }
            for (const auto& cp : m_hoverCut.cutPoints) {
                glm::vec3 sp = camera.project(cp.worldPos);
                if (sp.z > 0.0f) {
                    drawList->AddCircleFilled(ImVec2(sp.x, sp.y), 4.5f, IM_COL32(50, 255, 100, 255));
                }
            }
        }
    }

    // 3. Draw Multi-Cut Placed Chain & Live Preview
    if (m_subTool == QuadDrawSubTool::MultiCut && m_retopoMesh) {
        MultiCutPoint cand;
        bool hasCand = false;
        if (m_hoverVertex != -1) {
            hasCand = true;
            cand.type = MultiCutSnapType::Vertex;
            cand.vertexIdx = m_hoverVertex;
            glm::vec3 p(
                m_retopoMesh->verts[m_hoverVertex * 3],
                m_retopoMesh->verts[m_hoverVertex * 3 + 1],
                m_retopoMesh->verts[m_hoverVertex * 3 + 2]
            );
            cand.worldPos = (m_retopoMesh->matrix != glm::mat4(1.0f)) ? glm::vec3(m_retopoMesh->matrix * glm::vec4(p, 1.0f)) : p;
        } else if (m_hoverEdge.valid) {
            hasCand = true;
            cand.type = MultiCutSnapType::Edge;
            cand.edgeV0 = m_hoverEdge.v0;
            cand.edgeV1 = m_hoverEdge.v1;
            cand.edgeT = m_hoverEdge.t;
            cand.worldPos = glm::mix(m_hoverEdge.worldPos0, m_hoverEdge.worldPos1, m_hoverEdge.t);
        } else if (m_hoverPoly != -1 && m_hoverSnap.valid) {
            hasCand = true;
            cand.type = MultiCutSnapType::Face;
            cand.polyIndex = m_hoverPoly;
            cand.worldPos = m_hoverSnap.worldPos;
        }

        // Draw preview cuts from builder
        if (!m_multiCutPoints.empty()) {
            MultiCutResult preview = m_builder.BuildMultiCutFromPoints(
                camera, m_retopoMesh, m_targetMesh, m_snapper, m_multiCutPoints, hasCand ? &cand : nullptr
            );
            for (const auto& seg : preview.previewSegments) {
                glm::vec3 sp0 = camera.project(seg.p0);
                glm::vec3 sp1 = camera.project(seg.p1);
                if (sp0.z > 0.0f && sp1.z > 0.0f) {
                    drawList->AddLine(ImVec2(sp0.x, sp0.y), ImVec2(sp1.x, sp1.y), IM_COL32(255, 180, 20, 255), 2.5f);
                }
            }
        }

        // Draw placed points
        for (size_t i = 0; i < m_multiCutPoints.size(); ++i) {
            glm::vec3 sp = camera.project(m_multiCutPoints[i].worldPos);
            if (sp.z > 0.0f) {
                drawList->AddCircleFilled(ImVec2(sp.x, sp.y), 5.5f, IM_COL32(255, 180, 20, 255));
                drawList->AddCircle(ImVec2(sp.x, sp.y), 6.5f, IM_COL32(0, 0, 0, 220), 12, 1.5f);
            }
        }

        // Draw hover candidate indicator
        if (hasCand) {
            glm::vec3 sp = camera.project(cand.worldPos);
            if (sp.z > 0.0f) {
                ImU32 candColor = (cand.type == MultiCutSnapType::Vertex)
                    ? IM_COL32(50, 255, 120, 255)
                    : IM_COL32(0, 220, 255, 255);
                drawList->AddCircleFilled(ImVec2(sp.x, sp.y), 4.5f, candColor);
                drawList->AddCircle(ImVec2(sp.x, sp.y), 6.0f, IM_COL32(255, 255, 255, 220), 12, 1.2f);

                if (cand.type == MultiCutSnapType::Edge && ImGui::GetIO().KeyShift) {
                    char snapBuf[32];
                    snprintf(snapBuf, sizeof(snapBuf), "%.0f%%", cand.edgeT * 100.0f);
                    drawList->AddText(ImVec2(sp.x + 10, sp.y - 12), IM_COL32(255, 255, 100, 255), snapBuf);
                }
            }
        }
    }

    // 4. Draw Screen Slice Line & Real-time preview of cut segments
    if (m_dragMode == DragMode::SliceDrag) {
        drawList->AddLine(
            ImVec2(m_sliceStartScreen.x, m_sliceStartScreen.y),
            ImVec2(m_sliceEndScreen.x, m_sliceEndScreen.y),
            IM_COL32(255, 70, 70, 255), 2.5f
        );

        if (m_retopoMesh) {
            MultiCutResult slice = m_builder.BuildSliceCut(
                camera, m_retopoMesh, m_targetMesh, m_snapper, m_sliceStartScreen, m_sliceEndScreen
            );
            for (const auto& seg : slice.previewSegments) {
                glm::vec3 sp0 = camera.project(seg.p0);
                glm::vec3 sp1 = camera.project(seg.p1);
                if (sp0.z > 0.0f && sp1.z > 0.0f) {
                    drawList->AddLine(ImVec2(sp0.x, sp0.y), ImVec2(sp1.x, sp1.y), IM_COL32(50, 255, 150, 255), 3.0f);
                    drawList->AddCircleFilled(ImVec2(sp0.x, sp0.y), 4.0f, IM_COL32(50, 255, 150, 255));
                    drawList->AddCircleFilled(ImVec2(sp1.x, sp1.y), 4.0f, IM_COL32(50, 255, 150, 255));
                }
            }
        }
    }

    // 5. Draw Brush Circle for Grab, Relax, Pin, or BrushResize
    if (m_subTool == QuadDrawSubTool::Grab || m_subTool == QuadDrawSubTool::Pin ||
        m_dragMode == DragMode::BrushResize || m_dragMode == DragMode::RelaxDrag ||
        (ImGui::GetIO().KeyShift && m_subTool != QuadDrawSubTool::CreatePolygon)) {
        ImVec2 mousePos = ImGui::GetIO().MousePos;
        drawList->AddCircle(mousePos, m_brushRadius, IM_COL32(240, 240, 240, 160), 48, 1.5f);
        drawList->AddLine(ImVec2(mousePos.x - 6, mousePos.y), ImVec2(mousePos.x + 6, mousePos.y), IM_COL32(240, 240, 240, 200), 1.0f);
        drawList->AddLine(ImVec2(mousePos.x, mousePos.y - 6), ImVec2(mousePos.x, mousePos.y + 6), IM_COL32(240, 240, 240, 200), 1.0f);
    }

    // 6. Draw Strip Stroke & Live Quad Previews
    if (m_subTool == QuadDrawSubTool::Strip) {
        // Draw live preview quads
        if (!m_strokeQuadPreviews.empty()) {
            for (const auto& qp : m_strokeQuadPreviews) {
                if (!qp.valid) continue;
                bool allFront = true;
                ImVec2 pts[4];
                for (int k = 0; k < 4; ++k) {
                    if (qp.screenPositions[k].z <= 0.0f || qp.screenPositions[k].z >= 1.0f) {
                        allFront = false;
                        break;
                    }
                    pts[k] = ImVec2(qp.screenPositions[k].x, qp.screenPositions[k].y);
                }
                if (allFront) {
                    drawList->AddConvexPolyFilled(pts, 4, IM_COL32(40, 230, 90, 110));
                    drawList->AddPolyline(pts, 4, IM_COL32(80, 255, 120, 240), ImDrawFlags_Closed, 2.5f);
                    drawList->AddCircleFilled(pts[2], m_pointSize * 0.9f, IM_COL32(80, 255, 120, 255));
                    drawList->AddCircleFilled(pts[3], m_pointSize * 0.9f, IM_COL32(80, 255, 120, 255));
                }
            }
        }

        // Draw drawn stroke line
        if (m_strokeScreenPoints.size() >= 2) {
            std::vector<ImVec2> imPts;
            imPts.reserve(m_strokeScreenPoints.size());
            for (const auto& p : m_strokeScreenPoints) {
                imPts.push_back(ImVec2(p.x, p.y));
            }
            drawList->AddPolyline(imPts.data(), (int)imPts.size(), IM_COL32(0, 0, 0, 160), 0, 5.0f);
            drawList->AddPolyline(imPts.data(), (int)imPts.size(), IM_COL32(255, 215, 30, 255), 0, 3.0f);
            drawList->AddCircleFilled(imPts.front(), 5.0f, IM_COL32(100, 255, 100, 255));
            drawList->AddCircleFilled(imPts.back(), 5.0f, IM_COL32(255, 100, 100, 255));
        }

        char hudBuf[256];
        if (m_selectedEdges.empty()) {
            snprintf(hudBuf, sizeof(hudBuf), "Strip Tool: Click border edge to select | Ctrl+Click for chain");
        } else {
            snprintf(hudBuf, sizeof(hudBuf), "Strip Tool: %zu edge(s) selected | Draw stroke opposite edges to build quads", m_selectedEdges.size());
        }
        ImVec2 hudPos(20.0f, vH - 35.0f);
        drawList->AddText(ImVec2(hudPos.x + 1, hudPos.y + 1), IM_COL32(0, 0, 0, 220), hudBuf);
        drawList->AddText(hudPos, IM_COL32(255, 220, 60, 240), hudBuf);
    }

    // 7. Draw Tube Slice Tool Overlay & Preview
    if (m_subTool == QuadDrawSubTool::TubeSlice) {
        auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
            glm::vec3 p(m_retopoMesh->verts[idx * 3], m_retopoMesh->verts[idx * 3 + 1], m_retopoMesh->verts[idx * 3 + 2]);
            return (m_retopoMesh->matrix != glm::mat4(1.0f)) ? glm::vec3(m_retopoMesh->matrix * glm::vec4(p, 1.0f)) : p;
        };

        // Highlight active tube ring
        if (m_retopoMesh && !m_lastTubeSliceLoop.empty()) {
            int32_t N_prev = (int32_t)m_lastTubeSliceLoop.size();
            std::vector<ImVec2> activeRingScreen;
            activeRingScreen.reserve(N_prev);
            for (int32_t v : m_lastTubeSliceLoop) {
                if (v >= 0 && v < m_retopoMesh->nbVerts) {
                    glm::vec3 sp = camera.project(getVertWorld(v));
                    if (sp.z > 0.0f && sp.z < 1.0f) {
                        activeRingScreen.push_back(ImVec2(sp.x, sp.y));
                        drawList->AddCircleFilled(activeRingScreen.back(), m_pointSize * 1.1f, IM_COL32(0, 255, 200, 255));
                    }
                }
            }
            if (activeRingScreen.size() == (size_t)N_prev && N_prev >= 3) {
                drawList->AddPolyline(activeRingScreen.data(), N_prev, IM_COL32(0, 255, 200, 220), ImDrawFlags_Closed, 2.5f);
            }
        }

        // Draw live cut line & candidate ring during drag
        if (m_dragMode == DragMode::TubeSliceDrag) {
            drawList->AddLine(
                ImVec2(m_tubeSliceStartScreen.x, m_tubeSliceStartScreen.y),
                ImVec2(m_tubeSliceEndScreen.x, m_tubeSliceEndScreen.y),
                IM_COL32(0, 220, 255, 240), 2.5f
            );
            drawList->AddCircleFilled(ImVec2(m_tubeSliceStartScreen.x, m_tubeSliceStartScreen.y), 4.5f, IM_COL32(0, 255, 180, 255));
            drawList->AddCircleFilled(ImVec2(m_tubeSliceEndScreen.x, m_tubeSliceEndScreen.y), 4.5f, IM_COL32(255, 200, 0, 255));

            if (m_tubeSlicePreviewResult.valid && !m_tubeSlicePreviewResult.worldPoints.empty()) {
                const auto& pts = m_tubeSlicePreviewResult.worldPoints;
                int N_pts = (int)pts.size();
                std::vector<ImVec2> ringScreen(N_pts);
                glm::mat4 w2s = camera.computeWorldToScreenMatrix();
                bool allVisible = true;
                for (int i = 0; i < N_pts; ++i) {
                    glm::vec4 clip = w2s * glm::vec4(pts[i], 1.0f);
                    if (clip.w <= 0.001f) {
                        allVisible = false;
                        break;
                    }
                    glm::vec3 sp = camera.project(pts[i]);
                    ringScreen[i] = ImVec2(sp.x, sp.y);
                }

                if (allVisible) {
                    if (m_retopoMesh && !m_lastTubeSliceLoop.empty() && (int)m_lastTubeSliceLoop.size() == N_pts) {
                        std::vector<ImVec2> prevScreen(N_pts);
                        bool prevVis = true;
                        for (int i = 0; i < N_pts; ++i) {
                            glm::vec3 wP = getVertWorld(m_lastTubeSliceLoop[i]);
                            glm::vec4 clip = w2s * glm::vec4(wP, 1.0f);
                            if (clip.w <= 0.001f) { prevVis = false; break; }
                            glm::vec3 sp = camera.project(wP);
                            prevScreen[i] = ImVec2(sp.x, sp.y);
                        }
                        if (prevVis) {
                            for (int i = 0; i < N_pts; ++i) {
                                int iNext = (i + 1) % N_pts;
                                ImVec2 qpts[4] = { prevScreen[i], prevScreen[iNext], ringScreen[iNext], prevScreen[iNext] }; // quad preview
                                qpts[2] = ringScreen[iNext];
                                qpts[3] = ringScreen[i];
                                drawList->AddConvexPolyFilled(qpts, 4, IM_COL32(0, 220, 180, 50));
                                drawList->AddLine(prevScreen[i], ringScreen[i], IM_COL32(255, 215, 60, 180), 1.5f);
                            }
                        }
                    }

                    drawList->AddPolyline(ringScreen.data(), N_pts, IM_COL32(80, 255, 120, 255), ImDrawFlags_Closed, 2.5f);
                    for (int i = 0; i < N_pts; ++i) {
                        drawList->AddCircleFilled(ringScreen[i], m_pointSize, IM_COL32(100, 255, 100, 255));
                    }
                }
            }
        }

        char hudBuf[256];
        if (m_lastTubeSliceLoop.empty()) {
            snprintf(hudBuf, sizeof(hudBuf), "Tube Slice: Draw line across limb to start tube (%d divisions)", m_tubeSliceRadialSegments);
        } else {
            snprintf(hudBuf, sizeof(hudBuf), "Tube Slice: Active ring (%zu verts) | Draw next cut to bridge tube | Esc: End tube", m_lastTubeSliceLoop.size());
        }
        ImVec2 hudPos(20.0f, vH - 35.0f);
        drawList->AddText(ImVec2(hudPos.x + 1, hudPos.y + 1), IM_COL32(0, 0, 0, 220), hudBuf);
        drawList->AddText(hudPos, IM_COL32(0, 220, 255, 240), hudBuf);
    }

    // 8. Draw Tube Curve Tool Overlay & Preview
    if (m_subTool == QuadDrawSubTool::TubeCurve) {
        glm::mat4 w2s = camera.computeWorldToScreenMatrix();
        size_t numPts = m_tubeCurvePoints.size();

        // 1. Draw rubber-band guide line from last point to cursor
        if (numPts >= 1) {
            glm::vec3 lastWorld = m_tubeCurvePoints.back();
            glm::vec3 spLast = camera.project(lastWorld);
            glm::vec3 spCur = camera.project(m_tubeCurveCursorWorld);

            if (spLast.z > 0.0f && spLast.z < 1.0f && spCur.z > 0.0f && spCur.z < 1.0f) {
                drawList->AddLine(ImVec2(spLast.x, spLast.y), ImVec2(spCur.x, spCur.y), IM_COL32(0, 220, 255, 200), 2.0f);
            }
            if (spCur.z > 0.0f && spCur.z < 1.0f) {
                drawList->AddCircleFilled(ImVec2(spCur.x, spCur.y), 5.0f, IM_COL32(255, 220, 0, 240));
                char pbuf[32];
                snprintf(pbuf, sizeof(pbuf), "P%zu", numPts);
                drawList->AddText(ImVec2(spCur.x + 8.0f, spCur.y - 10.0f), IM_COL32(255, 220, 0, 240), pbuf);
            }
        }

        // 2. Draw centerline axis between all consecutive points
        if (numPts >= 2) {
            for (size_t i = 0; i + 1 < numPts; ++i) {
                glm::vec3 spA = camera.project(m_tubeCurvePoints[i]);
                glm::vec3 spB = camera.project(m_tubeCurvePoints[i + 1]);
                if (spA.z > 0.0f && spA.z < 1.0f && spB.z > 0.0f && spB.z < 1.0f) {
                    drawList->AddLine(ImVec2(spA.x, spA.y), ImVec2(spB.x, spB.y), IM_COL32(255, 220, 60, 200), 2.0f);
                }
            }

            // Draw full tube preview if valid
            if (m_tubeCurveGeomPreview.valid && !m_tubeCurveGeomPreview.rings.empty()) {
                int numRings = (int)m_tubeCurveGeomPreview.rings.size();
                int N = m_tubeCurveGeomPreview.radialSegments;

                std::vector<std::vector<ImVec2>> screenRings(numRings, std::vector<ImVec2>(N));
                std::vector<std::vector<bool>> screenVis(numRings, std::vector<bool>(N, false));

                for (int r = 0; r < numRings; ++r) {
                    for (int j = 0; j < N; ++j) {
                        const glm::vec3& wP = m_tubeCurveGeomPreview.rings[r][j];
                        glm::vec4 clip = w2s * glm::vec4(wP, 1.0f);
                        if (clip.w > 0.001f) {
                            glm::vec3 sp = camera.project(wP);
                            if (sp.z > 0.0f && sp.z < 1.0f) {
                                screenRings[r][j] = ImVec2(sp.x, sp.y);
                                screenVis[r][j] = true;
                            }
                        }
                    }
                }

                // Render quad faces & longitudinal edges between adjacent rings
                for (int r = 0; r + 1 < numRings; ++r) {
                    for (int j = 0; j < N; ++j) {
                        int jNext = (j + 1) % N;
                        if (screenVis[r][j] && screenVis[r][jNext] && screenVis[r + 1][jNext] && screenVis[r + 1][j]) {
                            ImVec2 qpts[4] = {
                                screenRings[r][j],
                                screenRings[r][jNext],
                                screenRings[r + 1][jNext],
                                screenRings[r + 1][j]
                            };
                            // Quad preview color matching standard polygon creation preview
                            drawList->AddConvexPolyFilled(qpts, 4, IM_COL32(40, 230, 90, 110));
                            drawList->AddLine(screenRings[r][j], screenRings[r + 1][j], IM_COL32(80, 255, 120, 240), 1.5f);
                        }
                    }
                }

                // Render ring loops
                for (int r = 0; r < numRings; ++r) {
                    bool allRingVis = true;
                    for (int j = 0; j < N; ++j) {
                        if (!screenVis[r][j]) { allRingVis = false; break; }
                    }
                    if (allRingVis) {
                        ImU32 ringCol = (r == 0 || r == numRings - 1)
                            ? IM_COL32(80, 255, 120, 255)
                            : IM_COL32(80, 255, 120, 220);
                        float lineThick = (r == 0 || r == numRings - 1) ? 2.5f : 1.5f;
                        drawList->AddPolyline(screenRings[r].data(), N, ringCol, ImDrawFlags_Closed, lineThick);
                        for (int j = 0; j < N; ++j) {
                            drawList->AddCircleFilled(screenRings[r][j], 2.5f, IM_COL32(80, 255, 120, 255));
                        }
                    }
                }
            }
        }

        // 3. Draw interactive handles for all points
        for (size_t i = 0; i < numPts; ++i) {
            glm::vec3 sp = camera.project(m_tubeCurvePoints[i]);
            if (sp.z > 0.0f && sp.z < 1.0f) {
                bool isHovered = ((int32_t)i == m_tubeCurveHoveredPointIdx) || ((int32_t)i == m_tubeCurveDraggingPointIdx);
                float radius = isHovered ? 10.0f : 7.0f;
                bool isEnd = (i == 0 || i == numPts - 1);

                ImU32 fillCol = isEnd
                    ? (i == 0 ? IM_COL32(0, 255, 180, 255) : IM_COL32(255, 190, 0, 255))
                    : IM_COL32(0, 220, 255, 255);
                if (isHovered) fillCol = IM_COL32(255, 255, 255, 255);

                drawList->AddCircleFilled(ImVec2(sp.x, sp.y), radius, fillCol);
                drawList->AddCircle(ImVec2(sp.x, sp.y), radius + 3.5f, IM_COL32(0, 0, 0, 220), 0, 2.0f);

                char lbuf[64];
                if (i == 0) {
                    snprintf(lbuf, sizeof(lbuf), "P0 (Start - Alt+Drag to Extrude)");
                } else if (i == numPts - 1) {
                    snprintf(lbuf, sizeof(lbuf), "P%zu (End - Alt+Drag to Extrude)", i);
                } else {
                    snprintf(lbuf, sizeof(lbuf), "P%zu", i);
                }
                drawList->AddText(ImVec2(sp.x + radius + 5.0f, sp.y - 10.0f), fillCol, lbuf);
            }
        }

        // 4. Draw snap/collapse indicator if dragging onto another point
        if (m_dragMode == DragMode::TubeCurveDrag && m_tubeCurveWeldTargetPointIdx >= 0 && m_tubeCurveWeldTargetPointIdx < (int32_t)numPts) {
            glm::vec3 sp = camera.project(m_tubeCurvePoints[m_tubeCurveWeldTargetPointIdx]);
            if (sp.z > 0.0f && sp.z < 1.0f) {
                drawList->AddCircle(ImVec2(sp.x, sp.y), 15.0f, IM_COL32(50, 255, 100, 255), 24, 2.5f);
                drawList->AddCircleFilled(ImVec2(sp.x, sp.y), 6.5f, IM_COL32(50, 255, 100, 220));
                drawList->AddText(ImVec2(sp.x + 18.0f, sp.y - 10.0f), IM_COL32(50, 255, 100, 255), "Collapse Point");
            }
        }

        // HUD Banner (Strictly English)
        char hudBuf[256];
        if (numPts == 0) {
            snprintf(hudBuf, sizeof(hudBuf), "Tube Curve: Click on mesh to place Start Point (P0)");
        } else if (numPts == 1) {
            snprintf(hudBuf, sizeof(hudBuf), "Tube Curve: Move cursor & click to place P1 | Drag P0 to reposition");
        } else {
            snprintf(hudBuf, sizeof(hudBuf), "Tube Curve: %zu Points | %d Quads | Drag: Move / Collapse | Alt+Drag End: Extrude | [Enter] Commit | [Backspace] Undo Pt | [Esc] Reset",
                numPts, m_tubeCurveGeomPreview.quadsCount);
        }
        ImVec2 hudPos(20.0f, vH - 35.0f);
        drawList->AddText(ImVec2(hudPos.x + 1, hudPos.y + 1), IM_COL32(0, 0, 0, 220), hudBuf);
        drawList->AddText(hudPos, IM_COL32(0, 230, 255, 240), hudBuf);
    }
}

void QuadDrawTool::cancelStroke() {
    m_strokeScreenPoints.clear();
    m_strokeWorldPoints.clear();
    m_strokeQuadPreviews.clear();
    m_edgeClickCandidate = false;
    m_candidateEdge.valid = false;
    m_dragMode = DragMode::None;
}

void QuadDrawTool::updateStrokePreview(const Camera& camera) {
    m_strokeQuadPreviews.clear();
    if (!m_retopoMesh || m_strokeWorldPoints.size() < 2) return;

    if (m_selectedEdges.empty()) {
        glm::vec3 strokeMid = m_strokeWorldPoints[m_strokeWorldPoints.size() / 2];
        float bestDist = 1e9f;
        int32_t bestV0 = -1, bestV1 = -1;
        int32_t polyCount = m_retopoMesh->nbFaces;

        auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
            glm::vec3 p(m_retopoMesh->verts[idx * 3], m_retopoMesh->verts[idx * 3 + 1], m_retopoMesh->verts[idx * 3 + 2]);
            return (m_retopoMesh->matrix != glm::mat4(1.0f)) ? glm::vec3(m_retopoMesh->matrix * glm::vec4(p, 1.0f)) : p;
        };

        for (int32_t p = 0; p < polyCount; ++p) {
            uint32_t q[4] = {
                m_retopoMesh->faces[p * 4 + 0],
                m_retopoMesh->faces[p * 4 + 1],
                m_retopoMesh->faces[p * 4 + 2],
                m_retopoMesh->faces[p * 4 + 3]
            };
            int numE = (q[3] != TRI_INDEX) ? 4 : 3;
            for (int e = 0; e < numE; ++e) {
                int32_t u = (int32_t)q[e];
                int32_t v = (int32_t)q[(e + 1) % numE];
                if (u > v) std::swap(u, v);
                int count = 0;
                for (int32_t p2 = 0; p2 < polyCount; ++p2) {
                    if (PolygonHasEdge(m_retopoMesh->faces, p2, u, v)) count++;
                }
                if (count == 1) {
                    glm::vec3 eMid = (getVertWorld(u) + getVertWorld(v)) * 0.5f;
                    float d = glm::distance(eMid, strokeMid);
                    if (d < bestDist) {
                        bestDist = d;
                        bestV0 = u;
                        bestV1 = v;
                    }
                }
            }
        }

        if (bestV0 != -1 && bestV1 != -1) {
            BorderChain bc = m_builder.FindBorderChain(m_retopoMesh, bestV0, bestV1);
            if (!bc.edges.empty()) {
                for (const auto& edge : bc.edges) {
                    selectEdge(edge.first, edge.second);
                }
            }
        }
    }

    if (m_selectedEdges.empty()) return;

    std::vector<BorderChain> chains = m_builder.ExtractBorderChains(m_retopoMesh, m_selectedEdges);
    if (chains.empty()) return;

    auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
        glm::vec3 p(m_retopoMesh->verts[idx * 3], m_retopoMesh->verts[idx * 3 + 1], m_retopoMesh->verts[idx * 3 + 2]);
        return (m_retopoMesh->matrix != glm::mat4(1.0f)) ? glm::vec3(m_retopoMesh->matrix * glm::vec4(p, 1.0f)) : p;
    };

    int bestChainIdx = 0;
    float bestChainDist = 1e9f;
    for (size_t c = 0; c < chains.size(); ++c) {
        float minDist = 1e9f;
        for (int32_t v : chains[c].vertices) {
            glm::vec3 vPos = getVertWorld(v);
            for (const auto& sp : m_strokeWorldPoints) {
                minDist = std::min(minDist, glm::distance(vPos, sp));
            }
        }
        if (minDist < bestChainDist) {
            bestChainDist = minDist;
            bestChainIdx = (int)c;
        }
    }

    const BorderChain& chain = chains[bestChainIdx];
    int32_t N = (int32_t)chain.edges.size();
    if (N == 0 || chain.vertices.size() < (size_t)(N + 1)) return;

    std::vector<glm::vec3> stroke = m_strokeWorldPoints;
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
    if (totalStrokeLen < 1e-5f) return;

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
            resampledPts[i] = glm::mix(stroke[seg], stroke[seg + 1], std::clamp(alpha, 0.0f, 1.0f));
        }

        if (m_targetMesh && m_targetMesh->nbFaces > 0) {
            SnapResult snap = m_snapper.SnapClosestPoint(m_targetMesh, resampledPts[i], 500.0f);
            if (snap.valid) resampledPts[i] = snap.worldPos;
        }
    }

    for (int32_t i = 0; i < N; ++i) {
        QuadPreview qp;
        qp.valid = true;
        qp.v[0] = chain.vertices[i];
        qp.v[1] = chain.vertices[i + 1];
        qp.worldPositions[0] = getVertWorld(chain.vertices[i]);
        qp.worldPositions[1] = getVertWorld(chain.vertices[i + 1]);
        qp.worldPositions[2] = resampledPts[i + 1];
        qp.worldPositions[3] = resampledPts[i];
        for (int k = 0; k < 4; ++k) {
            qp.screenPositions[k] = camera.project(qp.worldPositions[k]);
        }
        m_strokeQuadPreviews.push_back(qp);
    }
}

void QuadDrawTool::commitStrokeExtrude(Scene& scene, const Camera& camera) {
    if (!m_retopoMesh || m_strokeWorldPoints.size() < 2) return;

    if (m_selectedEdges.empty()) {
        glm::vec3 strokeMid = m_strokeWorldPoints[m_strokeWorldPoints.size() / 2];
        float bestDist = 1e9f;
        int32_t bestV0 = -1, bestV1 = -1;
        int32_t polyCount = m_retopoMesh->nbFaces;

        auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
            glm::vec3 p(m_retopoMesh->verts[idx * 3], m_retopoMesh->verts[idx * 3 + 1], m_retopoMesh->verts[idx * 3 + 2]);
            return (m_retopoMesh->matrix != glm::mat4(1.0f)) ? glm::vec3(m_retopoMesh->matrix * glm::vec4(p, 1.0f)) : p;
        };

        for (int32_t p = 0; p < polyCount; ++p) {
            uint32_t q[4] = {
                m_retopoMesh->faces[p * 4 + 0],
                m_retopoMesh->faces[p * 4 + 1],
                m_retopoMesh->faces[p * 4 + 2],
                m_retopoMesh->faces[p * 4 + 3]
            };
            int numE = (q[3] != TRI_INDEX) ? 4 : 3;
            for (int e = 0; e < numE; ++e) {
                int32_t u = (int32_t)q[e];
                int32_t v = (int32_t)q[(e + 1) % numE];
                if (u > v) std::swap(u, v);
                int count = 0;
                for (int32_t p2 = 0; p2 < polyCount; ++p2) {
                    if (PolygonHasEdge(m_retopoMesh->faces, p2, u, v)) count++;
                }
                if (count == 1) {
                    glm::vec3 eMid = (getVertWorld(u) + getVertWorld(v)) * 0.5f;
                    float d = glm::distance(eMid, strokeMid);
                    if (d < bestDist) {
                        bestDist = d;
                        bestV0 = u;
                        bestV1 = v;
                    }
                }
            }
        }

        if (bestV0 != -1 && bestV1 != -1) {
            BorderChain bc = m_builder.FindBorderChain(m_retopoMesh, bestV0, bestV1);
            if (!bc.edges.empty()) {
                for (const auto& edge : bc.edges) {
                    selectEdge(edge.first, edge.second);
                }
            }
        }
    }

    if (m_selectedEdges.empty()) {
        sculpt_log("[QuadDrawTool] Strip extrude: No border edges selected!\n");
        return;
    }

    std::vector<BorderChain> chains = m_builder.ExtractBorderChains(m_retopoMesh, m_selectedEdges);
    if (chains.empty()) return;

    auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
        glm::vec3 p(m_retopoMesh->verts[idx * 3], m_retopoMesh->verts[idx * 3 + 1], m_retopoMesh->verts[idx * 3 + 2]);
        return (m_retopoMesh->matrix != glm::mat4(1.0f)) ? glm::vec3(m_retopoMesh->matrix * glm::vec4(p, 1.0f)) : p;
    };

    int bestChainIdx = 0;
    float bestChainDist = 1e9f;
    for (size_t c = 0; c < chains.size(); ++c) {
        float minDist = 1e9f;
        for (int32_t v : chains[c].vertices) {
            glm::vec3 vPos = getVertWorld(v);
            for (const auto& sp : m_strokeWorldPoints) {
                minDist = std::min(minDist, glm::distance(vPos, sp));
            }
        }
        if (minDist < bestChainDist) {
            bestChainDist = minDist;
            bestChainIdx = (int)c;
        }
    }

    const BorderChain& activeChain = chains[bestChainIdx];

    g_undoManager.pushTopologyChange(scene, "Strip Extrude to Stroke", [&]() {
        StrokeExtrudeResult res = m_builder.ExtrudeEdgesToStroke(
            m_retopoMesh, m_targetMesh, m_snapper, activeChain,
            m_strokeWorldPoints, m_weldRadiusPixels, &camera
        );
        if (res.valid) {
            std::unordered_set<uint64_t> chainEdgeKeys;
            auto makeKey = [](int32_t a, int32_t b) -> uint64_t {
                uint32_t u = (uint32_t)std::min(a, b);
                uint32_t v = (uint32_t)std::max(a, b);
                return ((uint64_t)u << 32) | (uint64_t)v;
            };
            for (const auto& e : activeChain.edges) {
                chainEdgeKeys.insert(makeKey(e.first, e.second));
            }

            std::vector<std::pair<int32_t, int32_t>> updatedSelected;
            for (const auto& e : m_selectedEdges) {
                if (!chainEdgeKeys.count(makeKey(e.first, e.second))) {
                    updatedSelected.push_back(e);
                }
            }
            for (const auto& fe : res.newFrontEdges) {
                updatedSelected.push_back(fe);
            }
            m_selectedEdges = std::move(updatedSelected);
            m_builder.InvalidateEdgeCache();
        }
    });
}

void QuadDrawTool::setTubeCurveRadialSegments(int s) {
    m_tubeCurveRadialSegments = std::clamp(s, 4, 64);
    if (m_tubeCurvePoints.size() >= 2) {
        recalculateTubeCurvePreview();
    }
}

void QuadDrawTool::setTubeCurveSpans(int s) {
    m_tubeCurveSpans = std::clamp(s, 1, 64);
    if (m_tubeCurvePoints.size() >= 2) {
        recalculateTubeCurvePreview();
    }
}

void QuadDrawTool::recalculateTubeCurvePreview() {
    if (!m_targetMesh || m_tubeCurvePoints.size() < 2) {
        m_tubeCurveGeomPreview.valid = false;
        return;
    }

    m_tubeCurveGeomPreview = m_builder.GenerateTubeFromPoints(
        m_targetMesh, m_snapper,
        m_tubeCurvePoints,
        m_tubeCurveRadialSegments, m_tubeCurveSpans
    );
}

void QuadDrawTool::extrudeTubeCurveStart() {
    if (m_tubeCurvePoints.empty()) return;
    glm::vec3 newPt = m_tubeCurvePoints[0];
    if (m_tubeCurvePoints.size() >= 2) {
        glm::vec3 dir = m_tubeCurvePoints[0] - m_tubeCurvePoints[1];
        newPt = m_tubeCurvePoints[0] + dir;
    } else {
        newPt += glm::vec3(0.0f, 1.0f, 0.0f);
    }
    SnapResult snap = m_snapper.SnapClosestPoint(m_targetMesh, newPt, 500.0f);
    if (snap.valid) newPt = snap.worldPos;
    m_tubeCurvePoints.insert(m_tubeCurvePoints.begin(), newPt);
    if (m_tubeCurvePoints.size() >= 2) {
        recalculateTubeCurvePreview();
    }
}

void QuadDrawTool::extrudeTubeCurveEnd() {
    if (m_tubeCurvePoints.empty()) return;
    glm::vec3 newPt = m_tubeCurvePoints.back();
    if (m_tubeCurvePoints.size() >= 2) {
        glm::vec3 dir = m_tubeCurvePoints.back() - m_tubeCurvePoints[m_tubeCurvePoints.size() - 2];
        newPt = m_tubeCurvePoints.back() + dir;
    } else {
        newPt += glm::vec3(0.0f, 1.0f, 0.0f);
    }
    SnapResult snap = m_snapper.SnapClosestPoint(m_targetMesh, newPt, 500.0f);
    if (snap.valid) newPt = snap.worldPos;
    m_tubeCurvePoints.push_back(newPt);
    if (m_tubeCurvePoints.size() >= 2) {
        recalculateTubeCurvePreview();
    }
}

void QuadDrawTool::removeLastTubeCurvePoint() {
    if (!m_tubeCurvePoints.empty()) {
        m_tubeCurvePoints.pop_back();
        if (m_tubeCurvePoints.size() >= 2) {
            recalculateTubeCurvePreview();
        } else {
            m_tubeCurveGeomPreview.valid = false;
        }
    }
}

void QuadDrawTool::commitCurrentTubeCurve(Scene& scene) {
    if (!m_tubeCurveGeomPreview.valid || m_tubeCurvePoints.size() < 2) return;
    ensureTargetAndRetopo(scene, true);
    if (!m_retopoMesh) return;

    g_undoManager.pushTopologyChange(scene, "Tube Curve Retopo Commit", [&]() {
        TubeBuildResult bRes = m_builder.CommitTubeMesh(
            m_retopoMesh, m_targetMesh, m_snapper, m_tubeCurveGeomPreview
        );
        if (bRes.valid) {
            sculpt_log("[QuadDrawTool] Committed Tube Curve Retopo mesh: %d quads created.\n", bRes.quadsCreated);
        }
    });

    resetTubeCurve();
}

void QuadDrawTool::resetTubeCurve() {
    m_tubeCurvePoints.clear();
    m_tubeCurveGeomPreview.valid = false;
    m_tubeCurveHoveredPointIdx = -1;
    m_tubeCurveDraggingPointIdx = -1;
    m_tubeCurveExtrudeDrag = false;
    m_tubeCurveWeldTargetPointIdx = -1;
    if (m_dragMode == DragMode::TubeCurveDrag) {
        m_dragMode = DragMode::None;
    }
}

} // namespace retopo
