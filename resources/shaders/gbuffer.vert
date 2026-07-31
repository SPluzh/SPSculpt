#version 300 es
precision highp float;

layout(location = 0) in vec3 aVertex;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 3) in vec3 aMaterial;

uniform mat4 uMV;
uniform mat4 uMVP;
uniform mat4 uPrevMVP;
uniform mat3 uN;
uniform mat4 uEM;
uniform mat3 uEN;

out vec3 vVertex;
out vec3 vNormal;
out vec3 vColor;
out vec3 vMaterial;
out float vMasking;
out vec4 vCurrPos;
out vec4 vPrevPos;

void main() {
    vec4 v = vec4(aVertex, 1.0);
    vec3 n = aNormal;

    v = mix(v, uEM * v, aMaterial.z);
    n = mix(n, uEN * n, aMaterial.z);

    vVertex = vec3(uMV * v);
    vNormal = uN * n;
    vColor = aColor;
    vMaterial = aMaterial;
    vMasking = aMaterial.z;

    vCurrPos = uMVP * v;
    vPrevPos = uPrevMVP * v;
    gl_Position = vCurrPos;
}
