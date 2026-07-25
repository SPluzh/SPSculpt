#version 300 es
precision highp float;

in vec2 vTexCoord;

uniform sampler2D uOpaque;
uniform sampler2D uTransparent;
uniform int uFilmic;

uniform int uSsaoEnabled;
uniform sampler2D uSsaoTexture;
uniform float uSsaoIntensity;

uniform int uSsrEnabled;
uniform sampler2D uSsrTexture;
uniform float uSsrIntensity;

out vec4 fragColor;

#include "colorspace.glsl"

void main() {
    vec4 transp = texture(uTransparent, vTexCoord);
    vec4 opaqueSample = texture(uOpaque, vTexCoord);
    vec3 opaqueColor = decodeRGBM(opaqueSample);

    if (uSsaoEnabled == 1) {
        float ao = texture(uSsaoTexture, vTexCoord).r;
        opaqueColor *= mix(1.0, ao, uSsaoIntensity);
    }

    if (uSsrEnabled == 1) {
        vec4 ssrSample = texture(uSsrTexture, vTexCoord);
        opaqueColor = mix(opaqueColor, ssrSample.rgb, ssrSample.a * uSsrIntensity);
    }

    vec3 color = opaqueColor * (1.0 - transp.a) + transp.rgb;

    if (uFilmic == 1) {
        vec3 x = max(vec3(0.0), color - 0.004);
        fragColor = vec4((x * (6.2 * x + 0.5)) / (x * (6.2 * x + 1.7) + 0.06), 1.0);
    } else {
        fragColor = vec4(linearTosRGB(color), 1.0);
    }
}
