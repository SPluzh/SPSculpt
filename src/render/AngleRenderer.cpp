#include "render/AngleRenderer.h"
#include <iostream>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "scene/Scene.h"
#include "mesh/Mesh.h"
#include "../third_party/stb_image.h"

// Cache uniform locations to avoid driver lookups on every draw call
#define glGetUniformLocation(prog, name) getCachedUniformLocation(prog, name)

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

AngleRenderer::AngleRenderer() {}

AngleRenderer::~AngleRenderer() {
    if (m_pbrProgram) glDeleteProgram(m_pbrProgram);
    if (m_matcapProgram) glDeleteProgram(m_matcapProgram);
    if (m_flatProgram) glDeleteProgram(m_flatProgram);
    if (m_wireframeProgram) glDeleteProgram(m_wireframeProgram);
    if (m_bgProgram) glDeleteProgram(m_bgProgram);
    if (m_selectionProgram) glDeleteProgram(m_selectionProgram);
    if (m_refImageProgram) glDeleteProgram(m_refImageProgram);
    if (m_mergeProgram) glDeleteProgram(m_mergeProgram);
    if (m_fxaaProgram) glDeleteProgram(m_fxaaProgram);
    if (m_viewport2DProgram) glDeleteProgram(m_viewport2DProgram);
    if (m_contourProgram) glDeleteProgram(m_contourProgram);
    if (m_wetClayProgram) glDeleteProgram(m_wetClayProgram);
    if (m_voxelCheckerProgram) glDeleteProgram(m_voxelCheckerProgram);
    if (m_normalProgram) glDeleteProgram(m_normalProgram);

    if (m_bgVao) glDeleteVertexArrays(1, &m_bgVao);
    if (m_bgVbo) glDeleteBuffers(1, &m_bgVbo);
    if (m_fsqVao) glDeleteVertexArrays(1, &m_fsqVao);
    if (m_fsqVbo) glDeleteBuffers(1, &m_fsqVbo);
    if (m_gridVao) glDeleteVertexArrays(1, &m_gridVao);
    if (m_gridVbo) glDeleteBuffers(1, &m_gridVbo);

    if (m_selectionVao) glDeleteVertexArrays(1, &m_selectionVao);
    if (m_circleVbo) glDeleteBuffers(1, &m_circleVbo);
    if (m_dotVbo) glDeleteBuffers(1, &m_dotVbo);
    if (m_lassoVao) glDeleteVertexArrays(1, &m_lassoVao);
    if (m_lassoVbo) glDeleteBuffers(1, &m_lassoVbo);

    if (m_envTexture) glDeleteTextures(1, &m_envTexture);

    for (const auto& matcap : m_matcaps) {
        if (matcap.textureId) {
            GLuint tid = matcap.textureId;
            glDeleteTextures(1, &tid);
        }
    }

    m_rttContour.release();
    m_rttOpaque.release();
    m_rttTransparent.release();
    m_rttMerge.release();
    m_rttComposite.release();
}

