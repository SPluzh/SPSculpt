#version 300 es
precision highp float;

in vec2 vTexCoord;

uniform sampler2D uOpaque;
uniform sampler2D uTransparent;
uniform int uFilmic;

out vec4 fragColor;

#include "colorspace.glsl"

void main() {
    vec4 transp = texture(uTransparent, vTexCoord);
    vec4 opaqueSample = texture(uOpaque, vTexCoord);
    vec3 color = decodeRGBM(opaqueSample) * (1.0 - transp.a) + transp.rgb;
    
    if (uFilmic == 1) {
        vec3 x = max(vec3(0.0), color - 0.004);
        fragColor = vec4((x * (6.2 * x + 0.5)) / (x * (6.2 * x + 1.7) + 0.06), 1.0);
    } else {
        fragColor = vec4(linearTosRGB(color), 1.0);
    }
}
