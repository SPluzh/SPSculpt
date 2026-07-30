#version 300 es
layout(location = 0) in vec3 aVertex;
layout(location = 3) in vec3 aMaterial;
uniform mat4 uMVP;
uniform mat4 uEM;
void main() {
    vec4 vertex4 = vec4(aVertex, 1.0);
    vec4 pos = uMVP * mix(vertex4, uEM * vertex4, aMaterial.z);
    pos.z -= 0.0001; // offset slightly forward
    gl_Position = pos;
}


