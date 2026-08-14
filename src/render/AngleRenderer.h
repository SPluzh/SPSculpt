#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <chrono>
#include <GLES3/gl3.h>
#include <glm/glm.hpp>
#include "scene/Camera.h"
#include "render/ReferenceImage.h"
#include "render/RenderTarget.h"
#include "common/Enums.h"

class Mesh;
class Scene;
class ArmatureGraph;

struct SymmetryPlaneData {
    glm::vec3 origin{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec3 color{1.0f, 0.25f, 0.25f};
};

struct ModelSnapshot {
    Camera frozenCamera;
    RenderTarget rt;
    int width = 0;
    int height = 0;
    bool active = false;
    bool needsUpdate = false;
    bool isSilhouette = false;
};


struct MeshRenderBuffers {
    GLuint vao = 0;
    GLuint vboVertices = 0;
    GLuint vboNormals = 0;
    GLuint vboColors = 0;
    GLuint vboMaterials = 0;
    GLuint vboFaceGroups = 0;
    GLuint eboTriangles = 0;
    GLuint eboWireframe = 0;
    
    // Dedicated expanded VAO/VBOs for PolyGroup rendering
    GLuint polygroupVao = 0;
    GLuint polygroupVboVerts = 0;
    GLuint polygroupVboNormals = 0;
    GLuint polygroupVboMaterials = 0;
    GLuint polygroupVboGroups = 0;
    size_t polygroupVertCount = 0;
    bool polygroupDirty = true;
    
    size_t vertCount = 0;
    size_t triIndexCount = 0;
    size_t wireIndexCount = 0;

    ~MeshRenderBuffers() {
        if (vao) glDeleteVertexArrays(1, &vao);
        if (vboVertices) glDeleteBuffers(1, &vboVertices);
        if (vboNormals) glDeleteBuffers(1, &vboNormals);
        if (vboColors) glDeleteBuffers(1, &vboColors);
        if (vboMaterials) glDeleteBuffers(1, &vboMaterials);
        if (vboFaceGroups) glDeleteBuffers(1, &vboFaceGroups);
        if (eboTriangles) glDeleteBuffers(1, &eboTriangles);
        if (eboWireframe) glDeleteBuffers(1, &eboWireframe);

        if (polygroupVao) glDeleteVertexArrays(1, &polygroupVao);
        if (polygroupVboVerts) glDeleteBuffers(1, &polygroupVboVerts);
        if (polygroupVboNormals) glDeleteBuffers(1, &polygroupVboNormals);
        if (polygroupVboMaterials) glDeleteBuffers(1, &polygroupVboMaterials);
        if (polygroupVboGroups) glDeleteBuffers(1, &polygroupVboGroups);
    }
};

enum class RenderMode {
    PBR = 0,
    SSPT = 1
};

class AngleRenderer {
public:
    AngleRenderer();
    ~AngleRenderer();

    bool init(int width, int height);
    void resize(int width, int height, float dpiScale = 1.0f);
    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }

    void setRenderMode(RenderMode mode) { m_renderMode = mode; resetAccumulation(); }
    RenderMode getRenderMode() const { return m_renderMode; }
    void resetAccumulation() { m_accumFrameCount = 0; }

    void setUseShadows(bool enable) { m_shadowEnabled = enable; }
    bool getUseShadows() const { return m_shadowEnabled; }

    void setUseSsr(bool enable) { m_useSsr = enable; }
    bool getUseSsr() const { return m_useSsr; }
    void setSsrIntensity(float intensity) { m_ssrIntensity = intensity; }
    float getSsrIntensity() const { return m_ssrIntensity; }
    void setSsrMaxDistance(float dist) { m_ssrMaxDistance = dist; }
    float getSsrMaxDistance() const { return m_ssrMaxDistance; }
    
    // Core render loop called from JS requestAnimationFrame or SDL main loop
    void render(const Scene& scene, unsigned int targetFbo = 0);
    void setShowBackground(bool show) { m_showBackground = show; }
    bool getShowBackground() const { return m_showBackground; }
    void setTransparentClear(bool v) { m_transparentClear = v; }
    bool getTransparentClear() const { return m_transparentClear; }

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
    void setUseFxaa(bool enable) { m_useFxaa = enable; }
    bool getUseFxaa() const { return m_useFxaa; }
    void setFxaaSharpMode(bool sharp) { m_fxaaSharpMode = sharp; }
    bool getFxaaSharpMode() const { return m_fxaaSharpMode; }

