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
    
    // Core render loop called from JS requestAnimationFrame or SDL main loop
    void render(const Scene& scene);
    void setShowBackground(bool show) { m_showBackground = show; }
    bool getShowBackground() const { return m_showBackground; }

    void setBackgroundType(int type) { m_backgroundType = type; }
    int getBackgroundType() const { return m_backgroundType; }
    void setBgBlur(float blur) { m_bgBlur = blur; }
    float getBgBlur() const { return m_bgBlur; }
    void setFilmic(bool filmic) { m_filmic = filmic; }
    bool getFilmic() const { return m_filmic; }
    void setShowContour(bool show) { m_showContour = show; }
    bool getShowContour() const { return m_showContour; }
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
    void setCameraRight(std::shared_ptr<Camera> cameraRight) { m_cameraRight = cameraRight; }
    std::shared_ptr<Camera> getCameraRight() const { return m_cameraRight; }

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

    std::vector<uint8_t> renderToBuffer(const Scene& scene, int w, int h);

    // Helpers to update environmental params
    void setEnvironmentParameters(float exposure, const std::vector<float>& sph);
    void setEnvironmentParametersFast(float exposure, uintptr_t sphPtr);
    
    void setSymmetryParameters(bool showSymmetryLine, const std::vector<float>& planeOrigin, const std::vector<float>& planeNormal);
    void setSymmetryParametersFast(bool showSymmetryLine, uintptr_t planeOriginPtr, uintptr_t planeNormalPtr);

    // Selection cursor drawing parameters
    void setCursorParameters(
        bool showCursor,
        const std::vector<float>& circleMVP,
        const std::vector<float>& innerCircleMVP,
        const std::vector<float>& dotMVP,
        const std::vector<float>& symMVPs,
        const std::vector<float>& cursorColor
    );
    void setCursorParametersFast(
        bool showCursor,
        uintptr_t circleMVPPtr,
        uintptr_t innerCircleMVPPtr,
        uintptr_t dotMVPPtr,
        uintptr_t symMVPsPtr,
        int symMVPsCount,
        uintptr_t cursorColorPtr
    );

    void setLassoParameters(bool active, const std::vector<glm::vec2>& points, bool altMode);

    // Shader compilation helpers
    GLuint compileShader(GLenum type, const std::string& source);
    GLuint linkProgram(GLuint vs, GLuint fs);

    // Mesh buffer uploads
    void uploadIfDirty(Mesh* mesh);

private:
    void generateTriangleIndices(const Mesh* mesh, std::vector<uint32_t>& outIndices);
    void generateWireframeIndices(const Mesh* mesh, std::vector<uint32_t>& outEdges);

    void drawBackground(const Scene& scene, const Camera& camera);
    void drawMesh(Mesh* mesh, const Scene& scene); // Keep for compatibility
    void drawMeshSolid(Mesh* mesh, const Scene& scene, const Camera& camera);
    void drawMeshFlatColor(Mesh* mesh, const Scene& scene, const Camera& camera, const glm::vec4& color);
    void drawWireframe(Mesh* mesh, const Scene& scene, const Camera& camera);
    void drawReferenceImages(const Scene& scene);
    void drawSelectionCursor();
    void drawLasso();

    void initGrid();
    void drawGrid(const Scene& scene, const Camera& camera);
    void drawFullscreenMerge();
    void drawFullscreenFxaa();
    void drawFullscreenViewport2D(const Scene& scene);
    void drawContourOverlay(const Scene& scene);
    void initEnvironments();
    void loadEnvironmentTexture(int idx);
    void initMatcaps();

    void renderScenePass(const Scene& scene, int passType);
    void drawPassGeometry(const Scene& scene, int passType, const Camera& camera);

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
    glm::mat4 m_circleMVP{1.0f};
    glm::mat4 m_innerCircleMVP{1.0f};
    glm::mat4 m_dotMVP{1.0f};
    std::vector<glm::mat4> m_symMVPs;
    glm::vec3 m_cursorColor{1.0f, 0.0f, 0.0f};

    // Lasso selection parameters
    bool m_lassoActive = false;
    std::vector<glm::vec2> m_lassoPoints;
    bool m_lassoAlt = false;
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

    // RTT Targets
    RenderTarget m_rttContour;
    RenderTarget m_rttOpaque;
    RenderTarget m_rttTransparent;
    RenderTarget m_rttMerge;
    RenderTarget m_rttComposite;

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
    bool m_showContour = true;
    bool m_showGrid = true;
    glm::vec4 m_contourColor{1.0f, 0.75f, 0.1f, 1.0f};

    // Split Viewport
    bool m_splitMode = false;
    std::shared_ptr<Camera> m_cameraRight;

    // Environment presets
    std::vector<EnvironmentPreset> m_environments;
    int m_currentEnvIdx = 0;
    GLuint m_envTexture = 0;

    // Matcap presets
    std::vector<MatcapPreset> m_matcaps;

    // Static buffers for background quad, selection circle and dot
    GLuint m_bgVao = 0;
    GLuint m_bgVbo = 0;

    GLuint m_selectionVao = 0;
    GLuint m_circleVbo = 0;
    GLuint m_dotVbo = 0;

    // GPU buffers cache for meshes
    std::unordered_map<const Mesh*, std::unique_ptr<MeshRenderBuffers>> m_meshBuffers;

    // Uniform locations cache
    std::unordered_map<GLuint, std::unordered_map<std::string, GLint>> m_uniformLocations;
    GLint getCachedUniformLocation(GLuint program, const char* name);
};
