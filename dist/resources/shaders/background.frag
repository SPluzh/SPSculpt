#version 300 es
precision highp float;
in vec2 vTexCoord;
out vec4 fragColor;

uniform int uBackgroundType;
uniform float uBlur;
uniform sampler2D uTexture0; // env texture
uniform mat3 uIblTransform;
uniform vec3 uSPH[9];
uniform vec2 uEnvSize;

#define LIMIT_LOD 5.0
const mat3 LUVInverse = mat3(6.0013, -2.700, -1.7995, -1.332, 3.1029, -5.7720, 0.3007, -1.088, 5.6268);
vec3 decodeLUV(const in vec4 logLuv) {
  float Le = logLuv.z * 255.0 + logLuv.w;
  vec3 xp;
  xp.y = exp2((Le - 127.0) / 2.0);
  xp.z = xp.y / logLuv.y;
  xp.x = logLuv.x * xp.z;
  return max(LUVInverse * xp, 0.0);
}
vec2 toUVMipmap(const in float lod, const in vec2 uv) {
  float widthForLevel = uEnvSize.x / exp2(lod);
  vec2 uvSpaceLocal = vec2(1.0) + uv * (widthForLevel - 2.0);
  uvSpaceLocal.y += uEnvSize.y - widthForLevel * 2.0;
  return uvSpaceLocal / uEnvSize;
}
vec2 directionToUV(const in vec3 dir) {
  vec3 signOct = sign(dir);
  vec3 uvOct = dir / dot(dir, signOct);
  if (uvOct.z < 0.0) {
    uvOct.xy = signOct.xy * (1.0 - abs(uvOct)).yx;
  }
  return uvOct.xy * 0.5 + 0.5;
}
vec3 texturePanoramaLod(const in vec3 direction, const in float rLinear) {
  float lod = rLinear * (LIMIT_LOD - 1.0);
  vec2 uvBase = directionToUV(direction);
  return decodeLUV(mix(texture(uTexture0, toUVMipmap(floor(lod), uvBase)),
                       texture(uTexture0, toUVMipmap(ceil(lod), uvBase)),
                       fract(lod)));
}
vec3 sphericalHarmonics(const in vec3 N) {
  float x = N.x;
  float y = N.y;
  float z = -N.z;
  vec3 result = uSPH[0] + uSPH[1] * y + uSPH[2] * z + uSPH[3] * x +
                uSPH[4] * y * x + uSPH[5] * y * z +
                uSPH[6] * (3.0 * z * z - 1.0) + uSPH[7] * (z * x) +
                uSPH[8] * (x * x - y * y);
  return max(result, vec3(0.0));
}

#include "colorspace.glsl"

void main() {
    vec3 color;
    if (uBackgroundType == 0) {
        color = sRGBToLinear(texture(uTexture0, vTexCoord).rgb);
    } else {
        vec3 dir = uIblTransform * vec3(vTexCoord.xy * 2.0 - 1.0, -1.0);
        dir = normalize(dir);
        if (uBackgroundType == 1) {
            color = texturePanoramaLod(dir, uBlur * uBlur);
        } else {
            color = sphericalHarmonics(dir);
        }
    }
    fragColor = encodeRGBM(color);
}
