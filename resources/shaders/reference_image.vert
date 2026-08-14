#version 300 es
layout(location = 0) in vec2 aVertex;
out vec2 vTexCoord;
uniform mat4 uMVP;
uniform bool uPinned2D;
uniform vec2 uOffset;
uniform float uScale;
uniform vec2 uAspectScale;
void main() {
    vTexCoord = aVertex * 0.5 + 0.5;
    if (uPinned2D) {
        vec2 pos = aVertex * uAspectScale * uScale;
        pos += uOffset;
        gl_Position = vec4(pos, 0.0, 1.0);
    } else {
        gl_Position = uMVP * vec4(aVertex, 0.0, 1.0);
    }
}
