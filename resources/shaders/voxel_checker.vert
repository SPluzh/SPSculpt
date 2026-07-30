#version 300 es
layout(location = 0) in vec3 aVertex;
uniform mat4 uMVP;
uniform mat4 uMV;
out vec3 vViewPos;
void main() {
    vViewPos = vec3(uMV * vec4(aVertex, 1.0));
    gl_Position = uMVP * vec4(aVertex, 1.0);
}
