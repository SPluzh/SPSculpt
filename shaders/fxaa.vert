#version 300 es
layout(location = 0) in vec2 aVertex;
uniform vec2 uInvSize;
out vec2 vUVNW;
out vec2 vUVNE;
out vec2 vUVSW;
out vec2 vUVSE;
out vec2 vUVM;
void main() {
    vUVM = aVertex * 0.5 + 0.5;
    vUVNW = vUVM + vec2(-1.0, -1.0) * uInvSize;
    vUVNE = vUVM + vec2(1.0, -1.0) * uInvSize;
    vUVSW = vUVM + vec2(-1.0, 1.0) * uInvSize;
    vUVSE = vUVM + vec2(1.0, 1.0) * uInvSize;
    gl_Position = vec4(aVertex, 0.5, 1.0);
}
