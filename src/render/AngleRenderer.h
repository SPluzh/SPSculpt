#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <GLES3/gl3.h>
#include <glm/glm.hpp>
#include "render/ReferenceImage.h"
#include "render/RenderTarget.h"

class Mesh;
class Scene;
class Camera;

struct MeshRenderBuffers {
    GLuint vao = 0;
    GLuint vboVertices = 0;
    GLuint vboNormals = 0;
    GLuint vboColors = 0;
    GLuint vboMaterials = 0;
    GLuint eboTriangles = 0;
    GLuint eboWireframe = 0;
    
    size_t vertCount = 0;
    size_t triIndexCount = 0;
    size_t wireIndexCount = 0;

    ~MeshRenderBuffers() {
        if (vao) glDeleteVertexArrays(1, &vao);
        if (vboVertices) glDeleteBuffers(1, &vboVertices);
        if (vboNormals) glDeleteBuffers(1, &vboNormals);
        if (vboColors) glDeleteBuffers(1, &vboColors);
        if (vboMaterials) glDeleteBuffers(1, &vboMaterials);
        if (eboTriangles) glDeleteBuffers(1, &eboTriangles);
        if (eboWireframe) glDeleteBuffers(1, &eboWireframe);
    }
};

class AngleRenderer {
public:
    AngleRenderer();
    ~AngleRenderer();

