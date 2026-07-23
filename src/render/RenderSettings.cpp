#include "render/RenderSettings.h"
#include "render/AngleRenderer.h"
#include "scene/Scene.h"
#include "mesh/Mesh.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <unordered_map>

static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

static int safe_stoi(const std::string& str, int defaultVal = 0) {
    try {
        return std::stoi(str);
    } catch (...) {
        return defaultVal;
    }
}

static float safe_stof(const std::string& str, float defaultVal = 0.0f) {
    try {
        return std::stof(str);
    } catch (...) {
        return defaultVal;
    }
}

bool RenderSettings::save(const std::string& filepath, const AngleRenderer& renderer, const Scene& scene) {
    std::ofstream out(filepath);
    if (!out.is_open()) {
        std::cerr << "Failed to open settings file for writing: " << filepath << std::endl;
        return false;
    }

    out << "[Renderer]\n";
    out << "showBackground=" << (renderer.getShowBackground() ? "true" : "false") << "\n";
    out << "backgroundType=" << renderer.getBackgroundType() << "\n";
    out << "bgBlur=" << renderer.getBgBlur() << "\n";
    out << "bgFill=" << (renderer.getBgFill() ? "true" : "false") << "\n";
    out << "bgTexturePath=" << renderer.getBgTexturePath() << "\n";
    out << "filmic=" << (renderer.getFilmic() ? "true" : "false") << "\n";
    out << "showContour=" << (renderer.getShowContour() ? "true" : "false") << "\n";
    out << "showGrid=" << (renderer.getShowGrid() ? "true" : "false") << "\n";
    
    glm::vec4 cColor = renderer.getContourColor();
    out << "contourColor=" << cColor.r << " " << cColor.g << " " << cColor.b << " " << cColor.a << "\n";
    out << "cursorThickness=" << renderer.getCursorThickness() << "\n";
    out << "smoothCursor=" << (renderer.getSmoothCursor() ? "true" : "false") << "\n";
    out << "splitMode=" << static_cast<int>(scene.getSplitMode()) << "\n";
    out << "splitShowInactiveCursor=" << (scene.getSplitShowInactiveCursor() ? "true" : "false") << "\n";
    out << "currentEnvIdx=" << renderer.getCurrentEnvIdx() << "\n";
    out << "exposure=" << renderer.getExposure() << "\n\n";

    const auto& meshes = scene.getMeshes();
    for (size_t i = 0; i < meshes.size(); ++i) {
        const Mesh* m = meshes[i];
        out << "[Mesh_" << i << "]\n";
        out << "shaderType=" << m->shaderType << "\n";
        out << "matcapIdx=" << m->matcapIdx << "\n";
        out << "albedo=" << m->albedo[0] << " " << m->albedo[1] << " " << m->albedo[2] << "\n";
        out << "roughness=" << m->roughness << "\n";
        out << "metallic=" << m->metallic << "\n";
        out << "alpha=" << m->alpha << "\n";
        out << "showWireframe=" << (m->showWireframe ? "true" : "false") << "\n";
        out << "flatShading=" << (m->flatShading ? "true" : "false") << "\n";
        out << "curvature=" << m->curvature << "\n\n";
    }

    std::cout << "Successfully saved render and shading settings to: " << filepath << std::endl;
    return true;
}