GLuint AngleRenderer::compileShader(GLenum type, const std::string& source) {
    GLuint shader = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLchar infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        std::cerr << "Shader compile error (" << type << "): " << infoLog << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint AngleRenderer::linkProgram(GLuint vs, GLuint fs) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        GLchar infoLog[512];
        glGetProgramInfoLog(program, 512, nullptr, infoLog);
        std::cerr << "Shader link error: " << infoLog << std::endl;
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

bool AngleRenderer::init(int width, int height) {
    m_width = width;
    m_height = height;

    // A. Common GLSL shader fragments
    std::string commonFragCode = R"(
        #define PI 3.14159265358979323846f
        #define PI_2 (2.0f * PI)

        uniform vec3 uPlaneN;
        uniform vec3 uPlaneO;
        uniform int uSym;
        uniform int uDarken;
        uniform float uCurvature;
        uniform float uFov;
        uniform int uFlat;

        vec3 getNormal() {
            return uFlat == 0 ? normalize(vNormal) : normalize(cross(dFdy(vVertex), dFdx(vVertex)));
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
            vec3 col = computeCurvature(vVertex, vNormal, frag, uCurvature, uFov);
            if (uDarken == 1) col *= 0.3;
            col *= (0.15 + 0.85 * vMasking);
            if (uSym == 1 && abs(dot(uPlaneN, vVertex - uPlaneO)) < 0.15) {
                col = min(col * 1.5, vec3(1.0));
            }
            return alpha != 1.0 ? vec4(col * alpha, alpha) : encodeRGBM(col);
        }
    )";

    std::string wetClayCode = R"(
        uniform vec3 uClayColor;
        uniform float uWetness;
        uniform float uBumpStrength;
        uniform float uNoiseScale;
        uniform float uSSSIntensity;
        uniform vec3 uSSSColor;
        uniform mat3 uN;

        float hash3(vec3 p) {
            p = fract(p * 0.1031);
            p += dot(p, p.zyx + 31.32);
            return fract((p.x + p.y) * p.z);
        }

        float noise3(vec3 p) {
            vec3 i = floor(p);
            vec3 f = fract(p);
            vec3 u = f * f * (3.0 - 2.0 * f);
            return mix(
                mix(
                    mix(hash3(i + vec3(0.0, 0.0, 0.0)), hash3(i + vec3(1.0, 0.0, 0.0)), u.x),
                    mix(hash3(i + vec3(0.0, 1.0, 0.0)), hash3(i + vec3(1.0, 1.0, 0.0)), u.x),
                    u.y
                ),
                mix(
                    mix(hash3(i + vec3(0.0, 0.0, 1.0)), hash3(i + vec3(1.0, 0.0, 1.0)), u.x),
                    mix(hash3(i + vec3(0.0, 1.0, 1.0)), hash3(i + vec3(1.0, 1.0, 1.0)), u.x),
                    u.y
                ),
                u.z
            );
        }

        float clayHeight(vec3 p) {
            vec3 warp = vec3(
                noise3(p * 0.3),
                noise3(p * 0.3 + vec3(1.7, 3.4, 5.1)),
                noise3(p * 0.3 + vec3(2.6, 1.2, 4.8))
            );
            float low = noise3((p + warp * 0.5) * 0.5);
            float high = noise3(p * 4.0);
            return low * 0.85 + high * 0.15;
        }

        vec3 computeWetClay(vec3 viewVertex, vec3 viewNormal, vec3 vertexColor, float masking, vec3 objPos) {
            vec3 sp = objPos * uNoiseScale;
            float eps = 0.02;
            float nCenter = clayHeight(sp);
            float nX = clayHeight(sp + vec3(eps, 0.0, 0.0));
            float nY = clayHeight(sp + vec3(0.0, eps, 0.0));
            float nZ = clayHeight(sp + vec3(0.0, 0.0, eps));
            
            vec3 bump = vec3(nX - nCenter, nY - nCenter, nZ - nCenter) / eps;
            vec3 normal = getNormal();
            vec3 bumpedNormal = normalize(normal - (uN * bump) * uBumpStrength * 0.15);
            
            float curvature = clamp(dot(normalize(viewNormal), normalize(-viewVertex)) * 0.5 + 0.5, 0.0, 1.0);
            vec3 baseClayColor = uClayColor.x >= 0.0 ? uClayColor : vertexColor;
            vec3 linClayColor = sRGBToLinear(baseClayColor);
            vec3 wetClayColor = linClayColor * vec3(0.55, 0.5, 0.48);
            vec3 dryClayColor = linClayColor * vec3(1.15, 1.1, 1.05);
            vec3 baseColor = mix(wetClayColor, dryClayColor, curvature);
            
            vec3 lightDir = normalize(vec3(0.5, 1.0, 0.8));
            float wrap = 0.35;
            float NdotL = clamp((dot(bumpedNormal, lightDir) + wrap) / (1.0 + wrap), 0.0, 1.0);
            vec3 diffuse = baseColor * NdotL;
            
            float sssFactor = pow(clamp(dot(bumpedNormal, lightDir) * -0.5 + 0.5, 0.0, 1.0), 2.0);
            vec3 sss = uSSSColor * uSSSIntensity * sssFactor * baseColor;
            
            vec3 halfDir = normalize(normalize(-viewVertex) + lightDir);
            float NoH = max(dot(bumpedNormal, halfDir), 0.0);
            
            float wetNoise = noise3(sp * 1.5 + vec3(10.0));
            float roughness = mix(0.7, 0.08, uWetness * (0.4 + 0.6 * wetNoise));
            float shininess = 2.0 / (roughness * roughness) - 2.0;
            float specFactor = pow(NoH, shininess) * (1.0 - roughness);
            vec3 specColor = vec3(0.9, 0.95, 1.0) * specFactor * uWetness;
            
            float ao = mix(1.0, 0.5, clamp((0.5 - nCenter) * 2.0, 0.0, 1.0));
            vec3 finalColor = (diffuse + sss) * ao + specColor;
            finalColor += baseColor * 0.15 * ao;
            return finalColor;
        }
    )";

    std::string colorspaceCode = R"(
        #define LIN_SRGB(x) (x < 0.0031308 ? x * 12.92 : 1.055 * pow(x, 1.0/2.4) - 0.055)
        float linearTosRGB(const in float c) { return LIN_SRGB(c); }
        vec3 linearTosRGB(const in vec3 c) { return vec3(LIN_SRGB(c.r), LIN_SRGB(c.g), LIN_SRGB(c.b)); }
        
        #define SRGB_LIN(x) (x < 0.04045 ? x * (1.0 / 12.92) : pow((x + 0.055) * (1.0 / 1.055), 2.4))
        float sRGBToLinear(const in float c) { return SRGB_LIN(c); }
        vec3 sRGBToLinear(const in vec3 c) { return vec3(SRGB_LIN(c.r), SRGB_LIN(c.g), SRGB_LIN(c.b)); }

        vec4 encodeRGBM(const in vec3 col) {
            vec4 rgbm;
            vec3 color = col / 5.0;
            rgbm.a = clamp(max(max(color.r, color.g), max(color.b, 1e-6)), 0.0, 1.0);
            rgbm.a = ceil(rgbm.a * 255.0) / 255.0;
            rgbm.rgb = color / rgbm.a;
            return rgbm;
        }

        vec3 decodeRGBM(const in vec4 col) {
            return 5.0 * col.rgb * col.a;
        }
    )";

    std::string fxaaCode = R"(
        #define FXAA_REDUCE_MIN (1.0 / 128.0)
        #define FXAA_REDUCE_MUL (1.0 / 8.0)
        #define FXAA_SPAN_MAX 12.0

        vec3 fxaa(const in sampler2D tex, const in vec2 uvNW, const in vec2 uvNE, const in vec2 uvSW, const in vec2 uvSE, const in vec2 uvM, const in vec2 invRes) {
            const vec3 luma = vec3(0.299, 0.587, 0.114);
            float lumaNW = dot(texture(tex, uvNW).xyz, luma);
            float lumaNE = dot(texture(tex, uvNE).xyz, luma);
            float lumaSW = dot(texture(tex, uvSW).xyz, luma);
            float lumaSE = dot(texture(tex, uvSE).xyz, luma);
            float lumaM  = dot(texture(tex, uvM).xyz,  luma);
            float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
            float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

            vec2 dir = vec2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)), ((lumaNW + lumaSW) - (lumaNE + lumaSE)));
            float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
            float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
            dir = min(vec2(FXAA_SPAN_MAX, FXAA_SPAN_MAX), max(vec2(-FXAA_SPAN_MAX, -FXAA_SPAN_MAX), dir * rcpDirMin)) * invRes;
            
            vec3 rgbA = 0.5 * (texture(tex, uvM + dir * (1.0 / 3.0 - 0.5)).xyz + texture(tex, uvM + dir * (2.0 / 3.0 - 0.5)).xyz);
            vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(tex, uvM - dir * 0.5).xyz + texture(tex, uvM + dir * 0.5).xyz);
            
            float lumaB = dot(rgbB, luma);
            if ((lumaB < lumaMin) || (lumaB > lumaMax))
                return rgbA;
            return rgbB;
        }
    )";

    // B. Compile Shaders
    // 1. Background Shader
    std::string bgVert = R"(#version 300 es
        layout(location = 0) in vec2 aVertex;
        out vec2 vTexCoord;
        void main() {
            vTexCoord = aVertex * 0.5 + 0.5;
            gl_Position = vec4(aVertex, 1.0, 1.0);
        }
    )";
    std::string bgFrag = R"(#version 300 es
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
        )" + colorspaceCode + R"(
        void main() {
            vec3 color;
            if (uBackgroundType == 0) {
                vec3 color1 = vec3(0.08, 0.09, 0.1);
                vec3 color2 = vec3(0.22, 0.23, 0.25);
                color = sRGBToLinear(mix(color1, color2, vTexCoord.y));
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
    )";
    GLuint vsBg = compileShader(GL_VERTEX_SHADER, bgVert);
    GLuint fsBg = compileShader(GL_FRAGMENT_SHADER, bgFrag);
    if (vsBg && fsBg) {
        m_bgProgram = linkProgram(vsBg, fsBg);
        glDeleteShader(vsBg); glDeleteShader(fsBg);
    }

    // 2. Selection Shader
    std::string selVert = R"(#version 300 es
        layout(location = 0) in vec3 aVertex;
        uniform mat4 uMVP;
        uniform vec2 uViewportSize;
        uniform float uOffsetPixels;
        void main() {
            vec4 clipPos = uMVP * vec4(aVertex, 1.0);
            if (uOffsetPixels != 0.0 && clipPos.w > 0.0) {
                float len = length(aVertex.xy);
                if (len > 0.0001) {
                    vec2 dir = aVertex.xy / len;
                    vec2 ndcOffset = (dir * uOffsetPixels * 2.0) / uViewportSize;
                    clipPos.xy += ndcOffset * clipPos.w;
                }
            }
            gl_Position = clipPos;
        }
    )";
    std::string selFrag = R"(#version 300 es
        precision highp float;
        uniform vec3 uColor;
        uniform float uAlpha;
        uniform bool uDashed;
        out vec4 fragColor;
        void main() {
            if (uDashed) {
                float val = gl_FragCoord.x + gl_FragCoord.y;
                if (mod(val, 8.0) > 4.0) {
                    discard;
                }
            }
            fragColor = vec4(uColor, uAlpha);
        }
    )";
    GLuint vsSel = compileShader(GL_VERTEX_SHADER, selVert);
    GLuint fsSel = compileShader(GL_FRAGMENT_SHADER, selFrag);
    if (vsSel && fsSel) {
        m_selectionProgram = linkProgram(vsSel, fsSel);
        glDeleteShader(vsSel); glDeleteShader(fsSel);
    }

    // 3. Reference Image Shader
    std::string refVert = R"(#version 300 es
        layout(location = 0) in vec2 aVertex;
        out vec2 vTexCoord;
        uniform mat4 uMVP;
        uniform bool uPinned2D;
        uniform vec2 uOffset;
        uniform float uScale;
        void main() {
            vTexCoord = aVertex * 0.5 + 0.5;
            if (uPinned2D) {
                vec2 pos = aVertex * uScale;
                pos += uOffset;
                gl_Position = vec4(pos, 0.0, 1.0);
            } else {
                gl_Position = uMVP * vec4(aVertex, 0.0, 1.0);
            }
        }
    )";
    std::string refFrag = R"(#version 300 es
        precision highp float;
        in vec2 vTexCoord;
        out vec4 fragColor;
        uniform sampler2D uTexture;
        uniform float uOpacity;
        void main() {
            vec4 texColor = texture(uTexture, vTexCoord);
            fragColor = vec4(texColor.rgb, texColor.a * uOpacity);
        }
    )";
    GLuint vsRef = compileShader(GL_VERTEX_SHADER, refVert);
    GLuint fsRef = compileShader(GL_FRAGMENT_SHADER, refFrag);
    if (vsRef && fsRef) {
        m_refImageProgram = linkProgram(vsRef, fsRef);
        glDeleteShader(vsRef); glDeleteShader(fsRef);
    }

    // 4. Wireframe Shader
    std::string wfVert = R"(#version 300 es
        layout(location = 0) in vec3 aVertex;
        layout(location = 3) in vec3 aMaterial;
        uniform mat4 uMVP;
        uniform mat4 uEM;
        void main() {
            vec4 vertex4 = vec4(aVertex, 1.0);
            vec4 pos = uMVP * mix(vertex4, uEM * vertex4, aMaterial.z);
            pos.z -= 0.0001; // offset slightly forward
            gl_Position = pos;
        }
    )";
    std::string wfFrag = R"(#version 300 es
        precision highp float;
        out vec4 fragColor;
        void main() {
            fragColor = vec4(0.0, 0.0, 0.0, 0.4);
        }
    )";
    GLuint vsWf = compileShader(GL_VERTEX_SHADER, wfVert);
    GLuint fsWf = compileShader(GL_FRAGMENT_SHADER, wfFrag);
    if (vsWf && fsWf) {
        m_wireframeProgram = linkProgram(vsWf, fsWf);
        glDeleteShader(vsWf); glDeleteShader(fsWf);
    }

    // 5. Flat Shader
    std::string flatVert = R"(#version 300 es
        layout(location = 0) in vec3 aVertex;
        layout(location = 1) in vec3 aNormal;
        layout(location = 2) in vec3 aColor;
        layout(location = 3) in vec3 aMaterial;
        uniform mat4 uMV;
        uniform mat4 uMVP;
        uniform mat3 uN;
        uniform mat4 uEM;
        uniform mat3 uEN;
        out vec3 vVertex;
        out vec3 vNormal;
        out vec3 vColor;
        out float vMasking;
        void main() {
            vColor = aColor;
            vMasking = aMaterial.z;
            vNormal = mix(aNormal, uEN * aNormal, vMasking);
            vNormal = normalize(uN * vNormal);
            vec4 vertex4 = mix(vec4(aVertex, 1.0), uEM * vec4(aVertex, 1.0), vMasking);
            vVertex = vec3(uMV * vertex4);
            gl_Position = uMVP * vertex4;
        }
    )";
    std::string flatFrag = R"(#version 300 es
        precision highp float;
        in vec3 vVertex;
        in vec3 vNormal;
        in vec3 vColor;
        in float vMasking;
        uniform vec3 uAlbedo;
        uniform float uAlpha;
        out vec4 fragColor;
        )" + commonFragCode + R"(
        void main() {
            vec3 color = (uAlbedo.r >= 0.0) ? uAlbedo : vColor;
            fragColor = encodeFragColor(sRGBToLinear(color), uAlpha);
        }
    )";
    GLuint vsFlat = compileShader(GL_VERTEX_SHADER, flatVert);
    GLuint fsFlat = compileShader(GL_FRAGMENT_SHADER, flatFrag);
    if (vsFlat && fsFlat) {
        m_flatProgram = linkProgram(vsFlat, fsFlat);
        glDeleteShader(vsFlat); glDeleteShader(fsFlat);
    }

    // 6. Matcap Shader
    std::string mcVert = R"(#version 300 es
        layout(location = 0) in vec3 aVertex;
        layout(location = 1) in vec3 aNormal;
        layout(location = 2) in vec3 aColor;
        layout(location = 3) in vec3 aMaterial;

        uniform mat4 uMV;
        uniform mat4 uMVP;
        uniform mat3 uN;
        uniform mat4 uEM;
        uniform mat3 uEN;

        out vec3 vVertex;
        out vec3 vNormal;
        out vec3 vColor;
        out float vMasking;
        out vec3 vVertexPres;

        void main() {
            vColor = aColor;
            vMasking = aMaterial.z;
            vNormal = mix(aNormal, uEN * aNormal, vMasking);
            vNormal = normalize(uN * vNormal);
            vec4 vertex4 = vec4(aVertex, 1.0);
            vertex4 = mix(vertex4, uEM * vertex4, vMasking);
            vVertex = vec3(uMV * vertex4);
            vVertexPres = vVertex / max(1.0, abs(uMV[3][2]));
            gl_Position = uMVP * vertex4;
        }
    )";
    std::string mcFrag = R"(#version 300 es
        precision highp float;

        uniform sampler2D uTexture0;
        uniform vec3 uAlbedo;
        uniform float uAlpha;
        uniform int uUseTexture;

        in vec3 vVertex;
        in vec3 vVertexPres;
        in vec3 vNormal;
        in vec3 vColor;
        in float vMasking;

        out vec4 fragColor;
        )" + commonFragCode + R"(
        void main() {
            vec3 normal = getNormal();
            vec3 color;
            if (uUseTexture == 1) {
                vec3 nm_z = normalize(vVertexPres);
                vec3 nm_x = vec3(-nm_z.z, 0.0, nm_z.x);
                vec3 nm_y = cross(nm_x, nm_z);
                vec2 texCoord = 0.5 + 0.5 * vec2(dot(normal, nm_x), dot(normal, nm_y));
                vec3 matcapColor = texture(uTexture0, texCoord).rgb;
                color = matcapColor * (uAlbedo.r >= 0.0 ? uAlbedo : vColor);
                color = sRGBToLinear(color);
            } else {
                vec3 r = reflect(normalize(vVertex), normal);
                float m = 2.0 * sqrt(r.x*r.x + r.y*r.y + (r.z+1.0)*(r.z+1.0));
                float diffuse = max(dot(normal, vec3(0.5, 0.8, 1.0)), 0.0);
                vec3 lightColor = vec3(0.9, 0.85, 0.8) * diffuse + vec3(0.18, 0.18, 0.22);
                vec3 clayColor = (uAlbedo.r >= 0.0) ? uAlbedo : ((vColor.r > 0.0 || vColor.g > 0.0 || vColor.b > 0.0) ? vColor : vec3(0.72, 0.52, 0.45));
                color = clayColor * lightColor;
                float spec = pow(max(dot(r, vec3(0.5, 0.8, 1.0)), 0.0), 16.0);
                color += vec3(0.15) * spec;
                color = sRGBToLinear(color);
            }
            fragColor = encodeFragColor(color, uAlpha);
        }
    )";
    GLuint vsMc = compileShader(GL_VERTEX_SHADER, mcVert);
    GLuint fsMc = compileShader(GL_FRAGMENT_SHADER, mcFrag);
    if (vsMc && fsMc) {
        m_matcapProgram = linkProgram(vsMc, fsMc);
        glDeleteShader(vsMc); glDeleteShader(fsMc);
    }

    // 7. PBR Shader
    std::string pbrVert = R"(#version 300 es
        layout(location = 0) in vec3 aVertex;
        layout(location = 1) in vec3 aNormal;
        layout(location = 2) in vec3 aColor;
        layout(location = 3) in vec3 aMaterial;

        uniform mat4 uMV;
        uniform mat4 uMVP;
        uniform mat3 uN;
        uniform mat4 uEM;
        uniform mat3 uEN;

        out vec3 vVertex;
        out vec3 vNormal;
        out vec3 vColor;
        out vec3 vMaterial;
        out float vMasking;

        void main() {
            vColor = aColor;
            vMaterial = aMaterial;
            vMasking = aMaterial.z;
            vNormal = mix(aNormal, uEN * aNormal, vMasking);
            vNormal = normalize(uN * vNormal);
            vec4 vertex4 = vec4(aVertex, 1.0);
            vertex4 = mix(vertex4, uEM * vertex4, vMasking);
            vVertex = vec3(uMV * vertex4);
            gl_Position = uMVP * vertex4;
        }
    )";
    std::string pbrFrag = R"(#version 300 es
        precision highp float;

        #define LIMIT_LOD 5.0

        uniform sampler2D uTexture0;
        uniform float uExposure;
        uniform mat3 uIblTransform;
        uniform vec3 uSPH[9];
        uniform vec2 uEnvSize;

        uniform vec3 uAlbedo;
        uniform float uRoughness;
        uniform float uMetallic;
        uniform float uAlpha;
        uniform int uUseTexture;

        in vec3 vVertex;
        in vec3 vNormal;
        in vec3 vColor;
        in vec3 vMaterial;
        in float vMasking;

        out vec4 fragColor;
        )" + commonFragCode + R"(

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
        void main() {
            vec3 normal = getNormal();
            float roughness = max(0.0001, (uRoughness >= 0.0 ? uRoughness : vMaterial.x));
            float metallic = (uMetallic >= 0.0 ? uMetallic : vMaterial.y);
            vec3 rawColor = (uAlbedo.r >= 0.0 ? uAlbedo : vColor);
            vec3 linColor = sRGBToLinear(rawColor);
            vec3 albedo = linColor * (1.0 - metallic);
            vec3 specular = mix(vec3(0.04), linColor, metallic);
            vec3 color = vec3(0.0);
            if (uUseTexture == 1) {
                color = uExposure * computeIBL_UE4(normal, -normalize(vVertex), albedo, roughness, specular);
            } else {
                vec3 lightDir = normalize(vec3(0.5, 0.8, 1.0));
                vec3 viewDir = -normalize(vVertex);
                vec3 halfDir = normalize(lightDir + viewDir);
                float NdotL = max(dot(normal, lightDir), 0.0);
                float NdotH = max(dot(normal, halfDir), 0.0);
                vec3 diff = albedo * NdotL * 0.8;
                vec3 specVal = specular * pow(NdotH, 32.0) * 0.5;
                color = diff + specVal + albedo * 0.15;
            }
            fragColor = encodeFragColor(color, uAlpha);
        }
    )";
    GLuint vsPbr = compileShader(GL_VERTEX_SHADER, pbrVert);
    GLuint fsPbr = compileShader(GL_FRAGMENT_SHADER, pbrFrag);
    if (vsPbr && fsPbr) {
        m_pbrProgram = linkProgram(vsPbr, fsPbr);
        glDeleteShader(vsPbr); glDeleteShader(fsPbr);
    }

    // 8. Wet Clay Shader
    std::string wetClayVert = R"(#version 300 es
        layout(location = 0) in vec3 aVertex;
        layout(location = 1) in vec3 aNormal;
        layout(location = 2) in vec3 aColor;
        layout(location = 3) in vec3 aMaterial;
        uniform mat4 uMV;
        uniform mat4 uMVP;
        uniform mat3 uN;
        uniform mat4 uEM;
        uniform mat3 uEN;
        out vec3 vVertex;
        out vec3 vNormal;
        out vec3 vColor;
        out float vMasking;
        out vec3 vObjectPos;
        void main() {
            vColor = aColor;
            vMasking = aMaterial.z;
            vNormal = mix(aNormal, uEN * aNormal, vMasking);
            vNormal = normalize(uN * vNormal);
            vec4 vertex4 = vec4(aVertex, 1.0);
            vertex4 = mix(vertex4, uEM * vertex4, vMasking);
            vVertex = vec3(uMV * vertex4);
            vObjectPos = aVertex;
            gl_Position = uMVP * vertex4;
        }
    )";
    std::string wetClayFrag = R"(#version 300 es
        precision highp float;
        in vec3 vVertex;
        in vec3 vNormal;
        in vec3 vColor;
        in float vMasking;
        in vec3 vObjectPos;
        uniform float uAlpha;
        out vec4 fragColor;
        )" + commonFragCode + wetClayCode + R"(
        void main() {
            vec3 color = computeWetClay(vVertex, vNormal, vColor, vMasking, vObjectPos);
            fragColor = encodeFragColor(color, uAlpha);
        }
    )";
    GLuint vsWet = compileShader(GL_VERTEX_SHADER, wetClayVert);
    GLuint fsWet = compileShader(GL_FRAGMENT_SHADER, wetClayFrag);
    if (vsWet && fsWet) {
        m_wetClayProgram = linkProgram(vsWet, fsWet);
        glDeleteShader(vsWet); glDeleteShader(fsWet);
    }

    // 9. Normal Shader
    std::string normVert = R"(#version 300 es
        layout(location = 0) in vec3 aVertex;
        layout(location = 1) in vec3 aNormal;
        layout(location = 2) in vec3 aColor;
        layout(location = 3) in vec3 aMaterial;
        uniform mat4 uMV;
        uniform mat4 uMVP;
        uniform mat3 uN;
        uniform mat4 uEM;
        uniform mat3 uEN;
        out vec3 vVertex;
        out vec3 vNormal;
        out float vMasking;
        void main() {
            vMasking = aMaterial.z;
            vNormal = mix(aNormal, uEN * aNormal, vMasking);
            vNormal = normalize(uN * vNormal);
            vec4 vertex4 = vec4(aVertex, 1.0);
            vertex4 = mix(vertex4, uEM * vertex4, vMasking);
            vVertex = vec3(uMV * vertex4);
            gl_Position = uMVP * vertex4;
        }
    )";
    std::string normFrag = R"(#version 300 es
        precision highp float;
        in vec3 vVertex;
        in vec3 vNormal;
        in float vMasking;
        uniform float uAlpha;
        out vec4 fragColor;
        )" + commonFragCode + R"(
        void main() {
            vec3 normal = getNormal();
            vec3 col = sRGBToLinear(normal * 0.5 + 0.5);
            fragColor = encodeFragColor(col, uAlpha);
        }
    )";
    GLuint vsNorm = compileShader(GL_VERTEX_SHADER, normVert);
    GLuint fsNorm = compileShader(GL_FRAGMENT_SHADER, normFrag);
    if (vsNorm && fsNorm) {
        m_normalProgram = linkProgram(vsNorm, fsNorm);
        glDeleteShader(vsNorm); glDeleteShader(fsNorm);
    }

    // 10. Voxel Checker Shader
    std::string voxVert = R"(#version 300 es
        layout(location = 0) in vec3 aVertex;
        uniform mat4 uMVP;
        uniform mat4 uMV;
        out vec3 vViewPos;
        void main() {
            vViewPos = vec3(uMV * vec4(aVertex, 1.0));
            gl_Position = uMVP * vec4(aVertex, 1.0);
        }
    )";
    std::string voxFrag = R"(#version 300 es
        precision highp float;
        in vec3 vViewPos;
        uniform float uStep;
        out vec4 fragColor;
        void main() {
            vec2 cell = floor(vViewPos.xy / uStep + 0.0001);
            float cx = mod(abs(cell.x), 2.0);
            float cy = mod(abs(cell.y), 2.0);
            float checker = mod(cx + cy, 2.0);
            vec3 col = (checker > 0.5) ? vec3(0.5) : vec3(0.0);
            float alpha = (checker > 0.5) ? 0.15 : 0.6;
            fragColor = vec4(col, alpha);
        }
    )";
    GLuint vsVox = compileShader(GL_VERTEX_SHADER, voxVert);
    GLuint fsVox = compileShader(GL_FRAGMENT_SHADER, voxFrag);
    if (vsVox && fsVox) {
        m_voxelCheckerProgram = linkProgram(vsVox, fsVox);
        glDeleteShader(vsVox); glDeleteShader(fsVox);
    }

    // 11. Merge Shader
    std::string mergeVert = R"(#version 300 es
        layout(location = 0) in vec2 aVertex;
        out vec2 vTexCoord;
        void main() {
            vTexCoord = aVertex * 0.5 + 0.5;
            gl_Position = vec4(aVertex, 0.5, 1.0);
        }
    )";
    std::string mergeFrag = R"(#version 300 es
        precision highp float;
        in vec2 vTexCoord;
        uniform sampler2D uOpaque;
        uniform sampler2D uTransparent;
        uniform int uFilmic;
        out vec4 fragColor;
        )" + colorspaceCode + R"(
        void main() {
            vec4 transp = texture(uTransparent, vTexCoord);
            vec3 color = decodeRGBM(texture(uOpaque, vTexCoord)) * (1.0 - transp.a) + transp.rgb;
            if (uFilmic == 1) {
                vec3 x = max(vec3(0.0), color - 0.004);
                fragColor = vec4((x * (6.2 * x + 0.5)) / (x * (6.2 * x + 1.7) + 0.06), 1.0);
            } else {
                fragColor = vec4(linearTosRGB(color), 1.0);
            }
        }
    )";
    GLuint vsMerge = compileShader(GL_VERTEX_SHADER, mergeVert);
    GLuint fsMerge = compileShader(GL_FRAGMENT_SHADER, mergeFrag);
    if (vsMerge && fsMerge) {
        m_mergeProgram = linkProgram(vsMerge, fsMerge);
        glDeleteShader(vsMerge); glDeleteShader(fsMerge);
    }

    // 12. FXAA Shader
    std::string fxaaVert = R"(#version 300 es
        layout(location = 0) in vec2 aVertex;
        uniform vec2 uInvSize;
        out vec2 vUVNW;
        out vec2 vUVNE;
        out vec2 vUVSW;
        out vec2 vUVSE;
        out vec2 vUVM;
        void main() {
            vUVM = aVertex * 0.5 + 0.5;
            vUVNW = vUVM + vec2(-1.0, -1.0) * uInvSize;
            vUVNE = vUVM + vec2(1.0, -1.0) * uInvSize;
            vUVSW = vUVM + vec2(-1.0, 1.0) * uInvSize;
            vUVSE = vUVM + vec2(1.0, 1.0) * uInvSize;
            gl_Position = vec4(aVertex, 0.5, 1.0);
        }
    )";
    std::string fxaaFrag = R"(#version 300 es
        precision highp float;
        in vec2 vUVNW;
        in vec2 vUVNE;
        in vec2 vUVSW;
        in vec2 vUVSE;
        in vec2 vUVM;
        uniform sampler2D uTexture0;
        uniform vec2 uInvSize;
        out vec4 fragColor;
        )" + fxaaCode + R"(
        void main() {
            fragColor = vec4(fxaa(uTexture0, vUVNW, vUVNE, vUVSW, vUVSE, vUVM, uInvSize), 1.0);
        }
    )";
    GLuint vsFxaa = compileShader(GL_VERTEX_SHADER, fxaaVert);
    GLuint fsFxaa = compileShader(GL_FRAGMENT_SHADER, fxaaFrag);
    if (vsFxaa && fsFxaa) {
        m_fxaaProgram = linkProgram(vsFxaa, fsFxaa);
        glDeleteShader(vsFxaa); glDeleteShader(fsFxaa);
    }

    // 13. Viewport 2D Blit Shader (combines 2D panning + FXAA)
    std::string vpVert = R"(#version 300 es
        layout(location = 0) in vec2 aVertex;
        uniform vec2 uInvSize;
        uniform vec2 uView2DOffset;
        uniform float uView2DZoom;
        out vec2 vUVNW;
        out vec2 vUVNE;
        out vec2 vUVSW;
        out vec2 vUVSE;
        out vec2 vUVM;
        void main() {
            vec2 ndc = (aVertex - uView2DOffset) / uView2DZoom;
            vUVM = ndc * 0.5 + 0.5;
            vUVNW = vUVM + vec2(-1.0, -1.0) * uInvSize;
            vUVNE = vUVM + vec2(1.0, -1.0) * uInvSize;
            vUVSW = vUVM + vec2(-1.0, 1.0) * uInvSize;
            vUVSE = vUVM + vec2(1.0, 1.0) * uInvSize;
            gl_Position = vec4(aVertex, 0.5, 1.0);
        }
    )";
    GLuint vsVp = compileShader(GL_VERTEX_SHADER, vpVert);
    if (vsVp && fsFxaa) {
        m_viewport2DProgram = linkProgram(vsVp, fsFxaa);
        glDeleteShader(vsVp);
    }

    // 14. Contour outline postprocess shader
    std::string contourVert = R"(#version 300 es
        layout(location = 0) in vec2 aVertex;
        out vec2 vTexCoord;
        void main() {
            vTexCoord = aVertex * 0.5 + 0.5;
            gl_Position = vec4(aVertex, 0.5, 1.0);
        }
    )";
    std::string contourFrag = R"(#version 300 es
        precision highp float;
        in vec2 vTexCoord;
        uniform sampler2D uTexture0;
        uniform vec2 uInvSize;
        uniform vec3 uColor;
        out vec4 fragColor;
        
        float outlineDistance(const in vec2 uv, const in sampler2D tex, const in vec2 invSize) {
          float fac0 = 2.0;
          float fac1 = 1.0;
          float ox = invSize.x;
          float oy = invSize.y;
          vec4 texel0 = texture(tex, uv + vec2(ox, oy));
          vec4 texel1 = texture(tex, uv + vec2(ox, 0.0));
          vec4 texel2 = texture(tex, uv + vec2(ox, -oy));
          vec4 texel3 = texture(tex, uv + vec2(0.0, -oy));
          vec4 texel4 = texture(tex, uv + vec2(-ox, -oy));
          vec4 texel5 = texture(tex, uv + vec2(-ox, 0.0));
          vec4 texel6 = texture(tex, uv + vec2(-ox, oy));
          vec4 texel7 = texture(tex, uv + vec2(0.0, oy));
          vec4 rowx = -fac0 * texel5 + fac0 * texel1 + -fac1 * texel6 + fac1 * texel0 + -fac1 * texel4 + fac1 * texel2;
          vec4 rowy = -fac0 * texel3 + fac0 * texel7 + -fac1 * texel4 + fac1 * texel6 + -fac1 * texel2 + fac1 * texel0;
          return dot(rowy, rowy) + dot(rowx, rowx);
        }

        void main() {
            float val = outlineDistance(vTexCoord, uTexture0, uInvSize);
            if (val < 1.5)
                discard;
            fragColor = vec4(uColor, 1.0);
        }
    )";
    GLuint vsContour = compileShader(GL_VERTEX_SHADER, contourVert);
    GLuint fsContour = compileShader(GL_FRAGMENT_SHADER, contourFrag);
    if (vsContour && fsContour) {
        m_contourProgram = linkProgram(vsContour, fsContour);
        glDeleteShader(vsContour); glDeleteShader(fsContour);
    }

    // C. Initialize static geometry buffers
    // 1. Background quad
    float bgQuad[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f
    };
    glGenVertexArrays(1, &m_bgVao);
    glGenBuffers(1, &m_bgVbo);
    glBindVertexArray(m_bgVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_bgVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(bgQuad), bgQuad, GL_STATIC_DRAW);
    glBindVertexArray(0);

    // 2. Fullscreen quad triangle (Large triangle trick)
    float fsqTriangle[] = {
        -1.0f, -1.0f,
         3.0f, -1.0f,
        -1.0f,  3.0f
    };
    glGenVertexArrays(1, &m_fsqVao);
    glGenBuffers(1, &m_fsqVbo);
    glBindVertexArray(m_fsqVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_fsqVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(fsqTriangle), fsqTriangle, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glBindVertexArray(0);

    // 3. Selection circle geometry (64 points)
    std::vector<float> circleVerts;
    for (int i = 0; i < 64; ++i) {
        float angle = (float)i / 64.0f * 2.0f * M_PI;
        circleVerts.push_back(std::cos(angle));
        circleVerts.push_back(std::sin(angle));
        circleVerts.push_back(0.0f);
    }
    // Selection dot geometry (32 points)
    std::vector<float> dotVerts;
    dotVerts.push_back(0.0f); dotVerts.push_back(0.0f); dotVerts.push_back(0.0f);
    for (int i = 0; i <= 32; ++i) {
        float angle = (float)i / 32.0f * 2.0f * M_PI;
        dotVerts.push_back(std::cos(angle));
        dotVerts.push_back(std::sin(angle));
        dotVerts.push_back(0.0f);
    }

    glGenVertexArrays(1, &m_selectionVao);
    glGenBuffers(1, &m_circleVbo);
    glGenBuffers(1, &m_dotVbo);

    glBindVertexArray(m_selectionVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_circleVbo);
    glBufferData(GL_ARRAY_BUFFER, circleVerts.size() * sizeof(float), circleVerts.data(), GL_STATIC_DRAW);
    
    glBindBuffer(GL_ARRAY_BUFFER, m_dotVbo);
    glBufferData(GL_ARRAY_BUFFER, dotVerts.size() * sizeof(float), dotVerts.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);

    // Lasso buffers initialization
    glGenVertexArrays(1, &m_lassoVao);
    glGenBuffers(1, &m_lassoVbo);
    glBindVertexArray(m_lassoVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_lassoVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glBindVertexArray(0);

    // D. Initialize Grid
    initGrid();

    // E. Initialize environments presets
    initEnvironments();
    loadEnvironmentTexture(0); // Load Mpumalanga veld by default
    initMatcaps();

    // F. Initialize RTT Targets
    m_rttOpaque.init(width, height, true);
    m_rttContour.init(width, height, false);
    m_rttTransparent.init(width, height, true, m_rttOpaque.depth);
    m_rttMerge.init(width, height, false);
    m_rttComposite.init(width, height, false);

    return true;
}

void AngleRenderer::resize(int width, int height) {
    m_width = width;
    m_height = height;
    glViewport(0, 0, width, height);

    m_rttOpaque.resize(width, height);
    m_rttContour.resize(width, height);
    m_rttTransparent.resize(width, height);
    m_rttMerge.resize(width, height);
    m_rttComposite.resize(width, height);
}

void AngleRenderer::setEnvironmentParameters(float exposure, const std::vector<float>& sph) {
    m_exposure = exposure;
    if (sph.size() == 27) {
        std::memcpy(m_sph, sph.data(), 27 * sizeof(float));
    }
}

void AngleRenderer::setEnvironmentParametersFast(float exposure, uintptr_t sphPtr) {
    m_exposure = exposure;
    if (sphPtr) {
        std::memcpy(m_sph, reinterpret_cast<const float*>(sphPtr), 27 * sizeof(float));
    }
}

void AngleRenderer::setSymmetryParameters(bool showSymmetryLine, const std::vector<float>& planeOrigin, const std::vector<float>& planeNormal) {
    m_showSymmetryLine = showSymmetryLine;
    if (planeOrigin.size() == 3) {
        m_planeOrigin = glm::vec3(planeOrigin[0], planeOrigin[1], planeOrigin[2]);
    }
    if (planeNormal.size() == 3) {
        m_planeNormal = glm::normalize(glm::vec3(planeNormal[0], planeNormal[1], planeNormal[2]));
    }
}

void AngleRenderer::setSymmetryParametersFast(bool showSymmetryLine, uintptr_t planeOriginPtr, uintptr_t planeNormalPtr) {
    m_showSymmetryLine = showSymmetryLine;
    if (planeOriginPtr) {
        const float* o = reinterpret_cast<const float*>(planeOriginPtr);
        m_planeOrigin = glm::vec3(o[0], o[1], o[2]);
    }
    if (planeNormalPtr) {
        const float* n = reinterpret_cast<const float*>(planeNormalPtr);
        m_planeNormal = glm::normalize(glm::vec3(n[0], n[1], n[2]));
    }
}

void AngleRenderer::setCursorParameters(
    bool showCursor,
    bool showCircle,
    const std::vector<float>& circleMVP,
    const std::vector<float>& innerCircleMVP,
    const std::vector<float>& dotMVP,
    const std::vector<float>& symMVPs,
    const std::vector<float>& cursorColor
) {
    m_showCursor = showCursor;
    m_showCircle = showCircle;
    if (circleMVP.size() == 16) {
        std::memcpy(&m_circleMVP, circleMVP.data(), 16 * sizeof(float));
    }
    if (innerCircleMVP.size() == 16) {
        std::memcpy(&m_innerCircleMVP, innerCircleMVP.data(), 16 * sizeof(float));
    }
    if (dotMVP.size() == 16) {
        std::memcpy(&m_dotMVP, dotMVP.data(), 16 * sizeof(float));
    }
    if (cursorColor.size() == 3) {
        m_cursorColor = glm::vec3(cursorColor[0], cursorColor[1], cursorColor[2]);
    }
    m_symMVPs.clear();
    m_symMVPs.resize(symMVPs.size() / 16);
    if (!symMVPs.empty()) {
        std::memcpy(m_symMVPs.data(), symMVPs.data(), symMVPs.size() * sizeof(float));
    }
}

void AngleRenderer::setCursorParametersFast(
    bool showCursor,
    bool showCircle,
    uintptr_t circleMVPPtr,
    uintptr_t innerCircleMVPPtr,
    uintptr_t dotMVPPtr,
    uintptr_t symMVPsPtr,
    int symMVPsCount,
    uintptr_t cursorColorPtr
) {
    m_showCursor = showCursor;
    m_showCircle = showCircle;
    if (circleMVPPtr) {
        std::memcpy(&m_circleMVP, reinterpret_cast<const float*>(circleMVPPtr), 16 * sizeof(float));
    }
    if (innerCircleMVPPtr) {
        std::memcpy(&m_innerCircleMVP, reinterpret_cast<const float*>(innerCircleMVPPtr), 16 * sizeof(float));
    }
    if (dotMVPPtr) {
        std::memcpy(&m_dotMVP, reinterpret_cast<const float*>(dotMVPPtr), 16 * sizeof(float));
    }
    if (cursorColorPtr) {
        const float* c = reinterpret_cast<const float*>(cursorColorPtr);
        m_cursorColor = glm::vec3(c[0], c[1], c[2]);
    }
    m_symMVPs.clear();
    if (symMVPsPtr && symMVPsCount > 0) {
        m_symMVPs.resize(symMVPsCount);
        std::memcpy(m_symMVPs.data(), reinterpret_cast<const float*>(symMVPsPtr), symMVPsCount * 16 * sizeof(float));
    }
}

void AngleRenderer::render(const Scene& scene) {
    // 0. Ensure all mesh dirty buffers are uploaded first (must run on the active GL thread/context)
    for (auto* mesh : scene.getMeshes()) {
        uploadIfDirty(mesh);
    }

    // 1. Contour Pass
    glBindFramebuffer(GL_FRAMEBUFFER, m_rttContour.fbo);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    renderScenePass(scene, 0); // 0 = Contour

    // 2. Opaque Pass
    glBindFramebuffer(GL_FRAMEBUFFER, m_rttOpaque.fbo);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    renderScenePass(scene, 3); // 3 = Background & Grid
    renderScenePass(scene, 1); // 1 = Opaque geometry

    // 3. Transparent Pass
    glBindFramebuffer(GL_FRAMEBUFFER, m_rttTransparent.fbo);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT); // Shares depth buffer with opaque pass
    renderScenePass(scene, 2); // 2 = Transparent geometry

    // 4. Merge Pass (FBO Opaque + FBO Transparent -> FBO Merge)
    glBindFramebuffer(GL_FRAMEBUFFER, m_rttMerge.fbo);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    drawFullscreenMerge();

    // 5. FXAA Pass (FBO Merge -> FBO Composite)
    glBindFramebuffer(GL_FRAMEBUFFER, m_rttComposite.fbo);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    drawFullscreenFxaa();

    // 6. Postprocessing Overlays on FBO Composite
    // Reference Images
    drawReferenceImages(scene);

    // Contour Outlines (Sobel filter of the Contour Pass)
    if (m_showContour) {
        if (!m_splitMode) {
            glViewport(0, 0, m_width, m_height);
            drawContourOverlay(scene);
        } else {
            int w2 = m_width / 2;
            glViewport(0, 0, w2, m_height);
            drawContourOverlay(scene);
            glViewport(w2, 0, m_width - w2, m_height);
            drawContourOverlay(scene);
        }
    }

    // Selection Cursor
    drawSelectionCursor();

    // 7. Final Blit to Screen (or Viewport2D Zoom/Pan)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_width, m_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    drawFullscreenViewport2D(scene);

    // Render screen-space Lasso overlay on top of screen
    drawLasso();
}

