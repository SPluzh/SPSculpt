#version 300 es
precision highp float;
in vec3 vVertex;
in vec3 vNormal;
in float vMasking;
out vec4 fragColor;

#include "common.glsl"

void main() {
    // getNormal() returns the view-space normal, possibly taking bevel or flat-shading into account
    vec3 normal = normalize(getNormal());
    // Map normal from [-1,1] to [0,1]
    fragColor = vec4(normal * 0.5 + 0.5, 1.0);
}
