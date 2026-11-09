#pragma once

#include <algorithm>
#include <cmath>

namespace dynamic {

struct DyntopoSettings {
    bool enabled = false;
    // false: detail follows brush radius and zoom (relative)
    // true: fixed world-space detail
    bool constantDetail = false;
    float detail = 0.5f;          // 0.0 (coarse) .. 1.0 (fine)
    float smoothRemesh = 1.0f;    // 0 = no remesh, 1 = simplify 2x on Shift
    bool autoWireframe = true;    // wireframe auto-toggle during dyntopo/topology tool
};

// Edge length scale factor for Smooth (Shift): 0 = don't remesh; 1 + smoothRemesh
inline float dyntopoSmoothScale(const DyntopoSettings& s) {
    return s.smoothRemesh <= 0.005f ? 0.0f : 1.0f + std::min(s.smoothRemesh, 1.0f);
}

// Calculate target edge length for standard sculpting brushes
inline float dyntopoEdgeLength(const DyntopoSettings& s, float radius, float diagonal) {
    const float d = std::clamp(s.detail, 0.0f, 1.0f);
    if (s.constantDetail) {
        return std::max(diagonal, 1e-6f) * 0.05f * std::pow(0.04f, d);
    }
    return std::max(radius, 1e-7f) * 0.4f * std::pow(0.1f, d);
}

} // namespace dynamic