void AngleRenderer::renderScenePass(const Scene& scene, int passType) {
    if (!m_splitMode) {
        glViewport(0, 0, m_width, m_height);
        glScissor(0, 0, m_width, m_height);
        glEnable(GL_SCISSOR_TEST);
        drawPassGeometry(scene, passType, scene.getCamera());
        glDisable(GL_SCISSOR_TEST);
    } else {
        int w2 = m_width / 2;
        
        // Left camera (main scene camera)
        glViewport(0, 0, w2, m_height);
        glScissor(0, 0, w2, m_height);
        glEnable(GL_SCISSOR_TEST);
        drawPassGeometry(scene, passType, scene.getCamera());

        // Right camera (m_cameraRight)
        if (m_cameraRight) {
            glViewport(w2, 0, m_width - w2, m_height);
            glScissor(w2, 0, m_width - w2, m_height);
            m_cameraRight->onResize(m_width - w2, m_height);
            drawPassGeometry(scene, passType, *m_cameraRight);
        }
        glDisable(GL_SCISSOR_TEST);
    }
}

void AngleRenderer::drawPassGeometry(const Scene& scene, int passType, const Camera& camera) {
    if (passType == 0) {
        // Contour pass
        if (scene.getSelected()) {
            drawMeshFlatColor(scene.getSelected(), scene, camera, glm::vec4(1.0f));
        }
    } else if (passType == 3) {
        // Background and Grid
        if (m_showBackground) {
            drawBackground(scene, camera);
        }
        if (m_showGrid) {
            drawGrid(scene, camera);
        }
    } else if (passType == 1) {
        // Opaque meshes (alpha == 1.0)
        for (auto* mesh : scene.getMeshes()) {
            if (mesh->alpha == 1.0f) {
                drawMeshSolid(mesh, scene, camera);
                if (mesh->showWireframe) {
                    drawWireframe(mesh, scene, camera);
                }
            }
        }
    } else if (passType == 2) {
        // Transparent meshes (alpha < 1.0)
        for (auto* mesh : scene.getMeshes()) {
            if (mesh->alpha < 1.0f) {
                drawMeshSolid(mesh, scene, camera);
                if (mesh->showWireframe) {
                    drawWireframe(mesh, scene, camera);
                }
            }
        }
    }
}

