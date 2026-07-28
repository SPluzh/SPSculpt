#include "render/AngleRenderer.h"
#include <iostream>
#include <random>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "scene/Scene.h"
#include "mesh/Mesh.h"
#include "sculpt/ArmatureGraph.h"
#include "common/Logger.h"
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
    if (m_polygroupProgram) glDeleteProgram(m_polygroupProgram);
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
    if (m_bevelPrepassProgram) glDeleteProgram(m_bevelPrepassProgram);
    if (m_bevelFilterProgram) glDeleteProgram(m_bevelFilterProgram);
    if (m_ssaoNormalsProgram) glDeleteProgram(m_ssaoNormalsProgram);
    if (m_ssaoProgram) glDeleteProgram(m_ssaoProgram);
    if (m_ssaoBlurProgram) glDeleteProgram(m_ssaoBlurProgram);
    if (m_armatureProgram) glDeleteProgram(m_armatureProgram);
    if (m_armatureNormalsProgram) glDeleteProgram(m_armatureNormalsProgram);
    if (m_shadowProgram) glDeleteProgram(m_shadowProgram);
    if (m_ssrProgram) glDeleteProgram(m_ssrProgram);
    if (m_gbufferProgram) glDeleteProgram(m_gbufferProgram);
    if (m_ssptProgram) glDeleteProgram(m_ssptProgram);
    if (m_svgfTemporalProgram) glDeleteProgram(m_svgfTemporalProgram);
    if (m_svgfSpatialProgram) glDeleteProgram(m_svgfSpatialProgram);

    if (m_bgVao) glDeleteVertexArrays(1, &m_bgVao);
    if (m_bgVbo) glDeleteBuffers(1, &m_bgVbo);
    if (m_bgTexCoordVbo) glDeleteBuffers(1, &m_bgTexCoordVbo);
    if (m_bgTexture) glDeleteTextures(1, &m_bgTexture);
    if (m_bgMonoTexture) glDeleteTextures(1, &m_bgMonoTexture);
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
    if (m_ssaoNoiseTexture) glDeleteTextures(1, &m_ssaoNoiseTexture);

    if (m_armatureSphereVao) glDeleteVertexArrays(1, &m_armatureSphereVao);
    if (m_armatureSphereVbo) glDeleteBuffers(1, &m_armatureSphereVbo);
    if (m_armatureSphereEbo) glDeleteBuffers(1, &m_armatureSphereEbo);

    if (m_armatureCylVao) glDeleteVertexArrays(1, &m_armatureCylVao);
    if (m_armatureCylVbo) glDeleteBuffers(1, &m_armatureCylVbo);
    if (m_armatureCylEbo) glDeleteBuffers(1, &m_armatureCylEbo);

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
    m_rttPrepass.release();
    m_rttBevel.release();
    m_rttNormals.release();
    m_rttSsao.release();
    m_rttSsaoBlur.release();
    m_rttShadow.release();
    m_rttSsr.release();
    m_rttAccumA.release();
    m_rttAccumB.release();
    m_rttSvgfA.release();
    m_rttSvgfB.release();
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

std::string AngleRenderer::loadShaderSource(const std::string& filename) {
    std::vector<std::string> searchPaths = {
        "resources/shaders/" + filename,
        "../resources/shaders/" + filename,
        "../../resources/shaders/" + filename,
        "dist/resources/shaders/" + filename,
        "../dist/resources/shaders/" + filename,
        "../../dist/resources/shaders/" + filename
    };
    std::string path;
    for (const auto& p : searchPaths) {
        if (std::filesystem::exists(p)) {
            path = p;
            break;
        }
    }
    if (path.empty()) {
        return "";
    }
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::string source;
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("#include \"", 0) == 0) {
            size_t start = line.find("\"") + 1;
            size_t end = line.find("\"", start);
            std::string incFile = line.substr(start, end - start);
            std::string incSource = loadShaderSource(incFile);
            if (incSource.empty()) {
                std::cerr << "Failed to include file: " << incFile << " in " << filename << std::endl;
            }
            source += incSource + "\n";
        } else {
            source += line + "\n";
        }
    }
    return source;
}