    // TAA & Anti-Aliasing controls
    void setUseTaa(bool enable) { m_useTaa = enable; resetTaaHistory(); }
    bool getUseTaa() const { return m_useTaa; }
    void setTaaResetOnStroke(bool enable) { m_taaResetOnStroke = enable; }
    bool getTaaResetOnStroke() const { return m_taaResetOnStroke; }
    void resetTaaHistory() { m_taaResetHistory = true; }

    void setUseMotionVectors(bool enable) { m_useMotionVectors = enable; }
    bool getUseMotionVectors() const { return m_useMotionVectors; }

    // Depth Precision & Polygon Offset
    void setUseReverseZ(bool enable) { m_useReverseZ = enable; }
    bool getUseReverseZ() const { return m_useReverseZ; }

    void setUsePolygonOffset(bool enable) { m_usePolygonOffset = enable; }
    bool getUsePolygonOffset() const { return m_usePolygonOffset; }
    void setPolygonOffsetFactor(float factor) { m_polyOffsetFactor = factor; }
    float getPolygonOffsetFactor() const { return m_polyOffsetFactor; }
    void setPolygonOffsetUnits(float units) { m_polyOffsetUnits = units; }
    float getPolygonOffsetUnits() const { return m_polyOffsetUnits; }

    // Anisotropy & Subpixel Culling
    void setAnisotropyLevel(int level) { m_anisotropyLevel = level; }
    int getAnisotropyLevel() const { return m_anisotropyLevel; }

    void setUseSubpixelCulling(bool enable) { m_useSubpixelCulling = enable; }
    bool getUseSubpixelCulling() const { return m_useSubpixelCulling; }
    void setSubpixelCullThreshold(float threshold) { m_subpixelCullThreshold = threshold; }
    float getSubpixelCullThreshold() const { return m_subpixelCullThreshold; }

    void setUseSsao(bool enable) { m_useSsao = enable; }
    bool getUseSsao() const { return m_useSsao; }
    void setSsaoRadius(float radius) { m_ssaoRadius = radius; }
    float getSsaoRadius() const { return m_ssaoRadius; }
    void setSsaoBias(float bias) { m_ssaoBias = bias; }
    float getSsaoBias() const { return m_ssaoBias; }
    void setSsaoIntensity(float intensity) { m_ssaoIntensity = intensity; }
    float getSsaoIntensity() const { return m_ssaoIntensity; }
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
    void setEnableSelectionFlash(bool enable) { m_enableSelectionFlash = enable; }
    bool getEnableSelectionFlash() const { return m_enableSelectionFlash; }
    void setSelectionAnimDuration(float dur) { m_selectionAnimDuration = std::max(0.05f, dur); }
    float getSelectionAnimDuration() const { return m_selectionAnimDuration; }
    void triggerSelectionFlash() { m_selectionAnimTimer = m_selectionAnimDuration; }
    void setCursorThickness(float thickness) { m_cursorThickness = thickness; }
    float getCursorThickness() const { return m_cursorThickness; }
    void setSmoothCursor(bool smooth) { m_smoothCursor = smooth; }
    bool getSmoothCursor() const { return m_smoothCursor; }
    void setShowGrid(bool show) { m_showGrid = show; }
    bool getShowGrid() const { return m_showGrid; }
    void setShowSafeFrames(bool show) { m_showSafeFrames = show; }
    bool getShowSafeFrames() const { return m_showSafeFrames; }
    void setSafeFramesMargin(float margin) { m_safeFramesMargin = std::max(0.0f, margin); }
    float getSafeFramesMargin() const { return m_safeFramesMargin; }
    void setSafeFramesThickness(float thickness) { m_safeFramesThickness = std::max(0.01f, thickness); }
    float getSafeFramesThickness() const { return m_safeFramesThickness; }
    void setShowPolyGroups(bool show) { m_showPolyGroups = show; }
    bool getShowPolyGroups() const { return m_showPolyGroups; }
    void setActiveBrush(BrushType brush) { m_activeBrush = brush; }
    BrushType getActiveBrush() const { return m_activeBrush; }
    void setShowSymmetryLine(bool show) { m_showSymmetryLine = show; }
    bool getShowSymmetryLine() const { return m_showSymmetryLine; }
    void triggerTempSymmetryLine(float durationSeconds = 1.8f) {
        m_tempSymLineDuration = std::max(0.05f, durationSeconds);
        m_symmetryAnimTimer = m_tempSymLineDuration;
    }
    void cancelTempSymmetryLine() {
        m_symmetryAnimTimer = 0.0f;
        m_symmetryCurrentAlpha = 0.0f;
    }
    bool isTempSymmetryLineActive() const {
        return m_symmetryAnimTimer > 0.0f || m_symmetryCurrentAlpha > 0.001f;
    }
    bool isSymmetryLineVisible() const {
        return m_showSymmetryLine || isTempSymmetryLineActive();
    }
    float getSymmetryAlpha() const {
        return m_symmetryCurrentAlpha;
    }
    const std::vector<SymmetryPlaneData>& getSymmetryPlanes() const { return m_symmetryPlanes; }
    void setSymmetryLineWidth(float width) { m_symmetryLineWidth = width; }
    float getSymmetryLineWidth() const { return m_symmetryLineWidth; }
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
    void setMatcap(int idx);
    int getMatcap() const { return m_matcapIdx; }
    void setFlatShading(bool flat) { m_flatShading = flat; }
    bool getFlatShading() const { return m_flatShading; }
    void setDarkenUnselected(bool darken) { m_darkenUnselected = darken; }
    bool getDarkenUnselected() const { return m_darkenUnselected; }
    void setShowWireframe(bool show) { m_showWireframe = show; }
    bool getShowWireframe() const { return m_showWireframe; }

