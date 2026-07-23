#version 300 es
precision highp float;
in vec3 vVertex;
in vec3 vNormal;
in vec3 vColor;
in float vMasking;
uniform vec3 uAlbedo;
uniform float uAlpha;
out vec4 fragColor;

#include "common.glsl"

void main() {
    vec3 color = (uAlbedo.r >= 0.0) ? uAlbedo : vColor;
    fragColor = encodeFragColor(sRGBToLinear(color), uAlpha);
}
