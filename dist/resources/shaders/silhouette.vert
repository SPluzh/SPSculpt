#version 300 es
layout(location = 0) in vec3 aVertex;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 3) in vec3 aMaterial;
uniform mat4 uMVP;
uniform mat4 uEM;
void main() {
    float vMasking = aMaterial.z;
    vec4 vertex4 = mix(vec4(aVertex, 1.0), uEM * vec4(aVertex, 1.0), vMasking);
    gl_Position = uMVP * vertex4;
}
