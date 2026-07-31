#define FXAA_REDUCE_MIN (1.0 / 128.0)

vec3 fxaa(const in sampler2D tex, const in vec2 uvNW, const in vec2 uvNE, const in vec2 uvSW, const in vec2 uvSE, const in vec2 uvM, const in vec2 invRes, const in bool sharpMode) {
    const vec3 luma = vec3(0.299, 0.587, 0.114);
    float reduceMul = sharpMode ? (1.0 / 16.0) : (1.0 / 8.0);
    float spanMax = sharpMode ? 8.0 : 12.0;

    float lumaNW = dot(texture(tex, uvNW).xyz, luma);
    float lumaNE = dot(texture(tex, uvNE).xyz, luma);
    float lumaSW = dot(texture(tex, uvSW).xyz, luma);
    float lumaSE = dot(texture(tex, uvSE).xyz, luma);
    float lumaM  = dot(texture(tex, uvM).xyz,  luma);
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    vec2 dir = vec2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)), ((lumaNW + lumaSW) - (lumaNE + lumaSE)));
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * reduceMul), FXAA_REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = min(vec2(spanMax, spanMax), max(vec2(-spanMax, -spanMax), dir * rcpDirMin)) * invRes;
    
    vec3 rgbA = 0.5 * (texture(tex, uvM + dir * (1.0 / 3.0 - 0.5)).xyz + texture(tex, uvM + dir * (2.0 / 3.0 - 0.5)).xyz);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(tex, uvM - dir * 0.5).xyz + texture(tex, uvM + dir * 0.5).xyz);
    
    float lumaB = dot(rgbB, luma);
    if ((lumaB < lumaMin) || (lumaB > lumaMax))
        return rgbA;
    return rgbB;
}
