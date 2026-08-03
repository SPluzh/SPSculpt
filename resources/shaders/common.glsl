#define PI 3.14159265358979323846f
#define PI_2 (2.0f * PI)

#define MAX_SYM_PLANES 3
uniform vec3 uPlaneN[MAX_SYM_PLANES];
uniform vec3 uPlaneO[MAX_SYM_PLANES];
uniform vec3 uPlaneColor[MAX_SYM_PLANES];
uniform int uSymCount;
uniform int uSym;
uniform float uSymLineWidth;
uniform int uDarken;
uniform float uCurvature;
uniform float uFov;
uniform int uFlat;
uniform int uIsXRay;

uniform int uBevelEnabled;
uniform sampler2D uBevelNormalMap;
uniform vec2 uInvViewportSize;

vec3 getBevelNormal() {
    vec3 ssNormal = texture(uBevelNormalMap, gl_FragCoord.xy * uInvViewportSize).rgb;
    return normalize(ssNormal * 2.0 - 1.0);
}

vec3 getAlignedNormal() {
    if (uBevelEnabled == 1) {
        return getBevelNormal();
    }
    vec3 N = normalize(vNormal);
    vec3 flatNormal = -normalize(cross(dFdy(vVertex), dFdx(vVertex)));
    if (dot(N, flatNormal) < 0.0) {
        return -N;
    }
    return N;
}

vec3 getNormal() {
    if (uBevelEnabled == 1) {
        return getBevelNormal();
    }
    return uFlat == 0 ? getAlignedNormal() : -normalize(cross(dFdy(vVertex), dFdx(vVertex)));
}

vec3 sRGBToLinear(const in vec3 col) {
    return pow(col, vec3(2.2));
}
vec3 linearTosRGB(const in vec3 col) {
    return pow(col, vec3(1.0 / 2.2));
}

vec4 encodeRGBM(const in vec3 col) {
    vec4 rgbm;
    vec3 color = col / 5.0;
    rgbm.a = clamp(max(max(color.r, color.g), max(color.b, 1e-6)), 0.0, 1.0);
    rgbm.a = ceil(rgbm.a * 255.0) / 255.0;
    rgbm.rgb = color / rgbm.a;
    return rgbm;
}

vec3 decodeRGBM(const in vec4 rgbm) {
    return 5.0 * rgbm.rgb * rgbm.a;
}

vec3 computeCurvature(const in vec3 vertex, const in vec3 normal, const in vec3 color, const in float str, const in float fov) {
    if (str < 1e-3) return color;
    vec3 n = normalize(normal);
    vec3 dx = dFdx(n);
    vec3 dy = dFdy(n);
    vec3 xneg = n - dx;
    vec3 xpos = n + dx;
    vec3 yneg = n - dy;
    vec3 ypos = n + dy;
    float depth = fov > 0.0 ? length(vertex) * fov : -fov;
    float cur = (cross(xneg, xpos).y - cross(yneg, ypos).x) * str * 80.0 / depth;
    return mix(mix(color, color * 0.3, clamp(-cur * 15.0, 0.0, 1.0)), color * 2.0, clamp(cur * 25.0, 0.0, 1.0));
}

vec4 encodeFragColor(const in vec3 frag, const in float alpha) {
    if (uIsXRay == 1) {
        return vec4(frag * alpha, alpha);
    }
    vec3 col = computeCurvature(vVertex, getAlignedNormal(), frag, uCurvature, uFov);
    if (uDarken == 1) col *= 0.3;
    col *= (0.15 + 0.85 * vMasking);
    int activePlanes = uSymCount > 0 ? uSymCount : (uSym == 1 ? 1 : 0);
    float symWidth = uSymLineWidth > 0.0001 ? uSymLineWidth : 0.11;
    float innerWidth = symWidth * (0.03 / 0.22);
    for (int i = 0; i < 3; ++i) {
        if (i >= activePlanes) break;
        float dist = abs(dot(uPlaneN[i], vVertex - uPlaneO[i]));
        if (dist < symWidth) {
            float factor = smoothstep(symWidth, innerWidth, dist);
            vec3 vibrantColor = uPlaneColor[i] * 1.8;
            col = mix(col, vibrantColor, factor * 0.85) + uPlaneColor[i] * (factor * 0.4);
        }
    }
    return alpha != 1.0 ? vec4(col * alpha, alpha) : encodeRGBM(col);
}
