#version 300 es
precision highp float;
in vec3 vVertex;
in vec3 vNormal;
in vec3 vColor;
in float vMasking;
in vec3 vObjectPos;
uniform float uAlpha;
out vec4 fragColor;

#include "common.glsl"
#include "wet_clay.glsl"

void main() {
    vec3 color = computeWetClay(vVertex, getAlignedNormal(), vColor, vMasking, vObjectPos);
    fragColor = encodeFragColor(color, uAlpha);
}
