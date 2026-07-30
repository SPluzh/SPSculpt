#version 300 es
layout(location = 0) in vec3 aVertex;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 3) in vec3 aMaterial;

layout(location = 5) in uint aFaceGroup;

uniform mat4 uMV;
uniform mat4 uMVP;
uniform mat3 uN;
uniform mat4 uEM;
uniform mat3 uEN;

out vec3 vVertex;
out vec3 vNormal;
out vec3 vColor;
out float vMasking;
out vec3 vVertexPres;
flat out uint vFaceGroup;

void main() {
    vColor = aColor;
    vMasking = aMaterial.z;
    vFaceGroup = aFaceGroup;
    vNormal = mix(aNormal, uEN * aNormal, vMasking);
    vNormal = normalize(uN * vNormal);
    vec4 vertex4 = vec4(aVertex, 1.0);
    vertex4 = mix(vertex4, uEM * vertex4, vMasking);
    vVertex = vec3(uMV * vertex4);
    vVertexPres = vVertex / max(1.0, abs(uMV[3][2]));
    gl_Position = uMVP * vertex4;
}
