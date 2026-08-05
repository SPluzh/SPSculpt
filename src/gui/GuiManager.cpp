#include <GLES3/gl3.h>
#include "gui/GuiManager.h"
#include "common/IniFile.h"
#include "mesh/Multimesh.h"
#include "common/Constants.h"
#include "common/Logger.h"
#include "render/AngleRenderer.h"
#include "render/RenderSettings.h"
#include "editing/BrushCursor.h"
#include "files/FileManager.h"
#include "platform/FileDialog.h"
#include "brushes/BrushPresetManager.h"
#include "editing/ArmatureTool.h"
#include "editing/undo/UndoManager.h"
#include <imgui.h>
#include "gui/IconsLucide.h"
#include "gui/lucide_font.h"
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
#include "common/Logger.h"
#include "../third_party/stb_image.h"
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"

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
        case BRUSH_ARMATURE_SPHERES: return "Armature Spheres";
        case BRUSH_BRUSH:     return "Brush";
        case BRUSH_POLYGROUP: return "PolyGroup";
        case BRUSH_CLIP_CURVE: return "Clip Curve";
    }
    return "Unknown";
}

GuiManager::GuiManager() {}

GuiManager::~GuiManager() {
    shutdown();
}

GLuint GuiManager::getIconTexture(const std::string& iconName) {
    auto it = m_iconCache.find(iconName);
    if (it != m_iconCache.end()) {
        return it->second;
    }

    int w = 0, h = 0, channels = 0;
    unsigned char* data = nullptr;

    std::vector<std::string> searchPaths = {
        "resources/icons/" + iconName + ".png",
        "../resources/icons/" + iconName + ".png",
        "dist/resources/icons/" + iconName + ".png",
        "build/resources/icons/" + iconName + ".png"
    };

    char* basePath = SDL_GetBasePath();
    if (basePath) {
        std::string base(basePath);
        SDL_free(basePath);
        searchPaths.push_back(base + "resources/icons/" + iconName + ".png");
        searchPaths.push_back(base + "../resources/icons/" + iconName + ".png");
        searchPaths.push_back(base + "../../resources/icons/" + iconName + ".png");
    }

    std::string foundPath = "";
    std::filesystem::file_time_type newestTime;
    bool foundAny = false;

    for (const auto& path : searchPaths) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            auto ftime = std::filesystem::last_write_time(path, ec);
            if (!ec && (!foundAny || ftime > newestTime)) {
                newestTime = ftime;
                foundPath = path;
                foundAny = true;
            }
        }
    }

    if (!foundPath.empty()) {
        data = stbi_load(foundPath.c_str(), &w, &h, &channels, 4);
    }

    GLuint texID = 0;
    if (data) {
        glGenTextures(1, &texID);
        glBindTexture(GL_TEXTURE_2D, texID);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glBindTexture(GL_TEXTURE_2D, 0);
        stbi_image_free(data);
    }

    m_iconCache[iconName] = texID;
    return texID;
}

GLuint GuiManager::getXrayIconTexture() {
    return getIconTexture("xray-outline");
}

void GuiManager::openScene(Scene& scene, SculptManager* sculpt) {
    std::string path = FileDialog::openFile(FileDialog::getImportFilters(), "Open File");
    if (!path.empty()) {
        snprintf(m_importPath, sizeof(m_importPath), "%s", path.c_str());
        scene.clear();
        auto newMeshes = FileManager::importFiles(path, &scene, m_renderer, sculpt);
        for (auto* mesh : newMeshes) {
            scene.addMesh(mesh);
        }
        if (!newMeshes.empty()) {
            scene.selectMesh(newMeshes.front());
            scene.pushHistoryState();
        }
        if (FileManager::getExtension(path) == "sgl") {
            m_currentScenePath = path;
        } else {
            m_currentScenePath.clear();
        }
        scene.setModified(false);
    }
}

void GuiManager::saveScene(Scene& scene, SculptManager* sculpt) {
    if (m_currentScenePath.empty()) {
        saveSceneAs(scene, sculpt);
    } else {
        if (FileManager::exportMeshes(m_currentScenePath, scene.getMeshes(), &scene, m_renderer, sculpt)) {
            scene.setModified(false);
        }
    }
}

void GuiManager::saveSceneAs(Scene& scene, SculptManager* sculpt) {
    static const std::vector<FileDialog::FilterSpec> sglFilters = {
        { "SculptGL Scene (*.sgl)", "*.sgl" },
        { "All Files (*.*)", "*.*" }
    };
    std::string path = FileDialog::saveFile(sglFilters, "sgl", "Save Scene As");
    if (!path.empty()) {
        m_currentScenePath = path;
        snprintf(m_exportPath, sizeof(m_exportPath), "%s", path.c_str());
        if (FileManager::exportMeshes(m_currentScenePath, scene.getMeshes(), &scene, m_renderer, sculpt)) {
            scene.setModified(false);
        }
    }
}

void GuiManager::updateWindowTitle(SDL_Window* window, bool isModified) {
    if (!window) return;
    std::string title = "SPSculpt";
    if (!m_currentScenePath.empty()) {
        title += " - " + m_currentScenePath;
    }
    if (isModified) {
        title += " *";
    }
    if (title != m_lastWindowTitle) {
        SDL_SetWindowTitle(window, title.c_str());
        m_lastWindowTitle = title;
    }
}

void GuiManager::drawUnsavedChangesModal(Scene& scene, bool& quitApp) {
    if (m_showUnsavedModal) {
        ImGui::OpenPopup("Unsaved Changes##ExitModal");
        m_showUnsavedModal = false;
        m_unsavedModalOpen = true;
    }

    if (m_unsavedModalOpen) {
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Unsaved Changes##ExitModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::Text("The current project has unsaved changes.");
            ImGui::Text("Do you want to save changes before exiting?");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("Save", ImVec2(100, 0))) {
                saveScene(scene);
                if (!scene.isModified()) {
                    quitApp = true;
                    m_unsavedModalOpen = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Don't Save", ImVec2(110, 0))) {
                scene.setModified(false);
                quitApp = true;
                m_unsavedModalOpen = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                cancelQuit();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

void GuiManager::importFile(Scene& scene, SculptManager* sculpt) {
    std::string path = FileDialog::openFile(FileDialog::getImportFilters(), "Import File");
    if (!path.empty()) {
        snprintf(m_importPath, sizeof(m_importPath), "%s", path.c_str());
        auto newMeshes = FileManager::importFiles(path, &scene, m_renderer, sculpt);
        for (auto* mesh : newMeshes) {
            scene.addMesh(mesh);
        }
        if (!newMeshes.empty()) {
            scene.selectMesh(newMeshes.front());
            scene.pushHistoryState();
        }
    }
}

void GuiManager::exportFile(Scene& scene, SculptManager* sculpt) {
    std::string path = FileDialog::saveFile(FileDialog::getExportFilters(), "sgl", "Export File");
    if (!path.empty()) {
        snprintf(m_exportPath, sizeof(m_exportPath), "%s", path.c_str());
        FileManager::exportMeshes(path, scene.getMeshes(), &scene, m_renderer, sculpt);
    }
}

void GuiManager::rebuildFontsAndStyles() {
    if (!m_window) return;

    ImGuiIO& io = ImGui::GetIO();
    
    // Clear existing fonts
    io.Fonts->Clear();
    
    // Combined scale factor is the multiplier times the system DPI scale
    float combinedScale = getUiScale();
    
    // Load default font first (crisp high-DPI scaling)
    ImFont* mainFont = nullptr;
#ifdef _WIN32
    std::string fontPath = "C:\\Windows\\Fonts\\segoeui.ttf";
    mainFont = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), std::round(14.0f * combinedScale));
    if (!mainFont) {
        mainFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", std::round(14.0f * combinedScale));
    }
#endif

    if (!mainFont) {
        io.Fonts->AddFontDefault();
        if (combinedScale > 1.0f) {
            io.FontGlobalScale = combinedScale;
        } else {
            io.FontGlobalScale = 1.0f;
        }
    } else {
        io.FontGlobalScale = 1.0f;
    }

    // Configure and merge Lucide icon font
    ImFontConfig font_cfg;
    font_cfg.MergeMode = true;
    font_cfg.PixelSnapH = true;
    font_cfg.FontDataOwnedByAtlas = false;
    font_cfg.GlyphMinAdvanceX = 13.0f * combinedScale;
    font_cfg.GlyphOffset = ImVec2(0.0f, 2.0f * combinedScale); // Adjusted offset to align icons properly in buttons
    static const ImWchar icon_ranges[] = { 0xe000, 0xf8ff, 0 };
    io.Fonts->AddFontFromMemoryTTF((void*)lucide_font_data, lucide_font_size, std::round(14.0f * combinedScale), &font_cfg, icon_ranges);
    
    // Recreate the font texture on the GPU if OpenGL is already initialized and the texture exists
    if (m_imguiInitialized && io.Fonts->TexID != nullptr) {
        ImGui_ImplOpenGL3_DestroyFontsTexture();
        ImGui_ImplOpenGL3_CreateFontsTexture();
    }
    
    // Reset style and scale it
    ImGuiStyle& style = ImGui::GetStyle();
    ImGuiStyle defaultStyle;
    style = defaultStyle;
    
    ImGui::StyleColorsDark();
    
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
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.09f, 0.10f, 0.98f);
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

    if (combinedScale > 1.0f) {
        style.ScaleAllSizes(combinedScale);
    }
}

void GuiManager::init(SDL_Window* window, SDL_GLContext glContext) {
    if (m_imguiInitialized) return;

    m_window = window;

    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    int display_w, display_h;
    SDL_GL_GetDrawableSize(window, &display_w, &display_h);
    m_dpiScale = (w > 0) ? ((float)display_w / (float)w) : 1.0f;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    rebuildFontsAndStyles();

    ImGui_ImplSDL2_InitForOpenGL(window, glContext);
    ImGui_ImplOpenGL3_Init(nullptr);

    m_imguiInitialized = true;
}

