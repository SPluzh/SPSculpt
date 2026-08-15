#pragma once
#include <cstdint>

enum BrushType {
    BRUSH_FLATTEN = 0,
    BRUSH_SMOOTH,
    BRUSH_INFLATE,
    BRUSH_PINCH,
    BRUSH_CREASE,
    BRUSH_VTOOL,
    BRUSH_MOVE,
    BRUSH_DRAG,
    BRUSH_ELASTIC,
    // Group B
    BRUSH_MASK,
    BRUSH_PAINT,
    BRUSH_TWIST,
    BRUSH_LOCALSCALE,
    // Group C
    BRUSH_CLAY,
    BRUSH_CLAYBUILDUP,
    BRUSH_DAMSTANDARD,
    BRUSH_SQUAREBRUSH,
    BRUSH_VISIBILITY,
    BRUSH_MASK_GRADIENT_BLUR,
    BRUSH_MEASURE,
    BRUSH_DIVIDER,
    BRUSH_TRANSFORM,
    BRUSH_ARMATURE_SPHERES,
    BRUSH_BRUSH,
    BRUSH_POLYGROUP,
    BRUSH_CLIP_CURVE,
    BRUSH_TRIM,
    BRUSH_DELETE_LAYER,
    BRUSH_TOPOLOGY,
    BRUSH_COUNT
};

enum class ModalMode {
    NONE = 0,
    RADIUS,
    INTENSITY,
    FOCAL_SHIFT,
    REMESH_RESOLUTION,
    TOPOLOGY_DETAIL,
    CAMERA_FOV
};

enum class SymmetryMode {
    Local = 0,
    World = 1
};

inline const char* getBrushNameStr(BrushType brush) {
    switch (brush) {
        case BRUSH_FLATTEN: return "Flatten";
        case BRUSH_SMOOTH: return "Smooth";
        case BRUSH_INFLATE: return "Inflate";
        case BRUSH_PINCH: return "Pinch";
        case BRUSH_CREASE: return "Crease";
        case BRUSH_VTOOL: return "V-Tool";
        case BRUSH_MOVE: return "Move";
        case BRUSH_DRAG: return "Drag";
        case BRUSH_ELASTIC: return "Elastic";
        case BRUSH_MASK: return "Mask";
        case BRUSH_PAINT: return "Paint";
        case BRUSH_TWIST: return "Twist";
        case BRUSH_LOCALSCALE: return "Local Scale";
        case BRUSH_CLAY: return "Clay";
        case BRUSH_CLAYBUILDUP: return "ClayBuildup";
        case BRUSH_DAMSTANDARD: return "Dam Standard";
        case BRUSH_SQUAREBRUSH: return "Square Brush";
        case BRUSH_VISIBILITY: return "Visibility";
        case BRUSH_MASK_GRADIENT_BLUR: return "Mask Gradient/Blur";
        case BRUSH_MEASURE: return "Measure";
        case BRUSH_DIVIDER: return "Divider";
        case BRUSH_TRANSFORM: return "Transform";
        case BRUSH_ARMATURE_SPHERES: return "Armature Spheres";
        case BRUSH_BRUSH: return "Standard Brush";
        case BRUSH_POLYGROUP: return "PolyGroup";
        case BRUSH_CLIP_CURVE: return "Clip Curve";
        case BRUSH_TRIM: return "Trim";
        case BRUSH_DELETE_LAYER: return "Delete Layer";
        case BRUSH_TOPOLOGY: return "Topology";
        default: return "Unknown";
    }
}


