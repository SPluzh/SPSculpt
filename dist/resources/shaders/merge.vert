#version 300 es
layout(location = 0) in vec2 aVertex;
out vec2 vTexCoord;
void main() {
    vTexCoord = aVertex * 0.5 + 0.5;
    gl_Position = vec4(aVertex, 0.5, 1.0);
}