void GuiManager::shutdown() {
    for (auto& [name, texID] : m_iconCache) {
        if (texID != 0) {
            glDeleteTextures(1, &texID);
        }
    }
    m_iconCache.clear();

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

    if (m_pendingUiScaleRefresh) {
        rebuildFontsAndStyles();
        m_pendingUiScaleRefresh = false;
    }

    float scale = getUiScale();

    BrushType currentBrush = sculpt.getBrush();
    if (m_previewingPaint && currentBrush != BRUSH_PAINT) {
        m_previewingPaint = false;
        renderer.setAlbedo(m_savedAlbedo[0], m_savedAlbedo[1], m_savedAlbedo[2]);
        renderer.setRoughness(m_savedRoughness);
        renderer.setMetallic(m_savedMetallic);
        renderer.setUseVertexColors(m_savedUseVertexColors);
        renderer.setUseVertexMaterials(m_savedUseVertexMaterials);
    }

    if (currentBrush != m_lastBrushType) {
        if (currentBrush == BRUSH_PAINT) {
            renderer.setUseVertexColors(true);
            renderer.setUseVertexMaterials(true);
        }
        m_lastBrushType = currentBrush;
    }

    if (m_remeshAsync.state == RemeshState::Done) {
        applyRemeshResult(scene, m_remeshAsync.result);
        sculpt.cancelStroke();
        SDL_SetModState(KMOD_NONE);
        m_remeshAsync.result = RemeshResult(); // Free memory
        m_remeshAsync.state = RemeshState::Idle;
    } else if (m_remeshAsync.state == RemeshState::Error) {
        m_remeshAsync.state = RemeshState::Idle;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    if (m_timelapsePlayer.isOpen()) {
        m_timelapsePlayer.update(ImGui::GetIO().DeltaTime, scene);
    }

    // 1. Main Menu Bar (fallback when floating island is disabled)
    if (!m_showFloatingIsland) {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        bool menuBarOpen = ImGui::BeginMainMenuBar();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        if (menuBarOpen) {
            drawAppMenuItems(sculpt, scene, renderer);
            ImGui::EndMainMenuBar();
        }
    }

    // 2. Vertical Toolbar on the left
    if (m_showToolbar) {
        ImGui::SetNextWindowPos({10.0f * scale, 40.0f * scale}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(160.0f * scale, -1.0f), ImVec2(160.0f * scale, -1.0f));
        ImGui::Begin("Toolbar", &m_showToolbar, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
        
        const char* tools[] = { 
            "Flatten", "Smooth", "Inflate", "Pinch", "Crease", "V-Tool", "Move", "Drag", "Elastic", 
            "Mask", "Paint", "Twist", "Local Scale", "Clay", "Clay Buildup", "Dam Standard", "Square Brush", "Visibility", "Mask Gradient Blur",
            "Measure", "Divider", "Transform", "Armature Spheres", "Brush", "PolyGroup"
        };
        BrushType current = sculpt.getBrush();
        for (int i = 0; i < BRUSH_COUNT; i++) {
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
        ImGui::SetNextWindowPos({160.0f * scale, 40.0f * scale}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({300.0f * scale, 400.0f * scale}, ImGuiCond_FirstUseEver);
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
            ImGui::SliderFloat("Radius", &settings.radius, 1.0f, 1000.0f, "%.1f px");
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

            float intensityPct = settings.intensity * 100.0f;
            float maxPct = (intensityPct > 100.0f) ? std::min(1000.0f, std::max(100.0f, intensityPct)) : 100.0f;
            if (ImGui::SliderFloat("Intensity", &intensityPct, 0.0f, maxPct, "%.0f%%")) {
                settings.intensity = std::max(0.0f, std::min(10.0f, intensityPct / 100.0f));
            }
            if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.x > 0.0f && intensityPct >= maxPct - 0.1f) {
                intensityPct = std::min(1000.0f, intensityPct + 10.0f);
                settings.intensity = intensityPct / 100.0f;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Overall brush strength (0% to 1000%)");
                float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f) {
                    float stepPct = (intensityPct < 100.0f) ? 5.0f : 25.0f;
                    intensityPct = std::max(0.0f, std::min(1000.0f, intensityPct + wheel * stepPct));
                    settings.intensity = intensityPct / 100.0f;
                }
            }

            ImGui::SliderFloat("Hardness", &settings.hardness, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Brush profile hardness/falloff shape");

            ImGui::SliderFloat("Focal Shift", &settings.focalShift, -1.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Controls transition from center to border");

            ImGui::Checkbox("Focal Shift Falloff", &settings.focalShiftFalloff);

            bool isGrabBrush = (brushType == BRUSH_MOVE || brushType == BRUSH_DRAG || brushType == BRUSH_ELASTIC);
            if (!isGrabBrush) {
                ImGui::SliderFloat("Spacing", &settings.spacing, 0.01f, 1.0f, "%.2f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Spacing between stroke points as fraction of radius");
            }

            ImGui::Checkbox("Negative (Invert)", &settings.negative);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggles add vs subtract sculpting direction");

            ImGui::Checkbox("Backface Culling", &settings.culling);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable backface culling to avoid painting through surfaces");

            ImGui::Checkbox("Lock Single PolyGroup", &settings.singlePolyGroup);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Limit brush deformation/painting to the PolyGroup under the cursor at stroke start");

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
        }

        // 3. Tool-Specific Parameters Section
        bool hasSpecialParams = (brushType == BRUSH_SMOOTH || brushType == BRUSH_MOVE || 
                                 brushType == BRUSH_ELASTIC || brushType == BRUSH_CLAY || 
                                 brushType == BRUSH_CLAYBUILDUP || 
                                 brushType == BRUSH_SQUAREBRUSH || brushType == BRUSH_PAINT || 
                                 brushType == BRUSH_MASK || brushType == BRUSH_MASK_GRADIENT_BLUR ||
                                 brushType == BRUSH_MEASURE || brushType == BRUSH_DIVIDER ||
                                 brushType == BRUSH_TRANSFORM || brushType == BRUSH_ARMATURE_SPHERES ||
                                 brushType == BRUSH_BRUSH || brushType == BRUSH_POLYGROUP);

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
                else if (brushType == BRUSH_BRUSH) {
                    ImGui::Checkbox("Clay Mode", &settings.clay);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fills or cuts towards a flat surface instead of displacing vertices along normal");

                    ImGui::Checkbox("Lock Rotation", &settings.stampLockRotation);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keeps the stamp rotation fixed relative to screen space instead of aligning with stroke direction");

                    ImGui::Checkbox("Use Pen Tilt", &settings.stampUseTilt);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Controls the stamp rotation angle using the pen's tilt direction");

                    const char* stampTypes[] = { "Circle", "Polygon", "Star", "Ring", "Rectangle" };
                    ImGui::Combo("Stamp Type", &settings.stampType, stampTypes, IM_ARRAYSIZE(stampTypes));
                    
                    if (settings.stampType == 1 || settings.stampType == 2) {
                        ImGui::SliderInt("Stamp Sides", &settings.stampSides, 3, 12);
                    }
                    if (settings.stampType == 2 || settings.stampType == 3) {
                        ImGui::SliderFloat("Inner Ratio", &settings.stampInnerRatio, 0.05f, 0.95f, "%.2f");
                    }
                    else if (settings.stampType == 4) {
                        ImGui::SliderFloat("Aspect Ratio", &settings.stampInnerRatio, 0.05f, 1.00f, "%.2f");
                    }
                    ImGui::SliderFloat("Stamp Angle", &settings.stampAngle, -180.0f, 180.0f, "%.1f°");
                    ImGui::SliderFloat("Stamp Blur", &settings.stampBlur, 0.0f, 1.0f, "%.2f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Parametrically blurs the edges of the stamp shape");

                    // 2D Draw Preview!
                    ImGui::Spacing();
                    ImGui::Text("Stamp Shape Preview:");
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    ImVec2 curPos = ImGui::GetCursorScreenPos();
                    float size = 80.0f;
                    ImVec2 center = ImVec2(curPos.x + size * 0.5f, curPos.y + size * 0.5f);
                    
                    // Draw a background box
                    drawList->AddRectFilled(curPos, ImVec2(curPos.x + size, curPos.y + size), IM_COL32(30, 30, 30, 255), 4.0f);
                    drawList->AddRect(curPos, ImVec2(curPos.x + size, curPos.y + size), IM_COL32(60, 60, 60, 255), 4.0f, 0, 1.5f);
                    
                    // Generate points for the shape preview
                    std::vector<ImVec2> pts;
                    float rOut = size * 0.4f;
                    float rIn = rOut * settings.stampInnerRatio;
                    float finalAngle = settings.stampAngle;
#ifdef _WIN32
                    if (settings.stampUseTilt && g_tablet.isAvailable() && g_tablet.isPenActive() && g_tablet.isTiltEnabled()) {
                        float tx = g_tablet.getTiltX();
                        float ty = g_tablet.getTiltY();
                        if (tx * tx + ty * ty > 1.0f) {
                            finalAngle += std::atan2(ty, tx) * (180.0f / 3.1415926535f);
                        }
                    }
#endif
                    float angleRad = finalAngle * (3.14159265f / 180.0f);
                    float cosA = std::cos(angleRad);
                    float sinA = std::sin(angleRad);
                    
                    if (settings.stampType == 0) { // Circle
                        float rCore = rOut * (1.0f - settings.stampBlur);
                        drawList->AddCircleFilled(center, rCore, IM_COL32(0, 220, 255, 200), 32);
                        if (settings.stampBlur > 0.01f) {
                            int steps = 15;
                            float blurWidth = rOut * settings.stampBlur;
                            float thick = blurWidth / steps;
                            for (int j = 0; j < steps; ++j) {
                                float r = rCore + (j + 0.5f) * thick;
                                float t = (float)j / steps;
                                int alpha = (int)(200.0f * (1.0f - t));
                                drawList->AddCircle(center, r, IM_COL32(0, 220, 255, alpha), 32, thick + 0.5f);
                            }
                        }
                        drawList->AddCircle(center, rOut, IM_COL32(255, 255, 255, 255), 32, 2.0f);
                        if (settings.stampBlur > 0.01f) {
                            drawList->AddCircle(center, rCore, IM_COL32(255, 255, 255, 120), 32, 1.0f);
                        }
                    }
                    else if (settings.stampType == 1) { // Polygon
                        int sides = std::max(3, settings.stampSides);
                        for (int s = 0; s < sides; ++s) {
                            float a = angleRad + s * (2.0f * 3.14159265f / sides);
                            pts.push_back(ImVec2(center.x + rOut * std::cos(a), center.y + rOut * std::sin(a)));
                        }
                        
                        // Solid core
                        std::vector<ImVec2> corePts;
                        float coreScale = 1.0f - settings.stampBlur;
                        for (const auto& p : pts) {
                            float dx = p.x - center.x;
                            float dy = p.y - center.y;
                            corePts.push_back(ImVec2(center.x + dx * coreScale, center.y + dy * coreScale));
                        }
                        drawList->AddConvexPolyFilled(corePts.data(), (int)corePts.size(), IM_COL32(0, 220, 255, 200));
                        
                        // Blur region
                        if (settings.stampBlur > 0.01f) {
                            int steps = 15;
                            float thick = (rOut * settings.stampBlur) / steps;
                            for (int j = 0; j < steps; ++j) {
                                float t = (float)j / steps;
                                float scale = coreScale + t * settings.stampBlur;
                                std::vector<ImVec2> stepPts;
                                for (const auto& p : pts) {
                                    float dx = p.x - center.x;
                                    float dy = p.y - center.y;
                                    stepPts.push_back(ImVec2(center.x + dx * scale, center.y + dy * scale));
                                }
                                int alpha = (int)(200.0f * (1.0f - t));
                                drawList->AddPolyline(stepPts.data(), (int)stepPts.size(), IM_COL32(0, 220, 255, alpha), ImDrawFlags_Closed, thick + 0.5f);
                            }
                        }
                        
                        drawList->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(255, 255, 255, 255), ImDrawFlags_Closed, 2.0f);
                        if (settings.stampBlur > 0.01f) {
                            drawList->AddPolyline(corePts.data(), (int)corePts.size(), IM_COL32(255, 255, 255, 120), ImDrawFlags_Closed, 1.0f);
                        }
                    }
                    else if (settings.stampType == 2) { // Star
                        int sides = std::max(2, settings.stampSides);
                        for (int s = 0; s < sides * 2; ++s) {
                            float r = (s % 2 == 0) ? rOut : rIn;
                            float a = angleRad + s * (3.14159265f / sides);
                            pts.push_back(ImVec2(center.x + r * std::cos(a), center.y + r * std::sin(a)));
                        }
                        
                        // Solid core
                        std::vector<ImVec2> corePts;
                        float coreScale = 1.0f - settings.stampBlur;
                        for (const auto& p : pts) {
                            float dx = p.x - center.x;
                            float dy = p.y - center.y;
                            corePts.push_back(ImVec2(center.x + dx * coreScale, center.y + dy * coreScale));
                        }
                        drawList->AddConvexPolyFilled(corePts.data(), (int)corePts.size(), IM_COL32(0, 220, 255, 200));
                        
                        // Blur region
                        if (settings.stampBlur > 0.01f) {
                            int steps = 15;
                            float thick = (rOut * settings.stampBlur) / steps;
                            for (int j = 0; j < steps; ++j) {
                                float t = (float)j / steps;
                                float scale = coreScale + t * settings.stampBlur;
                                std::vector<ImVec2> stepPts;
                                for (const auto& p : pts) {
                                    float dx = p.x - center.x;
                                    float dy = p.y - center.y;
                                    stepPts.push_back(ImVec2(center.x + dx * scale, center.y + dy * scale));
                                }
                                int alpha = (int)(200.0f * (1.0f - t));
                                drawList->AddPolyline(stepPts.data(), (int)stepPts.size(), IM_COL32(0, 220, 255, alpha), ImDrawFlags_Closed, thick + 0.5f);
                            }
                        }
                        
                        drawList->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(255, 255, 255, 255), ImDrawFlags_Closed, 2.0f);
                        if (settings.stampBlur > 0.01f) {
                            drawList->AddPolyline(corePts.data(), (int)corePts.size(), IM_COL32(255, 255, 255, 120), ImDrawFlags_Closed, 1.0f);
                        }
                    }
                    else if (settings.stampType == 3) { // Ring
                        float blurWidth = (rOut - rIn) * 0.5f * settings.stampBlur;
                        float rOutCore = rOut - blurWidth;
                        float rInCore = rIn + blurWidth;
                        
                        drawList->AddCircleFilled(center, rOutCore, IM_COL32(0, 220, 255, 200), 32);
                        drawList->AddCircleFilled(center, rInCore, IM_COL32(30, 30, 30, 255), 32);
                        
                        if (blurWidth > 0.01f) {
                            int steps = 15;
                            float thick = blurWidth / steps;
                            // Outer blur
                            for (int j = 0; j < steps; ++j) {
                                float r = rOutCore + (j + 0.5f) * thick;
                                float t = (float)j / steps;
                                int alpha = (int)(200.0f * (1.0f - t));
                                drawList->AddCircle(center, r, IM_COL32(0, 220, 255, alpha), 32, thick + 0.5f);
                            }
                            // Inner blur
                            for (int j = 0; j < steps; ++j) {
                                float r = rInCore - (j + 0.5f) * thick;
                                float t = (float)j / steps;
                                int alpha = (int)(255.0f * t);
                                drawList->AddCircle(center, r, IM_COL32(30, 30, 30, alpha), 32, thick + 0.5f);
                            }
                        }
                        
                        drawList->AddCircle(center, rOut, IM_COL32(255, 255, 255, 255), 32, 2.0f);
                        drawList->AddCircle(center, rIn, IM_COL32(255, 255, 255, 255), 32, 1.5f);
                        if (settings.stampBlur > 0.01f) {
                            drawList->AddCircle(center, rOutCore, IM_COL32(255, 255, 255, 120), 32, 1.0f);
                            drawList->AddCircle(center, rInCore, IM_COL32(255, 255, 255, 120), 32, 1.0f);
                        }
                    }
                    else if (settings.stampType == 4) { // Rectangle
                        float w = rOut;
                        float h = rOut * settings.stampInnerRatio;
                        ImVec2 corners[4] = {
                            ImVec2(-w, -h),
                            ImVec2(w, -h),
                            ImVec2(w, h),
                            ImVec2(-w, h)
                        };
                        for (int k = 0; k < 4; ++k) {
                            float rx = corners[k].x * cosA - corners[k].y * sinA;
                            float ry = corners[k].x * sinA + corners[k].y * cosA;
                            pts.push_back(ImVec2(center.x + rx, center.y + ry));
                        }
                        
                        // Solid core
                        std::vector<ImVec2> corePts;
                        float coreScale = 1.0f - settings.stampBlur;
                        for (const auto& p : pts) {
                            float dx = p.x - center.x;
                            float dy = p.y - center.y;
                            corePts.push_back(ImVec2(center.x + dx * coreScale, center.y + dy * coreScale));
                        }
                        drawList->AddConvexPolyFilled(corePts.data(), (int)corePts.size(), IM_COL32(0, 220, 255, 200));
                        
                        // Blur region
                        if (settings.stampBlur > 0.01f) {
                            int steps = 15;
                            float thick = (rOut * settings.stampBlur) / steps;
                            for (int j = 0; j < steps; ++j) {
                                float t = (float)j / steps;
                                float scale = coreScale + t * settings.stampBlur;
                                std::vector<ImVec2> stepPts;
                                for (const auto& p : pts) {
                                    float dx = p.x - center.x;
                                    float dy = p.y - center.y;
                                    stepPts.push_back(ImVec2(center.x + dx * scale, center.y + dy * scale));
                                }
                                int alpha = (int)(200.0f * (1.0f - t));
                                drawList->AddPolyline(stepPts.data(), (int)stepPts.size(), IM_COL32(0, 220, 255, alpha), ImDrawFlags_Closed, thick + 0.5f);
                            }
                        }
                        
                        drawList->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(255, 255, 255, 255), ImDrawFlags_Closed, 2.0f);
                        if (settings.stampBlur > 0.01f) {
                            drawList->AddPolyline(corePts.data(), (int)corePts.size(), IM_COL32(255, 255, 255, 120), ImDrawFlags_Closed, 1.0f);
                        }
                    }
                    
                    ImGui::Dummy(ImVec2(size, size));
                }
                else if (brushType == BRUSH_PAINT) {
                    ImGui::ColorEdit3("Albedo (Color)", &settings.paintColor.r);
                    bool albedoActive = ImGui::IsItemActive();
                    
                    ImGui::SliderFloat("Roughness", &settings.paintRoughness, 0.0f, 1.0f, "%.2f");
                    bool roughnessActive = ImGui::IsItemActive();
                    
                    ImGui::SliderFloat("Metalness", &settings.paintMetallic, 0.0f, 1.0f, "%.2f");
                    bool metallicActive = ImGui::IsItemActive();

                    bool anyActive = albedoActive || roughnessActive || metallicActive;
                    if (anyActive) {
                        if (!m_previewingPaint) {
                            m_savedAlbedo[0] = renderer.getAlbedo()[0];
                            m_savedAlbedo[1] = renderer.getAlbedo()[1];
                            m_savedAlbedo[2] = renderer.getAlbedo()[2];
                            m_savedRoughness = renderer.getRoughness();
                            m_savedMetallic = renderer.getMetallic();
                            m_savedUseVertexColors = renderer.getUseVertexColors();
                            m_savedUseVertexMaterials = renderer.getUseVertexMaterials();
                            m_previewingPaint = true;
                        }
                        renderer.setUseVertexColors(false);
                        renderer.setUseVertexMaterials(false);
                        renderer.setAlbedo(settings.paintColor.r, settings.paintColor.g, settings.paintColor.b);
                        renderer.setRoughness(settings.paintRoughness);
                        renderer.setMetallic(settings.paintMetallic);
                    } else if (m_previewingPaint) {
                        m_previewingPaint = false;
                        renderer.setAlbedo(m_savedAlbedo[0], m_savedAlbedo[1], m_savedAlbedo[2]);
                        renderer.setRoughness(m_savedRoughness);
                        renderer.setMetallic(m_savedMetallic);
                        renderer.setUseVertexColors(m_savedUseVertexColors);
                        renderer.setUseVertexMaterials(m_savedUseVertexMaterials);
                    }
                    
                    ImGui::Separator();
                    Mesh* selectedMesh = scene.getSelected();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.44f, 0.70f, 1.00f));
                    if (ImGui::Button("Paint All", ImVec2(120, 26))) {
                        renderer.setUseVertexColors(true);
                        renderer.setUseVertexMaterials(true);
                        sculpt.paintAll(scene, selectedMesh);
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::Checkbox("Pick Color", &settings.pickColor);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Use the dropper tool to pick color/roughness/metallic from mesh surface");

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
                        selectedMesh = scene.getSelected();
                        if (selectedMesh) sculpt.clearMask(selectedMesh, &scene);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Invert Mask", ImVec2(120, 26))) {
                        scene.pushHistoryState();
                        selectedMesh = scene.getSelected();
                        if (selectedMesh) sculpt.invertMask(selectedMesh);
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
                        selectedMesh = scene.getSelected();
                        if (selectedMesh) sculpt.clearMask(selectedMesh, &scene);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Invert Mask", ImVec2(120, 26))) {
                        scene.pushHistoryState();
                        selectedMesh = scene.getSelected();
                        if (selectedMesh) sculpt.invertMask(selectedMesh);
                    }
                    ImGui::PopStyleColor();

                    ImGui::Separator();
                    ImGui::SliderInt("Blur Iterations", &settings.maskSharpenBlurIterations, 1, 100);
                    ImGui::Checkbox("Blur Masked Only", &settings.blurMaskedOnly);
                }
                else if (brushType == BRUSH_POLYGROUP) {
                    Mesh* selectedMesh = scene.getSelected();
                    uint32_t currentGid = sculpt.getActiveGroupID();

                    ImGui::Text("Active Group:");
                    ImGui::SameLine();

                    // Group color swatch preview
                    float goldenRatio = 0.618033988749895f;
                    float gh = fmodf((float)currentGid * goldenRatio, 1.0f);
                    float gr, gg, gb;
                    ImGui::ColorConvertHSVtoRGB(gh, 0.85f, 0.95f, gr, gg, gb);
                    ImGui::ColorButton("##groupColorPreview", ImVec4(gr, gg, gb, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(24, 24));
                    ImGui::SameLine();

                    int activeGid = (int)currentGid;
                    ImGui::PushItemWidth(70.0f);
                    if (ImGui::InputInt("##activeGid", &activeGid, 0, 0)) {
                        if (activeGid < 1) activeGid = 1;
                        sculpt.setActiveGroupID((uint32_t)activeGid);
                    }
                    ImGui::PopItemWidth();
                    ImGui::SameLine();
                    if (ImGui::Button("-##decGid")) {
                        if (currentGid > 1) sculpt.setActiveGroupID(currentGid - 1);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("+##incGid")) {
                        sculpt.setActiveGroupID(currentGid + 1);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("New Group (+)", ImVec2(100, 24))) {
                        uint32_t nextId = selectedMesh ? selectedMesh->getNextFreeGroupID() : (currentGid + 1);
                        sculpt.setActiveGroupID(nextId);
                    }

                    ImGui::Separator();
                    ImGui::TextDisabled("Shortcuts:\n - Stroke: Paint active group\n - Alt + Click: Eyedropper / Pick ID\n - Ctrl + Click: Flood Fill component");
                    ImGui::Separator();

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.44f, 0.70f, 1.00f));
                    if (ImGui::Button("Group From Mask", ImVec2(130, 26))) {
                        if (selectedMesh) {
                            scene.pushHistoryState();
                            selectedMesh = scene.getSelected();
                            sculpt.getPolyGroupTool().createGroupFromMask(selectedMesh);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Auto Group", ImVec2(130, 26))) {
                        if (selectedMesh) {
                            scene.pushHistoryState();
                            selectedMesh = scene.getSelected();
                            sculpt.getPolyGroupTool().autoGroupByConnectedComponents(selectedMesh);
                        }
                    }
                    if (ImGui::Button("Clear All Groups", ImVec2(-1, 26))) {
                        if (selectedMesh) {
                            scene.pushHistoryState();
                            selectedMesh = scene.getSelected();
                            sculpt.getPolyGroupTool().clearAllGroups(selectedMesh);
                        }
                    }
                    ImGui::PopStyleColor();
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
                    
                    Mesh* selectedMesh = scene.getSelected();
                    if (selectedMesh) {
                        bool meshHasMask = false;
                        if (!selectedMesh->materials.empty()) {
                            for (int i = 0; i < selectedMesh->nbVerts; ++i) {
                                if (selectedMesh->materials[i * 3 + 2] < 0.999f) {
                                    meshHasMask = true;
                                    break;
                                }
                            }
                        }

                        if (meshHasMask) {
                            if (ImGui::Button("Center Pivot on Unmasked", ImVec2(-1, 26))) {
                                scene.pushHistoryState();
                                glm::vec3 centerSum(0.0f);
                                float weightSum = 0.0f;
                                for (int i = 0; i < selectedMesh->nbVerts; ++i) {
                                    float m = selectedMesh->materials[i * 3 + 2];
                                    if (m > 0.001f) {
                                        glm::vec3 localPos(selectedMesh->verts[i * 3], selectedMesh->verts[i * 3 + 1], selectedMesh->verts[i * 3 + 2]);
                                        centerSum += localPos * m;
                                        weightSum += m;
                                    }
                                }
                                glm::vec3 targetLocalCenter(0.0f);
                                if (weightSum > 1e-5f) {
                                    targetLocalCenter = centerSum / weightSum;
                                } else {
                                    float bbox[6];
                                    selectedMesh->computeBbox(bbox);
                                    targetLocalCenter = glm::vec3((bbox[0]+bbox[1])*0.5f, (bbox[2]+bbox[3])*0.5f, (bbox[4]+bbox[5])*0.5f);
                                }

                                glm::vec3 targetWorldCenter = glm::vec3(selectedMesh->matrix * glm::vec4(targetLocalCenter, 1.0f));
                                glm::mat4 targetMatrix = selectedMesh->matrix;
                                targetMatrix[3] = glm::vec4(targetWorldCenter, 1.0f);

                                glm::mat4 deltaLocalMatrix = glm::inverse(selectedMesh->matrix) * targetMatrix;
                                glm::mat4 deltaLocalMatrixInv = glm::inverse(deltaLocalMatrix);
                                glm::mat3 enMatrix = glm::transpose(glm::inverse(glm::mat3(deltaLocalMatrixInv)));
                                for (int i = 0; i < selectedMesh->nbVerts; ++i) {
                                    glm::vec4 pos(selectedMesh->verts[i * 3], selectedMesh->verts[i * 3 + 1], selectedMesh->verts[i * 3 + 2], 1.0f);
                                    glm::vec4 newPos = deltaLocalMatrixInv * pos;
                                    selectedMesh->verts[i * 3]     = newPos.x;
                                    selectedMesh->verts[i * 3 + 1] = newPos.y;
                                    selectedMesh->verts[i * 3 + 2] = newPos.z;

                                    glm::vec3 normal(selectedMesh->normals[i * 3], selectedMesh->normals[i * 3 + 1], selectedMesh->normals[i * 3 + 2]);
                                    glm::vec3 newNormal = glm::normalize(enMatrix * normal);
                                    selectedMesh->normals[i * 3]     = newNormal.x;
                                    selectedMesh->normals[i * 3 + 1] = newNormal.y;
                                    selectedMesh->normals[i * 3 + 2] = newNormal.z;
                                }
                                selectedMesh->matrix = targetMatrix;
                                selectedMesh->postInit();
                                selectedMesh->isDirty = true;
                            }
                        }

                        if (ImGui::Button("Reset Matrix", ImVec2(-1, 26))) {
                            scene.pushHistoryState();
                            if (meshHasMask && !m_editPivot) {
                                glm::mat4 pivotStartMatrix = selectedMesh->matrix;
                                glm::mat4 targetMatrix(1.0f);
                                glm::mat4 invMnew = glm::inverse(targetMatrix);
                                glm::mat4 startToNewLocal = invMnew * pivotStartMatrix;
                                glm::mat3 normalMatrixStartToNew = glm::transpose(glm::inverse(glm::mat3(startToNewLocal)));
                                for (int i = 0; i < selectedMesh->nbVerts; ++i) {
                                    float m = selectedMesh->materials[i * 3 + 2];
                                    if (m < 0.999f) {
                                        glm::vec4 vStart(selectedMesh->verts[i * 3], selectedMesh->verts[i * 3 + 1], selectedMesh->verts[i * 3 + 2], 1.0f);
                                        glm::vec3 nStart(selectedMesh->normals[i * 3], selectedMesh->normals[i * 3 + 1], selectedMesh->normals[i * 3 + 2]);
                                        glm::vec4 vTransformed = startToNewLocal * vStart;
                                        glm::vec4 vNew = (1.0f - m) * vTransformed + m * vStart;

                                        glm::vec3 nTransformed = normalMatrixStartToNew * nStart;
                                        glm::vec3 nNew = glm::normalize((1.0f - m) * nTransformed + m * nStart);

                                        selectedMesh->verts[i * 3]     = vNew.x;
                                        selectedMesh->verts[i * 3 + 1] = vNew.y;
                                        selectedMesh->verts[i * 3 + 2] = vNew.z;
                                        selectedMesh->normals[i * 3]     = nNew.x;
                                        selectedMesh->normals[i * 3 + 1] = nNew.y;
                                        selectedMesh->normals[i * 3 + 2] = nNew.z;
                                    }
                                }
                                selectedMesh->matrix = targetMatrix;
                                selectedMesh->postInit();
                            } else {
                                selectedMesh->matrix = glm::mat4(1.0f);
                            }
                            selectedMesh->isDirty = true;
                        }
                    }
                }
                else if (brushType == BRUSH_ARMATURE_SPHERES) {
                    ArmatureTool* tool = sculpt.getArmatureTool();
                    if (tool) {
                        int mode = (int)tool->getMode();
                        ImGui::RadioButton("Draw", &mode, 0); ImGui::SameLine();
                        ImGui::RadioButton("Insert", &mode, 1); ImGui::SameLine();
                        ImGui::RadioButton("Move", &mode, 2);
                        ImGui::RadioButton("Scale", &mode, 3); ImGui::SameLine();
                        ImGui::RadioButton("Rotate", &mode, 4);
                        tool->setMode((ArmatureMode)mode);
                        
                        ImGui::Separator();
                        int res = tool->getResolution();
                        if (ImGui::SliderInt("Resolution", &res, 16, 256)) {
                            tool->setResolution(res);
                        }

                        if (ImGui::Button("Create Mesh", ImVec2(-1, 26))) {
                            tool->createMesh(scene);
                        }
                        if (ImGui::Button("Clear Graph", ImVec2(-1, 26))) {
                            auto* graph = tool->getGraph(scene);
                            if (graph) graph->clear();
                        }
                    }
                }
            }
        }

        ImGui::End();
    }

    // 3.5 Multiresolution Sculpting Panel
    if (m_showMultiresPanel) {
        ImGui::SetNextWindowPos({160.0f * scale, 450.0f * scale}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({300.0f * scale, 300.0f * scale}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Multiresolution", &m_showMultiresPanel);

        Mesh* selectedMesh = scene.getSelected();
        if (!selectedMesh) {
            ImGui::TextDisabled("No mesh selected");
        } else {
            Multimesh* multimesh = dynamic_cast<Multimesh*>(selectedMesh);
            
            // Auto-convert helper for standard Mesh
            auto ensureMultimesh = [&]() -> Multimesh* {
                if (multimesh) return multimesh;
                sculpt_log("[UI Multires] Auto-converting standard Mesh '%s' to Multimesh...\n", selectedMesh->outlinerName.c_str());
                auto newMm = std::make_unique<Multimesh>(selectedMesh);
                Multimesh* mmPtr = newMm.get();
                scene.replaceMesh(selectedMesh, newMm.release());
                multimesh = mmPtr;
                return multimesh;
            };

            MeshResolution* curMesh = multimesh ? multimesh->getCurrentMesh() : nullptr;
            int curSel = multimesh ? multimesh->getSelection() : 0;
            int numLevels = multimesh ? (int)multimesh->getNbLevels() : 1;

            ImGui::Text("Mesh: %s", selectedMesh->outlinerName.c_str());
            ImGui::Text("Active Level: %d / %d", curSel + 1, numLevels);
            if (curMesh) {
                ImGui::Text("Vertices: %d", curMesh->getNbVertices());
                ImGui::Text("Faces: %d", curMesh->getNbFaces());
            } else {
                ImGui::Text("Vertices: %d", selectedMesh->getNbVertices());
                ImGui::Text("Faces: %d", selectedMesh->getNbFaces());
            }
            ImGui::Separator();

            // Navigation buttons
            if (curSel > 0) {
                if (ImGui::Button("Lower Level", ImVec2(135, 28))) {
                    sculpt_log("[UI Multires] Lower Level clicked (Current: %d)\n", curSel);
                    if (multimesh) multimesh->lowerLevel();
                }
            } else {
                ImGui::BeginDisabled();
                ImGui::Button("Lower Level", ImVec2(135, 28));
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            if (curSel < numLevels - 1) {
                if (ImGui::Button("Higher Level", ImVec2(135, 28))) {
                    sculpt_log("[UI Multires] Higher Level clicked (Current: %d)\n", curSel);
                    if (multimesh) multimesh->higherLevel();
                }
            } else {
                ImGui::BeginDisabled();
                ImGui::Button("Higher Level", ImVec2(135, 28));
                ImGui::EndDisabled();
            }

            ImGui::Spacing();

            // Add / Reverse Subdivide buttons
            if (ImGui::Button("Subdivide (+)", ImVec2(135, 28))) {
                sculpt_log("[UI Multires] Subdivide (+) clicked. Active level: %d / %d\n", curSel + 1, numLevels);
                scene.pushHistoryState();
                Multimesh* mm = ensureMultimesh();
                if (mm) mm->addLevel();
                sculpt_log("[UI Multires] Subdivide (+) finished.\n");
            }
            ImGui::SameLine();
            if (ImGui::Button("Revert Topology (-)", ImVec2(135, 28))) {
                sculpt_log("[UI Multires] Revert Topology (-) clicked. Active level: %d / %d\n", curSel + 1, numLevels);
                scene.pushHistoryState();
                Multimesh* mm = ensureMultimesh();
                if (mm && !mm->computeReverse()) {
                    sculpt_log("[UI Multires] Failed to revert mesh topology (mesh is not a regular subdivision surface)\n");
                } else {
                    sculpt_log("[UI Multires] Revert Topology (-) succeeded.\n");
                }
            }

            ImGui::Spacing();
            ImGui::Separator();

            // Slider for direct level selection
            int targetSel = curSel;
            if (ImGui::SliderInt("Level", &targetSel, 0, numLevels - 1)) {
                sculpt_log("[UI Multires] Level slider changed to: %d\n", targetSel);
                if (multimesh) multimesh->selectResolution(targetSel);
            }

            ImGui::Spacing();
            if (curSel > 0) {
                if (ImGui::Button("Delete Lower", ImVec2(135, 24))) {
                    sculpt_log("[UI Multires] Delete Lower clicked (Current: %d)\n", curSel);
                    scene.pushHistoryState();
                    if (multimesh) multimesh->deleteLower();
                }
            } else {
                ImGui::BeginDisabled();
                ImGui::Button("Delete Lower", ImVec2(135, 24));
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            if (curSel < numLevels - 1) {
                if (ImGui::Button("Delete Higher", ImVec2(135, 24))) {
                    sculpt_log("[UI Multires] Delete Higher clicked (Current: %d)\n", curSel);
                    scene.pushHistoryState();
                    if (multimesh) multimesh->deleteHigher();
                }
            } else {
                ImGui::BeginDisabled();
                ImGui::Button("Delete Higher", ImVec2(135, 24));
                ImGui::EndDisabled();
            }
        }
        ImGui::End();
    }

    // 4. Scene outliner
    if (m_showScenePanel) {
        ImGui::SetNextWindowPos({450.0f * scale, 40.0f * scale}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({320.0f * scale, 450.0f * scale}, ImGuiCond_FirstUseEver);
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
                scene.addPrimitiveAtMask("sphere", spawnMirror, sculpt.getSymX(), sculpt.getSymY(), sculpt.getSymZ());
            } else {
                scene.addSphere();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Geosphere##Add", ImVec2(90, 0))) {
            if (spawnAtMask) {
                scene.addPrimitiveAtMask("geosphere", spawnMirror, sculpt.getSymX(), sculpt.getSymY(), sculpt.getSymZ());
            } else {
                scene.addGeosphere();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cube##Add", ImVec2(60, 0))) {
            if (spawnAtMask) {
                scene.addPrimitiveAtMask("cube", spawnMirror, sculpt.getSymX(), sculpt.getSymY(), sculpt.getSymZ());
            } else {
                scene.addCube();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cylinder##Add", ImVec2(75, 0))) {
            if (spawnAtMask) {
                scene.addPrimitiveAtMask("cylinder", spawnMirror, sculpt.getSymX(), sculpt.getSymY(), sculpt.getSymZ());
            } else {
                scene.addCylinder();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Torus##Add", ImVec2(60, 0))) {
            if (spawnAtMask) {
                scene.addPrimitiveAtMask("torus", spawnMirror, sculpt.getSymX(), sculpt.getSymY(), sculpt.getSymZ());
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

            // Measure Tool Row in Outliner
            bool showMeasureRow = !sculpt.getMeasureSegments().empty() || (sculpt.getBrush() == BRUSH_MEASURE);
            if (showMeasureRow) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(88801);
                bool isMeasureToolActive = (sculpt.getBrush() == BRUSH_MEASURE);
                if (ImGui::Checkbox("##ActMeasureTool", &isMeasureToolActive)) {
                    if (isMeasureToolActive) sculpt.setTool(BRUSH_MEASURE);
                }
                ImGui::PopID();

                ImGui::TableNextColumn();
                if (ImGui::Selectable("Measure Tool", isMeasureToolActive)) {
                    sculpt.setTool(BRUSH_MEASURE);
                }

                ImGui::TableNextColumn();
                ImGui::Text("%d segs", (int)sculpt.getMeasureSegments().size());

                ImGui::TableNextColumn();
                ImGui::PushID(88802);
                bool mV1 = sculpt.getMeasureVisibleV1();
                if (ImGui::Checkbox("##MeasureV1", &mV1)) {
                    sculpt.setMeasureVisibleV1(mV1);
                }
                ImGui::PopID();

                ImGui::TableNextColumn();
                ImGui::PushID(88803);
                bool mV2 = sculpt.getMeasureVisibleV2();
                if (ImGui::Checkbox("##MeasureV2", &mV2)) {
                    sculpt.setMeasureVisibleV2(mV2);
                }
                ImGui::PopID();
            }

            // Divider Tool Row in Outliner
            bool showDividerRow = !sculpt.getDividerSegments().empty() || (sculpt.getBrush() == BRUSH_DIVIDER);
            if (showDividerRow) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(88804);
                bool isDividerToolActive = (sculpt.getBrush() == BRUSH_DIVIDER);
                if (ImGui::Checkbox("##ActDividerTool", &isDividerToolActive)) {
                    if (isDividerToolActive) sculpt.setTool(BRUSH_DIVIDER);
                }
                ImGui::PopID();

                ImGui::TableNextColumn();
                if (ImGui::Selectable("Divider Tool", isDividerToolActive)) {
                    sculpt.setTool(BRUSH_DIVIDER);
                }

                ImGui::TableNextColumn();
                ImGui::Text("%d segs", (int)sculpt.getDividerSegments().size());

                ImGui::TableNextColumn();
                ImGui::PushID(88805);
                bool dV1 = sculpt.getDividerVisibleV1();
                if (ImGui::Checkbox("##DividerV1", &dV1)) {
                    sculpt.setDividerVisibleV1(dV1);
                }
                ImGui::PopID();

                ImGui::TableNextColumn();
                ImGui::PushID(88806);
                bool dV2 = sculpt.getDividerVisibleV2();
                if (ImGui::Checkbox("##DividerV2", &dV2)) {
                    sculpt.setDividerVisibleV2(dV2);
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
            sculpt.clearMeasurements();
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

        bool isArmatureActive = (sculpt.getBrush() == BRUSH_ARMATURE_SPHERES);
        if (isArmatureActive) ImGui::PushStyleColor(ImGuiCol_Button, tealActive);
        if (ImGui::Button("Armature Spheres", ImVec2(-1, 0))) {
            sculpt.setTool(BRUSH_ARMATURE_SPHERES);
        }
        if (isArmatureActive) ImGui::PopStyleColor();

        if (isMeasureActive) {
            bool useDist = sculpt.getMeasureUseDistanceThickness();
            if (ImGui::Checkbox("Use Distance Thickness", &useDist)) {
                sculpt.setMeasureUseDistanceThickness(useDist);
            }
        } else if (isDividerActive) {
            int divs = sculpt.getDividerDivisions();
            if (ImGui::SliderInt("Divisions", &divs, 2, 6)) {
                sculpt.setDividerDivisions(divs);
            }
        }

        // Created Measure & Divider Items Outliner
        sculpt.validateSegments(scene);
        auto& measureSegs = sculpt.getMeasureSegments();
        auto& dividerSegs = sculpt.getDividerSegments();
        int totalToolItems = (int)measureSegs.size() + (int)dividerSegs.size();

        if (totalToolItems > 0) {
            ImGui::Separator();
            ImGui::Text("Created Tool Items (%d):", totalToolItems);

            float referenceLength = 0.0f;
            for (const auto& seg : measureSegs) {
                if (seg.isReference) {
                    glm::vec3 worldA = SculptManager::getAnchorWorldPos(seg.vertA);
                    glm::vec3 worldB = SculptManager::getAnchorWorldPos(seg.vertB);
                    referenceLength = glm::distance(worldA, worldB);
                    break;
                }
            }

            static int renameSegType = 0; // 1 = Measure, 2 = Divider
            static int renameSegIdx = -1;
            static char renameSegBuf[128] = "";

            float availY = ImGui::GetContentRegionAvail().y;
            float desiredHeight = (float)(totalToolItems * 28 + 35);
            float listHeight = std::max(60.0f, std::min(desiredHeight, std::max(160.0f, availY - 10.0f)));

            ImGui::BeginChild("ToolOutlinerList", ImVec2(0, listHeight), true);
            if (ImGui::BeginTable("ToolOutlinerTable", 4, ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV)) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Ref", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                ImGui::TableSetupColumn("##DelCol", ImGuiTableColumnFlags_WidthFixed, 25.0f);
                ImGui::TableHeadersRow();

                int deleteMeasureIdx = -1;
                int deleteDividerIdx = -1;

                // Measure Segments
                for (int i = 0; i < (int)measureSegs.size(); ++i) {
                    auto& seg = measureSegs[i];
                    ImGui::TableNextRow();
                    ImGui::PushID(1000 + i);

                    glm::vec3 worldA = SculptManager::getAnchorWorldPos(seg.vertA);
                    glm::vec3 worldB = SculptManager::getAnchorWorldPos(seg.vertB);
                    float worldDist = glm::distance(worldA, worldB);

                    // Column 1: Name
                    ImGui::TableNextColumn();
                    std::string displayName = seg.name.empty() ? ("Measure " + std::to_string(i + 1)) : seg.name;
                    if (renameSegType == 1 && renameSegIdx == i) {
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        if (ImGui::InputText("##RenameMeasure", renameSegBuf, sizeof(renameSegBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                            seg.name = renameSegBuf;
                            renameSegIdx = -1;
                        }
                        if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit()) {
                            seg.name = renameSegBuf;
                            renameSegIdx = -1;
                        }
                    } else {
                        if (ImGui::Selectable(displayName.c_str(), false)) {
                            sculpt.setTool(BRUSH_MEASURE);
                        }
                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                            renameSegType = 1;
                            renameSegIdx = i;
                            strncpy(renameSegBuf, displayName.c_str(), sizeof(renameSegBuf));
                        }
                    }

                    // Column 2: Value / Ratio
                    ImGui::TableNextColumn();
                    if (seg.isReference) {
                        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.4f, 1.0f), "1.00x");
                    } else if (referenceLength > 0.0f) {
                        ImGui::Text("%.2fx", worldDist / referenceLength);
                    } else {
                        ImGui::Text("%.2f", worldDist);
                    }

                    // Column 3: Ref Checkbox
                    ImGui::TableNextColumn();
                    bool isRef = seg.isReference;
                    if (ImGui::Checkbox("##RefCheck", &isRef)) {
                        if (isRef) {
                            for (auto& s : measureSegs) s.isReference = false;
                            seg.isReference = true;
                        }
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Set as reference scale unit");
                    }

                    // Column 4: Delete Button
                    ImGui::TableNextColumn();
                    if (ImGui::Button("X##DelM", ImVec2(20, 0))) {
                        deleteMeasureIdx = i;
                    }

                    ImGui::PopID();
                }

                // Divider Segments
                for (int j = 0; j < (int)dividerSegs.size(); ++j) {
                    auto& seg = dividerSegs[j];
                    ImGui::TableNextRow();
                    ImGui::PushID(2000 + j);

                    glm::vec3 worldA = SculptManager::getAnchorWorldPos(seg.vertA);
                    glm::vec3 worldB = SculptManager::getAnchorWorldPos(seg.vertB);
                    float worldDist = glm::distance(worldA, worldB);

                    // Column 1: Name
                    ImGui::TableNextColumn();
                    std::string displayName = seg.name.empty() ? ("Divider " + std::to_string(j + 1)) : seg.name;
                    if (renameSegType == 2 && renameSegIdx == j) {
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        if (ImGui::InputText("##RenameDivider", renameSegBuf, sizeof(renameSegBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                            seg.name = renameSegBuf;
                            renameSegIdx = -1;
                        }
                        if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit()) {
                            seg.name = renameSegBuf;
                            renameSegIdx = -1;
                        }
                    } else {
                        if (ImGui::Selectable(displayName.c_str(), false)) {
                            sculpt.setTool(BRUSH_DIVIDER);
                        }
                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                            renameSegType = 2;
                            renameSegIdx = j;
                            strncpy(renameSegBuf, displayName.c_str(), sizeof(renameSegBuf));
                        }
                    }

                    // Column 2: Value / Divisions
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f (%dd)", worldDist, sculpt.getDividerDivisions());

                    // Column 3: Ref Checkbox (N/A)
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("-");

                    // Column 4: Delete Button
                    ImGui::TableNextColumn();
                    if (ImGui::Button("X##DelD", ImVec2(20, 0))) {
                        deleteDividerIdx = j;
                    }

                    ImGui::PopID();
                }

                ImGui::EndTable();

                if (deleteMeasureIdx >= 0 && deleteMeasureIdx < (int)measureSegs.size()) {
                    bool wasRef = measureSegs[deleteMeasureIdx].isReference;
                    measureSegs.erase(measureSegs.begin() + deleteMeasureIdx);
                    if (wasRef && !measureSegs.empty()) {
                        measureSegs[0].isReference = true;
                    }
                }

                if (deleteDividerIdx >= 0 && deleteDividerIdx < (int)dividerSegs.size()) {
                    dividerSegs.erase(dividerSegs.begin() + deleteDividerIdx);
                }
            }
            ImGui::EndChild();
        }

        ImGui::End();
    }

    // 5. Topology & Remesh settings panel
    if (m_showTopologyPanel) {
        ImGui::SetNextWindowPos({740.0f * scale, 40.0f * scale}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({280.0f * scale, 200.0f * scale}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Topology & Remesh", &m_showTopologyPanel, ImGuiWindowFlags_AlwaysAutoResize);

        Mesh* selectedMesh = scene.getSelected();
        if (selectedMesh) {
            ImGui::Text("Vertices: %d", selectedMesh->nbVerts);
            ImGui::Text("Faces: %d", selectedMesh->nbFaces);
            ImGui::Separator();

            ImGui::SliderFloat("Detail Factor", &m_dyntopoDetail, 10.0f, 500.0f, "%.1f");
            ImGui::SliderInt("Remesh Resolution", &m_remeshResolution, 10, 1000);
            if (ImGui::IsItemActive()) {
                float bbox[6];
                selectedMesh->computeBbox(bbox);
                float maxDim = std::max({bbox[3] - bbox[0], bbox[4] - bbox[1], bbox[5] - bbox[2]});
                float step = maxDim / (float)m_remeshResolution;
                scene.updateVoxelPreview(step, {selectedMesh});
            } else if (ImGui::IsItemDeactivated()) {
                scene.updateVoxelPreview(0.0f, {});
            }

            ImGui::Checkbox("Keep PolyGroups", &m_remeshKeepPolyGroups);
            ImGui::Checkbox("Align Symmetry Axes", &m_remeshAlignSymmetry);

            if (ImGui::Button("Remesh", ImVec2(-1, 0))) {
                std::cout << "[Topology] Trigger remesh with resolution: " << m_remeshResolution << std::endl;
                performRemesh(scene);
            }
        } else {
            ImGui::Text("No active mesh selected");
        }

        ImGui::End();
    }

    // 7. Reference Images Panel
    if (m_showReferenceImagesPanel) {
        ImGui::SetNextWindowPos({500.0f * scale, 40.0f * scale}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({300.0f * scale, 250.0f * scale}, ImGuiCond_FirstUseEver);
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

    // 8. Gizmo Cube Window
    if (m_showGizmoCube) {
        int wWidth, wHeight;
        SDL_GetWindowSize(window, &wWidth, &wHeight);

        // Position it at the top-right corner, below the main menu bar
        ImGui::SetNextWindowPos(ImVec2((float)wWidth - 150.0f * scale, 40.0f * scale), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(130.0f * scale, 130.0f * scale));
        
        ImGui::Begin("Gizmo Cube", nullptr, 
            ImGuiWindowFlags_NoTitleBar | 
            ImGuiWindowFlags_NoResize | 
            ImGuiWindowFlags_NoScrollbar | 
            ImGuiWindowFlags_NoBackground | 
            ImGuiWindowFlags_NoSavedSettings);

        Camera& camera = scene.getCamera();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        
        // Cube half size in pixels on screen
        float side = 36.0f * scale;
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
        ImGuiViewport* viewport = ImGui::GetMainViewport();

        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.75f);

        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.09f, 0.10f, 0.80f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.01f, 0.52f, 0.45f, 0.40f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * scale, 6.0f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * scale, 2.0f * scale));

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoMove;

        if (ImGui::Begin("Mesh Statistics HUD", nullptr, flags)) {
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

            if (ImGui::BeginTable("##MeshStatsHUDTable", 2, ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 60.0f * scale);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Active points");
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "%s", formatCount(activePoints).c_str());

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Total points");
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "%s", formatCount(totalPoints).c_str());

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("FPS");
                ImGui::TableNextColumn();
                if (m_fpsValue > 0) {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "%d", m_fpsValue);
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "--");
                }

                ImGui::EndTable();
            }
        }
        ImGui::End();
        
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    }

    drawHotkeyHUD();

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
        ImGui::ProgressBar(diag.currentPressure, ImVec2(-1, 20.0f * scale), "");
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
        {
            ImGui::Text("Pressure Curve Editing:");
            ImGui::TextDisabled("Double-click to add/remove points.");
            ImGui::TextDisabled("Right-click points to delete.");

            // Presets
            static int selectedPreset = 0; // 0: Custom, 1: Linear, 2: Soft, 3: Hard
            const char* presets[] = { "Custom", "Linear", "Soft", "Hard" };
            if (ImGui::Combo("Curve Preset", &selectedPreset, presets, 4)) {
                if (selectedPreset == 1) {
                    g_tablet.setPressureCurve({{0.0f, 0.0f}, {1.0f, 1.0f}});
                } else if (selectedPreset == 2) {
                    g_tablet.setPressureCurve({{0.0f, 0.0f}, {0.25f, 0.45f}, {0.5f, 0.75f}, {0.75f, 0.9f}, {1.0f, 1.0f}});
                } else if (selectedPreset == 3) {
                    g_tablet.setPressureCurve({{0.0f, 0.0f}, {0.25f, 0.05f}, {0.5f, 0.25f}, {0.75f, 0.6f}, {1.0f, 1.0f}});
                }
            }

            int interpMode = (int)g_tablet.getInterpolationType();
            const char* interpModes[] = { "Linear", "Monotone Spline", "Centripetal Catmull-Rom" };
            if (ImGui::Combo("Interpolation Mode", &interpMode, interpModes, 3)) {
                g_tablet.setInterpolationType((TabletInput::InterpolationType)interpMode);
                selectedPreset = 0; // mark custom / modified
            }

            // Custom widget rendering
            ImVec2 curveCanvasSize = ImVec2(240.0f, 240.0f);
            ImVec2 curveCanvasPos = ImGui::GetCursorScreenPos();
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            // Canvas background and border
            drawList->AddRectFilled(curveCanvasPos, ImVec2(curveCanvasPos.x + curveCanvasSize.x, curveCanvasPos.y + curveCanvasSize.y), IM_COL32(18, 18, 22, 255));
            drawList->AddRect(curveCanvasPos, ImVec2(curveCanvasPos.x + curveCanvasSize.x, curveCanvasPos.y + curveCanvasSize.y), IM_COL32(75, 75, 85, 255));

            // Draw grid
            for (int i = 1; i < 4; ++i) {
                float gx = curveCanvasPos.x + curveCanvasSize.x * (0.25f * i);
                float gy = curveCanvasPos.y + curveCanvasSize.y * (0.25f * i);
                drawList->AddLine(ImVec2(gx, curveCanvasPos.y), ImVec2(gx, curveCanvasPos.y + curveCanvasSize.y), IM_COL32(45, 45, 55, 255), 1.0f);
                drawList->AddLine(ImVec2(curveCanvasPos.x, gy), ImVec2(curveCanvasPos.x + curveCanvasSize.x, gy), IM_COL32(45, 45, 55, 255), 1.0f);
            }

            // Draw axis labels and watermarks
            drawList->AddText(ImVec2(curveCanvasPos.x + 10.0f, curveCanvasPos.y + 10.0f), IM_COL32(255, 255, 255, 100), "Output (Pressure / Size)");
            drawList->AddText(ImVec2(curveCanvasPos.x + curveCanvasSize.x - 135.0f, curveCanvasPos.y + curveCanvasSize.y - 22.0f), IM_COL32(255, 255, 255, 100), "Input (Pen Force)");

            // Draw coordinate values
            drawList->AddText(ImVec2(curveCanvasPos.x + 5.0f, curveCanvasPos.y + 25.0f), IM_COL32(255, 255, 255, 80), "1.0");
            drawList->AddText(ImVec2(curveCanvasPos.x + 5.0f, curveCanvasPos.y + curveCanvasSize.y - 35.0f), IM_COL32(255, 255, 255, 80), "0.0");
            drawList->AddText(ImVec2(curveCanvasPos.x + curveCanvasSize.x - 25.0f, curveCanvasPos.y + curveCanvasSize.y - 35.0f), IM_COL32(255, 255, 255, 80), "1.0");

            // Handle interaction and sorting
            std::vector<TabletInput::CurvePoint> pts = g_tablet.getPressureCurve();
            
            auto toScreen = [&](const TabletInput::CurvePoint& p) -> ImVec2 {
                return ImVec2(curveCanvasPos.x + p.x * curveCanvasSize.x, curveCanvasPos.y + (1.0f - p.y) * curveCanvasSize.y);
            };
            
            auto toCurve = [&](const ImVec2& screenPos) -> TabletInput::CurvePoint {
                float cx = (screenPos.x - curveCanvasPos.x) / curveCanvasSize.x;
                float cy = 1.0f - (screenPos.y - curveCanvasPos.y) / curveCanvasSize.y;
                cx = cx < 0.0f ? 0.0f : (cx > 1.0f ? 1.0f : cx);
                cy = cy < 0.0f ? 0.0f : (cy > 1.0f ? 1.0f : cy);
                return { cx, cy };
            };

            ImGui::InvisibleButton("pressure_curve_canvas", curveCanvasSize);
            bool canvasHovered = ImGui::IsItemHovered();
            bool canvasActive = ImGui::IsItemActive();
            static int draggedIdx = -1;

            if (canvasActive && ImGui::IsMouseDown(0)) {
                ImVec2 mPos = ImGui::GetIO().MousePos;
                if (draggedIdx == -1) {
                    // Find closest point to drag
                    float bestDist = 15.0f; // tolerance
                    int bestIdx = -1;
                    for (int i = 0; i < (int)pts.size(); ++i) {
                        ImVec2 screenPt = toScreen(pts[i]);
                        float dx = mPos.x - screenPt.x;
                        float dy = mPos.y - screenPt.y;
                        float dist = std::sqrt(dx*dx + dy*dy);
                        if (dist < bestDist) {
                            bestDist = dist;
                            bestIdx = i;
                        }
                    }
                    draggedIdx = bestIdx;
                }

                if (draggedIdx != -1) {
                    TabletInput::CurvePoint np = toCurve(mPos);
                    if (draggedIdx == 0) {
                        pts[0].y = np.y;
                    } else if (draggedIdx == (int)pts.size() - 1) {
                        pts[pts.size() - 1].y = np.y;
                    } else {
                        float minX = pts[draggedIdx - 1].x + 0.01f;
                        float maxX = pts[draggedIdx + 1].x - 0.01f;
                        pts[draggedIdx].x = np.x < minX ? minX : (np.x > maxX ? maxX : np.x);
                        pts[draggedIdx].y = np.y;
                    }
                    g_tablet.setPressureCurve(pts);
                    selectedPreset = 0; // custom now
                }
            } else {
                draggedIdx = -1;
            }

            // Add/remove points on double-click
            if (canvasHovered && ImGui::IsMouseDoubleClicked(0)) {
                ImVec2 mPos = ImGui::GetIO().MousePos;
                int clickedIdx = -1;
                for (int i = 0; i < (int)pts.size(); ++i) {
                    ImVec2 screenPt = toScreen(pts[i]);
                    float dx = mPos.x - screenPt.x;
                    float dy = mPos.y - screenPt.y;
                    if (dx*dx + dy*dy < 10.0f * 10.0f) {
                        clickedIdx = i;
                        break;
                    }
                }
                if (clickedIdx != -1) {
                    if (clickedIdx > 0 && clickedIdx < (int)pts.size() - 1) {
                        pts.erase(pts.begin() + clickedIdx);
                        g_tablet.setPressureCurve(pts);
                        selectedPreset = 0;
                    }
                } else {
                    TabletInput::CurvePoint newPt = toCurve(mPos);
                    pts.push_back(newPt);
                    std::sort(pts.begin(), pts.end(), [](const TabletInput::CurvePoint& a, const TabletInput::CurvePoint& b) {
                        return a.x < b.x;
                    });
                    g_tablet.setPressureCurve(pts);
                    selectedPreset = 0;
                }
            }

            // Delete point on right click
            if (canvasHovered && ImGui::IsMouseClicked(1)) {
                ImVec2 mPos = ImGui::GetIO().MousePos;
                for (int i = 1; i < (int)pts.size() - 1; ++i) {
                    ImVec2 screenPt = toScreen(pts[i]);
                    float dx = mPos.x - screenPt.x;
                    float dy = mPos.y - screenPt.y;
                    if (dx*dx + dy*dy < 10.0f * 10.0f) {
                        pts.erase(pts.begin() + i);
                        g_tablet.setPressureCurve(pts);
                        selectedPreset = 0;
                        break;
                    }
                }
            }

            // Draw curve lines (linear or smooth)
            if (g_tablet.isSplineEnabled()) {
                const int numSegments = 100;
                ImVec2 prevPt = toScreen({ 0.0f, g_tablet.evaluateCurve(0.0f) });
                for (int s = 1; s <= numSegments; ++s) {
                    float sx = (float)s / (float)numSegments;
                    float sy = g_tablet.evaluateCurve(sx);
                    ImVec2 nextPt = toScreen({ sx, sy });
                    drawList->AddLine(prevPt, nextPt, IM_COL32(0, 192, 255, 255), 2.5f);
                    prevPt = nextPt;
                }
            } else {
                for (size_t i = 0; i < pts.size() - 1; ++i) {
                    drawList->AddLine(toScreen(pts[i]), toScreen(pts[i + 1]), IM_COL32(0, 192, 255, 255), 2.5f);
                }
            }

            // Draw point handles
            ImVec2 mPos = ImGui::GetIO().MousePos;
            for (int i = 0; i < (int)pts.size(); ++i) {
                ImVec2 screenPt = toScreen(pts[i]);
                float dx = mPos.x - screenPt.x;
                float dy = mPos.y - screenPt.y;
                bool pCtrlHovered = (dx*dx + dy*dy < 8.0f * 8.0f) && canvasHovered;
                bool pCtrlDragged = (draggedIdx == i);
                
                ImU32 col = (pCtrlDragged || pCtrlHovered) ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 192, 255, 255);
                float rad = (pCtrlDragged || pCtrlHovered) ? 6.0f : 4.0f;
                drawList->AddCircleFilled(screenPt, rad, col);
                drawList->AddCircle(screenPt, rad + 1.5f, IM_COL32(18, 18, 22, 255), 0, 1.5f);
            }

            // Real-time position tracking dot
            if (g_tablet.isAvailable() && g_tablet.isPenActive()) {
                float rawP = g_tablet.getPressureRaw();
                float outP = g_tablet.evaluateCurve(rawP);
                ImVec2 indPos = toScreen({ rawP, outP });
                drawList->AddLine(ImVec2(indPos.x, curveCanvasPos.y), ImVec2(indPos.x, curveCanvasPos.y + curveCanvasSize.y), IM_COL32(255, 255, 255, 60), 1.0f);
                drawList->AddLine(ImVec2(curveCanvasPos.x, indPos.y), ImVec2(curveCanvasPos.x + curveCanvasSize.x, indPos.y), IM_COL32(255, 255, 255, 60), 1.0f);
                drawList->AddCircleFilled(indPos, 6.0f, IM_COL32(255, 128, 0, 255));
                drawList->AddCircle(indPos, 8.0f, IM_COL32(255, 255, 255, 200), 0, 1.5f);
            }

            if (ImGui::Button("Reset Curve to Linear")) {
                g_tablet.setPressureCurve({{0.0f, 0.0f}, {1.0f, 1.0f}});
                selectedPreset = 1;
            }
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
            float step = 10.0f * scale;
            int numSteps = (int)(len / step);
            float ux = dx / len;
            float uy = dy / len;
            for (int i = 0; i < numSteps; ++i) {
                float tStart = i * step;
                float tEnd = tStart + 5.0f * scale;
                if (tEnd > len) tEnd = len;
                drawList->AddLine(
                    ImVec2(pA.x + ux * tStart, pA.y + uy * tStart),
                    ImVec2(pA.x + ux * tEnd, pA.y + uy * tEnd),
                    IM_COL32(0, 229, 255, 255),
                    2.0f * scale
                );
            }
        }

        ImVec2 mousePos = ImGui::GetMousePos();
        float distA = std::sqrt((mousePos.x - pA.x) * (mousePos.x - pA.x) + (mousePos.y - pA.y) * (mousePos.y - pA.y));
        float distB = std::sqrt((mousePos.x - pB.x) * (mousePos.x - pB.x) + (mousePos.y - pB.y) * (mousePos.y - pB.y));

        float radA = (distA < 20.0f * scale) ? 12.0f * scale : 8.0f * scale;
        float radB = (distB < 20.0f * scale) ? 12.0f * scale : 8.0f * scale;

        drawList->AddCircleFilled(pA, radA, IM_COL32(255, 255, 255, 255));
        drawList->AddCircle(pA, radA, IM_COL32(0, 229, 255, 255), 0, 2.0f * scale);

        drawList->AddCircleFilled(pB, radB, IM_COL32(0, 229, 255, 255));
        drawList->AddCircle(pB, radB, IM_COL32(255, 255, 255, 255), 0, 2.0f * scale);
    }

    // 10. Measure / Divider Overlays
    if (sculpt.getBrush() == BRUSH_MEASURE || sculpt.getBrush() == BRUSH_DIVIDER ||
        !sculpt.getMeasureSegments().empty() || !sculpt.getDividerSegments().empty()) {
        sculpt.validateSegments(scene);
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        bool useDistanceThickness = sculpt.getMeasureUseDistanceThickness();

        float viewportWidth = ImGui::GetIO().DisplaySize.x;
        float viewportHeight = ImGui::GetIO().DisplaySize.y;
        bool isSplit = (scene.getSplitMode() != Scene::SplitMode::OFF);
        int numViewports = isSplit ? 2 : 1;
        float halfW = viewportWidth * 0.5f;

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

        int divisions = sculpt.getDividerDivisions();

        // Find reference length for Measure
        float referenceLength = 0.0f;
        for (const auto& seg : sculpt.getMeasureSegments()) {
            if (seg.isReference) {
                glm::vec3 worldA = SculptManager::getAnchorWorldPos(seg.vertA);
                glm::vec3 worldB = SculptManager::getAnchorWorldPos(seg.vertB);
                referenceLength = glm::distance(worldA, worldB);
                break;
            }
        }

        for (int vp = 0; vp < numViewports; ++vp) {
            bool showMeasure = sculpt.isMeasureVisible(vp);
            bool showDivider = sculpt.isDividerVisible(vp);
            if (!showMeasure && !showDivider) continue;

            const Camera* camPtr = isSplit ? scene.getCameraByIndex(vp) : &scene.getCamera();
            if (!camPtr) continue;
            const Camera& camera = *camPtr;
            float xOffset = (isSplit && vp == 1) ? halfW : 0.0f;
            float vpWidth = isSplit ? halfW : viewportWidth;

            if (isSplit) {
                drawList->PushClipRect(
                    ImVec2(xOffset, 0.0f),
                    ImVec2(xOffset + vpWidth, viewportHeight),
                    true
                );
            }

            auto drawSeg = [&](const MeasurementSegment& seg, bool isReference, bool isPreview, bool isDivider) {
                glm::vec3 worldA = SculptManager::getAnchorWorldPos(seg.vertA);
                glm::vec3 worldB = SculptManager::getAnchorWorldPos(seg.vertB);

                glm::vec3 screenA = camera.project(worldA);
                glm::vec3 screenB = camera.project(worldB);
                ImVec2 posA(screenA.x + xOffset, screenA.y);
                ImVec2 posB(screenB.x + xOffset, screenB.y);

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
                    strokeWidth = glm::clamp(strokeWidth, 0.25f * scale, 5.0f * scale);

                    float ppuA = getPixelsPerUnit(worldA, camera);
                    rA = (isReference ? 0.35f : 0.28f) * ppuA;
                    rA = glm::clamp(rA, 1.0f * scale, 15.0f * scale);

                    float ppuB = getPixelsPerUnit(worldB, camera);
                    rB = (isReference ? 0.35f : 0.28f) * ppuB;
                    rB = glm::clamp(rB, 1.0f * scale, 15.0f * scale);

                    rDiv = 0.2f * ppuMid;
                    rDiv = glm::clamp(rDiv, 0.8f * scale, 10.0f * scale);
                } else {
                    strokeWidth *= scale;
                    rA *= scale;
                    rB *= scale;
                    rDiv *= scale;
                }

                if (isHoveredA) rA = std::max(8.0f * scale, rA * 1.6f);
                if (isHoveredB) rB = std::max(8.0f * scale, rB * 1.6f);

                // 1. Line
                if (isPreview) {
                    ImVec2 d = ImVec2(posB.x - posA.x, posB.y - posA.y);
                    float len = std::sqrt(d.x * d.x + d.y * d.y);
                    if (len > 0.0f) {
                        float step = 10.0f * scale;
                        int numSteps = (int)(len / step);
                        float ux = d.x / len;
                        float uy = d.y / len;
                        for (int i = 0; i < numSteps; ++i) {
                            float tStart = i * step;
                            float tEnd = tStart + 5.0f * scale;
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
                            ImVec2 divPos(divScreen.x + xOffset, divScreen.y);
                            ImU32 fillCol = isPreview ? IM_COL32(255, 255, 255, 102) : IM_COL32(255, 255, 255, 255);
                            ImU32 strokeCol = isPreview ? IM_COL32(26, 26, 26, 102) : IM_COL32(26, 26, 26, 255);
                            drawList->AddCircleFilled(divPos, rDiv, fillCol);
                            drawList->AddCircle(divPos, rDiv, strokeCol, 0, 1.0f * scale);
                        }
                    }
                } else {
                    if (!isReference && referenceLength > 0.0f) {
                        int nTicks = (int)std::floor((worldDist - 1e-5f) / referenceLength);
                        for (int k = 1; k <= nTicks; ++k) {
                            float t = (k * referenceLength) / worldDist;
                            glm::vec3 tickWorld = glm::mix(worldA, worldB, t);
                            glm::vec3 tickScreen = camera.project(tickWorld);
                            ImVec2 tickPos(tickScreen.x + xOffset, tickScreen.y);
                            drawList->AddCircleFilled(tickPos, 2.5f * scale, IM_COL32(255, 255, 255, 255));
                            drawList->AddCircle(tickPos, 2.5f * scale, color, 0, 1.0f * scale);
                        }
                    }
                }

                // 3. Endpoint shapes
                ImU32 strokeA = isHoveredA ? IM_COL32(0, 229, 255, 255) : IM_COL32(26, 26, 26, 255);
                float swA = isHoveredA ? 2.5f * scale : 1.2f * scale;
                drawEndpointShape(drawList, posA, rA, seg.vertA.type, color, strokeA, swA);

                ImU32 strokeB = isHoveredB ? IM_COL32(0, 229, 255, 255) : IM_COL32(26, 26, 26, 255);
                float swB = isHoveredB ? 2.5f * scale : 1.2f * scale;
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
                    float textWidth = labelSize.x + 12.0f * scale;
                    float textHeight = labelSize.y + 6.0f * scale;

                    ImVec2 midPos((posA.x + posB.x) * 0.5f, (posA.y + posB.y) * 0.5f - 10.0f * scale);
                    ImVec2 minRect(midPos.x - textWidth * 0.5f, midPos.y - textHeight * 0.5f);
                    ImVec2 maxRect(midPos.x + textWidth * 0.5f, midPos.y + textHeight * 0.5f);

                    drawList->AddRectFilled(minRect, maxRect, IM_COL32(20, 20, 20, 217), 4.0f * scale);
                    drawList->AddRect(minRect, maxRect, color, 4.0f * scale, 0, 1.0f * scale);

                    ImVec2 textPos(midPos.x - labelSize.x * 0.5f, midPos.y - labelSize.y * 0.5f);
                    drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), label);
                }
            };

            // Draw Measure segments if visible in vp
            if (showMeasure) {
                for (const auto& s : sculpt.getMeasureSegments()) {
                    drawSeg(s, s.isReference, false, false);
                }
                if (sculpt.getBrush() == BRUSH_MEASURE && sculpt.hasPending()) {
                    MeasurementSegment pendingSeg;
                    pendingSeg.vertA = sculpt.getPendingAnchorA();
                    pendingSeg.vertB = sculpt.getPendingAnchorB();
                    bool isPendingRef = true;
                    for (const auto& s : sculpt.getMeasureSegments()) {
                        if (s.isReference) { isPendingRef = false; break; }
                    }
                    drawSeg(pendingSeg, isPendingRef, true, false);
                }
            }

            // Draw Divider segments if visible in vp
            if (showDivider) {
                for (const auto& s : sculpt.getDividerSegments()) {
                    drawSeg(s, false, false, true);
                }
                if (sculpt.getBrush() == BRUSH_DIVIDER && sculpt.hasPending()) {
                    MeasurementSegment pendingSeg;
                    pendingSeg.vertA = sculpt.getPendingAnchorA();
                    pendingSeg.vertB = sculpt.getPendingAnchorB();
                    drawSeg(pendingSeg, false, true, true);
                }
            }

            if (isSplit) {
                drawList->PopClipRect();
            }
        }
    }



    // 11. Transform Gizmo (ImGuizmo)
    if (sculpt.getBrush() == BRUSH_ARMATURE_SPHERES) {
        if (auto* tool = sculpt.getArmatureTool()) {
            renderer.setArmatureState(tool->getSelectedNode(), tool->getHoveredLinkParent(), tool->getHoveredLinkChild(), sculpt.getUseSym());
        }
    } else {
        renderer.setArmatureState(nullptr, nullptr, nullptr, sculpt.getUseSym());
    }

    if (sculpt.getBrush() == BRUSH_TRANSFORM) {
        Mesh* selectedMesh = scene.getSelected();
        const Camera& camera = scene.getCamera();
        if (selectedMesh) {
            ImGuizmo::BeginFrame();
            ImGuizmo::SetOrthographic(camera.isOrthographic());
            ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
            ImGuizmo::SetRect(0.0f, 0.0f, (float)camera.getWidth(), (float)camera.getHeight());
            ImGuizmo::AllowAxisFlip(false);

            bool cameraDragging = sculpt.getCameraController().isDragging();
            ImGuizmo::Enable(!cameraDragging);
            ImGuizmo::SetGizmoSizeClipSpace(m_gizmoSize);

            glm::mat4 view = camera.getViewMatrix();
            glm::mat4 proj = camera.getProjMatrix();
            glm::mat4 matrix = selectedMesh->matrix;

            static bool wasUsingGizmo = false;
            static bool draggedPivot = false;
            static glm::mat4 pivotStartMatrix = glm::mat4(1.0f);
            static glm::vec3 transformPivotStartLocal = glm::vec3(0.0f);
            static std::vector<float> transformStartVerts;
            static std::vector<float> transformStartNormals;
            static bool transformHasMask = false;

            bool isUsingGizmo = ImGuizmo::IsUsing();
            bool isMovingPivot = m_editPivot || ImGui::GetIO().KeyAlt;

            if (isUsingGizmo && !wasUsingGizmo) {
                scene.pushHistoryState();
                pivotStartMatrix = selectedMesh->matrix;
                draggedPivot = isMovingPivot;

                transformStartVerts = selectedMesh->verts;
                transformStartNormals = selectedMesh->normals;
                transformHasMask = false;

                glm::vec4 pivotWorld = matrix[3];
                transformPivotStartLocal = glm::vec3(glm::inverse(pivotStartMatrix) * pivotWorld);

                if (!draggedPivot && !selectedMesh->materials.empty()) {
                    for (int i = 0; i < selectedMesh->nbVerts; ++i) {
                        if (selectedMesh->materials[i * 3 + 2] < 0.999f) {
                            transformHasMask = true;
                            break;
                        }
                    }
                }
            }

            ImGuizmo::OPERATION op = ImGuizmo::UNIVERSAL;

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
            style.TranslationLineArrowSize = 5.0f;
            style.RotationLineThickness = 3.5f;
            style.RotationOuterLineThickness = 4.0f;
            style.ScaleLineThickness = 3.5f;
            style.ScaleLineCircleSize = 4.5f;
            style.CenterCircleSize = 4.5f;

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
                    bool useSymTransform = sculpt.getUseSym();
                    if ((transformHasMask || useSymTransform) && !transformStartVerts.empty() && transformStartVerts.size() == selectedMesh->verts.size()) {
                        selectedMesh->matrix = pivotStartMatrix;
                        glm::mat4 deltaLocal = glm::inverse(pivotStartMatrix) * matrix;
                        glm::mat3 normalMatrixPrimary = glm::transpose(glm::inverse(glm::mat3(deltaLocal)));

                        bool symX = sculpt.getSymX();
                        bool symY = sculpt.getSymY();
                        bool symZ = sculpt.getSymZ();

                        // Compute mesh bounding box in transformStartVerts coordinates
                        float bbox[6];
                        selectedMesh->computeBbox(bbox);
                        float meshCenterX = (bbox[0] + bbox[1]) * 0.5f;
                        float meshCenterY = (bbox[2] + bbox[3]) * 0.5f;
                        float meshCenterZ = (bbox[4] + bbox[5]) * 0.5f;

                        // Pivot location relative to mesh symmetry origin (meshCenter)
                        glm::vec3 d_pivot(
                            -meshCenterX,
                            -meshCenterY,
                            -meshCenterZ
                        );

                        float diagLen = std::sqrt(
                            (bbox[1] - bbox[0]) * (bbox[1] - bbox[0]) +
                            (bbox[3] - bbox[2]) * (bbox[3] - bbox[2]) +
                            (bbox[5] - bbox[4]) * (bbox[5] - bbox[4])
                        );
                        float blendEps = std::max(0.005f, 0.01f * diagLen);

                        // Symmetry reflection matrix across active symmetry plane in transformStartVerts local space
                        SymmetryMode symMode = sculpt.getSymmetryMode();
                        glm::vec3 P_plane(meshCenterX, meshCenterY, meshCenterZ);
                        if (symMode == SymmetryMode::Local && selectedMesh) {
                            int axis = sculpt.getSymAxis();
                            P_plane = selectedMesh->getSymmetryOriginForAxis(axis, SymmetryMode::Local);
                        } else if (symMode == SymmetryMode::World && selectedMesh) {
                            int axis = sculpt.getSymAxis();
                            P_plane = selectedMesh->getSymmetryOriginForAxis(axis, SymmetryMode::World);
                        }

                        glm::mat4 S_axis(1.0f);
                        if (symX) S_axis[0][0] = -1.0f;
                        if (symY) S_axis[1][1] = -1.0f;
                        if (symZ) S_axis[2][2] = -1.0f;

                        glm::mat4 deltaSym;
                        if (symMode == SymmetryMode::World) {
                            glm::mat4 invM0 = glm::inverse(pivotStartMatrix);
                            glm::mat4 S_world(1.0f);
                            if (symX) S_world[0][0] = -1.0f;
                            if (symY) S_world[1][1] = -1.0f;
                            if (symZ) S_world[2][2] = -1.0f;
                            glm::mat4 S_local = invM0 * S_world * pivotStartMatrix;
                            deltaSym = S_local * deltaLocal * S_local;
                        } else {
                            glm::mat4 T_plane = glm::translate(glm::mat4(1.0f), P_plane);
                            glm::mat4 T_plane_inv = glm::translate(glm::mat4(1.0f), -P_plane);
                            glm::mat4 M_sym = T_plane * S_axis * T_plane_inv;
                            deltaSym = M_sym * deltaLocal * M_sym;
                        }
                        glm::mat3 normalMatrixSym = glm::transpose(glm::inverse(glm::mat3(deltaSym)));

                        int nbVerts = selectedMesh->nbVerts;
                        for (int i = 0; i < nbVerts; ++i) {
                            float m = (transformHasMask && selectedMesh->materials.size() == (size_t)nbVerts * 3)
                                      ? selectedMesh->materials[i * 3 + 2]
                                      : 1.0f;

                            if (m <= 0.001f && !useSymTransform) {
                                selectedMesh->verts[i * 3]     = transformStartVerts[i * 3];
                                selectedMesh->verts[i * 3 + 1] = transformStartVerts[i * 3 + 1];
                                selectedMesh->verts[i * 3 + 2] = transformStartVerts[i * 3 + 2];
                                selectedMesh->normals[i * 3]     = transformStartNormals[i * 3];
                                selectedMesh->normals[i * 3 + 1] = transformStartNormals[i * 3 + 1];
                                selectedMesh->normals[i * 3 + 2] = transformStartNormals[i * 3 + 2];
                                continue;
                            }

                            glm::vec4 vStart(
                                transformStartVerts[i * 3],
                                transformStartVerts[i * 3 + 1],
                                transformStartVerts[i * 3 + 2],
                                1.0f
                            );
                            glm::vec3 nStart(
                                transformStartNormals[i * 3],
                                transformStartNormals[i * 3 + 1],
                                transformStartNormals[i * 3 + 2]
                            );

                            glm::vec4 vTransformed;
                            glm::vec3 nTransformed;

                            if (useSymTransform) {
                                // Compute distance from symmetry plane for each active axis
                                float d_vx = vStart.x - P_plane.x;
                                float d_vy = vStart.y - P_plane.y;
                                float d_vz = vStart.z - P_plane.z;
                                if (symMode == SymmetryMode::World && selectedMesh) {
                                    glm::vec4 vWorld = pivotStartMatrix * vStart;
                                    d_vx = vWorld.x;
                                    d_vy = vWorld.y;
                                    d_vz = vWorld.z;
                                }

                                bool isPrimaryX = !symX || ((d_pivot.x >= 0.0f) ? (d_vx >= 0.0f) : (d_vx < 0.0f));
                                bool isPrimaryY = !symY || ((d_pivot.y >= 0.0f) ? (d_vy >= 0.0f) : (d_vy < 0.0f));
                                bool isPrimaryZ = !symZ || ((d_pivot.z >= 0.0f) ? (d_vz >= 0.0f) : (d_vz < 0.0f));

                                bool isPrimaryVert = isPrimaryX && isPrimaryY && isPrimaryZ;
                                float weightP = isPrimaryVert ? 1.0f : 0.0f;

                                if (symX && std::abs(d_vx) < blendEps) {
                                    float t = 0.5f + 0.5f * (d_vx / blendEps);
                                    if (d_pivot.x < 0.0f) t = 1.0f - t;
                                    weightP = t;
                                }
                                if (symY && std::abs(d_vy) < blendEps) {
                                    float t = 0.5f + 0.5f * (d_vy / blendEps);
                                    if (d_pivot.y < 0.0f) t = 1.0f - t;
                                    weightP *= t;
                                }
                                if (symZ && std::abs(d_vz) < blendEps) {
                                    float t = 0.5f + 0.5f * (d_vz / blendEps);
                                    if (d_pivot.z < 0.0f) t = 1.0f - t;
                                    weightP *= t;
                                }

                                weightP = std::max(0.0f, std::min(1.0f, weightP));

                                glm::vec4 vP = deltaLocal * vStart;
                                glm::vec3 nP = normalMatrixPrimary * nStart;

                                glm::vec4 vS = deltaSym * vStart;
                                glm::vec3 nS = normalMatrixSym * nStart;

                                vTransformed = weightP * vP + (1.0f - weightP) * vS;
                                nTransformed = glm::normalize(weightP * nP + (1.0f - weightP) * nS);
                            } else {
                                vTransformed = deltaLocal * vStart;
                                nTransformed = normalMatrixPrimary * nStart;
                            }

                            glm::vec4 vNew = m * vTransformed + (1.0f - m) * vStart;
                            glm::vec3 nNew = glm::normalize(m * nTransformed + (1.0f - m) * nStart);

                            selectedMesh->verts[i * 3]     = vNew.x;
                            selectedMesh->verts[i * 3 + 1] = vNew.y;
                            selectedMesh->verts[i * 3 + 2] = vNew.z;
                            selectedMesh->normals[i * 3]     = nNew.x;
                            selectedMesh->normals[i * 3 + 1] = nNew.y;
                            selectedMesh->normals[i * 3 + 2] = nNew.z;
                        }

                        selectedMesh->isVertexDirty = true;
                        selectedMesh->dirtyVertMin = 0;
                        selectedMesh->dirtyVertMax = nbVerts - 1;
                    } else {
                        selectedMesh->matrix = matrix;
                    }
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
                } else if (transformHasMask || sculpt.getUseSym()) {
                    selectedMesh->matrix = pivotStartMatrix;
                    selectedMesh->postInit();
                    selectedMesh->isDirty = true;
                }
                draggedPivot = false;
                transformHasMask = false;
                transformStartVerts.clear();
                transformStartVerts.shrink_to_fit();
                transformStartNormals.clear();
                transformStartNormals.shrink_to_fit();
            }
            wasUsingGizmo = isUsingGizmo;

            // Draw floating pivot lock button near the gizmo center
            glm::vec3 pivotWorldPos = glm::vec3(selectedMesh->matrix[3]);
            glm::vec3 screenPos = camera.project(pivotWorldPos);

            if (screenPos.z >= 0.0f && screenPos.z <= 1.0f &&
                screenPos.x >= 0.0f && screenPos.x <= (float)camera.getWidth() &&
                screenPos.y >= 0.0f && screenPos.y <= (float)camera.getHeight()) {
                
                float sizeRatio = m_gizmoSize / 0.10f;
                ImGui::SetNextWindowPos(ImVec2(screenPos.x + 50.0f * sizeRatio * scale, screenPos.y - 120.0f * sizeRatio * scale), ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.7f);
                ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | 
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(3.0f * scale, 3.0f * scale));
                if (ImGui::Begin("##PivotLockWindow", nullptr, flags)) {
                    bool activeMoving = m_editPivot || ImGui::GetIO().KeyAlt;
                    if (activeMoving) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.83f, 0.18f, 0.18f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.25f, 0.25f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.70f, 0.10f, 0.10f, 1.0f));
                        if (ImGui::Button(ICON_LC_UNLOCK "##unlock")) {
                            m_editPivot = false;
                        }
                        ImGui::PopStyleColor(3);
                    } else {
                        if (ImGui::Button(ICON_LC_LOCK "##lock")) {
                            m_editPivot = true;
                        }
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(activeMoving ? "Unlock Pivot (Alt)" : "Lock Pivot (Alt)");
                    }

                    auto applyTargetMatrix = [&](Mesh* mesh, const glm::mat4& targetMatrix, bool editPivot) {
                        scene.pushHistoryState();
                        if (editPivot) {
                            glm::mat4 deltaLocalMatrix = glm::inverse(mesh->matrix) * targetMatrix;
                            glm::mat4 deltaLocalMatrixInv = glm::inverse(deltaLocalMatrix);
                            glm::mat3 enMatrix = glm::transpose(glm::inverse(glm::mat3(deltaLocalMatrixInv)));
                            for (int i = 0; i < mesh->nbVerts; ++i) {
                                glm::vec4 pos(mesh->verts[i * 3], mesh->verts[i * 3 + 1], mesh->verts[i * 3 + 2], 1.0f);
                                glm::vec4 newPos = deltaLocalMatrixInv * pos;
                                mesh->verts[i * 3]     = newPos.x;
                                mesh->verts[i * 3 + 1] = newPos.y;
                                mesh->verts[i * 3 + 2] = newPos.z;

                                glm::vec3 normal(mesh->normals[i * 3], mesh->normals[i * 3 + 1], mesh->normals[i * 3 + 2]);
                                glm::vec3 newNormal = glm::normalize(enMatrix * normal);
                                mesh->normals[i * 3]     = newNormal.x;
                                mesh->normals[i * 3 + 1] = newNormal.y;
                                mesh->normals[i * 3 + 2] = newNormal.z;
                            }
                            mesh->matrix = targetMatrix;
                            mesh->postInit();
                            mesh->isDirty = true;
                        } else {
                            bool hasMask = false;
                            if (!mesh->materials.empty()) {
                                for (int i = 0; i < mesh->nbVerts; ++i) {
                                    if (mesh->materials[i * 3 + 2] < 0.999f) {
                                        hasMask = true;
                                        break;
                                    }
                                }
                            }
                            if (hasMask) {
                                glm::mat4 pivotStartMatrix = mesh->matrix;
                                glm::mat4 invMnew = glm::inverse(targetMatrix);
                                glm::mat4 startToNewLocal = invMnew * pivotStartMatrix;
                                glm::mat3 normalMatrixStartToNew = glm::transpose(glm::inverse(glm::mat3(startToNewLocal)));
                                for (int i = 0; i < mesh->nbVerts; ++i) {
                                    float m = mesh->materials[i * 3 + 2];
                                    if (m < 0.999f) {
                                        glm::vec4 vStart(mesh->verts[i * 3], mesh->verts[i * 3 + 1], mesh->verts[i * 3 + 2], 1.0f);
                                        glm::vec3 nStart(mesh->normals[i * 3], mesh->normals[i * 3 + 1], mesh->normals[i * 3 + 2]);
                                        glm::vec4 vTransformed = startToNewLocal * vStart;
                                        glm::vec4 vNew = (1.0f - m) * vTransformed + m * vStart;

                                        glm::vec3 nTransformed = normalMatrixStartToNew * nStart;
                                        glm::vec3 nNew = glm::normalize((1.0f - m) * nTransformed + m * nStart);

                                        mesh->verts[i * 3]     = vNew.x;
                                        mesh->verts[i * 3 + 1] = vNew.y;
                                        mesh->verts[i * 3 + 2] = vNew.z;
                                        mesh->normals[i * 3]     = nNew.x;
                                        mesh->normals[i * 3 + 1] = nNew.y;
                                        mesh->normals[i * 3 + 2] = nNew.z;
                                    }
                                }
                                mesh->matrix = targetMatrix;
                                mesh->postInit();
                            } else {
                                mesh->matrix = targetMatrix;
                            }
                            mesh->isDirty = true;
                        }
                    };

                    ImGui::SameLine();
                    if (ImGui::Button(ICON_LC_TARGET "##gotoaxis")) {
                        glm::mat4 targetMatrix = selectedMesh->matrix;
                        targetMatrix[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                        applyTargetMatrix(selectedMesh, targetMatrix, activeMoving);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Go to Axis (Move pivot/mesh to origin)");
                    }

                    ImGui::SameLine();
                    if (ImGui::Button(ICON_LC_ROTATE_3D "##resetorient")) {
                        float sx = glm::length(glm::vec3(selectedMesh->matrix[0]));
                        float sy = glm::length(glm::vec3(selectedMesh->matrix[1]));
                        float sz = glm::length(glm::vec3(selectedMesh->matrix[2]));
                        if (sx < 1e-6f) sx = 1.0f;
                        if (sy < 1e-6f) sy = 1.0f;
                        if (sz < 1e-6f) sz = 1.0f;

                        glm::mat4 targetMatrix = glm::mat4(1.0f);
                        targetMatrix[0] = glm::vec4(sx, 0.0f, 0.0f, 0.0f);
                        targetMatrix[1] = glm::vec4(0.0f, sy, 0.0f, 0.0f);
                        targetMatrix[2] = glm::vec4(0.0f, 0.0f, sz, 0.0f);
                        targetMatrix[3] = selectedMesh->matrix[3];
                        applyTargetMatrix(selectedMesh, targetMatrix, activeMoving);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Reset orientation (Align rotation to world axes)");
                    }
                }
                ImGui::End();
                ImGui::PopStyleVar();
            }
        }
    }

    drawRemeshProgressModal();

    // Render brush cursor using ImGui foreground draw list for beautiful antialiased lines
    if (renderer.getSmoothCursor()) {
        const auto& cursorState = sculpt.getCursor().getState();
        bool hideForUi = ImGui::GetIO().WantCaptureMouse;
        if (hideForUi) {
            ImGuiContext* g = GImGui;
            if (g && g->HoveredWindow && g->HoveredWindow->Name) {
                std::string_view winName(g->HoveredWindow->Name);
                if (winName.find("FloatingIsland") != std::string_view::npos ||
                    winName.find("hud") != std::string_view::npos ||
                    winName.find("HUD") != std::string_view::npos ||
                    winName.find("PivotLock") != std::string_view::npos) {
                    hideForUi = false;
                }
            }
        }
        if (cursorState.visible && !hideForUi) {
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
            float thickness = renderer.getCursorThickness() * scale;

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

    // Draw Camera Pivot Point if enabled and actively orbiting or zooming
    const Camera& camera = scene.getCamera();
    bool isOrbitingOrZooming = (sculpt.getCameraController().getDragMode() == CameraController::DragMode::Orbit || 
                                sculpt.getCameraController().getDragMode() == CameraController::DragMode::Roll ||
                                sculpt.getCameraController().getDragMode() == CameraController::DragMode::Zoom);
    if (camera.getUsePivot() && isOrbitingOrZooming) {
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        glm::vec3 pivotWorld = camera.getPivot();
        
        ImU32 pivotColor = IM_COL32(230, 50, 50, 240);
        
        auto drawPivotMarker = [&](ImVec2 p) {
            if (isPointOverImGuiWindow(p)) return;

            // Draw center dot
            drawList->AddCircleFilled(p, 2.0f * scale, pivotColor);
            // Draw outer ring
            drawList->AddCircle(p, 6.0f * scale, pivotColor, 0, 1.0f * scale);
            // Draw crosshair ticks
            drawList->AddLine(ImVec2(p.x - 10.0f * scale, p.y), ImVec2(p.x - 6.0f * scale, p.y), pivotColor, 1.0f * scale);
            drawList->AddLine(ImVec2(p.x + 6.0f * scale, p.y), ImVec2(p.x + 10.0f * scale, p.y), pivotColor, 1.0f * scale);
            drawList->AddLine(ImVec2(p.x, p.y - 10.0f * scale), ImVec2(p.x, p.y - 6.0f * scale), pivotColor, 1.0f * scale);
            drawList->AddLine(ImVec2(p.x, p.y + 6.0f * scale), ImVec2(p.x, p.y + 10.0f * scale), pivotColor, 1.0f * scale);
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

    if (renderer.getShowSafeFrames()) {
        drawSafeFramesOverlay(renderer, scene);
    }

    if (m_activeModalMode != ModalMode::NONE) {
        drawModalIndicatorHUD(sculpt, scene);
    }

    drawFloatingIslandHUD(sculpt, scene, renderer);
    drawModelSnapshotWindow(scene, renderer);
    drawUndoDiagPanel(scene);
    drawDebugLogPanel();
    drawTimelapsePanel(scene, renderer);
    drawPreferencesPanel(sculpt, scene, renderer, window);
    drawUnsavedChangesModal(scene, m_pendingQuit);
    updateWindowTitle(window, scene.isModified());

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void GuiManager::performRemesh(Scene& scene) {
    sculpt_log("[DEBUG performRemesh] Entered performRemesh. state = %d\n", (int)m_remeshAsync.state.load());
    if (m_remeshAsync.state == RemeshState::Running) {
        sculpt_log("[DEBUG performRemesh] state is Running, returning.\n");
        return;
    }

    Mesh* selectedMesh = scene.getSelected();
    sculpt_log("[DEBUG performRemesh] selectedMesh = %p\n", (void*)selectedMesh);
    if (!selectedMesh) {
        sculpt_log("[DEBUG performRemesh] selectedMesh is null, returning.\n");
        return;
    }

    m_remeshBeforeState = scene.saveCurrentState();

    // Snapshot mesh data for worker thread
    auto verts = selectedMesh->verts;
    auto faces = MeshUtils::triangulate(*selectedMesh);
    auto colors = selectedMesh->colors;
    auto materials = selectedMesh->materials;

    bool keepGroups = m_remeshKeepPolyGroups && !selectedMesh->faceGroups.empty();
    std::vector<uint32_t> triFaceGroups;
    if (keepGroups) {
        triFaceGroups.reserve(selectedMesh->nbFaces * 2);
        for (int i = 0; i < selectedMesh->nbFaces; ++i) {
            uint32_t fg = i < (int)selectedMesh->faceGroups.size() ? selectedMesh->faceGroups[i] : 0;
            triFaceGroups.push_back(fg);
            uint32_t v3 = selectedMesh->faces[i * 4 + 3];
            if (v3 != TRI_INDEX) {
                triFaceGroups.push_back(fg);
            }
        }
    }

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
        triFaceGroups = std::move(triFaceGroups),
        keepGroups,
        alignSymmetry = m_remeshAlignSymmetry,
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
                keepGroups ? triFaceGroups.data() : nullptr,
                bboxArr.data(),
                resolution,
                false, // block
                false, // smooth
                false, // manifold (Marching Cubes) -> false uses Surface Nets (quads)
                uniColorArr.data(),
                uniMatArr.data(),
                hasColors,
                hasMaterials,
                keepGroups,
                alignSymmetry,
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
    sculpt_log("[DEBUG applyRemeshResult] Started applying remesh result.\n");
    sculpt_log("[DEBUG applyRemeshResult] RemeshResult: verts=%u, faces=%u, colors=%u, materials=%u, faceGroups=%u\n",
              (unsigned int)r.vertices.size(), (unsigned int)r.faces.size(),
              (unsigned int)r.colors.size(), (unsigned int)r.materials.size(),
              (unsigned int)r.faceGroups.size());

    Mesh* selectedMesh = scene.getSelected();
    if (!selectedMesh) {
        sculpt_log("[WARNING applyRemeshResult] No selected mesh to apply remesh result to!\n");
        return;
    }

    selectedMesh->verts = r.vertices;
    selectedMesh->faces = r.faces;
    selectedMesh->colors = r.colors;
    selectedMesh->materials = r.materials;
    selectedMesh->nbVerts = r.vertices.size() / 3;
    selectedMesh->nbFaces = r.faces.size() / 4;

    if (m_remeshKeepPolyGroups && !r.faceGroups.empty()) {
        selectedMesh->faceGroups = r.faceGroups;
        selectedMesh->isFaceGroupDirty = true;
    } else {
        selectedMesh->initFaceGroups();
    }

    sculpt_log("[DEBUG applyRemeshResult] Mesh configuration updated: nbVerts=%d, nbFaces=%d\n",
              selectedMesh->nbVerts, selectedMesh->nbFaces);

    // Validate face indices to prevent segmentation faults in computeTopology and postInit
    uint32_t maxVertIndex = selectedMesh->nbVerts;
    uint32_t outOfBoundsCount = 0;
    for (size_t i = 0; i < selectedMesh->faces.size(); ++i) {
        uint32_t vid = selectedMesh->faces[i];
        if (vid != 0xffffffff && vid >= maxVertIndex) {
            outOfBoundsCount++;
            selectedMesh->faces[i] = 0; // Safe clamp to prevent out-of-bounds crash
        }
    }
    if (outOfBoundsCount > 0) {
        sculpt_log("[WARNING applyRemeshResult] Found and corrected %u out-of-bounds face indices in the reconstruction output!\n", outOfBoundsCount);
    } else {
        sculpt_log("[DEBUG applyRemeshResult] Face indices validated. All indices are safe.\n");
    }

    sculpt_log("[DEBUG applyRemeshResult] Computing topology...\n");
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
    sculpt_log("[DEBUG applyRemeshResult] Topology computed successfully.\n");

    selectedMesh->vrfStartCount = vrfStartCount;
    selectedMesh->vertRingFace = vertRingFace;
    selectedMesh->vrvStartCount = vrvStartCount;
    selectedMesh->vertRingVert = vertRingVert;
    selectedMesh->vertOnEdge = vertOnEdge;

    sculpt_log("[DEBUG applyRemeshResult] Finalizing mesh initialization (postInit)...\n");
    selectedMesh->postInit();
    sculpt_log("[DEBUG applyRemeshResult] postInit completed successfully.\n");

    Multimesh* multimesh = dynamic_cast<Multimesh*>(selectedMesh);
    if (multimesh) {
        sculpt_log("[applyRemeshResult] Selected mesh is Multimesh. Resetting multiresolution hierarchy for newly remeshed topology.\n");
        multimesh->meshes.clear();
        auto baseRes = std::make_unique<MeshResolution>(*multimesh, true);
        multimesh->meshes.push_back(std::move(baseRes));
        multimesh->sel = 0;
        multimesh->setSelection(0);
    }

    selectedMesh->isDirty = true;

    HistoryState afterState = scene.saveCurrentState();
    g_undoManager.pushTopologyChange(scene, "Voxel Remesh", std::move(m_remeshBeforeState), std::move(afterState));
}

void GuiManager::drawRemeshProgressModal() {
    if (m_remeshAsync.state != RemeshState::Running) {
        return;
    }
    float scale = getUiScale();

    ImGui::OpenPopup("Remeshing...");

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(300.0f * scale, 120.0f * scale));

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
        ImGui::ProgressBar(progressFloat, ImVec2(-1.0f, 26.0f * scale), buf);

        ImGui::EndPopup();
    }
}

void GuiManager::drawModalIndicatorHUD(SculptManager& sculpt, Scene& scene) {
    float scale = getUiScale();
    const char* label = nullptr;
    char valStr[64] = "";
    float fraction = 0.0f;

    switch (m_activeModalMode) {
        case ModalMode::INTENSITY: {
            label = "Intensity";
            float valPct = sculpt.getBrushIntensity() * 100.0f;
            snprintf(valStr, sizeof(valStr), "%d%%", (int)std::round(valPct));
            if (valPct <= 100.0f) {
                fraction = valPct / 200.0f;
            } else {
                fraction = 0.5f + 0.5f * (valPct - 100.0f) / 900.0f;
            }
            break;
        }
        case ModalMode::FOCAL_SHIFT:
            if (sculpt.getBrush() == BRUSH_PAINT) {
                label = "Hardness";
                snprintf(valStr, sizeof(valStr), "%d%%", (int)(sculpt.getHardness() * 100.0f));
                fraction = sculpt.getHardness();
            } else {
                label = "Focal Shift";
                snprintf(valStr, sizeof(valStr), "%d%%", (int)(sculpt.getFocalShift() * 100.0f));
                fraction = (sculpt.getFocalShift() + 1.0f) * 0.5f;
            }
            break;
        case ModalMode::RADIUS:
            label = "Radius";
            snprintf(valStr, sizeof(valStr), "%d px", (int)sculpt.getBrushRadius());
            fraction = (sculpt.getBrushRadius() - 0.5f) / (1000.0f - 0.5f);
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
    ImGui::SetNextWindowPos(ImVec2((float)m_modalStartMouseX, (float)m_modalStartMouseY - 25.0f * scale), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);

    // Style overrides for floating indicator card
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * scale, 8.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 6.0f * scale));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | 
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | 
                             ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::Begin("##ModalIndicatorHUD", nullptr, flags)) {
        float width = 150.0f * scale; // matches min-width of 150px
        
        float posX = ImGui::GetCursorPosX();
        ImGui::Text("%s", label);
        float valWidth = ImGui::CalcTextSize(valStr).x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(posX + width - valWidth);
        ImGui::Text("%s", valStr);

        // Render sleek progress bar
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.01f, 0.52f, 0.45f, 1.00f)); // Teal Accent
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 1.0f, 0.2f)); // Track
        ImGui::ProgressBar(std::max(0.0f, std::min(1.0f, fraction)), ImVec2(width, 5.0f * scale), "");
        ImGui::PopStyleColor(2);
    }
    ImGui::End();

    ImGui::PopStyleVar(3);
}


