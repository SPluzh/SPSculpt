#version 300 es
precision highp float;
in vec2 vTexCoord;
uniform sampler2D uTexture0;
uniform vec2 uInvSize;
uniform vec3 uColor;
uniform float uAlpha;
out vec4 fragColor;

float outlineDistance(const in vec2 uv, const in sampler2D tex, const in vec2 invSize) {
  float fac0 = 2.0;
  float fac1 = 1.0;
  float ox = invSize.x;
  float oy = invSize.y;
  vec4 texel0 = texture(tex, uv + vec2(ox, oy));
  vec4 texel1 = texture(tex, uv + vec2(ox, 0.0));
  vec4 texel2 = texture(tex, uv + vec2(ox, -oy));
  vec4 texel3 = texture(tex, uv + vec2(0.0, -oy));
  vec4 texel4 = texture(tex, uv + vec2(-ox, -oy));
  vec4 texel5 = texture(tex, uv + vec2(-ox, 0.0));
  vec4 texel6 = texture(tex, uv + vec2(-ox, oy));
  vec4 texel7 = texture(tex, uv + vec2(0.0, oy));
  vec4 rowx = -fac0 * texel5 + fac0 * texel1 + -fac1 * texel6 + fac1 * texel0 + -fac1 * texel4 + fac1 * texel2;
  vec4 rowy = -fac0 * texel3 + fac0 * texel7 + -fac1 * texel4 + fac1 * texel6 + -fac1 * texel2 + fac1 * texel0;
  return dot(rowy, rowy) + dot(rowx, rowx);
}

void main() {
    float val = outlineDistance(vTexCoord, uTexture0, uInvSize);
    if (val < 1.5)
        discard;
    fragColor = vec4(uColor, uAlpha);
}
