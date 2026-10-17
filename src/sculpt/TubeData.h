#pragma once
#include "sculpt/CurveSpline.h"
#include <glm/glm.hpp>

enum class TubeProfileType {
    Round = 0,
    FlatRibbon = 1,
    Square = 2,
    Polygon = 3
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
};
