#include "brushes/BrushPreset.h"
#include <fstream>
#include <iostream>
#include <unordered_map>

BrushPreset normalizeBrushJSON(const nlohmann::json& raw, const std::string& name) {
    BrushPreset p;
    p.name = name;
    p.icon = raw.value("icon", "");
    p.color = raw.value("color", "");
    p.uid = raw.value("uid", "");

    // DeformMode
    std::string typeStr = raw.value("type", "brush");
    static const std::unordered_map<std::string, DeformMode> typeMap = {
        {"brush",      DeformMode::Normal},
        {"crease",     DeformMode::Crease},
        {"flatten",    DeformMode::Flatten},
        {"pinch",      DeformMode::Pinch},
        {"smooth",     DeformMode::Smooth},
        {"move",       DeformMode::Move},
        {"clay",       DeformMode::Clay},
        {"inflate",    DeformMode::Inflate}
    };
    auto typeIt = typeMap.find(typeStr);
    if (typeIt != typeMap.end()) {
        p.deformMode = typeIt->second;
    } else {
        p.deformMode = DeformMode::Normal;
    }

    // StrokeMode
    std::string strokeStr = raw.value("stroke", "dot");
    if (strokeStr == "roll") {
        p.strokeMode = StrokeMode::Roll;
    } else if (strokeStr == "grab") {
        p.strokeMode = StrokeMode::Grab;
    } else if (strokeStr == "grab_dynamic_radius") {
        p.strokeMode = StrokeMode::GrabDynamicRadius;
    } else {
        p.strokeMode = StrokeMode::Dot;
    }

    // Base params
    p.radius = raw.value("radius_screen", p.radius);
    p.radius = raw.value("radius", p.radius); // fallback
    p.intensity = raw.value("intensity", p.intensity);
    p.spacing = raw.value("min_spacing", p.spacing);
    p.spacing = raw.value("spacing", p.spacing); // fallback
    p.hardness = raw.value("hardness", p.hardness);
    p.focalShift = raw.value("focalShift", p.focalShift);
    p.focalShift = raw.value("focal_shift", p.focalShift); // fallback
    p.focalShiftFalloff = raw.value("focalShiftFalloff", p.focalShiftFalloff);
    p.negative = raw.value("negative", p.negative);
    p.culling = raw.value("culling", p.culling);
    p.accumulate = raw.value("accumulate", p.accumulate);
    p.lockPosition = raw.value("lockPosition", p.lockPosition);
    p.altmode = raw.value("altmode", p.altmode);
    p.idAlpha = raw.value("idAlpha", p.idAlpha);

    // Lazy
    p.lazyRadius = raw.value("lazy_radius", p.lazyRadius);
    p.lazyRadius = raw.value("lazyRadius", p.lazyRadius); // fallback
    p.lazySmooth = raw.value("lazy_smooth", p.lazySmooth);
    p.lazySmooth = raw.value("lazySmooth", p.lazySmooth); // fallback

    // Falloff
    if (raw.contains("falloff")) {
        auto f = raw["falloff"];
        p.falloff.preset = f.value("preset", p.falloff.preset);
        if (f.contains("curve") && f["curve"].contains("points")) {
            p.falloff.points.clear();
            for (auto& pt : f["curve"]["points"]) {
                if (pt.is_array() && pt.size() >= 2) {
                    p.falloff.points.push_back({pt[0].get<float>(), pt[1].get<float>()});
                }
            }
        }
    }

    // Grab
    p.grabRadius = raw.value("grab_radius", p.grabRadius);
    p.grabRadiusScale = raw.value("grab_radius_scale", p.grabRadiusScale);

    // Area
    p.areaNormalRadius = raw.value("area_normal_radius", p.areaNormalRadius);
    p.areaPointRadius = raw.value("area_point_radius", p.areaPointRadius);
    p.areaSharp = raw.value("area_sharp", p.areaSharp);
    p.areaSampling = raw.value("area_sampling", p.areaSampling);

    // Flatten
    p.flattenLockNormal = raw.value("flatten_lock_normal", p.flattenLockNormal);
    p.flattenLockOrigin = raw.value("flatten_lock_origin", p.flattenLockOrigin);

    // Smooth - Taubin
    p.smoothTaubin = raw.value("smooth_taubin", p.smoothTaubin);
    p.smoothTaubinInflate = raw.value("smooth_taubin_inflate", p.smoothTaubinInflate);
    p.smoothTaubinShrink = raw.value("smooth_taubin_shrink", p.smoothTaubinShrink);
    p.smoothRelax = raw.value("smooth_relax", p.smoothRelax);
    p.smoothStable = raw.value("smooth_stable", p.smoothStable);
    p.smoothStickyBorder = raw.value("smooth_sticky_border", p.smoothStickyBorder);
    p.tangent = raw.value("tangent", p.tangent);

    // Depth Filter
    bool dfEnable = false;
    if (raw.contains("depth_filter_enable")) {
        dfEnable = raw["depth_filter_enable"].get<bool>();
    } else if (raw.contains("depth_filter")) {
        dfEnable = raw["depth_filter"].get<bool>();
    }
    p.depthFilter.enable = dfEnable;
    if (raw.contains("depth_filter_falloff")) {
        p.depthFilter.falloff = raw["depth_filter_falloff"].get<bool>();
    }
    p.depthFilter.min = raw.value("depth_filter_min", p.depthFilter.min);
    p.depthFilter.max = raw.value("depth_filter_max", p.depthFilter.max);
    p.depthFilter.offset = raw.value("depth_filter_offset", p.depthFilter.offset);

    // Topology
    p.connectedTopology = raw.value("connected_topology", p.connectedTopology);
    p.onlyFrontFace = raw.value("only_front_face", p.onlyFrontFace);
    p.topoCheck = raw.value("topoCheck", p.topoCheck);
    p.useDynamicTopology = raw.value("use_dynamic_topology", p.useDynamicTopology);
    p.elasticity = raw.value("elasticity", p.elasticity);

    // Paint
    if (raw.contains("painting_config")) {
        auto pc = raw["painting_config"];
        if (pc.contains("color") && pc["color"].is_array() && pc["color"].size() >= 3) {
            p.paintColor[0] = pc["color"][0].get<float>();
            p.paintColor[1] = pc["color"][1].get<float>();
            p.paintColor[2] = pc["color"][2].get<float>();
        }
        p.roughness = pc.value("roughness", p.roughness);
        p.metallic = pc.value("metalness", p.metallic);
        p.metallic = pc.value("metallic", p.metallic); // fallback
        p.writeAlbedo = pc.value("use_color", p.writeAlbedo);
        p.writeRoughness = pc.value("use_roughness", p.writeRoughness);
        p.writeMetalness = pc.value("use_metalness", p.writeMetalness);
    }

    // Pressure
    if (raw.contains("pressure_config")) {
        auto prc = raw["pressure_config"];
        p.pressureIntensity = prc.value("use_intensity", p.pressureIntensity);
        p.pressureRadius = prc.value("use_radius", p.pressureRadius);
    }
    p.useGlobalPressure = raw.value("useGlobalPressure", p.useGlobalPressure);

    // DynTopo
    p.subdivFactor = raw.value("subdivFactor", p.subdivFactor);
    p.decimFactor = raw.value("decimFactor", p.decimFactor);

    return p;
}

BrushPreset loadBrushPresetFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "Failed to open brush preset file: " << path << std::endl;
        return BrushPreset();
    }
    nlohmann::json raw;
    try {
        f >> raw;
    } catch (const std::exception& e) {
        std::cerr << "JSON parse error in " << path << ": " << e.what() << std::endl;
        return BrushPreset();
    }
    // Extract filename as default preset name
    size_t lastSlash = path.find_last_of("/\\");
    std::string filename = (lastSlash == std::string::npos) ? path : path.substr(lastSlash + 1);
    size_t lastDot = filename.find_last_of('.');
    std::string name = (lastDot == std::string::npos) ? filename : filename.substr(0, lastDot);

    return normalizeBrushJSON(raw, name);
}
