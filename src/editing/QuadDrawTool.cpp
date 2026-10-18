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
    m_multiCutPoints.clear();
    m_grabbedVerts.clear();
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
    } else {
        m_pinnedVertices.clear();
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
    }
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
    m_multiCutPoints.clear();
    m_grabbedVerts.clear();
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

void QuadDrawTool::updateHoverState(
    const Camera& camera, float mouseX, float mouseY, bool ctrl, bool alt, bool shift
) {
    m_hoverSnap = m_snapper.RaycastSurface(camera, m_targetMesh, mouseX, mouseY);
    m_hoverVertex = -1;
    m_hoverEdge.valid = false;
    m_hoverQuad.valid = false;
    m_hoverCut.valid = false;
    m_hoverEdgeLoop.edges.clear();
    m_hoverPolyLoop.clear();
    m_weldTargetVertex = -1;
    m_extrudeWeldTarget0 = -1;
    m_extrudeWeldTarget1 = -1;
    m_extrudeVertexWeldTarget = -1;

    if (!m_retopoMesh || m_retopoMesh->nbVerts == 0) return;

    // Check vertex hover
    m_hoverVertex = m_snapper.FindNearestRetopoVertex(
        camera, m_retopoMesh, mouseX, mouseY, m_weldRadiusPixels, -1, m_targetMesh, &m_hoverSnap
    );

    // Check edge hover
    bool borderOnly = (m_subTool == QuadDrawSubTool::Extrude && !ctrl);
    m_hoverEdge = m_snapper.FindNearestRetopoEdge(
        camera, m_retopoMesh, mouseX, mouseY, 14.0f, m_targetMesh, &m_hoverSnap, borderOnly
    );

    if (m_hoverVertex != -1 && m_hoverEdge.valid) {
        if (m_hoverVertex == m_hoverEdge.v0 || m_hoverVertex == m_hoverEdge.v1) {
            m_hoverEdge.valid = false;
        }
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
    }

    if (ctrl && !shift) {
        if (m_hoverEdge.valid) {
            m_hoverEdgeLoop = m_builder.FindEdgeLoop(m_retopoMesh, m_hoverEdge.v0, m_hoverEdge.v1);
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

    if (button != SDL_BUTTON_LEFT) return false;

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
        if (m_hoverVertex != -1) {
            g_undoManager.pushTopologyChange(scene, "Delete Vertex", [&]() {
                m_builder.DeleteVertex(m_retopoMesh, m_hoverVertex, &m_pinnedVertices);
            });
            m_hoverVertex = -1;
            return true;
        } else if (m_hoverEdge.valid) {
            g_undoManager.pushTopologyChange(scene, "Delete Edge", [&]() {
                m_builder.DeleteEdge(m_retopoMesh, m_hoverEdge.v0, m_hoverEdge.v1, &m_pinnedVertices);
            });
            m_hoverEdge.valid = false;
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
            mcp.worldPos = glm::vec3(
                m_retopoMesh->verts[m_hoverVertex * 3],
                m_retopoMesh->verts[m_hoverVertex * 3 + 1],
                m_retopoMesh->verts[m_hoverVertex * 3 + 2]
            );
        } else if (m_hoverEdge.valid) {
            mcp.type = MultiCutSnapType::Edge;
            mcp.edgeV0 = m_hoverEdge.v0;
            mcp.edgeV1 = m_hoverEdge.v1;
            mcp.edgeT = m_hoverEdge.t;
            mcp.worldPos = glm::mix(m_hoverEdge.worldPos0, m_hoverEdge.worldPos1, m_hoverEdge.t);
        } else if (m_hoverSnap.valid) {
            mcp.type = MultiCutSnapType::Face;
            mcp.worldPos = m_hoverSnap.worldPos;
        } else {
            m_dragMode = DragMode::SliceDrag;
            m_sliceStartScreen = glm::vec2(mouseX, mouseY);
            m_sliceEndScreen = m_sliceStartScreen;
            return true;
        }
        m_multiCutPoints.push_back(mcp);
        return true;
    }

    // 8. Tweak / Extrude Mode
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
                    return true;
                }
            }
        }

        // Tweak Vertex Drag
        m_dragMode = DragMode::TweakDrag;
        m_dragVertex = m_hoverVertex;
        return true;
    }

    if (m_hoverEdge.valid && m_subTool == QuadDrawSubTool::Extrude) {
        // Border Edge Extrude Drag
        m_dragEdgeV0 = m_hoverEdge.v0;
        m_dragEdgeV1 = m_hoverEdge.v1;
        m_dragEdgeInitialPos0 = m_hoverEdge.worldPos0;
        m_dragEdgeInitialPos1 = m_hoverEdge.worldPos1;

        glm::vec3 edgeMid = (m_dragEdgeInitialPos0 + m_dragEdgeInitialPos1) * 0.5f;
        glm::vec3 sMid = camera.project(edgeMid);
        m_dragEdgeScreenZ = (sMid.z > 0.0f && sMid.z < 1.0f) ? sMid.z : 0.95f;
        m_dragStartWorldPos = m_hoverSnap.valid ? m_hoverSnap.worldPos : edgeMid;

        glm::vec3 targetNorm = m_hoverSnap.valid ? m_hoverSnap.normal : glm::vec3(0, 1, 0);

        g_undoManager.pushTopologyChange(scene, "Extrude Border Edge", [&]() {
            ExtrudeEdgeResult res = m_builder.ExtrudeEdge(
                m_retopoMesh, m_dragEdgeV0, m_dragEdgeV1,
                m_dragEdgeInitialPos0, m_dragEdgeInitialPos1, targetNorm
            );
            if (res.valid) {
                m_dragMode = DragMode::ExtrudeDrag;
                m_extrudedV0 = res.newV0;
                m_extrudedV1 = res.newV1;
            } else {
                m_dragMode = DragMode::None;
            }
        });
        if (m_dragMode == DragMode::ExtrudeDrag) {
            return true;
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

    if (m_dragMode == DragMode::TweakDrag && m_retopoMesh && m_dragVertex != -1) {
        SnapResult snap = m_snapper.RaycastSurface(camera, m_targetMesh, mouseX, mouseY);
        glm::vec3 targetPos = snap.valid ? snap.worldPos : camera.unproject(mouseX, mouseY, 0.5f);
        m_builder.SetVertexPosition(m_retopoMesh, m_dragVertex, targetPos);

        // Check weld candidate
        m_weldTargetVertex = m_snapper.FindNearestRetopoVertex(
            camera, m_retopoMesh, mouseX, mouseY, m_weldRadiusPixels, m_dragVertex, m_targetMesh, &snap
        );

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

    if (m_dragMode == DragMode::ExtrudeDrag && m_retopoMesh && m_extrudedV0 != -1 && m_extrudedV1 != -1) {
        SnapResult snap = m_snapper.RaycastSurface(camera, m_targetMesh, mouseX, mouseY);
        glm::vec3 targetPos = snap.valid ? snap.worldPos : camera.unproject(mouseX, mouseY, m_dragEdgeScreenZ);

        glm::vec3 offset = targetPos - m_dragStartWorldPos;

        glm::vec3 newPos0 = m_dragEdgeInitialPos0 + offset;
        glm::vec3 newPos1 = m_dragEdgeInitialPos1 + offset;

        if (m_targetMesh && m_targetMesh->nbFaces > 0) {
            // Snap newPos0 to target surface: camera line-of-sight first, then closest 3D point
            glm::vec3 s0 = camera.project(newPos0);
            SnapResult snap0;
            if (s0.z > 0.0f && s0.z < 1.0f) {
                snap0 = m_snapper.RaycastSurface(camera, m_targetMesh, s0.x, s0.y);
            }
            if (snap0.valid && glm::distance(snap0.worldPos, newPos0) < 100.0f) {
                newPos0 = snap0.worldPos;
            } else {
                SnapResult sc0 = m_snapper.SnapClosestPoint(m_targetMesh, newPos0, 500.0f);
                if (sc0.valid) newPos0 = sc0.worldPos;
            }

            // Snap newPos1 to target surface: camera line-of-sight first, then closest 3D point
            glm::vec3 s1 = camera.project(newPos1);
            SnapResult snap1;
            if (s1.z > 0.0f && s1.z < 1.0f) {
                snap1 = m_snapper.RaycastSurface(camera, m_targetMesh, s1.x, s1.y);
            }
            if (snap1.valid && glm::distance(snap1.worldPos, newPos1) < 100.0f) {
                newPos1 = snap1.worldPos;
            } else {
                SnapResult sc1 = m_snapper.SnapClosestPoint(m_targetMesh, newPos1, 500.0f);
                if (sc1.valid) newPos1 = sc1.worldPos;
            }
        }

        m_builder.SetVertexPosition(m_retopoMesh, m_extrudedV0, newPos0);
        m_builder.SetVertexPosition(m_retopoMesh, m_extrudedV1, newPos1);

        // Check weld candidates
        glm::vec3 sPos0 = camera.project(newPos0);
        glm::vec3 sPos1 = camera.project(newPos1);
        int32_t weld0 = m_snapper.FindNearestRetopoVertex(
            camera, m_retopoMesh, sPos0.x, sPos0.y, m_weldRadiusPixels, m_extrudedV0, m_targetMesh
        );
        if (weld0 == m_dragEdgeV0 || weld0 == m_dragEdgeV1 || weld0 == m_extrudedV1) {
            weld0 = -1;
        }

        int32_t weld1 = m_snapper.FindNearestRetopoVertex(
            camera, m_retopoMesh, sPos1.x, sPos1.y, m_weldRadiusPixels, m_extrudedV1, m_targetMesh
        );
        if (weld1 == m_dragEdgeV0 || weld1 == m_dragEdgeV1 || weld1 == m_extrudedV0) {
            weld1 = -1;
        }

        if (weld0 != -1 && weld0 == weld1) {
            float d0 = (sPos0.x - mouseX) * (sPos0.x - mouseX) + (sPos0.y - mouseY) * (sPos0.y - mouseY);
            float d1 = (sPos1.x - mouseX) * (sPos1.x - mouseX) + (sPos1.y - mouseY) * (sPos1.y - mouseY);
            if (d0 < d1) weld1 = -1; else weld0 = -1;
        }

        m_extrudeWeldTarget0 = weld0;
        m_extrudeWeldTarget1 = weld1;

        auto getVertWorld = [&](int32_t idx) -> glm::vec3 {
            glm::vec3 p(m_retopoMesh->verts[idx * 3], m_retopoMesh->verts[idx * 3 + 1], m_retopoMesh->verts[idx * 3 + 2]);
            return (m_retopoMesh->matrix != glm::mat4(1.0f)) ? glm::vec3(m_retopoMesh->matrix * glm::vec4(p, 1.0f)) : p;
        };

        // Magnetic visual snap when near a weld candidate
        if (m_extrudeWeldTarget0 != -1) {
            newPos0 = getVertWorld(m_extrudeWeldTarget0);
        }
        if (m_extrudeWeldTarget1 != -1) {
            newPos1 = getVertWorld(m_extrudeWeldTarget1);
        }

        m_builder.SetVertexPosition(m_retopoMesh, m_extrudedV0, newPos0);
        m_builder.SetVertexPosition(m_retopoMesh, m_extrudedV1, newPos1);

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

    m_lastMouseX = mouseX;
    m_lastMouseY = mouseY;
    return false;
}

bool QuadDrawTool::handleMouseUp(Scene& scene, const Camera& camera, int button) {
    if (m_dragMode == DragMode::None) return false;

    if (m_dragMode == DragMode::TweakDrag && m_retopoMesh && m_dragVertex != -1) {
        if (m_weldTargetVertex != -1 && m_weldTargetVertex != m_dragVertex) {
            g_undoManager.pushTopologyChange(scene, "Weld Vertices", [&]() {
                m_builder.WeldVertices(m_retopoMesh, m_dragVertex, m_weldTargetVertex, &m_pinnedVertices);
            });
        }
    } else if (m_dragMode == DragMode::ExtrudeDrag && m_retopoMesh && m_extrudedV0 != -1 && m_extrudedV1 != -1) {
        if (m_extrudeWeldTarget0 != -1 || m_extrudeWeldTarget1 != -1) {
            int32_t src0 = (m_extrudeWeldTarget0 != -1) ? m_extrudedV0 : -1;
            int32_t tgt0 = m_extrudeWeldTarget0;
            int32_t src1 = (m_extrudeWeldTarget1 != -1) ? m_extrudedV1 : -1;
            int32_t tgt1 = m_extrudeWeldTarget1;

            g_undoManager.pushTopologyChange(scene, "Weld Extruded Vertex", [&]() {
                m_builder.WeldVerticesPair(m_retopoMesh, src0, tgt0, src1, tgt1, &m_pinnedVertices);
            });
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
    m_grabbedVerts.clear();
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
    if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER) {
        commitMultiCut(scene);
        return true;
    }
    if (keycode == SDLK_BACKSPACE) {
        if (!m_multiCutPoints.empty()) {
            m_multiCutPoints.pop_back();
            return true;
        }
    }
    if (keycode == SDLK_ESCAPE) {
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
    if (m_hoverSnap.valid && (m_subTool == QuadDrawSubTool::Extrude || m_subTool == QuadDrawSubTool::CreatePolygon)) {
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
    if (m_hoverQuad.valid) {
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

            if (i == m_weldTargetVertex || i == m_extrudeWeldTarget0 || i == m_extrudeWeldTarget1 || i == m_extrudeVertexWeldTarget) {
                // Weld Target highlight: Pulsing red
                drawList->AddCircleFilled(p, m_pointSize * 1.6f, IM_COL32(255, 30, 30, 240));
                drawList->AddCircle(p, m_pointSize * 2.2f, IM_COL32(255, 255, 255, 220), 16, 2.0f);
            } else if (i == m_hoverVertex) {
                if (m_subTool == QuadDrawSubTool::Extrude && m_builder.CanExtrudeVertex(m_retopoMesh, i)) {
                    // Extrudable corner vertex: Green highlight ring
                    drawList->AddCircleFilled(p, m_pointSize * 1.5f, IM_COL32(50, 255, 100, 255));
                    drawList->AddCircle(p, m_pointSize * 2.2f, IM_COL32(255, 255, 255, 220), 16, 2.0f);
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

        // Draw Highlighted Border Edge
        if (m_hoverEdge.valid && m_subTool == QuadDrawSubTool::Extrude) {
            glm::vec3 s0 = camera.project(m_hoverEdge.worldPos0);
            glm::vec3 s1 = camera.project(m_hoverEdge.worldPos1);
            if (s0.z > 0.0f && s1.z > 0.0f) {
                drawList->AddLine(
                    ImVec2(s0.x, s0.y), ImVec2(s1.x, s1.y),
                    IM_COL32(255, 230, 40, 255), m_lineWidth * 2.5f
                );
            }
        }

        // Draw Edge Loop Highlight
        if (!m_hoverEdgeLoop.edges.empty()) {
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
        if (m_hoverCut.valid) {
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

    // 3. Draw Multi-Cut Placed Chain
    if (!m_multiCutPoints.empty()) {
        for (size_t i = 0; i < m_multiCutPoints.size(); ++i) {
            glm::vec3 sp = camera.project(m_multiCutPoints[i].worldPos);
            if (sp.z > 0.0f) {
                drawList->AddCircleFilled(ImVec2(sp.x, sp.y), 5.0f, IM_COL32(255, 180, 20, 255));
                if (i + 1 < m_multiCutPoints.size()) {
                    glm::vec3 spNext = camera.project(m_multiCutPoints[i + 1].worldPos);
                    if (spNext.z > 0.0f) {
                        drawList->AddLine(ImVec2(sp.x, sp.y), ImVec2(spNext.x, spNext.y), IM_COL32(255, 180, 20, 255), 2.0f);
                    }
                }
            }
        }
    }

    // 4. Draw Screen Slice Line
    if (m_dragMode == DragMode::SliceDrag) {
        drawList->AddLine(
            ImVec2(m_sliceStartScreen.x, m_sliceStartScreen.y),
            ImVec2(m_sliceEndScreen.x, m_sliceEndScreen.y),
            IM_COL32(255, 70, 70, 255), 2.5f
        );
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
}

} // namespace retopo
