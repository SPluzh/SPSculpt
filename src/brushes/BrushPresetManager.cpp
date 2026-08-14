#include "brushes/BrushPresetManager.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
static std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int count = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.length(), NULL, 0);
    if (count <= 0) return L"";
    std::wstring wstr(count, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.length(), &wstr[0], count);
    return wstr;
}
#endif

BrushPresetManager& BrushPresetManager::instance() {
    static BrushPresetManager inst;
    return inst;
}

void BrushPresetManager::loadDefaults() {
    m_presets.clear();
    // Try multiple search paths for ZBrushes/ directory
    std::string paths[] = { "ZBrushes", "../ZBrushes", "../../ZBrushes" };
    int loaded = 0;
    for (const auto& p : paths) {
        if (std::filesystem::exists(p) && std::filesystem::is_directory(p)) {
            loaded = loadFromFolder(p);
            if (loaded > 0) {
                std::cout << "Loaded " << loaded << " brush presets from " << p << std::endl;
                break;
            }
        }
    }
    if (loaded == 0) {
        std::cerr << "Warning: No brush presets loaded during loadDefaults()" << std::endl;
    }
}

bool BrushPresetManager::loadFromFile(const std::string& path) {
    BrushPreset p = loadBrushPresetFromFile(path);
    if (p.uid.empty()) {
        // Fallback: generate a unique ID if none is found
        p.uid = p.name;
    }
    // Remove if preset with same UID already exists to allow updates
    removePreset(p.uid);
    addPreset(p);
    return true;
}