bool GuiManager::saveSettings(IniFile& ini) {
    if (m_window) {
        Uint32 flags = SDL_GetWindowFlags(m_window);
        m_winMaximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
        if (!m_winMaximized) {
            SDL_GetWindowSize(m_window, &m_winWidth, &m_winHeight);
            SDL_GetWindowPosition(m_window, &m_winX, &m_winY);
        }
    }

    std::string panelSec = "Panels";
    ini.setBool(panelSec, "showToolbar", m_showToolbar);
    ini.setBool(panelSec, "showSculptingPanel", m_showSculptingPanel);
    ini.setBool(panelSec, "showScenePanel", m_showScenePanel);
    ini.setBool(panelSec, "showTopologyPanel", m_showTopologyPanel);
    ini.setBool(panelSec, "showFilesPanel", m_showFilesPanel);
    ini.setBool(panelSec, "showCameraPanel", m_showCameraPanel);
    ini.setBool(panelSec, "showRenderingPanel", m_showRenderingPanel);
    ini.setBool(panelSec, "showMaskingPanel", m_showMaskingPanel);
    ini.setBool(panelSec, "showMultiresPanel", m_showMultiresPanel);
    ini.setBool(panelSec, "showZSpheresPanel", m_showZSpheresPanel);
    ini.setBool(panelSec, "showReferenceImagesPanel", m_showReferenceImagesPanel);
    ini.setBool(panelSec, "showGizmoCube", m_showGizmoCube);
    ini.setBool(panelSec, "showMeshInfo", m_showMeshInfo);
    ini.setBool(panelSec, "showTabletDiagPanel", m_showTabletDiagPanel);
    ini.setBool(panelSec, "showUndoDiagPanel", m_showUndoDiagPanel);
    ini.setBool(panelSec, "showDebugLogPanel", m_showDebugLogPanel);
    ini.setBool(panelSec, "showFloatingIsland", m_showFloatingIsland);
    ini.setBool(panelSec, "showTimelapsePanel", m_showTimelapsePanel);
    ini.setBool(panelSec, "showPreferencesPanel", m_showPreferencesPanel);
    ini.setBool(panelSec, "showHotkeyHUD", m_showHotkeyHUD);

    std::string genSec = "GuiGeneral";
    ini.setFloat(genSec, "uiScaleMultiplier", m_uiScale);
    ini.setFloat(genSec, "floatingIslandScaleMultiplier", m_floatingIslandScale);
    ini.setFloat(genSec, "gizmoSize", m_gizmoSize);
    ini.setBool(genSec, "fpsLimitEnabled", m_fpsLimitEnabled);
    ini.setInt(genSec, "fpsLimit", m_fpsLimit);

    std::string winSec = "Window";
    ini.setInt(winSec, "width", m_winWidth);
    ini.setInt(winSec, "height", m_winHeight);
    ini.setInt(winSec, "x", m_winX);
    ini.setInt(winSec, "y", m_winY);
    ini.setBool(winSec, "maximized", m_winMaximized);

    std::string undoSec = "Undo";
    ini.setFloat(undoSec, "maxMemoryGB", (float)g_undoManager.getMaxMemoryGB());
    ini.setInt(undoSec, "maxEntries", (int)g_undoManager.getMaxEntries());

    return true;
}

