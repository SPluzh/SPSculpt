#version 300 es
precision highp float;
in vec3 vVertex;
in vec3 vNormal;
in vec3 vColor;
in float vMasking;
in vec3 vObjectPos;
flat in uint vFaceGroup;
uniform float uAlpha;
uniform bool uShowPolyGroups;
out vec4 fragColor;

#include "common.glsl"
#include "wet_clay.glsl"

vec3 groupColor(uint gid) {
    if (gid == 0u) return vec3(0.72, 0.52, 0.45);
    uint n = (gid * 1664525u + 1013904223u);
    float r = float(n & 0xFFu) / 255.0;
    float g = float((n >> 8u) & 0xFFu) / 255.0;
    float b = float((n >> 16u) & 0xFFu) / 255.0;
    return mix(vec3(r, g, b), vec3(1.0), 0.15);
}

void main() {
    vec3 col = vColor;
    if (uShowPolyGroups) {
        col = groupColor(vFaceGroup);
    }
    vec3 color = computeWetClay(vVertex, getAlignedNormal(), col, vMasking, vObjectPos);
    fragColor = encodeFragColor(color, uAlpha);
}