    bool init(int width, int height);
    void resize(int width, int height);
    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }
    
    // Core render loop called from JS requestAnimationFrame or SDL main loop
    void render(const Scene& scene, unsigned int targetFbo = 0);
    void setShowBackground(bool show) { m_showBackground = show; }
    bool getShowBackground() const { return m_showBackground; }

    void setBackgroundType(int type) { m_backgroundType = type; updateBackgroundGeometry(); }
    int getBackgroundType() const { return m_backgroundType; }
    void setBgBlur(float blur) { m_bgBlur = blur; }
    float getBgBlur() const { return m_bgBlur; }
    void setBgFill(bool fill) { m_bgFill = fill; updateBackgroundGeometry(); }
    bool getBgFill() const { return m_bgFill; }
    std::string getBgTexturePath() const { return m_bgTexturePath; }
    void setBgTexturePath(const std::string& path) { m_bgTexturePath = path; }
    void loadBackgroundTexture(const std::string& path);
    void deleteBackgroundTexture();
    void setFilmic(bool filmic) { m_filmic = filmic; }
    bool getFilmic() const { return m_filmic; }
    void setBevelEnabled(bool enabled) { m_bevelEnabled = enabled; }
    bool getBevelEnabled() const { return m_bevelEnabled; }
    void setBevelRadius(float radius) { m_bevelRadius = radius; }
    float getBevelRadius() const { return m_bevelRadius; }
    void setBevelStrength(float strength) { m_bevelStrength = strength; }
    float getBevelStrength() const { return m_bevelStrength; }
    void setBevelScaleWithDistance(bool scale) { m_bevelScaleWithDistance = scale; }
    bool getBevelScaleWithDistance() const { return m_bevelScaleWithDistance; }
    void setShowContour(bool show) { m_showContour = show; }
    bool getShowContour() const { return m_showContour; }
    void setCursorThickness(float thickness) { m_cursorThickness = thickness; }
    float getCursorThickness() const { return m_cursorThickness; }
    void setSmoothCursor(bool smooth) { m_smoothCursor = smooth; }
    bool getSmoothCursor() const { return m_smoothCursor; }
    void setShowGrid(bool show) { m_showGrid = show; }
    bool getShowGrid() const { return m_showGrid; }
    void setShowSymmetryLine(bool show) { m_showSymmetryLine = show; }
    bool getShowSymmetryLine() const { return m_showSymmetryLine; }
    void setContourColor(const glm::vec4& color) { m_contourColor = color; }
    glm::vec4 getContourColor() const { return m_contourColor; }
    void setSplitMode(bool split) { m_splitMode = split; }
    bool getSplitMode() const { return m_splitMode; }
    float getExposure() const { return m_exposure; }
    void setExposure(float exp) { m_exposure = exp; }
    void setCameraRight(std::shared_ptr<const Camera> cameraRight) { m_cameraRight = cameraRight; }
    std::shared_ptr<const Camera> getCameraRight() const { return m_cameraRight; }

    float getWetClayWetness() const { return m_wetClayWetness; }
    void setWetClayWetness(float val) { m_wetClayWetness = val; }
    float getWetClayBumpStrength() const { return m_wetClayBumpStrength; }
    void setWetClayBumpStrength(float val) { m_wetClayBumpStrength = val; }
    float getWetClayNoiseScale() const { return m_wetClayNoiseScale; }
    void setWetClayNoiseScale(float val) { m_wetClayNoiseScale = val; }
    float getWetClaySSSIntensity() const { return m_wetClaySSSIntensity; }
    void setWetClaySSSIntensity(float val) { m_wetClaySSSIntensity = val; }
    glm::vec3 getWetClaySSSColor() const { return m_wetClaySSSColor; }
    void setWetClaySSSColor(const glm::vec3& col) { m_wetClaySSSColor = col; }

    void setShaderType(int type) { m_shaderType = type; }
    int getShaderType() const { return m_shaderType; }
    void setMatcap(int idx) { m_matcapIdx = idx; }
    int getMatcap() const { return m_matcapIdx; }
    void setFlatShading(bool flat) { m_flatShading = flat; }
    bool getFlatShading() const { return m_flatShading; }
    void setDarkenUnselected(bool darken) { m_darkenUnselected = darken; }
    bool getDarkenUnselected() const { return m_darkenUnselected; }
    void setShowWireframe(bool show) { m_showWireframe = show; }
    bool getShowWireframe() const { return m_showWireframe; }
    void setCurvature(float c) { m_curvature = c; }
    float getCurvature() const { return m_curvature; }

    void setAlbedo(float r, float g, float b) { m_albedo[0] = r; m_albedo[1] = g; m_albedo[2] = b; }
    const float* getAlbedo() const { return m_albedo; }
    void setRoughness(float r) { m_roughness = r; }
    float getRoughness() const { return m_roughness; }
    void setMetallic(float m) { m_metallic = m; }
    float getMetallic() const { return m_metallic; }
    void setAlpha(float a) { m_alpha = a; }
    float getAlpha() const { return m_alpha; }
    void setTextureId(unsigned int id) { m_textureId = id; }
    unsigned int getTextureId() const { return m_textureId; }
    void setHasUV(bool h) { m_hasUV = h; }
    bool getHasUV() const { return m_hasUV; }

    struct EnvironmentPreset {
        std::string name;
        std::string texPath;
        float sph[27];
        float exposure;
    };
    const std::vector<EnvironmentPreset>& getEnvironments() const { return m_environments; }
    int getCurrentEnvIdx() const { return m_currentEnvIdx; }
    void setEnvironmentPreset(int idx);

    struct MatcapPreset {
        std::string name;
        std::string texPath;
        GLuint textureId = 0;
    };
    const std::vector<MatcapPreset>& getMatcaps() const { return m_matcaps; }
    void importMatcap(const std::string& name, const std::string& path);

    std::vector<uint8_t> renderToBuffer(const Scene& scene, int w, int h);

    // Helpers to update environmental params
    void setEnvironmentParameters(float exposure, const std::vector<float>& sph);
    void setEnvironmentParametersFast(float exposure, uintptr_t sphPtr);
    
    void setSymmetryParameters(bool showSymmetryLine, const std::vector<float>& planeOrigin, const std::vector<float>& planeNormal);
    void setSymmetryParametersFast(bool showSymmetryLine, uintptr_t planeOriginPtr, uintptr_t planeNormalPtr);

    // Selection cursor drawing parameters
    void setCursorParameters(
        bool showCursor,
        bool showCircle,
        const std::vector<float>& circleMVP,
        const std::vector<float>& innerCircleMVP,
        const std::vector<float>& dotMVP,
        const std::vector<float>& symMVPs,
        const std::vector<float>& cursorColor
    );
    void setCursorParametersFast(
        bool showCursor,
        bool showCircle,
        uintptr_t circleMVPPtr,
        uintptr_t innerCircleMVPPtr,
        uintptr_t dotMVPPtr,
        uintptr_t symMVPsPtr,
        int symMVPsCount,
        uintptr_t cursorColorPtr,
        uintptr_t symOccludedPtr = 0,
        bool isScreenspace = false
    );
    void setCursorParametersRightFast(
        uintptr_t circleMVPPtr,
        uintptr_t innerCircleMVPPtr,
        uintptr_t dotMVPPtr,
        uintptr_t symMVPsPtr,
        int symMVPsCount,
        uintptr_t symOccludedPtr = 0
    );

    void setLassoParameters(bool active, const std::vector<glm::vec2>& points, bool altMode, bool isMaskLasso);

    // Shader compilation helpers
    GLuint compileShader(GLenum type, const std::string& source);
    GLuint linkProgram(GLuint vs, GLuint fs);
    std::string loadShaderSource(const std::string& filename);
    GLuint loadAndCompileProgram(const std::string& vertFile, const std::string& fragFile);

    // Mesh buffer uploads
    void uploadIfDirty(Mesh* mesh);