bool GuiManager::loadSettings(const IniFile& ini) {
    std::string panelSec = "Panels";
    if (ini.hasSection(panelSec)) {
        if (ini.hasKey(panelSec, "showToolbar")) m_showToolbar = ini.getBool(panelSec, "showToolbar");
        if (ini.hasKey(panelSec, "showSculptingPanel")) m_showSculptingPanel = ini.getBool(panelSec, "showSculptingPanel");
        if (ini.hasKey(panelSec, "showScenePanel")) m_showScenePanel = ini.getBool(panelSec, "showScenePanel");
        if (ini.hasKey(panelSec, "showTopologyPanel")) m_showTopologyPanel = ini.getBool(panelSec, "showTopologyPanel");
        if (ini.hasKey(panelSec, "showFilesPanel")) m_showFilesPanel = ini.getBool(panelSec, "showFilesPanel");
        if (ini.hasKey(panelSec, "showCameraPanel")) m_showCameraPanel = ini.getBool(panelSec, "showCameraPanel");
        if (ini.hasKey(panelSec, "showRenderingPanel")) m_showRenderingPanel = ini.getBool(panelSec, "showRenderingPanel");
        if (ini.hasKey(panelSec, "showMaskingPanel")) m_showMaskingPanel = ini.getBool(panelSec, "showMaskingPanel");
        if (ini.hasKey(panelSec, "showMultiresPanel")) m_showMultiresPanel = ini.getBool(panelSec, "showMultiresPanel");
        if (ini.hasKey(panelSec, "showZSpheresPanel")) m_showZSpheresPanel = ini.getBool(panelSec, "showZSpheresPanel");
        if (ini.hasKey(panelSec, "showReferenceImagesPanel")) m_showReferenceImagesPanel = ini.getBool(panelSec, "showReferenceImagesPanel");
        if (ini.hasKey(panelSec, "showGizmoCube")) m_showGizmoCube = ini.getBool(panelSec, "showGizmoCube");
        if (ini.hasKey(panelSec, "showMeshInfo")) m_showMeshInfo = ini.getBool(panelSec, "showMeshInfo");
        if (ini.hasKey(panelSec, "showTabletDiagPanel")) m_showTabletDiagPanel = ini.getBool(panelSec, "showTabletDiagPanel");
        if (ini.hasKey(panelSec, "showUndoDiagPanel")) m_showUndoDiagPanel = ini.getBool(panelSec, "showUndoDiagPanel");
        if (ini.hasKey(panelSec, "showDebugLogPanel")) m_showDebugLogPanel = ini.getBool(panelSec, "showDebugLogPanel");
        if (ini.hasKey(panelSec, "showFloatingIsland")) m_showFloatingIsland = ini.getBool(panelSec, "showFloatingIsland");
        if (ini.hasKey(panelSec, "showTimelapsePanel")) m_showTimelapsePanel = ini.getBool(panelSec, "showTimelapsePanel");
        if (ini.hasKey(panelSec, "showPreferencesPanel")) m_showPreferencesPanel = ini.getBool(panelSec, "showPreferencesPanel");
        if (ini.hasKey(panelSec, "showHotkeyHUD")) m_showHotkeyHUD = ini.getBool(panelSec, "showHotkeyHUD");
    }

    std::string genSec = "GuiGeneral";
    if (!ini.hasSection(genSec)) genSec = "General";
    if (ini.hasSection(genSec)) {
        if (ini.hasKey(genSec, "uiScaleMultiplier")) {
            m_uiScale = ini.getFloat(genSec, "uiScaleMultiplier", 1.0f);
            if (m_uiScale < 0.5f) m_uiScale = 0.5f;
            if (m_uiScale > 2.5f) m_uiScale = 2.5f;
        }
        if (ini.hasKey(genSec, "floatingIslandScaleMultiplier")) {
            m_floatingIslandScale = ini.getFloat(genSec, "floatingIslandScaleMultiplier", 1.0f);
            if (m_floatingIslandScale < 0.5f) m_floatingIslandScale = 0.5f;
            if (m_floatingIslandScale > 2.5f) m_floatingIslandScale = 2.5f;
        }
        if (ini.hasKey(genSec, "gizmoSize")) {
            m_gizmoSize = ini.getFloat(genSec, "gizmoSize", 0.10f);
            if (m_gizmoSize < 0.04f) m_gizmoSize = 0.04f;
            if (m_gizmoSize > 0.25f) m_gizmoSize = 0.25f;
        }
        if (ini.hasKey(genSec, "fpsLimitEnabled")) m_fpsLimitEnabled = ini.getBool(genSec, "fpsLimitEnabled");
        if (ini.hasKey(genSec, "fpsLimit")) {
            m_fpsLimit = ini.getInt(genSec, "fpsLimit", 60);
            if (m_fpsLimit < 15) m_fpsLimit = 15;
            if (m_fpsLimit > 240) m_fpsLimit = 240;
        }
    }

    std::string winSec = "Window";
    if (ini.hasSection(winSec)) {
        if (ini.hasKey(winSec, "width")) m_winWidth = ini.getInt(winSec, "width", 1280);
        if (ini.hasKey(winSec, "height")) m_winHeight = ini.getInt(winSec, "height", 720);
        if (ini.hasKey(winSec, "x")) m_winX = ini.getInt(winSec, "x", SDL_WINDOWPOS_CENTERED);
        if (ini.hasKey(winSec, "y")) m_winY = ini.getInt(winSec, "y", SDL_WINDOWPOS_CENTERED);
        if (ini.hasKey(winSec, "maximized")) m_winMaximized = ini.getBool(winSec, "maximized");
    }

    std::string undoSec = "Undo";
    if (ini.hasSection(undoSec)) {
        if (ini.hasKey(undoSec, "maxMemoryGB")) {
            double gb = ini.getFloat(undoSec, "maxMemoryGB", 4.0f);
            if (gb >= 0.1 && gb <= 128.0) {
                g_undoManager.setMaxMemoryGB(gb);
            }
        }
        if (ini.hasKey(undoSec, "maxEntries")) {
            size_t n = ini.getInt(undoSec, "maxEntries", 100);
            if (n >= 1 && n <= 10000) {
                g_undoManager.setMaxEntries(n);
            }
        }
    }

    if (m_imguiInitialized) {
        m_pendingUiScaleRefresh = true;
    }

    return true;
}