bool RenderSettings::load(const std::string& filepath, AngleRenderer& renderer, Scene& scene) {
    std::ifstream in(filepath);
    if (!in.is_open()) {
        std::cerr << "Failed to open settings file for reading: " << filepath << std::endl;
        return false;
    }

    std::string line;
    std::string currentSection = "";
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> sections;

    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        if (line[0] == '[' && line[line.size() - 1] == ']') {
            currentSection = trim(line.substr(1, line.size() - 2));
            continue;
        }

        size_t eqPos = line.find('=');
        if (eqPos != std::string::npos && !currentSection.empty()) {
            std::string key = trim(line.substr(0, eqPos));
            std::string val = trim(line.substr(eqPos + 1));
            sections[currentSection][key] = val;
        }
    }

    // Process [Renderer] section
    auto rent = sections.find("Renderer");
    if (rent != sections.end()) {
        const auto& params = rent->second;

        auto it = params.find("showBackground");
        if (it != params.end()) {
            renderer.setShowBackground(it->second == "true" || it->second == "1");
        }
        it = params.find("backgroundType");
        if (it != params.end()) {
            renderer.setBackgroundType(safe_stoi(it->second));
        }
         it = params.find("bgBlur");
        if (it != params.end()) {
            renderer.setBgBlur(safe_stof(it->second));
        }
        it = params.find("bgFill");
        if (it != params.end()) {
            renderer.setBgFill(it->second == "true" || it->second == "1");
        }
        it = params.find("bgTexturePath");
        if (it != params.end()) {
            renderer.loadBackgroundTexture(it->second);
        }
        it = params.find("filmic");
        if (it != params.end()) {
            renderer.setFilmic(it->second == "true" || it->second == "1");
        }
        it = params.find("showContour");
        if (it != params.end()) {
            renderer.setShowContour(it->second == "true" || it->second == "1");
        }
        it = params.find("showGrid");
        if (it != params.end()) {
            renderer.setShowGrid(it->second == "true" || it->second == "1");
        }
        it = params.find("contourColor");
        if (it != params.end()) {
            std::stringstream ss(it->second);
            float r, g, b, a;
            if (ss >> r >> g >> b >> a) {
                renderer.setContourColor(glm::vec4(r, g, b, a));
            }
        }
        it = params.find("cursorThickness");
        if (it != params.end()) {
            renderer.setCursorThickness(safe_stof(it->second, 2.5f));
        }
        it = params.find("smoothCursor");
        if (it != params.end()) {
            renderer.setSmoothCursor(it->second == "true" || it->second == "1");
        }
        it = params.find("splitMode");
        if (it != params.end()) {
            scene.setSplitMode(static_cast<Scene::SplitMode>(safe_stoi(it->second, 0)));
        }
        it = params.find("splitShowInactiveCursor");
        if (it != params.end()) {
            scene.setSplitShowInactiveCursor(it->second == "true" || it->second == "1");
        }
        
        // Load environment preset first (so it resets exposure and SH)
        int envIdx = -1;
        it = params.find("currentEnvIdx");
        if (it != params.end()) {
            envIdx = safe_stoi(it->second);
            renderer.setEnvironmentPreset(envIdx);
        }

        // Apply custom exposure afterward, in case they tweaked it
        it = params.find("exposure");
        if (it != params.end()) {
            renderer.setExposure(safe_stof(it->second));
        }
    }

    // Process [Mesh_X] sections
    const auto& meshes = scene.getMeshes();
    for (size_t i = 0; i < meshes.size(); ++i) {
        std::string sectionName = "Mesh_" + std::to_string(i);
        auto meshIt = sections.find(sectionName);
        if (meshIt != sections.end()) {
            Mesh* m = meshes[i];
            const auto& params = meshIt->second;

            auto it = params.find("shaderType");
            if (it != params.end()) {
                m->setShaderType(safe_stoi(it->second));
            }
            it = params.find("matcapIdx");
            if (it != params.end()) {
                m->setMatcap(safe_stoi(it->second));
            }
            it = params.find("albedo");
            if (it != params.end()) {
                std::stringstream ss(it->second);
                float r, g, b;
                if (ss >> r >> g >> b) {
                    m->setAlbedo(r, g, b);
                }
            }
            it = params.find("roughness");
            if (it != params.end()) {
                m->setRoughness(safe_stof(it->second));
            }
            it = params.find("metallic");
            if (it != params.end()) {
                m->setMetallic(safe_stof(it->second));
            }
            it = params.find("alpha");
            if (it != params.end()) {
                m->setAlpha(safe_stof(it->second));
            }
            it = params.find("showWireframe");
            if (it != params.end()) {
                m->setShowWireframe(it->second == "true" || it->second == "1");
            }
            it = params.find("flatShading");
            if (it != params.end()) {
                m->setFlatShading(it->second == "true" || it->second == "1");
            }
            it = params.find("curvature");
            if (it != params.end()) {
                m->setCurvature(safe_stof(it->second));
            }
        }
    }

    std::cout << "Successfully loaded render and shading settings from: " << filepath << std::endl;
    return true;
}
