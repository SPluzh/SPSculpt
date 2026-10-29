#define PI 3.14159265358979323846f
#define PI_2 (2.0f * PI)

#define MAX_SYM_PLANES 3
uniform vec3 uPlaneN[MAX_SYM_PLANES];
uniform vec3 uPlaneO[MAX_SYM_PLANES];
uniform vec3 uPlaneColor[MAX_SYM_PLANES];
uniform int uSymCount;
uniform int uSym;
uniform float uSymLineWidth;
uniform float uSymAlpha;
uniform int uDarken;
uniform float uCurvature;
uniform float uFov;
uniform int uFlat;
uniform int uIsXRay;
uniform int uIsThumbnail;
uniform int uShowInvertedNormals;
uniform vec4 uInvertedNormalColor;

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
    vec3 dX = dFdx(vVertex);
    vec3 dY = dFdy(vVertex);
    vec3 crossN = cross(dY, dX);
    float lenCross = length(crossN);
    vec3 flatNormal = lenCross > 1e-6 ? -(crossN / lenCross) : vec3(0.0, 0.0, 1.0);

    float lenN = length(vNormal);
    if (lenN < 1e-4 || isnan(lenN)) {
        return flatNormal;
    }
    vec3 N = vNormal / lenN;

    // Two-sided normal consistency: guarantee flatNormal faces the camera
    vec3 eye = -vVertex;
    if (dot(flatNormal, eye) < 0.0) {
        flatNormal = -flatNormal;
    }
    if (dot(N, flatNormal) < 0.0) {
        N = -N;
    }

    float d = dot(N, flatNormal);
    // Smooth crease threshold:
    // On smooth curved surfaces, d is always > 0.95 (angle < 18 deg).
    // Across sharp cut edges and corners, d drops significantly (d < 0.90, angle > 25 deg).
    // When d < 0.90, blend directly towards flatNormal.
    // For any crease >= 36 deg (d <= 0.80, including cuts and obtuse corners):
    // fully clamp to flatNormal so cut surfaces and corners stay crisp and NEVER darken or blacken!
    if (d < 0.90) {
        float t = clamp((d - 0.80) / (0.90 - 0.80), 0.0, 1.0);
        N = normalize(mix(flatNormal, N, t * t));
    }
    return N;
}

vec3 getNormal() {
    if (uBevelEnabled == 1) {
        return getBevelNormal();
    }
    if (uFlat != 0) {
        vec3 dX = dFdx(vVertex);
        vec3 dY = dFdy(vVertex);
        vec3 crossN = cross(dY, dX);
        float lenCross = length(crossN);
        return lenCross > 1e-6 ? -(crossN / lenCross) : vec3(0.0, 0.0, 1.0);
    }
    return getAlignedNormal();
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
    if (uIsThumbnail == 1) {
        return vec4(linearTosRGB(frag), 1.0);
    }
    if (uIsXRay == 1) {
        return vec4(frag * alpha, alpha);
    }
    vec3 col = computeCurvature(vVertex, getAlignedNormal(), frag, uCurvature, uFov);
    if (uDarken == 1) col *= 0.3;
    col *= (0.15 + 0.85 * vMasking);
    if (uShowInvertedNormals == 1 && !gl_FrontFacing) {
        float luma = max(dot(col, vec3(0.2126, 0.7152, 0.0722)), 0.001);
        vec3 invLinear = sRGBToLinear(uInvertedNormalColor.rgb);
        float spec = pow(clamp((luma - 0.5) / 0.5, 0.0, 1.0), 3.0) * 0.25;
        vec3 tinted = invLinear * (luma * 1.25 + 0.05) + vec3(spec);
        col = mix(col, tinted, clamp(uInvertedNormalColor.a, 0.0, 1.0));
    }
    int activePlanes = uSymCount > 0 ? uSymCount : (uSym == 1 ? 1 : 0);
    float symWidth = uSymLineWidth > 0.0001 ? uSymLineWidth : 0.11;
    float innerWidth = symWidth * (0.03 / 0.22);
    float symAlpha = uSymAlpha > 0.0001 ? uSymAlpha : (uSymAlpha < 0.0 ? 0.0 : 1.0);
    for (int i = 0; i < 3; ++i) {
        if (i >= activePlanes) break;
        float dist = abs(dot(uPlaneN[i], vVertex - uPlaneO[i]));
        if (dist < symWidth) {
            float factor = smoothstep(symWidth, innerWidth, dist) * symAlpha;
            vec3 vibrantColor = uPlaneColor[i] * 1.8;
            col = mix(col, vibrantColor, factor * 0.85) + uPlaneColor[i] * (factor * 0.4);
        }
    }
    return alpha != 1.0 ? vec4(col * alpha, alpha) : encodeRGBM(col);
}
