#pragma once
#include <string>
#include <vector>
#include <array>
#include <nlohmann/json.hpp>

enum class StrokeMode { Dot, Roll, Grab, GrabDynamicRadius };
enum class DeformMode { Normal, Clay, Inflate, Pinch, Crease, Flatten, Smooth, Move };

struct FalloffCurve {
    std::string preset = "smoothstep"; // "smoothstep","linear","out_power_2"
    std::vector<std::array<float,2>> points;
};

struct DepthFilter {
    bool   enable  = false;
    bool   falloff = true;
    float  min     = 1.0f;
    float  max     = 1.0f;
    float  offset  = 0.0f;
};

struct BrushPreset {
    // Meta
    std::string name;
    std::string icon;
    std::string color;       // "#rrggbb"
    std::string uid;

    // Topology
    DeformMode  deformMode  = DeformMode::Normal;
    StrokeMode  strokeMode  = StrokeMode::Dot;

    // Base params
    float radius        = 35.0f;
    float intensity     = 1.0f;
    float spacing       = 0.08f;
    float hardness      = 0.75f;
    float focalShift    = 0.0f;
    bool  focalShiftFalloff = true;
    bool  negative      = false;
    bool  culling       = false;
    bool  accumulate    = false;
    bool  lockPosition  = false;
    bool  altmode       = false;
    int   idAlpha       = -1;

    // Lazy
    float lazyRadius    = 0.0f;
    float lazySmooth    = 0.0f;

    // Falloff
    FalloffCurve falloff;

    // Grab (Move / RoundEdge)
    bool  grabRadius      = false;
    float grabRadiusScale = 0.28f;

    // Area
    float areaNormalRadius = 0.4f;
    float areaPointRadius  = 0.0f;
    float areaSharp        = 0.0f;
    bool  areaSampling     = true;

    // Flatten
    bool  flattenLockNormal = false;
    bool  flattenLockOrigin = false;

    // Smooth — Taubin
    bool  smoothTaubin        = false;
    float smoothTaubinInflate = 0.53f;
    float smoothTaubinShrink  = 0.75f;
    bool  smoothRelax         = false;
    bool  smoothStable        = false;
    bool  smoothStickyBorder  = false;
    bool  tangent             = false;

    // Depth filter
    DepthFilter depthFilter;

    // Topology
    bool connectedTopology = false;
    bool onlyFrontFace     = false;
    bool topoCheck         = false;
    bool useDynamicTopology = false;
    float elasticity       = 1.5f;

    // Paint
    std::array<float,3> paintColor{1.0f, 0.766f, 0.336f};
    float roughness  = 0.3f;
    float metallic   = 0.0f;
    bool  writeAlbedo    = true;
    bool  writeRoughness = true;
    bool  writeMetalness = false;

    // Pressure
    bool  pressureIntensity   = true;
    bool  pressureRadius      = false;
    bool  useGlobalPressure   = false;

    // DynTopo
    float subdivFactor = 0.0f;
    float decimFactor  = 0.0f;
};

BrushPreset normalizeBrushJSON(const nlohmann::json& raw, const std::string& name);
BrushPreset loadBrushPresetFromFile(const std::string& path);
