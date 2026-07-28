#include "render/RenderSettings.h"
#include "render/AngleRenderer.h"
#include "scene/Scene.h"
#include "mesh/Mesh.h"
#include "common/IniFile.h"
#include <sstream>
#include <iostream>

bool RenderSettings::save(IniFile& ini, const AngleRenderer& renderer, const Scene& scene) {
    const std::string sec = "Renderer";

    ini.setBool(sec, "showBackground", renderer.getShowBackground());
    ini.setInt(sec, "backgroundType", renderer.getBackgroundType());
    ini.setFloat(sec, "bgBlur", renderer.getBgBlur());
    ini.setBool(sec, "bgFill", renderer.getBgFill());
    ini.set(sec, "bgTexturePath", renderer.getBgTexturePath());
    ini.setBool(sec, "filmic", renderer.getFilmic());
    ini.setBool(sec, "showContour", renderer.getShowContour());
    ini.setBool(sec, "showGrid", renderer.getShowGrid());
    ini.setBool(sec, "showSymmetryLine", renderer.getShowSymmetryLine());
    ini.setBool(sec, "darkenUnselected", renderer.getDarkenUnselected());
    
    glm::vec4 cColor = renderer.getContourColor();
    ini.set(sec, "contourColor", std::to_string(cColor.r) + " " + std::to_string(cColor.g) + " " + std::to_string(cColor.b) + " " + std::to_string(cColor.a));
    ini.setFloat(sec, "cursorThickness", renderer.getCursorThickness());
    ini.setBool(sec, "smoothCursor", renderer.getSmoothCursor());
    ini.setInt(sec, "splitMode", static_cast<int>(scene.getSplitMode()));
    ini.setBool(sec, "splitShowInactiveCursor", scene.getSplitShowInactiveCursor());
    ini.setInt(sec, "currentEnvIdx", renderer.getCurrentEnvIdx());
    
    // Global shading display settings
    ini.setInt(sec, "shaderType", renderer.getShaderType());
    ini.setInt(sec, "matcapIdx", renderer.getMatcap());
    ini.setBool(sec, "showWireframe", renderer.getShowWireframe());
    ini.setBool(sec, "flatShading", renderer.getFlatShading());
    ini.setFloat(sec, "curvature", renderer.getCurvature());
    ini.setBool(sec, "bevelEnabled", renderer.getBevelEnabled());
    ini.setFloat(sec, "bevelRadius", renderer.getBevelRadius());
    ini.setFloat(sec, "bevelStrength", renderer.getBevelStrength());
    ini.setBool(sec, "bevelScaleWithDistance", renderer.getBevelScaleWithDistance());
    ini.setBool(sec, "useFxaa", renderer.getUseFxaa());
    ini.setBool(sec, "useSsao", renderer.getUseSsao());
    ini.setFloat(sec, "ssaoRadius", renderer.getSsaoRadius());
    ini.setFloat(sec, "ssaoBias", renderer.getSsaoBias());
    ini.setFloat(sec, "ssaoIntensity", renderer.getSsaoIntensity());

    const Camera* mainCam = scene.getCameraByIndex(0);
    if (mainCam) {
        ini.setInt(sec, "cameraMode", static_cast<int>(mainCam->getMode()));
        ini.setInt(sec, "cameraProjection", static_cast<int>(mainCam->getProjectionType()));
        ini.setFloat(sec, "cameraFov", mainCam->getFov());
        ini.setBool(sec, "cameraUsePivot", mainCam->getUsePivot());
        ini.setFloat(sec, "speedRotate", mainCam->getSpeedRotate());
        ini.setFloat(sec, "speedTranslate", mainCam->getSpeedTranslate());
        ini.setFloat(sec, "speedZoom", mainCam->getSpeedZoom());
        ini.setFloat(sec, "speedRoll", mainCam->getSpeedRoll());
    }
    ini.setFloat(sec, "exposure", renderer.getExposure());
    ini.setFloat(sec, "wetClayWetness", renderer.getWetClayWetness());
    ini.setFloat(sec, "wetClayBumpStrength", renderer.getWetClayBumpStrength());
    ini.setFloat(sec, "wetClayNoiseScale", renderer.getWetClayNoiseScale());
    ini.setFloat(sec, "wetClaySSSIntensity", renderer.getWetClaySSSIntensity());

    glm::vec3 wcColor = renderer.getWetClaySSSColor();
    ini.set(sec, "wetClaySSSColor", std::to_string(wcColor.r) + " " + std::to_string(wcColor.g) + " " + std::to_string(wcColor.b));

    ini.setBool(sec, "useVertexColors", renderer.getUseVertexColors());
    ini.setBool(sec, "useVertexMaterials", renderer.getUseVertexMaterials());
    
    auto albedo = renderer.getAlbedo();
    ini.set(sec, "albedo", std::to_string(albedo[0]) + " " + std::to_string(albedo[1]) + " " + std::to_string(albedo[2]));
    ini.setFloat(sec, "roughness", renderer.getRoughness());
    ini.setFloat(sec, "metallic", renderer.getMetallic());
    ini.setFloat(sec, "transmission", renderer.getTransmission());
    ini.setFloat(sec, "ior", renderer.getIor());

    glm::vec3 sssCol = renderer.getSssColor();
    ini.set(sec, "sssColor", std::to_string(sssCol.r) + " " + std::to_string(sssCol.g) + " " + std::to_string(sssCol.b));
    ini.setFloat(sec, "sssIntensity", renderer.getSssIntensity());
    ini.setFloat(sec, "sssDepth", renderer.getSssDepth());
    ini.setFloat(sec, "alpha", renderer.getAlpha());

    return true;
}

