#version 300 es
precision highp float;
in vec2 vUVNW;
in vec2 vUVNE;
in vec2 vUVSW;
in vec2 vUVSE;
in vec2 vUVM;
uniform sampler2D uTexture0;
uniform vec2 uInvSize;
uniform bool uEnabled;
out vec4 fragColor;

#include "fxaa.glsl"

void main() {
    if (uEnabled) {
        fragColor = vec4(fxaa(uTexture0, vUVNW, vUVNE, vUVSW, vUVSE, vUVM, uInvSize), 1.0);
    } else {
        fragColor = texture(uTexture0, vUVM);
    }
}