void AngleRenderer::drawBackground(const Scene& scene, const Camera& camera) {
    if (m_bgProgram == 0) return;
    
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glUseProgram(m_bgProgram);

    glUniform1i(glGetUniformLocation(m_bgProgram, "uBackgroundType"), m_backgroundType);
    glUniform1f(glGetUniformLocation(m_bgProgram, "uBlur"), m_bgBlur);
    glUniform1i(glGetUniformLocation(m_bgProgram, "uTexture0"), 0);
    glUniform2f(glGetUniformLocation(m_bgProgram, "uEnvSize"), 1024.0f, 512.0f);
    glUniform3fv(glGetUniformLocation(m_bgProgram, "uSPH"), 9, m_sph);

    glm::mat3 uIblTransform = glm::transpose(glm::mat3(camera.getViewMatrix()));
    glUniformMatrix3fv(glGetUniformLocation(m_bgProgram, "uIblTransform"), 1, GL_FALSE, glm::value_ptr(uIblTransform));

    glActiveTexture(GL_TEXTURE0);
    if (m_envTexture != 0) {
        glBindTexture(GL_TEXTURE_2D, m_envTexture);
    } else {
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    
    GLint locPos = glGetAttribLocation(m_bgProgram, "aVertex");
    glBindVertexArray(m_bgVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_bgVbo);
    glVertexAttribPointer(locPos, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(locPos);
    
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}

void AngleRenderer::drawMesh(Mesh* mesh, const Scene& scene) {
    drawMeshSolid(mesh, scene, scene.getCamera());
    if (mesh->showWireframe) {
        drawWireframe(mesh, scene, scene.getCamera());
    }
}

void AngleRenderer::drawMeshSolid(Mesh* mesh, const Scene& scene, const Camera& camera) {
    auto it = m_meshBuffers.find(mesh);
    if (it == m_meshBuffers.end() || it->second->triIndexCount == 0) return;
    auto& bufs = it->second;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);

    GLuint program = m_flatProgram;
    if (mesh->shaderType == 0) program = m_pbrProgram;
    else if (mesh->shaderType == 1) program = m_matcapProgram;
    else if (mesh->shaderType == 2) program = m_wetClayProgram;
    else if (mesh->shaderType == 3) program = m_normalProgram;
    else if (mesh->shaderType == 4) program = m_voxelCheckerProgram;

    if (program == 0) return;
    glUseProgram(program);

    mesh->updateMatrices(camera);

    glUniformMatrix4fv(glGetUniformLocation(program, "uMV"), 1, GL_FALSE, glm::value_ptr(mesh->mvMatrix));
    glUniformMatrix4fv(glGetUniformLocation(program, "uMVP"), 1, GL_FALSE, glm::value_ptr(mesh->mvpMatrix));
    glUniformMatrix3fv(glGetUniformLocation(program, "uN"), 1, GL_FALSE, glm::value_ptr(mesh->nMatrix));
    glUniformMatrix4fv(glGetUniformLocation(program, "uEM"), 1, GL_FALSE, glm::value_ptr(mesh->editMatrix));
    glUniformMatrix3fv(glGetUniformLocation(program, "uEN"), 1, GL_FALSE, glm::value_ptr(mesh->enMatrix));
    glUniform1f(glGetUniformLocation(program, "uAlpha"), mesh->alpha);
    glUniform3fv(glGetUniformLocation(program, "uAlbedo"), 1, &mesh->albedo[0]);
    glUniform1i(glGetUniformLocation(program, "uFlat"), mesh->flatShading ? 1 : 0);

    glUniform3f(glGetUniformLocation(program, "uPlaneN"), m_planeNormal.x, m_planeNormal.y, m_planeNormal.z);
    glUniform3f(glGetUniformLocation(program, "uPlaneO"), m_planeOrigin.x, m_planeOrigin.y, m_planeOrigin.z);
    glUniform1i(glGetUniformLocation(program, "uSym"), m_showSymmetryLine ? 1 : 0);
    
    bool darken = false;
    if (scene.getMeshes().size() > 1 && scene.getSelected() && scene.getSelected() != mesh) {
        darken = true;
    }
    glUniform1i(glGetUniformLocation(program, "uDarken"), darken ? 1 : 0);
    glUniform1f(glGetUniformLocation(program, "uCurvature"), mesh->curvature);
    glUniform1f(glGetUniformLocation(program, "uFov"), camera.getFov());

    glActiveTexture(GL_TEXTURE0);
    if (mesh->textureId != 0) {
        glBindTexture(GL_TEXTURE_2D, mesh->textureId);
    } else if (mesh->shaderType == 0 && m_envTexture != 0) {
        glBindTexture(GL_TEXTURE_2D, m_envTexture);
    } else if (mesh->shaderType == 1 && mesh->matcapIdx >= 0 && mesh->matcapIdx < static_cast<int>(m_matcaps.size()) && m_matcaps[mesh->matcapIdx].textureId != 0) {
        glBindTexture(GL_TEXTURE_2D, m_matcaps[mesh->matcapIdx].textureId);
    } else {
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    if (mesh->shaderType == 0) {
        glUniform1f(glGetUniformLocation(program, "uRoughness"), mesh->roughness);
        glUniform1f(glGetUniformLocation(program, "uMetallic"), mesh->metallic);
        glUniform1f(glGetUniformLocation(program, "uExposure"), m_exposure);
        glUniform3fv(glGetUniformLocation(program, "uSPH"), 9, m_sph);
        
        glm::mat3 uIblTransform = glm::transpose(glm::mat3(camera.getViewMatrix()));
        glUniformMatrix3fv(glGetUniformLocation(program, "uIblTransform"), 1, GL_FALSE, glm::value_ptr(uIblTransform));
        
        glUniform1i(glGetUniformLocation(program, "uTexture0"), 0);
        glUniform2f(glGetUniformLocation(program, "uEnvSize"), 1024.0f, 512.0f);
        glUniform1i(glGetUniformLocation(program, "uUseTexture"), (mesh->textureId != 0 || m_envTexture != 0) ? 1 : 0);
    } else if (mesh->shaderType == 1) {
        glUniform1i(glGetUniformLocation(program, "uTexture0"), 0);
        bool hasMatcap = (mesh->textureId != 0) || (mesh->matcapIdx >= 0 && mesh->matcapIdx < static_cast<int>(m_matcaps.size()) && m_matcaps[mesh->matcapIdx].textureId != 0);
        glUniform1i(glGetUniformLocation(program, "uUseTexture"), hasMatcap ? 1 : 0);
    } else if (mesh->shaderType == 2) {
        glUniform3f(glGetUniformLocation(program, "uClayColor"), mesh->albedo[0], mesh->albedo[1], mesh->albedo[2]);
        glUniform1f(glGetUniformLocation(program, "uWetness"), 0.5f);
        glUniform1f(glGetUniformLocation(program, "uBumpStrength"), 0.5f);
        glUniform1f(glGetUniformLocation(program, "uNoiseScale"), 0.5f);
        glUniform1f(glGetUniformLocation(program, "uSSSIntensity"), 0.3f);
        glUniform3f(glGetUniformLocation(program, "uSSSColor"), 0.8f, 0.4f, 0.3f);
    } else if (mesh->shaderType == 4) {
        glUniform1f(glGetUniformLocation(program, "uStep"), 0.5f);
    }

    glBindVertexArray(bufs->vao);

    glBindBuffer(GL_ARRAY_BUFFER, bufs->vboVertices);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, bufs->vboNormals);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);

    glBindBuffer(GL_ARRAY_BUFFER, bufs->vboColors);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(2);

    glBindBuffer(GL_ARRAY_BUFFER, bufs->vboMaterials);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(3);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs->eboTriangles);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(bufs->triIndexCount), GL_UNSIGNED_INT, nullptr);

    glBindVertexArray(0);
}

