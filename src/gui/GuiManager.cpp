#include "gui/GuiManager.h"
#include "render/AngleRenderer.h"
#include "render/RenderSettings.h"
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#include <vector>
#include <string>
#include <iostream>
#include <cstdio>
#include <algorithm>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "mesh/Topology.h"

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

void GuiManager::render(SculptManager& sculpt, Scene& scene, AngleRenderer& renderer, SDL_Window* window) {
    if (!m_imguiInitialized) return;

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
            if (ImGui::MenuItem("Save Render Settings")) {
                RenderSettings::save("render_settings.cfg", renderer, scene);
            }
            if (ImGui::MenuItem("Load Render Settings")) {
                RenderSettings::load("render_settings.cfg", renderer, scene);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Panels")) {
            ImGui::MenuItem("Toolbar", nullptr, &m_showToolbar);
            ImGui::MenuItem("Sculpting Settings", nullptr, &m_showSculptingPanel);
            ImGui::MenuItem("Scene Outliner", nullptr, &m_showScenePanel);
            ImGui::MenuItem("Topology & Remesh", nullptr, &m_showTopologyPanel);
            ImGui::MenuItem("Camera & Viewport", nullptr, &m_showCameraPanel);
            ImGui::MenuItem("Rendering Quality", nullptr, &m_showRenderingPanel);
            ImGui::MenuItem("Reference Images", nullptr, &m_showReferenceImagesPanel);
            ImGui::MenuItem("Navigation Cube", nullptr, &m_showGizmoCube);
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
            "Mask", "Paint", "Twist", "Local Scale", "Clay", "Clay Buildup", "Dam Standard", "Square Brush", "Visibility"
        };
        BrushType current = sculpt.getBrush();
        for (int i = 0; i < 18; i++) {
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
        ImGui::SetNextWindowSize({280, 200}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Sculpting Settings", &m_showSculptingPanel, ImGuiWindowFlags_AlwaysAutoResize);
        
        float radius = sculpt.getBrushRadius();
        if (ImGui::SliderFloat("Radius", &radius, 1.0f, 100.0f, "%.1f")) {
            sculpt.setBrushRadius(radius);
        }
        
        float intensity = sculpt.getBrushIntensity();
        if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 1.0f, "%.2f")) {
            sculpt.setBrushIntensity(intensity);
        }

        float focal = sculpt.getFocalShift();
        if (ImGui::SliderFloat("Focal Shift", &focal, -1.0f, 1.0f, "%.2f")) {
            sculpt.setFocalShift(focal);
        }

        float hardness = sculpt.getHardness();
        if (ImGui::SliderFloat("Hardness", &hardness, 0.0f, 1.0f, "%.2f")) {
            sculpt.setHardness(hardness);
        }

        bool negative = sculpt.getNegative();
        if (ImGui::Checkbox("Negative (Invert)", &negative)) {
            sculpt.setNegative(negative);
        }

        if (sculpt.getBrush() == BRUSH_PAINT) {
            ImGui::Separator();
            ImGui::Text("Paint Tool Settings:");
            glm::vec3 col = sculpt.getPaintColor();
            if (ImGui::ColorEdit3("Paint Color", &col.r)) {
                sculpt.setPaintColor(col);
            }
            float rough = sculpt.getPaintRoughness();
            if (ImGui::SliderFloat("Paint Roughness", &rough, 0.0f, 1.0f, "%.2f")) {
                sculpt.setPaintRoughness(rough);
            }
            float metal = sculpt.getPaintMetallic();
            if (ImGui::SliderFloat("Paint Metalness", &metal, 0.0f, 1.0f, "%.2f")) {
                sculpt.setPaintMetallic(metal);
            }
        }

        ImGui::Separator();
        ImGui::Text("Active Brush: %s", getBrushNameLocal(sculpt.getBrush()));

        ImGui::End();
    }

    // 4. Scene outliner
    if (m_showScenePanel) {
        ImGui::SetNextWindowPos({450, 40}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({280, 300}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Scene Outliner", &m_showScenePanel);

        const auto& meshes = scene.getMeshes();
        int selected = scene.getSelectedIdx();
        
        ImGui::Text("Meshes in scene: %d", (int)meshes.size());
        ImGui::BeginChild("MeshList", ImVec2(0, 150), true);
        for (int i = 0; i < (int)meshes.size(); i++) {
            std::string name = "Mesh " + std::to_string(i + 1) + " (" + std::to_string(meshes[i]->nbVerts) + " verts)";
            if (ImGui::Selectable(name.c_str(), selected == i)) {
                scene.setSelectedIdx(i);
            }
        }
        ImGui::EndChild();

        if (ImGui::Button("Delete Mesh", ImVec2(-1, 0))) {
            if (selected >= 0 && selected < (int)meshes.size()) {
                scene.removeMesh(meshes[selected]);
            }
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
        if (ImGui::Checkbox("Use Pivot", &usePivot)) {
            camera.setUsePivot(usePivot);
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
            camera.setOrbitAngles(0.0f, -3.14159265f * 0.5f);
            camera.setProjectionType(CameraEnums::Projection::ORTHOGRAPHIC);
        }
        ImGui::SameLine();
        if (ImGui::Button("Right", ImVec2(80, 0))) {
            camera.setOrbitAngles(0.0f, 3.14159265f * 0.5f);
            camera.setProjectionType(CameraEnums::Projection::ORTHOGRAPHIC);
        }

        ImGui::Separator();
        if (ImGui::Button("Reset Camera View", ImVec2(-1, 0))) {
            camera.resetView();
        }

        ImGui::End();
    }

    // 7. Rendering Quality
    if (m_showRenderingPanel) {
        ImGui::SetNextWindowPos({450, 350}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({280, 200}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Rendering Quality", &m_showRenderingPanel, ImGuiWindowFlags_AlwaysAutoResize);

        Mesh* selectedMesh = scene.getSelected();
        if (selectedMesh) {
            // Material options
            const char* shaders[] = { "PBR Shader", "Matcap Shading", "Wet Clay Shading", "Normal Shader", "Voxel Checker Shader", "Flat Shading" };
            int type = selectedMesh->shaderType;
            if (ImGui::Combo("Material Shader", &type, shaders, IM_ARRAYSIZE(shaders))) {
                selectedMesh->setShaderType(type);
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
                ImGui::SliderFloat("Roughness", &selectedMesh->roughness, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Metallic", &selectedMesh->metallic, 0.0f, 1.0f, "%.2f");
            } else if (type == 1) { // Matcap Shading
                const auto& matcaps = renderer.getMatcaps();
                if (!matcaps.empty()) {
                    std::vector<const char*> matcapNames;
                    for (const auto& mc : matcaps) {
                        matcapNames.push_back(mc.name.c_str());
                    }
                    int matcapIdx = selectedMesh->getMatcap();
                    if (ImGui::Combo("Matcap Preset", &matcapIdx, matcapNames.data(), static_cast<int>(matcapNames.size()))) {
                        selectedMesh->setMatcap(matcapIdx);
                    }
                }
            }

            bool wire = selectedMesh->showWireframe;
            if (ImGui::Checkbox("Show Wireframe", &wire)) {
                selectedMesh->setShowWireframe(wire);
            }

            bool flat = selectedMesh->flatShading;
            if (ImGui::Checkbox("Flat Shading Mode", &flat)) {
                selectedMesh->setFlatShading(flat);
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

            float alpha = selectedMesh->alpha;
            if (ImGui::SliderFloat("Transparency (Alpha)", &alpha, 0.0f, 1.0f, "%.2f")) {
                selectedMesh->setAlpha(alpha);
            }

            ImGui::ColorEdit3("Albedo Base Color", selectedMesh->albedo);
        } else {
            ImGui::Text("No active mesh selected");
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
        ImGui::SetNextWindowPos(ImVec2((float)wWidth - 140.0f, 40.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(120.0f, 120.0f));
        
        ImGui::Begin("Gizmo Cube", nullptr, 
            ImGuiWindowFlags_NoTitleBar | 
            ImGuiWindowFlags_NoResize | 
            ImGuiWindowFlags_NoScrollbar | 
            ImGuiWindowFlags_NoBackground | 
            ImGuiWindowFlags_NoSavedSettings);

        Camera& camera = scene.getCamera();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        
        // Cube half size in pixels on screen
        float side = 30.0f;
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
        };

        // 26 parts in total: 6 faces, 12 edges, 8 corners
        GizmoPart parts[26] = {
            // --- 6 Faces ---
            { "FRONT",  4, { 0, 1, 2, 3 }, { 0.0f,  0.0f,  1.0f },  0.0f,                 0.0f,                 IM_COL32(50, 120, 230, 220),  IM_COL32(70, 150, 255, 255) },
            { "BACK",   4, { 4, 5, 6, 7 }, { 0.0f,  0.0f, -1.0f },  0.0f,                 3.14159265f,          IM_COL32(40, 90, 180, 220),   IM_COL32(60, 120, 220, 255) },
            { "LEFT",   4, { 8, 9, 10, 11 }, {-1.0f,  0.0f,  0.0f },  0.0f,                -3.14159265f * 0.5f,   IM_COL32(180, 40, 40, 220),   IM_COL32(220, 60, 60, 255) },
            { "RIGHT",  4, { 12, 13, 14, 15 }, { 1.0f,  0.0f,  0.0f },  0.0f,                 3.14159265f * 0.5f,   IM_COL32(230, 50, 50, 220),   IM_COL32(255, 70, 70, 255) },
            { "TOP",    4, { 16, 17, 18, 19 }, { 0.0f,  1.0f,  0.0f }, -3.14159265f * 0.49f,  0.0f,                 IM_COL32(50, 200, 50, 220),   IM_COL32(70, 240, 70, 255) },
            { "BOTTOM", 4, { 20, 21, 22, 23 }, { 0.0f, -1.0f,  0.0f },  3.14159265f * 0.49f,  0.0f,                 IM_COL32(40, 150, 40, 220),   IM_COL32(60, 190, 60, 255) },

            // --- 12 Edges ---
            { "", 4, { 3, 2, 17, 16 }, { 0.0f, 0.707f, 0.707f }, -3.14159265f * 0.25f, 0.0f,                 IM_COL32(50, 160, 140, 220),  IM_COL32(70, 200, 180, 255) },
            { "", 4, { 0, 23, 22, 1 }, { 0.0f, -0.707f, 0.707f },  3.14159265f * 0.25f, 0.0f,                 IM_COL32(45, 135, 135, 220),  IM_COL32(65, 170, 170, 255) },
            { "", 4, { 7, 6, 19, 18 }, { 0.0f, 0.707f, -0.707f }, -3.14159265f * 0.25f, 3.14159265f,          IM_COL32(45, 145, 115, 220),  IM_COL32(65, 180, 145, 255) },
            { "", 4, { 4, 21, 20, 5 }, { 0.0f, -0.707f, -0.707f },  3.14159265f * 0.25f, 3.14159265f,          IM_COL32(40, 120, 110, 220),  IM_COL32(60, 150, 140, 255) },

            { "", 4, { 3, 10, 9, 0 }, {-0.707f, 0.0f, 0.707f }, 0.0f,                 -3.14159265f * 0.25f,  IM_COL32(115, 80, 135, 220),  IM_COL32(145, 100, 170, 255) },
            { "", 4, { 1, 12, 15, 2 }, { 0.707f, 0.0f, 0.707f }, 0.0f,                  3.14159265f * 0.25f,  IM_COL32(140, 85, 140, 220),  IM_COL32(170, 110, 170, 255) },
            { "", 4, { 5, 8, 11, 6 }, {-0.707f, 0.0f, -0.707f }, 0.0f,                 -3.14159265f * 0.75f,  IM_COL32(110, 65, 110, 220),  IM_COL32(140, 85, 140, 255) },
            { "", 4, { 7, 14, 13, 4 }, { 0.707f, 0.0f, -0.707f }, 0.0f,                  3.14159265f * 0.75f,  IM_COL32(135, 70, 115, 220),  IM_COL32(165, 90, 145, 255) },

            { "", 4, { 16, 10, 11, 19 }, {-0.707f, 0.707f, 0.0f }, -3.14159265f * 0.25f, -3.14159265f * 0.5f,   IM_COL32(115, 120, 45, 220),  IM_COL32(145, 150, 65, 255) },
            { "", 4, { 17, 15, 14, 18 }, { 0.707f, 0.707f, 0.0f }, -3.14159265f * 0.25f,  3.14159265f * 0.5f,   IM_COL32(140, 125, 50, 220),  IM_COL32(175, 155, 70, 255) },
            { "", 4, { 23, 9, 8, 20 }, {-0.707f, -0.707f, 0.0f },  3.14159265f * 0.25f, -3.14159265f * 0.5f,   IM_COL32(110, 95, 40, 220),   IM_COL32(140, 120, 60, 255) },
            { "", 4, { 22, 12, 13, 21 }, { 0.707f, -0.707f, 0.0f },  3.14159265f * 0.25f,  3.14159265f * 0.5f,   IM_COL32(135, 100, 45, 220),  IM_COL32(165, 125, 65, 255) },

            // --- 8 Corners ---
            { "", 3, { 2, 15, 17, 0 }, { 0.577f, 0.577f, 0.577f }, -3.14159265f * 0.25f,  3.14159265f * 0.25f,   IM_COL32(110, 120, 110, 220), IM_COL32(140, 150, 140, 255) },
            { "", 3, { 3, 16, 10, 0 }, {-0.577f, 0.577f, 0.577f }, -3.14159265f * 0.25f, -3.14159265f * 0.25f,   IM_COL32(95, 120, 95, 220),   IM_COL32(125, 150, 125, 255) },
            { "", 3, { 7, 18, 14, 0 }, { 0.577f, 0.577f, -0.577f }, -3.14159265f * 0.25f,  3.14159265f * 0.75f,   IM_COL32(105, 110, 100, 220), IM_COL32(135, 140, 130, 255) },
            { "", 3, { 6, 11, 19, 0 }, {-0.577f, 0.577f, -0.577f }, -3.14159265f * 0.25f, -3.14159265f * 0.75f,   IM_COL32(90, 110, 90, 220),   IM_COL32(120, 140, 120, 255) },

            { "", 3, { 1, 22, 12, 0 }, { 0.577f, -0.577f, 0.577f },  3.14159265f * 0.25f,  3.14159265f * 0.25f,   IM_COL32(110, 100, 100, 220), IM_COL32(140, 130, 130, 255) },
            { "", 3, { 0, 9, 23, 0 }, {-0.577f, -0.577f, 0.577f },  3.14159265f * 0.25f, -3.14159265f * 0.25f,   IM_COL32(95, 100, 85, 220),   IM_COL32(125, 130, 115, 255) },
            { "", 3, { 4, 13, 21, 0 }, { 0.577f, -0.577f, -0.577f },  3.14159265f * 0.25f,  3.14159265f * 0.75f,   IM_COL32(105, 95, 90, 220),   IM_COL32(135, 125, 120, 255) },
            { "", 3, { 5, 8, 20, 0 }, {-0.577f, -0.577f, -0.577f },  3.14159265f * 0.25f, -3.14159265f * 0.75f,   IM_COL32(90, 95, 80, 220),    IM_COL32(120, 125, 110, 255) }
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

            // Draw centered text if label is set
            if (part.label[0] != '\0') {
                ImVec2 centerPos(0.0f, 0.0f);
                for (int j = 0; j < part.numVerts; ++j) {
                    centerPos.x += poly[j].x;
                    centerPos.y += poly[j].y;
                }
                centerPos.x /= (float)part.numVerts;
                centerPos.y /= (float)part.numVerts;

                ImVec2 textSize = ImGui::CalcTextSize(part.label);
                ImVec2 textPos = ImVec2(centerPos.x - textSize.x * 0.5f, centerPos.y - textSize.y * 0.5f);

                drawList->AddText(ImVec2(textPos.x + 1.0f, textPos.y + 1.0f), IM_COL32(0, 0, 0, 200), part.label);
                drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), part.label);
            }

            if (isHovered && mouseClicked) {
                camera.setOrbitAngles(part.rotX, part.rotY);
                camera.setProjectionType(CameraEnums::Projection::ORTHOGRAPHIC);
            }
        }

        ImGui::End();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