    // X-Ray mode controls
    void setXrayEnabled(bool enable) { m_xrayEnabled = enable; }
    bool getXrayEnabled() const { return m_xrayEnabled; }
    void setXrayTarget(int target) { m_xrayTarget = target; } // 0: Unselected (default), 1: Selected, 2: All
    int getXrayTarget() const { return m_xrayTarget; }
    void setXrayColor(const glm::vec4& col) { m_xrayColor = col; }
    const glm::vec4& getXrayColor() const { return m_xrayColor; }
    glm::vec4& getXrayColorRef() { return m_xrayColor; }
    bool isMeshXRay(const Scene& scene, Mesh* mesh) const;
    GLuint getXrayMatcapTexture() const;

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
    void setUseVertexColors(bool use) { m_useVertexColors = use; }
    bool getUseVertexColors() const { return m_useVertexColors; }
    void setUseVertexMaterials(bool use) { m_useVertexMaterials = use; }
    bool getUseVertexMaterials() const { return m_useVertexMaterials; }

    // Glass parameters
    float getTransmission() const { return m_transmission; }
    void setTransmission(float t) { m_transmission = t; }
    float getIor() const { return m_ior; }
    void setIor(float ior) { m_ior = ior; }

    // SSS parameters (3 parameters: Intensity, Depth & Color)
    float getSssIntensity() const { return m_sssIntensity; }
    void setSssIntensity(float val) { m_sssIntensity = val; }
    float getSssDepth() const { return m_sssDepth; }
    void setSssDepth(float val) { m_sssDepth = val; }
    glm::vec3 getSssColor() const { return m_sssColor; }
    void setSssColor(const glm::vec3& col) { m_sssColor = col; }

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
    int findMatcapIndexByName(const std::string& name) const;
    bool ensureMatcapLoaded(int idx);

    std::vector<uint8_t> renderToBuffer(const Scene& scene, int w, int h);
    void drawMeshForThumbnail(Mesh* mesh, const Camera& cam);

    // Model Snapshot (Render-to-Texture frozen camera overlay)
    void createSnapshot(const Scene& scene);
    void updateSnapshotIfNeeded(const Scene& scene);
    void destroySnapshot();
    void toggleSnapshot(const Scene& scene);
    void updateSnapshotCamera(const Scene& scene);
    bool hasActiveSnapshot() const { return m_snapshot.active; }
    GLuint getSnapshotTexture() const { return m_snapshot.rt.texture; }
    int getSnapshotWidth() const { return m_snapshot.width; }
    int getSnapshotHeight() const { return m_snapshot.height; }
    void markSnapshotDirty() { if (m_snapshot.active) m_snapshot.needsUpdate = true; }
    bool isSnapshotSilhouette() const { return m_snapshot.isSilhouette; }
    void setSnapshotSilhouette(bool silhouette) {
        if (m_snapshot.isSilhouette != silhouette) {
            m_snapshot.isSilhouette = silhouette;
            if (m_snapshot.active) m_snapshot.needsUpdate = true;
        }
    }