private:
    void generateTriangleIndices(const Mesh* mesh, std::vector<uint32_t>& outIndices);
    void generateWireframeIndices(const Mesh* mesh, std::vector<uint32_t>& outEdges);

    void drawBackground(const Scene& scene, const Camera& camera);
    void updateBackgroundGeometry();
    void drawMesh(Mesh* mesh, const Scene& scene); // Keep for compatibility
    void drawMeshSolid(Mesh* mesh, const Scene& scene, const Camera& camera);
    void drawMeshPrepass(Mesh* mesh, const Scene& scene, const Camera& camera);
    void drawMeshFlatColor(Mesh* mesh, const Scene& scene, const Camera& camera, const glm::vec4& color);
    void drawWireframe(Mesh* mesh, const Scene& scene, const Camera& camera);
    void drawReferenceImages(const Scene& scene, const Camera& camera);
    void drawSelectionCursor(const Scene& scene, bool isRight = false);
    void drawLasso();

    void initGrid();
    void drawGrid(const Scene& scene, const Camera& camera);
    void drawFullscreenMerge(const Scene& scene);
    void drawFullscreenFxaa();
    void drawFullscreenViewport2D(const Scene& scene);
    void drawContourOverlay(const Scene& scene);
    void initEnvironments();
    void loadEnvironmentTexture(int idx);
    void initMatcaps();

    void renderScenePass(const Scene& scene, int passType);
    void drawPassGeometry(const Scene& scene, int passType, const Camera& camera, int viewportIdx);

    int m_width = 0;
    int m_height = 0;
    bool m_showBackground = true;

    // Environmental parameters
    float m_exposure = 1.0f;
    float m_sph[27] = {0.0f};

    // Symmetry parameters
    bool m_showSymmetryLine = false;
    glm::vec3 m_planeOrigin{0.0f};
    glm::vec3 m_planeNormal{0.0f, 0.0f, 1.0f};

    // Selection parameters
    bool m_showCursor = false;
    bool m_showCircle = true;
    glm::mat4 m_circleMVP{1.0f};
    glm::mat4 m_innerCircleMVP{1.0f};
    glm::mat4 m_dotMVP{1.0f};
    std::vector<glm::mat4> m_symMVPs;
    std::vector<char> m_symOccluded;
    glm::vec3 m_cursorColor{1.0f, 0.0f, 0.0f};
    bool m_cursorIsScreenspace = false;

    glm::mat4 m_circleMVPRight{1.0f};
    glm::mat4 m_innerCircleMVPRight{1.0f};
    glm::mat4 m_dotMVPRight{1.0f};
    std::vector<glm::mat4> m_symMVPsRight;
    std::vector<char> m_symOccludedRight;

    // Lasso selection parameters
    bool m_lassoActive = false;
    std::vector<glm::vec2> m_lassoPoints;
    bool m_lassoAlt = false;
    bool m_isMaskLasso = false;
    GLuint m_lassoVao = 0;
    GLuint m_lassoVbo = 0;

    // Shader programs
    GLuint m_pbrProgram = 0;
    GLuint m_matcapProgram = 0;
    GLuint m_flatProgram = 0;
    GLuint m_wireframeProgram = 0;
    GLuint m_bgProgram = 0;
    GLuint m_selectionProgram = 0;
    GLuint m_refImageProgram = 0;
    GLuint m_mergeProgram = 0;
    GLuint m_fxaaProgram = 0;
    GLuint m_viewport2DProgram = 0;
    GLuint m_contourProgram = 0;
    GLuint m_wetClayProgram = 0;
    GLuint m_voxelCheckerProgram = 0;
    GLuint m_normalProgram = 0;
    GLuint m_bevelPrepassProgram = 0;
    GLuint m_bevelFilterProgram = 0;

    // RTT Targets
    RenderTarget m_rttContour;
    RenderTarget m_rttOpaque;
    RenderTarget m_rttTransparent;
    RenderTarget m_rttMerge;
    RenderTarget m_rttComposite;
    RenderTarget m_rttPrepass;
    RenderTarget m_rttBevel;

    // Fullscreen quad
    GLuint m_fsqVao = 0;
    GLuint m_fsqVbo = 0;

    // Grid geometry
    GLuint m_gridVao = 0;
    GLuint m_gridVbo = 0;
    int m_gridLineCount = 0;

    // Settings
    int m_backgroundType = 0;
    float m_bgBlur = 0.0f;
    bool m_filmic = false;
    bool m_bevelEnabled = false;
    float m_bevelRadius = 4.0f;
    float m_bevelStrength = 1.5f;
    bool m_bevelScaleWithDistance = false;
    bool m_showContour = true;
    bool m_showGrid = true;
    glm::vec4 m_contourColor{1.0f, 0.75f, 0.1f, 1.0f};
    float m_cursorThickness = 2.5f;
    bool m_smoothCursor = true;

    int m_shaderType = 0;
    int m_matcapIdx = 0;
    bool m_flatShading = false;
    bool m_darkenUnselected = true;
    bool m_showWireframe = false;
    float m_curvature = 0.0f;

    // Global material settings
    float m_albedo[3] = {0.72f, 0.52f, 0.45f};
    float m_roughness = 0.5f;
    float m_metallic = 0.0f;
    float m_alpha = 1.0f;
    unsigned int m_textureId = 0;
    bool m_hasUV = false;

    // Wet clay parameters
    float m_wetClayWetness = 0.6f;
    float m_wetClayBumpStrength = 0.4f;
    float m_wetClayNoiseScale = 8.0f;
    float m_wetClaySSSIntensity = 0.25f;
    glm::vec3 m_wetClaySSSColor{0.8f, 0.3f, 0.15f};

    // Split Viewport
    bool m_splitMode = false;
    std::shared_ptr<const Camera> m_cameraRight;
    bool m_isTakingScreenshot = false;

    // Environment presets
    std::vector<EnvironmentPreset> m_environments;
    int m_currentEnvIdx = 0;
    GLuint m_envTexture = 0;
    int m_envWidth = 1024;
    int m_envHeight = 512;

    // Matcap presets
    std::vector<MatcapPreset> m_matcaps;

    // Static buffers for background quad, selection circle and dot
    GLuint m_bgVao = 0;
    GLuint m_bgVbo = 0;
    GLuint m_bgTexCoordVbo = 0;
    GLuint m_bgTexture = 0;
    GLuint m_bgMonoTexture = 0;
    bool m_bgFill = true;
    int m_bgWidth = 1;
    int m_bgHeight = 1;
    std::string m_bgTexturePath;

    GLuint m_selectionVao = 0;
    GLuint m_circleVbo = 0;
    GLuint m_dotVbo = 0;

    // GPU buffers cache for meshes
    std::unordered_map<const Mesh*, std::unique_ptr<MeshRenderBuffers>> m_meshBuffers;

    // Uniform locations cache
    std::unordered_map<GLuint, std::unordered_map<std::string, GLint>> m_uniformLocations;
    GLint getCachedUniformLocation(GLuint program, const char* name);
};
