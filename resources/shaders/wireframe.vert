#version 300 es
layout(location = 0) in vec3 aVertex;
layout(location = 1) in vec3 aNormal;
layout(location = 3) in vec3 aMaterial;

uniform mat4 uMV;
uniform mat4 uMVP;
uniform mat3 uN;
uniform mat4 uEM;
uniform mat3 uEN;
uniform int uIsOrtho;

out float vFacing;

void main() {
    vec4 vertex4 = vec4(aVertex, 1.0);
    vec3 localPos = mix(vertex4, uEM * vertex4, aMaterial.z).xyz;
    vec3 localNorm = mix(aNormal, uEN * aNormal, aMaterial.z);
    float lenN = length(localNorm);
    vec3 N = lenN > 1e-4 ? (localNorm / lenN) : vec3(0.0);

    vec4 viewPos = uMV * vec4(localPos, 1.0);
    vec3 viewNorm = normalize(uN * N);

    // Compute surface facing towards camera in view space
    vec3 viewDir = (uIsOrtho == 1) ? vec3(0.0, 0.0, 1.0) : normalize(-viewPos.xyz);
    vFacing = dot(viewNorm, viewDir);

    // Normal extrusion in model space: pushes frontfaces forward and backfaces backward.
    // Unlike raw NDC Z-bias, normal extrusion never pushes backfaces in front of frontfaces!
    float dist = length(viewPos.xyz);
    float offset = clamp(dist * 0.0003, 0.00005, 0.02);
    vec3 extrudedPos = localPos + N * offset;

    gl_Position = uMVP * vec4(extrudedPos, 1.0);
}
