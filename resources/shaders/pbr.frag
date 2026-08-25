#version 300 es
precision highp float;

#define LIMIT_LOD 5.0
#define MAX_LIGHTS 8

struct Light {
    vec3  position;
    vec3  direction;
    vec3  color;
    float intensity;
    float range;
    float innerCos;
    float outerCos;
    int   type;        // 0=directional, 1=point, 2=spot
    int   castShadow;
    int   enabled;
};

uniform Light uLights[MAX_LIGHTS];
uniform int   uNumLights;

uniform highp sampler2DShadow uShadowMap;
uniform mat4 uLightMVP;
uniform int  uShadowEnabled;

uniform sampler2D uTexture0;
uniform sampler2D uNormalMap;
uniform int uHasNormalMap;
uniform float uExposure;
uniform mat3 uIblTransform;
uniform vec3 uSPH[9];
uniform vec2 uEnvSize;

uniform vec3 uAlbedo;
uniform float uRoughness;
uniform float uMetallic;
uniform float uAlpha;
uniform int uUseTexture;

// Glass / Transmission uniforms
uniform float uTransmission;
uniform float uIor;

// Subsurface Scattering (SSS) uniforms (Intensity, Depth, Color)
uniform vec3  uSssColor;
uniform float uSssIntensity;
uniform float uSssDepth;

in vec3 vVertex;
in vec3 vNormal;
in vec3 vColor;
in vec3 vMaterial;
in float vMasking;
flat in uint vFaceGroup;
in vec2 vTexCoord;
in vec3 vTangent;
in vec3 vBitangent;
uniform bool uShowPolyGroups;

out vec4 fragColor;

vec3 groupColor(uint gid) {
    if (gid == 0u) return vec3(0.72, 0.52, 0.45);
    uint n = (gid * 1664525u + 1013904223u);
    float r = float(n & 0xFFu) / 255.0;
    float g = float((n >> 8u) & 0xFFu) / 255.0;
    float b = float((n >> 16u) & 0xFFu) / 255.0;
    return mix(vec3(r, g, b), vec3(1.0), 0.15);
}

#include "common.glsl"

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
  return decodeLUV(mix(textureLod(uTexture0, toUVMipmap(floor(lod), uvBase), 0.0),
                       textureLod(uTexture0, toUVMipmap(ceil(lod), uvBase), 0.0),
                       fract(lod)));
}
vec3 integrateBRDFApprox(const in vec3 specular, float roughness, float NoV) {
  const vec4 c0 = vec4(-1, -0.0275, -0.572, 0.022);
  const vec4 c1 = vec4(1, 0.0425, 1.04, -0.04);
  vec4 r = roughness * c0 + c1;
  float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
  vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
  return specular * AB.x + AB.y;
}
vec3 getSpecularDominantDir(const in vec3 N, const in vec3 R, const in float realRoughness) {
  float smoothness = 1.0 - realRoughness;
  return mix(N, R, smoothness * (sqrt(smoothness) + realRoughness));
}
vec3 approximateSpecularIBL(const in vec3 specularColor, float rLinear, const in vec3 N, const in vec3 V) {
  float NoV = clamp(dot(N, V), 0.0, 1.0);
  vec3 R = normalize((2.0 * NoV) * N - V);
  R = getSpecularDominantDir(N, R, rLinear);
  vec3 prefilteredColor = texturePanoramaLod(uIblTransform * R, rLinear);
  return prefilteredColor * integrateBRDFApprox(specularColor, rLinear, NoV);
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
vec3 computeIBL_UE4(const in vec3 N, const in vec3 V, const in vec3 albedo, const in float roughness, const in vec3 specular) {
  vec3 color = albedo * sphericalHarmonics(uIblTransform * N);
  color += approximateSpecularIBL(specular, roughness, N, V);
  return color;
}

float D_GGX(float NoH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NoH2 = NoH * NoH;
    float num = a2;
    float denom = (NoH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return num / max(denom, 0.00001);
}

float V_SmithGGXCorrelated(float NoV, float NoL, float roughness) {
    float a = roughness * roughness;
    float GGXV = NoL * (NoV * (1.0 - a) + a);
    float GGXL = NoV * (NoL * (1.0 - a) + a);
    return 0.5 / max(GGXV + GGXL, 0.00001);
}

vec3 F_Schlick(float VoH, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - VoH, 0.0, 1.0), 5.0);
}

