#version 300 es
precision highp float;

in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uInputTex;
uniform sampler2D uNormalTex;
uniform sampler2D uDepthTex;
uniform vec2 uTexelSize;
uniform int uStepSize;

void main() {
    vec2 uv = vTexCoord;
    vec3 centerColor = texture(uInputTex, uv).rgb;
    vec3 centerNorm = texture(uNormalTex, uv).rgb * 2.0 - 1.0;
    float centerDepth = texture(uDepthTex, uv).r;

    if (centerDepth >= 1.0) {
        fragColor = vec4(centerColor, 1.0);
        return;
    }

    float kernel[5] = float[](0.0625, 0.25, 0.375, 0.25, 0.0625);
    vec3 sumColor = vec3(0.0);
    float sumWeight = 0.0;

    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            vec2 offset = vec2(float(x), float(y)) * uTexelSize * float(uStepSize);
            vec2 sampleUv = uv + offset;

            vec3 c = texture(uInputTex, sampleUv).rgb;
            vec3 n = texture(uNormalTex, sampleUv).rgb * 2.0 - 1.0;
            float d = texture(uDepthTex, sampleUv).r;

            float wN = pow(max(0.0, dot(centerNorm, n)), 16.0);
            float wD = exp(-abs(centerDepth - d) * 100.0);
            float w = kernel[x + 2] * kernel[y + 2] * wN * wD;

            sumColor += c * w;
            sumWeight += w;
        }
    }

    fragColor = vec4(sumWeight > 0.0 ? sumColor / sumWeight : centerColor, 1.0);
}
