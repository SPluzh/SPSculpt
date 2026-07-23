#version 300 es
precision highp float;
in vec3 vVertex;
in vec3 vNormal;
in float vMasking;
uniform float uAlpha;
out vec4 fragColor;

#include "common.glsl"

void main() {
    vec3 normal = getNormal();
    vec3 col = sRGBToLinear(normal * 0.5 + 0.5);
    fragColor = encodeFragColor(col, uAlpha);
}
