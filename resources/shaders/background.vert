#version 300 es
layout(location = 0) in vec2 aVertex;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    vTexCoord = aTexCoord;
    gl_Position = vec4(aVertex, 1.0, 1.0);
}
