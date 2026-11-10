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
uniform vec2 uUvScale;
uniform vec2 uUvMax;

vec3 fetchViewPos(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 view = uInvProjection * clip;
    return view.xyz / view.w;
}

void main() {
    vec2 localUv = vTexCoord / max(uUvScale, vec2(1e-5));
    vec2 uv = localUv;
    if (uSplitMode == 1) {
        if (uv.x >= 0.5) uv.x = (uv.x - 0.5) * 2.0;
        else uv.x = uv.x * 2.0;
    }

    vec2 screenUv = clamp(vTexCoord, vec2(0.0), uUvMax);
    float depth = texture(uDepthTex, screenUv).r;
    if (depth >= 1.0) {
        fragColor = vec4(0.0);
        return;
    }

    vec3 viewPos = fetchViewPos(uv, depth);
    vec3 viewNorm = normalize(texture(uNormalsTex, screenUv).rgb * 2.0 - 1.0);
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

        vec2 sampleLocalUv = ssPos.xy;
        if (uSplitMode == 1) {
            if (localUv.x >= 0.5) sampleLocalUv.x = sampleLocalUv.x * 0.5 + 0.5;
            else sampleLocalUv.x = sampleLocalUv.x * 0.5;
        }
        vec2 sampleTexUv = clamp(sampleLocalUv * uUvScale, vec2(0.0), uUvMax);

        float sampleDepth = texture(uDepthTex, sampleTexUv).r;
        vec3 sampleViewPos = fetchViewPos(ssPos.xy, sampleDepth);

        float delta = currentPos.z - sampleViewPos.z;
        if (delta > 0.0 && delta < 0.4) {
            vec3 sampledColor = texture(uColorTex, sampleTexUv).rgb;
            float fade = 1.0 - smoothstep(0.0, uMaxDistance, length(currentPos - viewPos));
            hitColor = vec4(sampledColor, fade * uIntensity);
            break;
        }
    }

    fragColor = hitColor;
}