void AngleRenderer::drawMeshFlatColor(Mesh* mesh, const Scene& scene, const Camera& camera, const glm::vec4& color) {
    auto it = m_meshBuffers.find(mesh);
    if (it == m_meshBuffers.end() || it->second->triIndexCount == 0) return;
    auto& bufs = it->second;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);

    if (m_flatProgram == 0) return;
    glUseProgram(m_flatProgram);

    mesh->updateMatrices(camera);

    glUniformMatrix4fv(glGetUniformLocation(m_flatProgram, "uMV"), 1, GL_FALSE, glm::value_ptr(mesh->mvMatrix));
    glUniformMatrix4fv(glGetUniformLocation(m_flatProgram, "uMVP"), 1, GL_FALSE, glm::value_ptr(mesh->mvpMatrix));
    glUniformMatrix3fv(glGetUniformLocation(m_flatProgram, "uN"), 1, GL_FALSE, glm::value_ptr(mesh->nMatrix));
    glUniformMatrix4fv(glGetUniformLocation(m_flatProgram, "uEM"), 1, GL_FALSE, glm::value_ptr(mesh->editMatrix));
    glUniformMatrix3fv(glGetUniformLocation(m_flatProgram, "uEN"), 1, GL_FALSE, glm::value_ptr(mesh->enMatrix));
    glUniform1f(glGetUniformLocation(m_flatProgram, "uAlpha"), color.a);
    glm::vec3 albedo(color.r, color.g, color.b);
    glUniform3fv(glGetUniformLocation(m_flatProgram, "uAlbedo"), 1, &albedo[0]);
    glUniform1i(glGetUniformLocation(m_flatProgram, "uFlat"), 1);

    glUniform1i(glGetUniformLocation(m_flatProgram, "uSym"), 0);
    glUniform1i(glGetUniformLocation(m_flatProgram, "uDarken"), 0);
    glUniform1f(glGetUniformLocation(m_flatProgram, "uCurvature"), 0.0f);
    glUniform1f(glGetUniformLocation(m_flatProgram, "uFov"), camera.getFov());

    glBindVertexArray(bufs->vao);

    glBindBuffer(GL_ARRAY_BUFFER, bufs->vboVertices);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, bufs->vboNormals);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);

    glBindBuffer(GL_ARRAY_BUFFER, bufs->vboColors);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(2);

    glBindBuffer(GL_ARRAY_BUFFER, bufs->vboMaterials);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(3);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs->eboTriangles);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(bufs->triIndexCount), GL_UNSIGNED_INT, nullptr);

    glBindVertexArray(0);
}

