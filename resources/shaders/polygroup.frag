#version 300 es
precision highp float;

in vec3 vVertex;
in vec3 vNormal;
in vec3 vColor;
in float vMasking;
flat in uint vFaceGroup;

uniform float uAlpha;

out vec4 fragColor;

#include "common.glsl"

vec3 groupColor(uint gid) {
    if (gid == 0u) return vec3(0.72, 0.52, 0.45);
    float goldenRatio = 0.618033988749895;
    float h = fract(float(gid) * goldenRatio);
    float s = 0.85;
    float v = 0.95;
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(vec3(h) + K.xyz) * 6.0 - K.www);
    return v * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), s);
}

void main() {
    vec3 normal = getNormal();
    vec3 col = groupColor(vFaceGroup);
    vec3 linCol = sRGBToLinear(col);
    
    vec3 lightDir1 = normalize(vec3(0.5, 0.8, 1.0));
    vec3 lightDir2 = normalize(vec3(-0.5, -0.2, -0.5));
    float diff1 = max(dot(normal, lightDir1), 0.0);
    float diff2 = max(dot(normal, lightDir2), 0.0) * 0.35;
    float ambient = 0.35;
    
    vec3 lightColor = vec3(1.0) * (diff1 + diff2 + ambient);
    vec3 finalColor = linCol * lightColor;
    
    vec3 viewDir = normalize(-vVertex);
    vec3 halfDir = normalize(lightDir1 + viewDir);
    float spec = pow(max(dot(normal, halfDir), 0.0), 32.0) * 0.15;
    finalColor += vec3(spec);

    fragColor = encodeFragColor(finalColor, uAlpha);
}