    // Helpers to update environmental params
    void setEnvironmentParameters(float exposure, const std::vector<float>& sph);
    void setEnvironmentParametersFast(float exposure, uintptr_t sphPtr);
    
    void setSymmetryParameters(bool showSymmetryLine, const std::vector<float>& planeOrigin, const std::vector<float>& planeNormal);
    void setSymmetryParametersFast(bool showSymmetryLine, uintptr_t planeOriginPtr, uintptr_t planeNormalPtr);
    void setSymmetryPlanes(bool showSymmetryLine, const std::vector<SymmetryPlaneData>& planes);

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
    void setClipCurveOverlay(bool active, const std::vector<glm::vec2>& points, bool altMode = false);
    void setTrimLassoOverlay(bool active, const std::vector<glm::vec2>& points, bool altMode = false);

    void drawArmature(const ArmatureGraph& graph, const Camera& camera, void* selectedNode = nullptr, void* hoveredParent = nullptr, void* hoveredChild = nullptr, bool hasSymmetry = false, bool normalsPass = false);

    // Armature tool state for rendering
    void* m_armatureSelectedNode = nullptr;
    void* m_armatureHoveredParent = nullptr;
    void* m_armatureHoveredChild = nullptr;
    bool m_armatureHasSymmetry = false;

    void setArmatureState(void* selectedNode, void* hoveredParent, void* hoveredChild, bool hasSymmetry) {
        m_armatureSelectedNode = selectedNode;
        m_armatureHoveredParent = hoveredParent;
        m_armatureHoveredChild = hoveredChild;
        m_armatureHasSymmetry = hasSymmetry;
    }


    // Shader compilation helpers
    GLuint compileShader(GLenum type, const std::string& source);
    GLuint linkProgram(GLuint vs, GLuint fs);
    std::string loadShaderSource(const std::string& filename);
    GLuint loadAndCompileProgram(const std::string& vertFile, const std::string& fragFile);

    // Mesh buffer uploads
    bool uploadIfDirty(Mesh* mesh);

private:
    void generateTriangleIndices(const Mesh* mesh, std::vector<uint32_t>& outIndices);
    void generateWireframeIndices(const Mesh* mesh, std::vector<uint32_t>& outEdges);
    void buildPolyGroupBuffers(const Mesh* mesh, MeshRenderBuffers* bufs);

    void drawBackground(const Scene& scene, const Camera& camera);
    void updateBackgroundGeometry();
    void drawMesh(Mesh* mesh, const Scene& scene); // Keep for compatibility
    void drawMeshSolid(Mesh* mesh, const Scene& scene, const Camera& camera);
    void drawMeshPrepass(Mesh* mesh, const Scene& scene, const Camera& camera);
    void drawMeshNormals(Mesh* mesh, const Scene& scene, const Camera& camera);
    void drawMeshFlatColor(Mesh* mesh, const Scene& scene, const Camera& camera, const glm::vec4& color);
    void drawVoxelPreview(const Scene& scene, const Camera& camera, int viewportIdx);
    void drawWireframe(Mesh* mesh, const Scene& scene, const Camera& camera);
    void drawReferenceImages(const Scene& scene, const Camera& camera, int viewportIdx = 0);
    void drawSelectionCursor(const Scene& scene, bool isRight = false);
    void drawLasso();
    void drawClipCurve();
    void drawTrimLasso();

    void initGrid();
    void initArmatureGeometry();
    void initSsaoKernel();
    void initSsaoNoiseTexture();
    void drawGrid(const Scene& scene, const Camera& camera);
    void drawFullscreenMerge(const Scene& scene);
    void drawFullscreenTaa();
    void drawFullscreenFxaa();
    void drawFullscreenViewport2D(const Scene& scene);
    void drawContourOverlay(const Scene& scene);
    void initEnvironments();
    void loadEnvironmentTexture(int idx);
    void initMatcaps();

    void renderScenePass(const Scene& scene, int passType);
    void drawPassGeometry(const Scene& scene, int passType, const Camera& camera, int viewportIdx);
    void drawMeshGBuffer(Mesh* mesh, const Scene& scene, const Camera& camera);