void GuiManager::takeScreenshot(const Scene& scene, AngleRenderer& renderer) {
    // 1. Resolve screenshot dimensions
    int w = m_screenshotWidth;
    int h = m_screenshotHeight;
    if (m_screenshotPreset == 0) {
        w = renderer.getWidth();
        h = renderer.getHeight();
    } else if (m_screenshotPreset == 1) {
        w = 1920;
        h = 1080;
    } else if (m_screenshotPreset == 2) {
        w = 2560;
        h = 1440;
    } else if (m_screenshotPreset == 3) {
        w = 3840;
        h = 2160;
    }

    // 2. Temporarily apply screenshot specific rendering overrides
    bool oldGrid = renderer.getShowGrid();
    bool oldContour = renderer.getShowContour();

    renderer.setShowGrid(m_screenshotShowGrid);
    renderer.setShowContour(m_screenshotShowContour);

    // 3. Render offscreen
    std::vector<uint8_t> pixels = renderer.renderToBuffer(scene, w, h);

    // 4. Restore original rendering settings
    renderer.setShowGrid(oldGrid);
    renderer.setShowContour(oldContour);

    // 5. Create screenshots directory if it doesn't exist
    std::error_code ec;
    std::filesystem::create_directories("screenshots", ec);
    if (ec) {
        std::cerr << "Failed to create screenshots directory: " << ec.message() << std::endl;
        return;
    }

    // 6. Generate filename using timestamp
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << "screenshots/screenshot_";
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d_%H-%M-%S");
    ss << ".png";
    std::string filepath = ss.str();

    // 7. Save PNG to disk using stb_image_write
    int success = stbi_write_png(filepath.c_str(), w, h, 4, pixels.data(), w * 4);    if (success) {
        std::cout << "Screenshot successfully saved to: " << filepath << " (" << w << "x" << h << ")" << std::endl;
    } else {
        std::cerr << "Failed to write screenshot image file: " << filepath << std::endl;
    }
}

