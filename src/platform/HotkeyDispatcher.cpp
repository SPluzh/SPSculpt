#include "platform/HotkeyDispatcher.h"
#include "platform/HotkeyManager.h"
#include "platform/FileDialog.h"
#include "files/FileManager.h"
#include "render/AngleRenderer.h"
#include "common/Logger.h"
#include "editing/TubeTool.h"
#include "editing/QuadDrawTool.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <iostream>

HotkeyDispatcher::HotkeyDispatcher() 
    : m_modalMode(ModalMode::NONE)
    , m_prevBrush(BRUSH_FLATTEN)
    , m_shiftActive(false)
    , m_ctrlActive(false) {}

bool HotkeyDispatcher::processEvent(const SDL_Event& event, SculptManager& sculpt, Scene& scene, GuiManager& gui, AngleRenderer* renderer) {
    if (gui.isRemeshRunning()) {
        if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
            updateModifiers(event, sculpt, scene, renderer);
        }
        return true; // Consume event without acting
    }

    if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureKeyboard) {
            // Exception: Ctrl+Z / Ctrl+Y and Alt+Z / Alt+Shift+Z camera undo/redo allowed when not typing text
            bool isCtrl = (event.key.keysym.mod & KMOD_CTRL) != 0;
            bool isAlt = (event.key.keysym.mod & KMOD_ALT) != 0;
            SDL_Keycode sym = event.key.keysym.sym;

            bool isUndoRedoHotkey = (isCtrl && (sym == SDLK_z || sym == SDLK_y)) ||
                                    (isAlt && sym == SDLK_z);
            bool isDeleteHotkey = (sym == SDLK_DELETE || sym == SDLK_BACKSPACE);

            if ((!isUndoRedoHotkey && !isDeleteHotkey) || io.WantTextInput) {
                return false;
            }
        }

        updateModifiers(event, sculpt, scene, renderer);

        bool ctrlPressed = (event.key.keysym.mod & KMOD_CTRL) != 0;
        bool altPressed = (event.key.keysym.mod & KMOD_ALT) != 0;
        bool shiftPressed = (event.key.keysym.mod & KMOD_SHIFT) != 0;
        
        HKAction action = mapKeyToAction(event.key.keysym, ctrlPressed, altPressed, shiftPressed);
        bool isDown = (event.type == SDL_KEYDOWN);
        return executeAction(action, isDown, sculpt, scene, gui);
    }
    
    if (event.type == SDL_MOUSEMOTION) {
        if (m_modalMode != ModalMode::NONE) {
            int deltaX = event.motion.xrel;
            if (deltaX != 0) {
                switch (m_modalMode) {
                    case ModalMode::RADIUS: {
                        float r = sculpt.getBrushRadius() + deltaX * 1.5f;
                        sculpt.setBrushRadius(std::max(0.5f, std::min(1000.0f, r)));
                        break;
                    }
                    case ModalMode::INTENSITY: {
                        float curI = sculpt.getBrushIntensity();
                        float scaleFactor = 0.005f + curI * 0.005f;
                        float i = curI + deltaX * scaleFactor;
                        sculpt.setBrushIntensity(std::max(0.0f, std::min(10.0f, i)));
                        break;
                    }
                    case ModalMode::FOCAL_SHIFT: {
                        if (sculpt.getBrush() == BRUSH_PAINT) {
                            float h = sculpt.getHardness() + deltaX * 0.005f;
                            sculpt.setHardness(std::max(0.0f, std::min(1.0f, h)));
                        } else {
                            float f = sculpt.getFocalShift() + deltaX * 0.005f;
                            sculpt.setFocalShift(std::max(-1.0f, std::min(1.0f, f)));
                        }
                        break;
                    }
                    case ModalMode::REMESH_RESOLUTION: {
                        int r = gui.getRemeshResolution() + deltaX;
                        gui.setRemeshResolution(std::max(10, std::min(1000, r)));
                        Mesh* selected = scene.getSelected();
                        if (selected) {
                            float step = selected->computeWorldStep(gui.getRemeshResolution());
                            scene.updateVoxelPreview(step, {selected});
                        }
                        break;
                    }
                    case ModalMode::TOPOLOGY_DETAIL: {
                        float d = gui.getDyntopoDetail() + deltaX * 0.5f;
                        gui.setDyntopoDetail(std::max(1.0f, std::min(500.0f, d)));
                        break;
                    }
                    case ModalMode::CAMERA_FOV: {
                        float fov = scene.getCamera().getFov() + deltaX * 0.25f;
                        scene.getCamera().setFov(std::max(10.0f, std::min(200.0f, fov)));
                        break;
                    }
                    default: break;
                }
            }
            return true; // Consume mouse motion in modal parameter edits
        }
    }
    
    return false;
}