GLuint AngleRenderer::loadAndCompileProgram(const std::string& vertFile, const std::string& fragFile) {
    std::string vertSrc = loadShaderSource(vertFile);
    std::string fragSrc = loadShaderSource(fragFile);
    if (vertSrc.empty() || fragSrc.empty()) {
        std::cerr << "Failed to read shader source: " << vertFile << " or " << fragFile << std::endl;
        return 0;
    }
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = linkProgram(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}


bool AngleRenderer::init(int width, int height) {
    m_width = width;
    m_height = height;

    // Compile Shaders from files
    m_bgProgram = loadAndCompileProgram("background.vert", "background.frag");
    m_selectionProgram = loadAndCompileProgram("selection.vert", "selection.frag");
    m_refImageProgram = loadAndCompileProgram("reference_image.vert", "reference_image.frag");
    m_wireframeProgram = loadAndCompileProgram("wireframe.vert", "wireframe.frag");
    m_flatProgram = loadAndCompileProgram("flat.vert", "flat.frag");
    m_matcapProgram = loadAndCompileProgram("matcap.vert", "matcap.frag");
    m_polygroupProgram = loadAndCompileProgram("polygroup.vert", "polygroup.frag");
    m_pbrProgram = loadAndCompileProgram("pbr.vert", "pbr.frag");
    m_wetClayProgram = loadAndCompileProgram("wet_clay.vert", "wet_clay.frag");
    m_normalProgram = loadAndCompileProgram("normal.vert", "normal.frag");
    m_voxelCheckerProgram = loadAndCompileProgram("voxel_checker.vert", "voxel_checker.frag");
    m_mergeProgram = loadAndCompileProgram("merge.vert", "merge.frag");
    m_fxaaProgram = loadAndCompileProgram("fxaa.vert", "fxaa.frag");
    m_viewport2DProgram = loadAndCompileProgram("viewport2d.vert", "fxaa.frag");
    m_contourProgram = loadAndCompileProgram("contour.vert", "contour.frag");
    m_bevelPrepassProgram = loadAndCompileProgram("bevel_prepass.vert", "bevel_prepass.frag");
    m_bevelFilterProgram = loadAndCompileProgram("bevel.vert", "bevel.frag");

    m_armatureProgram = loadAndCompileProgram("armature.vert", "armature.frag");
    m_armatureNormalsProgram = loadAndCompileProgram("armature.vert", "armature_normals.frag");
    m_ssaoNormalsProgram = loadAndCompileProgram("normal.vert", "ssao_normals.frag");
    m_ssaoProgram = loadAndCompileProgram("merge.vert", "ssao.frag");
    m_ssaoBlurProgram = loadAndCompileProgram("merge.vert", "ssao_blur.frag");
    m_shadowProgram = loadAndCompileProgram("shadow.vert", "shadow.frag");
    m_ssrProgram = loadAndCompileProgram("merge.vert", "ssr.frag");
    m_gbufferProgram = loadAndCompileProgram("gbuffer.vert", "gbuffer.frag");
    m_ssptProgram = loadAndCompileProgram("merge.vert", "sspt.frag");
    m_svgfTemporalProgram = loadAndCompileProgram("merge.vert", "svgf_temporal.frag");
    m_svgfSpatialProgram = loadAndCompileProgram("merge.vert", "svgf_spatial.frag");

    // Check if critical shader programs failed to load/link
    if (!m_bgProgram || !m_selectionProgram || !m_refImageProgram || !m_wireframeProgram ||
        !m_flatProgram || !m_matcapProgram || !m_pbrProgram || !m_wetClayProgram ||
        !m_normalProgram || !m_voxelCheckerProgram || !m_mergeProgram || !m_fxaaProgram ||
        !m_viewport2DProgram || !m_contourProgram || !m_bevelPrepassProgram || !m_bevelFilterProgram ||
        !m_ssaoNormalsProgram || !m_ssaoProgram || !m_ssaoBlurProgram || !m_armatureProgram || !m_armatureNormalsProgram ||
        !m_shadowProgram || !m_ssrProgram) {
        std::cerr << "Error: One or more shader programs failed to compile and link." << std::endl;
        return false;
    }

    // C. Initialize static geometry buffers
    // 1. Background quad
    glGenVertexArrays(1, &m_bgVao);
    glGenBuffers(1, &m_bgVbo);
    glBindVertexArray(m_bgVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_bgVbo);
    // Buffer initialized dynamically in updateBackgroundGeometry
    
    // Texture coordinates buffer
    float bgTexCoords[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f
    };
    glGenBuffers(1, &m_bgTexCoordVbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_bgTexCoordVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(bgTexCoords), bgTexCoords, GL_STATIC_DRAW);
    glBindVertexArray(0);

    // Initialize 1x1 grey background mono texture
    glGenTextures(1, &m_bgMonoTexture);
    glBindTexture(GL_TEXTURE_2D, m_bgMonoTexture);
    unsigned char greyPixel[] = { 50, 50, 50, 255 };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, greyPixel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    updateBackgroundGeometry();

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
    initArmatureGeometry();
    initSsaoKernel();
    initSsaoNoiseTexture();

    // E. Initialize environments presets
    initEnvironments();
    loadEnvironmentTexture(0); // Load Mpumalanga veld by default
    initMatcaps();

    // F. Initialize RTT Targets
    m_rttOpaque.init(width, height, true, 0, true);
    m_rttContour.init(width, height, false);
    m_rttTransparent.init(width, height, true, m_rttOpaque.depth);
    m_rttMerge.init(width, height, false);
    m_rttComposite.init(width, height, true, m_rttOpaque.depth);
    m_rttPrepass.init(width, height, true, 0, true);
    m_rttBevel.init(width, height, false);
    m_rttNormals.init(width, height, true, 0, false);
    m_rttSsao.init(width, height, false);
    m_rttSsaoBlur.init(width, height, false);
    m_rttShadow.initDepthOnly(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
    m_rttSsr.init(width, height, false);
    m_rttAccumA.initFloat(width, height, false);
    m_rttAccumB.initFloat(width, height, false);
    m_rttSvgfA.initFloat(width, height, false);
    m_rttSvgfB.initFloat(width, height, false);

    return true;
}

void AngleRenderer::resize(int width, int height, float dpiScale) {
    m_width = width;
    m_height = height;
    m_dpiScale = dpiScale;
    glViewport(0, 0, width, height);

    m_rttOpaque.resize(width, height);
    m_rttContour.resize(width, height);
    m_rttTransparent.resize(width, height);
    m_rttMerge.resize(width, height);
    m_rttComposite.resize(width, height);
    m_rttPrepass.resize(width, height);
    m_rttBevel.resize(width, height);
    m_rttNormals.resize(width, height);
    m_rttSsao.resize(width, height);
    m_rttSsaoBlur.resize(width, height);
    m_rttSsr.resize(width, height);
    m_rttAccumA.resize(width, height);
    m_rttAccumB.resize(width, height);
    m_rttSvgfA.resize(width, height);
    m_rttSvgfB.resize(width, height);

    updateBackgroundGeometry();
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
    uintptr_t cursorColorPtr,
    uintptr_t symOccludedPtr,
    bool isScreenspace
) {
    m_showCursor = showCursor;
    m_showCircle = showCircle;
    m_cursorIsScreenspace = isScreenspace;
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
    m_symOccluded.clear();
    if (symMVPsPtr && symMVPsCount > 0) {
        m_symMVPs.resize(symMVPsCount);
        std::memcpy(m_symMVPs.data(), reinterpret_cast<const float*>(symMVPsPtr), symMVPsCount * 16 * sizeof(float));

        m_symOccluded.resize(symMVPsCount);
        if (symOccludedPtr) {
            std::memcpy(m_symOccluded.data(), reinterpret_cast<const char*>(symOccludedPtr), symMVPsCount * sizeof(char));
        } else {
            std::fill(m_symOccluded.begin(), m_symOccluded.end(), 0);
        }
    }
}

void AngleRenderer::setCursorParametersRightFast(
    uintptr_t circleMVPPtr,
    uintptr_t innerCircleMVPPtr,
    uintptr_t dotMVPPtr,
    uintptr_t symMVPsPtr,
    int symMVPsCount,
    uintptr_t symOccludedPtr
) {
    if (circleMVPPtr) {
        std::memcpy(&m_circleMVPRight, reinterpret_cast<const float*>(circleMVPPtr), 16 * sizeof(float));
    }
    if (innerCircleMVPPtr) {
        std::memcpy(&m_innerCircleMVPRight, reinterpret_cast<const float*>(innerCircleMVPPtr), 16 * sizeof(float));
    }
    if (dotMVPPtr) {
        std::memcpy(&m_dotMVPRight, reinterpret_cast<const float*>(dotMVPPtr), 16 * sizeof(float));
    }
    m_symMVPsRight.clear();
    m_symOccludedRight.clear();
    if (symMVPsPtr && symMVPsCount > 0) {
        m_symMVPsRight.resize(symMVPsCount);
        std::memcpy(m_symMVPsRight.data(), reinterpret_cast<const float*>(symMVPsPtr), symMVPsCount * 16 * sizeof(float));

        m_symOccludedRight.resize(symMVPsCount);
        if (symOccludedPtr) {
            std::memcpy(m_symOccludedRight.data(), reinterpret_cast<const char*>(symOccludedPtr), symMVPsCount * sizeof(char));
        } else {
            std::fill(m_symOccludedRight.begin(), m_symOccludedRight.end(), 0);
        }
    }
}

void AngleRenderer::render(const Scene& scene, unsigned int targetFbo) {
    // Sync split mode and right camera from scene
    m_splitMode = m_isTakingScreenshot ? false : (scene.getSplitMode() != Scene::SplitMode::OFF);
    m_cameraRight = scene.getCameraRight();

    // 0. Ensure all mesh dirty buffers are uploaded first (must run on the active GL thread/context)
    for (auto* mesh : scene.getMeshes()) {
        uploadIfDirty(mesh);
    }

    // Whether the active shader actually consumes shadow/SSAO/SSR data.
    // Matcap (1), WetClay (2), Normal (3), VoxelChecker (4), PolyGroup (5) do NOT.
    const bool needsLightingPasses = (m_shaderType == 0 && m_renderMode == RenderMode::PBR) || m_showPolyGroups;

    // 0a. Shadow Map Pass
    if (m_shadowEnabled && needsLightingPasses) {
        int shadowLightIdx = -1;
        const auto& lights = scene.getLights();
        for (int i = 0; i < (int)lights.size(); ++i) {
            if (lights[i].enabled && lights[i].castShadow && lights[i].type == LightType::DIRECTIONAL) {
                shadowLightIdx = i;
                break;
            }
        }
        if (shadowLightIdx >= 0) {
            glm::vec3 lightDir = glm::normalize(lights[shadowLightIdx].direction);
            glm::mat4 lightView = glm::lookAt(-lightDir * 50.0f, glm::vec3(0.0f), glm::vec3(0, 1, 0));
            glm::mat4 lightProj = glm::ortho(-30.0f, 30.0f, -30.0f, 30.0f, 0.1f, 200.0f);
            m_shadowLightMVP = lightProj * lightView;

            glBindFramebuffer(GL_FRAMEBUFFER, m_rttShadow.fbo);
            glViewport(0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
            glClear(GL_DEPTH_BUFFER_BIT);
            glEnable(GL_DEPTH_TEST);

            if (m_shadowProgram) {
                glUseProgram(m_shadowProgram);
                for (auto* mesh : scene.getMeshes()) {
                    if (scene.isMeshRenderVisible(mesh, 0)) {
                        mesh->updateMatrices(scene.getCamera());
                        glm::mat4 mvp = m_shadowLightMVP * mesh->matrix;
                        glUniformMatrix4fv(glGetUniformLocation(m_shadowProgram, "uLightMVP"), 1, GL_FALSE, glm::value_ptr(mvp));
                        glUniformMatrix4fv(glGetUniformLocation(m_shadowProgram, "uEM"), 1, GL_FALSE, glm::value_ptr(mesh->editMatrix));
                        auto it = m_meshBuffers.find(mesh);
                        if (it != m_meshBuffers.end() && it->second->triIndexCount > 0) {
                            glBindVertexArray(it->second->vao);
                            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, it->second->eboTriangles);
                            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(it->second->triIndexCount), GL_UNSIGNED_INT, nullptr);
                            glBindVertexArray(0);
                        }
                    }
                }
            }
        }
    }

    // 0b. Bevel Pre-pass and Filtering
    if (m_bevelEnabled) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_rttPrepass.fbo);
        glViewport(0, 0, m_width, m_height);
        glClearColor(0.5f, 0.5f, 0.5f, 1.0f); // Default normal is (0,0,1) -> encoded as (0.5,0.5,1.0)
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderScenePass(scene, 4); // 4 = Bevel prepass
        
        glBindFramebuffer(GL_FRAMEBUFFER, m_rttBevel.fbo);
        glViewport(0, 0, m_width, m_height);
        glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glUseProgram(m_bevelFilterProgram);
        
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_rttPrepass.texture);
        glUniform1i(glGetUniformLocation(m_bevelFilterProgram, "uNormalMap"), 0);
        
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_rttPrepass.depth);
        glUniform1i(glGetUniformLocation(m_bevelFilterProgram, "uDepthMap"), 1);
        
        glUniform2f(glGetUniformLocation(m_bevelFilterProgram, "uInvViewportSize"), 1.0f / m_width, 1.0f / m_height);
        glUniform1f(glGetUniformLocation(m_bevelFilterProgram, "uBevelRadius"), m_bevelRadius);
        glUniform1f(glGetUniformLocation(m_bevelFilterProgram, "uBevelStrength"), m_bevelStrength);

        const Camera* camLeft = scene.getCameraByIndex(0);
        if (!camLeft) camLeft = &scene.getCamera();
        const Camera* camRight = scene.getCameraByIndex(1);

        float nearVals[2] = { 0.05f, 0.05f };
        float farVals[2] = { 5000.0f, 5000.0f };
        float targetDists[2] = { 100.0f, 100.0f };
        int projTypes[2] = { 0, 0 };
        float orthoZooms[2] = { 0.044f, 0.044f };
        float fovs[2] = { 45.0f, 45.0f };
        float vpHeights[2] = { (float)m_height, (float)m_height };

        if (camLeft) {
            nearVals[0] = camLeft->getNear();
            farVals[0] = camLeft->getFar();
            targetDists[0] = glm::distance(camLeft->computePosition(), camLeft->getPivot());
            projTypes[0] = (camLeft->getProjectionType() == CameraEnums::Projection::ORTHOGRAPHIC) ? 1 : 0;
            orthoZooms[0] = camLeft->getOrthoZoom();
            fovs[0] = camLeft->getFov();
            vpHeights[0] = (float)m_height;
        }
        if (camRight) {
            nearVals[1] = camRight->getNear();
            farVals[1] = camRight->getFar();
            targetDists[1] = glm::distance(camRight->computePosition(), camRight->getPivot());
            projTypes[1] = (camRight->getProjectionType() == CameraEnums::Projection::ORTHOGRAPHIC) ? 1 : 0;
            orthoZooms[1] = camRight->getOrthoZoom();
            fovs[1] = camRight->getFov();
            vpHeights[1] = (float)m_height;
        } else {
            nearVals[1] = nearVals[0];
            farVals[1] = farVals[0];
            targetDists[1] = targetDists[0];
            projTypes[1] = projTypes[0];
            orthoZooms[1] = orthoZooms[0];
            fovs[1] = fovs[0];
            vpHeights[1] = vpHeights[0];
        }

        glUniform1fv(glGetUniformLocation(m_bevelFilterProgram, "uNear"), 2, nearVals);
        glUniform1fv(glGetUniformLocation(m_bevelFilterProgram, "uFar"), 2, farVals);
        glUniform1fv(glGetUniformLocation(m_bevelFilterProgram, "uTargetDistance"), 2, targetDists);
        glUniform1iv(glGetUniformLocation(m_bevelFilterProgram, "uProjType"), 2, projTypes);
        glUniform1fv(glGetUniformLocation(m_bevelFilterProgram, "uOrthoZoom"), 2, orthoZooms);
        glUniform1fv(glGetUniformLocation(m_bevelFilterProgram, "uFov"), 2, fovs);
        glUniform1fv(glGetUniformLocation(m_bevelFilterProgram, "uViewportHeight"), 2, vpHeights);
        glUniform1i(glGetUniformLocation(m_bevelFilterProgram, "uSplitMode"), m_splitMode ? 1 : 0);
        glUniform1i(glGetUniformLocation(m_bevelFilterProgram, "uBevelScaleWithDistance"), m_bevelScaleWithDistance ? 1 : 0);
        
        glBindVertexArray(m_fsqVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        
        glEnable(GL_DEPTH_TEST);
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

    // 2b. SSAO Pass
    if (m_useSsao && needsLightingPasses) {
        // A. Normals pre-pass
        glBindFramebuffer(GL_FRAMEBUFFER, m_rttNormals.fbo);
        glViewport(0, 0, m_width, m_height);
        glClearColor(0.5f, 0.5f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderScenePass(scene, 5); // 5 = Normals pre-pass
        
        // B. SSAO Calculation Pass
        glBindFramebuffer(GL_FRAMEBUFFER, m_rttSsao.fbo);
        glViewport(0, 0, m_width, m_height);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glUseProgram(m_ssaoProgram);
        
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_rttOpaque.depth);
        glUniform1i(glGetUniformLocation(m_ssaoProgram, "uDepthTex"), 0);
        
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_rttNormals.texture);
        glUniform1i(glGetUniformLocation(m_ssaoProgram, "uNormalsTex"), 1);
        
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_ssaoNoiseTexture);
        glUniform1i(glGetUniformLocation(m_ssaoProgram, "uNoiseTex"), 2);
        
        const Camera* camLeft = scene.getCameraByIndex(0);
        if (!camLeft) camLeft = &scene.getCamera();
        const Camera* camRight = scene.getCameraByIndex(1);
        if (!camRight) camRight = camLeft;
        
        glm::mat4 projection[2];
        projection[0] = camLeft->getProjMatrix();
        projection[1] = camRight->getProjMatrix();
        
        glm::mat4 invProjection[2];
        invProjection[0] = glm::inverse(projection[0]);
        invProjection[1] = glm::inverse(projection[1]);
        
        glUniformMatrix4fv(glGetUniformLocation(m_ssaoProgram, "uProjection"), 2, GL_FALSE, glm::value_ptr(projection[0]));
        glUniformMatrix4fv(glGetUniformLocation(m_ssaoProgram, "uInvProjection"), 2, GL_FALSE, glm::value_ptr(invProjection[0]));
        glUniform1i(glGetUniformLocation(m_ssaoProgram, "uSplitMode"), m_splitMode ? 1 : 0);
        
        glUniform3fv(glGetUniformLocation(m_ssaoProgram, "uSamples"), 64, glm::value_ptr(m_ssaoKernel[0]));
        glUniform2f(glGetUniformLocation(m_ssaoProgram, "uNoiseScale"), m_width / 4.0f, m_height / 4.0f);
        glUniform1f(glGetUniformLocation(m_ssaoProgram, "uRadius"), m_ssaoRadius);
        glUniform1f(glGetUniformLocation(m_ssaoProgram, "uBias"), m_ssaoBias);
        
        glBindVertexArray(m_fsqVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        
        // C. Bilateral Blur Pass
        glBindFramebuffer(GL_FRAMEBUFFER, m_rttSsaoBlur.fbo);
        glViewport(0, 0, m_width, m_height);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(m_ssaoBlurProgram);
        
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_rttSsao.texture);
        glUniform1i(glGetUniformLocation(m_ssaoBlurProgram, "uSsaoTex"), 0);
        
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_rttOpaque.depth);
        glUniform1i(glGetUniformLocation(m_ssaoBlurProgram, "uDepthTex"), 1);
        
        glBindVertexArray(m_fsqVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        
        glEnable(GL_DEPTH_TEST);
    }

    // 2c. SSR Pass
    if (m_useSsr && needsLightingPasses && m_ssrProgram) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_rttSsr.fbo);
        glViewport(0, 0, m_width, m_height);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glUseProgram(m_ssrProgram);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_rttOpaque.texture);
        glUniform1i(glGetUniformLocation(m_ssrProgram, "uColorTex"), 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_rttOpaque.depth);
        glUniform1i(glGetUniformLocation(m_ssrProgram, "uDepthTex"), 1);

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_rttNormals.texture);
        glUniform1i(glGetUniformLocation(m_ssrProgram, "uNormalsTex"), 2);

        const Camera* cam = scene.getCameraByIndex(0);
        if (!cam) cam = &scene.getCamera();

        glm::mat4 proj = cam->getProjMatrix();
        glm::mat4 invProj = glm::inverse(proj);

        glUniformMatrix4fv(glGetUniformLocation(m_ssrProgram, "uProjection"), 1, GL_FALSE, glm::value_ptr(proj));
        glUniformMatrix4fv(glGetUniformLocation(m_ssrProgram, "uInvProjection"), 1, GL_FALSE, glm::value_ptr(invProj));
        glUniform1f(glGetUniformLocation(m_ssrProgram, "uMaxDistance"), m_ssrMaxDistance);
        glUniform1f(glGetUniformLocation(m_ssrProgram, "uIntensity"), m_ssrIntensity);
        glUniform1i(glGetUniformLocation(m_ssrProgram, "uSplitMode"), m_splitMode ? 1 : 0);

        glBindVertexArray(m_fsqVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);

        glEnable(GL_DEPTH_TEST);
    }

    // 2d. Screen-Space Path Tracing (SSPT) Pass
    if (m_renderMode == RenderMode::SSPT && m_ssptProgram) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_rttNormals.fbo);
        glViewport(0, 0, m_width, m_height);
        glClearColor(0.5f, 0.5f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderScenePass(scene, 6); // G-Buffer pass

        RenderTarget& currentAccum = m_accumPing ? m_rttAccumA : m_rttAccumB;
        RenderTarget& prevAccum = m_accumPing ? m_rttAccumB : m_rttAccumA;

        m_accumFrameCount++;
        m_frameIndex++;

        glBindFramebuffer(GL_FRAMEBUFFER, currentAccum.fbo);
        glViewport(0, 0, m_width, m_height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);

        glUseProgram(m_ssptProgram);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_rttOpaque.texture);
        glUniform1i(glGetUniformLocation(m_ssptProgram, "uGAlbedo"), 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_rttNormals.texture);
        glUniform1i(glGetUniformLocation(m_ssptProgram, "uGNormal"), 1);

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_rttOpaque.depth);
        glUniform1i(glGetUniformLocation(m_ssptProgram, "uDepthTex"), 2);

        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, prevAccum.texture);
        glUniform1i(glGetUniformLocation(m_ssptProgram, "uPrevAccum"), 3);

        const Camera* cam = scene.getCameraByIndex(0);
        if (!cam) cam = &scene.getCamera();

        glm::mat4 proj = cam->getProjMatrix();
        glm::mat4 invProj = glm::inverse(proj);
        glm::mat4 view = cam->getViewMatrix();
        glm::mat4 invView = glm::inverse(view);

        glUniformMatrix4fv(glGetUniformLocation(m_ssptProgram, "uProjection"), 1, GL_FALSE, glm::value_ptr(proj));
        glUniformMatrix4fv(glGetUniformLocation(m_ssptProgram, "uInvProjection"), 1, GL_FALSE, glm::value_ptr(invProj));
        glUniformMatrix4fv(glGetUniformLocation(m_ssptProgram, "uView"), 1, GL_FALSE, glm::value_ptr(view));
        glUniformMatrix4fv(glGetUniformLocation(m_ssptProgram, "uInvView"), 1, GL_FALSE, glm::value_ptr(invView));
        glUniform3fv(glGetUniformLocation(m_ssptProgram, "uSPH"), 9, m_sph);
        glUniform1i(glGetUniformLocation(m_ssptProgram, "uFrameIndex"), m_frameIndex);
        glUniform1i(glGetUniformLocation(m_ssptProgram, "uAccumCount"), m_accumFrameCount);

        glBindVertexArray(m_fsqVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);

        if (m_svgfTemporalProgram) {
            glBindFramebuffer(GL_FRAMEBUFFER, m_rttSvgfA.fbo);
            glViewport(0, 0, m_width, m_height);
            glUseProgram(m_svgfTemporalProgram);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, currentAccum.texture);
            glUniform1i(glGetUniformLocation(m_svgfTemporalProgram, "uCurrentTex"), 0);

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, prevAccum.texture);
            glUniform1i(glGetUniformLocation(m_svgfTemporalProgram, "uPrevAccumTex"), 1);

            glUniform1i(glGetUniformLocation(m_svgfTemporalProgram, "uFrameCount"), m_accumFrameCount);

            glBindVertexArray(m_fsqVao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
        }

        if (m_svgfSpatialProgram) {
            glBindFramebuffer(GL_FRAMEBUFFER, m_rttSvgfB.fbo);
            glViewport(0, 0, m_width, m_height);
            glUseProgram(m_svgfSpatialProgram);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_rttSvgfA.texture);
            glUniform1i(glGetUniformLocation(m_svgfSpatialProgram, "uInputTex"), 0);

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, m_rttNormals.texture);
            glUniform1i(glGetUniformLocation(m_svgfSpatialProgram, "uNormalTex"), 1);

            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, m_rttOpaque.depth);
            glUniform1i(glGetUniformLocation(m_svgfSpatialProgram, "uDepthTex"), 2);

            glUniform2f(glGetUniformLocation(m_svgfSpatialProgram, "uTexelSize"), 1.0f / m_width, 1.0f / m_height);
            glUniform1i(glGetUniformLocation(m_svgfSpatialProgram, "uStepSize"), 1);

            glBindVertexArray(m_fsqVao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_rttSvgfB.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_rttOpaque.fbo);
        glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        m_accumPing = !m_accumPing;
    }

    // 3. Transparent Pass
    glBindFramebuffer(GL_FRAMEBUFFER, m_rttTransparent.fbo);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT); // Shares depth buffer with opaque pass
    renderScenePass(scene, 2); // 2 = Transparent geometry

    // 4. Merge Pass (FBO Opaque + FBO Transparent -> FBO Merge)
    glBindFramebuffer(GL_FRAMEBUFFER, m_rttMerge.fbo);
    glViewport(0, 0, m_width, m_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    drawFullscreenMerge(scene);

    // 5. FXAA Pass (FBO Merge -> FBO Composite)
    glBindFramebuffer(GL_FRAMEBUFFER, m_rttComposite.fbo);
    glViewport(0, 0, m_width, m_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    drawFullscreenFxaa();

    // 6. Postprocessing Overlays on FBO Composite
    // Reference Images
    if (!m_splitMode) {
        glViewport(0, 0, m_width, m_height);
        glScissor(0, 0, m_width, m_height);
        glEnable(GL_SCISSOR_TEST);
        drawReferenceImages(scene, scene.getCamera());
        glDisable(GL_SCISSOR_TEST);
    } else {
        int w2 = m_width / 2;
        const Camera* camLeft = scene.getCameraByIndex(0);
        if (camLeft) {
            glViewport(0, 0, w2, m_height);
            glScissor(0, 0, w2, m_height);
            glEnable(GL_SCISSOR_TEST);
            drawReferenceImages(scene, *camLeft);
        }
        const Camera* camRight = scene.getCameraByIndex(1);
        if (camRight) {
            glViewport(w2, 0, m_width - w2, m_height);
            glScissor(w2, 0, m_width - w2, m_height);
            glEnable(GL_SCISSOR_TEST);
            drawReferenceImages(scene, *camRight);
        }
        glDisable(GL_SCISSOR_TEST);
    }

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
    if (!m_isTakingScreenshot) {
        if (!m_splitMode) {
            glViewport(0, 0, m_width, m_height);
            glScissor(0, 0, m_width, m_height);
            glEnable(GL_SCISSOR_TEST);
            drawSelectionCursor(scene, false);
            glDisable(GL_SCISSOR_TEST);
        } else {
            int w2 = m_width / 2;
            int activeVp = scene.getActiveViewport();
            bool showInactive = scene.getSplitShowInactiveCursor();

            // Draw Left Viewport Cursor (isRight = false)
            if (activeVp == 0 || showInactive) {
                glViewport(0, 0, w2, m_height);
                glScissor(0, 0, w2, m_height);
                glEnable(GL_SCISSOR_TEST);
                drawSelectionCursor(scene, false);
                glDisable(GL_SCISSOR_TEST);
            }

            // Draw Right Viewport Cursor (isRight = true)
            if (activeVp == 1 || showInactive) {
                glViewport(w2, 0, m_width - w2, m_height);
                glScissor(w2, 0, m_width - w2, m_height);
                glEnable(GL_SCISSOR_TEST);
                drawSelectionCursor(scene, true);
                glDisable(GL_SCISSOR_TEST);
            }
        }
    }

    // 7. Final Blit to Screen (or Viewport2D Zoom/Pan)
    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
    glViewport(0, 0, m_width, m_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    drawFullscreenViewport2D(scene);

    // Render screen-space Lasso overlay on top of screen
    if (!m_isTakingScreenshot) {
        if (!m_splitMode) {
            glViewport(0, 0, m_width, m_height);
            glScissor(0, 0, m_width, m_height);
            glEnable(GL_SCISSOR_TEST);
            drawLasso();
            glDisable(GL_SCISSOR_TEST);
        } else {
            int w2 = m_width / 2;
            int activeVp = scene.getActiveViewport();
            if (activeVp == 0) {
                glViewport(0, 0, w2, m_height);
                glScissor(0, 0, w2, m_height);
                glEnable(GL_SCISSOR_TEST);
                drawLasso();
            } else {
                glViewport(w2, 0, m_width - w2, m_height);
                glScissor(w2, 0, m_width - w2, m_height);
                glEnable(GL_SCISSOR_TEST);
                drawLasso();
            }
            glDisable(GL_SCISSOR_TEST);
        }
    }
}

void AngleRenderer::renderScenePass(const Scene& scene, int passType) {
    if (!m_splitMode) {
        glViewport(0, 0, m_width, m_height);
        glScissor(0, 0, m_width, m_height);
        glEnable(GL_SCISSOR_TEST);
        drawPassGeometry(scene, passType, scene.getCamera(), 0);
        glDisable(GL_SCISSOR_TEST);
    } else {
        int w2 = m_width / 2;
        
        // Left camera (main scene camera)
        glViewport(0, 0, w2, m_height);
        glScissor(0, 0, w2, m_height);
        glEnable(GL_SCISSOR_TEST);
        const Camera* camLeft = scene.getCameraByIndex(0);
        if (camLeft) {
            int logicalW2 = (int)(w2 / m_dpiScale);
            int logicalH = (int)(m_height / m_dpiScale);
            const_cast<Camera*>(camLeft)->onResize(logicalW2, logicalH);
            drawPassGeometry(scene, passType, *camLeft, 0);
        }

        // Right camera (mirror or independent)
        const Camera* camRight = scene.getCameraByIndex(1);
        if (camRight) {
            glViewport(w2, 0, m_width - w2, m_height);
            glScissor(w2, 0, m_width - w2, m_height);
            int logicalWRight = (int)((m_width - w2) / m_dpiScale);
            int logicalH = (int)(m_height / m_dpiScale);
            const_cast<Camera*>(camRight)->onResize(logicalWRight, logicalH);
            drawPassGeometry(scene, passType, *camRight, 1);
        }
        glDisable(GL_SCISSOR_TEST);
    }
}

void AngleRenderer::drawPassGeometry(const Scene& scene, int passType, const Camera& camera, int viewportIdx) {
    if (passType == 0) {
        // Contour pass
        if (scene.getSelected() && scene.isMeshRenderVisible(scene.getSelected(), viewportIdx)) {
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
            if (scene.isMeshRenderVisible(mesh, viewportIdx) && m_alpha == 1.0f) {
                if (mesh->isArmature && mesh->armatureGraph) {
                    if (mesh == scene.getSelected()) {
                        drawArmature(*mesh->armatureGraph, camera, m_armatureSelectedNode, m_armatureHoveredParent, m_armatureHoveredChild, m_armatureHasSymmetry);
                    } else {
                        drawArmature(*mesh->armatureGraph, camera, nullptr, nullptr, nullptr, m_armatureHasSymmetry);
                    }
                } else {
                    drawMeshSolid(mesh, scene, camera);
                    if (m_showWireframe) {
                        drawWireframe(mesh, scene, camera);
                    }
                }
            }
        }
        if (scene.getVoxelPreview()) {
            drawVoxelPreview(scene, camera, viewportIdx);
        }
    } else if (passType == 2) {
        // Transparent meshes (alpha < 1.0)
        for (auto* mesh : scene.getMeshes()) {
            if (scene.isMeshRenderVisible(mesh, viewportIdx) && m_alpha < 1.0f) {
                if (mesh->isArmature && mesh->armatureGraph) {
                    if (mesh == scene.getSelected()) {
                        drawArmature(*mesh->armatureGraph, camera, m_armatureSelectedNode, m_armatureHoveredParent, m_armatureHoveredChild, m_armatureHasSymmetry);
                    } else {
                        drawArmature(*mesh->armatureGraph, camera, nullptr, nullptr, nullptr, m_armatureHasSymmetry);
                    }
                } else {
                    drawMeshSolid(mesh, scene, camera);
                    if (m_showWireframe) {
                        drawWireframe(mesh, scene, camera);
                    }
                }
            }
        }
        if (scene.getVoxelPreview()) {
            drawVoxelPreview(scene, camera, viewportIdx);
        }
    } else if (passType == 4) {
        // Bevel pre-pass
        for (auto* mesh : scene.getMeshes()) {
            if (scene.isMeshRenderVisible(mesh, viewportIdx)) {
                drawMeshPrepass(mesh, scene, camera);
            }
        }
    } else if (passType == 5) {
        // Normals pre-pass for SSAO
        for (auto* mesh : scene.getMeshes()) {
            if (scene.isMeshRenderVisible(mesh, viewportIdx)) {
                if (mesh->isArmature && mesh->armatureGraph) {
                    if (mesh == scene.getSelected()) {
                        drawArmature(*mesh->armatureGraph, camera, m_armatureSelectedNode, m_armatureHoveredParent, m_armatureHoveredChild, m_armatureHasSymmetry, true);
                    } else {
                        drawArmature(*mesh->armatureGraph, camera, nullptr, nullptr, nullptr, m_armatureHasSymmetry, true);
                    }
                } else {
                    drawMeshNormals(mesh, scene, camera);
                }
            }
        }
    } else if (passType == 6) {
        // G-Buffer pass
        for (auto* mesh : scene.getMeshes()) {
            if (scene.isMeshRenderVisible(mesh, viewportIdx)) {
                drawMeshGBuffer(mesh, scene, camera);
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
    glUniform2f(glGetUniformLocation(m_bgProgram, "uEnvSize"), (float)m_envWidth, (float)m_envHeight);
    glUniform3fv(glGetUniformLocation(m_bgProgram, "uSPH"), 9, m_sph);

    glm::mat3 uIblTransform = glm::transpose(glm::mat3(camera.getViewMatrix()));
    glUniformMatrix3fv(glGetUniformLocation(m_bgProgram, "uIblTransform"), 1, GL_FALSE, glm::value_ptr(uIblTransform));

    glActiveTexture(GL_TEXTURE0);
    if (m_backgroundType == 0) {
        if (m_bgTexture != 0) {
            glBindTexture(GL_TEXTURE_2D, m_bgTexture);
        } else {
            glBindTexture(GL_TEXTURE_2D, m_bgMonoTexture);
        }
    } else {
        if (m_envTexture != 0) {
            glBindTexture(GL_TEXTURE_2D, m_envTexture);
        } else {
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }
    
    GLint locPos = glGetAttribLocation(m_bgProgram, "aVertex");
    GLint locTex = glGetAttribLocation(m_bgProgram, "aTexCoord");

    glBindVertexArray(m_bgVao);

    glBindBuffer(GL_ARRAY_BUFFER, m_bgVbo);
    glVertexAttribPointer(locPos, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(locPos);
    
    if (locTex != -1) {
        glBindBuffer(GL_ARRAY_BUFFER, m_bgTexCoordVbo);
        glVertexAttribPointer(locTex, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(locTex);
    }
    
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}

void AngleRenderer::drawMesh(Mesh* mesh, const Scene& scene) {
    drawMeshSolid(mesh, scene, scene.getCamera());
    if (m_showWireframe) {
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
    if (m_showPolyGroups || m_activeBrush == BRUSH_POLYGROUP || m_shaderType == 5) program = m_polygroupProgram;
    else if (m_shaderType == 0) program = m_pbrProgram;
    else if (m_shaderType == 1) program = m_matcapProgram;
    else if (m_shaderType == 2) program = m_wetClayProgram;
    else if (m_shaderType == 3) program = m_normalProgram;
    else if (m_shaderType == 4) program = m_voxelCheckerProgram;

    if (program == 0) return;
    glUseProgram(program);

    mesh->updateMatrices(camera);

    glUniformMatrix4fv(glGetUniformLocation(program, "uMV"), 1, GL_FALSE, glm::value_ptr(mesh->mvMatrix));
    glUniformMatrix4fv(glGetUniformLocation(program, "uMVP"), 1, GL_FALSE, glm::value_ptr(mesh->mvpMatrix));
    glUniformMatrix3fv(glGetUniformLocation(program, "uN"), 1, GL_FALSE, glm::value_ptr(mesh->nMatrix));
    glUniformMatrix4fv(glGetUniformLocation(program, "uEM"), 1, GL_FALSE, glm::value_ptr(mesh->editMatrix));
    glUniformMatrix3fv(glGetUniformLocation(program, "uEN"), 1, GL_FALSE, glm::value_ptr(mesh->enMatrix));
    glUniform1f(glGetUniformLocation(program, "uAlpha"), m_alpha);
    float effectiveAlbedo[3] = { m_albedo[0], m_albedo[1], m_albedo[2] };
    if (m_useVertexColors) {
        effectiveAlbedo[0] = -1.0f;
        effectiveAlbedo[1] = -1.0f;
        effectiveAlbedo[2] = -1.0f;
    }
    glUniform3fv(glGetUniformLocation(program, "uAlbedo"), 1, &effectiveAlbedo[0]);
    glUniform1i(glGetUniformLocation(program, "uFlat"), m_flatShading ? 1 : 0);

    glUniform3f(glGetUniformLocation(program, "uPlaneN"), m_planeNormal.x, m_planeNormal.y, m_planeNormal.z);
    glUniform3f(glGetUniformLocation(program, "uPlaneO"), m_planeOrigin.x, m_planeOrigin.y, m_planeOrigin.z);
    glUniform1i(glGetUniformLocation(program, "uSym"), m_showSymmetryLine ? 1 : 0);
    
    bool darken = false;
    if (m_darkenUnselected && scene.getMeshes().size() > 1 && scene.getSelected() && scene.getSelected() != mesh) {
        darken = true;
    }
    glUniform1i(glGetUniformLocation(program, "uDarken"), darken ? 1 : 0);
    glUniform1f(glGetUniformLocation(program, "uCurvature"), m_curvature);
    glUniform1f(glGetUniformLocation(program, "uFov"), camera.getFov());

    glUniform1i(glGetUniformLocation(program, "uBevelEnabled"), m_bevelEnabled ? 1 : 0);
    if (m_bevelEnabled) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_rttBevel.texture);
        glUniform1i(glGetUniformLocation(program, "uBevelNormalMap"), 1);
        glUniform2f(glGetUniformLocation(program, "uInvViewportSize"), 1.0f / m_width, 1.0f / m_height);
    }

    glActiveTexture(GL_TEXTURE0);
    if (m_textureId != 0) {
        glBindTexture(GL_TEXTURE_2D, m_textureId);
    } else if (m_shaderType == 0 && m_envTexture != 0) {
        glBindTexture(GL_TEXTURE_2D, m_envTexture);
    } else if (m_shaderType == 1 && m_matcapIdx >= 0 && m_matcapIdx < static_cast<int>(m_matcaps.size()) && m_matcaps[m_matcapIdx].textureId != 0) {
        glBindTexture(GL_TEXTURE_2D, m_matcaps[m_matcapIdx].textureId);
    } else {
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    if (m_shaderType == 0) {
        float effectiveRoughness = m_useVertexMaterials ? -1.0f : m_roughness;
        float effectiveMetallic = m_useVertexMaterials ? -1.0f : m_metallic;
        glUniform1f(glGetUniformLocation(program, "uRoughness"), effectiveRoughness);
        glUniform1f(glGetUniformLocation(program, "uMetallic"), effectiveMetallic);
        glUniform1f(glGetUniformLocation(program, "uExposure"), m_exposure);
        glUniform3fv(glGetUniformLocation(program, "uSPH"), 9, m_sph);
        
        glm::mat3 uIblTransform = glm::transpose(glm::mat3(camera.getViewMatrix()));
        glUniformMatrix3fv(glGetUniformLocation(program, "uIblTransform"), 1, GL_FALSE, glm::value_ptr(uIblTransform));
        
        glUniform1i(glGetUniformLocation(program, "uTexture0"), 0);
        glUniform2f(glGetUniformLocation(program, "uEnvSize"), (float)m_envWidth, (float)m_envHeight);
        glUniform1i(glGetUniformLocation(program, "uUseTexture"), (m_textureId != 0 || m_envTexture != 0) ? 1 : 0);

        // Glass / Transmission uniforms
        glUniform1f(glGetUniformLocation(program, "uTransmission"), m_transmission);
        glUniform1f(glGetUniformLocation(program, "uIor"), m_ior);

        // SSS uniforms (3 parameters: Intensity, Depth, Color)
        glUniform3f(glGetUniformLocation(program, "uSssColor"), m_sssColor.x, m_sssColor.y, m_sssColor.z);
        glUniform1f(glGetUniformLocation(program, "uSssIntensity"), m_sssIntensity);
        glUniform1f(glGetUniformLocation(program, "uSssDepth"), m_sssDepth);

        const auto& lights = scene.getLights();
        int numLights = std::min((int)lights.size(), 8);
        glUniform1i(glGetUniformLocation(program, "uNumLights"), numLights);

        glm::mat4 viewMatrix = camera.getViewMatrix();

        for (int i = 0; i < numLights; ++i) {
            const auto& L = lights[i];
            std::string base = "uLights[" + std::to_string(i) + "].";

            glm::vec3 viewPos = glm::vec3(viewMatrix * glm::vec4(L.position, 1.0f));
            glm::vec3 viewDir = glm::normalize(glm::vec3(viewMatrix * glm::vec4(L.direction, 0.0f)));

            glUniform3fv(glGetUniformLocation(program, (base + "position").c_str()), 1, glm::value_ptr(viewPos));
            glUniform3fv(glGetUniformLocation(program, (base + "direction").c_str()), 1, glm::value_ptr(viewDir));
            glUniform3fv(glGetUniformLocation(program, (base + "color").c_str()), 1, glm::value_ptr(L.color));
            glUniform1f(glGetUniformLocation(program, (base + "intensity").c_str()), L.intensity);
            glUniform1f(glGetUniformLocation(program, (base + "range").c_str()), L.range);
            glUniform1f(glGetUniformLocation(program, (base + "innerCos").c_str()), std::cos(glm::radians(L.innerAngle)));
            glUniform1f(glGetUniformLocation(program, (base + "outerCos").c_str()), std::cos(glm::radians(L.outerAngle)));
            glUniform1i(glGetUniformLocation(program, (base + "type").c_str()), (int)L.type);
            glUniform1i(glGetUniformLocation(program, (base + "castShadow").c_str()), L.castShadow ? 1 : 0);
            glUniform1i(glGetUniformLocation(program, (base + "enabled").c_str()), L.enabled ? 1 : 0);
        }

        glUniform1i(glGetUniformLocation(program, "uShadowEnabled"), m_shadowEnabled ? 1 : 0);
        glUniformMatrix4fv(glGetUniformLocation(program, "uLightMVP"), 1, GL_FALSE, glm::value_ptr(m_shadowLightMVP));
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, m_rttShadow.depth);
        glUniform1i(glGetUniformLocation(program, "uShadowMap"), 4);
    } else if (m_shaderType == 1) {
        glUniform1i(glGetUniformLocation(program, "uTexture0"), 0);
        bool hasMatcap = (m_textureId != 0) || (m_matcapIdx >= 0 && m_matcapIdx < static_cast<int>(m_matcaps.size()) && m_matcaps[m_matcapIdx].textureId != 0);
        glUniform1i(glGetUniformLocation(program, "uUseTexture"), hasMatcap ? 1 : 0);
    } else if (m_shaderType == 2) {
        float effectiveClayColor[3] = { m_albedo[0], m_albedo[1], m_albedo[2] };
        if (m_useVertexColors) {
            effectiveClayColor[0] = -1.0f;
        }
        glUniform3f(glGetUniformLocation(program, "uClayColor"), effectiveClayColor[0], effectiveClayColor[1], effectiveClayColor[2]);
        glUniform1f(glGetUniformLocation(program, "uWetness"), m_wetClayWetness);
        glUniform1f(glGetUniformLocation(program, "uBumpStrength"), m_wetClayBumpStrength);
        glUniform1f(glGetUniformLocation(program, "uNoiseScale"), m_wetClayNoiseScale);
        glUniform1f(glGetUniformLocation(program, "uSSSIntensity"), m_wetClaySSSIntensity);
        glUniform3f(glGetUniformLocation(program, "uSSSColor"), m_wetClaySSSColor.x, m_wetClaySSSColor.y, m_wetClaySSSColor.z);
    } else if (m_shaderType == 4) {
        glUniform1f(glGetUniformLocation(program, "uStep"), 0.5f);
        glUniform1i(glGetUniformLocation(program, "uIsPerspective"), camera.isOrthographic() ? 0 : 1);
        glUniform1f(glGetUniformLocation(program, "uCenterDepth"), camera.getTransZ());
        glUniform1i(glGetUniformLocation(program, "uIsPreview"), 0);
    }

    glUniform1i(glGetUniformLocation(program, "uShowPolyGroups"), m_showPolyGroups ? 1 : 0);

    if (program == m_polygroupProgram) {
        if (bufs->polygroupVao == 0 || bufs->polygroupDirty) {
            buildPolyGroupBuffers(mesh, bufs.get());
            bufs->polygroupDirty = false;
        }
        if (bufs->polygroupVao != 0 && bufs->polygroupVertCount > 0) {
            glBindVertexArray(bufs->polygroupVao);
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(bufs->polygroupVertCount));
            glBindVertexArray(0);
        }
        return;
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

    if (bufs->vboFaceGroups) {
        glBindBuffer(GL_ARRAY_BUFFER, bufs->vboFaceGroups);
        glVertexAttribIPointer(5, 1, GL_UNSIGNED_INT, sizeof(uint32_t), (void*)0);
        glEnableVertexAttribArray(5);
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs->eboTriangles);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(bufs->triIndexCount), GL_UNSIGNED_INT, nullptr);

    glBindVertexArray(0);
}

void AngleRenderer::drawMeshPrepass(Mesh* mesh, const Scene& scene, const Camera& camera) {
    auto it = m_meshBuffers.find(mesh);
    if (it == m_meshBuffers.end() || it->second->triIndexCount == 0) return;
    auto& bufs = it->second;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);

    if (m_bevelPrepassProgram == 0) return;
    glUseProgram(m_bevelPrepassProgram);

    mesh->updateMatrices(camera);

    glUniformMatrix4fv(glGetUniformLocation(m_bevelPrepassProgram, "uMV"), 1, GL_FALSE, glm::value_ptr(mesh->mvMatrix));
    glUniformMatrix4fv(glGetUniformLocation(m_bevelPrepassProgram, "uMVP"), 1, GL_FALSE, glm::value_ptr(mesh->mvpMatrix));
    glUniformMatrix3fv(glGetUniformLocation(m_bevelPrepassProgram, "uN"), 1, GL_FALSE, glm::value_ptr(mesh->nMatrix));
    glUniformMatrix4fv(glGetUniformLocation(m_bevelPrepassProgram, "uEM"), 1, GL_FALSE, glm::value_ptr(mesh->editMatrix));
    glUniformMatrix3fv(glGetUniformLocation(m_bevelPrepassProgram, "uEN"), 1, GL_FALSE, glm::value_ptr(mesh->enMatrix));
    glUniform1i(glGetUniformLocation(m_bevelPrepassProgram, "uFlat"), m_flatShading ? 1 : 0);

    glBindVertexArray(bufs->vao);

    glBindBuffer(GL_ARRAY_BUFFER, bufs->vboVertices);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, bufs->vboNormals);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);

    glBindBuffer(GL_ARRAY_BUFFER, bufs->vboMaterials);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(3);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs->eboTriangles);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(bufs->triIndexCount), GL_UNSIGNED_INT, nullptr);

    glBindVertexArray(0);
}

void AngleRenderer::drawVoxelPreview(const Scene& scene, const Camera& camera, int viewportIdx) {
    if (!scene.getVoxelPreview() || m_voxelCheckerProgram == 0) return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);

    glUseProgram(m_voxelCheckerProgram);
    glUniform1f(glGetUniformLocation(m_voxelCheckerProgram, "uStep"), scene.getVoxelStep());
    glUniform1i(glGetUniformLocation(m_voxelCheckerProgram, "uIsPerspective"), camera.isOrthographic() ? 0 : 1);
    glUniform1f(glGetUniformLocation(m_voxelCheckerProgram, "uCenterDepth"), camera.getTransZ());
    glUniform1i(glGetUniformLocation(m_voxelCheckerProgram, "uIsPreview"), 1);

    for (auto* mesh : scene.getVoxelMeshes()) {
        if (!scene.isMeshRenderVisible(mesh, viewportIdx)) continue;

        auto it = m_meshBuffers.find(mesh);
        if (it == m_meshBuffers.end() || it->second->triIndexCount == 0) continue;
        auto& bufs = it->second;

        mesh->updateMatrices(camera);

        glUniformMatrix4fv(glGetUniformLocation(m_voxelCheckerProgram, "uMV"), 1, GL_FALSE, glm::value_ptr(mesh->mvMatrix));
        glUniformMatrix4fv(glGetUniformLocation(m_voxelCheckerProgram, "uMVP"), 1, GL_FALSE, glm::value_ptr(mesh->mvpMatrix));

        glBindVertexArray(bufs->vao);

        glBindBuffer(GL_ARRAY_BUFFER, bufs->vboVertices);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs->eboTriangles);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(bufs->triIndexCount), GL_UNSIGNED_INT, nullptr);

        glBindVertexArray(0);
    }

    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_BLEND);
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

    glUniform1i(glGetUniformLocation(m_flatProgram, "uBevelEnabled"), m_bevelEnabled ? 1 : 0);
    if (m_bevelEnabled) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_rttBevel.texture);
        glUniform1i(glGetUniformLocation(m_flatProgram, "uBevelNormalMap"), 1);
        glUniform2f(glGetUniformLocation(m_flatProgram, "uInvViewportSize"), 1.0f / m_width, 1.0f / m_height);
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