    int m_width = 0;
    int m_height = 0;
    float m_dpiScale = 1.0f;
    bool m_showBackground = true;
    bool m_transparentClear = false;

    // Environmental parameters
    float m_exposure = 1.0f;
    float m_sph[27] = {0.0f};

    // Symmetry parameters
    bool m_showSymmetryLine = false;
    float m_symmetryLineWidth = 0.11f;
    float m_symmetryAnimTimer = 0.0f;
    float m_symmetryCurrentAlpha = 0.0f;
    float m_tempSymLineDuration = 1.8f;
    glm::vec3 m_planeOrigin{0.0f};
    glm::vec3 m_planeNormal{0.0f, 0.0f, 1.0f};
    glm::vec3 m_planeColor{1.0f, 0.25f, 0.25f};
    std::vector<SymmetryPlaneData> m_symmetryPlanes;

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

    // Clip curve parameters
    bool m_clipCurveActive = false;
    std::vector<glm::vec2> m_clipCurvePoints;
    bool m_clipCurveAlt = false;

    // Trim lasso parameters
    bool m_trimLassoActive = false;
    std::vector<glm::vec2> m_trimLassoPoints;
    bool m_trimLassoAlt = false;

    // Shader programs
    GLuint m_pbrProgram = 0;
    GLuint m_matcapProgram = 0;
    GLuint m_polygroupProgram = 0;
    GLuint m_flatProgram = 0;
    GLuint m_wireframeProgram = 0;
    GLuint m_bgProgram = 0;
    GLuint m_gridProgram = 0;
    GLuint m_selectionProgram = 0;
    GLuint m_refImageProgram = 0;
    GLuint m_mergeProgram = 0;
    GLuint m_fxaaProgram = 0;
    GLuint m_viewport2DProgram = 0;
    GLuint m_contourProgram = 0;
    GLuint m_wetClayProgram = 0;
    GLuint m_silhouetteProgram = 0;
    GLuint m_voxelCheckerProgram = 0;
    GLuint m_normalProgram = 0;
    GLuint m_bevelPrepassProgram = 0;
    GLuint m_bevelFilterProgram = 0;
    GLuint m_armatureProgram = 0;
    GLuint m_armatureNormalsProgram = 0;
    GLuint m_ssaoNormalsProgram = 0;
    GLuint m_ssaoProgram = 0;
    GLuint m_ssaoBlurProgram = 0;
    GLuint m_shadowProgram = 0;
    GLuint m_ssrProgram = 0;
    GLuint m_gbufferProgram = 0;
    GLuint m_ssptProgram = 0;
    GLuint m_svgfTemporalProgram = 0;
    GLuint m_svgfSpatialProgram = 0;
    GLuint m_taaProgram = 0;

    // RTT Targets
    RenderTarget m_rttContour;
    RenderTarget m_rttOpaque;
    RenderTarget m_rttTransparent;
    RenderTarget m_rttMerge;
    RenderTarget m_rttComposite;
    RenderTarget m_rttPrepass;
    RenderTarget m_rttBevel;
    RenderTarget m_rttNormals;
    RenderTarget m_rttSsao;
    RenderTarget m_rttSsaoBlur;
    RenderTarget m_rttShadow;
    RenderTarget m_rttSsr;
    RenderTarget m_rttAccumA;
    RenderTarget m_rttAccumB;
    RenderTarget m_rttSvgfA;
    RenderTarget m_rttSvgfB;
    RenderTarget m_rttMotionVectors;
    RenderTarget m_rttTaaAccumA;
    RenderTarget m_rttTaaAccumB;

    RenderMode m_renderMode = RenderMode::PBR;
    bool m_shadowEnabled = true;
    bool m_useSsr = false;
    float m_ssrIntensity = 0.5f;
    float m_ssrMaxDistance = 10.0f;
    glm::mat4 m_shadowLightMVP{1.0f};
    static constexpr int SHADOW_MAP_SIZE = 2048;

    bool m_accumPing = true;
    bool m_taaPing = true;
    int m_accumFrameCount = 0;
    int m_frameIndex = 0;

    // Fullscreen quad
    GLuint m_fsqVao = 0;
    GLuint m_fsqVbo = 0;

    // Grid geometry
    GLuint m_gridVao = 0;
    GLuint m_gridVbo = 0;
    int m_gridLineCount = 0;

