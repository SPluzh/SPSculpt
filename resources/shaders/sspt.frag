#version 300 es
precision highp float;

in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uGAlbedo;
uniform sampler2D uGNormal;
uniform sampler2D uGMaterial;
uniform sampler2D uDepthTex;
uniform sampler2D uPrevAccum;

uniform mat4 uInvProjection;
uniform mat4 uProjection;
uniform mat4 uInvView;
uniform mat4 uView;
uniform vec3 uSPH[9];
uniform int uFrameIndex;
uniform int uAccumCount;

#define PI 3.14159265358979323846f

float hash12(vec2 p) {
    vec3 p3  = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec2 hash22(vec2 p) {
    float n = hash12(p);
    return vec2(n, hash12(p + n));
}

vec3 sampleCosineHemisphere(vec2 u, vec3 N) {
    float r = sqrt(u.x);
    float theta = 2.0 * PI * u.y;
    vec3 B = normalize(abs(N.z) < 0.999 ? cross(N, vec3(0,0,1)) : cross(N, vec3(1,0,0)));
    vec3 T = cross(B, N);
    return normalize(r * cos(theta) * T + r * sin(theta) * B + sqrt(max(0.0, 1.0 - u.x)) * N);
}

vec3 sphericalHarmonicsEval(vec3 N) {
    float x = N.x; float y = N.y; float z = -N.z;
    vec3 result = uSPH[0] + uSPH[1] * y + uSPH[2] * z + uSPH[3] * x +
                  uSPH[4] * y * x + uSPH[5] * y * z +
                  uSPH[6] * (3.0 * z * z - 1.0) + uSPH[7] * (z * x) +
                  uSPH[8] * (x * x - y * y);
    return max(result, vec3(0.0));
}

void main() {
    vec2 uv = vTexCoord;
    float depth = texture(uDepthTex, uv).r;
    if (depth >= 1.0) {
        vec3 prev = texture(uPrevAccum, uv).rgb;
        fragColor = vec4(prev, 1.0);
        return;
    }

    vec4 gAlb = texture(uGAlbedo, uv);
    vec3 albedo = gAlb.rgb;
    float metallic = gAlb.a;

    vec3 normal = normalize(texture(uGNormal, uv).rgb * 2.0 - 1.0);
    vec4 gMat = texture(uGMaterial, uv);
    float roughness = gMat.r;

    vec4 clip = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 viewPos4 = uInvProjection * clip;
    vec3 viewPos = viewPos4.xyz / viewPos4.w;

    vec2 rnd = hash22(uv * 1000.0 + vec2(float(uFrameIndex) * 17.1, float(uFrameIndex) * 31.3));
    vec3 rayDirView = sampleCosineHemisphere(rnd, normal);

    float stepSize = 0.2;
    vec3 rayPos = viewPos + rayDirView * 0.1;
    vec3 sampledColor = vec3(0.0);
    bool hit = false;

    for (int i = 0; i < 20; ++i) {
        rayPos += rayDirView * stepSize;
        vec4 proj = uProjection * vec4(rayPos, 1.0);
        vec3 ss = proj.xyz / proj.w * 0.5 + 0.5;
        if (ss.x < 0.0 || ss.x > 1.0 || ss.y < 0.0 || ss.y > 1.0) break;

        float sampleD = texture(uDepthTex, ss.xy).r;
        vec4 sClip = vec4(ss.xy * 2.0 - 1.0, sampleD * 2.0 - 1.0, 1.0);
        vec4 sView4 = uInvProjection * sClip;
        vec3 sView = sView4.xyz / sView4.w;

        if (rayPos.z - sView.z > 0.0 && rayPos.z - sView.z < 0.4) {
            vec4 hitAlb = texture(uGAlbedo, ss.xy);
            sampledColor = hitAlb.rgb;
            hit = true;
            break;
        }
    }

    if (!hit) {
        vec3 worldN = mat3(uInvView) * rayDirView;
        sampledColor = sphericalHarmonicsEval(worldN);
    }

    vec3 directLighting = albedo * max(0.0, dot(normal, normalize(vec3(0.5, 0.8, 1.0)))) * 0.8;
    vec3 currentSample = directLighting + albedo * sampledColor * 0.5;

    if (uAccumCount <= 1) {
        fragColor = vec4(currentSample, 1.0);
    } else {
        vec3 prevColor = texture(uPrevAccum, uv).rgb;
        float weight = 1.0 / float(uAccumCount);
        vec3 accumulated = mix(prevColor, currentSample, weight);
        fragColor = vec4(accumulated, 1.0);
    }
}
