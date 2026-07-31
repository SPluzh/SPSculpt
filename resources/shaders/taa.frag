#version 300 es
precision highp float;

uniform sampler2D uCurrentTex;     // Current frame
uniform sampler2D uPrevAccumTex;   // Accumulated previous frame
uniform sampler2D uMotionVec;      // Motion vectors
uniform vec2 uInvSize;
uniform vec2 uCurrentJitter;       // De-jitter offset
uniform float uResetHistory;       // 1.0 if history should be reset

in vec2 vTexCoord;
out vec4 fragColor;

vec3 clipAABB(vec3 aabbMin, vec3 aabbMax, vec3 prevSample) {
    vec3 pClip = 0.5 * (aabbMax + aabbMin);
    vec3 eClip = 0.5 * (aabbMax - aabbMin) + 0.0001;
    vec3 vClip = prevSample - pClip;
    vec3 vUnit = vClip / eClip;
    vec3 aUnit = abs(vUnit);
    float maUnit = max(aUnit.x, max(aUnit.y, aUnit.z));
    return maUnit > 1.0 ? pClip + vClip / maUnit : prevSample;
}

void main() {
    vec2 uv = vTexCoord - uCurrentJitter;
    vec3 curr = texture(uCurrentTex, uv).rgb;

    if (uResetHistory > 0.5) {
        fragColor = vec4(curr, 1.0);
        return;
    }

    vec2 mv = texture(uMotionVec, vTexCoord).rg;
    vec2 prevUV = vTexCoord - mv;
    vec3 prev = texture(uPrevAccumTex, prevUV).rgb;

    // Neighbourhood AABB clamping
    vec3 nMin = curr, nMax = curr;
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            vec3 n = texture(uCurrentTex, uv + vec2(x, y) * uInvSize).rgb;
            nMin = min(nMin, n);
            nMax = max(nMax, n);
        }
    }
    prev = clipAABB(nMin, nMax, prev);

    float blendFactor = (prevUV.x < 0.0 || prevUV.x > 1.0 || prevUV.y < 0.0 || prevUV.y > 1.0) ? 1.0 : 0.1;
    fragColor = vec4(mix(prev, curr, blendFactor), 1.0);
}
