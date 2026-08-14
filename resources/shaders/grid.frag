#version 300 es
precision highp float;

in vec4 vColor;
in vec3 vWorldPos;

uniform vec3 uPivot;
uniform float uFogNear;
uniform float uFogFar;

out vec4 fragColor;

// Calculates analytical anti-aliased grid line intensity using screen-space derivatives (F3D style)
float computeGrid(vec2 uv, float spacing, float lineWidthPixels) {
    vec2 coord = uv / spacing;
    vec2 derivative = fwidth(coord);
    vec2 grid = abs(fract(coord - 0.5) - 0.5);
    vec2 lineDist = grid / max(derivative, vec2(1e-5));
    vec2 alpha2d = clamp(vec2(lineWidthPixels * 0.5) - lineDist + 0.5, 0.0, 1.0);
    return max(alpha2d.x, alpha2d.y);
}

void main() {
    vec2 uv = vWorldPos.xz;
    
    // 1. Minor grid lines (step 10.0, 1.2px thickness)
    float minor = computeGrid(uv, 10.0, 1.2);
    
    // 2. Major grid lines (step 50.0, 1.8px thickness)
    float major = computeGrid(uv, 50.0, 1.8);
    
    // 3. Main Axes (X axis: z near 0, Z axis: x near 0, 2.5px thickness)
    vec2 coordAxes = uv;
    vec2 derivAxes = fwidth(coordAxes);
    vec2 distAxes = abs(coordAxes) / max(derivAxes, vec2(1e-5));
    vec2 axisAlpha = clamp(vec2(1.25) - distAxes + 0.5, 0.0, 1.0); // 2.5px thick axes
    
    float xAxis = axisAlpha.y; // z = 0 (X axis)
    float zAxis = axisAlpha.x; // x = 0 (Z axis)
    
    // Mask out minor and major lines on top of the main axes to prevent double-line / white fringe artifacts
    float axisMask = max(xAxis, zAxis);
    minor *= (1.0 - axisMask);
    major *= (1.0 - axisMask);
    
    // Smooth grey grid palette (F3D style - soft grey lines)
    vec3 minorCol = vec3(0.12, 0.13, 0.14);       // Deep soft grey for minor lines
    vec3 majorCol = vec3(0.24, 0.25, 0.27);       // Subtle medium grey for major lines
    vec3 xCol     = vec3(1.00, 0.05, 0.05);       // Pure Red X Axis
    vec3 zCol     = vec3(0.00, 0.45, 1.00);       // Pure Blue Z Axis
    
    vec3 finalColor = vec3(0.0);
    float finalAlpha = 0.0;

    if (minor > 0.001) {
        finalColor = minorCol;
        finalAlpha = minor * 0.35;
    }
    
    if (major > 0.001) {
        finalColor = mix(finalColor, majorCol, major);
        finalAlpha = max(finalAlpha, major * 0.55);
    }
    
    if (xAxis > 0.001) {
        finalColor = mix(finalColor, xCol, xAxis);
        finalAlpha = max(finalAlpha, xAxis * 0.98);
    }
    
    if (zAxis > 0.001) {
        finalColor = mix(finalColor, zCol, zAxis);
        finalAlpha = max(finalAlpha, zAxis * 0.98);
    }
    
    // Distance fog attenuation around camera pivot
    float dist = length(vWorldPos.xz - uPivot.xz);
    float fogFactor = clamp((uFogFar - dist) / max(uFogFar - uFogNear, 0.001), 0.0, 1.0);
    fogFactor = fogFactor * fogFactor * (3.0 - 2.0 * fogFactor);
    
    finalAlpha *= fogFactor;
    if (finalAlpha <= 0.001) {
        discard;
    }
    
    fragColor = vec4(finalColor, finalAlpha);
}
