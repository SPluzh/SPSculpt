#version 300 es
precision highp float;
in vec3 vViewPos;
uniform float uStep;
out vec4 fragColor;
void main() {
    vec2 cell = floor(vViewPos.xy / uStep + 0.0001);
    float cx = mod(abs(cell.x), 2.0);
    float cy = mod(abs(cell.y), 2.0);
    float checker = mod(cx + cy, 2.0);
    vec3 col = (checker > 0.5) ? vec3(0.5) : vec3(0.0);
    float alpha = (checker > 0.5) ? 0.15 : 0.6;
    fragColor = vec4(col, alpha);
}