static BrushType hkActionToBrushType(HKAction act, BrushType fallback) {
    switch (act) {
        case HKAction::ToolPaint: return BRUSH_PAINT;
        case HKAction::ToolBrush: return BRUSH_BRUSH;
        case HKAction::ToolInflate: return BRUSH_INFLATE;
        case HKAction::ToolTwist: return BRUSH_TWIST;
        case HKAction::ToolVTool: return BRUSH_VTOOL;
        case HKAction::ToolTransform: return BRUSH_TRANSFORM;
        case HKAction::ToolSmooth: return BRUSH_SMOOTH;
        case HKAction::ToolRelax: return BRUSH_RELAX;
        case HKAction::ToolFlatten: return BRUSH_FLATTEN;
        case HKAction::ToolPinch: return BRUSH_PINCH;
        case HKAction::ToolCrease: return BRUSH_CREASE;
        case HKAction::ToolDrag: return BRUSH_DRAG;
        case HKAction::ToolMove: return BRUSH_MOVE;
        case HKAction::ToolClayBuildup: return BRUSH_CLAYBUILDUP;
        case HKAction::ToolDamStandard: return BRUSH_DAMSTANDARD;
        case HKAction::ToolMask: return BRUSH_MASK;
        case HKAction::ToolVisibility: return BRUSH_VISIBILITY;
        case HKAction::ToolPose: return BRUSH_POSE;
        case HKAction::ToolSnakeHook: return BRUSH_SNAKEHOOK;
        case HKAction::ToolTube: return BRUSH_TUBE;
        case HKAction::ToolQuadDraw: return BRUSH_QUAD_DRAW;
        default: return fallback;
    }
}

