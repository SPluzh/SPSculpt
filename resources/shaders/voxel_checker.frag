#version 300 es
precision highp float;
in vec3 vViewPos;
uniform float uStep;
uniform int uIsPerspective;
uniform float uCenterDepth;
uniform int uIsPreview;
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
    vec2 projPos = vViewPos.xy;
    if (uIsPerspective != 0) {
        projPos = (vViewPos.xy / -vViewPos.z) * uCenterDepth;
    }
    vec2 cell = floor(projPos / uStep + 0.0001);
    float cx = mod(abs(cell.x), 2.0);
    float cy = mod(abs(cell.y), 2.0);
    float checker = mod(cx + cy, 2.0);
    vec3 col = (checker > 0.5) ? vec3(0.5) : vec3(0.0);
    float alpha = (checker > 0.5) ? 0.15 : 0.6;
    
    if (uIsPreview != 0 || uAlpha < 1.0) {
        float finalAlpha = (uIsPreview != 0) ? alpha : alpha * uAlpha;
        fragColor = vec4(col * finalAlpha, finalAlpha);
    } else {
        fragColor = encodeRGBM(col);
    }
}
