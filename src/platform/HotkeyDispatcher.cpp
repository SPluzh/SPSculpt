#include "platform/HotkeyDispatcher.h"
#include "platform/FileDialog.h"
#include "files/FileManager.h"
#include "render/AngleRenderer.h"
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
            // Exception: Ctrl+Z / Ctrl+Y always allowed
            bool isCtrl = (event.key.keysym.mod & KMOD_CTRL) != 0;
            if (!isCtrl) return false;
            if (event.key.keysym.sym != SDLK_z && event.key.keysym.sym != SDLK_y)
                return false;
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
                            float bbox[6];
                            selected->computeBbox(bbox);
                            float maxDim = std::max({bbox[3] - bbox[0], bbox[4] - bbox[1], bbox[5] - bbox[2]});
                            float step = maxDim / (float)gui.getRemeshResolution();
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

void HotkeyDispatcher::updateModifiers(const SDL_Event& event, SculptManager& sculpt, Scene& scene, AngleRenderer* renderer) {
    if (event.type == SDL_KEYDOWN) {
        if (event.key.keysym.sym == SDLK_LSHIFT || event.key.keysym.sym == SDLK_RSHIFT) {
            if (!m_shiftActive) {
                if (!m_ctrlActive) {
                    m_prevBrush = sculpt.getBrush();
                    sculpt.getSettings(BRUSH_SMOOTH).radius = sculpt.getSettings(m_prevBrush).radius;
                    sculpt.setBrush(BRUSH_SMOOTH);
                } else {
                    sculpt.getSettings(BRUSH_VISIBILITY).radius = sculpt.getSettings(m_prevBrush).radius;
                    sculpt.setBrush(BRUSH_VISIBILITY);
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
                    sculpt.getSettings(BRUSH_MASK).radius = sculpt.getSettings(m_prevBrush).radius;
                    sculpt.setBrush(BRUSH_MASK);
                } else {
                    sculpt.getSettings(BRUSH_VISIBILITY).radius = sculpt.getSettings(m_prevBrush).radius;
                    sculpt.setBrush(BRUSH_VISIBILITY);
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
        if (event.key.keysym.sym == SDLK_LSHIFT || event.key.keysym.sym == SDLK_RSHIFT) {
            if (m_shiftActive) {
                m_shiftActive = false;
                if (m_ctrlActive) {
                    sculpt.getSettings(BRUSH_MASK).radius = sculpt.getSettings(m_prevBrush).radius;
                    sculpt.setBrush(BRUSH_MASK);
                } else {
                    sculpt.setBrush(m_prevBrush);
                }
            }
        } else if (event.key.keysym.sym == SDLK_LCTRL || event.key.keysym.sym == SDLK_RCTRL) {
            if (m_ctrlActive) {
                m_ctrlActive = false;
                if (m_shiftActive) {
                    sculpt.getSettings(BRUSH_SMOOTH).radius = sculpt.getSettings(m_prevBrush).radius;
                    sculpt.setBrush(BRUSH_SMOOTH);
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
    if (m_ctrlActive || m_shiftActive) {
        m_ctrlActive = false;
        m_shiftActive = false;
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
    SDL_Keycode sym = keysym.sym;
    
    // Check Ctrl combinations
    if (ctrlPressed) {
        if (altPressed && sym == SDLK_n) return HKAction::ClearScene;
        if (sym == SDLK_z) {
            if (shiftPressed) return HKAction::Redo;
            return HKAction::Undo;
        }
        if (sym == SDLK_s) {
            if (shiftPressed) return HKAction::SaveFileAs;
            return HKAction::SaveFile;
        }
        if (sym == SDLK_y) return HKAction::Redo;
        if (sym == SDLK_d) return HKAction::DuplicateSelection;
        if (sym == SDLK_o) return HKAction::OpenFile;
        if (sym == SDLK_e) return HKAction::ExportOBJ;
        if (sym == SDLK_t) return HKAction::ToggleDyntopo;
        if (sym == SDLK_x) return HKAction::RunRemesh;
    }
    
    // Check Alt combinations
    if (altPressed) {
        if (sym == SDLK_x) return HKAction::ToggleSymmetry;
        if (sym == SDLK_z) {
            if (shiftPressed) return HKAction::CameraRedo;
            return HKAction::CameraUndo;
        }
    }
    
    // Normal keys
    switch (sym) {
        case SDLK_0: return HKAction::ToolPaint;
        case SDLK_1: return HKAction::ToolBrush;
        case SDLK_2: return HKAction::ToolInflate;
        case SDLK_3: return HKAction::ToolTwist;
        case SDLK_4: return HKAction::ToolTransform;
        case SDLK_5: return HKAction::ToolSmooth;
        case SDLK_6: return HKAction::ToolFlatten;
        case SDLK_7: return HKAction::ToolPinch;
        case SDLK_8: return HKAction::ToolCrease;
        case SDLK_9: return HKAction::ToolDrag;
        
        case SDLK_q: return HKAction::ToolMove;
        case SDLK_w: return HKAction::ToolClayBuildup;
        case SDLK_e: return HKAction::ToolDamStandard;
        case SDLK_r: return HKAction::ToolPinch;
        case SDLK_t: return HKAction::ToolTopology;
        
        case SDLK_a: return HKAction::BrushIntensity;
        case SDLK_s: return HKAction::BrushRadius;
        case SDLK_d: return HKAction::BrushFocalShift;
        case SDLK_n: return HKAction::BrushNegative;
        case SDLK_i: return HKAction::BrushPicker;
        case SDLK_x: return HKAction::RemeshResolution;
        case SDLK_z: return HKAction::TopologyDetail;
        
        case SDLK_DELETE: return HKAction::DeleteSelected;
        
        case SDLK_g: return HKAction::CameraFov;
        case SDLK_p: return HKAction::CameraProjection;
        case SDLK_f: return HKAction::CameraFrame;
        case SDLK_l: return HKAction::CameraLeft;
        
        case SDLK_LEFT: return HKAction::StrifeLeft;
        case SDLK_RIGHT: return HKAction::StrifeRight;
        case SDLK_UP: return HKAction::StrifeUp;
        case SDLK_DOWN: return HKAction::StrifeDown;
        
        case SDLK_c: return HKAction::SoloSelected;
        case SDLK_F1: return HKAction::OpenContextPopup;
        
        default: break;
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
            case HKAction::ToolFlatten: sculpt.setBrush(BRUSH_FLATTEN); break;
            case HKAction::ToolPinch: sculpt.setBrush(BRUSH_PINCH); break;
            case HKAction::ToolCrease: sculpt.setBrush(BRUSH_CREASE); break;
            case HKAction::ToolDrag: sculpt.setBrush(BRUSH_DRAG); break;
            case HKAction::ToolMove: sculpt.setBrush(BRUSH_MOVE); break;
            case HKAction::ToolClayBuildup: sculpt.setBrush(BRUSH_CLAYBUILDUP); break;
            case HKAction::ToolDamStandard: sculpt.setBrush(BRUSH_DAMSTANDARD); break;
            case HKAction::ToolPaint: sculpt.setBrush(BRUSH_PAINT); break;
            case HKAction::ToolTwist: sculpt.setBrush(BRUSH_TWIST); break;
            case HKAction::ToolTransform: sculpt.setBrush(BRUSH_TRANSFORM); break;
            
            case HKAction::BrushNegative: sculpt.toggleNegative(); break;
            case HKAction::ToggleSymmetry: sculpt.setUseSym(!sculpt.getUseSym()); break;
            
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
                        float bbox[6];
                        selected->computeBbox(bbox);
                        float maxDim = std::max({bbox[3] - bbox[0], bbox[4] - bbox[1], bbox[5] - bbox[2]});
                        float step = maxDim / (float)gui.getRemeshResolution();
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
                Mesh* selected = scene.getSelected();
                if (selected) {
                    scene.removeMesh(selected);
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
                    scene.getCamera().resetViewToMeshes(selectedMeshes);
                } else {
                    scene.getCamera().resetView();
                }
                break;
            }
            case HKAction::CameraLeft: scene.getCamera().toggleViewLeft(); break;
            case HKAction::CameraReset: scene.getCamera().resetView(); break;
            case HKAction::CameraUndo: scene.getCamera().undo(); break;
            case HKAction::CameraRedo: scene.getCamera().redo(); break;
            
            case HKAction::StrifeLeft: scene.getCamera().translate(-10.0f, 0.0f); break;
            case HKAction::StrifeRight: scene.getCamera().translate(10.0f, 0.0f); break;
            case HKAction::StrifeUp: scene.getCamera().translate(0.0f, 10.0f); break;
            case HKAction::StrifeDown: scene.getCamera().translate(0.0f, -10.0f); break;
            
            case HKAction::ClearScene: scene.clear(); break;
            case HKAction::Undo: scene.undo(); break;
            case HKAction::Redo: scene.redo(); break;
            
            case HKAction::OpenFile: gui.openScene(scene, &sculpt); break;
            case HKAction::SaveFile: gui.saveScene(scene, &sculpt); break;
            case HKAction::SaveFileAs: gui.saveSceneAs(scene, &sculpt); break;
            case HKAction::ExportOBJ: gui.exportFile(scene, &sculpt); break;
            case HKAction::ToggleDyntopo: gui.toggleTopologyPanel(); break;
            case HKAction::OpenContextPopup: gui.m_openContextPopup = true; break;
            case HKAction::RunRemesh: gui.performRemesh(scene); break;
            case HKAction::SoloSelected: scene.toggleSolo(scene.getSelected()); break;
            
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