float PCF(vec3 viewPos) {
    vec4 lp = uLightMVP * vec4(viewPos, 1.0);
    vec3 projCoords = lp.xyz / lp.w;
    projCoords = projCoords * 0.5 + 0.5;
    if (projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0 || projCoords.z > 1.0) {
        return 1.0;
    }
    float shadow = 0.0;
    vec2 texelSize = vec2(1.0 / 2048.0);
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            shadow += texture(uShadowMap, vec3(projCoords.xy + vec2(x, y) * texelSize, projCoords.z - 0.002));
        }
    }
    return shadow / 9.0;
}

void main() {
    vec3 normal = getNormal();
    vec3 viewDir = -normalize(vVertex);
    if (uHasNormalMap == 1) {
        vec3 mapN = texture(uNormalMap, vTexCoord).rgb * 2.0 - 1.0;
        mat3 TBN = mat3(normalize(vTangent), normalize(vBitangent), normal);
        vec3 perturbedN = normalize(TBN * mapN);
        if (dot(perturbedN, viewDir) < 0.0) {
            perturbedN = reflect(perturbedN, normal);
        }
        normal = perturbedN;
    }
    
    // Geometric Specular & Normal Filtering (Screen-Space Derivative Roughness Modification)
    vec3 dNdx = dFdx(normal);
    vec3 dNdy = dFdy(normal);
    float normalVariance = max(dot(dNdx, dNdx), dot(dNdy, dNdy));

    float baseRoughness = max(0.001, (uRoughness >= 0.0 ? uRoughness : vMaterial.x));
    float roughness = clamp(sqrt(baseRoughness * baseRoughness + normalVariance * 0.5), 0.001, 1.0);
    float metallic = (uMetallic >= 0.0 ? uMetallic : vMaterial.y);
    vec3 rawColor = (uAlbedo.r >= 0.0 ? uAlbedo : vColor);
    if (uShowPolyGroups && vFaceGroup > 0u) {
        rawColor = groupColor(vFaceGroup);
    }
    vec3 linColor = sRGBToLinear(rawColor);
    
    // Glass / Transmission IOR and dielectric specular F0
    float ior = max(1.0, uIor);
    float f0_dielectric = 0.04;
    if (uTransmission > 0.0) {
        float customF0 = pow((1.0 - ior) / (1.0 + ior), 2.0);
        f0_dielectric = mix(0.04, customF0, clamp(uTransmission, 0.0, 1.0));
    }
    vec3 dielectricSpecular = vec3(f0_dielectric);

    vec3 albedo = linColor * (1.0 - metallic);
    vec3 specular = mix(dielectricSpecular, linColor, metallic);
    vec3 color = vec3(0.0);

    float NoV = max(dot(normal, viewDir), 0.0);
    vec3 F_v = F_Schlick(NoV, specular);

    if (uUseTexture == 1) {
        color = uExposure * computeIBL_UE4(normal, viewDir, albedo, roughness, specular);
    } else {
        color = albedo * 0.15;
    }

    // Glass / Transmission Refraction calculation
    if (uTransmission > 0.0) {
        vec3 refrDir = refract(-viewDir, normal, 1.0 / ior);
        if (length(refrDir) < 0.001) {
            refrDir = reflect(-viewDir, normal); // Total Internal Reflection
        }
        vec3 refrEnv = texturePanoramaLod(uIblTransform * refrDir, roughness) * linColor * uExposure;
        vec3 specIBL = approximateSpecularIBL(specular, roughness, normal, viewDir) * uExposure;
        vec3 glassColor = mix(refrEnv, specIBL, F_v);
        color = mix(color, glassColor, clamp(uTransmission, 0.0, 1.0));
    }

    if (uNumLights > 0) {
        for (int i = 0; i < uNumLights; ++i) {
            if (uLights[i].enabled == 0) continue;

            vec3 L;
            float attenuation = 1.0;

            if (uLights[i].type == 0) {
                L = -normalize(uLights[i].direction);
            } else {
                vec3 lightVec = uLights[i].position - vVertex;
                float dist = length(lightVec);
                L = normalize(lightVec);
                attenuation = clamp(1.0 - (dist / max(0.01, uLights[i].range)), 0.0, 1.0);
                attenuation *= attenuation;

                if (uLights[i].type == 2) {
                    float cosAngle = dot(-L, normalize(uLights[i].direction));
                    float spotAtten = clamp((cosAngle - uLights[i].outerCos) / max(0.0001, uLights[i].innerCos - uLights[i].outerCos), 0.0, 1.0);
                    attenuation *= spotAtten;
                }
            }

            float NoL = dot(normal, L);
            float shadowFactor = 1.0;
            if (uShadowEnabled == 1 && uLights[i].castShadow == 1 && uLights[i].type == 0) {
                shadowFactor = PCF(vVertex);
            }

            // Direct Subsurface Scattering (Translucency & Back-lighting)
            if (uSssIntensity > 0.0) {
                float depthVal = max(0.01, uSssDepth);
                float sssDistortion = 0.3 * depthVal;
                float sssPower = max(1.0, 4.0 / depthVal);
                float sssWrap = clamp(0.4 * depthVal, 0.0, 1.0);

                vec3 sssLightDir = L + normal * sssDistortion;
                float sssDot = max(0.0, dot(-viewDir, sssLightDir));
                float sssTranslucency = pow(sssDot, sssPower) * uSssIntensity;
                vec3 sssContrib = uSssColor * sssTranslucency * uLights[i].color * uLights[i].intensity * attenuation * shadowFactor;
                color += sssContrib;
            }

            if (NoL > 0.0 || uSssIntensity > 0.0) {
                float depthVal = max(0.01, uSssDepth);
                float sssWrap = clamp(0.4 * depthVal, 0.0, 1.0);
                float effectiveNoL = max(NoL, 0.0);
                vec3 H = normalize(viewDir + L);
                float NoH = max(dot(normal, H), 0.0);
                float VoH = max(dot(viewDir, H), 0.0);

                float D = D_GGX(NoH, roughness);
                float Vis = V_SmithGGXCorrelated(NoV, effectiveNoL, roughness);
                vec3 F = F_Schlick(VoH, specular);

                vec3 specBRDF = D * Vis * F;
                vec3 diffBRDF = (vec3(1.0) - F) * (1.0 - metallic) * (albedo / PI);

                if (uSssIntensity > 0.0) {
                    float NoL_wrap = max(0.0, (NoL + sssWrap) / (1.0 + sssWrap));
                    float scatterTerm = smoothstep(0.0, 0.4, NoL_wrap) * (1.0 - smoothstep(0.4, 0.9, NoL_wrap));
                    vec3 sssDiffColor = mix(albedo, albedo * uSssColor * 2.5, scatterTerm * uSssIntensity);
                    diffBRDF = (vec3(1.0) - F) * (1.0 - metallic) * (sssDiffColor / PI);
                    effectiveNoL = NoL_wrap;
                }

                vec3 lightContrib = (diffBRDF + specBRDF) * uLights[i].color * uLights[i].intensity * effectiveNoL * attenuation * shadowFactor;
                color += lightContrib;
            }
        }
    } else {
        vec3 lightDir = normalize(vec3(0.5, 0.8, 1.0));
        vec3 halfDir = normalize(lightDir + viewDir);
        float NdotL = max(dot(normal, lightDir), 0.0);
        float NdotH = max(dot(normal, halfDir), 0.0);
        vec3 diff = albedo * NdotL * 0.8;
        vec3 specVal = specular * pow(NdotH, 32.0) * 0.5;
        color += diff + specVal;
    }

    fragColor = encodeFragColor(color, uAlpha);
}
