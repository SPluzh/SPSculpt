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
    out << "showSymmetryLine=" << (renderer.getShowSymmetryLine() ? "true" : "false") << "\n";
    out << "darkenUnselected=" << (renderer.getDarkenUnselected() ? "true" : "false") << "\n";
    
    glm::vec4 cColor = renderer.getContourColor();
    out << "contourColor=" << cColor.r << " " << cColor.g << " " << cColor.b << " " << cColor.a << "\n";
    out << "cursorThickness=" << renderer.getCursorThickness() << "\n";
    out << "smoothCursor=" << (renderer.getSmoothCursor() ? "true" : "false") << "\n";
    out << "splitMode=" << static_cast<int>(scene.getSplitMode()) << "\n";
    out << "splitShowInactiveCursor=" << (scene.getSplitShowInactiveCursor() ? "true" : "false") << "\n";
    out << "currentEnvIdx=" << renderer.getCurrentEnvIdx() << "\n";
    
    // Global shading display settings
    out << "shaderType=" << renderer.getShaderType() << "\n";
    out << "matcapIdx=" << renderer.getMatcap() << "\n";
    out << "showWireframe=" << (renderer.getShowWireframe() ? "true" : "false") << "\n";
    out << "flatShading=" << (renderer.getFlatShading() ? "true" : "false") << "\n";
    out << "curvature=" << renderer.getCurvature() << "\n";
    out << "bevelEnabled=" << (renderer.getBevelEnabled() ? "true" : "false") << "\n";
    out << "bevelRadius=" << renderer.getBevelRadius() << "\n";
    out << "bevelStrength=" << renderer.getBevelStrength() << "\n";
    out << "bevelScaleWithDistance=" << (renderer.getBevelScaleWithDistance() ? "true" : "false") << "\n";
    out << "useFxaa=" << (renderer.getUseFxaa() ? "true" : "false") << "\n";
    out << "useSsao=" << (renderer.getUseSsao() ? "true" : "false") << "\n";
    out << "ssaoRadius=" << renderer.getSsaoRadius() << "\n";
    out << "ssaoBias=" << renderer.getSsaoBias() << "\n";
    out << "ssaoIntensity=" << renderer.getSsaoIntensity() << "\n";

    const Camera* mainCam = scene.getCameraByIndex(0);
    if (mainCam) {
        out << "speedRotate=" << mainCam->getSpeedRotate() << "\n";
        out << "speedTranslate=" << mainCam->getSpeedTranslate() << "\n";
        out << "speedZoom=" << mainCam->getSpeedZoom() << "\n";
        out << "speedRoll=" << mainCam->getSpeedRoll() << "\n";
    }
    out << "exposure=" << renderer.getExposure() << "\n";
    out << "wetClayWetness=" << renderer.getWetClayWetness() << "\n";
    out << "wetClayBumpStrength=" << renderer.getWetClayBumpStrength() << "\n";
    out << "wetClayNoiseScale=" << renderer.getWetClayNoiseScale() << "\n";
    out << "wetClaySSSIntensity=" << renderer.getWetClaySSSIntensity() << "\n";
    out << "wetClaySSSColor=" << renderer.getWetClaySSSColor().r << " " << renderer.getWetClaySSSColor().g << " " << renderer.getWetClaySSSColor().b << "\n\n";

    out << "useVertexColors=" << (renderer.getUseVertexColors() ? "true" : "false") << "\n";
    out << "useVertexMaterials=" << (renderer.getUseVertexMaterials() ? "true" : "false") << "\n";
    out << "albedo=" << renderer.getAlbedo()[0] << " " << renderer.getAlbedo()[1] << " " << renderer.getAlbedo()[2] << "\n";
    out << "roughness=" << renderer.getRoughness() << "\n";
    out << "metallic=" << renderer.getMetallic() << "\n";
    out << "transmission=" << renderer.getTransmission() << "\n";
    out << "ior=" << renderer.getIor() << "\n";
    out << "sssColor=" << renderer.getSssColor().r << " " << renderer.getSssColor().g << " " << renderer.getSssColor().b << "\n";
    out << "sssIntensity=" << renderer.getSssIntensity() << "\n";
    out << "sssDepth=" << renderer.getSssDepth() << "\n";
    out << "alpha=" << renderer.getAlpha() << "\n\n";

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
        it = params.find("showSymmetryLine");
        if (it != params.end()) {
            renderer.setShowSymmetryLine(it->second == "true" || it->second == "1");
        }
        it = params.find("darkenUnselected");
        if (it != params.end()) {
            renderer.setDarkenUnselected(it->second == "true" || it->second == "1");
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

        it = params.find("speedRotate");
        if (it != params.end()) {
            float val = safe_stof(it->second, 1.0f);
            for (int cIdx = 0; cIdx < 2; ++cIdx) {
                Camera* cam = scene.getCameraByIndex(cIdx);
                if (cam) cam->setSpeedRotate(val);
            }
        }
        it = params.find("speedTranslate");
        if (it != params.end()) {
            float val = safe_stof(it->second, 1.0f);
            for (int cIdx = 0; cIdx < 2; ++cIdx) {
                Camera* cam = scene.getCameraByIndex(cIdx);
                if (cam) cam->setSpeedTranslate(val);
            }
        }
        it = params.find("speedZoom");
        if (it != params.end()) {
            float val = safe_stof(it->second, 1.0f);
            for (int cIdx = 0; cIdx < 2; ++cIdx) {
                Camera* cam = scene.getCameraByIndex(cIdx);
                if (cam) cam->setSpeedZoom(val);
            }
        }
        it = params.find("speedRoll");
        if (it != params.end()) {
            float val = safe_stof(it->second, 1.0f);
            for (int cIdx = 0; cIdx < 2; ++cIdx) {
                Camera* cam = scene.getCameraByIndex(cIdx);
                if (cam) cam->setSpeedRoll(val);
            }
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

        it = params.find("wetClayWetness");
        if (it != params.end()) {
            renderer.setWetClayWetness(safe_stof(it->second, 0.6f));
        }
        it = params.find("wetClayBumpStrength");
        if (it != params.end()) {
            renderer.setWetClayBumpStrength(safe_stof(it->second, 0.4f));
        }
        it = params.find("wetClayNoiseScale");
        if (it != params.end()) {
            renderer.setWetClayNoiseScale(safe_stof(it->second, 8.0f));
        }
        it = params.find("wetClaySSSIntensity");
        if (it != params.end()) {
            renderer.setWetClaySSSIntensity(safe_stof(it->second, 0.25f));
        }
        it = params.find("wetClaySSSColor");
        if (it != params.end()) {
            std::stringstream ss(it->second);
            float r, g, b;
            if (ss >> r >> g >> b) {
                renderer.setWetClaySSSColor(glm::vec3(r, g, b));
            }
        }

        // Global shading display settings
        it = params.find("shaderType");
        if (it != params.end()) {
            renderer.setShaderType(safe_stoi(it->second, 0));
        }
        it = params.find("matcapIdx");
        if (it != params.end()) {
            renderer.setMatcap(safe_stoi(it->second, 0));
        }
        it = params.find("showWireframe");
        if (it != params.end()) {
            renderer.setShowWireframe(it->second == "true" || it->second == "1");
        }
        it = params.find("flatShading");
        if (it != params.end()) {
            renderer.setFlatShading(it->second == "true" || it->second == "1");
        }
        it = params.find("curvature");
        if (it != params.end()) {
            renderer.setCurvature(safe_stof(it->second, 0.0f));
        }
        it = params.find("bevelEnabled");
        if (it != params.end()) {
            renderer.setBevelEnabled(it->second == "true" || it->second == "1");
        }
        it = params.find("bevelRadius");
        if (it != params.end()) {
            renderer.setBevelRadius(safe_stof(it->second, 4.0f));
        }
        it = params.find("bevelStrength");
        if (it != params.end()) {
            renderer.setBevelStrength(safe_stof(it->second, 1.5f));
        }
        it = params.find("bevelScaleWithDistance");
        if (it != params.end()) {
            renderer.setBevelScaleWithDistance(it->second == "true" || it->second == "1");
        }
        it = params.find("useFxaa");
        if (it != params.end()) {
            renderer.setUseFxaa(it->second == "true" || it->second == "1");
        }
        it = params.find("useSsao");
        if (it != params.end()) {
            renderer.setUseSsao(it->second == "true" || it->second == "1");
        }
        it = params.find("ssaoRadius");
        if (it != params.end()) {
            renderer.setSsaoRadius(safe_stof(it->second, 0.5f));
        }
        it = params.find("ssaoBias");
        if (it != params.end()) {
            renderer.setSsaoBias(safe_stof(it->second, 0.025f));
        }
        it = params.find("ssaoIntensity");
        if (it != params.end()) {
            renderer.setSsaoIntensity(safe_stof(it->second, 1.0f));
        }

        it = params.find("useVertexColors");
        if (it != params.end()) {
            renderer.setUseVertexColors(it->second == "true" || it->second == "1");
        }
        it = params.find("useVertexMaterials");
        if (it != params.end()) {
            renderer.setUseVertexMaterials(it->second == "true" || it->second == "1");
        }

        // Global material settings
        it = params.find("albedo");
        if (it != params.end()) {
            std::stringstream ss(it->second);
            float r, g, b;
            if (ss >> r >> g >> b) {
                renderer.setAlbedo(r, g, b);
            }
        }
        it = params.find("roughness");
        if (it != params.end()) {
            renderer.setRoughness(safe_stof(it->second, 0.5f));
        }
        it = params.find("metallic");
        if (it != params.end()) {
            renderer.setMetallic(safe_stof(it->second, 0.0f));
        }
        it = params.find("transmission");
        if (it != params.end()) {
            renderer.setTransmission(safe_stof(it->second, 0.0f));
        }
        it = params.find("ior");
        if (it != params.end()) {
            renderer.setIor(safe_stof(it->second, 1.5f));
        }
        it = params.find("sssColor");
        if (it != params.end()) {
            std::stringstream ss(it->second);
            float r, g, b;
            if (ss >> r >> g >> b) {
                renderer.setSssColor(glm::vec3(r, g, b));
            }
        }
        it = params.find("sssIntensity");
        if (it != params.end()) {
            renderer.setSssIntensity(safe_stof(it->second, 0.0f));
        }
        it = params.find("sssDepth");
        if (it != params.end()) {
            renderer.setSssDepth(safe_stof(it->second, 1.0f));
        }


        it = params.find("alpha");
        if (it != params.end()) {
            renderer.setAlpha(safe_stof(it->second, 1.0f));
        }
    }

    // Fallback: check if settings exist in [Mesh_0] but not [Renderer]
    auto renderer_params = (rent != sections.end()) ? rent->second : std::unordered_map<std::string, std::string>();
    bool has_shaderType = renderer_params.find("shaderType") != renderer_params.end();
    bool has_matcapIdx = renderer_params.find("matcapIdx") != renderer_params.end();
    bool has_showWireframe = renderer_params.find("showWireframe") != renderer_params.end();
    bool has_flatShading = renderer_params.find("flatShading") != renderer_params.end();
    bool has_curvature = renderer_params.find("curvature") != renderer_params.end();
    bool has_albedo = renderer_params.find("albedo") != renderer_params.end();
    bool has_roughness = renderer_params.find("roughness") != renderer_params.end();
    bool has_metallic = renderer_params.find("metallic") != renderer_params.end();
    bool has_alpha = renderer_params.find("alpha") != renderer_params.end();
 
    if (!has_shaderType || !has_matcapIdx || !has_showWireframe || !has_flatShading || !has_curvature ||
        !has_albedo || !has_roughness || !has_metallic || !has_alpha) {
        auto m0 = sections.find("Mesh_0");
        if (m0 != sections.end()) {
            const auto& m0Params = m0->second;
            if (!has_shaderType) {
                auto it = m0Params.find("shaderType");
                if (it != m0Params.end()) renderer.setShaderType(safe_stoi(it->second, 0));
            }
            if (!has_matcapIdx) {
                auto it = m0Params.find("matcapIdx");
                if (it != m0Params.end()) renderer.setMatcap(safe_stoi(it->second, 0));
            }
            if (!has_showWireframe) {
                auto it = m0Params.find("showWireframe");
                if (it != m0Params.end()) renderer.setShowWireframe(it->second == "true" || it->second == "1");
            }
            if (!has_flatShading) {
                auto it = m0Params.find("flatShading");
                if (it != m0Params.end()) renderer.setFlatShading(it->second == "true" || it->second == "1");
            }
            if (!has_curvature) {
                auto it = m0Params.find("curvature");
                if (it != m0Params.end()) renderer.setCurvature(safe_stof(it->second, 0.0f));
            }
            if (!has_albedo) {
                auto it = m0Params.find("albedo");
                if (it != m0Params.end()) {
                    std::stringstream ss(it->second);
                    float r, g, b;
                    if (ss >> r >> g >> b) renderer.setAlbedo(r, g, b);
                }
            }
            if (!has_roughness) {
                auto it = m0Params.find("roughness");
                if (it != m0Params.end()) renderer.setRoughness(safe_stof(it->second, 0.5f));
            }
            if (!has_metallic) {
                auto it = m0Params.find("metallic");
                if (it != m0Params.end()) renderer.setMetallic(safe_stof(it->second, 0.0f));
            }
            if (!has_alpha) {
                auto it = m0Params.find("alpha");
                if (it != m0Params.end()) renderer.setAlpha(safe_stof(it->second, 1.0f));
            }
        }
    }



    std::cout << "Successfully loaded render and shading settings from: " << filepath << std::endl;
    return true;
}