void HotkeyDispatcher::updateModifiers(const SDL_Event& event, SculptManager& sculpt, Scene& scene, AngleRenderer* renderer) {
    const ModifierBehavior& mb = m_manager ? m_manager->getModifierBehavior() : ModifierBehavior{};
    bool ctrlAltInverts = mb.ctrlAltInvertsBrush;
    BrushType shiftBrush = hkActionToBrushType(mb.shiftHoldAction, BRUSH_SMOOTH);
    BrushType ctrlBrush = hkActionToBrushType(mb.ctrlHoldAction, BRUSH_MASK);
    BrushType ctrlShiftBrush = hkActionToBrushType(mb.ctrlShiftHoldAction, BRUSH_VISIBILITY);

    if (event.type == SDL_KEYDOWN) {
        if (event.key.keysym.sym == SDLK_LALT || event.key.keysym.sym == SDLK_RALT) {
            if (!m_altActive) {
                m_altActive = true;
                if (ctrlAltInverts && m_ctrlActive && !m_shiftActive) {
                    // When Ctrl+Alt inverts sculpt rather than masks (e.g. Maya mode),
                    // restore the active sculpt brush.
                    sculpt.setBrush(m_prevBrush);
                }
            }
        } else if (event.key.keysym.sym == SDLK_LSHIFT || event.key.keysym.sym == SDLK_RSHIFT) {
            if (!m_shiftActive) {
                if (!m_ctrlActive) {
                    m_prevBrush = sculpt.getBrush();
                    sculpt.getSettings(shiftBrush).radius = sculpt.getSettings(m_prevBrush).radius;
                    sculpt.setBrush(shiftBrush);
                } else {
                    sculpt.getSettings(ctrlShiftBrush).radius = sculpt.getSettings(m_prevBrush).radius;
                    sculpt.setBrush(ctrlShiftBrush);
                    Mesh* selected = scene.getSelected();
                    if (selected && renderer) {
                        auto groupIDs = sculpt.getPolyGroupTool().getAllGroupIDs(selected);
                        if (groupIDs.size() > 1) {
                            if (!m_polyGroupsTemporarilyEnabled) {
                                m_prevPolyGroupsState = renderer->getShowPolyGroups();
                                renderer->setShowPolyGroups(true);
                                m_polyGroupsTemporarilyEnabled = true;
                            }
                        }
                    }
                }
                m_shiftActive = true;
            }
        } else if (event.key.keysym.sym == SDLK_LCTRL || event.key.keysym.sym == SDLK_RCTRL) {
            if (!m_ctrlActive) {
                if (!m_shiftActive) {
                    m_prevBrush = sculpt.getBrush();
                    // If Alt is already held and Ctrl+Alt inverts, do not switch to Mask.
                    if (!(ctrlAltInverts && m_altActive)) {
                        sculpt.getSettings(ctrlBrush).radius = sculpt.getSettings(m_prevBrush).radius;
                        sculpt.setBrush(ctrlBrush);
                    }
                } else {
                    sculpt.getSettings(ctrlShiftBrush).radius = sculpt.getSettings(m_prevBrush).radius;
                    sculpt.setBrush(ctrlShiftBrush);
                    Mesh* selected = scene.getSelected();
                    if (selected && renderer) {
                        auto groupIDs = sculpt.getPolyGroupTool().getAllGroupIDs(selected);
                        if (groupIDs.size() > 1) {
                            if (!m_polyGroupsTemporarilyEnabled) {
                                m_prevPolyGroupsState = renderer->getShowPolyGroups();
                                renderer->setShowPolyGroups(true);
                                m_polyGroupsTemporarilyEnabled = true;
                            }
                        }
                    }
                }
                m_ctrlActive = true;
            }
        }
    } else if (event.type == SDL_KEYUP) {
        if (event.key.keysym.sym == SDLK_LALT || event.key.keysym.sym == SDLK_RALT) {
            if (m_altActive) {
                m_altActive = false;
                if (ctrlAltInverts && m_ctrlActive && !m_shiftActive) {
                    // Releasing Alt while Ctrl is held returns to Masking
                    sculpt.getSettings(ctrlBrush).radius = sculpt.getSettings(m_prevBrush).radius;
                    sculpt.setBrush(ctrlBrush);
                }
            }
        } else if (event.key.keysym.sym == SDLK_LSHIFT || event.key.keysym.sym == SDLK_RSHIFT) {
            if (m_shiftActive) {
                m_shiftActive = false;
                if (m_ctrlActive) {
                    if (ctrlAltInverts && m_altActive) {
                        sculpt.setBrush(m_prevBrush);
                    } else {
                        sculpt.getSettings(ctrlBrush).radius = sculpt.getSettings(m_prevBrush).radius;
                        sculpt.setBrush(ctrlBrush);
                    }
                } else {
                    sculpt.setBrush(m_prevBrush);
                }
            }
        } else if (event.key.keysym.sym == SDLK_LCTRL || event.key.keysym.sym == SDLK_RCTRL) {
            if (m_ctrlActive) {
                m_ctrlActive = false;
                if (m_shiftActive) {
                    sculpt.getSettings(shiftBrush).radius = sculpt.getSettings(m_prevBrush).radius;
                    sculpt.setBrush(shiftBrush);
                } else {
                    sculpt.setBrush(m_prevBrush);
                }
            }
        }
    }

    if (!(m_shiftActive && m_ctrlActive) && m_polyGroupsTemporarilyEnabled) {
        if (renderer) {
            renderer->setShowPolyGroups(m_prevPolyGroupsState);
        }
        m_polyGroupsTemporarilyEnabled = false;
    }
}

void HotkeyDispatcher::resetModifiers(SculptManager& sculpt, AngleRenderer* renderer) {
    if (m_ctrlActive || m_shiftActive || m_altActive) {
        m_ctrlActive = false;
        m_shiftActive = false;
        m_altActive = false;
        sculpt.setBrush(m_prevBrush);
    }
    if (m_polyGroupsTemporarilyEnabled) {
        if (renderer) {
            renderer->setShowPolyGroups(m_prevPolyGroupsState);
        }
        m_polyGroupsTemporarilyEnabled = false;
    }
}

HKAction HotkeyDispatcher::mapKeyToAction(const SDL_Keysym& keysym, bool ctrlPressed, bool altPressed, bool shiftPressed) {
    if (m_manager) {
        return m_manager->lookupKeyAction(keysym.sym, ctrlPressed, altPressed, shiftPressed);
    }
    static HotkeyPreset s_fallbackPreset = HotkeyPreset::createZBrush();
    for (const auto& [action, binding] : s_fallbackPreset.keyBindings) {
        if (binding.matches(keysym.sym, ctrlPressed, altPressed, shiftPressed)) {
            return action;
        }
    }
    return HKAction::None;
}