void GuiManager::drawAppMenuItems(SculptManager& sculpt, Scene& scene, AngleRenderer& renderer) {
    float scale = getUiScale();
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open...", "Ctrl+O")) {
            openScene(scene, &sculpt);
        }
        if (ImGui::MenuItem("Save", "Ctrl+S")) {
            saveScene(scene, &sculpt);
        }
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) {
            saveSceneAs(scene, &sculpt);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Import File...", "Ctrl+I")) {
            importFile(scene, &sculpt);
        }
        if (ImGui::MenuItem("Export File...", "Ctrl+E")) {
            exportFile(scene, &sculpt);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Load Default Sphere")) {
            scene.loadDefaultSphere();
            m_currentScenePath.clear();
            scene.setModified(false);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save App Settings")) {
            IniFile ini;
            ini.load("app_settings.cfg");
            RenderSettings::save(ini, renderer, scene);
            sculpt.saveSettings(ini);
            saveSettings(ini);
            ini.save("app_settings.cfg");
        }
        if (ImGui::MenuItem("Load App Settings")) {
            IniFile ini;
            if (ini.load("app_settings.cfg")) {
                RenderSettings::load(ini, renderer, scene);
                sculpt.loadSettings(ini);
                loadSettings(ini);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) {
            if (scene.isModified()) {
                requestExit();
            } else {
                requestExit(false);
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Panels")) {
        ImGui::MenuItem("Toolbar", nullptr, &m_showToolbar);
        ImGui::MenuItem("Sculpting Settings", nullptr, &m_showSculptingPanel);
        ImGui::MenuItem("Scene Outliner", nullptr, &m_showScenePanel);
        ImGui::MenuItem("Topology & Remesh", nullptr, &m_showTopologyPanel);
        ImGui::MenuItem("Multiresolution", nullptr, &m_showMultiresPanel);
        ImGui::MenuItem("Reference Images", nullptr, &m_showReferenceImagesPanel);
        ImGui::MenuItem("Undo History", nullptr, &m_showUndoDiagPanel);
        ImGui::MenuItem("Sculpt Timelapse", nullptr, &m_showTimelapsePanel);
        bool hasSnapshot = renderer.hasActiveSnapshot();
        if (ImGui::MenuItem("Model Snapshot (Screen Reference)", nullptr, &hasSnapshot)) {
            renderer.toggleSnapshot(scene);
        }
#ifdef _WIN32
        ImGui::MenuItem("Tablet Diagnostics", nullptr, &m_showTabletDiagPanel);
#endif
        ImGui::Separator();
        ImGui::MenuItem("Preferences & App Settings", nullptr, &m_showPreferencesPanel);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Options")) {
        if (ImGui::MenuItem("Preferences & App Settings...", "Ctrl+,")) {
            m_showPreferencesPanel = true;
        }
        ImGui::EndMenu();
    }
}

void GuiManager::drawFloatingIslandHUD(SculptManager& sculpt, Scene& scene, AngleRenderer& renderer) {
    if (!m_showFloatingIsland) return;
    float scale = getFloatingIslandScale();
    float fontScale = m_floatingIslandScale / std::max(0.01f, m_uiScale);

    ImGuiViewport* viewport = ImGui::GetMainViewport();

    // Common style colors & variables for HUD islands
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.09f, 0.10f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.01f, 0.52f, 0.45f, 0.40f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f * scale, 3.0f * scale));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove;

    // ------------------------------------------------------------------------
    // 1. VERTICAL BRUSH HUD (Top-Left)
    // ------------------------------------------------------------------------
    ImVec2 posTopLeft = ImVec2(viewport->Pos.x, viewport->Pos.y);
    ImGui::SetNextWindowPos(posTopLeft, ImGuiCond_Always, ImVec2(0.0f, 0.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * scale, 4.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * scale, 6.0f * scale));

    if (ImGui::Begin("##FloatingIslandBrushVert", nullptr, flags)) {
        ImGui::SetWindowFontScale(fontScale);
        float squareSize = ImGui::GetFrameHeight();
        float vSliderH = 80.0f * scale;

        // Main Menu Button (Top)
        if (ImGui::Button(ICON_LC_MENU "##hudVertAppMenu", ImVec2(squareSize, squareSize))) {
            ImGui::OpenPopup("##hudVertAppMenuPopup");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Main Menu (File, Panels, Options)");

        if (ImGui::BeginPopup("##hudVertAppMenuPopup")) {
            ImGui::SetWindowFontScale(fontScale);
            drawAppMenuItems(sculpt, scene, renderer);
            ImGui::EndPopup();
        }

        // Scene Outliner Button (Below Menu)
        if (m_showScenePanel) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }

        if (ImGui::Button(ICON_LC_LIST "##hudVertOutlinerBtn", ImVec2(squareSize, squareSize))) {
            m_showScenePanel = !m_showScenePanel;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scene Outliner");

        if (m_showScenePanel) {
            ImGui::PopStyleColor(3);
        }

        ImGui::Separator();

        // Brush Selection Button & Popup List
        BrushType currentBrush = sculpt.getBrush();
        const char* brushName = getBrushNameLocal(currentBrush);

        if (ImGui::Button(ICON_LC_BRUSH "##hudVertBrushBtn", ImVec2(squareSize, squareSize))) {
            ImGui::OpenPopup("##hudVertBrushPopup");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Brush: %s", brushName);

        if (ImGui::BeginPopup("##hudVertBrushPopup")) {
            ImGui::SetWindowFontScale(fontScale);
            for (int i = 0; i < BRUSH_COUNT; i++) {
                BrushType bType = static_cast<BrushType>(i);
                bool isSelected = (currentBrush == bType);
                if (ImGui::Selectable(getBrushNameLocal(bType), isSelected)) {
                    sculpt.setBrush(bType);
                }
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndPopup();
        }

        // Alt (Invert / Subtractive Mode) Toggle Button
        bool isAltPhysicallyPressed = ImGui::GetIO().KeyAlt || ((SDL_GetModState() & KMOD_ALT) != 0);
        bool isAltActive = isAltPhysicallyPressed || sculpt.getNegative();

        if (isAltActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }

        if (ImGui::Button("Alt##hudVertAltBtn", ImVec2(squareSize, squareSize))) {
            sculpt.toggleNegative();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Invert Brush Mode (Alt)");
        }

        if (isAltActive) {
            ImGui::PopStyleColor(3);
        }

        ImGui::Separator();

        // Top Vertical Slider: Size
        float radius = sculpt.getBrushRadius();
        if (ImGui::VSliderFloat("##hudVertRadius", ImVec2(squareSize, vSliderH), &radius, 1.0f, 1000.0f, "")) {
            sculpt.setBrushRadius(radius);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Size: %.0f px", radius);

        ImGui::Spacing();

        // Middle Vertical Slider: Intensity
        float intensityPct = sculpt.getBrushIntensity() * 100.0f;
        float maxHudPct = (intensityPct > 100.0f) ? std::min(1000.0f, std::max(100.0f, intensityPct)) : 100.0f;
        if (ImGui::VSliderFloat("##hudVertIntensity", ImVec2(squareSize, vSliderH), &intensityPct, 0.0f, maxHudPct, "")) {
            sculpt.setBrushIntensity(std::max(0.0f, std::min(10.0f, intensityPct / 100.0f)));
        }
        if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.y < 0.0f && intensityPct >= maxHudPct - 0.1f) {
            intensityPct = std::min(1000.0f, intensityPct + 10.0f);
            sculpt.setBrushIntensity(intensityPct / 100.0f);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Intensity: %.0f%% (0%% to 1000%%)", intensityPct);
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                float stepPct = (intensityPct < 100.0f) ? 5.0f : 25.0f;
                intensityPct = std::max(0.0f, std::min(1000.0f, intensityPct + wheel * stepPct));
                sculpt.setBrushIntensity(intensityPct / 100.0f);
            }
        }

        ImGui::Spacing();

        // Bottom Vertical Slider: Focal Shift
        float focalShift = sculpt.getFocalShift();
        if (ImGui::VSliderFloat("##hudVertFocalShift", ImVec2(squareSize, vSliderH), &focalShift, -1.0f, 1.0f, "")) {
            sculpt.setFocalShift(focalShift);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Focal Shift: %.0f%%", focalShift * 100.0f);
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                focalShift = std::max(-1.0f, std::min(1.0f, focalShift + wheel * 0.05f));
                sculpt.setFocalShift(focalShift);
            }
        }

        ImGui::Separator();

        // Split-button Voxel Remesh: Main Box button + Arrow button attached with 0 gap
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));

        bool runningRemesh = isRemeshRunning();
        if (runningRemesh) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.45f, 0.05f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.55f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.35f, 0.00f, 1.0f));
        }

        GLuint vrTex = getIconTexture("voxelremesh");
        bool clickedVr = false;
        if (vrTex != 0) {
            clickedVr = ImGui::Button("##hudVertVoxelRemesh", ImVec2(squareSize, squareSize));
            ImVec2 pMin = ImGui::GetItemRectMin();
            ImVec2 pMax = ImGui::GetItemRectMax();
            float pad = 1.0f * scale;
            ImVec2 imgMin(pMin.x + pad, pMin.y + pad);
            ImVec2 imgMax(pMax.x - pad, pMax.y - pad);
            ImU32 tintCol = runningRemesh ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 240);
            ImGui::GetWindowDrawList()->AddImage((ImTextureID)(uintptr_t)vrTex, imgMin, imgMax, ImVec2(0, 1), ImVec2(1, 0), tintCol);
        } else {
            clickedVr = ImGui::Button(ICON_LC_BOXES "##hudVertVoxelRemesh", ImVec2(squareSize, squareSize));
        }

        if (clickedVr) {
            performRemesh(scene);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Voxel Remesh (Resolution: %d)", m_remeshResolution);
        }

        // Half-Height Arrow Button attached directly below (0 gap)
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        ImGui::SetWindowFontScale(fontScale * 0.65f);
        bool openRemeshPopup = ImGui::Button(ICON_LC_CHEVRON_DOWN "##hudVertRemeshArrow", ImVec2(squareSize, squareSize * 0.45f));
        ImGui::SetWindowFontScale(fontScale);
        ImGui::PopStyleVar();

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Remesh Settings");
        }

        if (runningRemesh) {
            ImGui::PopStyleColor(3);
        }

        ImGui::PopStyleVar(); // Restore ItemSpacing

        ImVec2 arrowMin = ImGui::GetItemRectMin();
        ImVec2 arrowMax = ImGui::GetItemRectMax();

        if (openRemeshPopup) {
            ImGui::OpenPopup("##hudVertRemeshPopup");
        }

        ImGui::SetNextWindowPos(ImVec2(arrowMax.x + 6.0f * scale, arrowMin.y), ImGuiCond_Appearing);
        if (ImGui::BeginPopup("##hudVertRemeshPopup")) {
            ImGui::SetWindowFontScale(fontScale);
            ImGui::TextUnformatted("Voxel Remesh");
            ImGui::Separator();
            ImGui::Text("Resolution: %d", m_remeshResolution);
            ImGui::PushItemWidth(140.0f * scale);
            ImGui::SliderInt("##hudRemeshResSlider", &m_remeshResolution, 10, 1000);
            if (ImGui::IsItemActive()) {
                Mesh* selectedMesh = scene.getSelected();
                if (selectedMesh) {
                    float bbox[6];
                    selectedMesh->computeBbox(bbox);
                    float maxDim = std::max({bbox[3] - bbox[0], bbox[4] - bbox[1], bbox[5] - bbox[2]});
                    float step = maxDim / (float)m_remeshResolution;
                    scene.updateVoxelPreview(step, {selectedMesh});
                }
            } else if (ImGui::IsItemDeactivated()) {
                scene.updateVoxelPreview(0.0f, {});
            }
            ImGui::PopItemWidth();
            ImGui::Checkbox("Keep PolyGroups", &m_remeshKeepPolyGroups);
            ImGui::Checkbox("Align Symmetry Axes", &m_remeshAlignSymmetry);
            ImGui::Separator();
            if (ImGui::Button("Remesh Now", ImVec2(-1.0f, 0.0f))) {
                scene.updateVoxelPreview(0.0f, {});
                performRemesh(scene);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::Separator();

        // Divider Tool Button with attached Arrow dropdown
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));

        bool isDividerActive = (currentBrush == BRUSH_DIVIDER);
        if (isDividerActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }

        GLuint dividerTex = getIconTexture("dividertool");
        bool clickedDivider = false;
        if (dividerTex != 0) {
            clickedDivider = ImGui::Button("##hudVertDividerTool", ImVec2(squareSize, squareSize));
            ImVec2 pMin = ImGui::GetItemRectMin();
            ImVec2 pMax = ImGui::GetItemRectMax();
            float pad = 1.0f * scale;
            ImVec2 imgMin(pMin.x + pad, pMin.y + pad);
            ImVec2 imgMax(pMax.x - pad, pMax.y - pad);
            ImU32 tintCol = isDividerActive ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 240);
            ImGui::GetWindowDrawList()->AddImage((ImTextureID)(uintptr_t)dividerTex, imgMin, imgMax, ImVec2(0, 1), ImVec2(1, 0), tintCol);
        } else {
            clickedDivider = ImGui::Button(ICON_LC_RULER "##hudVertDividerTool", ImVec2(squareSize, squareSize));
        }

        if (clickedDivider) {
            sculpt.setBrush(BRUSH_DIVIDER);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Divider Tool (Divisions: %d)", sculpt.getDividerDivisions());
        }

        // Half-Height Arrow Button attached directly below (0 gap)
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        ImGui::SetWindowFontScale(fontScale * 0.65f);
        bool openDividerPopup = ImGui::Button(ICON_LC_CHEVRON_DOWN "##hudVertDividerArrow", ImVec2(squareSize, squareSize * 0.45f));
        ImGui::SetWindowFontScale(fontScale);
        ImGui::PopStyleVar();

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Divider Settings");
        }

        if (isDividerActive) {
            ImGui::PopStyleColor(3);
        }

        ImGui::PopStyleVar(); // Restore ItemSpacing

        ImVec2 divArrowMin = ImGui::GetItemRectMin();
        ImVec2 divArrowMax = ImGui::GetItemRectMax();

        if (openDividerPopup) {
            ImGui::OpenPopup("##hudVertDividerPopup");
        }

        ImGui::SetNextWindowPos(ImVec2(divArrowMax.x + 6.0f * scale, divArrowMin.y), ImGuiCond_Appearing);
        if (ImGui::BeginPopup("##hudVertDividerPopup")) {
            ImGui::SetWindowFontScale(fontScale);
            ImGui::TextUnformatted("Divider Settings");
            ImGui::Separator();

            int divs = sculpt.getDividerDivisions();
            ImGui::Text("Divisions / Segments: %d", divs);
            ImGui::PushItemWidth(140.0f * scale);
            if (ImGui::SliderInt("##hudDividerDivsSlider", &divs, 2, 10)) {
                sculpt.setDividerDivisions(divs);
            }
            ImGui::PopItemWidth();

            bool useDist = sculpt.getMeasureUseDistanceThickness();
            if (ImGui::Checkbox("Perspective Thickness", &useDist)) {
                sculpt.setMeasureUseDistanceThickness(useDist);
            }

            ImGui::Separator();
            if (ImGui::Button("Clear Dividers", ImVec2(-1.0f, 0.0f))) {
                sculpt.clearMeasurements();
            }
            ImGui::EndPopup();
        }

        // Measure Tool Button
        bool isMeasureActive = (currentBrush == BRUSH_MEASURE);
        if (isMeasureActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }

        GLuint measureTex = getIconTexture("measuretool");
        bool clickedMeasure = false;
        if (measureTex != 0) {
            clickedMeasure = ImGui::Button("##hudVertMeasureTool", ImVec2(squareSize, squareSize));
            ImVec2 pMin = ImGui::GetItemRectMin();
            ImVec2 pMax = ImGui::GetItemRectMax();
            float pad = 1.0f * scale;
            ImVec2 imgMin(pMin.x + pad, pMin.y + pad);
            ImVec2 imgMax(pMax.x - pad, pMax.y - pad);
            ImU32 tintCol = isMeasureActive ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 240);
            ImGui::GetWindowDrawList()->AddImage((ImTextureID)(uintptr_t)measureTex, imgMin, imgMax, ImVec2(0, 1), ImVec2(1, 0), tintCol);
        } else {
            clickedMeasure = ImGui::Button(ICON_LC_RULER "##hudVertMeasureTool", ImVec2(squareSize, squareSize));
        }

        if (clickedMeasure) {
            sculpt.setBrush(BRUSH_MEASURE);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Measure Tool");
        }

        if (isMeasureActive) {
            ImGui::PopStyleColor(3);
        }

    }
    ImGui::End();

    ImGui::PopStyleVar(2); // WindowPadding, ItemSpacing

    // ------------------------------------------------------------------------
    // 2. HORIZONTAL VIEWPORT & SYMMETRY HUD (Top-Right)
    // ------------------------------------------------------------------------
    ImVec2 posRight = ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y);
    ImGui::SetNextWindowPos(posRight, ImGuiCond_Always, ImVec2(1.0f, 0.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, 4.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * scale, 0.0f));

    if (ImGui::Begin("##FloatingIslandHUD_v2", nullptr, flags)) {
        ImGui::SetWindowFontScale(fontScale);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.0f * scale, 0.0f));

        float squareBtnSize = ImGui::GetFrameHeight();
        ImVec2 squareBtn(squareBtnSize, squareBtnSize);
        ImVec2 halfSquareBtn(squareBtnSize * 0.5f, squareBtnSize);

        bool useSym = sculpt.getUseSym();

        if (useSym) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }

        GLuint symTex = getIconTexture("symmetry");
        bool clickedSym = false;
        if (symTex != 0) {
            clickedSym = ImGui::Button("##hudSym", squareBtn);
            ImVec2 pMin = ImGui::GetItemRectMin();
            ImVec2 pMax = ImGui::GetItemRectMax();
            float pad = 1.0f * scale;
            ImVec2 imgMin(pMin.x + pad, pMin.y + pad);
            ImVec2 imgMax(pMax.x - pad, pMax.y - pad);
            ImU32 tintCol = useSym ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 240);
            ImGui::GetWindowDrawList()->AddImage((ImTextureID)(uintptr_t)symTex, imgMin, imgMax, ImVec2(0, 1), ImVec2(1, 0), tintCol);
        } else {
            clickedSym = ImGui::Button(ICON_LC_SPLIT "##hudSym", squareBtn);
        }

        if (clickedSym) {
            sculpt.setUseSym(!useSym);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Symmetry (Alt+X)");

        ImVec2 symMin = ImGui::GetItemRectMin();

        // Arrow button flush against Symmetry button
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
        ImGui::SetWindowFontScale(fontScale * 0.65f);
        bool openSymPopup = ImGui::Button(ICON_LC_CHEVRON_DOWN "##hudSymArrow", halfSquareBtn);
        ImGui::SetWindowFontScale(fontScale);
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Symmetry Settings, Flip & Mirror");

        ImVec2 symMax = ImGui::GetItemRectMax();

        if (useSym) {
            ImGui::PopStyleColor(3);
        }

        float symCenterX = (symMin.x + symMax.x) * 0.5f;
        float symBottomY = symMax.y + 4.0f * scale;

        if (openSymPopup) {
            ImGui::OpenPopup("##hudSymPopup");
        }

        ImGui::SetNextWindowPos(ImVec2(symCenterX, symBottomY), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
        if (ImGui::BeginPopup("##hudSymPopup")) {
            ImGui::SetWindowFontScale(fontScale);

            // 1. Enable Symmetry Toggle
            bool symEnabled = sculpt.getUseSym();
            if (ImGui::Checkbox("Enable Symmetry", &symEnabled)) {
                sculpt.setUseSym(symEnabled);
            }

            ImGui::Separator();

            // 2. Symmetry Space (Local / World)
            ImGui::TextUnformatted("Symmetry Space:");
            SymmetryMode symModePopup = sculpt.getSymmetryMode();
            int modeIdx = (symModePopup == SymmetryMode::World) ? 1 : 0;
            if (ImGui::RadioButton("Local Space", &modeIdx, 0)) {
                sculpt.setSymmetryMode(SymmetryMode::Local);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("World Space", &modeIdx, 1)) {
                sculpt.setSymmetryMode(SymmetryMode::World);
            }

            ImGui::Separator();

            // 3. Symmetry Axes
            ImGui::TextUnformatted("Active Axes:");
            bool symXPop = sculpt.getSymX();
            bool symYPop = sculpt.getSymY();
            bool symZPop = sculpt.getSymZ();
            if (ImGui::Checkbox("X", &symXPop)) sculpt.setSymX(symXPop);
            ImGui::SameLine();
            if (ImGui::Checkbox("Y", &symYPop)) sculpt.setSymY(symYPop);
            ImGui::SameLine();
            if (ImGui::Checkbox("Z", &symZPop)) sculpt.setSymZ(symZPop);

            ImGui::Separator();

            // 4. Show Symmetry Line / Guide & Offset
            bool showSymLine = renderer.getShowSymmetryLine();
            if (ImGui::Checkbox("Show Symmetry Line", &showSymLine)) {
                renderer.setShowSymmetryLine(showSymLine);
            }
            if (showSymLine) {
                ImGui::Indent();
                float symLineWidth = renderer.getSymmetryLineWidth();
                ImGui::PushItemWidth(140.0f * scale);
                if (ImGui::SliderFloat("Line Width", &symLineWidth, 0.01f, 0.5f, "%.3f")) {
                    renderer.setSymmetryLineWidth(symLineWidth);
                }
                ImGui::PopItemWidth();
                ImGui::Unindent();
            }

            Mesh* activeMesh = scene.getSelected();
            if (activeMesh) {
                float symOffset = activeMesh->getSymmetryOffset();
                ImGui::PushItemWidth(160.0f * scale);
                if (ImGui::SliderFloat("Symmetry Offset", &symOffset, -1.0f, 1.0f, "%.3f")) {
                    activeMesh->setSymmetryOffset(symOffset);
                }
                ImGui::PopItemWidth();
            }

            ImGui::Separator();

            // 5. Flip Object
            ImGui::TextUnformatted("Flip Object:");
            float btnWidth = 65.0f * scale;
            if (ImGui::Button("Flip X", ImVec2(btnWidth, 24.0f * scale))) {
                std::vector<Mesh*> selected = scene.getSelectedMeshes();
                if (selected.empty() && scene.getSelected()) selected.push_back(scene.getSelected());
                if (!selected.empty()) {
                    scene.pushHistoryState();
                    for (Mesh* m : selected) {
                        if (m) m->flip(0);
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Flip Y", ImVec2(btnWidth, 24.0f * scale))) {
                std::vector<Mesh*> selected = scene.getSelectedMeshes();
                if (selected.empty() && scene.getSelected()) selected.push_back(scene.getSelected());
                if (!selected.empty()) {
                    scene.pushHistoryState();
                    for (Mesh* m : selected) {
                        if (m) m->flip(1);
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Flip Z", ImVec2(btnWidth, 24.0f * scale))) {
                std::vector<Mesh*> selected = scene.getSelectedMeshes();
                if (selected.empty() && scene.getSelected()) selected.push_back(scene.getSelected());
                if (!selected.empty()) {
                    scene.pushHistoryState();
                    for (Mesh* m : selected) {
                        if (m) m->flip(2);
                    }
                }
            }

            ImGui::Separator();

            // 6. Mirror Object
            ImGui::TextUnformatted("Mirror Object:");
            ImGui::RadioButton("X##hudMirror", &m_mirrorAxis, 0); ImGui::SameLine();
            ImGui::RadioButton("Y##hudMirror", &m_mirrorAxis, 1); ImGui::SameLine();
            ImGui::RadioButton("Z##hudMirror", &m_mirrorAxis, 2);

            int dirIdx = m_mirrorPositiveToNegative ? 0 : 1;
            if (ImGui::RadioButton("+ to -", &dirIdx, 0)) {
                m_mirrorPositiveToNegative = true;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("- to +", &dirIdx, 1)) {
                m_mirrorPositiveToNegative = false;
            }

            if (ImGui::Button("Mirror Object", ImVec2(-1.0f, 26.0f * scale))) {
                std::vector<Mesh*> selected = scene.getSelectedMeshes();
                if (selected.empty() && scene.getSelected()) selected.push_back(scene.getSelected());
                if (!selected.empty()) {
                    scene.pushHistoryState();
                    for (Mesh* m : selected) {
                        if (m) m->mirror(m_mirrorAxis, m_mirrorPositiveToNegative, sculpt.getSymmetryMode());
                    }
                }
            }

            ImGui::EndPopup();
        }

        ImGui::SameLine();

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));

        // World / Local Symmetry Mode Button
        SymmetryMode symMode = sculpt.getSymmetryMode();
        bool isWorldSym = (symMode == SymmetryMode::World);
        if (useSym) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }
        if (ImGui::Button(isWorldSym ? "W##hudSymMode" : "L##hudSymMode", halfSquareBtn)) {
            sculpt.setSymmetryMode(isWorldSym ? SymmetryMode::Local : SymmetryMode::World);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Symmetry Space: %s (Click to toggle)", isWorldSym ? "World" : "Local");
        }
        if (useSym) {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine();

        // X Axis Button
        bool symX = sculpt.getSymX();
        if (symX) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }
        if (ImGui::Button("X##hudSymX", halfSquareBtn)) {
            sculpt.setSymX(!symX);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle X Symmetry");
        if (symX) {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine();

        // Y Axis Button
        bool symY = sculpt.getSymY();
        if (symY) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }
        if (ImGui::Button("Y##hudSymY", halfSquareBtn)) {
            sculpt.setSymY(!symY);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Y Symmetry");
        if (symY) {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine();

        // Z Axis Button
        bool symZ = sculpt.getSymZ();
        if (symZ) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }
        if (ImGui::Button("Z##hudSymZ", halfSquareBtn)) {
            sculpt.setSymZ(!symZ);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Z Symmetry");
        if (symZ) {
            ImGui::PopStyleColor(3);
        }

        ImGui::PopStyleVar(1); // FramePadding

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        bool showGrid = renderer.getShowGrid();
        if (showGrid) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }
        GLuint gridTex = getIconTexture("grid");
        bool clickedGrid = false;
        if (gridTex != 0) {
            clickedGrid = ImGui::Button("##hudGrid", squareBtn);
            ImVec2 pMin = ImGui::GetItemRectMin();
            ImVec2 pMax = ImGui::GetItemRectMax();
            float pad = 1.0f * scale;
            ImVec2 imgMin(pMin.x + pad, pMin.y + pad);
            ImVec2 imgMax(pMax.x - pad, pMax.y - pad);
            ImU32 tintCol = showGrid ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 240);
            ImGui::GetWindowDrawList()->AddImage((ImTextureID)(uintptr_t)gridTex, imgMin, imgMax, ImVec2(0, 1), ImVec2(1, 0), tintCol);
        } else {
            clickedGrid = ImGui::Button(ICON_LC_GRID "##hudGrid", squareBtn);
        }

        if (clickedGrid) {
            renderer.setShowGrid(!showGrid);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Grid Display");
        if (showGrid) {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine();

        bool flatShading = renderer.getFlatShading();
        if (flatShading) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }
        GLuint fsTex = getIconTexture("flatshading");
        bool clickedFs = false;
        if (fsTex != 0) {
            clickedFs = ImGui::Button("##hudFlatShading", squareBtn);
            ImVec2 pMin = ImGui::GetItemRectMin();
            ImVec2 pMax = ImGui::GetItemRectMax();
            float pad = 1.0f * scale;
            ImVec2 imgMin(pMin.x + pad, pMin.y + pad);
            ImVec2 imgMax(pMax.x - pad, pMax.y - pad);
            ImU32 tintCol = flatShading ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 240);
            ImGui::GetWindowDrawList()->AddImage((ImTextureID)(uintptr_t)fsTex, imgMin, imgMax, ImVec2(0, 1), ImVec2(1, 0), tintCol);
        } else {
            clickedFs = ImGui::Button(ICON_LC_CUBOID "##hudFlatShading", squareBtn);
        }

        if (clickedFs) {
            renderer.setFlatShading(!flatShading);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Flat Shading");
        if (flatShading) {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine();

        bool showWireframe = renderer.getShowWireframe();
        if (showWireframe) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }
        GLuint wfTex = getIconTexture("wireframe");
        bool clickedWf = false;
        if (wfTex != 0) {
            clickedWf = ImGui::Button("##hudWireframe", squareBtn);
            ImVec2 pMin = ImGui::GetItemRectMin();
            ImVec2 pMax = ImGui::GetItemRectMax();
            float pad = 1.0f * scale;
            ImVec2 imgMin(pMin.x + pad, pMin.y + pad);
            ImVec2 imgMax(pMax.x - pad, pMax.y - pad);
            ImU32 tintCol = showWireframe ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 240);
            ImGui::GetWindowDrawList()->AddImage((ImTextureID)(uintptr_t)wfTex, imgMin, imgMax, ImVec2(0, 1), ImVec2(1, 0), tintCol);
        } else {
            clickedWf = ImGui::Button(ICON_LC_TRIANGLE "##hudWireframe", squareBtn);
        }

        if (clickedWf) {
            renderer.setShowWireframe(!showWireframe);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Wireframe Shading");
        if (showWireframe) {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine();

        bool showPolyGroups = renderer.getShowPolyGroups() || (sculpt.getBrush() == BRUSH_POLYGROUP);
        if (showPolyGroups) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }
        GLuint pgTex = getIconTexture("polygroup");
        bool clickedPg = false;
        if (pgTex != 0) {
            clickedPg = ImGui::Button("##hudPolyGroups", squareBtn);
            ImVec2 pMin = ImGui::GetItemRectMin();
            ImVec2 pMax = ImGui::GetItemRectMax();
            float pad = 1.0f * scale;
            ImVec2 imgMin(pMin.x + pad, pMin.y + pad);
            ImVec2 imgMax(pMax.x - pad, pMax.y - pad);
            ImU32 tintCol = showPolyGroups ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 240);
            ImGui::GetWindowDrawList()->AddImage((ImTextureID)(uintptr_t)pgTex, imgMin, imgMax, ImVec2(0, 1), ImVec2(1, 0), tintCol);
        } else {
            clickedPg = ImGui::Button(ICON_LC_LAYERS "##hudPolyGroups", squareBtn);
        }

        if (clickedPg) {
            renderer.setShowPolyGroups(!renderer.getShowPolyGroups());
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle PolyGroups Display");
        if (showPolyGroups) {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine();

        bool isSilhouette = (renderer.getShaderType() == 6);
        if (isSilhouette) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }
        GLuint silTex = getIconTexture("silhouette");
        bool clickedSil = false;
        if (silTex != 0) {
            clickedSil = ImGui::Button("##hudSilhouette", squareBtn);
            ImVec2 pMin = ImGui::GetItemRectMin();
            ImVec2 pMax = ImGui::GetItemRectMax();
            float pad = 1.0f * scale;
            ImVec2 imgMin(pMin.x + pad, pMin.y + pad);
            ImVec2 imgMax(pMax.x - pad, pMax.y - pad);
            ImU32 tintCol = isSilhouette ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 240);
            ImGui::GetWindowDrawList()->AddImage((ImTextureID)(uintptr_t)silTex, imgMin, imgMax, ImVec2(0, 1), ImVec2(1, 0), tintCol);
        } else {
            clickedSil = ImGui::Button(ICON_LC_CONTRAST "##hudSilhouette", squareBtn);
        }

        if (clickedSil) {
            if (isSilhouette) {
                renderer.setShaderType(m_previousShaderType);
            } else {
                m_previousShaderType = renderer.getShaderType();
                renderer.setShaderType(6);
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Silhouette Mode");
        if (isSilhouette) {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine();

        bool showSafeFrames = renderer.getShowSafeFrames();
        if (showSafeFrames) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }
        GLuint frameTex = getIconTexture("frame");
        bool clickedFrame = false;
        if (frameTex != 0) {
            clickedFrame = ImGui::Button("##hudSafeFrames", squareBtn);
            ImVec2 pMin = ImGui::GetItemRectMin();
            ImVec2 pMax = ImGui::GetItemRectMax();
            float pad = 1.0f * scale;
            ImVec2 imgMin(pMin.x + pad, pMin.y + pad);
            ImVec2 imgMax(pMax.x - pad, pMax.y - pad);
            ImU32 tintCol = showSafeFrames ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 240);
            ImGui::GetWindowDrawList()->AddImage((ImTextureID)(uintptr_t)frameTex, imgMin, imgMax, ImVec2(0, 1), ImVec2(1, 0), tintCol);
        } else {
            clickedFrame = ImGui::Button(ICON_LC_FRAME "##hudSafeFrames", squareBtn);
        }

        if (clickedFrame) {
            renderer.setShowSafeFrames(!showSafeFrames);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Safe Frames Overlay");
        if (showSafeFrames) {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine();

        // X-Ray Mode Toggle + Settings Arrow
        bool xrayEnabled = renderer.getXrayEnabled();
        if (xrayEnabled) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }

        GLuint xrayTex = getXrayIconTexture();
        bool clickedXray = false;

        if (xrayTex != 0) {
            clickedXray = ImGui::Button("##hudXray", squareBtn);
            ImVec2 pMin = ImGui::GetItemRectMin();
            ImVec2 pMax = ImGui::GetItemRectMax();
            float pad = 1.0f * scale;
            ImVec2 imgMin(pMin.x + pad, pMin.y + pad);
            ImVec2 imgMax(pMax.x - pad, pMax.y - pad);
            ImU32 tintCol = xrayEnabled ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 240);
            ImGui::GetWindowDrawList()->AddImage((ImTextureID)(uintptr_t)xrayTex, imgMin, imgMax, ImVec2(0, 1), ImVec2(1, 0), tintCol);
        } else {
            clickedXray = ImGui::Button(ICON_LC_SCAN_FACE "##hudXray", squareBtn);
        }

        if (clickedXray) {
            renderer.setXrayEnabled(!xrayEnabled);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle X-Ray Mode");

        ImVec2 xrayMin = ImGui::GetItemRectMin();

        // Flush narrow arrow button directly attached to X-Ray button
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
        ImGui::SetWindowFontScale(fontScale * 0.65f);
        bool openXrayPopup = ImGui::Button(ICON_LC_CHEVRON_DOWN "##hudXrayArrow", halfSquareBtn);
        ImGui::SetWindowFontScale(fontScale);
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("X-Ray Settings");

        ImVec2 xrayMax = ImGui::GetItemRectMax();

        if (xrayEnabled) {
            ImGui::PopStyleColor(3);
        }

        float xrayCenterX = (xrayMin.x + xrayMax.x) * 0.5f;
        float xrayBottomY = xrayMax.y + 4.0f * scale;

        if (openXrayPopup) {
            ImGui::OpenPopup("##hudXrayPopup");
        }

        ImGui::SetNextWindowPos(ImVec2(xrayCenterX, xrayBottomY), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
        if (ImGui::BeginPopup("##hudXrayPopup")) {
            ImGui::SetWindowFontScale(fontScale);
            ImGui::TextUnformatted("X-Ray Settings");
            ImGui::Separator();

            bool enable = renderer.getXrayEnabled();
            if (ImGui::Checkbox("Enable X-Ray Mode", &enable)) {
                renderer.setXrayEnabled(enable);
            }

            ImGui::Separator();
            ImGui::TextUnformatted("Target:");

            int target = renderer.getXrayTarget(); // 0: Unselected (default), 1: Selected, 2: All
            bool changedTarget = false;
            if (ImGui::RadioButton("Unselected (default)", &target, 0)) changedTarget = true;
            if (ImGui::RadioButton("Selected", &target, 1)) changedTarget = true;
            if (ImGui::RadioButton("All", &target, 2)) changedTarget = true;
            if (changedTarget) {
                renderer.setXrayTarget(target);
            }

            ImGui::Separator();
            ImGui::TextUnformatted("X-Ray Color:");
            glm::vec4 xrayCol = renderer.getXrayColor();
            if (ImGui::ColorEdit4("##hudXrayColor", &xrayCol.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs)) {
                renderer.setXrayColor(xrayCol);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("Color & Alpha");

            ImGui::EndPopup();
        }

        ImGui::SameLine();

        bool isPerspective = (scene.getCamera().getProjectionType() == CameraEnums::Projection::PERSPECTIVE);
        if (isPerspective) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }

        // Split-button: Perspective toggle (left) + FOV arrow (right)
        GLuint fovTex = getIconTexture("persp");
        bool clickedFov = false;
        if (fovTex != 0) {
            clickedFov = ImGui::Button("##hudPerspective", squareBtn);
            ImVec2 pMin = ImGui::GetItemRectMin();
            ImVec2 pMax = ImGui::GetItemRectMax();
            float pad = 1.0f * scale;
            ImVec2 imgMin(pMin.x + pad, pMin.y + pad);
            ImVec2 imgMax(pMax.x - pad, pMax.y - pad);
            ImU32 tintCol = isPerspective ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 240);
            ImGui::GetWindowDrawList()->AddImage((ImTextureID)(uintptr_t)fovTex, imgMin, imgMax, ImVec2(0, 1), ImVec2(1, 0), tintCol);
        } else {
            clickedFov = ImGui::Button(ICON_LC_CAMERA "##hudPerspective", squareBtn);
        }

        if (clickedFov) {
            scene.getCamera().setProjectionType(isPerspective ? CameraEnums::Projection::ORTHOGRAPHIC : CameraEnums::Projection::PERSPECTIVE);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Perspective Projection (P)");

        ImVec2 pMin = ImGui::GetItemRectMin();

        // Narrow arrow button flush against Perspective button (0 gap, 0 padding, half square width)
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
        ImGui::SetWindowFontScale(fontScale * 0.65f);
        bool openFovPopup = ImGui::Button(ICON_LC_CHEVRON_DOWN "##hudFovArrow", halfSquareBtn);
        ImGui::SetWindowFontScale(fontScale);
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("FOV Settings");

        ImVec2 pMax = ImGui::GetItemRectMax();

        if (isPerspective) {
            ImGui::PopStyleColor(3);
        }

        float splitCenterX = (pMin.x + pMax.x) * 0.5f;
        float splitBottomY = pMax.y + 4.0f * scale;

        if (openFovPopup) {
            ImGui::OpenPopup("##hudFovPopup");
        }

        ImGui::SetNextWindowPos(ImVec2(splitCenterX, splitBottomY), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
        if (ImGui::BeginPopup("##hudFovPopup")) {
            ImGui::SetWindowFontScale(fontScale);
            float fov = scene.getCamera().getFov();
            ImGui::TextUnformatted("FOV:");
            ImGui::SameLine();
            ImGui::PushItemWidth(120.0f * scale);
            if (ImGui::SliderFloat("##hudFovSlider", &fov, 10.0f, 120.0f, "%.0f mm")) {
                scene.getCamera().setFov(fov);
            }
            ImGui::PopItemWidth();
            ImGui::EndPopup();
        }

        ImGui::SameLine();

        // Split View Button + Dropdown Arrow
        bool isSplit = (scene.getSplitMode() != Scene::SplitMode::OFF);
        if (isSplit) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }

        auto applySplitMode = [&](Scene::SplitMode newMode) {
            scene.setSplitMode(newMode);
            if (m_window) {
                int w, h;
                SDL_GetWindowSize(m_window, &w, &h);
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
        };

        GLuint splitTex = getIconTexture("splitviewport");
        bool clickedSplit = false;
        if (splitTex != 0) {
            clickedSplit = ImGui::Button("##hudSplitView", squareBtn);
            ImVec2 pMin = ImGui::GetItemRectMin();
            ImVec2 pMax = ImGui::GetItemRectMax();
            float pad = 1.0f * scale;
            ImVec2 imgMin(pMin.x + pad, pMin.y + pad);
            ImVec2 imgMax(pMax.x - pad, pMax.y - pad);
            ImU32 tintCol = isSplit ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 240);
            ImGui::GetWindowDrawList()->AddImage((ImTextureID)(uintptr_t)splitTex, imgMin, imgMax, ImVec2(0, 1), ImVec2(1, 0), tintCol);
        } else {
            clickedSplit = ImGui::Button(ICON_LC_COLUMNS "##hudSplitView", squareBtn);
        }

        if (clickedSplit) {
            applySplitMode(isSplit ? Scene::SplitMode::OFF : Scene::SplitMode::MIRROR);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Split Viewport");

        ImVec2 sMin = ImGui::GetItemRectMin();

        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
        ImGui::SetWindowFontScale(fontScale * 0.65f);
        bool openSplitPopup = ImGui::Button(ICON_LC_CHEVRON_DOWN "##hudSplitViewArrow", halfSquareBtn);
        ImGui::SetWindowFontScale(fontScale);
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Split View Options");

        ImVec2 sMax = ImGui::GetItemRectMax();

        if (isSplit) {
            ImGui::PopStyleColor(3);
        }

        float sCenterX = (sMin.x + sMax.x) * 0.5f;
        float sBottomY = sMax.y + 4.0f * scale;

        if (openSplitPopup) {
            ImGui::OpenPopup("##hudSplitViewPopup");
        }

        ImGui::SetNextWindowPos(ImVec2(sCenterX, sBottomY), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
        if (ImGui::BeginPopup("##hudSplitViewPopup")) {
            ImGui::SetWindowFontScale(fontScale);
            ImGui::TextUnformatted("Split View:");
            int curSplitMode = static_cast<int>(scene.getSplitMode());
            bool changed = false;
            if (ImGui::RadioButton("Off", &curSplitMode, 0)) changed = true;
            if (ImGui::RadioButton("Mirror", &curSplitMode, 1)) changed = true;
            if (ImGui::RadioButton("Independent", &curSplitMode, 2)) changed = true;
            if (changed) {
                applySplitMode(static_cast<Scene::SplitMode>(curSplitMode));
            }
            if (scene.getSplitMode() != Scene::SplitMode::OFF) {
                ImGui::Separator();
                bool showInactive = scene.getSplitShowInactiveCursor();
                if (ImGui::Checkbox("Show Inactive Cursor", &showInactive)) {
                    scene.setSplitShowInactiveCursor(showInactive);
                }
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine();

        bool isSolo = scene.isSoloActive();
        if (isSolo) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.45f, 0.05f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.55f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.35f, 0.00f, 1.0f));
        }
        if (ImGui::Button(ICON_LC_EYE "##hudSolo", squareBtn)) {
            scene.toggleSolo(scene.getSelected());
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Solo Mode (C)");
        if (isSolo) {
            ImGui::PopStyleColor(3);
        }

        ImGui::SameLine();

        bool hasSnapshot = renderer.hasActiveSnapshot();
        if (hasSnapshot) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }
        if (ImGui::Button(ICON_LC_IMAGE "##hudSnapshot", squareBtn)) {
            renderer.toggleSnapshot(scene);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Model Snapshot (Screen Reference)");
        if (hasSnapshot) {
            ImGui::PopStyleColor(3);
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(2); // WindowPadding, ItemSpacing
    ImGui::PopStyleVar(3); // WindowRounding, FrameRounding, FramePadding
    ImGui::PopStyleColor(2); // WindowBg, Border
}

void GuiManager::drawModelSnapshotWindow(const Scene& scene, AngleRenderer& renderer) {
    if (!renderer.hasActiveSnapshot()) return;

    float scale = getUiScale();

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    float defaultWidth = 320.0f * scale;
    float defaultHeight = 260.0f * scale;

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x - defaultWidth - 20.0f * scale, viewport->Pos.y + 60.0f * scale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(defaultWidth, defaultHeight), ImGuiCond_FirstUseEver);

    ImGui::SetNextWindowSizeConstraints(ImVec2(180.0f * scale, 140.0f * scale), ImVec2(viewport->Size.x, viewport->Size.y));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar;

    bool open = true;
    if (ImGui::Begin("Model Snapshot", &open, flags)) {
        // Toolbar controls
        bool isSilhouette = renderer.isSnapshotSilhouette();

        if (ImGui::Button(ICON_LC_CAMERA " View")) {
            renderer.updateSnapshotCamera(scene);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Update snapshot camera angle to current main camera view");

        ImGui::SameLine();

        if (ImGui::Button(ICON_LC_ROTATE_CW " Refresh")) {
            renderer.markSnapshotDirty();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-render model snapshot");

        ImGui::SameLine();

        if (isSilhouette) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
        }
        if (ImGui::Button(ICON_LC_MOON " Silhouette")) {
            renderer.setSnapshotSilhouette(!isSilhouette);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Silhouette mode for snapshot window");
        if (isSilhouette) {
            ImGui::PopStyleColor(3);
        }

        ImGui::Separator();

        GLuint tex = renderer.getSnapshotTexture();
        if (tex != 0) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            if (avail.x > 1.0f && avail.y > 1.0f) {
                // Texture UV mapping: OpenGL textures are Y-flipped in ImGui. (0,1) to (1,0) flips Y correctly.
                ImGui::Image((ImTextureID)(uintptr_t)tex, avail, ImVec2(0, 1), ImVec2(1, 0));

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Frozen Model Snapshot (Auto-updates on edit)");
                }
            }
        }
    }
    ImGui::End();

    if (!open) {
        renderer.destroySnapshot();
    }
}

void GuiManager::drawUndoDiagPanel(Scene& scene) {
    if (!m_showUndoDiagPanel) return;

    float scale = getUiScale();
    ImGui::SetNextWindowSize(ImVec2(360.0f * scale, 420.0f * scale), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Undo History", &m_showUndoDiagPanel)) {
        ImGui::End();
        return;
    }

    // Header action buttons
    bool canUndo = g_undoManager.canUndo();
    bool canRedo = g_undoManager.canRedo();

    if (!canUndo) ImGui::BeginDisabled();
    if (ImGui::Button(ICON_LC_UNDO " Undo")) {
        g_undoManager.undo(scene);
    }
    if (!canUndo) ImGui::EndDisabled();

    ImGui::SameLine();
    if (!canRedo) ImGui::BeginDisabled();
    if (ImGui::Button(ICON_LC_REDO " Redo")) {
        g_undoManager.redo(scene);
    }
    if (!canRedo) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button(ICON_LC_TRASH_2 " Clear")) {
        g_undoManager.clear();
    }

    size_t memBytes = g_undoManager.getTotalMemoryUsage();
    double memMB = (double)memBytes / (1024.0 * 1024.0);
    double maxMemGB = g_undoManager.getMaxMemoryGB();

    ImGui::Separator();
    float progress = (float)(memBytes / (double)g_undoManager.getMaxMemory());
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    char memBuf[128];
    if (maxMemGB >= 1.0) {
        snprintf(memBuf, sizeof(memBuf), "%.2f MB / %.2f GB (%.1f%%)", memMB, maxMemGB, progress * 100.0f);
    } else {
        snprintf(memBuf, sizeof(memBuf), "%.2f MB / %.0f MB (%.1f%%)", memMB, maxMemGB * 1024.0, progress * 100.0f);
    }

    ImGui::Text("Memory Usage:");
    ImGui::ProgressBar(progress, ImVec2(-1.0f, 24.0f * scale), memBuf);

    float memLimitGB = (float)maxMemGB;
    ImGui::PushItemWidth(120.0f * scale);
    if (ImGui::SliderFloat("Memory Limit (GB)", &memLimitGB, 0.5f, 32.0f, "%.1f GB")) {
        if (memLimitGB < 0.1f) memLimitGB = 0.1f;
        g_undoManager.setMaxMemoryGB((double)memLimitGB);
    }
    int maxEntries = (int)g_undoManager.getMaxEntries();
    if (ImGui::SliderInt("Max Entries", &maxEntries, 0, 500, maxEntries == 0 ? "Unlimited" : "%d")) {
        if (maxEntries < 0) maxEntries = 0;
        g_undoManager.setMaxEntries((size_t)maxEntries);
    }
    ImGui::PopItemWidth();

    ImGui::Separator();

    const auto& undoStack = g_undoManager.getUndoStack();
    const auto& redoStack = g_undoManager.getRedoStack();

    ImGui::Text("Undo Entries: %zu | Redo Entries: %zu", undoStack.size(), redoStack.size());

    if (ImGui::BeginChild("UndoHistoryList", ImVec2(0, 0), true)) {
        // Render Active Undo Stack items
        for (int i = 0; i < (int)undoStack.size(); ++i) {
            const auto& entry = undoStack[i];
            bool isCurrentTop = (i == (int)undoStack.size() - 1);

            std::string label = (isCurrentTop ? "-> [" : "   [") + std::to_string(i + 1) + "] " + entry->getDescription();
            size_t entryMem = entry->getMemoryUsage();
            std::string sizeStr = (entryMem >= 1024 * 1024) ? 
                (std::to_string(entryMem / (1024 * 1024)) + " MB") : 
                (std::to_string(entryMem / 1024) + " KB");

            if (isCurrentTop) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.95f, 0.4f, 1.0f));
            }

            ImGui::PushID(i);
            if (ImGui::Selectable(label.c_str(), isCurrentTop)) {
                int stepsToUndo = (int)undoStack.size() - 1 - i;
                for (int s = 0; s < stepsToUndo; ++s) {
                    g_undoManager.undo(scene);
                }
            }
            if (ImGui::IsItemHovered()) {
                const char* typeStr = (entry->getType() == UndoEntryType::Sculpt) ? "Delta Sculpt" :
                                      (entry->getType() == UndoEntryType::Topology) ? "Topology Snapshot" : "Meta Snapshot";
                ImGui::SetTooltip("Size: %s | Type: %s\nClick to jump to this undo state", sizeStr.c_str(), typeStr);
            }
            ImGui::PopID();

            if (isCurrentTop) {
                ImGui::PopStyleColor();
            }
        }

        // Render Redo Stack items
        if (!redoStack.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("--- REDO STACK ---");

            for (int i = (int)redoStack.size() - 1; i >= 0; --i) {
                const auto& entry = redoStack[i];
                std::string label = "   (Redo) " + entry->getDescription();

                ImGui::PushID(1000000 + i);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                if (ImGui::Selectable(label.c_str(), false)) {
                    int stepsToRedo = (int)redoStack.size() - i;
                    for (int s = 0; s < stepsToRedo; ++s) {
                        g_undoManager.redo(scene);
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Click to redo to this step");
                }
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
        }

        ImGui::EndChild();
    }

    ImGui::End();
}

void GuiManager::drawDebugLogPanel() {
    if (!m_showDebugLogPanel) return;

    float scale = getUiScale();
    ImGui::SetNextWindowSize(ImVec2(550.0f * scale, 320.0f * scale), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Debug Log", &m_showDebugLogPanel)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button(ICON_LC_TRASH_2 " Clear Log")) {
        Logger::instance().clear();
    }
    ImGui::SameLine();
    static bool autoScroll = true;
    ImGui::Checkbox("Auto-scroll", &autoScroll);

    ImGui::Separator();

    if (ImGui::BeginChild("LogRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar)) {
        const auto& entries = Logger::instance().getEntries();
        for (const auto& entry : entries) {
            ImVec4 color;
            switch (entry.level) {
                case LogLevel::Debug:   color = ImVec4(0.65f, 0.65f, 0.65f, 1.0f); break;
                case LogLevel::Info:    color = ImVec4(0.85f, 0.95f, 1.0f, 1.0f); break;
                case LogLevel::Warning: color = ImVec4(1.0f, 0.85f, 0.2f, 1.0f); break;
                case LogLevel::Error:   color = ImVec4(1.0f, 0.35f, 0.35f, 1.0f); break;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::Text("[%s] %s", entry.timestamp.c_str(), entry.message.c_str());
            ImGui::PopStyleColor();
        }

        if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }

    ImGui::End();
}



void GuiManager::drawSafeFramesOverlay(const AngleRenderer& renderer, const Scene& scene) {
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    float viewportWidth = ImGui::GetIO().DisplaySize.x;
    float viewportHeight = ImGui::GetIO().DisplaySize.y;
    float scale = getUiScale();

    float margin = renderer.getSafeFramesMargin() * scale;
    float rawThickness = renderer.getSafeFramesThickness() * scale;

    float thickness = std::max(1.0f, rawThickness);
    float alphaScale = std::min(1.0f, std::max(0.05f, rawThickness));

    ImU32 lineColor = IM_COL32(240, 240, 240, (int)(220.0f * alphaScale));

    auto drawSingleSafeFrame = [&](float vx0, float vy0, float vx1, float vy1) {
        float x0 = vx0 + margin;
        float y0 = vy0 + margin;
        float x1 = vx1 - margin;
        float y1 = vy1 - margin;

        if (x1 <= x0 || y1 <= y0) return;

        ImGui::GetForegroundDrawList()->PushClipRect(ImVec2(vx0, vy0), ImVec2(vx1, vy1), true);

        // Clean Safe Frame Rect with pure line color
        drawList->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), lineColor, 0.0f, 0, thickness);

        ImGui::GetForegroundDrawList()->PopClipRect();
    };

    if (!renderer.getSplitMode()) {
        drawSingleSafeFrame(0.0f, 0.0f, viewportWidth, viewportHeight);
    } else {
        float halfW = viewportWidth * 0.5f;
        drawSingleSafeFrame(0.0f, 0.0f, halfW, viewportHeight);
        drawSingleSafeFrame(halfW, 0.0f, viewportWidth, viewportHeight);
    }
}

void GuiManager::drawTimelapsePanel(Scene& scene, AngleRenderer& renderer) {
    if (!m_showTimelapsePanel) return;

    float scale = getUiScale();
    ImGui::SetNextWindowSize(ImVec2(620.0f * scale, 230.0f * scale), ImGuiCond_FirstUseEver);

    float vpW = ImGui::GetIO().DisplaySize.x;
    float vpH = ImGui::GetIO().DisplaySize.y;
    ImGui::SetNextWindowPos(ImVec2(vpW * 0.5f, vpH - 250.0f * scale), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.0f));

    if (ImGui::Begin("Sculpt Timelapse Player", &m_showTimelapsePanel, ImGuiWindowFlags_NoCollapse)) {

        if (!m_timelapsePlayer.isOpen()) {
            ImGui::TextWrapped("The Sculpt Timelapse Player replays your sculpting session history step-by-step and exports PNG frame sequences.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            size_t undoCount = g_undoManager.getUndoStack().size();
            if (undoCount == 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "No sculpt history in RAM.");
            } else {
                ImGui::Text("Recorded session steps in history: %zu", undoCount);
                ImGui::Spacing();
                if (ImGui::Button("Open History in Player", ImVec2(200.0f * scale, 36.0f * scale))) {
                    m_timelapsePlayer.open(g_undoManager, scene);
                }
                ImGui::SameLine();
            }

            static const std::vector<FileDialog::FilterSpec> stlapseFilters = {
                {"Sculpt Timelapse (*.stlapse)", "*.stlapse"}
            };

            if (ImGui::Button("Load .stlapse File...", ImVec2(180.0f * scale, 36.0f * scale))) {
                std::string path = FileDialog::openFile(stlapseFilters, "Open Sculpt Timelapse");
                if (!path.empty()) {
                    TimelapseMetadata meta;
                    m_timelapsePlayer.loadTimelapse(path, scene, &meta);
                }
            }
        } else {
            int currentStep = m_timelapsePlayer.getCurrentStep();
            int totalSteps = m_timelapsePlayer.getTotalSteps();
            auto state = m_timelapsePlayer.getState();

            // Controls row
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * scale, 4.0f * scale));

            if (ImGui::Button("|<##Start", ImVec2(36.0f * scale, 28.0f * scale))) {
                m_timelapsePlayer.seekToStart(scene);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Jump to Start");
            ImGui::SameLine();

            if (ImGui::Button("<##Back", ImVec2(36.0f * scale, 28.0f * scale))) {
                m_timelapsePlayer.stepBackward(scene);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step Backward");
            ImGui::SameLine();

            const char* playIcon = (state == TimelapsePlayer::State::PLAYING) ? "||" : ">";
            if (ImGui::Button(playIcon, ImVec2(48.0f * scale, 28.0f * scale))) {
                m_timelapsePlayer.togglePlayPause(scene);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(state == TimelapsePlayer::State::PLAYING ? "Pause" : "Play");
            ImGui::SameLine();

            if (ImGui::Button(">##Fwd", ImVec2(36.0f * scale, 28.0f * scale))) {
                m_timelapsePlayer.stepForward(scene);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step Forward");
            ImGui::SameLine();

            if (ImGui::Button(">|##End", ImVec2(36.0f * scale, 28.0f * scale))) {
                m_timelapsePlayer.seekToEnd(scene);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Jump to End");

            ImGui::PopStyleVar();

            ImGui::SameLine();
            ImGui::Text("Step: %d / %d", currentStep, totalSteps);

            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", m_timelapsePlayer.getCurrentDescription().c_str());

            // Timeline slider
            ImGui::Spacing();
            int sliderStep = currentStep;
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderInt("##TimelapseTimeline", &sliderStep, 0, totalSteps, "Step: %d")) {
                m_timelapsePlayer.seekToStep(sliderStep, scene);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Options & Export & Save .stlapse
            float playSpeed = m_timelapsePlayer.getPlaySpeed();
            ImGui::SetNextItemWidth(140.0f * scale);
            if (ImGui::SliderFloat("Speed (steps/sec)", &playSpeed, 1.0f, 60.0f, "%.1f")) {
                m_timelapsePlayer.setPlaySpeed(playSpeed);
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(100.0f * scale);
            ImGui::InputInt("Steps / Frame", &m_exportStepsPerFrame);
            if (m_exportStepsPerFrame < 1) m_exportStepsPerFrame = 1;

            static const std::vector<FileDialog::FilterSpec> stlapseFilters = {
                {"Sculpt Timelapse (*.stlapse)", "*.stlapse"}
            };

            ImGui::SameLine();
            if (ImGui::Button("Save .stlapse...", ImVec2(130.0f * scale, 26.0f * scale))) {
                std::string path = FileDialog::saveFile(stlapseFilters, "stlapse", "Save Sculpt Timelapse");
                if (!path.empty()) {
                    TimelapseMetadata meta;
                    meta.title = "Sculpt Timelapse";
                    meta.totalStrokes = m_timelapsePlayer.getTotalSteps();
                    m_timelapsePlayer.saveTimelapse(path, meta);
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save entire timelapse recording to a .stlapse binary file");

            ImGui::SameLine();
            if (ImGui::Button("Export PNGs...", ImVec2(130.0f * scale, 26.0f * scale))) {
                ImGui::OpenPopup("Export Timelapse PNGs");
            }

            ImGui::SameLine();
            if (ImGui::Button("Close Player", ImVec2(100.0f * scale, 26.0f * scale))) {
                m_timelapsePlayer.close(g_undoManager, scene);
            }

            // Modal for export config
            if (ImGui::BeginPopupModal("Export Timelapse PNGs", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Configure frame export options:");
                ImGui::Spacing();

                ImGui::InputText("Output Directory", m_exportDir, sizeof(m_exportDir));
                ImGui::InputInt("Width", &m_exportWidth);
                ImGui::InputInt("Height", &m_exportHeight);
                ImGui::InputInt("Steps per Frame", &m_exportStepsPerFrame);

                if (m_exportWidth < 64) m_exportWidth = 64;
                if (m_exportHeight < 64) m_exportHeight = 64;
                if (m_exportStepsPerFrame < 1) m_exportStepsPerFrame = 1;

                int frameCount = (totalSteps + m_exportStepsPerFrame - 1) / m_exportStepsPerFrame;
                ImGui::Text("Total frames to export: %d", frameCount);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::Button("Start Export", ImVec2(120.0f * scale, 30.0f * scale))) {
                    ImGui::CloseCurrentPopup();
                    m_timelapsePlayer.exportFrames(scene, renderer, m_exportDir, m_exportWidth, m_exportHeight, m_exportStepsPerFrame,
                        [](int current, int total) {
                            sculpt_log("[Timelapse Export] Exporting frame %d / %d...\n", current, total);
                        });
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120.0f * scale, 30.0f * scale))) {
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }
        }
    }
    ImGui::End();
}

void GuiManager::drawPreferencesPanel(SculptManager& sculpt, Scene& scene, AngleRenderer& renderer, SDL_Window* window) {
    if (!m_showPreferencesPanel) return;

    float scale = getUiScale();

    ImGui::SetNextWindowPos(ImVec2(120.0f * scale, 120.0f * scale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(640.0f * scale, 520.0f * scale), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Preferences & Application Settings", &m_showPreferencesPanel, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("##PreferencesTabs")) {

            // 1. Interface & Display Tab
            if (ImGui::BeginTabItem("Interface & Display")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Scaling & Layout");
                ImGui::Separator();
                ImGui::Spacing();

                // Global UI Scale
                ImGui::SetNextItemWidth(180.0f * scale);
                if (ImGui::SliderFloat("Global UI Scale", &m_uiScale, 0.5f, 2.5f, "%.2fx")) {
                    if (m_uiScale < 0.5f) m_uiScale = 0.5f;
                    if (m_uiScale > 2.5f) m_uiScale = 2.5f;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    m_pendingUiScaleRefresh = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset UI Scale")) {
                    m_uiScale = 1.0f;
                    m_pendingUiScaleRefresh = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset Global UI scale multiplier to default (1.0x)");

                ImGui::Spacing();

                // Floating Island HUD Scale
                ImGui::SetNextItemWidth(180.0f * scale);
                if (ImGui::SliderFloat("Floating HUD Scale", &m_floatingIslandScale, 0.5f, 2.5f, "%.2fx")) {
                    if (m_floatingIslandScale < 0.5f) m_floatingIslandScale = 0.5f;
                    if (m_floatingIslandScale > 2.5f) m_floatingIslandScale = 2.5f;
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset HUD Scale")) {
                    m_floatingIslandScale = 1.0f;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset Floating Island HUD scale multiplier to default (1.0x)");

                ImGui::Spacing();
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Gizmo & Viewport Overlays");
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::SetNextItemWidth(180.0f * scale);
                ImGui::SliderFloat("Transform Gizmo Size", &m_gizmoSize, 0.04f, 0.25f, "%.2f");
                if (m_gizmoSize < 0.04f) m_gizmoSize = 0.04f;
                if (m_gizmoSize > 0.25f) m_gizmoSize = 0.25f;

                ImGui::Spacing();
                ImGui::Checkbox("Show Floating Island HUD", &m_showFloatingIsland);
                ImGui::Checkbox("Show Navigation Cube", &m_showGizmoCube);
                ImGui::Checkbox("Show Point Count & FPS HUD", &m_showMeshInfo);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle the Mesh Statistics HUD (active/total points and FPS) in the bottom-right corner");
                ImGui::Checkbox("Show Hotkey HUD (Bottom-Left)", &m_showHotkeyHUD);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle the Keyboard Shortcuts HUD overlay in the bottom-left corner");

                ImGui::EndTabItem();
            }

            // 2. Camera & Viewport Tab
            ImGuiTabItemFlags cameraTabFlags = (m_preferencesActiveTab == 1) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Camera & Viewport", nullptr, cameraTabFlags)) {
                Camera& camera = scene.getCamera();

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Projection & View Mode");
                ImGui::Separator();
                ImGui::Spacing();

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

                const char* modes[] = { "Orbit", "Plane Trackball", "Spherical Trackball" };
                int currentMode = (int)camera.getMode();
                if (ImGui::Combo("Camera Mode", &currentMode, modes, IM_ARRAYSIZE(modes))) {
                    camera.setMode((CameraEnums::CameraMode)currentMode);
                }

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Safe Frames Overlay");
                ImGui::Separator();
                ImGui::Spacing();

                bool showSafeFrames = renderer.getShowSafeFrames();
                if (ImGui::Checkbox("Enable Safe Frames", &showSafeFrames)) {
                    renderer.setShowSafeFrames(showSafeFrames);
                }
                if (showSafeFrames) {
                    ImGui::Indent();
                    float sfMargin = renderer.getSafeFramesMargin();
                    if (ImGui::SliderFloat("Margin##SFMargin", &sfMargin, 5.0f, 200.0f, "%.0f px")) {
                        renderer.setSafeFramesMargin(sfMargin);
                    }
                    float sfThickness = renderer.getSafeFramesThickness();
                    if (ImGui::SliderFloat("Line Thickness##SFThickness", &sfThickness, 0.1f, 5.0f, "%.1f px")) {
                        renderer.setSafeFramesThickness(sfThickness);
                    }
                    ImGui::Unindent();
                }

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Camera Movement Speeds");
                ImGui::Separator();
                ImGui::Spacing();

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

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Snap Views & Actions");
                ImGui::Separator();
                ImGui::Spacing();

                float btnW = 75.0f * scale;
                if (ImGui::Button("Front", ImVec2(btnW, 0))) {
                    camera.setOrbitAngles(0.0f, 0.0f);
                    camera.setProjectionType(CameraEnums::Projection::ORTHOGRAPHIC);
                }
                ImGui::SameLine();
                if (ImGui::Button("Back", ImVec2(btnW, 0))) {
                    camera.setOrbitAngles(0.0f, 3.14159265f);
                    camera.setProjectionType(CameraEnums::Projection::ORTHOGRAPHIC);
                }
                ImGui::SameLine();
                if (ImGui::Button("Top", ImVec2(btnW, 0))) {
                    camera.setOrbitAngles(-3.14159265f * 0.49f, 0.0f);
                    camera.setProjectionType(CameraEnums::Projection::ORTHOGRAPHIC);
                }

                if (ImGui::Button("Bottom", ImVec2(btnW, 0))) {
                    camera.setOrbitAngles(3.14159265f * 0.49f, 0.0f);
                    camera.setProjectionType(CameraEnums::Projection::ORTHOGRAPHIC);
                }
                ImGui::SameLine();
                if (ImGui::Button("Left", ImVec2(btnW, 0))) {
                    camera.setOrbitAngles(0.0f, 3.14159265f * 0.5f);
                    camera.setProjectionType(CameraEnums::Projection::ORTHOGRAPHIC);
                }
                ImGui::SameLine();
                if (ImGui::Button("Right", ImVec2(btnW, 0))) {
                    camera.setOrbitAngles(0.0f, -3.14159265f * 0.5f);
                    camera.setProjectionType(CameraEnums::Projection::ORTHOGRAPHIC);
                }

                ImGui::Spacing();
                if (ImGui::Button("Frame Selection (F)", ImVec2(160.0f * scale, 0))) {
                    std::vector<Mesh*> selected = scene.getSelectedMeshes();
                    if (selected.empty() && scene.getSelected()) selected.push_back(scene.getSelected());
                    if (selected.empty()) {
                        for (Mesh* m : scene.getMeshes()) {
                            if (m && m->isVisible(scene.getActiveViewport())) selected.push_back(m);
                        }
                    }
                    if (!selected.empty()) camera.resetViewToMeshes(selected);
                    else camera.resetView();
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset Camera", ImVec2(120.0f * scale, 0))) {
                    camera.resetView();
                }

                ImGui::Spacing();
                if (ImGui::Button("Take Model Snapshot", ImVec2(180.0f * scale, 0))) {
                    renderer.createSnapshot(scene);
                }

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Split Viewport");
                ImGui::Separator();
                ImGui::Spacing();

                int splitModeVal = static_cast<int>(scene.getSplitMode());
                bool splitChanged = false;
                if (ImGui::RadioButton("Off", &splitModeVal, 0)) splitChanged = true;
                ImGui::SameLine();
                if (ImGui::RadioButton("Mirror", &splitModeVal, 1)) splitChanged = true;
                ImGui::SameLine();
                if (ImGui::RadioButton("Independent", &splitModeVal, 2)) splitChanged = true;

                if (splitChanged) {
                    scene.setSplitMode(static_cast<Scene::SplitMode>(splitModeVal));
                    int w = (int)ImGui::GetIO().DisplaySize.x;
                    int h = (int)ImGui::GetIO().DisplaySize.y;
                    if (window) SDL_GetWindowSize(window, &w, &h);
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

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Screenshot Export");
                ImGui::Separator();
                ImGui::Spacing();

                const char* screenshotPresets[] = { "Viewport Size", "1080p (1920x1080)", "2K (2560x1440)", "4K (3840x2160)", "Custom" };
                ImGui::Combo("Preset##Screenshot", &m_screenshotPreset, screenshotPresets, IM_ARRAYSIZE(screenshotPresets));

                if (m_screenshotPreset == 4) { // Custom
                    ImGui::InputInt("Width##Screenshot", &m_screenshotWidth);
                    ImGui::InputInt("Height##Screenshot", &m_screenshotHeight);
                    if (m_screenshotWidth < 256) m_screenshotWidth = 256;
                    if (m_screenshotWidth > 7680) m_screenshotWidth = 7680;
                    if (m_screenshotHeight < 256) m_screenshotHeight = 256;
                    if (m_screenshotHeight > 4320) m_screenshotHeight = 4320;
                }

                ImGui::Checkbox("Show Grid##Screenshot", &m_screenshotShowGrid);
                ImGui::Checkbox("Show Contour##Screenshot", &m_screenshotShowContour);

                if (ImGui::Button("Take Screenshot", ImVec2(160.0f * scale, 0))) {
                    takeScreenshot(scene, renderer);
                }

                ImGui::EndTabItem();
            }

            // 3. Rendering Quality Tab
            ImGuiTabItemFlags renderTabFlags = (m_preferencesActiveTab == 2) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Rendering Quality", nullptr, renderTabFlags)) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Material & Shading Mode");
                ImGui::Separator();
                ImGui::Spacing();

                const char* shaders[] = { "PBR Shader", "Matcap Shading", "Wet Clay Shading", "Normal Shader", "Voxel Checker Shader", "Flat Shading", "Silhouette Shader" };
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

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Viewport Overlays & Shading Features");
                ImGui::Separator();
                ImGui::Spacing();

                bool wire = renderer.getShowWireframe();
                if (ImGui::Checkbox("Show Wireframe", &wire)) {
                    renderer.setShowWireframe(wire);
                }
                ImGui::SameLine();
                bool polyGroups = renderer.getShowPolyGroups();
                if (ImGui::Checkbox("Show PolyGroups", &polyGroups)) {
                    renderer.setShowPolyGroups(polyGroups);
                }

                bool flat = renderer.getFlatShading();
                if (ImGui::Checkbox("Flat Shading Mode", &flat)) {
                    renderer.setFlatShading(flat);
                }

                bool darkenUnselected = renderer.getDarkenUnselected();
                if (ImGui::Checkbox("Darken unselected", &darkenUnselected)) {
                    renderer.setDarkenUnselected(darkenUnselected);
                }

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Post-Processing Effects");
                ImGui::Separator();
                ImGui::Spacing();

                bool bevel = renderer.getBevelEnabled();
                if (ImGui::Checkbox("Screen-space Bevel (Melt)", &bevel)) {
                    renderer.setBevelEnabled(bevel);
                }
                if (bevel) {
                    ImGui::Indent();
                    float radius = renderer.getBevelRadius();
                    if (ImGui::SliderFloat("Bevel Radius", &radius, 1.0f, 8.0f, "%.1f px")) {
                        renderer.setBevelRadius(radius);
                    }
                    float strength = renderer.getBevelStrength();
                    if (ImGui::SliderFloat("Bevel Strength", &strength, 0.1f, 5.0f, "%.2f")) {
                        renderer.setBevelStrength(strength);
                    }
                    bool scaleBevel = renderer.getBevelScaleWithDistance();
                    if (ImGui::Checkbox("Constant World-space Size", &scaleBevel)) {
                        renderer.setBevelScaleWithDistance(scaleBevel);
                    }
                    ImGui::Unindent();
                }

                bool filmic = renderer.getFilmic();
                if (ImGui::Checkbox("Filmic Tonemapping", &filmic)) {
                    renderer.setFilmic(filmic);
                }

                // Anti-Aliasing & Temporal Stability Controls
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Anti-Aliasing & Temporal Stability");
                ImGui::Separator();
                ImGui::Spacing();

                bool useFxaa = renderer.getUseFxaa();
                if (ImGui::Checkbox("FXAA Anti-aliasing", &useFxaa)) {
                    renderer.setUseFxaa(useFxaa);
                }
                if (useFxaa) {
                    ImGui::Indent();
                    bool fxaaSharp = renderer.getFxaaSharpMode();
                    if (ImGui::Checkbox("FXAA High Sharpness Mode", &fxaaSharp)) {
                        renderer.setFxaaSharpMode(fxaaSharp);
                    }
                    ImGui::Unindent();
                }

                bool useTaa = renderer.getUseTaa();
                if (ImGui::Checkbox("Temporal Anti-Aliasing (TAA)", &useTaa)) {
                    renderer.setUseTaa(useTaa);
                }
                if (useTaa) {
                    ImGui::Indent();
                    bool taaResetStroke = renderer.getTaaResetOnStroke();
                    if (ImGui::Checkbox("Reset TAA History on Sculpting Stroke", &taaResetStroke)) {
                        renderer.setTaaResetOnStroke(taaResetStroke);
                    }
                    ImGui::Unindent();
                }

                bool useMotionVec = renderer.getUseMotionVectors();
                if (ImGui::Checkbox("Motion Vectors Buffer (gMotionVec)", &useMotionVec)) {
                    renderer.setUseMotionVectors(useMotionVec);
                }

                bool useReverseZ = renderer.getUseReverseZ();
                if (ImGui::Checkbox("Reverse-Z Depth Buffer (Depth Precision)", &useReverseZ)) {
                    renderer.setUseReverseZ(useReverseZ);
                }

                bool usePolyOffset = renderer.getUsePolygonOffset();
                if (ImGui::Checkbox("Polygon Offset Overlays (Reduce Z-Fighting)", &usePolyOffset)) {
                    renderer.setUsePolygonOffset(usePolyOffset);
                }

                int anisotropyLevel = renderer.getAnisotropyLevel();
                if (ImGui::SliderInt("Anisotropic Filtering Level", &anisotropyLevel, 1, 16)) {
                    renderer.setAnisotropyLevel(anisotropyLevel);
                }

                bool useSubpixelCull = renderer.getUseSubpixelCulling();
                if (ImGui::Checkbox("Sub-pixel Mesh Culling", &useSubpixelCull)) {
                    renderer.setUseSubpixelCulling(useSubpixelCull);
                }
                if (useSubpixelCull) {
                    ImGui::Indent();
                    float cullThresh = renderer.getSubpixelCullThreshold();
                    if (ImGui::SliderFloat("Cull Radius Threshold", &cullThresh, 0.1f, 2.0f, "%.2f px")) {
                        renderer.setSubpixelCullThreshold(cullThresh);
                    }
                    ImGui::Unindent();
                }

                bool useSsao = renderer.getUseSsao();
                if (ImGui::Checkbox("SSAO Ambient Occlusion", &useSsao)) {
                    renderer.setUseSsao(useSsao);
                }
                if (useSsao) {
                    ImGui::Indent();
                    float ssaoRad = renderer.getSsaoRadius();
                    if (ImGui::SliderFloat("SSAO Radius", &ssaoRad, 0.05f, 2.0f, "%.2f")) {
                        renderer.setSsaoRadius(ssaoRad);
                    }
                    float ssaoBias = renderer.getSsaoBias();
                    if (ImGui::SliderFloat("SSAO Bias", &ssaoBias, 0.001f, 0.2f, "%.3f")) {
                        renderer.setSsaoBias(ssaoBias);
                    }
                    float ssaoInt = renderer.getSsaoIntensity();
                    if (ImGui::SliderFloat("SSAO Intensity", &ssaoInt, 0.0f, 3.0f, "%.2f")) {
                        renderer.setSsaoIntensity(ssaoInt);
                    }
                    ImGui::Unindent();
                }

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Advanced GPU Pipeline");
                ImGui::Separator();
                ImGui::Spacing();

                int renderMode = (int)renderer.getRenderMode();
                const char* renderModes[] = { "Real-time PBR", "Progressive Path Tracing (SSPT)" };
                if (ImGui::Combo("Engine Mode", &renderMode, renderModes, 2)) {
                    renderer.setRenderMode((RenderMode)renderMode);
                }

                bool shadowEnabled = renderer.getUseShadows();
                if (ImGui::Checkbox("Shadow Mapping", &shadowEnabled)) {
                    renderer.setUseShadows(shadowEnabled);
                }

                bool useSsr = renderer.getUseSsr();
                if (ImGui::Checkbox("Screen Space Reflections (SSR)", &useSsr)) {
                    renderer.setUseSsr(useSsr);
                }
                if (useSsr) {
                    ImGui::Indent();
                    float ssrIntensity = renderer.getSsrIntensity();
                    if (ImGui::SliderFloat("SSR Intensity", &ssrIntensity, 0.0f, 2.0f, "%.2f")) {
                        renderer.setSsrIntensity(ssrIntensity);
                    }
                    ImGui::Unindent();
                }

                ImGui::Spacing();
                if (ImGui::TreeNode("Light Source Management")) {
                    auto& lights = const_cast<Scene&>(scene).getLights();
                    if (ImGui::Button("Add Light", ImVec2(100, 24))) {
                        LightSource newLight;
                        newLight.name = "Light " + std::to_string(lights.size() + 1);
                        newLight.type = LightType::POINT;
                        newLight.position = glm::vec3(0.0f, 10.0f, 10.0f);
                        newLight.color = glm::vec3(1.0f, 1.0f, 1.0f);
                        newLight.intensity = 2.0f;
                        const_cast<Scene&>(scene).addLight(newLight);
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(Max 8 lights)");

                    int lightToRemove = -1;
                    for (size_t i = 0; i < lights.size(); ++i) {
                        auto& L = lights[i];
                        std::string label = L.name + "##light_" + std::to_string(i);
                        if (ImGui::TreeNode(label.c_str())) {
                            ImGui::Checkbox("Enabled", &L.enabled);
                            ImGui::SameLine();
                            ImGui::Checkbox("Cast Shadow", &L.castShadow);

                            int lType = (int)L.type;
                            const char* lTypeNames[] = { "Directional", "Point", "Spot" };
                            if (ImGui::Combo("Type", &lType, lTypeNames, 3)) {
                                L.type = (LightType)lType;
                            }

                            if (L.type == LightType::DIRECTIONAL) {
                                ImGui::SliderFloat3("Direction", glm::value_ptr(L.direction), -1.0f, 1.0f);
                                if (glm::length(L.direction) > 0.001f) {
                                    L.direction = glm::normalize(L.direction);
                                }
                            } else {
                                ImGui::DragFloat3("Position", glm::value_ptr(L.position), 0.5f);
                                if (L.type == LightType::SPOT) {
                                    ImGui::SliderFloat3("Direction", glm::value_ptr(L.direction), -1.0f, 1.0f);
                                    if (glm::length(L.direction) > 0.001f) {
                                        L.direction = glm::normalize(L.direction);
                                    }
                                    ImGui::SliderFloat("Inner Angle", &L.innerAngle, 1.0f, 89.0f);
                                    ImGui::SliderFloat("Outer Angle", &L.outerAngle, L.innerAngle, 89.0f);
                                }
                                ImGui::SliderFloat("Range", &L.range, 1.0f, 100.0f);
                            }

                            ImGui::ColorEdit3("Color", glm::value_ptr(L.color));
                            ImGui::SliderFloat("Intensity", &L.intensity, 0.0f, 20.0f);

                            if (lights.size() > 1) {
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
                                if (ImGui::Button("Remove Light")) {
                                    lightToRemove = (int)i;
                                }
                                ImGui::PopStyleColor();
                            }

                            ImGui::TreePop();
                        }
                    }

                    if (lightToRemove >= 0) {
                        const_cast<Scene&>(scene).removeLight(lightToRemove);
                    }

                    ImGui::TreePop();
                }

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Selection Outline & Cursor");
                ImGui::Separator();
                ImGui::Spacing();

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

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Material Settings");
                ImGui::Separator();
                ImGui::Spacing();

                float alpha = renderer.getAlpha();
                if (ImGui::SliderFloat("Transparency (Alpha)", &alpha, 0.0f, 1.0f, "%.2f")) {
                    renderer.setAlpha(alpha);
                }

                bool useVertexColors = renderer.getUseVertexColors();
                if (ImGui::Checkbox("Use Vertex Colors", &useVertexColors)) {
                    renderer.setUseVertexColors(useVertexColors);
                }
                ImGui::SameLine();
                bool useVertexMaterials = renderer.getUseVertexMaterials();
                if (ImGui::Checkbox("Use Vertex Materials", &useVertexMaterials)) {
                    renderer.setUseVertexMaterials(useVertexMaterials);
                }

                ImGui::BeginDisabled(useVertexColors);
                float albedo[3] = { renderer.getAlbedo()[0], renderer.getAlbedo()[1], renderer.getAlbedo()[2] };
                if (ImGui::ColorEdit3("Albedo Base Color", albedo)) {
                    renderer.setAlbedo(albedo[0], albedo[1], albedo[2]);
                }
                ImGui::EndDisabled();

                if (type == 0) { // PBR Shader settings
                    ImGui::BeginDisabled(useVertexMaterials);
                    float roughness = renderer.getRoughness();
                    if (ImGui::SliderFloat("Roughness", &roughness, 0.0f, 1.0f, "%.2f")) {
                        renderer.setRoughness(roughness);
                    }
                    float metallic = renderer.getMetallic();
                    if (ImGui::SliderFloat("Metallic", &metallic, 0.0f, 1.0f, "%.2f")) {
                        renderer.setMetallic(metallic);
                    }
                    ImGui::EndDisabled();

                    ImGui::Separator();
                    ImGui::Text("Glass / Transmission Settings:");
                    float transmission = renderer.getTransmission();
                    if (ImGui::SliderFloat("Glass Transmission", &transmission, 0.0f, 1.0f, "%.2f")) {
                        renderer.setTransmission(transmission);
                    }
                    ImGui::BeginDisabled(transmission <= 0.0f);
                    float ior = renderer.getIor();
                    if (ImGui::SliderFloat("Index of Refraction (IOR)", &ior, 1.0f, 2.5f, "%.2f")) {
                        renderer.setIor(ior);
                    }
                    ImGui::EndDisabled();

                    ImGui::Separator();
                    ImGui::Text("Subsurface Scattering (SSS):");
                    float sssIntensity = renderer.getSssIntensity();
                    if (ImGui::SliderFloat("SSS Intensity", &sssIntensity, 0.0f, 2.0f, "%.2f")) {
                        renderer.setSssIntensity(sssIntensity);
                    }
                    float sssDepth = renderer.getSssDepth();
                    if (ImGui::SliderFloat("SSS Depth (Radius)", &sssDepth, 0.1f, 5.0f, "%.2f")) {
                        renderer.setSssDepth(sssDepth);
                    }
                    glm::vec3 sssCol = renderer.getSssColor();
                    if (ImGui::ColorEdit3("SSS Color", glm::value_ptr(sssCol))) {
                        renderer.setSssColor(sssCol);
                    }

                    ImGui::Separator();

                    static char texturePath[256] = "";
                    ImGui::InputText("UV Texture Path", texturePath, sizeof(texturePath));
                    ImGui::SameLine();
                    if (ImGui::Button("Import UV Texture")) {
                        int w, h, channels;
                        unsigned char* data = stbi_load(texturePath, &w, &h, &channels, 4);
                        if (data) {
                            GLuint tid = renderer.getTextureId();
                            if (tid != 0) glDeleteTextures(1, &tid);
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

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Background Settings");
                ImGui::Separator();
                ImGui::Spacing();

                bool showBg = renderer.getShowBackground();
                if (ImGui::Checkbox("Show Background", &showBg)) {
                    renderer.setShowBackground(showBg);
                }

                if (showBg) {
                    const char* bgTypes[] = { "Image", "Environment", "Ambient env" };
                    int bgType = renderer.getBackgroundType();
                    int selectedIdx = bgType;
                    if (selectedIdx < 0 || selectedIdx > 2) selectedIdx = 0;
                    if (ImGui::Combo("Type##bg", &selectedIdx, bgTypes, IM_ARRAYSIZE(bgTypes))) {
                        renderer.setBackgroundType(selectedIdx);
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
                        if (bgImagePath[0] == '\0' && !path.empty()) {
                            strncpy(bgImagePath, path.c_str(), sizeof(bgImagePath));
                        }
                        ImGui::InputText("Bg Image Path", bgImagePath, sizeof(bgImagePath));
                        ImGui::SameLine();
                        if (ImGui::Button("Import Bg Image")) {
                            renderer.setBgTexturePath(bgImagePath);
                        }
                    }
                }

                ImGui::EndTabItem();
            }

            // 4. Performance Tab
            if (ImGui::BeginTabItem("Performance")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Frame Rate Control");
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Checkbox("Limit Render Frame Rate (FPS)", &m_fpsLimitEnabled);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cap the render loop to the target FPS to reduce GPU load and battery usage");

                if (m_fpsLimitEnabled) {
                    ImGui::SetNextItemWidth(180.0f * scale);
                    ImGui::SliderInt("Target FPS Cap", &m_fpsLimit, 15, 240, "%d FPS");
                    if (m_fpsLimit < 15) m_fpsLimit = 15;
                    if (m_fpsLimit > 240) m_fpsLimit = 240;
                }

                ImGui::Spacing();
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Statistics");
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Current Measured Frame Rate: %d FPS", m_fpsValue);

                ImGui::EndTabItem();
            }

            // 5. Undo & System Tab
            if (ImGui::BeginTabItem("Undo & System")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Undo History Allocation");
                ImGui::Separator();
                ImGui::Spacing();

                float maxMemGB = (float)g_undoManager.getMaxMemoryGB();
                ImGui::SetNextItemWidth(180.0f * scale);
                if (ImGui::SliderFloat("Max RAM Limit (GB)", &maxMemGB, 0.5f, 16.0f, "%.1f GB")) {
                    g_undoManager.setMaxMemoryGB((double)maxMemGB);
                }

                int maxEntries = (int)g_undoManager.getMaxEntries();
                ImGui::SetNextItemWidth(180.0f * scale);
                if (ImGui::SliderInt("Max Undo History Steps", &maxEntries, 5, 200, "%d steps")) {
                    g_undoManager.setMaxEntries((size_t)maxEntries);
                }

                ImGui::Spacing();
                ImGui::Text("Current Memory Used: %.2f MB (%zu entries)",
                    (double)g_undoManager.getTotalMemoryUsage() / (1024.0 * 1024.0), g_undoManager.getUndoCount());

                ImGui::Spacing();
                if (ImGui::Button("Clear Undo History")) {
                    g_undoManager.clear();
                }

                ImGui::EndTabItem();
            }

            // 6. Debug Log Tab
            ImGuiTabItemFlags debugTabFlags = (m_preferencesActiveTab == 5) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem("Debug Log", nullptr, debugTabFlags)) {
                ImGui::Spacing();
                if (ImGui::Button(ICON_LC_TRASH_2 " Clear Log")) {
                    Logger::instance().clear();
                }
                ImGui::SameLine();
                static bool autoScroll = true;
                ImGui::Checkbox("Auto-scroll", &autoScroll);

                ImGui::Separator();

                if (ImGui::BeginChild("LogRegionInPrefs", ImVec2(0, 300.0f * scale), true, ImGuiWindowFlags_HorizontalScrollbar)) {
                    const auto& entries = Logger::instance().getEntries();
                    for (const auto& entry : entries) {
                        ImVec4 color;
                        switch (entry.level) {
                            case LogLevel::Debug:   color = ImVec4(0.65f, 0.65f, 0.65f, 1.0f); break;
                            case LogLevel::Info:    color = ImVec4(0.85f, 0.95f, 1.0f, 1.0f); break;
                            case LogLevel::Warning: color = ImVec4(1.0f, 0.85f, 0.2f, 1.0f); break;
                            case LogLevel::Error:   color = ImVec4(1.0f, 0.35f, 0.35f, 1.0f); break;
                        }
                        ImGui::PushStyleColor(ImGuiCol_Text, color);
                        ImGui::Text("[%s] %s", entry.timestamp.c_str(), entry.message.c_str());
                        ImGui::PopStyleColor();
                    }

                    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                        ImGui::SetScrollHereY(1.0f);
                    }
                    ImGui::EndChild();
                }

                ImGui::EndTabItem();
            }

            if (m_preferencesActiveTab >= 0) {
                m_preferencesActiveTab = -1;
            }

            ImGui::EndTabBar();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Save Settings", ImVec2(130.0f * scale, 28.0f * scale))) {
            IniFile ini;
            saveSettings(ini);
            sculpt.saveSettings(ini);
            ini.save("app_settings.cfg");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save settings to app_settings.cfg");

        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(100.0f * scale, 28.0f * scale))) {
            m_showPreferencesPanel = false;
        }
    }
    ImGui::End();
}

void GuiManager::drawHotkeyHUD() {
    if (!m_showHotkeyHUD) return;

    float scale = getUiScale();
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.75f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.09f, 0.10f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.01f, 0.52f, 0.45f, 0.40f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * scale, 6.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * scale, 2.0f * scale));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoMove;

    if (ImGui::Begin("Hotkey HUD", nullptr, flags)) {
        struct HotkeyEntry {
            const char* key;
            const char* desc;
        };

        static const HotkeyEntry shortcuts[] = {
            { "Q",      "Move Brush" },
            { "W",      "Clay Buildup" },
            { "E",      "Dam Standard" },
            { "R",      "Pinch Brush" },
            { "A",      "Intensity" },
            { "S",      "Radius / Size" },
            { "D",      "Focal Shift" },
            { "F",      "Frame View" },
            { "G",      "Camera FOV" },
            { "X",      "Remesh Res" },
            { "Ctrl+X", "Voxel Remesh" },
            { "C",      "Solo Mode" }
        };

        if (ImGui::BeginTable("##HotkeyHUDTable", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 50.0f * scale);
            ImGui::TableSetupColumn("Desc", ImGuiTableColumnFlags_WidthStretch);

            for (const auto& item : shortcuts) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "%s", item.key);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", item.desc);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}



