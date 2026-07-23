#version 300 es
precision highp float;
uniform vec3 uColor;
uniform float uAlpha;
uniform bool uDashed;
out vec4 fragColor;
void main() {
    if (uDashed) {
        float val = gl_FragCoord.x + gl_FragCoord.y;
        if (mod(val, 8.0) > 4.0) {
            discard;
        }
    }
    fragColor = vec4(uColor, uAlpha);
}
