#version 300 es
precision highp float;
uniform float uAlpha;
out vec4 fragColor;

vec4 encodeRGBM(const in vec3 col) {
    vec4 rgbm;
    vec3 color = col / 5.0;
    rgbm.a = clamp(max(max(color.r, color.g), max(color.b, 1e-6)), 0.0, 1.0);
    rgbm.a = ceil(rgbm.a * 255.0) / 255.0;
    rgbm.rgb = color / rgbm.a;
    return rgbm;
}

void main() {
    vec3 col = vec3(0.0);
    fragColor = (uAlpha != 1.0) ? vec4(col * uAlpha, uAlpha) : encodeRGBM(col);
}
