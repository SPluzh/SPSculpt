#version 300 es
precision highp float;

uniform vec4 uWireColor;
uniform int uCullBackfaces;
in float vFacing;

out vec4 fragColor;

void main() {
    // Cull lines on the back of the object so they never bleed through front faces when zooming out
    if (uCullBackfaces == 1 && vFacing < -0.15) {
        discard;
    }
    fragColor = uWireColor;
}