void AngleRenderer::drawWireframe(Mesh* mesh, const Scene& scene, const Camera& camera) {
    auto it = m_meshBuffers.find(mesh);
    if (it == m_meshBuffers.end() || it->second->wireIndexCount == 0) return;
    auto& bufs = it->second;

    if (m_wireframeProgram == 0) return;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(m_wireframeProgram);
    
    mesh->updateMatrices(camera);
    glUniformMatrix4fv(glGetUniformLocation(m_wireframeProgram, "uMVP"), 1, GL_FALSE, glm::value_ptr(mesh->mvpMatrix));
    glUniformMatrix4fv(glGetUniformLocation(m_wireframeProgram, "uEM"), 1, GL_FALSE, glm::value_ptr(mesh->editMatrix));

    glBindVertexArray(bufs->vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs->eboWireframe);
    glDrawElements(GL_LINES, static_cast<GLsizei>(bufs->wireIndexCount), GL_UNSIGNED_INT, nullptr);

    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

void AngleRenderer::drawSelectionCursor() {
    if (m_smoothCursor) return; // Drawn via ImGui in GuiManager
    if (!m_showCursor || m_selectionProgram == 0) return;
    
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    glUseProgram(m_selectionProgram);
    GLint locColor = glGetUniformLocation(m_selectionProgram, "uColor");
    GLint locMVP = glGetUniformLocation(m_selectionProgram, "uMVP");
    GLint locPos = glGetAttribLocation(m_selectionProgram, "aVertex");
    
    GLint locAlpha = glGetUniformLocation(m_selectionProgram, "uAlpha");
    GLint locDashed = glGetUniformLocation(m_selectionProgram, "uDashed");
    if (locAlpha != -1) glUniform1f(locAlpha, 1.0f);
    if (locDashed != -1) glUniform1i(locDashed, 0);
    
    glUniform3fv(locColor, 1, &m_cursorColor[0]);
    
    glBindVertexArray(m_selectionVao);
    
    if (m_showCircle) {
        GLint locViewport = glGetUniformLocation(m_selectionProgram, "uViewportSize");
        GLint locOffsetPixels = glGetUniformLocation(m_selectionProgram, "uOffsetPixels");
        
        float viewportWidth = (float)m_width;
        if (m_splitMode) {
            viewportWidth *= 0.5f;
        }
        float viewportHeight = (float)m_height;
        
        if (locViewport != -1) {
            glUniform2f(locViewport, viewportWidth, viewportHeight);
        }

        int passes = static_cast<int>(std::round(m_cursorThickness));
        if (passes < 1) passes = 1;

        // Draw outer circle
        glBindBuffer(GL_ARRAY_BUFFER, m_circleVbo);
        glVertexAttribPointer(locPos, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(locPos);

        for (int i = 0; i < passes; ++i) {
            float offset = 0.0f;
            if (passes > 1) {
                offset = -0.5f * (passes - 1) + i;
            }
            if (locOffsetPixels != -1) glUniform1f(locOffsetPixels, offset);
            
            glUniformMatrix4fv(locMVP, 1, GL_FALSE, &m_circleMVP[0][0]);
            glDrawArrays(GL_LINE_LOOP, 0, 64);
        }
        
        // Draw inner circle
        for (int i = 0; i < passes; ++i) {
            float offset = 0.0f;
            if (passes > 1) {
                offset = -0.5f * (passes - 1) + i;
            }
            if (locOffsetPixels != -1) glUniform1f(locOffsetPixels, offset);

            glUniformMatrix4fv(locMVP, 1, GL_FALSE, &m_innerCircleMVP[0][0]);
            glDrawArrays(GL_LINE_LOOP, 0, 64);
        }

        if (locOffsetPixels != -1) glUniform1f(locOffsetPixels, 0.0f);
    }
    
    // Draw main dot
    glUniformMatrix4fv(locMVP, 1, GL_FALSE, &m_dotMVP[0][0]);
    glBindBuffer(GL_ARRAY_BUFFER, m_dotVbo);
    glVertexAttribPointer(locPos, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(locPos);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 34);
    
    // Draw symmetry dots
    for (const auto& symMVP : m_symMVPs) {
        glUniformMatrix4fv(locMVP, 1, GL_FALSE, &symMVP[0][0]);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 34);
    }
    
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(0);
}

void AngleRenderer::setLassoParameters(bool active, const std::vector<glm::vec2>& points, bool altMode, bool isMaskLasso) {
    m_lassoActive = active;
    m_lassoPoints = points;
    m_lassoAlt = altMode;
    m_isMaskLasso = isMaskLasso;
}

void AngleRenderer::drawLasso() {
    if (!m_lassoActive || m_lassoPoints.size() < 2 || m_selectionProgram == 0) return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Map screen-space points to NDC [-1, 1]
    std::vector<float> ndcPoints;
    ndcPoints.reserve(m_lassoPoints.size() * 3);
    for (const auto& p : m_lassoPoints) {
        float x = (p.x / m_width) * 2.0f - 1.0f;
        float y = 1.0f - (p.y / m_height) * 2.0f;
        ndcPoints.push_back(x);
        ndcPoints.push_back(y);
        ndcPoints.push_back(0.0f);
    }

    glBindVertexArray(m_lassoVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_lassoVbo);
    glBufferData(GL_ARRAY_BUFFER, ndcPoints.size() * sizeof(float), ndcPoints.data(), GL_DYNAMIC_DRAW);

    glUseProgram(m_selectionProgram);
    GLint locColor = glGetUniformLocation(m_selectionProgram, "uColor");
    GLint locMVP = glGetUniformLocation(m_selectionProgram, "uMVP");
    GLint locAlpha = glGetUniformLocation(m_selectionProgram, "uAlpha");
    GLint locDashed = glGetUniformLocation(m_selectionProgram, "uDashed");
    GLint locOffsetPixels = glGetUniformLocation(m_selectionProgram, "uOffsetPixels");
    if (locOffsetPixels != -1) glUniform1f(locOffsetPixels, 0.0f);

    // Colors matching JS project
    glm::vec3 lassoColor;
    if (!m_isMaskLasso) { // Visibility lasso
        if (m_lassoAlt) {
            lassoColor = glm::vec3(1.0f, 0.2f, 0.2f); // #FF3333
        } else {
            lassoColor = glm::vec3(0.0f, 0.902f, 0.463f); // #00E676
        }
    } else { // Mask lasso
        if (m_lassoAlt) {
            lassoColor = glm::vec3(1.0f, 1.0f, 1.0f); // #FFFFFF
        } else {
            lassoColor = glm::vec3(0.0f, 0.898f, 1.0f); // #00E5FF
        }
    }

    glUniform3fv(locColor, 1, &lassoColor[0]);

    glm::mat4 identityMVP(1.0f);
    glUniformMatrix4fv(locMVP, 1, GL_FALSE, &identityMVP[0][0]);

    // 1. Draw fill (opacity 0.15, not dashed)
    if (locAlpha != -1) glUniform1f(locAlpha, 0.15f);
    if (locDashed != -1) glUniform1i(locDashed, 0);
    glDrawArrays(GL_TRIANGLE_FAN, 0, (GLsizei)m_lassoPoints.size());

    // 2. Draw border/stroke (dashed, opacity 1.0)
    if (locAlpha != -1) glUniform1f(locAlpha, 1.0f);
    if (locDashed != -1) glUniform1i(locDashed, 1);
    glLineWidth(2.0f);
    glDrawArrays(GL_LINE_LOOP, 0, (GLsizei)m_lassoPoints.size());
    glLineWidth(1.0f);

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(0);
}


void AngleRenderer::generateTriangleIndices(const Mesh* mesh, std::vector<uint32_t>& outIndices) {
    outIndices.clear();
    outIndices.reserve(mesh->nbFaces * 6);
    const auto& visible = mesh->vertVisible;
    bool hasVisibility = !visible.empty();

    for (int i = 0; i < mesh->nbFaces; ++i) {
        uint32_t iv1 = mesh->faces[i * 4];
        uint32_t iv2 = mesh->faces[i * 4 + 1];
        uint32_t iv3 = mesh->faces[i * 4 + 2];
        uint32_t iv4 = mesh->faces[i * 4 + 3];

        if (hasVisibility) {
            if (iv1 >= visible.size() || !visible[iv1]) continue;
            if (iv2 >= visible.size() || !visible[iv2]) continue;
            if (iv3 >= visible.size() || !visible[iv3]) continue;
            if (iv4 != 0xffffffff && (iv4 >= visible.size() || !visible[iv4])) continue;
        }

        if (iv4 == 0xffffffff) {
            outIndices.push_back(iv1);
            outIndices.push_back(iv2);
            outIndices.push_back(iv3);
        } else {
            outIndices.push_back(iv1);
            outIndices.push_back(iv2);
            outIndices.push_back(iv3);
            outIndices.push_back(iv1);
            outIndices.push_back(iv3);
            outIndices.push_back(iv4);
        }
    }
}

void AngleRenderer::generateWireframeIndices(const Mesh* mesh, std::vector<uint32_t>& outEdges) {
    outEdges.clear();
    outEdges.reserve(mesh->nbFaces * 8);
    const auto& visible = mesh->vertVisible;
    bool hasVisibility = !visible.empty();

    for (int i = 0; i < mesh->nbFaces; ++i) {
        uint32_t iv1 = mesh->faces[i * 4];
        uint32_t iv2 = mesh->faces[i * 4 + 1];
        uint32_t iv3 = mesh->faces[i * 4 + 2];
        uint32_t iv4 = mesh->faces[i * 4 + 3];
        bool isQuad = (iv4 != 0xffffffff);

        if (hasVisibility) {
            if (iv1 >= visible.size() || !visible[iv1]) continue;
            if (iv2 >= visible.size() || !visible[iv2]) continue;
            if (iv3 >= visible.size() || !visible[iv3]) continue;
            if (isQuad && (iv4 >= visible.size() || !visible[iv4])) continue;
        }
        
        outEdges.push_back(iv1);
        outEdges.push_back(iv2);
        outEdges.push_back(iv2);
        outEdges.push_back(iv3);
        if (isQuad) {
            outEdges.push_back(iv3);
            outEdges.push_back(iv4);
            outEdges.push_back(iv4);
            outEdges.push_back(iv1);
        } else {
            outEdges.push_back(iv3);
            outEdges.push_back(iv1);
        }
    }
}

void AngleRenderer::uploadIfDirty(Mesh* mesh) {
    auto& bufs = m_meshBuffers[mesh];
    if (!bufs) {
        bufs = std::make_unique<MeshRenderBuffers>();
        glGenVertexArrays(1, &bufs->vao);
        glGenBuffers(1, &bufs->vboVertices);
        glGenBuffers(1, &bufs->vboNormals);
        glGenBuffers(1, &bufs->vboColors);
        glGenBuffers(1, &bufs->vboMaterials);
        glGenBuffers(1, &bufs->eboTriangles);
        glGenBuffers(1, &bufs->eboWireframe);
        mesh->isDirty = true;
    }
    
    if (mesh->isDirty) {
        glBindVertexArray(bufs->vao);
        
        glBindBuffer(GL_ARRAY_BUFFER, bufs->vboVertices);
        glBufferData(GL_ARRAY_BUFFER, mesh->verts.size() * sizeof(float), mesh->verts.data(), GL_DYNAMIC_DRAW);
        
        glBindBuffer(GL_ARRAY_BUFFER, bufs->vboNormals);
        glBufferData(GL_ARRAY_BUFFER, mesh->normals.size() * sizeof(float), mesh->normals.data(), GL_DYNAMIC_DRAW);
        
        glBindBuffer(GL_ARRAY_BUFFER, bufs->vboColors);
        glBufferData(GL_ARRAY_BUFFER, mesh->colors.size() * sizeof(float), mesh->colors.data(), GL_DYNAMIC_DRAW);
        
        glBindBuffer(GL_ARRAY_BUFFER, bufs->vboMaterials);
        glBufferData(GL_ARRAY_BUFFER, mesh->materials.size() * sizeof(float), mesh->materials.data(), GL_DYNAMIC_DRAW);
        
        std::vector<uint32_t> triIndices;
        generateTriangleIndices(mesh, triIndices);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs->eboTriangles);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, triIndices.size() * sizeof(uint32_t), triIndices.data(), GL_STATIC_DRAW);
        bufs->triIndexCount = triIndices.size();
        
        std::vector<uint32_t> wireIndices;
        generateWireframeIndices(mesh, wireIndices);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs->eboWireframe);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, wireIndices.size() * sizeof(uint32_t), wireIndices.data(), GL_STATIC_DRAW);
        bufs->wireIndexCount = wireIndices.size();
        
        glBindVertexArray(0);
        bufs->vertCount = mesh->nbVerts;
        
        mesh->isDirty = false;
        mesh->isVertexDirty = false;
        mesh->isTopologyDirty = false;
    } else {
        if (mesh->isVertexDirty && mesh->dirtyVertMin <= mesh->dirtyVertMax && mesh->dirtyVertMax < (uint32_t)mesh->nbVerts) {
            size_t offset = mesh->dirtyVertMin * 3 * sizeof(float);
            size_t size   = (mesh->dirtyVertMax - mesh->dirtyVertMin + 1) * 3 * sizeof(float);
            
            glBindBuffer(GL_ARRAY_BUFFER, bufs->vboVertices);
            glBufferSubData(GL_ARRAY_BUFFER, offset, size,
                            mesh->verts.data() + mesh->dirtyVertMin * 3);
            
            glBindBuffer(GL_ARRAY_BUFFER, bufs->vboNormals);
            glBufferSubData(GL_ARRAY_BUFFER, offset, size,
                            mesh->normals.data() + mesh->dirtyVertMin * 3);
            
            glBindBuffer(GL_ARRAY_BUFFER, bufs->vboColors);
            glBufferSubData(GL_ARRAY_BUFFER, offset, size,
                            mesh->colors.data() + mesh->dirtyVertMin * 3);
            
            glBindBuffer(GL_ARRAY_BUFFER, bufs->vboMaterials);
            glBufferSubData(GL_ARRAY_BUFFER, offset, size,
                            mesh->materials.data() + mesh->dirtyVertMin * 3);
            
            mesh->isVertexDirty = false;
        }
        
        if (mesh->isTopologyDirty) {
            std::vector<uint32_t> triIndices;
            generateTriangleIndices(mesh, triIndices);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs->eboTriangles);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, triIndices.size() * sizeof(uint32_t), triIndices.data(), GL_STATIC_DRAW);
            bufs->triIndexCount = triIndices.size();
            
            std::vector<uint32_t> wireIndices;
            generateWireframeIndices(mesh, wireIndices);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs->eboWireframe);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, wireIndices.size() * sizeof(uint32_t), wireIndices.data(), GL_STATIC_DRAW);
            bufs->wireIndexCount = wireIndices.size();
            
            mesh->isTopologyDirty = false;
        }
    }
}