bool HotkeyDispatcher::executeAction(HKAction action, bool isDown, SculptManager& sculpt, Scene& scene, GuiManager& gui) {
    if (action == HKAction::None) return false;
    
    if (isDown) {
        switch (action) {
            case HKAction::ToolBrush: sculpt.setBrush(BRUSH_BRUSH); break;
            case HKAction::ToolInflate: sculpt.setBrush(BRUSH_INFLATE); break;
            case HKAction::ToolSmooth: sculpt.setBrush(BRUSH_SMOOTH); break;
            case HKAction::ToolRelax: sculpt.setBrush(BRUSH_RELAX); break;
            case HKAction::ToolFlatten: sculpt.setBrush(BRUSH_FLATTEN); break;
            case HKAction::ToolPinch: sculpt.setBrush(BRUSH_PINCH); break;
            case HKAction::ToolCrease: sculpt.setBrush(BRUSH_CREASE); break;
            case HKAction::ToolDrag: sculpt.setBrush(BRUSH_DRAG); break;
            case HKAction::ToolMove: sculpt.setBrush(BRUSH_MOVE); break;
            case HKAction::ToolClayBuildup: sculpt.setBrush(BRUSH_CLAYBUILDUP); break;
            case HKAction::ToolDamStandard: sculpt.setBrush(BRUSH_DAMSTANDARD); break;
            case HKAction::ToolPaint: sculpt.setBrush(BRUSH_PAINT); break;
            case HKAction::ToolTwist: sculpt.setBrush(BRUSH_TWIST); break;
            case HKAction::ToolVTool: sculpt.setBrush(BRUSH_VTOOL); break;
            case HKAction::ToolTransform: sculpt.setBrush(BRUSH_TRANSFORM); break;
            case HKAction::ToolPose: sculpt.setBrush(BRUSH_POSE); break;
            case HKAction::ToolSnakeHook: sculpt.setBrush(BRUSH_SNAKEHOOK); break;
            case HKAction::ToolTube: {
                sculpt.setBrush(BRUSH_TUBE);
                gui.setShowSculptingPanel(true);
                if (auto* tube = sculpt.getTubeTool()) {
                    tube->onActivate(scene);
                }
                break;
            }
            case HKAction::ToolQuadDraw: {
                sculpt.setBrush(BRUSH_QUAD_DRAW);
                gui.setShowSculptingPanel(true);
                if (auto* qd = sculpt.getQuadDrawTool()) {
                    qd->onActivate(scene);
                }
                break;
            }
            case HKAction::ToolMask: sculpt.setBrush(BRUSH_MASK); break;
            case HKAction::ToolVisibility: sculpt.setBrush(BRUSH_VISIBILITY); break;
            
            case HKAction::BrushNegative: sculpt.toggleNegative(); break;
            case HKAction::ToggleSymmetry: sculpt.setUseSym(!sculpt.getUseSym()); break;
            case HKAction::BrushPicker: sculpt.getSettings(BRUSH_PAINT).pickColor = !sculpt.getSettings(BRUSH_PAINT).pickColor; break;
            
            case HKAction::BrushIntensity: {
                if (m_modalMode != ModalMode::INTENSITY) {
                    m_modalMode = ModalMode::INTENSITY;
                    int mx, my;
                    SDL_GetMouseState(&mx, &my);
                    gui.setModalMode(m_modalMode, mx, my);
                }
                break;
            }
            case HKAction::BrushRadius: {
                if (m_modalMode != ModalMode::RADIUS) {
                    m_modalMode = ModalMode::RADIUS;
                    int mx, my;
                    SDL_GetMouseState(&mx, &my);
                    gui.setModalMode(m_modalMode, mx, my);
                }
                break;
            }
            case HKAction::BrushFocalShift: {
                if (m_modalMode != ModalMode::FOCAL_SHIFT) {
                    m_modalMode = ModalMode::FOCAL_SHIFT;
                    int mx, my;
                    SDL_GetMouseState(&mx, &my);
                    gui.setModalMode(m_modalMode, mx, my);
                }
                break;
            }
            case HKAction::RemeshResolution: {
                if (m_modalMode != ModalMode::REMESH_RESOLUTION) {
                    m_modalMode = ModalMode::REMESH_RESOLUTION;
                    int mx, my;
                    SDL_GetMouseState(&mx, &my);
                    gui.setModalMode(m_modalMode, mx, my);

                    Mesh* selected = scene.getSelected();
                    if (selected) {
                        float step = selected->computeWorldStep(gui.getRemeshResolution());
                        scene.updateVoxelPreview(step, {selected});
                    }
                }
                break;
            }
            case HKAction::TopologyDetail: {
                if (m_modalMode != ModalMode::TOPOLOGY_DETAIL) {
                    m_modalMode = ModalMode::TOPOLOGY_DETAIL;
                    int mx, my;
                    SDL_GetMouseState(&mx, &my);
                    gui.setModalMode(m_modalMode, mx, my);
                }
                break;
            }
            case HKAction::CameraFov: {
                if (m_modalMode != ModalMode::CAMERA_FOV) {
                    m_modalMode = ModalMode::CAMERA_FOV;
                    int mx, my;
                    SDL_GetMouseState(&mx, &my);
                    gui.setModalMode(m_modalMode, mx, my);
                }
                break;
            }
            
            case HKAction::DeleteSelected: {
                if (sculpt.getSelectedMeasureIndex() >= 0) {
                    sculpt.deleteMeasureSegment(scene, sculpt.getSelectedMeasureIndex());
                    sculpt.setSelectedMeasureIndex(-1);
                } else if (sculpt.getSelectedDividerIndex() >= 0) {
                    sculpt.deleteDividerSegment(scene, sculpt.getSelectedDividerIndex());
                    sculpt.setSelectedDividerIndex(-1);
                } else {
                    scene.deleteSelection();
                }
                break;
            }

            case HKAction::CameraProjection: {
                Camera& camera = scene.getCamera();
                if (camera.isOrthographic()) {
                    camera.setProjectionType(CameraEnums::Projection::PERSPECTIVE);
                } else {
                    camera.setProjectionType(CameraEnums::Projection::ORTHOGRAPHIC);
                }
                break;
            }
            case HKAction::CameraFrame: {
                std::vector<Mesh*> selectedMeshes = scene.getSelectedMeshes();
                if (selectedMeshes.empty() && scene.getSelected()) {
                    selectedMeshes.push_back(scene.getSelected());
                }
                if (selectedMeshes.empty()) {
                    for (Mesh* m : scene.getMeshes()) {
                        if (m && m->isVisible(scene.getActiveViewport())) {
                            selectedMeshes.push_back(m);
                        }
                    }
                }
                if (!selectedMeshes.empty()) {
                    scene.getCamera().resetViewToMeshes(selectedMeshes, gui.getAnimateBookmarks());
                } else {
                    scene.getCamera().resetView();
                }
                break;
            }
            case HKAction::CameraLeft: scene.getCamera().toggleViewLeft(); break;
            case HKAction::CameraReset: scene.getCamera().resetView(); break;
            case HKAction::CameraUndo: scene.getCamera().undo(); break;
            case HKAction::CameraRedo: scene.getCamera().redo(); break;
            
            case HKAction::ApplyBookmark1: scene.applyBookmark(0, gui.getAnimateBookmarks()); break;
            case HKAction::ApplyBookmark2: scene.applyBookmark(1, gui.getAnimateBookmarks()); break;
            case HKAction::ApplyBookmark3: scene.applyBookmark(2, gui.getAnimateBookmarks()); break;
            case HKAction::ApplyBookmark4: scene.applyBookmark(3, gui.getAnimateBookmarks()); break;
            case HKAction::ApplyBookmark5: scene.applyBookmark(4, gui.getAnimateBookmarks()); break;
            case HKAction::ApplyBookmark6: scene.applyBookmark(5, gui.getAnimateBookmarks()); break;
            case HKAction::ApplyBookmark7: scene.applyBookmark(6, gui.getAnimateBookmarks()); break;
            case HKAction::ApplyBookmark8: scene.applyBookmark(7, gui.getAnimateBookmarks()); break;
            case HKAction::ApplyBookmark9: scene.applyBookmark(8, gui.getAnimateBookmarks()); break;
            
            case HKAction::StrifeLeft: scene.getCamera().translate(-10.0f, 0.0f); break;
            case HKAction::StrifeRight: scene.getCamera().translate(10.0f, 0.0f); break;
            case HKAction::StrifeUp: scene.getCamera().translate(0.0f, 10.0f); break;
            case HKAction::StrifeDown: scene.getCamera().translate(0.0f, -10.0f); break;
            
            case HKAction::ClearScene: scene.clear(); break;
            case HKAction::DuplicateSelection: scene.duplicateSelection(); break;
            case HKAction::Undo:
                sculpt_log_lvl(LogLevel::Info, "[Hotkey] Undo action triggered by key shortcut\n");
                scene.undo();
                gui.thumbInvalidateAll();
                break;
            case HKAction::Redo:
                sculpt_log_lvl(LogLevel::Info, "[Hotkey] Redo action triggered by key shortcut\n");
                scene.redo();
                gui.thumbInvalidateAll();
                break;
            
            case HKAction::OpenFile: gui.openScene(scene, &sculpt); break;
            case HKAction::OpenRecentFile: gui.toggleRecentFilesWindow(); break;
            case HKAction::SaveFile: gui.saveScene(scene, &sculpt); break;
            case HKAction::SaveFileAs: gui.saveSceneAs(scene, &sculpt); break;
            case HKAction::ExportOBJ: gui.exportFile(scene, &sculpt); break;
            case HKAction::ToggleDyntopo: gui.toggleTopologyPanel(); break;
            case HKAction::ToggleOutliner: gui.toggleScenePanel(); break;
            case HKAction::ToggleCameraBookmarks: gui.toggleCameraBookmarksPanel(); break;
            case HKAction::OpenContextPopup: gui.m_openContextPopup = true; break;
            case HKAction::RunRemesh: gui.performRemesh(scene); break;
            case HKAction::SoloSelected: scene.toggleSolo(scene.getSelected()); break;
            case HKAction::ToggleRefImagesVisible: {
                scene.toggleAllReferenceImagesVisible();
                if (!scene.areAnyReferenceImagesVisible()) {
                    scene.getCamera().setRefDragEnabled(false);
                    if (scene.getCameraRight()) {
                        scene.getCameraRight()->setRefDragEnabled(false);
                    }
                }
                break;
            }
            case HKAction::ToggleRefEditMode: {
                if (scene.areAnyReferenceImagesVisible()) {
                    Camera& camera = scene.getCamera();
                    bool newMode = !camera.getRefDragEnabled();
                    camera.setRefDragEnabled(newMode);
                    if (scene.getCameraRight()) {
                        scene.getCameraRight()->setRefDragEnabled(newMode);
                    }
                } else {
                    scene.getCamera().setRefDragEnabled(false);
                    if (scene.getCameraRight()) {
                        scene.getCameraRight()->setRefDragEnabled(false);
                    }
                }
                break;
            }
            
            default: break;
        }
    } else {
        switch (action) {
            case HKAction::BrushIntensity:
                if (m_modalMode == ModalMode::INTENSITY) {
                    m_modalMode = ModalMode::NONE;
                    gui.setModalMode(m_modalMode, 0, 0);
                }
                break;
            case HKAction::BrushRadius:
                if (m_modalMode == ModalMode::RADIUS) {
                    m_modalMode = ModalMode::NONE;
                    gui.setModalMode(m_modalMode, 0, 0);
                }
                break;
            case HKAction::BrushFocalShift:
                if (m_modalMode == ModalMode::FOCAL_SHIFT) {
                    m_modalMode = ModalMode::NONE;
                    gui.setModalMode(m_modalMode, 0, 0);
                }
                break;
            case HKAction::RemeshResolution:
                if (m_modalMode == ModalMode::REMESH_RESOLUTION) {
                    m_modalMode = ModalMode::NONE;
                    gui.setModalMode(m_modalMode, 0, 0);
                    scene.updateVoxelPreview(0.0f, {});
                }
                break;
            case HKAction::TopologyDetail:
                if (m_modalMode == ModalMode::TOPOLOGY_DETAIL) {
                    m_modalMode = ModalMode::NONE;
                    gui.setModalMode(m_modalMode, 0, 0);
                }
                break;
            case HKAction::CameraFov:
                if (m_modalMode == ModalMode::CAMERA_FOV) {
                    m_modalMode = ModalMode::NONE;
                    gui.setModalMode(m_modalMode, 0, 0);
                }
                break;
            default: break;
        }
    }
    
    return true;
}
