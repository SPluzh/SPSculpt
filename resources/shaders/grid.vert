#version 300 es
layout(location = 0) in vec3 aVertex;
layout(location = 1) in vec4 aColor;

uniform mat4 uMVP;

out vec4 vColor;
out vec3 vWorldPos;

void main() {
    vColor = aColor;
    vWorldPos = aVertex;
    gl_Position = uMVP * vec4(aVertex, 1.0);
}