int BrushPresetManager::loadFromFolder(const std::string& dir) {
    int count = 0;
    try {
        if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
            return 0;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                if (loadFromFile(entry.path().string())) {
                    count++;
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error loading from folder " << dir << ": " << e.what() << std::endl;
    }
    return count;
}

const BrushPreset* BrushPresetManager::findByName(const std::string& name) const {
    for (const auto& p : m_presets) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

const BrushPreset* BrushPresetManager::findByUid(const std::string& uid) const {
    for (const auto& p : m_presets) {
        if (p.uid == uid) return &p;
    }
    return nullptr;
}

BrushPreset* BrushPresetManager::findByNameMut(const std::string& name) {
    for (auto& p : m_presets) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

void BrushPresetManager::addPreset(BrushPreset p) {
    m_presets.push_back(p);
    if (m_activeUid.empty()) {
        m_activeUid = p.uid;
    }
}

void BrushPresetManager::removePreset(const std::string& uid) {
    auto it = std::remove_if(m_presets.begin(), m_presets.end(),
        [&uid](const BrushPreset& p) { return p.uid == uid; });
    if (it != m_presets.end()) {
        m_presets.erase(it, m_presets.end());
    }
}

void BrushPresetManager::setActive(const std::string& uid) {
    m_activeUid = uid;
}

const BrushPreset* BrushPresetManager::active() const {
    return findByUid(m_activeUid);
}

BrushPreset* BrushPresetManager::activeMut() {
    for (auto& p : m_presets) {
        if (p.uid == m_activeUid) return &p;
    }
    return nullptr;
}

bool BrushPresetManager::savePreset(const BrushPreset& p, const std::string& path) const {
    nlohmann::json j;
    j["name"] = p.name;
    j["icon"] = p.icon;
    j["color"] = p.color;
    j["uid"] = p.uid;

    // DeformMode
    switch (p.deformMode) {
        case DeformMode::Normal:   j["type"] = "brush"; break;
        case DeformMode::Crease:   j["type"] = "crease"; break;
        case DeformMode::Flatten:  j["type"] = "flatten"; break;
        case DeformMode::Pinch:    j["type"] = "pinch"; break;
        case DeformMode::Smooth:   j["type"] = "smooth"; break;
        case DeformMode::Move:     j["type"] = "move"; break;
        case DeformMode::Clay:     j["type"] = "clay"; break;
        case DeformMode::Inflate:  j["type"] = "inflate"; break;
    }

    // StrokeMode
    switch (p.strokeMode) {
        case StrokeMode::Dot:               j["stroke"] = "dot"; break;
        case StrokeMode::Roll:              j["stroke"] = "roll"; break;
        case StrokeMode::Grab:              j["stroke"] = "grab"; break;
        case StrokeMode::GrabDynamicRadius: j["stroke"] = "grab_dynamic_radius"; break;
    }

    j["radius_screen"] = p.radius;
    j["intensity"] = p.intensity;
    j["min_spacing"] = p.spacing;
    j["hardness"] = p.hardness;
    j["focal_shift"] = p.focalShift;
    j["focalShiftFalloff"] = p.focalShiftFalloff;
    j["negative"] = p.negative;
    j["culling"] = p.culling;
    j["accumulate"] = p.accumulate;
    j["lockPosition"] = p.lockPosition;
    j["altmode"] = p.altmode;
    j["idAlpha"] = p.idAlpha;

    j["lazy_radius"] = p.lazyRadius;
    j["lazy_smooth"] = p.lazySmooth;

    // Falloff
    j["falloff"]["preset"] = p.falloff.preset;
    j["falloff"]["curve"]["points"] = p.falloff.points;

    // Grab
    j["grab_radius"] = p.grabRadius;
    j["grab_radius_scale"] = p.grabRadiusScale;

    // Area
    j["area_normal_radius"] = p.areaNormalRadius;
    j["area_point_radius"] = p.areaPointRadius;
    j["area_sharp"] = p.areaSharp;
    j["area_sampling"] = p.areaSampling;

    // Flatten
    j["flatten_lock_normal"] = p.flattenLockNormal;
    j["flatten_lock_origin"] = p.flattenLockOrigin;

    // Smooth - Taubin
    j["smooth_taubin"] = p.smoothTaubin;
    j["smooth_taubin_inflate"] = p.smoothTaubinInflate;
    j["smooth_taubin_shrink"] = p.smoothTaubinShrink;
    j["smooth_relax"] = p.smoothRelax;
    j["smooth_stable"] = p.smoothStable;
    j["smooth_sticky_border"] = p.smoothStickyBorder;
    j["tangent"] = p.tangent;

    // Depth Filter
    j["depth_filter_enable"] = p.depthFilter.enable;
    j["depth_filter_falloff"] = p.depthFilter.falloff;
    j["depth_filter_min"] = p.depthFilter.min;
    j["depth_filter_max"] = p.depthFilter.max;
    j["depth_filter_offset"] = p.depthFilter.offset;

    // Topology
    j["connected_topology"] = p.connectedTopology;
    j["only_front_face"] = p.onlyFrontFace;
    j["topoCheck"] = p.topoCheck;
    j["use_dynamic_topology"] = p.useDynamicTopology;
    j["elasticity"] = p.elasticity;

    // Paint
    j["painting_config"]["color"] = { p.paintColor[0], p.paintColor[1], p.paintColor[2] };
    j["painting_config"]["roughness"] = p.roughness;
    j["painting_config"]["metalness"] = p.metallic;
    j["painting_config"]["use_color"] = p.writeAlbedo;
    j["painting_config"]["use_roughness"] = p.writeRoughness;
    j["painting_config"]["use_metalness"] = p.writeMetalness;

    // Pressure
    j["pressure_config"]["use_intensity"] = p.pressureIntensity;
    j["pressure_config"]["use_radius"] = p.pressureRadius;
    j["useGlobalPressure"] = p.useGlobalPressure;

    // DynTopo
    j["subdivFactor"] = p.subdivFactor;
    j["decimFactor"] = p.decimFactor;

#ifdef _WIN32
    std::ofstream out(utf8ToWide(path).c_str());
#else
    std::ofstream out(path);
#endif
    if (!out.is_open()) return false;
    out << j.dump(4);
    return true;
}
