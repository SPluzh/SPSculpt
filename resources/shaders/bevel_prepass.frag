#version 300 es
precision highp float;

in vec3 vVertex;
in vec3 vNormal;

uniform int uFlat;
uniform int uOrtho;

out vec4 fragColor;

void main() {
    float lenN = length(vNormal);
    bool hasValidN = (lenN > 1e-4 && !isnan(lenN));
    vec3 N = hasValidN ? (vNormal / lenN) : vec3(0.0, 0.0, 1.0);

    vec3 dX = dFdx(vVertex);
    vec3 dY = dFdy(vVertex);
    vec3 crossN = cross(dY, dX);
    float lenCross = length(crossN);
    vec3 flatNormal = lenCross > 1e-10 ? -(crossN / lenCross) : N;

    vec3 eye = (uOrtho == 1) ? vec3(0.0, 0.0, 1.0) : -vVertex;
    if (dot(flatNormal, eye) < 0.0) {
        flatNormal = -flatNormal;
    }
    if (dot(N, flatNormal) < 0.0) {
        N = -N;
    }

    if (uFlat == 1) {
        N = flatNormal;
    } else {
        float d = dot(N, flatNormal);
        if (d < 0.90) {
            float t = clamp((d - 0.80) / (0.90 - 0.80), 0.0, 1.0);
            N = normalize(mix(flatNormal, N, t * t));
        }
    }
    
    // Store in [0, 1] range
    fragColor = vec4(N * 0.5 + 0.5, 1.0);
}
