#pragma once
#include "sculpt/CurveSpline.h"
#include <glm/glm.hpp>

enum class TubeProfileType {
    Round = 0,
    FlatRibbon = 1,
    Square = 2,
    Polygon = 3,
    CustomGraph = 4
};

enum class TubeCapType {
    Rounded = 0,
    Flat = 1,
    Open = 2
};

enum class TubeCreationMode {
    FreehandStroke = 0,
    PointByPoint = 1
};

struct TubeData {
    CurveSpline spline;
    TubeCreationMode creationMode = TubeCreationMode::FreehandStroke;
    TubeProfileType profileType = TubeProfileType::Round;
    TubeCapType capType = TubeCapType::Rounded;
    int strokeSmoothing = 2;
    float curveFitTolerance = 1.0f;
    float baseRadius = 2.5f;
    float startRadiusScale = 1.0f;
    float endRadiusScale = 1.0f;
    int curveSegments = 64;
    bool autoDensity = true;
    int cornerSegments = 8;
    int radialSegments = 16;
    float ribbonThickness = 0.25f;
    float twistDegrees = 0.0f;
    bool snapToSurface = true;
    float surfaceOffset = 0.0f;

    // Longitudinal Radius Profile Graph (taper/radius curve along tube length)
    bool useRadiusProfile = false;
    std::vector<ProfilePoint2D> radiusProfile = { {0.0f, 1.0f, false}, {1.0f, 1.0f, false} };
    bool radiusProfileSmooth = true;

    // Cross-Section 2D Profile (2D custom contour)
    std::vector<ProfilePoint2D> crossSectionProfile;
    bool crossSectionSmooth = true;
    bool showProfileAtEnd = true;
};

