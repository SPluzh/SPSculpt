#version 300 es
precision highp float;

in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uCurrentTex;
uniform sampler2D uPrevAccumTex;
uniform sampler2D uDepthTex;
uniform int uFrameCount;

void main() {
    vec3 curr = texture(uCurrentTex, vTexCoord).rgb;
    if (uFrameCount <= 1) {
        fragColor = vec4(curr, 1.0);
        return;
    }
    vec3 prev = texture(uPrevAccumTex, vTexCoord).rgb;
    float alpha = max(0.05, 1.0 / float(uFrameCount));
    fragColor = vec4(mix(prev, curr, alpha), 1.0);
}
