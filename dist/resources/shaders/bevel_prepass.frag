#version 300 es
precision highp float;

in vec3 vVertex;
in vec3 vNormal;

uniform int uFlat;

out vec4 fragColor;

void main() {
    vec3 N = normalize(vNormal);
    if (uFlat == 1) {
        N = normalize(cross(dFdy(vVertex), dFdx(vVertex)));
    } else {
        // Enforce outward-facing normal orientation similar to getAlignedNormal
        vec3 flatNormal = normalize(cross(dFdy(vVertex), dFdx(vVertex)));
        if (dot(N, flatNormal) < 0.0) {
            N = -N;
        }
    }
    
    // Store in [0, 1] range
    fragColor = vec4(N * 0.5 + 0.5, 1.0);
}