void AngleRenderer::drawReferenceImages(const Scene& scene) {
    const auto& images = scene.getReferenceImages();
    if (images.empty() || m_refImageProgram == 0) return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    glUseProgram(m_refImageProgram);
    glBindVertexArray(m_bgVao);
    
    GLint locPos = glGetAttribLocation(m_refImageProgram, "aVertex");
    glBindBuffer(GL_ARRAY_BUFFER, m_bgVbo);
    glVertexAttribPointer(locPos, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(locPos);

    GLint locMVP = glGetUniformLocation(m_refImageProgram, "uMVP");
    GLint locPinned2D = glGetUniformLocation(m_refImageProgram, "uPinned2D");
    GLint locOffset = glGetUniformLocation(m_refImageProgram, "uOffset");
    GLint locScale = glGetUniformLocation(m_refImageProgram, "uScale");
    GLint locOpacity = glGetUniformLocation(m_refImageProgram, "uOpacity");
    GLint locTexture = glGetUniformLocation(m_refImageProgram, "uTexture");

    glActiveTexture(GL_TEXTURE0);
    glUniform1i(locTexture, 0);

    for (const auto& img : images) {
        if (!img.visible || img.texId == 0) continue;

        glBindTexture(GL_TEXTURE_2D, img.texId);
        glUniform1f(locOpacity, img.opacity);

        if (img.pinned2D) {
            glDisable(GL_DEPTH_TEST);
            glUniform1i(locPinned2D, 1);
            glUniform2f(locOffset, img.offsetX, img.offsetY);
            glUniform1f(locScale, img.scale);
        } else {
            glEnable(GL_DEPTH_TEST);
            glUniform1i(locPinned2D, 0);
            
            glm::mat4 model = glm::mat4(1.0f);
            model = glm::translate(model, glm::vec3(img.offsetX, img.offsetY, 0.0f));
            model = glm::scale(model, glm::vec3(img.scale, img.scale, 1.0f));
            
            glm::mat4 mvp = scene.getCamera().getProjMatrix() * scene.getCamera().getViewMatrix() * model;
            glUniformMatrix4fv(locMVP, 1, GL_FALSE, glm::value_ptr(mvp));
        }

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(0);
}

void AngleRenderer::drawGrid(const Scene& scene, const Camera& camera) {
    if (!m_showGrid || m_selectionProgram == 0 || m_gridLineCount == 0) return;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(m_selectionProgram);
    
    GLint locAlpha = glGetUniformLocation(m_selectionProgram, "uAlpha");
    GLint locDashed = glGetUniformLocation(m_selectionProgram, "uDashed");
    if (locAlpha != -1) glUniform1f(locAlpha, 1.0f);
    if (locDashed != -1) glUniform1i(locDashed, 0);
    GLint locOffsetPixels = glGetUniformLocation(m_selectionProgram, "uOffsetPixels");
    if (locOffsetPixels != -1) glUniform1f(locOffsetPixels, 0.0f);

    glm::vec3 gridColor(0.4f, 0.4f, 0.4f);
    glUniform3fv(glGetUniformLocation(m_selectionProgram, "uColor"), 1, &gridColor[0]);

    glm::mat4 mvp = camera.getProjMatrix() * camera.getViewMatrix();
    glUniformMatrix4fv(glGetUniformLocation(m_selectionProgram, "uMVP"), 1, GL_FALSE, glm::value_ptr(mvp));

    glBindVertexArray(m_gridVao);
    glDrawArrays(GL_LINES, 0, m_gridLineCount);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
}

void AngleRenderer::initGrid() {
    std::vector<float> gridVerts;
    float size = 10.0f;
    float step = 0.5f;
    int lines = 0;
    for (float x = -size; x <= size; x += step) {
        gridVerts.push_back(x); gridVerts.push_back(0.0f); gridVerts.push_back(-size);
        gridVerts.push_back(x); gridVerts.push_back(0.0f); gridVerts.push_back(size);
        lines++;

        gridVerts.push_back(-size); gridVerts.push_back(0.0f); gridVerts.push_back(x);
        gridVerts.push_back(size); gridVerts.push_back(0.0f); gridVerts.push_back(x);
        lines++;
    }

    m_gridLineCount = lines * 2;

    glGenVertexArrays(1, &m_gridVao);
    glGenBuffers(1, &m_gridVbo);

    glBindVertexArray(m_gridVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_gridVbo);
    glBufferData(GL_ARRAY_BUFFER, gridVerts.size() * sizeof(float), gridVerts.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);

    glBindVertexArray(0);
}

void AngleRenderer::drawFullscreenMerge() {
    if (m_mergeProgram == 0) return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glUseProgram(m_mergeProgram);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_rttOpaque.texture);
    glUniform1i(glGetUniformLocation(m_mergeProgram, "uOpaque"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_rttTransparent.texture);
    glUniform1i(glGetUniformLocation(m_mergeProgram, "uTransparent"), 1);

    glUniform1i(glGetUniformLocation(m_mergeProgram, "uFilmic"), m_filmic ? 1 : 0);

    glBindVertexArray(m_fsqVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
}

void AngleRenderer::drawFullscreenFxaa() {
    if (m_fxaaProgram == 0) return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glUseProgram(m_fxaaProgram);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_rttMerge.texture);
    glUniform1i(glGetUniformLocation(m_fxaaProgram, "uTexture0"), 0);

    glm::vec2 invSize(1.0f / m_width, 1.0f / m_height);
    glUniform2fv(glGetUniformLocation(m_fxaaProgram, "uInvSize"), 1, &invSize[0]);

    glBindVertexArray(m_fsqVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
}

void AngleRenderer::drawFullscreenViewport2D(const Scene& scene) {
    if (m_viewport2DProgram == 0) return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glUseProgram(m_viewport2DProgram);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_rttComposite.texture);
    glUniform1i(glGetUniformLocation(m_viewport2DProgram, "uTexture0"), 0);

    glm::vec2 invSize(1.0f / m_width, 1.0f / m_height);
    glUniform2fv(glGetUniformLocation(m_viewport2DProgram, "uInvSize"), 1, &invSize[0]);

    float offset[2] = { scene.getCamera().getView2DOffsetX(), scene.getCamera().getView2DOffsetY() };
    glUniform2fv(glGetUniformLocation(m_viewport2DProgram, "uView2DOffset"), 1, offset);
    glUniform1f(glGetUniformLocation(m_viewport2DProgram, "uView2DZoom"), scene.getCamera().getView2DZoom());

    glBindVertexArray(m_fsqVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);
}

void AngleRenderer::drawContourOverlay(const Scene& scene) {
    if (!m_showContour || m_contourProgram == 0) return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(m_contourProgram);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_rttContour.texture);
    glUniform1i(glGetUniformLocation(m_contourProgram, "uTexture0"), 0);

    glm::vec2 invSize(1.0f / m_width, 1.0f / m_height);
    glUniform2fv(glGetUniformLocation(m_contourProgram, "uInvSize"), 1, &invSize[0]);

    glm::vec3 col(m_contourColor.r, m_contourColor.g, m_contourColor.b);
    glUniform3fv(glGetUniformLocation(m_contourProgram, "uColor"), 1, &col[0]);

    glBindVertexArray(m_fsqVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

void AngleRenderer::initEnvironments() {
    m_environments.clear();

    // 0. Mpumalanga veld
    EnvironmentPreset env0;
    env0.name = "Mpumalanga veld";
    env0.texPath = "mpumalanga_veld_1k.png";
    env0.exposure = 2.5f;
    float sph0[] = {0.136819f, 0.174125f, 0.253762f, 0.027778f, 0.056838f, 0.131221f, 0.074356f, 0.086793f, 0.099181f, -0.079040f, -0.091269f, -0.102346f, -0.027550f, -0.032300f, -0.039217f, 0.031822f, 0.034773f, 0.039945f, 0.017235f, 0.021044f, 0.026136f, -0.106608f, -0.118640f, -0.132761f, 0.041000f, 0.049794f, 0.061183f};
    std::memcpy(env0.sph, sph0, 27 * sizeof(float));
    m_environments.push_back(env0);

    // 1. Venetian crossroads
    EnvironmentPreset env1;
    env1.name = "Venetian crossroads";
    env1.texPath = "venetian_crossroads_1k.png";
    env1.exposure = 2.5f;
    float sph1[] = {0.200626f, 0.198426f, 0.209579f, 0.090452f, 0.127807f, 0.188390f, 0.093188f, 0.103245f, 0.106131f, 0.033349f, 0.054751f, 0.067044f, 0.074350f, 0.081670f, 0.079716f, 0.063127f, 0.085940f, 0.101710f, 0.007751f, 0.005710f, -0.000791f, 0.104134f, 0.103979f, 0.094236f, -0.022747f, -0.028166f, -0.037714f};
    std::memcpy(env1.sph, sph1, 27 * sizeof(float));
    m_environments.push_back(env1);

    // 2. Studio small 01
    EnvironmentPreset env2;
    env2.name = "Studio small 01";
    env2.texPath = "studio_small_01_1k.png";
    env2.exposure = 0.5f;
    float sph2[] = {0.534107f, 0.589985f, 0.617478f, 0.119999f, 0.130480f, 0.128019f, 0.089872f, 0.088707f, 0.088017f, 0.099999f, 0.151282f, 0.138458f, 0.005015f, 0.035588f, 0.027592f, 0.114999f, 0.116739f, 0.120579f, -0.057997f, -0.069532f, -0.070401f, 0.385123f, 0.411714f, 0.454725f, 0.303242f, 0.333004f, 0.350270f};
    std::memcpy(env2.sph, sph2, 27 * sizeof(float));
    m_environments.push_back(env2);

    // 3. Moonless golf
    EnvironmentPreset env3;
    env3.name = "Moonless golf";
    env3.texPath = "moonless_golf_1k.png";
    env3.exposure = 1.0f;
    float sph3[] = {0.137579f, 0.112906f, 0.093470f, 0.070711f, 0.066043f, 0.065337f, -0.029564f, -0.020720f, -0.007737f, -0.037254f, -0.033270f, -0.028294f, -0.023847f, -0.021208f, -0.018767f, -0.007873f, -0.002587f, 0.003955f, 0.009241f, 0.007711f, 0.006063f, 0.017917f, 0.011733f, 0.007669f, 0.036859f, 0.026285f, 0.014740f};
    std::memcpy(env3.sph, sph3, 27 * sizeof(float));
    m_environments.push_back(env3);

    // 4. Winter river
    EnvironmentPreset env4;
    env4.name = "Winter river";
    env4.texPath = "winter_river_1k.png";
    env4.exposure = 0.5f;
    float sph4[] = {0.560145f, 0.554695f, 0.513523f, -0.213105f, -0.155190f, -0.063568f, 0.135182f, 0.114211f, 0.069349f, 0.172852f, 0.151820f, 0.105477f, 0.065753f, 0.064050f, 0.052622f, 0.096352f, 0.086557f, 0.063826f, 0.021830f, 0.016560f, 0.008804f, 0.186193f, 0.163720f, 0.119627f, 0.025363f, 0.022278f, 0.014461f};
    std::memcpy(env4.sph, sph4, 27 * sizeof(float));
    m_environments.push_back(env4);
}

void AngleRenderer::loadEnvironmentTexture(int idx) {
    if (idx < 0 || idx >= static_cast<int>(m_environments.size())) return;
    m_currentEnvIdx = idx;
    const auto& env = m_environments[idx];

    std::vector<std::string> searchPaths = {
        "resources/environments/" + env.texPath,
        "dist/resources/environments/" + env.texPath,
        "../dist/resources/environments/" + env.texPath
    };

    int width = 0, height = 0, channels = 0;
    unsigned char* data = nullptr;
    std::string loadedPath;

    stbi_set_flip_vertically_on_load(false);
    for (const auto& path : searchPaths) {
        data = stbi_load(path.c_str(), &width, &height, &channels, 4);
        if (data) {
            loadedPath = path;
            break;
        }
    }

    if (!data) {
        std::cerr << "Failed to load environment map: " << env.texPath << std::endl;
        return;
    }

    if (m_envTexture) {
        glDeleteTextures(1, &m_envTexture);
        m_envTexture = 0;
    }

    glGenTextures(1, &m_envTexture);
    glBindTexture(GL_TEXTURE_2D, m_envTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);

    std::cout << "Successfully loaded environment map: " << loadedPath 
              << " (" << width << "x" << height << ")" << std::endl;

    m_exposure = env.exposure;
    std::memcpy(m_sph, env.sph, 27 * sizeof(float));
}

void AngleRenderer::setEnvironmentPreset(int idx) {
    if (idx < 0 || idx >= static_cast<int>(m_environments.size())) return;
    loadEnvironmentTexture(idx);
}

std::vector<uint8_t> AngleRenderer::renderToBuffer(const Scene& scene, int w, int h) {
    int oldW = m_width;
    int oldH = m_height;
    
    resize(w, h);
    render(scene);

    std::vector<uint8_t> buffer(w * h * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, m_rttComposite.fbo);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buffer.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    resize(oldW, oldH);

    return buffer;
}

void AngleRenderer::initMatcaps() {
    m_matcaps.clear();

    struct TempPreset {
        std::string name;
        std::string texPath;
    };
    std::vector<TempPreset> temps = {
        { "Matcap FV", "matcapFV.jpg" },
        { "Red clay", "redClay.jpg" },
        { "Skin hazardousarts", "skinHazardousarts.jpg" },
        { "Skin Hazardousarts2", "skinHazardousarts2.jpg" },
        { "Pearl", "pearl.jpg" },
        { "Clay", "clay.jpg" },
        { "Skin", "skin.jpg" },
        { "Green", "green.jpg" },
        { "White", "white.jpg" }
    };

    for (const auto& t : temps) {
        MatcapPreset preset;
        preset.name = t.name;
        preset.texPath = t.texPath;
        preset.textureId = 0;
        
        std::vector<std::string> searchPaths = {
            "resources/matcaps/" + t.texPath,
            "dist/resources/matcaps/" + t.texPath,
            "../dist/resources/matcaps/" + t.texPath
        };

        int width = 0, height = 0, channels = 0;
        unsigned char* data = nullptr;
        std::string loadedPath;

        stbi_set_flip_vertically_on_load(false);
        for (const auto& path : searchPaths) {
            data = stbi_load(path.c_str(), &width, &height, &channels, 4);
            if (data) {
                loadedPath = path;
                break;
            }
        }

        if (data) {
            glGenTextures(1, &preset.textureId);
            glBindTexture(GL_TEXTURE_2D, preset.textureId);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
            
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            
            glBindTexture(GL_TEXTURE_2D, 0);
            stbi_image_free(data);

            std::cout << "Successfully loaded matcap map: " << loadedPath 
                      << " (" << width << "x" << height << ")" << std::endl;
        } else {
            std::cerr << "Failed to load matcap map: " << t.texPath << std::endl;
        }

        m_matcaps.push_back(preset);
    }
}

GLint AngleRenderer::getCachedUniformLocation(GLuint program, const char* name) {
    auto& programMap = m_uniformLocations[program];
    std::string nameStr(name);
    auto it = programMap.find(nameStr);
    if (it != programMap.end()) {
        return it->second;
    }
    GLint loc = (::glGetUniformLocation)(program, name);
    programMap[nameStr] = loc;
    return loc;
}
