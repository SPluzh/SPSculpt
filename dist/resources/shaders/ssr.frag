#version 300 es
precision highp float;

in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uColorTex;
uniform sampler2D uDepthTex;
uniform sampler2D uNormalsTex;

uniform mat4 uProjection;
uniform mat4 uInvProjection;
uniform float uMaxDistance;
uniform float uIntensity;
uniform int uSplitMode;

vec3 fetchViewPos(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 view = uInvProjection * clip;
    return view.xyz / view.w;
}

void main() {
    vec2 uv = vTexCoord;
    if (uSplitMode == 1) {
        if (uv.x >= 0.5) uv.x = (uv.x - 0.5) * 2.0;
        else uv.x = uv.x * 2.0;
    }

    float depth = texture(uDepthTex, uv).r;
    if (depth >= 1.0) {
        fragColor = vec4(0.0);
        return;
    }

    vec3 viewPos = fetchViewPos(uv, depth);
    vec3 viewNorm = normalize(texture(uNormalsTex, uv).rgb * 2.0 - 1.0);
    vec3 rayDir = normalize(reflect(normalize(viewPos), viewNorm));

    if (dot(rayDir, viewNorm) < 0.0) {
        fragColor = vec4(0.0);
        return;
    }

    float stepSize = 0.15;
    int maxSteps = 35;
    vec3 currentPos = viewPos + rayDir * 0.1;
    vec4 hitColor = vec4(0.0);

    for (int i = 0; i < maxSteps; ++i) {
        currentPos += rayDir * stepSize;
        if (length(currentPos - viewPos) > uMaxDistance) break;

        vec4 projPos = uProjection * vec4(currentPos, 1.0);
        vec3 ssPos = projPos.xyz / projPos.w * 0.5 + 0.5;

        if (ssPos.x < 0.0 || ssPos.x > 1.0 || ssPos.y < 0.0 || ssPos.y > 1.0) break;

        vec2 sampleUv = ssPos.xy;
        if (uSplitMode == 1) {
            if (vTexCoord.x >= 0.5) sampleUv.x = sampleUv.x * 0.5 + 0.5;
            else sampleUv.x = sampleUv.x * 0.5;
        }

        float sampleDepth = texture(uDepthTex, sampleUv).r;
        vec3 sampleViewPos = fetchViewPos(sampleUv, sampleDepth);

        float delta = currentPos.z - sampleViewPos.z;
        if (delta > 0.0 && delta < 0.4) {
            vec3 sampledColor = texture(uColorTex, sampleUv).rgb;
            float fade = 1.0 - smoothstep(0.0, uMaxDistance, length(currentPos - viewPos));
            hitColor = vec4(sampledColor, fade * uIntensity);
            break;
        }
    }

    fragColor = hitColor;
}
