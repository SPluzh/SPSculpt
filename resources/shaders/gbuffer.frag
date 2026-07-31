#version 300 es
precision highp float;

layout(location = 0) out vec4 gAlbedo;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gMaterial;
layout(location = 3) out vec2 gMotionVec;

in vec3 vVertex;
in vec3 vNormal;
in vec3 vColor;
in vec3 vMaterial;
in float vMasking;
in vec4 vCurrPos;
in vec4 vPrevPos;

uniform vec3 uAlbedo;
uniform float uRoughness;
uniform float uMetallic;

#include "common.glsl"

void main() {
    vec3 normal = getNormal();
    vec3 rawColor = (uAlbedo.r >= 0.0 ? uAlbedo : vColor);
    vec3 linColor = sRGBToLinear(rawColor);
    float roughness = max(0.001, (uRoughness >= 0.0 ? uRoughness : vMaterial.x));
    float metallic = (uMetallic >= 0.0 ? uMetallic : vMaterial.y);

    gAlbedo = vec4(linColor, metallic);
    gNormal = vec4(normalize(normal) * 0.5 + 0.5, 1.0);
    gMaterial = vec4(roughness, vMasking, length(vVertex), 1.0);

    vec2 currScreen = (vCurrPos.xy / vCurrPos.w) * 0.5 + 0.5;
    vec2 prevScreen = (vPrevPos.xy / vPrevPos.w) * 0.5 + 0.5;
    gMotionVec = currScreen - prevScreen;
}
