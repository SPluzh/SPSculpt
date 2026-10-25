#version 300 es
precision highp float;

uniform vec4 uWireColor;
uniform int uCullBackfaces;
uniform int uDashed;
in float vFacing;

out vec4 fragColor;

void main() {
    // Cull lines on the back of the object so they never bleed through front faces when zooming out
    if (uCullBackfaces == 1 && vFacing < -0.15) {
        discard;
    }
    if (uDashed == 1) {
        float val = gl_FragCoord.x + gl_FragCoord.y;
        if (mod(val, 12.0) > 6.0) {
            discard;
        }
    }
    fragColor = uWireColor;
}
