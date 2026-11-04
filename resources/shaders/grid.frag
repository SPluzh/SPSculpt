#version 300 es
precision highp float;

in vec4 vColor;
in vec3 vWorldPos;

uniform vec3 uCamPos;
uniform vec3 uPivot;
uniform int uInfiniteGrid;
uniform int uCenterAtOrigin;
uniform float uGridRadius;
uniform float uEdgeFeather;
uniform float uHorizonNear;
uniform float uHorizonFar;

uniform float uSpacing;          // Major line spacing (e.g. 50.0)
uniform float uSubdivisions;     // Subdivisions count (e.g. 5.0)
uniform float uLineWidth;        // Line width in pixels (e.g. 1.2)
uniform float uGridOpacity;      // Overall opacity multiplier

uniform vec4 uMajorColor;        // rgb + alpha
uniform vec4 uMinorColor;        // rgb + alpha
uniform vec3 uXAxisColor;        // Pure red
uniform vec3 uZAxisColor;        // Pure blue
uniform int uShowAxes;           // 1 = on, 0 = off
uniform int uShowSubdivisions;   // 1 = on, 0 = off

out vec4 fragColor;

// Analytical anti-aliased grid line calculation using screen-space derivatives
float computeGridAxis(float coord, float spacing, float widthPx) {
    float c = coord / spacing;
    float lineDist = abs(fract(c - 0.5) - 0.5) / max(fwidth(c), 1e-5);
    return clamp(widthPx * 0.5 - lineDist + 0.5, 0.0, 1.0);
}

void main() {
    vec2 uv = vWorldPos.xz;
    vec2 fwidthUV = max(fwidth(uv), vec2(1e-5));
    
    // 1. Central Axes
    float axisWidthPx = uLineWidth * 1.8;
    // X Axis: z = 0 (distance along Y in UV coords)
    float distAxisX = abs(uv.y) / fwidthUV.y;
    float alphaAxisX = clamp(axisWidthPx * 0.5 - distAxisX + 0.5, 0.0, 1.0);
    
    // Z Axis: x = 0 (distance along X in UV coords)
    float distAxisZ = abs(uv.x) / fwidthUV.x;
    float alphaAxisZ = clamp(axisWidthPx * 0.5 - distAxisZ + 0.5, 0.0, 1.0);
    
    // 2. Major grid lines
    float majorX = computeGridAxis(uv.x, uSpacing, uLineWidth * 1.3);
    float majorZ = computeGridAxis(uv.y, uSpacing, uLineWidth * 1.3);
    float major = max(majorX, majorZ);
    
    // 3. Minor grid lines (Subdivisions)
    float minor = 0.0;
    if (uShowSubdivisions == 1 && uSubdivisions > 1.0) {
        float minorStep = uSpacing / uSubdivisions;
        float minorX = computeGridAxis(uv.x, minorStep, uLineWidth);
        float minorZ = computeGridAxis(uv.y, minorStep, uLineWidth);
        minor = max(minorX, minorZ);
        // Subtract major lines to avoid double-brightness
        minor = max(0.0, minor - major);
    }
    
    // 4. Eliminate parasitic white fringe/line along axes:
    // Completely cut off background grid lines where colored axes pass
    if (uShowAxes == 1) {
        float maxAxis = max(alphaAxisX, alphaAxisZ);
        major *= (1.0 - maxAxis);
        minor *= (1.0 - maxAxis);
    }
    
    // 5. Color compositing in straight linear RGB + analytical Alpha
    vec3 finalCol = vec3(0.0);
    float finalA = 0.0;
    
    // Minor grid layer
    if (minor > 0.001) {
        finalCol = uMinorColor.rgb;
        finalA = minor * uMinorColor.a;
    }
    
    // Major grid layer
    if (major > 0.001) {
        float aMaj = major * uMajorColor.a;
        finalCol = mix(finalCol, uMajorColor.rgb, aMaj / max(finalA + aMaj, 1e-4));
        finalA = max(finalA, aMaj);
    }
    
    // Colored axes layer (Red X axis, Blue Z axis)
    if (uShowAxes == 1) {
        if (alphaAxisX > 0.001) {
            float aX = alphaAxisX * 0.95;
            finalCol = mix(finalCol, uXAxisColor, aX / max(finalA + aX, 1e-4));
            finalA = max(finalA, aX);
        }
        if (alphaAxisZ > 0.001) {
            float aZ = alphaAxisZ * 0.95;
            finalCol = mix(finalCol, uZAxisColor, aZ / max(finalA + aZ, 1e-4));
            finalA = max(finalA, aZ);
        }
    }
    
    // 6. Extent attenuation (Bounded podium vs Infinite Horizon)
    float fadeFactor = 1.0;
    if (uInfiniteGrid == 0) {
        vec2 center = (uCenterAtOrigin == 1) ? vec2(0.0) : uPivot.xz;
        float dist = length(vWorldPos.xz - center);
        float innerRadius = uGridRadius * (1.0 - clamp(uEdgeFeather, 0.0, 0.99));
        fadeFactor = clamp((uGridRadius - dist) / max(uGridRadius - innerRadius, 0.001), 0.0, 1.0);
        fadeFactor = fadeFactor * fadeFactor * (3.0 - 2.0 * fadeFactor);
    } else {
        float distToCam = length(vWorldPos - uCamPos);
        fadeFactor = clamp((uHorizonFar - distToCam) / max(uHorizonFar - uHorizonNear, 1.0), 0.0, 1.0);
        fadeFactor = fadeFactor * fadeFactor * (3.0 - 2.0 * fadeFactor);
    }
    
    finalA *= fadeFactor * uGridOpacity;
    
    if (finalA <= 0.001) {
        discard;
    }
    
    // Premultiplied alpha output for transparent framebuffer
    fragColor = vec4(finalCol * finalA, finalA);
}