void AngleRenderer::drawSelectionCursor(const Scene& scene, bool isRight) {
    if (m_smoothCursor) return; // Drawn via ImGui in GuiManager
    if (!m_showCursor || m_selectionProgram == 0) return;

    const Camera& camera = isRight ? *scene.getCameraByIndex(1) : scene.getCamera();

    const glm::mat4& circleMVP = isRight ? m_circleMVPRight : m_circleMVP;
    const glm::mat4& innerCircleMVP = isRight ? m_innerCircleMVPRight : m_innerCircleMVP;
    const glm::mat4& dotMVP = isRight ? m_dotMVPRight : m_dotMVP;
    const std::vector<glm::mat4>& symMVPs = isRight ? m_symMVPsRight : m_symMVPs;
    const std::vector<char>& symOccluded = isRight ? m_symOccludedRight : m_symOccluded;
    
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    glUseProgram(m_selectionProgram);
    GLint locColor = glGetUniformLocation(m_selectionProgram, "uColor");
    GLint locMVP = glGetUniformLocation(m_selectionProgram, "uMVP");
    GLint locPos = glGetAttribLocation(m_selectionProgram, "aVertex");
    
    glUniform1i(glGetUniformLocation(m_selectionProgram, "uRef2DMode"), (camera.getRef2DMode() && !m_cursorIsScreenspace) ? 1 : 0);
    glUniform2f(glGetUniformLocation(m_selectionProgram, "uView2DOffset"), camera.getView2DOffsetX(), camera.getView2DOffsetY());
    glUniform1f(glGetUniformLocation(m_selectionProgram, "uView2DZoom"), camera.getView2DZoom());

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

        int passes = static_cast<int>(std::round(m_cursorThickness * m_dpiScale));
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
            
            glUniformMatrix4fv(locMVP, 1, GL_FALSE, &circleMVP[0][0]);
            glDrawArrays(GL_LINE_LOOP, 0, 64);
        }
        
        // Draw inner circle
        for (int i = 0; i < passes; ++i) {
            float offset = 0.0f;
            if (passes > 1) {
                offset = -0.5f * (passes - 1) + i;
            }
            if (locOffsetPixels != -1) glUniform1f(locOffsetPixels, offset);

            glUniformMatrix4fv(locMVP, 1, GL_FALSE, &innerCircleMVP[0][0]);
            glDrawArrays(GL_LINE_LOOP, 0, 64);
        }

        if (locOffsetPixels != -1) glUniform1f(locOffsetPixels, 0.0f);
    }
    
    // Draw main dot and symmetry dots
    glDisable(GL_DEPTH_TEST);

    // Draw main dot
    glUniform3fv(locColor, 1, &m_cursorColor[0]);
    glUniformMatrix4fv(locMVP, 1, GL_FALSE, &dotMVP[0][0]);
    glBindBuffer(GL_ARRAY_BUFFER, m_dotVbo);
    glVertexAttribPointer(locPos, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(locPos);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 34);

    // Draw symmetry dots
    for (size_t idx = 0; idx < symMVPs.size(); ++idx) {
        bool occluded = (idx < symOccluded.size()) ? symOccluded[idx] : false;
        if (occluded) {
            glm::vec3 darkColor = m_cursorColor * 0.3f;
            glUniform3fv(locColor, 1, &darkColor[0]);
        } else {
            glUniform3fv(locColor, 1, &m_cursorColor[0]);
        }
        glUniformMatrix4fv(locMVP, 1, GL_FALSE, &symMVPs[idx][0][0]);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 34);
    }

    // Restore state
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

    float viewportWidth = (m_splitMode ? m_width * 0.5f : (float)m_width) / m_dpiScale;
    float viewportHeight = (float)m_height / m_dpiScale;

    // Map screen-space points to NDC [-1, 1]
    std::vector<float> ndcPoints;
    ndcPoints.reserve(m_lassoPoints.size() * 3);
    for (const auto& p : m_lassoPoints) {
        float x = (p.x / viewportWidth) * 2.0f - 1.0f;
        float y = 1.0f - (p.y / viewportHeight) * 2.0f;
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
    GLint locRef2DMode = glGetUniformLocation(m_selectionProgram, "uRef2DMode");
    if (locRef2DMode != -1) glUniform1i(locRef2DMode, 0);

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
    const auto& fVisible = mesh->faceVisible;
    bool hasVisibility = !visible.empty();
    bool hasFaceVis = (fVisible.size() == (size_t)mesh->nbFaces);

    for (int i = 0; i < mesh->nbFaces; ++i) {
        if (hasFaceVis) {
            if (!fVisible[i]) continue;
        } else if (hasVisibility) {
            uint32_t v0 = mesh->faces[i * 4];
            uint32_t v1 = mesh->faces[i * 4 + 1];
            uint32_t v2 = mesh->faces[i * 4 + 2];
            uint32_t v3 = mesh->faces[i * 4 + 3];
            if (v0 >= visible.size() || !visible[v0]) continue;
            if (v1 >= visible.size() || !visible[v1]) continue;
            if (v2 >= visible.size() || !visible[v2]) continue;
            if (v3 != 0xffffffff && (v3 >= visible.size() || !visible[v3])) continue;
        }

        uint32_t iv1 = mesh->faces[i * 4];
        uint32_t iv2 = mesh->faces[i * 4 + 1];
        uint32_t iv3 = mesh->faces[i * 4 + 2];
        uint32_t iv4 = mesh->faces[i * 4 + 3];
        bool isQuad = (iv4 != 0xffffffff);

        outIndices.push_back(iv1);
        outIndices.push_back(iv2);
        outIndices.push_back(iv3);
        if (isQuad) {
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
    const auto& fVisible = mesh->faceVisible;
    bool hasVisibility = !visible.empty();
    bool hasFaceVis = (fVisible.size() == (size_t)mesh->nbFaces);

    for (int i = 0; i < mesh->nbFaces; ++i) {
        if (hasFaceVis) {
            if (!fVisible[i]) continue;
        } else if (hasVisibility) {
            uint32_t v0 = mesh->faces[i * 4];
            uint32_t v1 = mesh->faces[i * 4 + 1];
            uint32_t v2 = mesh->faces[i * 4 + 2];
            uint32_t v3 = mesh->faces[i * 4 + 3];
            if (v0 >= visible.size() || !visible[v0]) continue;
            if (v1 >= visible.size() || !visible[v1]) continue;
            if (v2 >= visible.size() || !visible[v2]) continue;
            if (v3 != 0xffffffff && (v3 >= visible.size() || !visible[v3])) continue;
        }

        uint32_t iv1 = mesh->faces[i * 4];
        uint32_t iv2 = mesh->faces[i * 4 + 1];
        uint32_t iv3 = mesh->faces[i * 4 + 2];
        uint32_t iv4 = mesh->faces[i * 4 + 3];
        bool isQuad = (iv4 != 0xffffffff);

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

void AngleRenderer::buildPolyGroupBuffers(const Mesh* mesh, MeshRenderBuffers* bufs) {
    if (!mesh || !bufs) return;

    const auto& fVisible = mesh->faceVisible;
    bool hasFaceVis = (fVisible.size() == (size_t)mesh->nbFaces);
    const auto& visible = mesh->vertVisible;
    bool hasVertVis = !visible.empty();
    bool hasGroups = (mesh->faceGroups.size() == (size_t)mesh->nbFaces);

    std::vector<float> expandedVerts;
    std::vector<float> expandedNormals;
    std::vector<float> expandedMaterials;
    std::vector<uint32_t> expandedGroups;

    size_t estimatedVerts = mesh->nbFaces * 6;
    expandedVerts.reserve(estimatedVerts * 3);
    expandedNormals.reserve(estimatedVerts * 3);
    expandedMaterials.reserve(estimatedVerts * 3);
    expandedGroups.reserve(estimatedVerts);

    for (int i = 0; i < mesh->nbFaces; ++i) {
        if (hasFaceVis) {
            if (!fVisible[i]) continue;
        } else if (hasVertVis) {
            uint32_t v0 = mesh->faces[i * 4];
            uint32_t v1 = mesh->faces[i * 4 + 1];
            uint32_t v2 = mesh->faces[i * 4 + 2];
            uint32_t v3 = mesh->faces[i * 4 + 3];
            if (v0 >= visible.size() || !visible[v0]) continue;
            if (v1 >= visible.size() || !visible[v1]) continue;
            if (v2 >= visible.size() || !visible[v2]) continue;
            if (v3 != 0xffffffff && (v3 >= visible.size() || !visible[v3])) continue;
        }

        uint32_t iv0 = mesh->faces[i * 4];
        uint32_t iv1 = mesh->faces[i * 4 + 1];
        uint32_t iv2 = mesh->faces[i * 4 + 2];
        uint32_t iv3 = mesh->faces[i * 4 + 3];
        bool isQuad = (iv3 != 0xffffffff);
        uint32_t gid = hasGroups ? mesh->faceGroups[i] : 0;

        auto addVertex = [&](uint32_t vid) {
            if (vid < (uint32_t)mesh->nbVerts) {
                expandedVerts.push_back(mesh->verts[vid * 3]);
                expandedVerts.push_back(mesh->verts[vid * 3 + 1]);
                expandedVerts.push_back(mesh->verts[vid * 3 + 2]);

                expandedNormals.push_back(mesh->normals[vid * 3]);
                expandedNormals.push_back(mesh->normals[vid * 3 + 1]);
                expandedNormals.push_back(mesh->normals[vid * 3 + 2]);

                if (mesh->materials.size() == (size_t)mesh->nbVerts * 3) {
                    expandedMaterials.push_back(mesh->materials[vid * 3]);
                    expandedMaterials.push_back(mesh->materials[vid * 3 + 1]);
                    expandedMaterials.push_back(mesh->materials[vid * 3 + 2]);
                } else {
                    expandedMaterials.push_back(0.5f);
                    expandedMaterials.push_back(0.0f);
                    expandedMaterials.push_back(1.0f);
                }
            } else {
                expandedVerts.insert(expandedVerts.end(), {0.0f, 0.0f, 0.0f});
                expandedNormals.insert(expandedNormals.end(), {0.0f, 1.0f, 0.0f});
                expandedMaterials.insert(expandedMaterials.end(), {0.5f, 0.0f, 1.0f});
            }
            expandedGroups.push_back(gid);
        };

        // Triangle 1: iv0, iv1, iv2
        addVertex(iv0);
        addVertex(iv1);
        addVertex(iv2);

        // Triangle 2 (if quad): iv0, iv2, iv3
        if (isQuad) {
            addVertex(iv0);
            addVertex(iv2);
            addVertex(iv3);
        }
    }

    bufs->polygroupVertCount = expandedGroups.size();

    if (bufs->polygroupVao == 0) {
        glGenVertexArrays(1, &bufs->polygroupVao);
        glGenBuffers(1, &bufs->polygroupVboVerts);
        glGenBuffers(1, &bufs->polygroupVboNormals);
        glGenBuffers(1, &bufs->polygroupVboMaterials);
        glGenBuffers(1, &bufs->polygroupVboGroups);
    }

    glBindVertexArray(bufs->polygroupVao);

    glBindBuffer(GL_ARRAY_BUFFER, bufs->polygroupVboVerts);
    glBufferData(GL_ARRAY_BUFFER, expandedVerts.size() * sizeof(float), expandedVerts.data(), GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, bufs->polygroupVboNormals);
    glBufferData(GL_ARRAY_BUFFER, expandedNormals.size() * sizeof(float), expandedNormals.data(), GL_DYNAMIC_DRAW);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);

    glBindBuffer(GL_ARRAY_BUFFER, bufs->polygroupVboMaterials);
    glBufferData(GL_ARRAY_BUFFER, expandedMaterials.size() * sizeof(float), expandedMaterials.data(), GL_DYNAMIC_DRAW);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(3);

    glBindBuffer(GL_ARRAY_BUFFER, bufs->polygroupVboGroups);
    glBufferData(GL_ARRAY_BUFFER, expandedGroups.size() * sizeof(uint32_t), expandedGroups.data(), GL_DYNAMIC_DRAW);
    glVertexAttribIPointer(5, 1, GL_UNSIGNED_INT, sizeof(uint32_t), (void*)0);
    glEnableVertexAttribArray(5);

    glBindVertexArray(0);
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
        glGenBuffers(1, &bufs->vboFaceGroups);
        glGenBuffers(1, &bufs->eboTriangles);
        glGenBuffers(1, &bufs->eboWireframe);
        mesh->isDirty = true;
    }
    
    if (mesh->nbVerts < 0) {
        mesh->nbVerts = 0;
    }
    size_t expectedSize = (size_t)mesh->nbVerts * 3;
    if (mesh->verts.size() != expectedSize) {
        sculpt_log("[WARNING uploadIfDirty] mesh->verts size mismatch: verts.size=%u, expected=%u. Correcting...\n",
                   (unsigned int)mesh->verts.size(), (unsigned int)expectedSize);
        mesh->verts.resize(expectedSize, 0.0f);
        mesh->isDirty = true;
    }
    if (mesh->normals.size() != expectedSize) {
        sculpt_log("[WARNING uploadIfDirty] mesh->normals size mismatch: normals.size=%u, expected=%u. Correcting...\n",
                   (unsigned int)mesh->normals.size(), (unsigned int)expectedSize);
        mesh->normals.resize(expectedSize, 0.0f);
        mesh->isDirty = true;
    }
    if (mesh->colors.size() != expectedSize) {
        sculpt_log("[WARNING uploadIfDirty] mesh->colors size mismatch: colors.size=%u, expected=%u. Correcting...\n",
                   (unsigned int)mesh->colors.size(), (unsigned int)expectedSize);
        mesh->colors.assign(expectedSize, 1.0f);
        mesh->isDirty = true;
    }
    if (mesh->materials.size() != expectedSize) {
        sculpt_log("[WARNING uploadIfDirty] mesh->materials size mismatch: materials.size=%u, expected=%u. Correcting...\n",
                   (unsigned int)mesh->materials.size(), (unsigned int)expectedSize);
        mesh->materials.resize(expectedSize);
        for (int i = 0; i < mesh->nbVerts; ++i) {
            mesh->materials[i * 3]     = 0.5f;
            mesh->materials[i * 3 + 1] = 0.0f;
            mesh->materials[i * 3 + 2] = 1.0f;
        }
        mesh->isDirty = true;
    }

    if (mesh->isDirty || bufs->vertCount != (size_t)mesh->nbVerts || mesh->isFaceGroupDirty) {
        glBindVertexArray(bufs->vao);
        
        glBindBuffer(GL_ARRAY_BUFFER, bufs->vboVertices);
        glBufferData(GL_ARRAY_BUFFER, mesh->verts.size() * sizeof(float), mesh->verts.data(), GL_DYNAMIC_DRAW);
        
        glBindBuffer(GL_ARRAY_BUFFER, bufs->vboNormals);
        glBufferData(GL_ARRAY_BUFFER, mesh->normals.size() * sizeof(float), mesh->normals.data(), GL_DYNAMIC_DRAW);
        
        glBindBuffer(GL_ARRAY_BUFFER, bufs->vboColors);
        glBufferData(GL_ARRAY_BUFFER, mesh->colors.size() * sizeof(float), mesh->colors.data(), GL_DYNAMIC_DRAW);
        
        glBindBuffer(GL_ARRAY_BUFFER, bufs->vboMaterials);
        glBufferData(GL_ARRAY_BUFFER, mesh->materials.size() * sizeof(float), mesh->materials.data(), GL_DYNAMIC_DRAW);

        std::vector<uint32_t> vertGroups(mesh->nbVerts, 0);
        if (mesh->faceGroups.size() == (size_t)mesh->nbFaces) {
            bool hasFaceVis = (mesh->faceVisible.size() == (size_t)mesh->nbFaces);
            for (int f = 0; f < mesh->nbFaces; ++f) {
                if (hasFaceVis && !mesh->faceVisible[f]) continue;
                uint32_t gid = mesh->faceGroups[f];
                for (int k = 0; k < 4; ++k) {
                    uint32_t vid = mesh->faces[f * 4 + k];
                    if (vid != 0xffffffff && vid < (uint32_t)mesh->nbVerts) {
                        vertGroups[vid] = gid;
                    }
                }
            }
        }
        glBindBuffer(GL_ARRAY_BUFFER, bufs->vboFaceGroups);
        glBufferData(GL_ARRAY_BUFFER, vertGroups.size() * sizeof(uint32_t), vertGroups.data(), GL_DYNAMIC_DRAW);
        
        std::vector<uint32_t> triIndices;
        generateTriangleIndices(mesh, triIndices);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs->eboTriangles);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, triIndices.size() * sizeof(uint32_t), triIndices.data(), GL_DYNAMIC_DRAW);
        bufs->triIndexCount = triIndices.size();
        
        std::vector<uint32_t> wireIndices;
        generateWireframeIndices(mesh, wireIndices);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs->eboWireframe);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, wireIndices.size() * sizeof(uint32_t), wireIndices.data(), GL_DYNAMIC_DRAW);
        bufs->wireIndexCount = wireIndices.size();
        
        glBindVertexArray(0);
        bufs->vertCount = mesh->nbVerts;

        bufs->polygroupDirty = true;
        if (bufs->polygroupVao != 0 && (m_showPolyGroups || m_activeBrush == BRUSH_POLYGROUP || m_shaderType == 5)) {
            buildPolyGroupBuffers(mesh, bufs.get());
            bufs->polygroupDirty = false;
        }
        mesh->isFaceGroupDirty = false;
        
        mesh->isDirty = false;
        mesh->isVertexDirty = false;
        mesh->isColorDirty = false;
        mesh->isMaterialDirty = false;
        mesh->isTopologyDirty = false;
    } else {
        if (mesh->isFaceGroupDirty) {
            std::vector<uint32_t> vertGroups(mesh->nbVerts, 0);
            if (mesh->faceGroups.size() == (size_t)mesh->nbFaces) {
                bool hasFaceVis = (mesh->faceVisible.size() == (size_t)mesh->nbFaces);
                for (int f = 0; f < mesh->nbFaces; ++f) {
                    if (hasFaceVis && !mesh->faceVisible[f]) continue;
                    uint32_t gid = mesh->faceGroups[f];
                    for (int k = 0; k < 4; ++k) {
                        uint32_t vid = mesh->faces[f * 4 + k];
                        if (vid != 0xffffffff && vid < (uint32_t)mesh->nbVerts) {
                            vertGroups[vid] = gid;
                        }
                    }
                }
            }
            glBindBuffer(GL_ARRAY_BUFFER, bufs->vboFaceGroups);
            glBufferData(GL_ARRAY_BUFFER, vertGroups.size() * sizeof(uint32_t), vertGroups.data(), GL_DYNAMIC_DRAW);

            std::vector<uint32_t> triIndices;
            generateTriangleIndices(mesh, triIndices);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs->eboTriangles);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, triIndices.size() * sizeof(uint32_t), triIndices.data(), GL_DYNAMIC_DRAW);
            bufs->triIndexCount = triIndices.size();

            std::vector<uint32_t> wireIndices;
            generateWireframeIndices(mesh, wireIndices);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs->eboWireframe);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, wireIndices.size() * sizeof(uint32_t), wireIndices.data(), GL_DYNAMIC_DRAW);
            bufs->wireIndexCount = wireIndices.size();

            bufs->polygroupDirty = true;
            if (bufs->polygroupVao != 0 && (m_showPolyGroups || m_activeBrush == BRUSH_POLYGROUP || m_shaderType == 5)) {
                buildPolyGroupBuffers(mesh, bufs.get());
                bufs->polygroupDirty = false;
            }
            mesh->isFaceGroupDirty = false;
        }
        if (mesh->isVertexDirty && mesh->dirtyVertMin <= mesh->dirtyVertMax && mesh->dirtyVertMax < (uint32_t)mesh->nbVerts) {
            size_t offset = mesh->dirtyVertMin * 3 * sizeof(float);
            size_t size   = (mesh->dirtyVertMax - mesh->dirtyVertMin + 1) * 3 * sizeof(float);
            
            glBindBuffer(GL_ARRAY_BUFFER, bufs->vboVertices);
            glBufferSubData(GL_ARRAY_BUFFER, offset, size,
                            mesh->verts.data() + mesh->dirtyVertMin * 3);
            
            glBindBuffer(GL_ARRAY_BUFFER, bufs->vboNormals);
            glBufferSubData(GL_ARRAY_BUFFER, offset, size,
                            mesh->normals.data() + mesh->dirtyVertMin * 3);

            bufs->polygroupDirty = true;
            if (bufs->polygroupVao != 0 && (m_showPolyGroups || m_activeBrush == BRUSH_POLYGROUP || m_shaderType == 5)) {
                buildPolyGroupBuffers(mesh, bufs.get());
                bufs->polygroupDirty = false;
            }
            
            mesh->isVertexDirty = false;
        }

        if (mesh->isColorDirty && mesh->dirtyVertMin <= mesh->dirtyVertMax && mesh->dirtyVertMax < (uint32_t)mesh->nbVerts) {
            size_t offset = mesh->dirtyVertMin * 3 * sizeof(float);
            size_t size   = (mesh->dirtyVertMax - mesh->dirtyVertMin + 1) * 3 * sizeof(float);

            glBindBuffer(GL_ARRAY_BUFFER, bufs->vboColors);
            glBufferSubData(GL_ARRAY_BUFFER, offset, size,
                            mesh->colors.data() + mesh->dirtyVertMin * 3);

            mesh->isColorDirty = false;
        }

        if (mesh->isMaterialDirty && mesh->dirtyVertMin <= mesh->dirtyVertMax && mesh->dirtyVertMax < (uint32_t)mesh->nbVerts) {
            size_t offset = mesh->dirtyVertMin * 3 * sizeof(float);
            size_t size   = (mesh->dirtyVertMax - mesh->dirtyVertMin + 1) * 3 * sizeof(float);

            glBindBuffer(GL_ARRAY_BUFFER, bufs->vboMaterials);
            glBufferSubData(GL_ARRAY_BUFFER, offset, size,
                            mesh->materials.data() + mesh->dirtyVertMin * 3);

            mesh->isMaterialDirty = false;
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

void AngleRenderer::drawReferenceImages(const Scene& scene, const Camera& camera) {
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
            
            glm::mat4 mvp = camera.getProjMatrix() * camera.getViewMatrix() * model;
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

void AngleRenderer::drawArmature(const ArmatureGraph& graph, const Camera& camera, void* selectedNodePtr, void* hoveredParentPtr, void* hoveredChildPtr, bool hasSymmetry, bool normalsPass) {
    if (m_armatureSphereIndicesCount == 0 || m_armatureCylIndicesCount == 0) return;
    
    GLuint program = normalsPass ? m_armatureNormalsProgram : m_armatureProgram;
    if (program == 0) return;
    
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    
    glUseProgram(program);
    
    if (!normalsPass) {
        glActiveTexture(GL_TEXTURE0);
        int matcapTexId = 0;
        if (m_matcapIdx >= 0 && m_matcapIdx < static_cast<int>(m_matcaps.size()) && m_matcaps[m_matcapIdx].textureId != 0) {
            matcapTexId = m_matcaps[m_matcapIdx].textureId;
        } else if (!m_matcaps.empty() && m_matcaps[0].textureId != 0) {
            matcapTexId = m_matcaps[0].textureId;
        }
        glBindTexture(GL_TEXTURE_2D, matcapTexId);
        glUniform1i(glGetUniformLocation(program, "uTexture0"), 0);
    }

    glm::mat4 proj = camera.getProjMatrix();
    glm::mat4 view = camera.getViewMatrix();
    
    const auto& nodes = graph.getNodes();
    
    ArmatureNode* selectedNode = static_cast<ArmatureNode*>(selectedNodePtr);
    ArmatureNode* hoveredParent = static_cast<ArmatureNode*>(hoveredParentPtr);
    ArmatureNode* hoveredChild = static_cast<ArmatureNode*>(hoveredChildPtr);

    auto drawObject = [&](GLuint vao, int indices, const glm::mat4& model, float rBot, float rTop, const glm::vec3& color, bool selected) {
        glm::mat4 mv = view * model;
        glm::mat4 mvp = proj * mv;
        glm::mat3 n = glm::transpose(glm::inverse(glm::mat3(mv)));
        
        glUniformMatrix4fv(glGetUniformLocation(program, "uMV"), 1, GL_FALSE, glm::value_ptr(mv));
        glUniformMatrix4fv(glGetUniformLocation(program, "uMVP"), 1, GL_FALSE, glm::value_ptr(mvp));
        glUniformMatrix3fv(glGetUniformLocation(program, "uN"), 1, GL_FALSE, glm::value_ptr(n));
        glUniform1f(glGetUniformLocation(program, "uRadiusTop"), rTop);
        glUniform1f(glGetUniformLocation(program, "uRadiusBottom"), rBot);
        if (!normalsPass) {
            glUniform3fv(glGetUniformLocation(program, "uColor"), 1, glm::value_ptr(color));
            glUniform1f(glGetUniformLocation(program, "uSelected"), selected ? 1.0f : 0.0f);
        }
        
        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, indices, GL_UNSIGNED_INT, nullptr);
    };

    for (const auto& nodePtr : nodes) {
        auto* node = nodePtr.get();
        if (!node) continue;
        
        // Sphere Colors (like JS)
        bool isSelected = (node == selectedNode) || (hasSymmetry && selectedNode && node == selectedNode->symmetryPartner);
        bool isOnSymPlane = false;
        if (hasSymmetry) {
            float dist = std::abs(glm::dot(node->position - m_planeOrigin, m_planeNormal));
            float threshold = std::max(0.08f, 0.15f * node->radius);
            if (dist <= threshold) isOnSymPlane = true;
        }

        glm::vec3 color;
        if (isOnSymPlane) {
            color = isSelected ? glm::vec3(0.8f, 0.1f, 0.9f) : glm::vec3(0.75f, 0.35f, 0.95f);
        } else {
            color = isSelected ? glm::vec3(0.8f, 0.1f, 0.1f) : glm::vec3(0.5f, 0.5f, 0.5f);
        }
        
        glm::mat4 model = glm::translate(glm::mat4(1.0f), node->position);
        model = glm::scale(model, glm::vec3(node->radius));
        drawObject(m_armatureSphereVao, m_armatureSphereIndicesCount, model, 1.0f, 1.0f, color, false);
        
        // Draw Cylinders
        for (auto* child : node->children) {
            if (child) {
                glm::vec3 dir = child->position - node->position;
                float len = glm::length(dir);
                if (len < 0.0001f) continue;
                dir /= len;
                
                glm::vec3 up(0, 0, 1);
                if (std::abs(glm::dot(dir, up)) > 0.99f) up = glm::vec3(1, 0, 0);
                glm::vec3 right = glm::normalize(glm::cross(up, dir));
                up = glm::cross(dir, right);
                
                glm::mat4 rot(1.0f);
                rot[0] = glm::vec4(right, 0);
                rot[1] = glm::vec4(up, 0);
                rot[2] = glm::vec4(dir, 0);
                
                glm::mat4 cmodel = glm::translate(glm::mat4(1.0f), node->position);
                cmodel = cmodel * rot;
                cmodel = glm::scale(cmodel, glm::vec3(1.0f, 1.0f, len));
                
                bool linkHovered = false;
                if (hoveredParent == node && hoveredChild == child) linkHovered = true;
                if (hasSymmetry && node->symmetryPartner && child->symmetryPartner) {
                    if (hoveredParent == node->symmetryPartner && hoveredChild == child->symmetryPartner) {
                        linkHovered = true;
                    }
                }
                
                drawObject(m_armatureCylVao, m_armatureCylIndicesCount, cmodel, node->radius, child->radius, glm::vec3(0.6f, 0.6f, 0.6f), linkHovered);
            }
        }
    }
    
    glBindVertexArray(0);
    glDisable(GL_CULL_FACE);
}

void AngleRenderer::initArmatureGeometry() {
    // 1. Sphere Geometry (Icosahedron subdivided once)
    float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
    std::vector<glm::vec3> baseVerts = {
        {-1,  t,  0}, { 1,  t,  0}, {-1, -t,  0}, { 1, -t,  0},
        { 0, -1,  t}, { 0,  1,  t}, { 0, -1, -t}, { 0,  1, -t},
        { t,  0, -1}, { t,  0,  1}, {-t,  0, -1}, {-t,  0,  1}
    };
    for (auto& v : baseVerts) v = glm::normalize(v);

    std::vector<uint32_t> baseFaces = {
        0, 11, 5,   0, 5, 1,    0, 1, 7,    0, 7, 10,   0, 10, 11,
        1, 5, 9,    5, 11, 4,   11, 10, 2,  10, 7, 6,   7, 1, 8,
        3, 9, 4,    3, 4, 2,    3, 2, 6,    3, 6, 8,    3, 8, 9,
        4, 9, 5,    2, 4, 11,   6, 2, 10,   8, 6, 7,    9, 8, 1
    };

    std::vector<float> sphereData;
    std::vector<uint32_t> sphereIndices;
    
    // We will do flat shading or smooth shading? 
    // Matcap expects smooth shading, so we can just use vertices as normals.
    // Subdivide three times for even smoother spheres
    int subdivisions = 3;
    std::vector<glm::vec3> verts = baseVerts;
    std::vector<uint32_t> faces = baseFaces;
    
    auto getMid = [&](int i1, int i2, std::vector<glm::vec3>& verts) {
        glm::vec3 m = glm::normalize(verts[i1] + verts[i2]);
        verts.push_back(m);
        return (uint32_t)(verts.size() - 1);
    };

    for (int s = 0; s < subdivisions; ++s) {
        std::vector<uint32_t> newFaces;
        for (size_t i = 0; i < faces.size(); i += 3) {
            uint32_t v1 = faces[i], v2 = faces[i+1], v3 = faces[i+2];
            uint32_t a = getMid(v1, v2, verts);
            uint32_t b = getMid(v2, v3, verts);
            uint32_t c = getMid(v3, v1, verts);
            
            newFaces.insert(newFaces.end(), {v1, a, c, v2, b, a, v3, c, b, a, b, c});
        }
        faces = newFaces;
    }
    sphereIndices = faces;

    for (const auto& v : verts) {
        sphereData.push_back(v.x); sphereData.push_back(v.y); sphereData.push_back(v.z);
        sphereData.push_back(v.x); sphereData.push_back(v.y); sphereData.push_back(v.z);
    }

    m_armatureSphereIndicesCount = sphereIndices.size();
    glGenVertexArrays(1, &m_armatureSphereVao);
    glGenBuffers(1, &m_armatureSphereVbo);
    glGenBuffers(1, &m_armatureSphereEbo);

    glBindVertexArray(m_armatureSphereVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_armatureSphereVbo);
    glBufferData(GL_ARRAY_BUFFER, sphereData.size() * sizeof(float), sphereData.data(), GL_STATIC_DRAW);
    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_armatureSphereEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sphereIndices.size() * sizeof(uint32_t), sphereIndices.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glBindVertexArray(0);

    // 2. Cylinder Geometry (Octagonal prism mapping Z=0 to Z=1)
    std::vector<float> cylData;
    std::vector<uint32_t> cylIndices;
    const int segments = 32;
    for (int i = 0; i <= segments; ++i) {
        float angle = (float)i / segments * 2.0f * (float)M_PI;
        float x = std::cos(angle);
        float y = std::sin(angle);
        // bottom vertex (z=0)
        cylData.push_back(x); cylData.push_back(y); cylData.push_back(0.0f);
        cylData.push_back(x); cylData.push_back(y); cylData.push_back(0.0f); // normal
        // top vertex (z=1)
        cylData.push_back(x); cylData.push_back(y); cylData.push_back(1.0f);
        cylData.push_back(x); cylData.push_back(y); cylData.push_back(0.0f); // normal
    }

    for (int i = 0; i < segments; ++i) {
        uint32_t b1 = i * 2;
        uint32_t t1 = i * 2 + 1;
        uint32_t b2 = (i + 1) * 2;
        uint32_t t2 = (i + 1) * 2 + 1;
        cylIndices.insert(cylIndices.end(), {b1, b2, t1, b2, t2, t1});
    }

    m_armatureCylIndicesCount = cylIndices.size();
    glGenVertexArrays(1, &m_armatureCylVao);
    glGenBuffers(1, &m_armatureCylVbo);
    glGenBuffers(1, &m_armatureCylEbo);

    glBindVertexArray(m_armatureCylVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_armatureCylVbo);
    glBufferData(GL_ARRAY_BUFFER, cylData.size() * sizeof(float), cylData.data(), GL_STATIC_DRAW);
    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_armatureCylEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, cylIndices.size() * sizeof(uint32_t), cylIndices.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glBindVertexArray(0);
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

void AngleRenderer::drawFullscreenMerge(const Scene& scene) {
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

    // Bind depth texture to slot 2
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_rttOpaque.depth);
    glUniform1i(glGetUniformLocation(m_mergeProgram, "uOpaqueDepth"), 2);

    // Bind current matcap texture (slot 3) if in Matcap Shading mode
    glActiveTexture(GL_TEXTURE3);
    if (m_shaderType == 1 && m_matcapIdx >= 0 && m_matcapIdx < static_cast<int>(m_matcaps.size()) && m_matcaps[m_matcapIdx].textureId != 0) {
        glBindTexture(GL_TEXTURE_2D, m_matcaps[m_matcapIdx].textureId);
    } else {
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glUniform1i(glGetUniformLocation(m_mergeProgram, "uMatcapTexture"), 3);

    // Pass bevel parameters
    glUniform1i(glGetUniformLocation(m_mergeProgram, "uBevelEnabled"), m_bevelEnabled ? 1 : 0);
    glUniform1f(glGetUniformLocation(m_mergeProgram, "uBevelRadius"), m_bevelRadius);
    glUniform1f(glGetUniformLocation(m_mergeProgram, "uBevelStrength"), m_bevelStrength);
    glUniform1i(glGetUniformLocation(m_mergeProgram, "uShaderType"), m_shaderType);
    glUniform3fv(glGetUniformLocation(m_mergeProgram, "uAlbedo"), 1, m_albedo);
    glUniform1f(glGetUniformLocation(m_mergeProgram, "uFov"), scene.getCamera().getFov());
    glUniform1f(glGetUniformLocation(m_mergeProgram, "uNear"), scene.getCamera().getNear());
    glUniform1f(glGetUniformLocation(m_mergeProgram, "uFar"), scene.getCamera().getFar());
    glUniform1i(glGetUniformLocation(m_mergeProgram, "uOrtho"), scene.getCamera().isOrthographic() ? 1 : 0);
    
    // Also pass camera aspect ratio
    float aspect = (m_height > 0) ? (float)m_width / (float)m_height : 1.0f;
    if (m_splitMode) aspect *= 0.5f; // Split mode viewports are half width
    glUniform1f(glGetUniformLocation(m_mergeProgram, "uAspect"), aspect);

    glUniform1i(glGetUniformLocation(m_mergeProgram, "uFilmic"), m_filmic ? 1 : 0);

    // Pass SSAO parameters — only effective when PBR shader is active
    const bool ssaoActive = m_useSsao && (m_shaderType == 0);
    glActiveTexture(GL_TEXTURE4);
    if (ssaoActive) {
        glBindTexture(GL_TEXTURE_2D, m_rttSsaoBlur.texture);
    } else {
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glUniform1i(glGetUniformLocation(m_mergeProgram, "uSsaoTexture"), 4);
    glUniform1i(glGetUniformLocation(m_mergeProgram, "uSsaoEnabled"), ssaoActive ? 1 : 0);
    glUniform1f(glGetUniformLocation(m_mergeProgram, "uSsaoIntensity"), m_ssaoIntensity);

    const bool ssrActive = m_useSsr && (m_shaderType == 0) && m_renderMode == RenderMode::PBR;
    glActiveTexture(GL_TEXTURE5);
    if (ssrActive) {
        glBindTexture(GL_TEXTURE_2D, m_rttSsr.texture);
    } else {
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glUniform1i(glGetUniformLocation(m_mergeProgram, "uSsrTexture"), 5);
    glUniform1i(glGetUniformLocation(m_mergeProgram, "uSsrEnabled"), ssrActive ? 1 : 0);
    glUniform1f(glGetUniformLocation(m_mergeProgram, "uSsrIntensity"), m_ssrIntensity);

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
    glUniform1i(glGetUniformLocation(m_fxaaProgram, "uEnabled"), m_useFxaa ? 1 : 0);

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

    float offset[2] = { 0.0f, 0.0f };
    float zoom = 1.0f;
    if (scene.getCamera().getRef2DMode()) {
        offset[0] = scene.getCamera().getView2DOffsetX();
        offset[1] = scene.getCamera().getView2DOffsetY();
        zoom = scene.getCamera().getView2DZoom();
    }
    glUniform2fv(glGetUniformLocation(m_viewport2DProgram, "uView2DOffset"), 1, offset);
    glUniform1f(glGetUniformLocation(m_viewport2DProgram, "uView2DZoom"), zoom);

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

    stbi_set_flip_vertically_on_load(true);
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

    m_envWidth = width;
    m_envHeight = height;

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
    
    m_isTakingScreenshot = true;
    
    // Resize renderer and all its RTTs to target width & height
    resize(w, h);
    
    // Resize active camera temporarily to match screenshot dimensions/aspect ratio
    Camera& camera = const_cast<Camera&>(scene.getCamera());
    int oldCamW = camera.getWidth();
    int oldCamH = camera.getHeight();
    camera.onResize(w, h);
    
    // Create temporary offscreen framebuffer & texture to render the final blit
    GLuint tempFbo = 0;
    GLuint tempTex = 0;
    glGenFramebuffers(1, &tempFbo);
    glGenTextures(1, &tempTex);
    
    glBindTexture(GL_TEXTURE_2D, tempTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glBindFramebuffer(GL_FRAMEBUFFER, tempFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tempTex, 0);
    
    // Render the scene to the temporary FBO
    render(scene, tempFbo);
    
    // Read the rendered pixels from the temporary FBO
    std::vector<uint8_t> buffer(w * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buffer.data());
    
    // Clean up temporary GL resources
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &tempFbo);
    glDeleteTextures(1, &tempTex);
    
    // Restore states
    m_isTakingScreenshot = false;
    camera.onResize(oldCamW, oldCamH);
    resize(oldW, oldH);
    
    // Flip pixels vertically (since OpenGL coords start from bottom-left)
    std::vector<uint8_t> flippedBuffer(w * h * 4);
    for (int y = 0; y < h; ++y) {
        std::memcpy(
            flippedBuffer.data() + y * w * 4,
            buffer.data() + (h - 1 - y) * w * 4,
            w * 4
        );
    }
    
    return flippedBuffer;
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

        stbi_set_flip_vertically_on_load(true);
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

void AngleRenderer::updateBackgroundGeometry() {
    float x = 1.0f;
    float y = 1.0f;

    if (m_backgroundType == 0) {
        if (m_bgWidth > 0 && m_bgHeight > 0 && m_width > 0 && m_height > 0) {
            float ratio = ((float)m_width / m_height) / ((float)m_bgWidth / m_bgHeight);
            float comp = m_bgFill ? 1.0f / ratio : ratio;
            x = comp < 1.0f ? 1.0f : 1.0f / ratio;
            y = comp < 1.0f ? ratio : 1.0f;
        }
    }

    float bgQuad[] = {
        -x, -y,
         x, -y,
        -x,  y,
         x,  y
    };

    glBindBuffer(GL_ARRAY_BUFFER, m_bgVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(bgQuad), bgQuad, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void AngleRenderer::loadBackgroundTexture(const std::string& path) {
    m_bgTexturePath = path;
    if (path.empty()) {
        deleteBackgroundTexture();
        return;
    }
    
    int width = 0, height = 0, channels = 0;
    stbi_set_flip_vertically_on_load(true);
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!data) {
        std::cerr << "Failed to load background image: " << path << std::endl;
        deleteBackgroundTexture();
        return;
    }

    if (m_bgTexture) {
        glDeleteTextures(1, &m_bgTexture);
        m_bgTexture = 0;
    }

    glGenTextures(1, &m_bgTexture);
    glBindTexture(GL_TEXTURE_2D, m_bgTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    stbi_image_free(data);

    m_bgWidth = width;
    m_bgHeight = height;

    updateBackgroundGeometry();
}

void AngleRenderer::deleteBackgroundTexture() {
    if (m_bgTexture) {
        glDeleteTextures(1, &m_bgTexture);
        m_bgTexture = 0;
    }
    m_bgTexturePath = "";
    m_bgWidth = 1;
    m_bgHeight = 1;
    updateBackgroundGeometry();
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

void AngleRenderer::importMatcap(const std::string& name, const std::string& path) {
    int width = 0, height = 0, channels = 0;
    stbi_set_flip_vertically_on_load(true);
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!data) {
        std::cerr << "Failed to load matcap image: " << path << std::endl;
        return;
    }
    MatcapPreset preset;
    preset.name = name;
    preset.texPath = path;
    glGenTextures(1, &preset.textureId);
    glBindTexture(GL_TEXTURE_2D, preset.textureId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);
    m_matcaps.push_back(preset);
}

void AngleRenderer::initSsaoKernel() {
    m_ssaoKernel.clear();
    std::default_random_engine generator(12345);
    std::uniform_real_distribution<float> randomFloats(0.0f, 1.0f);
    
    for (unsigned int i = 0; i < 64; ++i) {
        glm::vec3 sample(
            randomFloats(generator) * 2.0f - 1.0f,
            randomFloats(generator) * 2.0f - 1.0f,
            randomFloats(generator)
        );
        sample = glm::normalize(sample);
        sample *= randomFloats(generator);
        
        // Scale samples to cluster them close to the origin
        float scale = (float)i / 64.0f;
        scale = glm::mix(0.1f, 1.0f, scale * scale);
        sample *= scale;
        m_ssaoKernel.push_back(sample);
    }
}

void AngleRenderer::initSsaoNoiseTexture() {
    std::default_random_engine generator(54321);
    std::uniform_real_distribution<float> randomFloats(0.0f, 1.0f);
    std::vector<glm::vec4> ssaoNoise;
    for (unsigned int i = 0; i < 16; i++) {
        float nx = randomFloats(generator) * 2.0f - 1.0f;
        float ny = randomFloats(generator) * 2.0f - 1.0f;
        float len = std::sqrt(nx * nx + ny * ny);
        if (len > 0.0001f) {
            nx /= len;
            ny /= len;
        } else {
            nx = 1.0f;
            ny = 0.0f;
        }
        ssaoNoise.push_back(glm::vec4(nx, ny, 0.0f, 1.0f));
    }
    
    glGenTextures(1, &m_ssaoNoiseTexture);
    glBindTexture(GL_TEXTURE_2D, m_ssaoNoiseTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 4, 4, 0, GL_RGBA, GL_FLOAT, ssaoNoise.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void AngleRenderer::drawMeshNormals(Mesh* mesh, const Scene& scene, const Camera& camera) {
    auto it = m_meshBuffers.find(mesh);
    if (it == m_meshBuffers.end() || it->second->triIndexCount == 0) return;
    auto& bufs = it->second;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);

    if (m_ssaoNormalsProgram == 0) return;
    glUseProgram(m_ssaoNormalsProgram);

    mesh->updateMatrices(camera);

    glUniformMatrix4fv(glGetUniformLocation(m_ssaoNormalsProgram, "uMV"), 1, GL_FALSE, glm::value_ptr(mesh->mvMatrix));
    glUniformMatrix4fv(glGetUniformLocation(m_ssaoNormalsProgram, "uMVP"), 1, GL_FALSE, glm::value_ptr(mesh->mvpMatrix));
    glUniformMatrix3fv(glGetUniformLocation(m_ssaoNormalsProgram, "uN"), 1, GL_FALSE, glm::value_ptr(mesh->nMatrix));
    glUniformMatrix4fv(glGetUniformLocation(m_ssaoNormalsProgram, "uEM"), 1, GL_FALSE, glm::value_ptr(mesh->editMatrix));
    glUniformMatrix3fv(glGetUniformLocation(m_ssaoNormalsProgram, "uEN"), 1, GL_FALSE, glm::value_ptr(mesh->enMatrix));
    glUniform1i(glGetUniformLocation(m_ssaoNormalsProgram, "uFlat"), m_flatShading ? 1 : 0);

    glUniform1i(glGetUniformLocation(m_ssaoNormalsProgram, "uBevelEnabled"), m_bevelEnabled ? 1 : 0);
    if (m_bevelEnabled) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_rttBevel.texture);
        glUniform1i(glGetUniformLocation(m_ssaoNormalsProgram, "uBevelNormalMap"), 1);
        glUniform2f(glGetUniformLocation(m_ssaoNormalsProgram, "uInvViewportSize"), 1.0f / m_width, 1.0f / m_height);
    }

    glBindVertexArray(bufs->vao);

    glBindBuffer(GL_ARRAY_BUFFER, bufs->vboVertices);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, bufs->vboNormals);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);

    glBindBuffer(GL_ARRAY_BUFFER, bufs->vboMaterials);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(3);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufs->eboTriangles);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(bufs->triIndexCount), GL_UNSIGNED_INT, nullptr);

    glBindVertexArray(0);
}

void AngleRenderer::drawMeshGBuffer(Mesh* mesh, const Scene& scene, const Camera& camera) {
    auto it = m_meshBuffers.find(mesh);
    if (it == m_meshBuffers.end() || it->second->triIndexCount == 0) return;
    auto& bufs = it->second;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);

    if (m_gbufferProgram == 0) return;
    glUseProgram(m_gbufferProgram);

    mesh->updateMatrices(camera);

    glUniformMatrix4fv(glGetUniformLocation(m_gbufferProgram, "uMV"), 1, GL_FALSE, glm::value_ptr(mesh->mvMatrix));
    glUniformMatrix4fv(glGetUniformLocation(m_gbufferProgram, "uMVP"), 1, GL_FALSE, glm::value_ptr(mesh->mvpMatrix));
    glUniformMatrix3fv(glGetUniformLocation(m_gbufferProgram, "uN"), 1, GL_FALSE, glm::value_ptr(mesh->nMatrix));
    glUniformMatrix4fv(glGetUniformLocation(m_gbufferProgram, "uEM"), 1, GL_FALSE, glm::value_ptr(mesh->editMatrix));
    glUniformMatrix3fv(glGetUniformLocation(m_gbufferProgram, "uEN"), 1, GL_FALSE, glm::value_ptr(mesh->enMatrix));

    float effectiveAlbedo[3] = { m_albedo[0], m_albedo[1], m_albedo[2] };
    if (m_useVertexColors) {
        effectiveAlbedo[0] = -1.0f;
    }
    glUniform3fv(glGetUniformLocation(m_gbufferProgram, "uAlbedo"), 1, &effectiveAlbedo[0]);
    glUniform1f(glGetUniformLocation(m_gbufferProgram, "uRoughness"), m_useVertexMaterials ? -1.0f : m_roughness);
    glUniform1f(glGetUniformLocation(m_gbufferProgram, "uMetallic"), m_useVertexMaterials ? -1.0f : m_metallic);

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