    // Anti-Aliasing & Depth Precision Settings
    bool m_useFxaa = true;
    bool m_fxaaSharpMode = true;
    bool m_useTaa = true;
    bool m_taaResetOnStroke = true;
    bool m_taaResetHistory = true;
    bool m_useMotionVectors = true;
    bool m_useReverseZ = false;
    bool m_usePolygonOffset = true;
    float m_polyOffsetFactor = -1.0f;
    float m_polyOffsetUnits = -1.0f;
    int m_anisotropyLevel = 4;
    bool m_useSubpixelCulling = false;
    float m_subpixelCullThreshold = 0.5f;

    struct TaaState {
        int frameIndex = 0;
        glm::vec2 currentJitter{0.0f};
        glm::vec2 prevJitter{0.0f};
    } m_taaState;
    bool m_useSsao = true;
    float m_ssaoRadius = 0.5f;
    float m_ssaoBias = 0.025f;
    float m_ssaoIntensity = 1.0f;

    std::vector<glm::vec3> m_ssaoKernel;
    GLuint m_ssaoNoiseTexture = 0;

    int m_backgroundType = 0;
    float m_bgBlur = 0.0f;
    bool m_filmic = false;
    bool m_bevelEnabled = false;
    float m_bevelRadius = 4.0f;
    float m_bevelStrength = 1.5f;
    bool m_bevelScaleWithDistance = false;
    bool m_showContour = true;
    bool m_enableSelectionFlash = true;
    float m_selectionAnimTimer = 0.0f;
    float m_selectionAnimDuration = 1.2f;
    const Mesh* m_lastSelectedMesh = nullptr;
    std::chrono::high_resolution_clock::time_point m_lastRenderTime;
    bool m_showGrid = true;
    bool m_showSafeFrames = false;
    float m_safeFramesMargin = 20.0f;
    float m_safeFramesThickness = 1.5f;
    glm::vec4 m_contourColor{1.0f, 0.75f, 0.1f, 1.0f};
    float m_cursorThickness = 2.5f;
    bool m_smoothCursor = true;

    int m_shaderType = 0;
    int m_matcapIdx = 0;
    bool m_flatShading = false;
    bool m_darkenUnselected = true;
    bool m_showWireframe = false;
    bool m_showPolyGroups = false;
    bool m_xrayEnabled = false;
    int m_xrayTarget = 0; // 0: Unselected (default), 1: Selected, 2: All
    glm::vec4 m_xrayColor{0.2f, 0.6f, 1.0f, 0.5f};
    BrushType m_activeBrush = BRUSH_FLATTEN;
    float m_curvature = 0.0f;

    // Global material settings
    float m_albedo[3] = {0.72f, 0.52f, 0.45f};
    float m_roughness = 0.5f;
    float m_metallic = 0.0f;
    float m_alpha = 1.0f;
    unsigned int m_textureId = 0;
    bool m_hasUV = false;
    bool m_useVertexColors = false;
    bool m_useVertexMaterials = false;

    // Glass parameters
    float m_transmission = 0.0f;
    float m_ior = 1.5f;

    // SSS parameters (3 parameters: Intensity, Depth, Color)
    float m_sssIntensity = 0.0f;
    float m_sssDepth = 1.0f;
    glm::vec3 m_sssColor{0.8f, 0.3f, 0.15f};

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

    GLuint m_refImageVao = 0;
    GLuint m_refImageVbo = 0;

    GLuint m_selectionVao = 0;
    GLuint m_circleVbo = 0;
    GLuint m_dotVbo = 0;

    // GPU buffers cache for meshes
    GLuint m_armatureSphereVao = 0;
    GLuint m_armatureSphereVbo = 0;
    GLuint m_armatureSphereEbo = 0;
    int m_armatureSphereIndicesCount = 0;

    GLuint m_armatureCylVao = 0;
    GLuint m_armatureCylVbo = 0;
    GLuint m_armatureCylEbo = 0;
    int m_armatureCylIndicesCount = 0;

    std::unordered_map<const Mesh*, std::unique_ptr<MeshRenderBuffers>> m_meshBuffers;

    // Model Snapshot storage
    ModelSnapshot m_snapshot;

    // Uniform locations cache
    std::unordered_map<GLuint, std::unordered_map<std::string, GLint>> m_uniformLocations;
    GLint getCachedUniformLocation(GLuint program, const char* name);
};