bool RenderSettings::load(const IniFile& ini, AngleRenderer& renderer, Scene& scene) {
    const std::string sec = "Renderer";
    if (!ini.hasSection(sec)) {
        return false;
    }

    if (ini.hasKey(sec, "showBackground")) renderer.setShowBackground(ini.getBool(sec, "showBackground"));
    if (ini.hasKey(sec, "backgroundType")) renderer.setBackgroundType(ini.getInt(sec, "backgroundType"));
    if (ini.hasKey(sec, "bgBlur")) renderer.setBgBlur(ini.getFloat(sec, "bgBlur"));
    if (ini.hasKey(sec, "bgFill")) renderer.setBgFill(ini.getBool(sec, "bgFill"));
    if (ini.hasKey(sec, "bgTexturePath")) renderer.loadBackgroundTexture(ini.get(sec, "bgTexturePath"));
    if (ini.hasKey(sec, "filmic")) renderer.setFilmic(ini.getBool(sec, "filmic"));
    if (ini.hasKey(sec, "showContour")) renderer.setShowContour(ini.getBool(sec, "showContour"));
    if (ini.hasKey(sec, "showGrid")) renderer.setShowGrid(ini.getBool(sec, "showGrid"));
    if (ini.hasKey(sec, "showSymmetryLine")) renderer.setShowSymmetryLine(ini.getBool(sec, "showSymmetryLine"));
    if (ini.hasKey(sec, "darkenUnselected")) renderer.setDarkenUnselected(ini.getBool(sec, "darkenUnselected"));
    
    if (ini.hasKey(sec, "contourColor")) {
        std::stringstream ss(ini.get(sec, "contourColor"));
        float r, g, b, a;
        if (ss >> r >> g >> b >> a) {
            renderer.setContourColor(glm::vec4(r, g, b, a));
        }
    }

    if (ini.hasKey(sec, "cursorThickness")) renderer.setCursorThickness(ini.getFloat(sec, "cursorThickness", 2.5f));
    if (ini.hasKey(sec, "smoothCursor")) renderer.setSmoothCursor(ini.getBool(sec, "smoothCursor"));
    if (ini.hasKey(sec, "splitMode")) scene.setSplitMode(static_cast<Scene::SplitMode>(ini.getInt(sec, "splitMode", 0)));
    if (ini.hasKey(sec, "splitShowInactiveCursor")) scene.setSplitShowInactiveCursor(ini.getBool(sec, "splitShowInactiveCursor"));

    if (ini.hasKey(sec, "cameraMode")) {
        auto val = static_cast<CameraEnums::CameraMode>(ini.getInt(sec, "cameraMode", 0));
        for (int cIdx = 0; cIdx < 2; ++cIdx) {
            Camera* cam = scene.getCameraByIndex(cIdx);
            if (cam) cam->setMode(val);
        }
    }
    if (ini.hasKey(sec, "cameraProjection")) {
        auto val = static_cast<CameraEnums::Projection>(ini.getInt(sec, "cameraProjection", 0));
        for (int cIdx = 0; cIdx < 2; ++cIdx) {
            Camera* cam = scene.getCameraByIndex(cIdx);
            if (cam) cam->setProjectionType(val);
        }
    }
    if (ini.hasKey(sec, "cameraFov")) {
        float val = ini.getFloat(sec, "cameraFov", 45.0f);
        for (int cIdx = 0; cIdx < 2; ++cIdx) {
            Camera* cam = scene.getCameraByIndex(cIdx);
            if (cam) cam->setFov(val);
        }
    }
    if (ini.hasKey(sec, "cameraUsePivot")) {
        bool val = ini.getBool(sec, "cameraUsePivot");
        for (int cIdx = 0; cIdx < 2; ++cIdx) {
            Camera* cam = scene.getCameraByIndex(cIdx);
            if (cam) cam->setUsePivot(val);
        }
    }

    if (ini.hasKey(sec, "speedRotate")) {
        float val = ini.getFloat(sec, "speedRotate", 1.0f);
        for (int cIdx = 0; cIdx < 2; ++cIdx) {
            Camera* cam = scene.getCameraByIndex(cIdx);
            if (cam) cam->setSpeedRotate(val);
        }
    }
    if (ini.hasKey(sec, "speedTranslate")) {
        float val = ini.getFloat(sec, "speedTranslate", 1.0f);
        for (int cIdx = 0; cIdx < 2; ++cIdx) {
            Camera* cam = scene.getCameraByIndex(cIdx);
            if (cam) cam->setSpeedTranslate(val);
        }
    }
    if (ini.hasKey(sec, "speedZoom")) {
        float val = ini.getFloat(sec, "speedZoom", 1.0f);
        for (int cIdx = 0; cIdx < 2; ++cIdx) {
            Camera* cam = scene.getCameraByIndex(cIdx);
            if (cam) cam->setSpeedZoom(val);
        }
    }
    if (ini.hasKey(sec, "speedRoll")) {
        float val = ini.getFloat(sec, "speedRoll", 1.0f);
        for (int cIdx = 0; cIdx < 2; ++cIdx) {
            Camera* cam = scene.getCameraByIndex(cIdx);
            if (cam) cam->setSpeedRoll(val);
        }
    }
    
    if (ini.hasKey(sec, "currentEnvIdx")) {
        renderer.setEnvironmentPreset(ini.getInt(sec, "currentEnvIdx"));
    }

    if (ini.hasKey(sec, "exposure")) renderer.setExposure(ini.getFloat(sec, "exposure"));
    if (ini.hasKey(sec, "wetClayWetness")) renderer.setWetClayWetness(ini.getFloat(sec, "wetClayWetness", 0.6f));
    if (ini.hasKey(sec, "wetClayBumpStrength")) renderer.setWetClayBumpStrength(ini.getFloat(sec, "wetClayBumpStrength", 0.4f));
    if (ini.hasKey(sec, "wetClayNoiseScale")) renderer.setWetClayNoiseScale(ini.getFloat(sec, "wetClayNoiseScale", 8.0f));
    if (ini.hasKey(sec, "wetClaySSSIntensity")) renderer.setWetClaySSSIntensity(ini.getFloat(sec, "wetClaySSSIntensity", 0.25f));
    
    if (ini.hasKey(sec, "wetClaySSSColor")) {
        std::stringstream ss(ini.get(sec, "wetClaySSSColor"));
        float r, g, b;
        if (ss >> r >> g >> b) {
            renderer.setWetClaySSSColor(glm::vec3(r, g, b));
        }
    }

    if (ini.hasKey(sec, "shaderType")) renderer.setShaderType(ini.getInt(sec, "shaderType", 0));
    if (ini.hasKey(sec, "matcapIdx")) renderer.setMatcap(ini.getInt(sec, "matcapIdx", 0));
    if (ini.hasKey(sec, "showWireframe")) renderer.setShowWireframe(ini.getBool(sec, "showWireframe"));
    if (ini.hasKey(sec, "flatShading")) renderer.setFlatShading(ini.getBool(sec, "flatShading"));
    if (ini.hasKey(sec, "curvature")) renderer.setCurvature(ini.getFloat(sec, "curvature", 0.0f));
    if (ini.hasKey(sec, "bevelEnabled")) renderer.setBevelEnabled(ini.getBool(sec, "bevelEnabled"));
    if (ini.hasKey(sec, "bevelRadius")) renderer.setBevelRadius(ini.getFloat(sec, "bevelRadius", 4.0f));
    if (ini.hasKey(sec, "bevelStrength")) renderer.setBevelStrength(ini.getFloat(sec, "bevelStrength", 1.5f));
    if (ini.hasKey(sec, "bevelScaleWithDistance")) renderer.setBevelScaleWithDistance(ini.getBool(sec, "bevelScaleWithDistance"));
    if (ini.hasKey(sec, "useFxaa")) renderer.setUseFxaa(ini.getBool(sec, "useFxaa"));
    if (ini.hasKey(sec, "useSsao")) renderer.setUseSsao(ini.getBool(sec, "useSsao"));
    if (ini.hasKey(sec, "ssaoRadius")) renderer.setSsaoRadius(ini.getFloat(sec, "ssaoRadius", 0.5f));
    if (ini.hasKey(sec, "ssaoBias")) renderer.setSsaoBias(ini.getFloat(sec, "ssaoBias", 0.025f));
    if (ini.hasKey(sec, "ssaoIntensity")) renderer.setSsaoIntensity(ini.getFloat(sec, "ssaoIntensity", 1.0f));

    if (ini.hasKey(sec, "useVertexColors")) renderer.setUseVertexColors(ini.getBool(sec, "useVertexColors"));
    if (ini.hasKey(sec, "useVertexMaterials")) renderer.setUseVertexMaterials(ini.getBool(sec, "useVertexMaterials"));

    if (ini.hasKey(sec, "albedo")) {
        std::stringstream ss(ini.get(sec, "albedo"));
        float r, g, b;
        if (ss >> r >> g >> b) {
            renderer.setAlbedo(r, g, b);
        }
    }
    if (ini.hasKey(sec, "roughness")) renderer.setRoughness(ini.getFloat(sec, "roughness", 0.5f));
    if (ini.hasKey(sec, "metallic")) renderer.setMetallic(ini.getFloat(sec, "metallic", 0.0f));
    if (ini.hasKey(sec, "transmission")) renderer.setTransmission(ini.getFloat(sec, "transmission", 0.0f));
    if (ini.hasKey(sec, "ior")) renderer.setIor(ini.getFloat(sec, "ior", 1.5f));
    
    if (ini.hasKey(sec, "sssColor")) {
        std::stringstream ss(ini.get(sec, "sssColor"));
        float r, g, b;
        if (ss >> r >> g >> b) {
            renderer.setSssColor(glm::vec3(r, g, b));
        }
    }
    if (ini.hasKey(sec, "sssIntensity")) renderer.setSssIntensity(ini.getFloat(sec, "sssIntensity", 0.0f));
    if (ini.hasKey(sec, "sssDepth")) renderer.setSssDepth(ini.getFloat(sec, "sssDepth", 1.0f));
    if (ini.hasKey(sec, "alpha")) renderer.setAlpha(ini.getFloat(sec, "alpha", 1.0f));

    return true;
}
