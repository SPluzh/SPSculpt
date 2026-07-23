#include "gui/GuiManager.h"
#include "render/AngleRenderer.h"
#include "render/RenderSettings.h"
#include "editing/BrushCursor.h"
#include "files/FileManager.h"
#include "brushes/BrushPresetManager.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#include <ImGuizmo.h>
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <cstdio>
#include <algorithm>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "mesh/Topology.h"
#include "sculpt/Remesh.h"
#include "files/MeshUtils.h"
#include "../third_party/stb_image.h"

static void exportOBJ(const Mesh& mesh, const std::string& path) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        std::cerr << "Failed to open " << path << " for writing" << std::endl;
        return;
    }
    for (int i = 0; i < mesh.nbVerts; ++i) {
        fprintf(f, "v %f %f %f\n", mesh.verts[i*3], mesh.verts[i*3+1], mesh.verts[i*3+2]);
        fprintf(f, "vn %f %f %f\n", mesh.normals[i*3], mesh.normals[i*3+1], mesh.normals[i*3+2]);
    }
    for (int i = 0; i < mesh.nbFaces; ++i) {
        if (mesh.faces[i*4+3] == 0xffffffff) {
            fprintf(f, "f %d//%d %d//%d %d//%d\n", mesh.faces[i*4]+1, mesh.faces[i*4]+1, mesh.faces[i*4+1]+1, mesh.faces[i*4+1]+1, mesh.faces[i*4+2]+1, mesh.faces[i*4+2]+1);
        } else {
            fprintf(f, "f %d//%d %d//%d %d//%d %d//%d\n", mesh.faces[i*4]+1, mesh.faces[i*4]+1, mesh.faces[i*4+1]+1, mesh.faces[i*4+1]+1, mesh.faces[i*4+2]+1, mesh.faces[i*4+2]+1, mesh.faces[i*4+3]+1, mesh.faces[i*4+3]+1);
        }
    }
    fclose(f);
}

static bool importOBJ(Mesh& mesh, const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        std::cerr << "Failed to open " << path << " for reading" << std::endl;
        return false;
    }
    std::vector<float> verts;
    std::vector<uint32_t> faces;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'v' && line[1] == ' ') {
            float x, y, z;
            if (sscanf(line + 2, "%f %f %f", &x, &y, &z) == 3) {
                verts.push_back(x);
                verts.push_back(y);
                verts.push_back(z);
            }
        } else if (line[0] == 'f' && line[1] == ' ') {
            int v0, v1, v2, v3 = -1;
            int matched = sscanf(line + 2, "%d/%*d/%*d %d/%*d/%*d %d/%*d/%*d %d/%*d/%*d", &v0, &v1, &v2, &v3);
            if (matched < 3) {
                matched = sscanf(line + 2, "%d//%*d %d//%*d %d//%*d %d//%*d", &v0, &v1, &v2, &v3);
            }
            if (matched < 3) {
                matched = sscanf(line + 2, "%d %d %d %d", &v0, &v1, &v2, &v3);
            }
            if (matched >= 3) {
                faces.push_back(v0 - 1);
                faces.push_back(v1 - 1);
                faces.push_back(v2 - 1);
                faces.push_back(matched == 4 ? v3 - 1 : 0xffffffff);
            }
        }
    }
    fclose(f);
    if (verts.empty()) return false;
    
    mesh.verts = verts;
    mesh.faces = faces;
    mesh.nbVerts = verts.size() / 3;
    mesh.nbFaces = faces.size() / 4;
    
    std::vector<uint32_t> vrvStartCount;
    std::vector<uint32_t> vertRingVert;
    std::vector<uint32_t> vrfStartCount;
    std::vector<uint32_t> vertRingFace;
    std::vector<uint8_t> vertOnEdge;
    computeTopology(mesh.nbVerts, mesh.faces.data(), mesh.nbFaces, vrfStartCount, vertRingFace, vrvStartCount, vertRingVert, vertOnEdge);
    
    mesh.vrfStartCount = vrfStartCount;
    mesh.vertRingFace = vertRingFace;
    mesh.vrvStartCount = vrvStartCount;
    mesh.vertRingVert = vertRingVert;
    mesh.vertOnEdge = vertOnEdge;
    mesh.postInit();
    return true;
}

static std::string formatCount(int n) {
    if (n >= 1000000) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1fM", n / 1000000.0f);
        return buf;
    }
    if (n >= 1000) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1fK", n / 1000.0f);
        return buf;
    }
    return std::to_string(n);
}

static bool isPointInPolygon(ImVec2 p, const ImVec2* poly, int count) {
    auto side = [](ImVec2 p1, ImVec2 p2, ImVec2 p) {
        return (p2.x - p1.x) * (p.y - p1.y) - (p2.y - p1.y) * (p.x - p1.x);
    };
    bool allPos = true;
    bool allNeg = true;
    for (int i = 0; i < count; ++i) {
        float s = side(poly[i], poly[(i + 1) % count], p);
        if (s > 0) allNeg = false;
        if (s < 0) allPos = false;
    }
    return allPos || allNeg;
}

// Helper to get brush name
static const char* getBrushNameLocal(BrushType brush) {
    switch (brush) {
        case BRUSH_FLATTEN: return "Flatten";
        case BRUSH_SMOOTH:   return "Smooth";
        case BRUSH_INFLATE:  return "Inflate";
        case BRUSH_PINCH:    return "Pinch";
        case BRUSH_CREASE:   return "Crease";
        case BRUSH_VTOOL:    return "V-Tool";
        case BRUSH_MOVE:     return "Move";
        case BRUSH_DRAG:     return "Drag";
        case BRUSH_ELASTIC:  return "Elastic";
        case BRUSH_MASK:     return "Mask";
        case BRUSH_PAINT:    return "Paint";
        case BRUSH_TWIST:    return "Twist";
        case BRUSH_LOCALSCALE: return "Local Scale";
        case BRUSH_CLAY:     return "Clay";
        case BRUSH_CLAYBUILDUP: return "Clay Buildup";
        case BRUSH_DAMSTANDARD: return "Dam Standard";
        case BRUSH_SQUAREBRUSH: return "Square Brush";
        case BRUSH_VISIBILITY: return "Visibility";
        case BRUSH_MASK_GRADIENT_BLUR: return "Mask Gradient Blur";
        case BRUSH_MEASURE:  return "Measure";
        case BRUSH_DIVIDER:  return "Divider";
        case BRUSH_TRANSFORM: return "Transform";
    }
    return "Unknown";
}

GuiManager::GuiManager() {}

GuiManager::~GuiManager() {
    shutdown();
}

void GuiManager::init(SDL_Window* window, SDL_GLContext glContext) {
    if (m_imguiInitialized) return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    
    // Sleek premium dark styling with teal accent
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    
    ImVec4 tealAccent = ImVec4(0.01f, 0.52f, 0.45f, 1.00f);
    ImVec4 tealAccentHover = ImVec4(0.02f, 0.65f, 0.54f, 1.00f);
    ImVec4 tealAccentActive = ImVec4(0.00f, 0.39f, 0.30f, 1.00f);
    
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.10f, 0.95f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.18f, 0.20f, 0.22f, 0.60f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.01f, 0.52f, 0.45f, 0.35f);
    style.Colors[ImGuiCol_HeaderHovered] = tealAccentHover;
    style.Colors[ImGuiCol_HeaderActive] = tealAccentActive;
    style.Colors[ImGuiCol_Button] = ImVec4(0.16f, 0.17f, 0.19f, 1.00f);
    style.Colors[ImGuiCol_ButtonHovered] = tealAccentHover;
    style.Colors[ImGuiCol_ButtonActive] = tealAccentActive;
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.15f, 0.16f, 1.00f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.20f, 0.22f, 1.00f);
    style.Colors[ImGuiCol_FrameBgActive] = tealAccentActive;
    style.Colors[ImGuiCol_CheckMark] = tealAccent;
    style.Colors[ImGuiCol_SliderGrab] = tealAccent;
    style.Colors[ImGuiCol_SliderGrabActive] = tealAccentHover;
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.07f, 0.08f, 1.00f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.09f, 0.10f, 1.00f);
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.08f, 0.09f, 0.10f, 0.60f);
    style.Colors[ImGuiCol_PlotHistogram] = tealAccent;
    style.Colors[ImGuiCol_PlotHistogramHovered] = tealAccentHover;

    ImGui_ImplSDL2_InitForOpenGL(window, glContext);
    ImGui_ImplOpenGL3_Init(nullptr);

    m_imguiInitialized = true;
}

void GuiManager::shutdown() {
    if (!m_imguiInitialized) return;

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    m_imguiInitialized = false;
}

static bool isPointOverImGuiWindow(const ImVec2& pt) {
    ImGuiContext& g = *GImGui;
    for (int i = 0; i < g.Windows.Size; i++) {
        ImGuiWindow* window = g.Windows[i];
        if (window->Active && window->WasActive && !window->Hidden) {
            if (!(window->Flags & ImGuiWindowFlags_NoMouseInputs)) {
                ImRect rect = window->Rect();
                if (rect.Contains(pt)) {
                    return true;
                }
            }
        }
    }
    return false;
}

void GuiManager::render(SculptManager& sculpt, Scene& scene, AngleRenderer& renderer, SDL_Window* window) {
    m_renderer = &renderer;
    if (!m_imguiInitialized) return;

    if (m_remeshAsync.state == RemeshState::Done) {
        applyRemeshResult(scene, m_remeshAsync.result);
        m_remeshAsync.result = RemeshResult(); // Free memory
        m_remeshAsync.state = RemeshState::Idle;
    } else if (m_remeshAsync.state == RemeshState::Error) {
        m_remeshAsync.state = RemeshState::Idle;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // 1. Main Menu Bar
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Load Default Sphere")) {
                scene.loadDefaultSphere();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Import File...")) {
                m_showFilesPanel = true;
            }
            if (ImGui::MenuItem("Export File...")) {
                m_showFilesPanel = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save Render Settings")) {
                RenderSettings::save("render_settings.cfg", renderer, scene);
            }
            if (ImGui::MenuItem("Load Render Settings")) {
                RenderSettings::load("render_settings.cfg", renderer, scene);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save GUI Settings")) {
                saveSettings("gui_settings.cfg");
            }
            if (ImGui::MenuItem("Load GUI Settings")) {
                loadSettings("gui_settings.cfg");
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Panels")) {
            ImGui::MenuItem("Toolbar", nullptr, &m_showToolbar);
            ImGui::MenuItem("Sculpting Settings", nullptr, &m_showSculptingPanel);
            ImGui::MenuItem("Scene Outliner", nullptr, &m_showScenePanel);
            ImGui::MenuItem("Topology & Remesh", nullptr, &m_showTopologyPanel);
            ImGui::MenuItem("Import & Export", nullptr, &m_showFilesPanel);
            ImGui::MenuItem("Camera & Viewport", nullptr, &m_showCameraPanel);
            ImGui::MenuItem("Rendering Quality", nullptr, &m_showRenderingPanel);
            ImGui::MenuItem("Reference Images", nullptr, &m_showReferenceImagesPanel);
            ImGui::MenuItem("Navigation Cube", nullptr, &m_showGizmoCube);
            ImGui::MenuItem("Mesh Statistics & FPS", nullptr, &m_showMeshInfo);
#ifdef _WIN32
            ImGui::MenuItem("Tablet Diagnostics", nullptr, &m_showTabletDiagPanel);
#endif
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // 2. Vertical Toolbar on the left
    if (m_showToolbar) {
        ImGui::SetNextWindowPos({10, 40}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(160.0f, -1.0f), ImVec2(160.0f, -1.0f));
        ImGui::Begin("Toolbar", &m_showToolbar, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
        
        const char* tools[] = { 
            "Flatten", "Smooth", "Inflate", "Pinch", "Crease", "V-Tool", "Move", "Drag", "Elastic", 
            "Mask", "Paint", "Twist", "Local Scale", "Clay", "Clay Buildup", "Dam Standard", "Square Brush", "Visibility", "Mask Gradient Blur",
            "Measure", "Divider", "Transform"
        };
        BrushType current = sculpt.getBrush();
        for (int i = 0; i < 22; i++) {
            bool selected = (current == (BrushType)i);
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_HeaderActive]);
            }
            if (ImGui::Button(tools[i], ImVec2(-1, 26))) {
                sculpt.setBrush((BrushType)i);
            }
            if (selected) {
                ImGui::PopStyleColor();
            }
        }
        ImGui::End();
    }

    // 3. Sculpting settings panel
    if (m_showSculptingPanel) {
        ImGui::SetNextWindowPos({160, 40}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({300, 400}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Sculpting Settings", &m_showSculptingPanel, ImGuiWindowFlags_AlwaysAutoResize);

        BrushSettings& settings = sculpt.getCurrentSettings();
        BrushType brushType = sculpt.getBrush();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.9f, 1.0f, 1.0f));
        ImGui::Text("Active Brush: %s", getBrushNameLocal(brushType));
        ImGui::PopStyleColor();
        ImGui::Separator();

        // ZBrush Presets Section
        if (ImGui::CollapsingHeader("ZBrush Brush Presets", ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto& presets = BrushPresetManager::instance().presets();
            const BrushPreset* activePreset = BrushPresetManager::instance().active();
            std::string activeName = activePreset ? activePreset->name : "None (Custom)";

            if (ImGui::BeginCombo("Select Preset", activeName.c_str())) {
                for (const auto& preset : presets) {
                    bool isSelected = (activePreset && activePreset->uid == preset.uid);
                    if (ImGui::Selectable(preset.name.c_str(), isSelected)) {
                        BrushPresetManager::instance().setActive(preset.uid);
                        sculpt.applyActivePreset();
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            if (activePreset) {
                ImGui::Indent();
                ImGui::Text("Deform Mode: %s", activePreset->deformMode == DeformMode::Normal ? "Normal" :
                                            activePreset->deformMode == DeformMode::Clay ? "Clay" :
                                            activePreset->deformMode == DeformMode::Inflate ? "Inflate" :
                                            activePreset->deformMode == DeformMode::Pinch ? "Pinch" :
                                            activePreset->deformMode == DeformMode::Crease ? "Crease" :
                                            activePreset->deformMode == DeformMode::Flatten ? "Flatten" :
                                            activePreset->deformMode == DeformMode::Smooth ? "Smooth" :
                                            activePreset->deformMode == DeformMode::Move ? "Move" : "Unknown");
                ImGui::Text("Stroke Mode: %s", activePreset->strokeMode == StrokeMode::Dot ? "Dot" :
                                            activePreset->strokeMode == StrokeMode::Roll ? "Roll" :
                                            activePreset->strokeMode == StrokeMode::Grab ? "Grab" :
                                            activePreset->strokeMode == StrokeMode::GrabDynamicRadius ? "Grab (Dynamic Radius)" : "Unknown");
                ImGui::Text("Depth Filter: %s", activePreset->depthFilter.enable ? "Enabled" : "Disabled");
                if (activePreset->depthFilter.enable) {
                    ImGui::Text("  Range: [%.2f, %.2f], Offset: %.2f", activePreset->depthFilter.min, activePreset->depthFilter.max, activePreset->depthFilter.offset);
                }
                if (activePreset->smoothTaubin) {
                    ImGui::Text("Taubin Smoothing: Enabled");
                    ImGui::Text("  Shrink: %.2f, Inflate: %.2f", activePreset->smoothTaubinShrink, activePreset->smoothTaubinInflate);
                }
                if (activePreset->grabRadius) {
                    ImGui::Text("Grab Radius Scale: %.2f", activePreset->grabRadiusScale);
                }
                ImGui::Unindent();
            }

            if (ImGui::Button("Load Custom Preset (.json)...", ImVec2(-1, 0))) {
                ImGui::OpenPopup("Load Preset Path Dialog");
            }

            if (ImGui::BeginPopupModal("Load Preset Path Dialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                static char presetPathBuf[256] = "";
                ImGui::Text("Enter absolute or relative path to brush preset JSON file:");
                ImGui::InputText("Path", presetPathBuf, sizeof(presetPathBuf));
                
                if (ImGui::Button("Load", ImVec2(120, 0))) {
                    if (std::strlen(presetPathBuf) > 0) {
                        if (BrushPresetManager::instance().loadFromFile(presetPathBuf)) {
                            std::string pathStr(presetPathBuf);
                            size_t lastSlash = pathStr.find_last_of("/\\");
                            std::string filename = (lastSlash == std::string::npos) ? pathStr : pathStr.substr(lastSlash + 1);
                            size_t lastDot = filename.find_last_of('.');
                            std::string name = (lastDot == std::string::npos) ? filename : filename.substr(0, lastDot);
                            const auto* loadedPreset = BrushPresetManager::instance().findByName(name);
                            if (loadedPreset) {
                                BrushPresetManager::instance().setActive(loadedPreset->uid);
                                sculpt.applyActivePreset();
                            }
                        }
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        ImGui::Separator();

        // 1. General Brush Settings Section
        if (ImGui::CollapsingHeader("General Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("Radius", &settings.radius, 1.0f, 250.0f, "%.1f px");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Brush radius in pixels");

#ifdef _WIN32
            bool pressureSize = g_tablet.isPressureSizeEnabled();
            if (ImGui::Checkbox("Use Pressure for Size", &pressureSize)) {
                g_tablet.setPressureSizeEnabled(pressureSize);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Vary brush radius dynamically based on stylus pressure");

            bool pressureCursor = g_tablet.isPressureCursorEnabled();
            if (ImGui::Checkbox("Use Pressure for Cursor Dot", &pressureCursor)) {
                g_tablet.setPressureCursorEnabled(pressureCursor);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Vary cursor dot/point size dynamically based on stylus pressure");
#endif

            ImGui::SliderFloat("Intensity", &settings.intensity, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Overall brush strength");

            ImGui::SliderFloat("Hardness", &settings.hardness, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Brush profile hardness/falloff shape");

            ImGui::SliderFloat("Focal Shift", &settings.focalShift, -1.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Controls transition from center to border");

            ImGui::Checkbox("Focal Shift Falloff", &settings.focalShiftFalloff);

            ImGui::SliderFloat("Spacing", &settings.spacing, 0.01f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Spacing between stroke points as fraction of radius");

            ImGui::Checkbox("Negative (Invert)", &settings.negative);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggles add vs subtract sculpting direction");

            ImGui::Checkbox("Backface Culling", &settings.culling);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable backface culling to avoid painting through surfaces");

            // Alpha Texture
            const char* alphas[] = { "None (Sphere)", "Square (Clay)", "Alpha 1", "Alpha 2" };
            ImGui::Combo("Alpha Texture", &settings.idAlpha, alphas, IM_ARRAYSIZE(alphas));
        }

        // 2. Symmetry Settings Section
        if (ImGui::CollapsingHeader("Symmetry Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool useSym = sculpt.getUseSym();
            if (ImGui::Checkbox("Enable Symmetry", &useSym)) {
                sculpt.setUseSym(useSym);
            }
            if (useSym) {
                int axis = sculpt.getSymAxis();
                const char* axes[] = { "X Axis", "Y Axis", "Z Axis" };
                if (ImGui::Combo("Symmetry Axis", &axis, axes, IM_ARRAYSIZE(axes))) {
                    sculpt.setSymAxis(axis);
                }
            }
        }

        // 3. Tool-Specific Parameters Section
        bool hasSpecialParams = (brushType == BRUSH_SMOOTH || brushType == BRUSH_MOVE || 
                                 brushType == BRUSH_ELASTIC || brushType == BRUSH_CLAY || 
                                 brushType == BRUSH_CLAYBUILDUP || 
                                 brushType == BRUSH_SQUAREBRUSH || brushType == BRUSH_PAINT || 
                                 brushType == BRUSH_MASK || brushType == BRUSH_MASK_GRADIENT_BLUR ||
                                 brushType == BRUSH_MEASURE || brushType == BRUSH_DIVIDER ||
                                 brushType == BRUSH_TRANSFORM);

        if (hasSpecialParams) {
            if (ImGui::CollapsingHeader("Tool Specific Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (brushType == BRUSH_SMOOTH) {
                    ImGui::Checkbox("Tangential Smoothing", &settings.tangent);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Applies smoothing tangentially to the surface");
                }
                else if (brushType == BRUSH_MOVE || brushType == BRUSH_ELASTIC) {
                    ImGui::Checkbox("Topological Check", &settings.topoCheck);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only grabs topologically connected vertices within radius");
                    
                    if (brushType == BRUSH_ELASTIC) {
                        ImGui::SliderFloat("Elasticity", &settings.elasticity, 0.1f, 3.0f, "%.2f");
                    }
                }
                else if (brushType == BRUSH_CLAY || brushType == BRUSH_CLAYBUILDUP || 
                         brushType == BRUSH_SQUAREBRUSH) {
                    ImGui::Checkbox("Accumulate", &settings.accumulate);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Allows the brush to build up stroke details on overlap");
                    
                    ImGui::Checkbox("Lock Position", &settings.lockPosition);
                }
                else if (brushType == BRUSH_PAINT) {
                    ImGui::ColorEdit3("Albedo (Color)", &settings.paintColor.r);
                    ImGui::SliderFloat("Roughness", &settings.paintRoughness, 0.0f, 1.0f, "%.2f");
                    ImGui::SliderFloat("Metalness", &settings.paintMetallic, 0.0f, 1.0f, "%.2f");
                    
                    ImGui::Separator();
                    ImGui::Text("Paint Channels:");
                    ImGui::Checkbox("Write Albedo", &settings.writeAlbedo);
                    ImGui::SameLine();
                    ImGui::Checkbox("Write Roughness", &settings.writeRoughness);
                    ImGui::SameLine();
                    ImGui::Checkbox("Write Metalness", &settings.writeMetalness);
                }
                else if (brushType == BRUSH_MASK) {
                    Mesh* selectedMesh = scene.getSelected();
                    
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.44f, 0.70f, 1.00f));
                    if (ImGui::Button("Clear Mask", ImVec2(120, 26))) {
                        sculpt.clearMask(selectedMesh);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Invert Mask", ImVec2(120, 26))) {
                        sculpt.invertMask(selectedMesh);
                    }
                    if (ImGui::Button("Blur Mask", ImVec2(120, 26))) {
                        sculpt.blurMask(selectedMesh);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Sharpen Mask", ImVec2(120, 26))) {
                        sculpt.sharpenMask(selectedMesh);
                    }
                    ImGui::PopStyleColor();

                    ImGui::Separator();
                    ImGui::SliderInt("Iterations", &settings.maskSharpenBlurIterations, 1, 50);
                    ImGui::SliderFloat("Sharpen Factor", &settings.maskSharpenFactor, 0.1f, 5.0f, "%.2f");
                    ImGui::SliderFloat("Extract Thickness", &settings.maskExtractThickness, -0.5f, 0.5f, "%.2f");
                }
                else if (brushType == BRUSH_MASK_GRADIENT_BLUR) {
                    Mesh* selectedMesh = scene.getSelected();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.44f, 0.70f, 1.00f));
                    if (ImGui::Button("Reset Gradient Line", ImVec2(-1, 26))) {
                        sculpt.setGradActive(false);
                    }
                    if (ImGui::Button("Clear Mask", ImVec2(120, 26))) {
                        sculpt.clearMask(selectedMesh);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Invert Mask", ImVec2(120, 26))) {
                        sculpt.invertMask(selectedMesh);
                    }
                    ImGui::PopStyleColor();

                    ImGui::Separator();
                    ImGui::SliderInt("Blur Iterations", &settings.maskSharpenBlurIterations, 1, 100);
                    ImGui::Checkbox("Blur Masked Only", &settings.blurMaskedOnly);
                }
                else if (brushType == BRUSH_MEASURE) {
                    bool useDist = sculpt.getMeasureUseDistanceThickness();
                    if (ImGui::Checkbox("Use Perspective Thickness", &useDist)) {
                        sculpt.setMeasureUseDistanceThickness(useDist);
                    }
                    if (ImGui::Button("Clear Measurements", ImVec2(-1, 26))) {
                        sculpt.clearMeasurements();
                    }
                }
                else if (brushType == BRUSH_DIVIDER) {
                    int divs = sculpt.getDividerDivisions();
                    if (ImGui::SliderInt("Divisions", &divs, 2, 6)) {
                        sculpt.setDividerDivisions(divs);
                    }
                    bool useDist = sculpt.getMeasureUseDistanceThickness();
                    if (ImGui::Checkbox("Use Perspective Thickness", &useDist)) {
                        sculpt.setMeasureUseDistanceThickness(useDist);
                    }
                    if (ImGui::Button("Clear Dividers", ImVec2(-1, 26))) {
                        sculpt.clearMeasurements();
                    }
                }
                else if (brushType == BRUSH_TRANSFORM) {
                    ImGui::Checkbox("Edit Pivot (Alt)", &m_editPivot);
                    ImGui::Separator();
                    
                    if (ImGui::Button("Reset Matrix", ImVec2(-1, 26))) {
                        Mesh* selectedMesh = scene.getSelected();
                        if (selectedMesh) {
                            scene.pushHistoryState();
                            selectedMesh->matrix = glm::mat4(1.0f);
                            selectedMesh->isDirty = true;
                        }
                    }
                }
            }
        }

        ImGui::End();
    }

    // 4. Scene outliner
    if (m_showScenePanel) {
        ImGui::SetNextWindowPos({450, 40}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({320, 450}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Scene Outliner", &m_showScenePanel);

        // Primitive Spawning Tools
        ImGui::TextDisabled("PRIMITIVES");
        static bool spawnAtMask = false;
        static bool spawnMirror = false;
        ImGui::Checkbox("At Masked BBox", &spawnAtMask);
        ImGui::SameLine();
        ImGui::Checkbox("Mirror Symmetry", &spawnMirror);

        if (ImGui::Button("Sphere##Add", ImVec2(65, 0))) {
            if (spawnAtMask) {
                scene.addPrimitiveAtMask("sphere", spawnMirror, sculpt.getSymAxis());
            } else {
                scene.addSphere();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Geosphere##Add", ImVec2(90, 0))) {
            if (spawnAtMask) {
                scene.addPrimitiveAtMask("geosphere", spawnMirror, sculpt.getSymAxis());
            } else {
                scene.addGeosphere();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cube##Add", ImVec2(60, 0))) {
            if (spawnAtMask) {
                scene.addPrimitiveAtMask("cube", spawnMirror, sculpt.getSymAxis());
            } else {
                scene.addCube();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cylinder##Add", ImVec2(75, 0))) {
            if (spawnAtMask) {
                scene.addPrimitiveAtMask("cylinder", spawnMirror, sculpt.getSymAxis());
            } else {
                scene.addCylinder();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Torus##Add", ImVec2(60, 0))) {
            if (spawnAtMask) {
                scene.addPrimitiveAtMask("torus", spawnMirror, sculpt.getSymAxis());
            } else {
                scene.addTorus();
            }
        }

        ImGui::Separator();

        const auto& meshes = scene.getMeshes();
        int selected = scene.getSelectedIdx();

        ImGui::Text("Meshes in scene: %d", (int)meshes.size());

        static int renameTargetId = -1;
        static char renameBuf[128] = "";

        ImGui::BeginChild("MeshList", ImVec2(0, 180), true);
        if (ImGui::BeginTable("MeshListTable", 5, ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV)) {
            ImGui::TableSetupColumn("Act", ImGuiTableColumnFlags_WidthFixed, 30.0f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Verts", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("V1", ImGuiTableColumnFlags_WidthFixed, 30.0f);
            ImGui::TableSetupColumn("V2", ImGuiTableColumnFlags_WidthFixed, 30.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < (int)meshes.size(); i++) {
                Mesh* mesh = meshes[i];
                ImGui::TableNextRow();

                // Column 0: Act (Active checkbox)
                ImGui::TableNextColumn();
                ImGui::PushID(mesh->getID() * 10 + 3);
                bool isActive = (scene.getSelected() == mesh);
                if (ImGui::Checkbox("##Active", &isActive)) {
                    if (isActive) {
                        scene.setOrUnsetMesh(mesh, false);
                    } else {
                        scene.setOrUnsetMesh(nullptr, false);
                    }
                }
                ImGui::PopID();

                // Column 1: Name (Selectable / Renaming input)
                ImGui::TableNextColumn();
                ImGui::PushID(mesh->getID());

                bool isSelected = scene.isMeshSelected(mesh);
                if (renameTargetId == (int)mesh->getID()) {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputText("##RenameInput", renameBuf, sizeof(renameBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                        mesh->outlinerName = renameBuf;
                        renameTargetId = -1;
                    }
                    if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit()) {
                        mesh->outlinerName = renameBuf;
                        renameTargetId = -1;
                    }
                } else {
                    std::string displayName = mesh->outlinerName;
                    if (displayName.empty()) {
                        displayName = "Mesh " + std::to_string(i + 1);
                    }
                    if (ImGui::Selectable(displayName.c_str(), isSelected)) {
                        bool ctrl = ImGui::GetIO().KeyCtrl;
                        bool shift = ImGui::GetIO().KeyShift;
                        if (ctrl) {
                            scene.setOrUnsetMesh(mesh, true);
                        } else if (shift && selected != -1) {
                            int currentIdx = i;
                            int start = std::min(selected, currentIdx);
                            int end = std::max(selected, currentIdx);
                            scene.setOrUnsetMesh(nullptr, false);
                            for (int j = start; j <= end; ++j) {
                                scene.setOrUnsetMesh(meshes[j], true);
                            }
                        } else {
                            scene.setOrUnsetMesh(mesh, false);
                        }
                    }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                        renameTargetId = (int)mesh->getID();
                        strncpy(renameBuf, mesh->outlinerName.c_str(), sizeof(renameBuf));
                    }
                }
                ImGui::PopID();

                // Column 2: Verts Count
                ImGui::TableNextColumn();
                ImGui::Text("%s", formatCount(mesh->nbVerts).c_str());

                // Column 3: V1 Toggle
                ImGui::TableNextColumn();
                ImGui::PushID(mesh->getID() * 10 + 1);
                bool v1 = mesh->visibleV1;
                if (ImGui::Checkbox("##V1", &v1)) {
                    mesh->visibleV1 = v1;
                }
                ImGui::PopID();

                // Column 4: V2 Toggle
                ImGui::TableNextColumn();
                ImGui::PushID(mesh->getID() * 10 + 2);
                bool v2 = mesh->visibleV2;
                if (ImGui::Checkbox("##V2", &v2)) {
                    mesh->visibleV2 = v2;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();

        // Selection Actions
        bool canMerge = scene.getSelectedMeshes().size() >= 2;
        bool hasSelection = !scene.getSelectedMeshes().empty();

        if (ImGui::Button("Duplicate", ImVec2(80, 0))) {
            scene.duplicateSelection();
        }
        ImGui::SameLine();
        if (!canMerge) ImGui::BeginDisabled();
        if (ImGui::Button("Merge", ImVec2(60, 0))) {
            scene.mergeSelection();
        }
        if (!canMerge) ImGui::EndDisabled();
        ImGui::SameLine();
        if (!hasSelection) ImGui::BeginDisabled();
        if (ImGui::Button("Delete", ImVec2(60, 0))) {
            std::vector<Mesh*> toDel = scene.getSelectedMeshes();
            for (Mesh* m : toDel) {
                scene.removeMesh(m);
            }
        }
        if (!hasSelection) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Clear All", ImVec2(-1, 0))) {
            scene.clearScene();
        }

        // Measurement & Divider Tools Section
        ImGui::Separator();
        ImGui::TextDisabled("MEASURE & DIVIDER TOOLS");
        
        bool isMeasureActive = (sculpt.getBrush() == BRUSH_MEASURE);
        bool isDividerActive = (sculpt.getBrush() == BRUSH_DIVIDER);

        ImVec4 tealActive = ImVec4(0.01f, 0.52f, 0.45f, 1.00f);

        if (isMeasureActive) ImGui::PushStyleColor(ImGuiCol_Button, tealActive);
        if (ImGui::Button("Measure Tool", ImVec2(100, 0))) {
            sculpt.setTool(BRUSH_MEASURE);
        }
        if (isMeasureActive) ImGui::PopStyleColor();

        ImGui::SameLine();

        if (isDividerActive) ImGui::PushStyleColor(ImGuiCol_Button, tealActive);
        if (ImGui::Button("Divider Tool", ImVec2(100, 0))) {
            sculpt.setTool(BRUSH_DIVIDER);
        }
        if (isDividerActive) ImGui::PopStyleColor();

        ImGui::SameLine();
        if (ImGui::Button("Clear Tools", ImVec2(-1, 0))) {
            sculpt.clearMeasurements();
        }

        if (isMeasureActive) {
            bool useDist = sculpt.getMeasureUseDistanceThickness();
            if (ImGui::Checkbox("Use Distance Thickness", &useDist)) {
                sculpt.setMeasureUseDistanceThickness(useDist);
            }
            ImGui::Text("Active measure segments: %d", (int)sculpt.getMeasureSegments().size());
        } else if (isDividerActive) {
            int divs = sculpt.getDividerDivisions();
            if (ImGui::SliderInt("Divisions", &divs, 2, 6)) {
                sculpt.setDividerDivisions(divs);
            }
            ImGui::Text("Active divider segments: %d", (int)sculpt.getDividerSegments().size());
        }

        ImGui::End();
    }

    // 5. Topology & Remesh settings panel
    if (m_showTopologyPanel) {
        ImGui::SetNextWindowPos({740, 40}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({280, 200}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Topology & Remesh", &m_showTopologyPanel, ImGuiWindowFlags_AlwaysAutoResize);

        Mesh* selectedMesh = scene.getSelected();
        if (selectedMesh) {
            ImGui::Text("Vertices: %d", selectedMesh->nbVerts);
            ImGui::Text("Faces: %d", selectedMesh->nbFaces);
            ImGui::Separator();

            ImGui::SliderFloat("Detail Factor", &m_dyntopoDetail, 10.0f, 500.0f, "%.1f");
            ImGui::SliderInt("Remesh Resolution", &m_remeshResolution, 32, 512);

            if (ImGui::Button("Remesh", ImVec2(-1, 0))) {
                std::cout << "[Topology] Trigger remesh with resolution: " << m_remeshResolution << std::endl;
                performRemesh(scene);
            }
        } else {
            ImGui::Text("No active mesh selected");
        }

        ImGui::End();
    }

    // 6. Camera & Viewport settings panel
    if (m_showCameraPanel) {
        ImGui::SetNextWindowPos({160, 260}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({280, 320}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Camera Settings", &m_showCameraPanel, ImGuiWindowFlags_AlwaysAutoResize);

        Camera& camera = scene.getCamera();
        
        bool ortho = camera.isOrthographic();
        if (ImGui::Checkbox("Orthographic Projection", &ortho)) {
            camera.setProjectionType(ortho ? CameraEnums::Projection::ORTHOGRAPHIC : CameraEnums::Projection::PERSPECTIVE);
        }

        float fov = camera.getFov();
        if (ImGui::SliderFloat("FOV", &fov, 10.0f, 120.0f, "%.0f")) {
            camera.setFov(fov);
        }

        bool usePivot = camera.getUsePivot();
        if (ImGui::Checkbox("Picking pivot", &usePivot)) {
            camera.setUsePivot(usePivot);
        }

        // Camera Mode Selection
        const char* modes[] = { "Orbit", "Plane Trackball", "Spherical Trackball" };
        int currentMode = (int)camera.getMode();
        if (ImGui::Combo("Camera Mode", &currentMode, modes, IM_ARRAYSIZE(modes))) {
            camera.setMode((CameraEnums::CameraMode)currentMode);
        }

        ImGui::Separator();
        ImGui::Text("Camera Speeds:");
        
        float rot = camera.getSpeedRotate();
        if (ImGui::SliderFloat("Rotate Speed", &rot, 0.1f, 5.0f, "%.1f")) {
            camera.setSpeedRotate(rot);
        }

        float pan = camera.getSpeedTranslate();
        if (ImGui::SliderFloat("Pan Speed", &pan, 0.1f, 5.0f, "%.1f")) {
            camera.setSpeedTranslate(pan);
        }

        float zm = camera.getSpeedZoom();
        if (ImGui::SliderFloat("Zoom Speed", &zm, 0.1f, 5.0f, "%.1f")) {
            camera.setSpeedZoom(zm);
        }

        float roll = camera.getSpeedRoll();
        if (ImGui::SliderFloat("Roll Speed", &roll, 0.1f, 5.0f, "%.1f")) {
            camera.setSpeedRoll(roll);
        }

        ImGui::Separator();
        ImGui::Text("Snap Views:");
        
        if (ImGui::Button("Front", ImVec2(80, 0))) {
            camera.setOrbitAngles(0.0f, 0.0f);
            camera.setProjectionType(CameraEnums::Projection::ORTHOGRAPHIC);
        }
        ImGui::SameLine();
        if (ImGui::Button("Back", ImVec2(80, 0))) {
            camera.setOrbitAngles(0.0f, 3.14159265f);
            camera.setProjectionType(CameraEnums::Projection::ORTHOGRAPHIC);
        }
        ImGui::SameLine();
        if (ImGui::Button("Top", ImVec2(80, 0))) {
            camera.setOrbitAngles(-3.14159265f * 0.49f, 0.0f);
            camera.setProjectionType(CameraEnums::Projection::ORTHOGRAPHIC);
        }

        if (ImGui::Button("Bottom", ImVec2(80, 0))) {
            camera.setOrbitAngles(3.14159265f * 0.49f, 0.0f);
            camera.setProjectionType(CameraEnums::Projection::ORTHOGRAPHIC);
        }
        ImGui::SameLine();
        if (ImGui::Button("Left", ImVec2(80, 0))) {
            camera.setOrbitAngles(0.0f, 3.14159265f * 0.5f);
            camera.setProjectionType(CameraEnums::Projection::ORTHOGRAPHIC);
        }
        ImGui::SameLine();
        if (ImGui::Button("Right", ImVec2(80, 0))) {
            camera.setOrbitAngles(0.0f, -3.14159265f * 0.5f);
            camera.setProjectionType(CameraEnums::Projection::ORTHOGRAPHIC);
        }

        ImGui::Separator();
        if (ImGui::Button("Reset Camera View", ImVec2(-1, 0))) {
            camera.resetView();
        }

        ImGui::Separator();
        ImGui::Text("Split Viewport:");
        int splitModeVal = static_cast<int>(scene.getSplitMode());
        bool splitChanged = false;
        if (ImGui::RadioButton("Off", &splitModeVal, 0)) {
            splitChanged = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Mirror", &splitModeVal, 1)) {
            splitChanged = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Independent", &splitModeVal, 2)) {
            splitChanged = true;
        }
        if (splitChanged) {
            scene.setSplitMode(static_cast<Scene::SplitMode>(splitModeVal));
            int w, h;
            SDL_GetWindowSize(window, &w, &h);
            renderer.resize(w, h);
            if (scene.getSplitMode() != Scene::SplitMode::OFF) {
                int halfW = w / 2;
                scene.getCamera().onResize(halfW, h);
                if (scene.getCameraRight()) {
                    scene.getCameraRight()->onResize(w - halfW, h);
                }
            } else {
                scene.getCamera().onResize(w, h);
            }
        }

        if (scene.getSplitMode() != Scene::SplitMode::OFF) {
            bool showInactive = scene.getSplitShowInactiveCursor();
            if (ImGui::Checkbox("Show cursor in inactive viewport", &showInactive)) {
                scene.setSplitShowInactiveCursor(showInactive);
            }
        }

        ImGui::End();
    }

    // 7. Rendering Quality
    if (m_showRenderingPanel) {
        ImGui::SetNextWindowPos({450, 350}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({280, 200}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Rendering Quality", &m_showRenderingPanel, ImGuiWindowFlags_AlwaysAutoResize);

        // Global shading settings (always accessible)
        const char* shaders[] = { "PBR Shader", "Matcap Shading", "Wet Clay Shading", "Normal Shader", "Voxel Checker Shader", "Flat Shading" };
        int type = renderer.getShaderType();
        if (ImGui::Combo("Material Shader", &type, shaders, IM_ARRAYSIZE(shaders))) {
            renderer.setShaderType(type);
        }

        float curvatureVal = renderer.getCurvature() * 20.0f;
        if (ImGui::SliderFloat("Curvature", &curvatureVal, 0.0f, 100.0f, "%.0f")) {
            renderer.setCurvature(curvatureVal / 20.0f);
        }

        if (type == 0) { // PBR Shader
            const auto& envs = renderer.getEnvironments();
            if (!envs.empty()) {
                std::vector<const char*> envNames;
                for (const auto& env : envs) {
                    envNames.push_back(env.name.c_str());
                }
                int currentEnvIdx = renderer.getCurrentEnvIdx();
                if (ImGui::Combo("Environment Preset", &currentEnvIdx, envNames.data(), static_cast<int>(envNames.size()))) {
                    renderer.setEnvironmentPreset(currentEnvIdx);
                }
            }
        } else if (type == 1) { // Matcap Shading
            const auto& matcaps = renderer.getMatcaps();
            if (!matcaps.empty()) {
                std::vector<const char*> matcapNames;
                for (const auto& mc : matcaps) {
                    matcapNames.push_back(mc.name.c_str());
                }
                int matcapIdx = renderer.getMatcap();
                if (ImGui::Combo("Matcap Preset", &matcapIdx, matcapNames.data(), static_cast<int>(matcapNames.size()))) {
                    renderer.setMatcap(matcapIdx);
                }
            }

            // Import custom Matcap option
            static char matcapPath[256] = "";
            ImGui::InputText("Matcap Path", matcapPath, sizeof(matcapPath));
            ImGui::SameLine();
            if (ImGui::Button("Import Matcap")) {
                std::string pathStr(matcapPath);
                size_t lastSlash = pathStr.find_last_of("/\\");
                std::string name = (lastSlash != std::string::npos) ? pathStr.substr(lastSlash + 1) : pathStr;
                renderer.importMatcap(name, pathStr);
            }
        } else if (type == 2) { // Wet Clay Shading
            float wetness = renderer.getWetClayWetness();
            if (ImGui::SliderFloat("Wetness", &wetness, 0.0f, 1.0f, "%.2f")) {
                renderer.setWetClayWetness(wetness);
            }
            float bumpStrength = renderer.getWetClayBumpStrength();
            if (ImGui::SliderFloat("Bump Strength", &bumpStrength, 0.0f, 1.0f, "%.2f")) {
                renderer.setWetClayBumpStrength(bumpStrength);
            }
            float noiseScale = renderer.getWetClayNoiseScale();
            if (ImGui::SliderFloat("Noise Scale", &noiseScale, 1.0f, 30.0f, "%.1f")) {
                renderer.setWetClayNoiseScale(noiseScale);
            }
            float sssIntensity = renderer.getWetClaySSSIntensity();
            if (ImGui::SliderFloat("SSS Intensity", &sssIntensity, 0.0f, 1.0f, "%.2f")) {
                renderer.setWetClaySSSIntensity(sssIntensity);
            }
            glm::vec3 sssColor = renderer.getWetClaySSSColor();
            if (ImGui::ColorEdit3("SSS Color", glm::value_ptr(sssColor))) {
                renderer.setWetClaySSSColor(sssColor);
            }
        }

        bool wire = renderer.getShowWireframe();
        if (ImGui::Checkbox("Show Wireframe", &wire)) {
            renderer.setShowWireframe(wire);
        }

        bool flat = renderer.getFlatShading();
        if (ImGui::Checkbox("Flat Shading Mode", &flat)) {
            renderer.setFlatShading(flat);
        }

        bool filmic = renderer.getFilmic();
        if (ImGui::Checkbox("Filmic Tonemapping", &filmic)) {
            renderer.setFilmic(filmic);
        }

        bool showContour = renderer.getShowContour();
        if (ImGui::Checkbox("Show Selection Outline", &showContour)) {
            renderer.setShowContour(showContour);
        }

        if (showContour) {
            glm::vec4 cColor = renderer.getContourColor();
            if (ImGui::ColorEdit4("Outline Color", glm::value_ptr(cColor))) {
                renderer.setContourColor(cColor);
            }
        }

        float cursorThickness = renderer.getCursorThickness();
        if (ImGui::SliderFloat("Brush Cursor Thickness", &cursorThickness, 1.0f, 5.0f, "%.1f px")) {
            renderer.setCursorThickness(cursorThickness);
        }

        bool smoothCursor = renderer.getSmoothCursor();
        if (ImGui::Checkbox("Smooth (Antialiased) Cursor", &smoothCursor)) {
            renderer.setSmoothCursor(smoothCursor);
        }

        // Global Material Settings (always accessible)
        ImGui::Separator();
        ImGui::TextDisabled("MATERIAL SETTINGS");
        
        float alpha = renderer.getAlpha();
        if (ImGui::SliderFloat("Transparency (Alpha)", &alpha, 0.0f, 1.0f, "%.2f")) {
            renderer.setAlpha(alpha);
        }

        float albedo[3] = { renderer.getAlbedo()[0], renderer.getAlbedo()[1], renderer.getAlbedo()[2] };
        if (ImGui::ColorEdit3("Albedo Base Color", albedo)) {
            renderer.setAlbedo(albedo[0], albedo[1], albedo[2]);
        }

        if (type == 0) { // PBR Shader settings
            float roughness = renderer.getRoughness();
            if (ImGui::SliderFloat("Roughness", &roughness, 0.0f, 1.0f, "%.2f")) {
                renderer.setRoughness(roughness);
            }
            float metallic = renderer.getMetallic();
            if (ImGui::SliderFloat("Metallic", &metallic, 0.0f, 1.0f, "%.2f")) {
                renderer.setMetallic(metallic);
            }

            // Import UV texture option
            static char texturePath[256] = "";
            ImGui::InputText("UV Texture Path", texturePath, sizeof(texturePath));
            ImGui::SameLine();
            if (ImGui::Button("Import UV##PBR")) {
                int w = 0, h = 0, ch = 0;
                unsigned char* data = stbi_load(texturePath, &w, &h, &ch, 4);
                if (data) {
                    if (renderer.getTextureId() != 0) {
                        GLuint tid = renderer.getTextureId();
                        glDeleteTextures(1, &tid);
                    }
                    GLuint newTexId = 0;
                    glGenTextures(1, &newTexId);
                    glBindTexture(GL_TEXTURE_2D, newTexId);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    stbi_image_free(data);
                    renderer.setTextureId(newTexId);
                    renderer.setHasUV(true);
                } else {
                    std::cerr << "Failed to load UV texture: " << texturePath << std::endl;
                }
            }
        }

        ImGui::Separator();
        if (ImGui::CollapsingHeader("Background Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool showBg = renderer.getShowBackground();
            if (ImGui::Checkbox("Show Background", &showBg)) {
                renderer.setShowBackground(showBg);
            }

            if (showBg) {
                const char* bgTypes[] = { "Image", "Ambient env" };
                int bgType = renderer.getBackgroundType();
                int selectedIdx = (bgType == 2) ? 1 : 0;
                if (ImGui::Combo("Type##bg", &selectedIdx, bgTypes, IM_ARRAYSIZE(bgTypes))) {
                    renderer.setBackgroundType((selectedIdx == 1) ? 2 : 0);
                }

                if (bgType == 1) { // Environment
                    float bgBlur = renderer.getBgBlur();
                    if (ImGui::SliderFloat("Blur##bg", &bgBlur, 0.0f, 1.0f, "%.2f")) {
                        renderer.setBgBlur(bgBlur);
                    }
                }

                if (bgType == 0) { // Image
                    bool bgFill = renderer.getBgFill();
                    if (ImGui::Checkbox("Fill##bg", &bgFill)) {
                        renderer.setBgFill(bgFill);
                    }

                    std::string path = renderer.getBgTexturePath();
                    static char bgImagePath[256] = "";
                    if (path != bgImagePath && path.size() < sizeof(bgImagePath)) {
                        std::strncpy(bgImagePath, path.c_str(), sizeof(bgImagePath));
                    }

                    ImGui::InputText("Image Path##bg", bgImagePath, sizeof(bgImagePath));
                    if (ImGui::Button("Import Image##bg", ImVec2(120, 0))) {
                        renderer.loadBackgroundTexture(bgImagePath);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Reset Image##bg", ImVec2(120, 0))) {
                        renderer.deleteBackgroundTexture();
                        bgImagePath[0] = '\0';
                    }
                }
            }
        }

        ImGui::Separator();
        ImGui::Text("Settings Profile:");
        if (ImGui::Button("Save Profile", ImVec2(120, 0))) {
            RenderSettings::save("render_settings.cfg", renderer, scene);
        }
        ImGui::SameLine();
        if (ImGui::Button("Load Profile", ImVec2(120, 0))) {
            RenderSettings::load("render_settings.cfg", renderer, scene);
        }

        ImGui::End();
    }

    // 7. Reference Images Panel
    if (m_showReferenceImagesPanel) {
        ImGui::SetNextWindowPos({500, 40}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({300, 250}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Reference Images", &m_showReferenceImagesPanel);

        ImGui::InputText("Image Path", m_refImagePath, sizeof(m_refImagePath));
        if (ImGui::Button("Load Reference Image", ImVec2(-1, 0))) {
            scene.addReferenceImage(m_refImagePath);
        }

        ImGui::Separator();

        Camera& camera = scene.getCamera();
        bool ref2D = camera.getRef2DMode();
        if (ImGui::Checkbox("2D Pan/Zoom Mode", &ref2D)) {
            camera.setRef2DMode(ref2D);
        }
        ImGui::SameLine();
        bool refDrag = camera.getRefDragEnabled();
        if (ImGui::Checkbox("Ref Drag", &refDrag)) {
            camera.setRefDragEnabled(refDrag);
        }

        if (ImGui::Button("Reset 2D View", ImVec2(-1, 0))) {
            camera.resetView2D();
        }

        ImGui::Separator();

        auto& images = scene.getReferenceImages();
        if (images.empty()) {
            ImGui::Text("No reference images loaded.");
        } else {
            for (size_t i = 0; i < images.size(); ++i) {
                auto& img = images[i];
                ImGui::PushID(static_cast<int>(i));

                std::string displayName = img.path;
                size_t lastSlash = displayName.find_last_of("\\/");
                if (lastSlash != std::string::npos) {
                    displayName = displayName.substr(lastSlash + 1);
                }

                if (ImGui::TreeNode(displayName.c_str())) {
                    ImGui::Checkbox("Visible", &img.visible);
                    ImGui::Checkbox("Pinned 2D Overlay", &img.pinned2D);
                    ImGui::SliderFloat("Opacity", &img.opacity, 0.0f, 1.0f, "%.2f");
                    ImGui::SliderFloat("Scale", &img.scale, 0.1f, 10.0f, "%.2f");
                    
                    if (img.pinned2D) {
                        ImGui::SliderFloat("Offset X", &img.offsetX, -1.0f, 1.0f, "%.2f");
                        ImGui::SliderFloat("Offset Y", &img.offsetY, -1.0f, 1.0f, "%.2f");
                    } else {
                        ImGui::SliderFloat("Position X", &img.offsetX, -200.0f, 200.0f, "%.1f");
                        ImGui::SliderFloat("Position Y", &img.offsetY, -200.0f, 200.0f, "%.1f");
                    }

                    if (ImGui::Button("Remove", ImVec2(-1, 0))) {
                        scene.removeReferenceImage(i);
                        ImGui::TreePop();
                        ImGui::PopID();
                        break;
                    }

                    ImGui::TreePop();
                }

                ImGui::PopID();
            }
        }

        ImGui::End();
    }

    // 7.5. Import & Export Panel
    if (m_showFilesPanel) {
        ImGui::SetNextWindowPos({740, 260}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({300, 200}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Import & Export", &m_showFilesPanel, ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::InputText("Import Path", m_importPath, sizeof(m_importPath));
        if (ImGui::Button("Import##file", ImVec2(-1, 0))) {
            auto newMeshes = FileManager::importFiles(m_importPath, &scene, &renderer);
            for (auto* mesh : newMeshes) {
                scene.addMesh(mesh);
            }
            scene.pushHistoryState();
        }
        
        ImGui::Separator();

        ImGui::InputText("Export Path", m_exportPath, sizeof(m_exportPath));
        if (ImGui::Button("Export##file", ImVec2(-1, 0))) {
            FileManager::exportMeshes(m_exportPath, scene.getMeshes(), &scene, &renderer);
        }

        ImGui::End();
    }

    // 8. Gizmo Cube Window
    if (m_showGizmoCube) {
        int wWidth, wHeight;
        SDL_GetWindowSize(window, &wWidth, &wHeight);

        // Position it at the top-right corner, below the main menu bar
        ImGui::SetNextWindowPos(ImVec2((float)wWidth - 150.0f, 40.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(130.0f, 130.0f));
        
        ImGui::Begin("Gizmo Cube", nullptr, 
            ImGuiWindowFlags_NoTitleBar | 
            ImGuiWindowFlags_NoResize | 
            ImGuiWindowFlags_NoScrollbar | 
            ImGuiWindowFlags_NoBackground | 
            ImGuiWindowFlags_NoSavedSettings);

        Camera& camera = scene.getCamera();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        
        // Cube half size in pixels on screen
        float side = 36.0f;
        const float w = 0.70f;
        glm::vec3 localVerts[24] = {
            // Front face (Z = +1.0)
            { -w, -w,  1.0f }, // 0
            {  w, -w,  1.0f }, // 1
            {  w,  w,  1.0f }, // 2
            { -w,  w,  1.0f }, // 3

            // Back face (Z = -1.0)
            {  w, -w, -1.0f }, // 4
            { -w, -w, -1.0f }, // 5
            { -w,  w, -1.0f }, // 6
            {  w,  w, -1.0f }, // 7

            // Left face (X = -1.0)
            { -1.0f, -w, -w }, // 8
            { -1.0f, -w,  w }, // 9
            { -1.0f,  w,  w }, // 10
            { -1.0f,  w, -w }, // 11

            // Right face (X = +1.0)
            {  1.0f, -w,  w }, // 12
            {  1.0f, -w, -w }, // 13
            {  1.0f,  w, -w }, // 14
            {  1.0f,  w,  w }, // 15

            // Top face (Y = +1.0)
            { -w,  1.0f,  w }, // 16
            {  w,  1.0f,  w }, // 17
            {  w,  1.0f, -w }, // 18
            { -w,  1.0f, -w }, // 19

            // Bottom face (Y = -1.0)
            { -w, -1.0f, -w }, // 20
            {  w, -1.0f, -w }, // 21
            {  w, -1.0f,  w }, // 22
            { -w, -1.0f,  w }  // 23
        };

        // Center of the Gizmo Cube window
        ImVec2 winPos = ImGui::GetWindowPos();
        ImVec2 winSize = ImGui::GetWindowSize();
        ImVec2 center = ImVec2(winPos.x + winSize.x * 0.5f, winPos.y + winSize.y * 0.5f);

        glm::mat4 V = camera.getViewMatrix();
        glm::mat3 viewRot = glm::mat3(V);

        ImVec2 screenVerts[24];
        float depth[24];
        for (int i = 0; i < 24; ++i) {
            glm::vec3 rotVert = viewRot * localVerts[i];
            screenVerts[i] = ImVec2(center.x + rotVert.x * side, center.y - rotVert.y * side);
            depth[i] = rotVert.z;
        }

        struct GizmoPart {
            const char* label;
            int numVerts;
            int vIdx[4];
            glm::vec3 normal;
            float rotX;
            float rotY;
            ImU32 color;
            ImU32 hoverColor;
            glm::vec3 localX;
        };

        // 26 parts in total: 6 faces, 12 edges, 8 corners
        GizmoPart parts[26] = {
            // --- 6 Faces ---
            { "FRONT",  4, { 0, 1, 2, 3 }, { 0.0f,  0.0f,  1.0f },  0.0f,                 0.0f,                 IM_COL32(50, 120, 230, 220),  IM_COL32(70, 150, 255, 255), { 1.0f,  0.0f,  0.0f } },
            { "BACK",   4, { 4, 5, 6, 7 }, { 0.0f,  0.0f, -1.0f },  0.0f,                -3.14159265f,          IM_COL32(40, 90, 180, 220),   IM_COL32(60, 120, 220, 255), {-1.0f,  0.0f,  0.0f } },
            { "LEFT",   4, { 8, 9, 10, 11 }, {-1.0f,  0.0f,  0.0f },  0.0f,                 3.14159265f * 0.5f,   IM_COL32(180, 40, 40, 220),   IM_COL32(220, 60, 60, 255), { 0.0f,  0.0f,  1.0f } },
            { "RIGHT",  4, { 12, 13, 14, 15 }, { 1.0f,  0.0f,  0.0f },  0.0f,                -3.14159265f * 0.5f,   IM_COL32(230, 50, 50, 220),   IM_COL32(255, 70, 70, 255), { 0.0f,  0.0f, -1.0f } },
            { "TOP",    4, { 16, 17, 18, 19 }, { 0.0f,  1.0f,  0.0f }, -3.14159265f * 0.49f,  0.0f,                 IM_COL32(50, 200, 50, 220),   IM_COL32(70, 240, 70, 255), { 1.0f,  0.0f,  0.0f } },
            { "BOTTOM", 4, { 20, 21, 22, 23 }, { 0.0f, -1.0f,  0.0f },  3.14159265f * 0.49f,  0.0f,                 IM_COL32(40, 150, 40, 220),   IM_COL32(60, 190, 60, 255), { 1.0f,  0.0f,  0.0f } },

            // --- 12 Edges ---
            { "", 4, { 3, 2, 17, 16 }, { 0.0f, 0.707f, 0.707f }, -3.14159265f * 0.25f, 0.0f,                 IM_COL32(50, 160, 140, 220),  IM_COL32(70, 200, 180, 255), { 0.0f, 0.0f, 0.0f } },
            { "", 4, { 0, 23, 22, 1 }, { 0.0f, -0.707f, 0.707f },  3.14159265f * 0.25f, 0.0f,                 IM_COL32(45, 135, 135, 220),  IM_COL32(65, 170, 170, 255), { 0.0f, 0.0f, 0.0f } },
            { "", 4, { 7, 6, 19, 18 }, { 0.0f, 0.707f, -0.707f }, -3.14159265f * 0.25f, -3.14159265f,          IM_COL32(45, 145, 115, 220),  IM_COL32(65, 180, 145, 255), { 0.0f, 0.0f, 0.0f } },
            { "", 4, { 4, 21, 20, 5 }, { 0.0f, -0.707f, -0.707f },  3.14159265f * 0.25f, -3.14159265f,          IM_COL32(40, 120, 110, 220),  IM_COL32(60, 150, 140, 255), { 0.0f, 0.0f, 0.0f } },

            { "", 4, { 3, 10, 9, 0 }, {-0.707f, 0.0f, 0.707f }, 0.0f,                  3.14159265f * 0.25f,  IM_COL32(115, 80, 135, 220),  IM_COL32(145, 100, 170, 255), { 0.0f, 0.0f, 0.0f } },
            { "", 4, { 1, 12, 15, 2 }, { 0.707f, 0.0f, 0.707f }, 0.0f,                 -3.14159265f * 0.25f,  IM_COL32(140, 85, 140, 220),  IM_COL32(170, 110, 170, 255), { 0.0f, 0.0f, 0.0f } },
            { "", 4, { 5, 8, 11, 6 }, {-0.707f, 0.0f, -0.707f }, 0.0f,                  3.14159265f * 0.75f,  IM_COL32(110, 65, 110, 220),  IM_COL32(140, 85, 140, 255), { 0.0f, 0.0f, 0.0f } },
            { "", 4, { 7, 14, 13, 4 }, { 0.707f, 0.0f, -0.707f }, 0.0f,                 -3.14159265f * 0.75f,  IM_COL32(135, 70, 115, 220),  IM_COL32(165, 90, 145, 255), { 0.0f, 0.0f, 0.0f } },

            { "", 4, { 16, 10, 11, 19 }, {-0.707f, 0.707f, 0.0f }, -3.14159265f * 0.25f,  3.14159265f * 0.5f,   IM_COL32(115, 120, 45, 220),  IM_COL32(145, 150, 65, 255), { 0.0f, 0.0f, 0.0f } },
            { "", 4, { 17, 15, 14, 18 }, { 0.707f, 0.707f, 0.0f }, -3.14159265f * 0.25f, -3.14159265f * 0.5f,   IM_COL32(140, 125, 50, 220),  IM_COL32(175, 155, 70, 255), { 0.0f, 0.0f, 0.0f } },
            { "", 4, { 23, 9, 8, 20 }, {-0.707f, -0.707f, 0.0f },  3.14159265f * 0.25f,  3.14159265f * 0.5f,   IM_COL32(110, 95, 40, 220),   IM_COL32(140, 120, 60, 255), { 0.0f, 0.0f, 0.0f } },
            { "", 4, { 22, 12, 13, 21 }, { 0.707f, -0.707f, 0.0f },  3.14159265f * 0.25f, -3.14159265f * 0.5f,   IM_COL32(135, 100, 45, 220),  IM_COL32(165, 125, 65, 255), { 0.0f, 0.0f, 0.0f } },

            // --- 8 Corners ---
            { "", 3, { 2, 15, 17, 0 }, { 0.577f, 0.577f, 0.577f }, -3.14159265f * 0.25f, -3.14159265f * 0.25f,   IM_COL32(110, 120, 110, 220), IM_COL32(140, 150, 140, 255), { 0.0f, 0.0f, 0.0f } },
            { "", 3, { 3, 16, 10, 0 }, {-0.577f, 0.577f, 0.577f }, -3.14159265f * 0.25f,  3.14159265f * 0.25f,   IM_COL32(95, 120, 95, 220),   IM_COL32(125, 150, 125, 255), { 0.0f, 0.0f, 0.0f } },
            { "", 3, { 7, 18, 14, 0 }, { 0.577f, 0.577f, -0.577f }, -3.14159265f * 0.25f, -3.14159265f * 0.75f,   IM_COL32(105, 110, 100, 220), IM_COL32(135, 140, 130, 255), { 0.0f, 0.0f, 0.0f } },
            { "", 3, { 6, 11, 19, 0 }, {-0.577f, 0.577f, -0.577f }, -3.14159265f * 0.25f,  3.14159265f * 0.75f,   IM_COL32(90, 110, 90, 220),   IM_COL32(120, 140, 120, 255), { 0.0f, 0.0f, 0.0f } },

            { "", 3, { 1, 22, 12, 0 }, { 0.577f, -0.577f, 0.577f },  3.14159265f * 0.25f, -3.14159265f * 0.25f,   IM_COL32(110, 100, 100, 220), IM_COL32(140, 130, 130, 255), { 0.0f, 0.0f, 0.0f } },
            { "", 3, { 0, 9, 23, 0 }, {-0.577f, -0.577f, 0.577f },  3.14159265f * 0.25f,  3.14159265f * 0.25f,   IM_COL32(95, 100, 85, 220),   IM_COL32(125, 130, 115, 255), { 0.0f, 0.0f, 0.0f } },
            { "", 3, { 4, 13, 21, 0 }, { 0.577f, -0.577f, -0.577f },  3.14159265f * 0.25f, -3.14159265f * 0.75f,   IM_COL32(105, 95, 90, 220),   IM_COL32(135, 125, 120, 255), { 0.0f, 0.0f, 0.0f } },
            { "", 3, { 5, 8, 20, 0 }, {-0.577f, -0.577f, -0.577f },  3.14159265f * 0.25f,  3.14159265f * 0.75f,   IM_COL32(90, 95, 80, 220),    IM_COL32(120, 125, 110, 255), { 0.0f, 0.0f, 0.0f } }
        };

        std::vector<int> visiblePartIndices;
        for (int i = 0; i < 26; ++i) {
            glm::vec3 viewNormal = viewRot * parts[i].normal;
            if (viewNormal.z > 0.0f) {
                visiblePartIndices.push_back(i);
            }
        }

        std::sort(visiblePartIndices.begin(), visiblePartIndices.end(), [&](int a, int b) {
            float depthA = 0.0f;
            for (int j = 0; j < parts[a].numVerts; ++j) {
                depthA += depth[parts[a].vIdx[j]];
            }
            depthA /= (float)parts[a].numVerts;

            float depthB = 0.0f;
            for (int j = 0; j < parts[b].numVerts; ++j) {
                depthB += depth[parts[b].vIdx[j]];
            }
            depthB /= (float)parts[b].numVerts;

            return depthA < depthB; // Back-to-front sorting
        });

        ImVec2 mousePos = ImGui::GetMousePos();
        bool mouseClicked = ImGui::IsMouseClicked(0);
        int hoveredPartIdx = -1;

        for (int i = (int)visiblePartIndices.size() - 1; i >= 0; --i) {
            int partIdx = visiblePartIndices[i];
            const auto& part = parts[partIdx];
            ImVec2 poly[4];
            for (int j = 0; j < part.numVerts; ++j) {
                poly[j] = screenVerts[part.vIdx[j]];
            }

            if (isPointInPolygon(mousePos, poly, part.numVerts)) {
                hoveredPartIdx = partIdx;
                break;
            }
        }

        for (int partIdx : visiblePartIndices) {
            const auto& part = parts[partIdx];
            ImVec2 poly[4];
            for (int j = 0; j < part.numVerts; ++j) {
                poly[j] = screenVerts[part.vIdx[j]];
            }

            bool isHovered = (partIdx == hoveredPartIdx);
            ImU32 color = isHovered ? part.hoverColor : part.color;

            // Draw part polygon
            drawList->AddConvexPolyFilled(poly, part.numVerts, color);

            // Draw borders
            drawList->AddPolyline(poly, part.numVerts, IM_COL32(220, 220, 220, 255), ImDrawFlags_Closed, 1.2f);

            // Draw centered text if label is set and mostly facing the camera
            if (part.label[0] != '\0') {
                glm::vec3 viewNormal = viewRot * part.normal;
                if (viewNormal.z >= 0.5f) {
                    ImGui::SetWindowFontScale(1.0f);
                    ImVec2 centerPos(0.0f, 0.0f);
                    for (int j = 0; j < part.numVerts; ++j) {
                        centerPos.x += poly[j].x;
                        centerPos.y += poly[j].y;
                    }
                    centerPos.x /= (float)part.numVerts;
                    centerPos.y /= (float)part.numVerts;

                    ImVec2 textSize = ImGui::CalcTextSize(part.label);
                    ImVec2 textPos = ImVec2(centerPos.x - textSize.x * 0.5f, centerPos.y - textSize.y * 0.5f);

                    int vtxStart = drawList->VtxBuffer.Size;

                    drawList->AddText(ImVec2(textPos.x + 1.0f, textPos.y + 1.0f), IM_COL32(0, 0, 0, 200), part.label);
                    drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), part.label);

                    int vtxEnd = drawList->VtxBuffer.Size;

                    glm::vec3 localY = glm::cross(part.normal, part.localX);
                    for (int vIdx = vtxStart; vIdx < vtxEnd; ++vIdx) {
                        ImDrawVert& v = drawList->VtxBuffer[vIdx];
                        float dx = v.pos.x - centerPos.x;
                        float dy = v.pos.y - centerPos.y;

                        float local_dx = dx / side;
                        float local_dy = -dy / side;

                        glm::vec3 P_local = part.normal + local_dx * part.localX + local_dy * localY;
                        glm::vec3 rotVert = viewRot * P_local;

                        v.pos.x = center.x + rotVert.x * side;
                        v.pos.y = center.y - rotVert.y * side;
                    }

                    ImGui::SetWindowFontScale(1.0f);
                }
            }

            if (isHovered && mouseClicked) {
                std::ofstream logFile("c:\\Users\\user\\Desktop\\cpp\\sculptsp-native\\debug_log.txt", std::ios::app);
                if (logFile.is_open()) {
                    logFile << "Hovered and Clicked Part: " << part.label << std::endl;
                    logFile << "  part.rotX: " << part.rotX << ", part.rotY: " << part.rotY << std::endl;
                    logFile << "  m_gizmoClickPending: " << (m_gizmoClickPending ? "yes" : "no") << std::endl;
                    if (m_gizmoClickPending) {
                        logFile << "  pendingRotX: " << m_gizmoClickPartRotX << ", pendingRotY: " << m_gizmoClickPartRotY << std::endl;
                    }
                    logFile.close();
                }

                if (m_gizmoClickPending && std::abs(m_gizmoClickPartRotX - (-part.rotX)) < 1e-3f && std::abs(m_gizmoClickPartRotY - part.rotY) < 1e-3f) {
                    // Cancel pending view change on double-click
                    m_gizmoClickPending = false;
                } else {
                    // Set pending click
                    m_gizmoClickPending = true;
                    m_gizmoClickTime = std::chrono::steady_clock::now();
                    m_gizmoClickPartRotX = -part.rotX;
                    m_gizmoClickPartRotY = part.rotY;
                }
            }
        }

        // Process pending gizmo click
        if (m_gizmoClickPending) {
            auto now = std::chrono::steady_clock::now();
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_gizmoClickTime).count();
            if (elapsedMs > 250) {
                m_gizmoClickPending = false;

                const float PI = 3.14159265f;
                float targetRotX = camera.getTargetRotX();
                float targetRotY = camera.getTargetRotY();

                float diffX = std::abs(targetRotX - m_gizmoClickPartRotX);
                float diffY = targetRotY - m_gizmoClickPartRotY;
                diffY = std::fmod(diffY, 2.0f * PI);
                if (diffY < -PI) diffY += 2.0f * PI;
                if (diffY > PI) diffY -= 2.0f * PI;
                diffY = std::abs(diffY);

                bool alreadyMatch = camera.isOrthographic() && (diffX < 1e-3f) && (diffY < 1e-3f);

                std::ofstream logFile("c:\\Users\\user\\Desktop\\cpp\\sculptsp-native\\debug_log.txt", std::ios::app);
                if (logFile.is_open()) {
                    logFile << "Gizmo Click Triggered:" << std::endl;
                    logFile << "  targetRotX: " << targetRotX << ", targetRotY: " << targetRotY << std::endl;
                    logFile << "  clickedRotX: " << m_gizmoClickPartRotX << ", clickedRotY: " << m_gizmoClickPartRotY << std::endl;
                    logFile << "  diffX: " << diffX << ", diffY: " << diffY << std::endl;
                    logFile << "  isOrthographic: " << (camera.isOrthographic() ? "yes" : "no") << std::endl;
                    logFile << "  alreadyMatch: " << (alreadyMatch ? "yes" : "no") << std::endl;
                    logFile << "--------------------------------------" << std::endl;
                    logFile.close();
                }

                if (!alreadyMatch) {
                    camera.toggleViewAngles(m_gizmoClickPartRotX, m_gizmoClickPartRotY);
                }
            }
        }

        ImGui::End();
    }

    // 9. Mesh Statistics & FPS HUD Window (bottom-right)
    if (m_showMeshInfo) {
        int wWidth, wHeight;
        SDL_GetWindowSize(window, &wWidth, &wHeight);

        // Position it at the bottom-right corner, with a small padding
        float padX = 10.0f;
        float padY = 10.0f;
        ImGui::SetNextWindowPos(ImVec2((float)wWidth - padX, (float)wHeight - padY), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.75f); // Transparent background

        // Use custom style colors for the window bg/border to match the sleek dark theme/CSS
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.27f, 0.27f, 0.27f, 0.75f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.18f, 0.20f, 0.22f, 0.60f));
        
        ImGui::Begin("Mesh Statistics HUD", nullptr, 
            ImGuiWindowFlags_NoTitleBar | 
            ImGuiWindowFlags_NoResize | 
            ImGuiWindowFlags_NoScrollbar | 
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove);

        // Get mesh stats
        Mesh* activeMesh = scene.getSelected();
        int activePoints = activeMesh ? activeMesh->getNbVertices() : 0;
        
        int totalPoints = 0;
        for (Mesh* m : scene.getMeshes()) {
            if (m) {
                totalPoints += m->getNbVertices();
            }
        }

        // Calculate sliding-window FPS
        m_fpsTimes.push_back(std::chrono::steady_clock::now());
        while (m_fpsTimes.size() > 60) {
            m_fpsTimes.pop_front();
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_fpsLastUpdate).count();
        if (elapsed > 500 && m_fpsTimes.size() >= 2) {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_fpsTimes.front()).count();
            if (duration > 0) {
                m_fpsValue = (int)std::round(((m_fpsTimes.size() - 1) * 1000.0f) / duration);
            }
            m_fpsLastUpdate = now;
        }

        // Display
        ImGui::Text("Active points: %s", formatCount(activePoints).c_str());
        ImGui::Text("Total points: %s", formatCount(totalPoints).c_str());
        if (m_fpsValue > 0) {
            ImGui::Text("FPS: %d", m_fpsValue);
        } else {
            ImGui::Text("FPS: --");
        }

        ImGui::End();
        
        ImGui::PopStyleColor(2);
    }

#ifdef _WIN32
    if (m_showTabletDiagPanel) {
        ImGui::Begin("Tablet Diagnostics", &m_showTabletDiagPanel, ImGuiWindowFlags_AlwaysAutoResize);
        
        TabletMode activeMode = g_tablet.getActiveMode();
        TabletMode forcedMode = g_tablet.getForcedMode();
        
        const char* modeNames[] = { "Auto (Fallback)", "Windows Ink", "WinTab" };
        int currentModeIdx = 0;
        if (forcedMode == TabletMode::WININK) currentModeIdx = 1;
        else if (forcedMode == TabletMode::WINTAB) currentModeIdx = 2;
        
        if (ImGui::Combo("Forced Mode", &currentModeIdx, modeNames, 3)) {
            if (currentModeIdx == 0) g_tablet.setForcedMode(TabletMode::NONE);
            else if (currentModeIdx == 1) g_tablet.setForcedMode(TabletMode::WININK);
            else if (currentModeIdx == 2) g_tablet.setForcedMode(TabletMode::WINTAB);
        }
        
        TabletInput::DiagInfo diag = g_tablet.getDiagInfo();
        
        ImGui::Text("Active Mode: %s", (diag.activeMode == TabletMode::WINTAB ? "WinTab" : (diag.activeMode == TabletMode::WININK ? "Windows Ink" : "None (Mouse)")));
        ImGui::SameLine();
        if (diag.activeMode != TabletMode::NONE) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "  [Connected]");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "  [Disconnected]");
        }
        
        ImGui::Separator();
        
        ImGui::Text("Pressure:");
        ImGui::ProgressBar(diag.currentPressure, ImVec2(-1, 20), "");
        ImGui::SameLine(ImGui::GetWindowWidth() * 0.5f);
        ImGui::Text("Pressure Value: %.3f", diag.currentPressure);
        
        ImGui::Text("Tilt X: %.1f deg", diag.currentTiltX);
        ImGui::Text("Tilt Y: %.1f deg", diag.currentTiltY);
        
        ImGui::Text("Pen Down: %s", (diag.isPenDown ? "YES" : "NO"));
        
        ImGui::Separator();
        
        ImGui::Text("Wintab32.dll: %s", (diag.wintabLoaded ? "LOADED" : "NOT FOUND"));
        ImGui::Text("WinTab Context: %s", (diag.wintabContextOpen ? "OPEN" : "CLOSED"));
        ImGui::Text("Windows Ink: %s", (diag.winInkAvailable ? "AVAILABLE" : "NOT ACTIVE"));
        ImGui::Text("Max Pressure: %d", diag.maxPressure);
        ImGui::Text("Packets/Sec: %d", diag.packetsLastSecond);
        
        ImGui::Separator();
        
        bool pressureEnabled = g_tablet.isPressureEnabled();
        if (ImGui::Checkbox("Use Pressure for Sculpting", &pressureEnabled)) {
            g_tablet.setPressureEnabled(pressureEnabled);
        }
        
        bool pressureSizeEnabled = g_tablet.isPressureSizeEnabled();
        if (ImGui::Checkbox("Use Pressure for Brush Size", &pressureSizeEnabled)) {
            g_tablet.setPressureSizeEnabled(pressureSizeEnabled);
        }
        
        bool pressureCursorEnabled = g_tablet.isPressureCursorEnabled();
        if (ImGui::Checkbox("Use Pressure for Cursor Dot", &pressureCursorEnabled)) {
            g_tablet.setPressureCursorEnabled(pressureCursorEnabled);
        }
        
        bool tiltEnabled = g_tablet.isTiltEnabled();
        if (ImGui::Checkbox("Use Tilt for Sculpting", &tiltEnabled)) {
            g_tablet.setTiltEnabled(tiltEnabled);
        }
        
        ImGui::Separator();
        ImGui::Text("Live Pressure Test (Draw below):");
        
        struct TestPoint {
            ImVec2 pos;
            float pressure;
            bool isStart;
        };
        static std::vector<TestPoint> testPoints;
        static bool lastPenDown = false;
        
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImVec2(280.0f, 100.0f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        
        drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), IM_COL32(15, 15, 18, 255));
        drawList->AddRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), IM_COL32(70, 70, 80, 255));
        
        ImGui::InvisibleButton("canvas", canvasSize);
        bool isHovered = ImGui::IsItemHovered();
        
        if (isHovered && diag.isPenDown) {
            ImVec2 mPos = ImGui::GetIO().MousePos;
            bool isStart = !lastPenDown;
            testPoints.push_back({ mPos, diag.currentPressure, isStart });
            if (testPoints.size() > 500) {
                testPoints.erase(testPoints.begin());
            }
        }
        lastPenDown = diag.isPenDown;
        
        if (testPoints.size() > 1) {
            for (size_t i = 1; i < testPoints.size(); ++i) {
                if (testPoints[i].isStart) continue;
                ImVec2 p1 = testPoints[i-1].pos;
                ImVec2 p2 = testPoints[i].pos;
                
                if (p1.x >= canvasPos.x && p1.x <= canvasPos.x + canvasSize.x &&
                    p1.y >= canvasPos.y && p1.y <= canvasPos.y + canvasSize.y &&
                    p2.x >= canvasPos.x && p2.x <= canvasPos.x + canvasSize.x &&
                    p2.y >= canvasPos.y && p2.y <= canvasPos.y + canvasSize.y) {
                    
                    float thickness = testPoints[i].pressure * 10.0f;
                    if (thickness < 1.0f) thickness = 1.0f;
                    
                    drawList->AddLine(p1, p2, IM_COL32(0, 192, 255, 255), thickness);
                }
            }
        }
        
        if (ImGui::Button("Clear Test Canvas")) {
            testPoints.clear();
        }
        
        ImGui::End();
    }
#endif

    if (sculpt.getBrush() == BRUSH_MASK_GRADIENT_BLUR && sculpt.getGradActive()) {
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        ImVec2 pA = ImVec2(sculpt.getGradPointA().x, sculpt.getGradPointA().y);
        ImVec2 pB = ImVec2(sculpt.getGradPointB().x, sculpt.getGradPointB().y);

        float dx = pB.x - pA.x;
        float dy = pB.y - pA.y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len > 0.0f) {
            float step = 10.0f;
            int numSteps = (int)(len / step);
            float ux = dx / len;
            float uy = dy / len;
            for (int i = 0; i < numSteps; ++i) {
                float tStart = i * step;
                float tEnd = tStart + 5.0f;
                if (tEnd > len) tEnd = len;
                drawList->AddLine(
                    ImVec2(pA.x + ux * tStart, pA.y + uy * tStart),
                    ImVec2(pA.x + ux * tEnd, pA.y + uy * tEnd),
                    IM_COL32(0, 229, 255, 255),
                    2.0f
                );
            }
        }

        ImVec2 mousePos = ImGui::GetMousePos();
        float distA = std::sqrt((mousePos.x - pA.x) * (mousePos.x - pA.x) + (mousePos.y - pA.y) * (mousePos.y - pA.y));
        float distB = std::sqrt((mousePos.x - pB.x) * (mousePos.x - pB.x) + (mousePos.y - pB.y) * (mousePos.y - pB.y));

        float radA = (distA < 20.0f) ? 12.0f : 8.0f;
        float radB = (distB < 20.0f) ? 12.0f : 8.0f;

        drawList->AddCircleFilled(pA, radA, IM_COL32(255, 255, 255, 255));
        drawList->AddCircle(pA, radA, IM_COL32(0, 229, 255, 255), 0, 2.0f);

        drawList->AddCircleFilled(pB, radB, IM_COL32(0, 229, 255, 255));
        drawList->AddCircle(pB, radB, IM_COL32(255, 255, 255, 255), 0, 2.0f);
    }

    // 10. Measure / Divider Overlays
    if (sculpt.getBrush() == BRUSH_MEASURE || sculpt.getBrush() == BRUSH_DIVIDER) {
        sculpt.validateSegments(scene);
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        const Camera& camera = scene.getCamera();
        bool useDistanceThickness = sculpt.getMeasureUseDistanceThickness();

        auto getPixelsPerUnit = [](const glm::vec3& worldPos, const Camera& camera) -> float {
            glm::mat4 view = camera.getViewMatrix();
            glm::vec3 right(view[0][0], view[1][0], view[2][0]);
            glm::vec3 offsetPos = worldPos + right * 1.0f;
            glm::vec3 screenPos = camera.project(worldPos);
            glm::vec3 screenOffsetPos = camera.project(offsetPos);
            return glm::length(glm::vec2(screenPos.x - screenOffsetPos.x, screenPos.y - screenOffsetPos.y));
        };

        auto drawEndpointShape = [](ImDrawList* drawList, ImVec2 center, float r, MeasurementAnchor::Type type, ImU32 fillColor, ImU32 strokeColor, float strokeWidth) {
            if (type == MeasurementAnchor::VERTEX) {
                drawList->AddCircleFilled(center, r, fillColor);
                drawList->AddCircle(center, r, strokeColor, 0, strokeWidth);
            } else {
                ImVec2 pts[4] = {
                    ImVec2(center.x, center.y - r),
                    ImVec2(center.x + r, center.y),
                    ImVec2(center.x, center.y + r),
                    ImVec2(center.x - r, center.y)
                };
                drawList->AddConvexPolyFilled(pts, 4, fillColor);
                drawList->AddPolyline(pts, 4, strokeColor, ImDrawFlags_Closed, strokeWidth);
            }
        };

        const MeasurementSegment* hoveredSeg = sculpt.getHoveredSegment();
        std::string hoveredKey = sculpt.getHoveredVertexKey();

        bool isDivider = (sculpt.getBrush() == BRUSH_DIVIDER);
        int divisions = sculpt.getDividerDivisions();

        // Find reference length for Measure
        float referenceLength = 0.0f;
        const auto& segments = isDivider ? sculpt.getDividerSegments() : sculpt.getMeasureSegments();
        if (!isDivider) {
            for (const auto& s : segments) {
                if (s.isReference) {
                    glm::vec3 worldA = SculptManager::getAnchorWorldPos(s.vertA);
                    glm::vec3 worldB = SculptManager::getAnchorWorldPos(s.vertB);
                    referenceLength = glm::distance(worldA, worldB);
                    break;
                }
            }
        }

        auto drawSeg = [&](const MeasurementSegment& seg, bool isReference, bool isPreview) {
            glm::vec3 worldA = SculptManager::getAnchorWorldPos(seg.vertA);
            glm::vec3 worldB = SculptManager::getAnchorWorldPos(seg.vertB);

            glm::vec3 screenA = camera.project(worldA);
            glm::vec3 screenB = camera.project(worldB);
            ImVec2 posA(screenA.x, screenA.y);
            ImVec2 posB(screenB.x, screenB.y);

            float worldDist = glm::distance(worldA, worldB);

            bool isHoveredA = (&seg == hoveredSeg && hoveredKey == "vertA");
            bool isHoveredB = (&seg == hoveredSeg && hoveredKey == "vertB");

            ImU32 color = isReference ? IM_COL32(255, 255, 255, 255) : IM_COL32(176, 190, 197, 255);
            if (isPreview) {
                color = isReference ? IM_COL32(255, 255, 255, 150) : IM_COL32(176, 190, 197, 150);
            }

            float strokeWidth = isReference ? 1.5f : 1.0f;
            float rA = isReference ? 5.0f : 4.0f;
            float rB = isReference ? 5.0f : 4.0f;
            float rDiv = 3.5f;

            if (useDistanceThickness) {
                glm::vec3 worldMid = (worldA + worldB) * 0.5f;
                float ppuMid = getPixelsPerUnit(worldMid, camera);
                strokeWidth = (isReference ? 0.11f : 0.075f) * ppuMid;
                strokeWidth = glm::clamp(strokeWidth, 0.25f, 5.0f);

                float ppuA = getPixelsPerUnit(worldA, camera);
                rA = (isReference ? 0.35f : 0.28f) * ppuA;
                rA = glm::clamp(rA, 1.0f, 15.0f);

                float ppuB = getPixelsPerUnit(worldB, camera);
                rB = (isReference ? 0.35f : 0.28f) * ppuB;
                rB = glm::clamp(rB, 1.0f, 15.0f);

                rDiv = 0.2f * ppuMid;
                rDiv = glm::clamp(rDiv, 0.8f, 10.0f);
            }

            if (isHoveredA) rA = std::max(8.0f, rA * 1.6f);
            if (isHoveredB) rB = std::max(8.0f, rB * 1.6f);

            // 1. Line
            if (isPreview) {
                ImVec2 d = ImVec2(posB.x - posA.x, posB.y - posA.y);
                float len = std::sqrt(d.x * d.x + d.y * d.y);
                if (len > 0.0f) {
                    float step = 10.0f;
                    int numSteps = (int)(len / step);
                    float ux = d.x / len;
                    float uy = d.y / len;
                    for (int i = 0; i < numSteps; ++i) {
                        float tStart = i * step;
                        float tEnd = tStart + 5.0f;
                        if (tEnd > len) tEnd = len;
                        drawList->AddLine(
                            ImVec2(posA.x + ux * tStart, posA.y + uy * tStart),
                            ImVec2(posA.x + ux * tEnd, posA.y + uy * tEnd),
                            color,
                            strokeWidth
                        );
                    }
                }
            } else {
                drawList->AddLine(posA, posB, color, strokeWidth);
            }

            // 2. Division marks or ticks
            if (isDivider) {
                if (divisions >= 2 && divisions <= 6) {
                    for (int k = 1; k < divisions; ++k) {
                        float t = (float)k / (float)divisions;
                        glm::vec3 divWorld = glm::mix(worldA, worldB, t);
                        glm::vec3 divScreen = camera.project(divWorld);
                        ImVec2 divPos(divScreen.x, divScreen.y);
                        ImU32 fillCol = isPreview ? IM_COL32(255, 255, 255, 102) : IM_COL32(255, 255, 255, 255);
                        ImU32 strokeCol = isPreview ? IM_COL32(26, 26, 26, 102) : IM_COL32(26, 26, 26, 255);
                        drawList->AddCircleFilled(divPos, rDiv, fillCol);
                        drawList->AddCircle(divPos, rDiv, strokeCol, 0, 1.0f);
                    }
                }
            } else {
                if (!isReference && referenceLength > 0.0f) {
                    int nTicks = (int)std::floor((worldDist - 1e-5f) / referenceLength);
                    for (int k = 1; k <= nTicks; ++k) {
                        float t = (k * referenceLength) / worldDist;
                        glm::vec3 tickWorld = glm::mix(worldA, worldB, t);
                        glm::vec3 tickScreen = camera.project(tickWorld);
                        ImVec2 tickPos(tickScreen.x, tickScreen.y);
                        drawList->AddCircleFilled(tickPos, 2.5f, IM_COL32(255, 255, 255, 255));
                        drawList->AddCircle(tickPos, 2.5f, color, 0, 1.0f);
                    }
                }
            }

            // 3. Endpoint shapes
            ImU32 strokeA = isHoveredA ? IM_COL32(0, 229, 255, 255) : IM_COL32(26, 26, 26, 255);
            float swA = isHoveredA ? 2.5f : 1.2f;
            drawEndpointShape(drawList, posA, rA, seg.vertA.type, color, strokeA, swA);

            ImU32 strokeB = isHoveredB ? IM_COL32(0, 229, 255, 255) : IM_COL32(26, 26, 26, 255);
            float swB = isHoveredB ? 2.5f : 1.2f;
            drawEndpointShape(drawList, posB, rB, seg.vertB.type, color, strokeB, swB);

            // 4. Text Label (Measure tool only)
            if (!isDivider) {
                char label[32];
                if (isReference) {
                    strcpy(label, "1.00x");
                } else {
                    if (referenceLength > 0.0f) {
                        sprintf(label, "%.2fx", worldDist / referenceLength);
                    } else {
                        sprintf(label, "%.2f", worldDist);
                    }
                }

                ImVec2 labelSize = ImGui::CalcTextSize(label);
                float textWidth = labelSize.x + 12.0f;
                float textHeight = labelSize.y + 6.0f;

                ImVec2 midPos((posA.x + posB.x) * 0.5f, (posA.y + posB.y) * 0.5f - 10.0f);
                ImVec2 minRect(midPos.x - textWidth * 0.5f, midPos.y - textHeight * 0.5f);
                ImVec2 maxRect(midPos.x + textWidth * 0.5f, midPos.y + textHeight * 0.5f);

                drawList->AddRectFilled(minRect, maxRect, IM_COL32(20, 20, 20, 217), 4.0f);
                drawList->AddRect(minRect, maxRect, color, 4.0f, 0, 1.0f);

                ImVec2 textPos(midPos.x - labelSize.x * 0.5f, midPos.y - labelSize.y * 0.5f);
                drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), label);
            }
        };

        // Draw completed segments
        for (const auto& s : segments) {
            drawSeg(s, !isDivider && s.isReference, false);
        }

        // Draw pending/preview
        if (sculpt.hasPending()) {
            MeasurementSegment pendingSeg;
            pendingSeg.vertA = sculpt.getPendingAnchorA();
            pendingSeg.vertB = sculpt.getPendingAnchorB();
            bool isPendingRef = true;
            if (!isDivider) {
                bool hasRef = false;
                for (const auto& s : segments) {
                    if (s.isReference) { hasRef = true; break; }
                }
                isPendingRef = !hasRef;
            }
            drawSeg(pendingSeg, isPendingRef, true);
        }
    }

    // 11. Transform Gizmo (ImGuizmo)
    if (sculpt.getBrush() == BRUSH_TRANSFORM) {
        Mesh* selectedMesh = scene.getSelected();
        const Camera& camera = scene.getCamera();
        if (selectedMesh) {
            ImGuizmo::BeginFrame();
            ImGuizmo::SetOrthographic(camera.isOrthographic());
            ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
            ImGuizmo::SetRect(0.0f, 0.0f, (float)camera.getWidth(), (float)camera.getHeight());

            bool cameraDragging = sculpt.getCameraController().isDragging();
            ImGuizmo::Enable(!cameraDragging);
            ImGuizmo::SetGizmoSizeClipSpace(0.20f);

            glm::mat4 view = camera.getViewMatrix();
            glm::mat4 proj = camera.getProjMatrix();
            glm::mat4 matrix = selectedMesh->matrix;

            static bool wasUsingGizmo = false;
            static bool draggedPivot = false;
            static glm::mat4 pivotStartMatrix = glm::mat4(1.0f);

            bool isUsingGizmo = ImGuizmo::IsUsing();
            bool isMovingPivot = m_editPivot || ImGui::GetIO().KeyAlt;

            if (isUsingGizmo && !wasUsingGizmo) {
                scene.pushHistoryState();
                pivotStartMatrix = selectedMesh->matrix;
                draggedPivot = isMovingPivot;
            }

            ImGuizmo::OPERATION op = draggedPivot ? (ImGuizmo::TRANSLATE | ImGuizmo::ROTATE) : ImGuizmo::UNIVERSAL;

            // Apply custom styling matching legacy JS colors and sizes
            ImGuizmo::Style& style = ImGuizmo::GetStyle();
            style.Colors[ImGuizmo::DIRECTION_X] = ImVec4(0.7f, 0.2f, 0.2f, 1.0f);
            style.Colors[ImGuizmo::DIRECTION_Y] = ImVec4(0.2f, 0.7f, 0.2f, 1.0f);
            style.Colors[ImGuizmo::DIRECTION_Z] = ImVec4(0.2f, 0.2f, 0.7f, 1.0f);
            style.Colors[ImGuizmo::PLANE_X] = ImVec4(0.7f, 0.2f, 0.2f, 0.38f);
            style.Colors[ImGuizmo::PLANE_Y] = ImVec4(0.2f, 0.7f, 0.2f, 0.38f);
            style.Colors[ImGuizmo::PLANE_Z] = ImVec4(0.2f, 0.2f, 0.7f, 0.38f);
            style.Colors[ImGuizmo::SELECTION] = ImVec4(1.0f, 1.0f, 0.4f, 1.0f);
            style.Colors[ImGuizmo::INACTIVE] = ImVec4(0.4f, 0.4f, 0.4f, 0.54f);
            style.TranslationLineThickness = 3.5f;
            style.TranslationLineArrowSize = 10.0f;
            style.RotationLineThickness = 3.5f;
            style.RotationOuterLineThickness = 4.0f;
            style.ScaleLineThickness = 3.5f;
            style.ScaleLineCircleSize = 9.0f;
            style.CenterCircleSize = 9.0f;

            // Draw and manipulate gizmo
            if (ImGuizmo::Manipulate(
                glm::value_ptr(view), glm::value_ptr(proj),
                op, ImGuizmo::LOCAL, glm::value_ptr(matrix)
            )) {
                if (draggedPivot) {
                    glm::mat4 deltaLocalMatrix = glm::inverse(pivotStartMatrix) * matrix;
                    glm::mat4 deltaLocalMatrixInv = glm::inverse(deltaLocalMatrix);
                    selectedMesh->matrix = matrix;
                    selectedMesh->editMatrix = deltaLocalMatrixInv;
                    selectedMesh->enMatrix = glm::transpose(glm::inverse(glm::mat3(deltaLocalMatrixInv)));
                } else {
                    selectedMesh->matrix = matrix;
                }
                selectedMesh->isDirty = true;
            }

            if (!isUsingGizmo && wasUsingGizmo) {
                if (draggedPivot) {
                    if (selectedMesh->editMatrix != glm::mat4(1.0f)) {
                        for (int i = 0; i < selectedMesh->nbVerts; ++i) {
                            glm::vec4 pos(selectedMesh->verts[i * 3], selectedMesh->verts[i * 3 + 1], selectedMesh->verts[i * 3 + 2], 1.0f);
                            glm::vec4 newPos = selectedMesh->editMatrix * pos;
                            selectedMesh->verts[i * 3]     = newPos.x;
                            selectedMesh->verts[i * 3 + 1] = newPos.y;
                            selectedMesh->verts[i * 3 + 2] = newPos.z;

                            glm::vec3 normal(selectedMesh->normals[i * 3], selectedMesh->normals[i * 3 + 1], selectedMesh->normals[i * 3 + 2]);
                            glm::vec3 newNormal = glm::normalize(selectedMesh->enMatrix * normal);
                            selectedMesh->normals[i * 3]     = newNormal.x;
                            selectedMesh->normals[i * 3 + 1] = newNormal.y;
                            selectedMesh->normals[i * 3 + 2] = newNormal.z;
                        }
                        selectedMesh->editMatrix = glm::mat4(1.0f);
                        selectedMesh->enMatrix = glm::mat3(1.0f);
                        selectedMesh->postInit();
                        selectedMesh->isDirty = true;
                    }
                }
                draggedPivot = false;
            }
            wasUsingGizmo = isUsingGizmo;

            // Draw floating pivot lock button near the gizmo center
            glm::vec3 pivotWorldPos = glm::vec3(selectedMesh->matrix[3]);
            glm::vec3 screenPos = camera.project(pivotWorldPos);

            if (screenPos.z >= 0.0f && screenPos.z <= 1.0f &&
                screenPos.x >= 0.0f && screenPos.x <= (float)camera.getWidth() &&
                screenPos.y >= 0.0f && screenPos.y <= (float)camera.getHeight()) {
                
                ImGui::SetNextWindowPos(ImVec2(screenPos.x - 45, screenPos.y - 45), ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.7f);
                ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | 
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
                if (ImGui::Begin("##PivotLockWindow", nullptr, flags)) {
                    bool activeMoving = m_editPivot || ImGui::GetIO().KeyAlt;
                    if (activeMoving) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.83f, 0.18f, 0.18f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.25f, 0.25f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.70f, 0.10f, 0.10f, 1.0f));
                        if (ImGui::Button(" Unlock Pivot ")) {
                            m_editPivot = false;
                        }
                        ImGui::PopStyleColor(3);
                    } else {
                        if (ImGui::Button(" Lock Pivot ")) {
                            m_editPivot = true;
                        }
                    }
                }
                ImGui::End();
            }
        }
    }

    drawRemeshProgressModal();

    // Render brush cursor using ImGui foreground draw list for beautiful antialiased lines
    if (renderer.getSmoothCursor()) {
        const auto& cursorState = sculpt.getCursor().getState();
        if (cursorState.visible && !ImGui::GetIO().WantCaptureMouse) {
            float viewportWidth = ImGui::GetIO().DisplaySize.x;
            float viewportHeight = ImGui::GetIO().DisplaySize.y;
            float leftViewportWidth = viewportWidth;
            float leftViewportX = 0.0f;
            
            if (renderer.getSplitMode()) {
                leftViewportWidth = viewportWidth * 0.5f;
            }

            auto projectPoint = [](const glm::mat4& mvp, const glm::vec3& localPos, float width, float height, float xOffset, const Camera& camera, bool apply2D) -> ImVec2 {
                glm::vec4 clipPos = mvp * glm::vec4(localPos, 1.0f);
                if (clipPos.w == 0.0f) return ImVec2(0.0f, 0.0f);
                glm::vec3 ndcPos = glm::vec3(clipPos) / clipPos.w;
                if (apply2D && camera.getRef2DMode()) {
                    ndcPos.x = ndcPos.x * camera.getView2DZoom() + camera.getView2DOffsetX();
                    ndcPos.y = ndcPos.y * camera.getView2DZoom() + camera.getView2DOffsetY();
                }
                float sx = (ndcPos.x * 0.5f + 0.5f) * width + xOffset;
                float sy = (0.5f - ndcPos.y * 0.5f) * height;
                return ImVec2(sx, sy);
            };

            ImU32 colorU32 = ImGui::ColorConvertFloat4ToU32(ImVec4(cursorState.color.r, cursorState.color.g, cursorState.color.b, 1.0f));
            float thickness = renderer.getCursorThickness();

            auto drawViewportCursor = [&](bool isRight, float xOffset, float width) {
                const Camera& camera = isRight ? *scene.getCameraByIndex(1) : scene.getCamera();
                ImGui::GetForegroundDrawList()->PushClipRect(
                    ImVec2(xOffset, 0.0f),
                    ImVec2(xOffset + width, viewportHeight),
                    true
                );

                const glm::mat4& circleMVP = isRight ? cursorState.circleMVPRight : cursorState.circleMVP;
                const glm::mat4& innerCircleMVP = isRight ? cursorState.innerCircleMVPRight : cursorState.innerCircleMVP;
                const glm::mat4& dotMVP = isRight ? cursorState.dotMVPRight : cursorState.dotMVP;
                const std::vector<glm::mat4>& symMVPs = isRight ? cursorState.symMVPsRight : cursorState.symMVPs;
                const std::vector<char>& symOccluded = isRight ? cursorState.symOccludedRight : cursorState.symOccluded;

                bool apply2D = !cursorState.isScreenspace;

                if (cursorState.showCircle) {
                    // Draw outer circle
                    const int numSegments = 64;
                    std::vector<ImVec2> outerPoints(numSegments);
                    for (int i = 0; i < numSegments; ++i) {
                        float angle = i * 2.0f * 3.1415926535f / numSegments;
                        glm::vec3 localPos(std::cos(angle), std::sin(angle), 0.0f);
                        outerPoints[i] = projectPoint(circleMVP, localPos, width, viewportHeight, xOffset, camera, apply2D);
                    }
                    ImGui::GetForegroundDrawList()->AddPolyline(outerPoints.data(), numSegments, colorU32, ImDrawFlags_Closed, thickness);

                    // Draw inner circle
                    std::vector<ImVec2> innerPoints(numSegments);
                    for (int i = 0; i < numSegments; ++i) {
                        float angle = i * 2.0f * 3.1415926535f / numSegments;
                        glm::vec3 localPos(std::cos(angle), std::sin(angle), 0.0f);
                        innerPoints[i] = projectPoint(innerCircleMVP, localPos, width, viewportHeight, xOffset, camera, apply2D);
                    }
                    ImGui::GetForegroundDrawList()->AddPolyline(innerPoints.data(), numSegments, colorU32, ImDrawFlags_Closed, thickness);
                }

                // Draw main dot (filled circle)
                const int dotSegments = 32;
                std::vector<ImVec2> dotPoints(dotSegments);
                for (int i = 0; i < dotSegments; ++i) {
                    float angle = i * 2.0f * 3.1415926535f / dotSegments;
                    glm::vec3 localPos(std::cos(angle), std::sin(angle), 0.0f);
                    dotPoints[i] = projectPoint(dotMVP, localPos, width, viewportHeight, xOffset, camera, apply2D);
                }
                ImGui::GetForegroundDrawList()->AddConvexPolyFilled(dotPoints.data(), dotSegments, colorU32);

                // Draw symmetry dots
                for (size_t idx = 0; idx < symMVPs.size(); ++idx) {
                    const auto& symMVP = symMVPs[idx];
                    bool occluded = (idx < symOccluded.size()) ? symOccluded[idx] : false;

                    // Project center of symmetry dot to check if it's covered by an ImGui panel
                    ImVec2 symCenter = projectPoint(symMVP, glm::vec3(0.0f), width, viewportHeight, xOffset, camera, apply2D);
                    if (isPointOverImGuiWindow(symCenter)) {
                        continue;
                    }

                    std::vector<ImVec2> symPoints(dotSegments);
                    for (int i = 0; i < dotSegments; ++i) {
                        float angle = i * 2.0f * 3.1415926535f / dotSegments;
                        glm::vec3 localPos(std::cos(angle), std::sin(angle), 0.0f);
                        symPoints[i] = projectPoint(symMVP, localPos, width, viewportHeight, xOffset, camera, apply2D);
                    }
                    
                    ImU32 dotColorU32 = colorU32;
                    if (occluded) {
                        glm::vec3 darkColor = cursorState.color * 0.3f;
                        dotColorU32 = ImGui::ColorConvertFloat4ToU32(ImVec4(darkColor.r, darkColor.g, darkColor.b, 1.0f));
                    }
                    ImGui::GetForegroundDrawList()->AddConvexPolyFilled(symPoints.data(), dotSegments, dotColorU32);
                }

                ImGui::GetForegroundDrawList()->PopClipRect();
            };

            if (!renderer.getSplitMode()) {
                drawViewportCursor(false, 0.0f, viewportWidth);
            } else {
                int activeVp = scene.getActiveViewport();
                bool showInactive = scene.getSplitShowInactiveCursor();
                float halfW = viewportWidth * 0.5f;

                if (activeVp == 0 || showInactive) {
                    drawViewportCursor(false, 0.0f, halfW);
                }
                if (activeVp == 1 || showInactive) {
                    drawViewportCursor(true, halfW, halfW);
                }
            }
        }
    }

    // Draw Camera Pivot Point if enabled and actively orbiting
    const Camera& camera = scene.getCamera();
    bool isOrbiting = (sculpt.getCameraController().getDragMode() == CameraController::DragMode::Orbit || sculpt.getCameraController().getDragMode() == CameraController::DragMode::Roll);
    if (camera.getUsePivot() && isOrbiting) {
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        glm::vec3 pivotWorld = camera.getPivot();
        
        ImU32 pivotColor = IM_COL32(230, 50, 50, 240);
        
        auto drawPivotMarker = [&](ImVec2 p) {
            if (isPointOverImGuiWindow(p)) return;

            // Draw center dot
            drawList->AddCircleFilled(p, 2.0f, pivotColor);
            // Draw outer ring
            drawList->AddCircle(p, 6.0f, pivotColor, 0, 1.0f);
            // Draw crosshair ticks
            drawList->AddLine(ImVec2(p.x - 10.0f, p.y), ImVec2(p.x - 6.0f, p.y), pivotColor, 1.0f);
            drawList->AddLine(ImVec2(p.x + 6.0f, p.y), ImVec2(p.x + 10.0f, p.y), pivotColor, 1.0f);
            drawList->AddLine(ImVec2(p.x, p.y - 10.0f), ImVec2(p.x, p.y - 6.0f), pivotColor, 1.0f);
            drawList->AddLine(ImVec2(p.x, p.y + 6.0f), ImVec2(p.x, p.y + 10.0f), pivotColor, 1.0f);
        };

        if (!renderer.getSplitMode()) {
            glm::vec3 screenPos = camera.project(pivotWorld);
            if (screenPos.z >= 0.0f && screenPos.z <= 1.0f) {
                drawPivotMarker(ImVec2(screenPos.x, screenPos.y));
            }
        } else {
            int activeVp = scene.getActiveViewport();
            float w2 = ImGui::GetIO().DisplaySize.x * 0.5f;

            if (activeVp == 0) {
                glm::vec3 screenPosLeft = camera.project(pivotWorld);
                if (screenPosLeft.z >= 0.0f && screenPosLeft.z <= 1.0f) {
                    drawPivotMarker(ImVec2(screenPosLeft.x, screenPosLeft.y));
                }
            } else if (activeVp == 1) {
                glm::vec3 screenPosRight = camera.project(pivotWorld);
                if (screenPosRight.z >= 0.0f && screenPosRight.z <= 1.0f) {
                    drawPivotMarker(ImVec2(screenPosRight.x + w2, screenPosRight.y));
                }
            }
        }
    }

    if (m_activeModalMode != ModalMode::NONE) {
        drawModalIndicatorHUD(sculpt, scene);
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void GuiManager::performRemesh(Scene& scene) {
    if (m_remeshAsync.state == RemeshState::Running) {
        return;
    }

    Mesh* selectedMesh = scene.getSelected();
    if (!selectedMesh) return;

    scene.pushHistoryState();

    // Snapshot mesh data for worker thread
    auto verts = selectedMesh->verts;
    auto faces = MeshUtils::triangulate(*selectedMesh);
    auto colors = selectedMesh->colors;
    auto materials = selectedMesh->materials;
    int nbVerts = selectedMesh->nbVerts;
    float bbox[6];
    selectedMesh->computeBbox(bbox);
    float resolution = (float)m_remeshResolution;
    
    float uniformColor[3] = { 0.72f, 0.52f, 0.45f };
    float uniformMaterial[3] = { 0.5f, 0.0f, 1.0f };
    if (m_renderer) {
        uniformColor[0] = m_renderer->getAlbedo()[0];
        uniformColor[1] = m_renderer->getAlbedo()[1];
        uniformColor[2] = m_renderer->getAlbedo()[2];
        uniformMaterial[0] = m_renderer->getRoughness();
        uniformMaterial[1] = m_renderer->getMetallic();
    }

    m_remeshAsync.state = RemeshState::Running;
    m_remeshAsync.stage = 0;
    m_remeshAsync.progress = 0;

    std::thread([this,
        verts = std::move(verts),
        faces = std::move(faces),
        colors = std::move(colors),
        materials = std::move(materials),
        nbVerts,
        bboxArr = std::array<float,6>{bbox[0],bbox[1],bbox[2],bbox[3],bbox[4],bbox[5]},
        resolution,
        uniColorArr = std::array<float,3>{uniformColor[0], uniformColor[1], uniformColor[2]},
        uniMatArr = std::array<float,3>{uniformMaterial[0], uniformMaterial[1], uniformMaterial[2]}
    ]() mutable
    {
        try {
            bool hasColors = !colors.empty();
            bool hasMaterials = !materials.empty();
            auto result = doRemesh(
                verts.data(), nbVerts,
                faces.data(), faces.size() / 3,
                hasColors ? colors.data() : nullptr,
                hasMaterials ? materials.data() : nullptr,
                bboxArr.data(),
                resolution,
                false, // block
                false, // smooth
                false, // manifold (Marching Cubes) -> false uses Surface Nets (quads)
                uniColorArr.data(),
                uniMatArr.data(),
                hasColors,
                hasMaterials,
                [this](int stage, int pct) {
                    m_remeshAsync.stage = stage;
                    m_remeshAsync.progress = pct;
                }
            );
            m_remeshAsync.result = std::move(result);
            m_remeshAsync.state = RemeshState::Done;
        } catch (...) {
            m_remeshAsync.state = RemeshState::Error;
        }
    }).detach();
}

void GuiManager::applyRemeshResult(Scene& scene, const RemeshResult& r) {
    Mesh* selectedMesh = scene.getSelected();
    if (!selectedMesh) return;

    selectedMesh->verts = r.vertices;
    selectedMesh->faces = r.faces;
    selectedMesh->colors = r.colors;
    selectedMesh->materials = r.materials;
    selectedMesh->nbVerts = r.vertices.size() / 3;
    selectedMesh->nbFaces = r.faces.size() / 4;

    std::vector<uint32_t> vrvStartCount;
    std::vector<uint32_t> vertRingVert;
    std::vector<uint32_t> vrfStartCount;
    std::vector<uint32_t> vertRingFace;
    std::vector<uint8_t> vertOnEdge;
    computeTopology(
        selectedMesh->nbVerts,
        selectedMesh->faces.data(),
        selectedMesh->nbFaces,
        vrfStartCount,
        vertRingFace,
        vrvStartCount,
        vertRingVert,
        vertOnEdge
    );

    selectedMesh->vrfStartCount = vrfStartCount;
    selectedMesh->vertRingFace = vertRingFace;
    selectedMesh->vrvStartCount = vrvStartCount;
    selectedMesh->vertRingVert = vertRingVert;
    selectedMesh->vertOnEdge = vertOnEdge;

    selectedMesh->postInit();
    selectedMesh->isDirty = true;
}

void GuiManager::drawRemeshProgressModal() {
    if (m_remeshAsync.state != RemeshState::Running) {
        return;
    }

    ImGui::OpenPopup("Remeshing...");

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(300, 120));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar;

    if (ImGui::BeginPopupModal("Remeshing...", nullptr, flags)) {
        int stage = m_remeshAsync.stage.load();
        int progress = m_remeshAsync.progress.load();

        const char* label = "Unknown Stage";
        if (stage == 0) label = "Voxelizing geometry...";
        else if (stage == 1) label = "Flood-filling interior...";
        else if (stage == 2) label = "Reconstructing surface...";

        ImGui::Text("%s", label);
        ImGui::Separator();
        
        float progressFloat = (float)progress / 100.0f;
        char buf[32];
        sprintf(buf, "%d%%", progress);
        ImGui::ProgressBar(progressFloat, ImVec2(-1.0f, 26.0f), buf);

        ImGui::EndPopup();
    }
}

void GuiManager::drawModalIndicatorHUD(SculptManager& sculpt, Scene& scene) {
    const char* label = nullptr;
    char valStr[64] = "";
    float fraction = 0.0f;

    switch (m_activeModalMode) {
        case ModalMode::INTENSITY:
            label = "Intensity";
            snprintf(valStr, sizeof(valStr), "%d%%", (int)(sculpt.getBrushIntensity() * 100.0f));
            fraction = sculpt.getBrushIntensity();
            break;
        case ModalMode::FOCAL_SHIFT:
            label = "Focal Shift";
            snprintf(valStr, sizeof(valStr), "%d%%", (int)(sculpt.getFocalShift() * 100.0f));
            fraction = (sculpt.getFocalShift() + 1.0f) * 0.5f;
            break;
        case ModalMode::RADIUS:
            label = "Radius";
            snprintf(valStr, sizeof(valStr), "%d px", (int)sculpt.getBrushRadius());
            fraction = (sculpt.getBrushRadius() - 0.5f) / (250.0f - 0.5f);
            break;
        case ModalMode::REMESH_RESOLUTION:
            label = "Remesh Resolution";
            snprintf(valStr, sizeof(valStr), "%d", m_remeshResolution);
            fraction = (float)(m_remeshResolution - 10) / (1000 - 10);
            break;
        case ModalMode::TOPOLOGY_DETAIL:
            label = "Topology Detail";
            snprintf(valStr, sizeof(valStr), "%d", (int)m_dyntopoDetail);
            fraction = (m_dyntopoDetail - 1.0f) / (500.0f - 1.0f);
            break;
        case ModalMode::CAMERA_FOV:
            label = "FOV";
            snprintf(valStr, sizeof(valStr), "%d mm", (int)scene.getCamera().getFov());
            fraction = (scene.getCamera().getFov() - 10.0f) / (200.0f - 10.0f);
            break;
        default:
            return;
    }

    if (!label) return;

    // Center horizontally (-50% pivot) and place slightly above mouse cursor (-100% pivot)
    ImGui::SetNextWindowPos(ImVec2((float)m_modalStartMouseX, (float)m_modalStartMouseY - 25.0f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);

    // Style overrides for floating indicator card
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 6.0f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | 
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | 
                             ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::Begin("##ModalIndicatorHUD", nullptr, flags)) {
        float width = 150.0f; // matches min-width of 150px
        
        float posX = ImGui::GetCursorPosX();
        ImGui::Text("%s", label);
        float valWidth = ImGui::CalcTextSize(valStr).x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(posX + width - valWidth);
        ImGui::Text("%s", valStr);

        // Render sleek progress bar
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.01f, 0.52f, 0.45f, 1.00f)); // Teal Accent
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 1.0f, 0.2f)); // Track
        ImGui::ProgressBar(std::max(0.0f, std::min(1.0f, fraction)), ImVec2(width, 5.0f), "");
        ImGui::PopStyleColor(2);
    }
    ImGui::End();

    ImGui::PopStyleVar(3);
}


bool GuiManager::saveSettings(const std::string& filepath) {
    std::ofstream out(filepath);
    if (!out.is_open()) {
        std::cerr << "Failed to open GUI settings file for writing: " << filepath << std::endl;
        return false;
    }

    out << "[Panels]\n";
    out << "showToolbar=" << (m_showToolbar ? "true" : "false") << "\n";
    out << "showSculptingPanel=" << (m_showSculptingPanel ? "true" : "false") << "\n";
    out << "showScenePanel=" << (m_showScenePanel ? "true" : "false") << "\n";
    out << "showTopologyPanel=" << (m_showTopologyPanel ? "true" : "false") << "\n";
    out << "showFilesPanel=" << (m_showFilesPanel ? "true" : "false") << "\n";
    out << "showCameraPanel=" << (m_showCameraPanel ? "true" : "false") << "\n";
    out << "showRenderingPanel=" << (m_showRenderingPanel ? "true" : "false") << "\n";
    out << "showMaskingPanel=" << (m_showMaskingPanel ? "true" : "false") << "\n";
    out << "showMultiresPanel=" << (m_showMultiresPanel ? "true" : "false") << "\n";
    out << "showZSpheresPanel=" << (m_showZSpheresPanel ? "true" : "false") << "\n";
    out << "showReferenceImagesPanel=" << (m_showReferenceImagesPanel ? "true" : "false") << "\n";
    out << "showGizmoCube=" << (m_showGizmoCube ? "true" : "false") << "\n";
    out << "showMeshInfo=" << (m_showMeshInfo ? "true" : "false") << "\n";
    out << "showTabletDiagPanel=" << (m_showTabletDiagPanel ? "true" : "false") << "\n";

    std::cout << "Successfully saved GUI panel settings to: " << filepath << std::endl;
    return true;
}

bool GuiManager::loadSettings(const std::string& filepath) {
    std::ifstream in(filepath);
    if (!in.is_open()) {
        std::cerr << "Failed to open GUI settings file for reading: " << filepath << std::endl;
        return false;
    }

    std::string line;
    std::string currentSection = "";
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> sections;

    auto trimLocal = [](const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string("");
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    };

    while (std::getline(in, line)) {
        line = trimLocal(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        if (line[0] == '[' && line[line.size() - 1] == ']') {
            currentSection = trimLocal(line.substr(1, line.size() - 2));
            continue;
        }

        size_t eqPos = line.find('=');
        if (eqPos != std::string::npos && !currentSection.empty()) {
            std::string key = trimLocal(line.substr(0, eqPos));
            std::string val = trimLocal(line.substr(eqPos + 1));
            sections[currentSection][key] = val;
        }
    }

    auto itSection = sections.find("Panels");
    if (itSection != sections.end()) {
        const auto& params = itSection->second;
        auto getBoolParam = [&](const std::string& key, bool& outVal) {
            auto it = params.find(key);
            if (it != params.end()) {
                outVal = (it->second == "true" || it->second == "1");
            }
        };

        getBoolParam("showToolbar", m_showToolbar);
        getBoolParam("showSculptingPanel", m_showSculptingPanel);
        getBoolParam("showScenePanel", m_showScenePanel);
        getBoolParam("showTopologyPanel", m_showTopologyPanel);
        getBoolParam("showFilesPanel", m_showFilesPanel);
        getBoolParam("showCameraPanel", m_showCameraPanel);
        getBoolParam("showRenderingPanel", m_showRenderingPanel);
        getBoolParam("showMaskingPanel", m_showMaskingPanel);
        getBoolParam("showMultiresPanel", m_showMultiresPanel);
        getBoolParam("showZSpheresPanel", m_showZSpheresPanel);
        getBoolParam("showReferenceImagesPanel", m_showReferenceImagesPanel);
        getBoolParam("showGizmoCube", m_showGizmoCube);
        getBoolParam("showMeshInfo", m_showMeshInfo);
        getBoolParam("showTabletDiagPanel", m_showTabletDiagPanel);
    }

    std::cout << "Successfully loaded GUI panel settings from: " << filepath << std::endl;
    return true;
}

