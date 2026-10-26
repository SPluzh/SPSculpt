#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <string>
#include "editing/Thickness.h"
#include "mesh/Mesh.h"

class Scene;
class Camera;
class SculptManager;

enum class ExtractSmoothMode {
    BorderOnly = 0,
    All = 1
};

enum class ExtractFillMode {
    Shell = 0, // Double-walled hollow shell with thickness (Front + Rim + Back)
    Fill  = 1, // Watertight solid volume capped across the boundary
    None  = 2  // Single-sided open surface without thickness or back
};

struct MaskExtractParams {
    float thickness = 0.03f;
    float offset = 1.0f; // 1.0 = Outward, 0.0 = Centered, -1.0 = Inward
    int   smoothIterations = 4;
    ExtractSmoothMode smoothMode = ExtractSmoothMode::BorderOnly;
    ExtractFillMode   fillMode   = ExtractFillMode::Shell;
    int   rimSegments = 1;
    bool  creaseRim = true;
    float maskThreshold = 0.5f;
};

class MaskExtractTool {
public:
    MaskExtractTool(SculptManager& sm);
    ~MaskExtractTool();

    bool isPreviewActive() const { return m_isPreviewActive; }
    bool isDragging() const { return m_isDragging; }

    bool startPreview(Scene& scene, Mesh* baseMesh);
    void updateSmoothAndTopology();
    void updateThicknessFast(float newThickness);
    bool accept(Scene& scene);
    void cancel(Scene& scene);

    void renderOverlay(Scene& scene, const Camera& camera, float viewportW, float viewportH);

    bool handleMouseDown(Scene& scene, const Camera& camera, float mouseX, float mouseY, bool isCtrl, bool isAlt, bool isShift);
    bool handleMouseMove(Scene& scene, const Camera& camera, float mouseX, float mouseY);
    bool handleMouseUp(Scene& scene);
    bool handleKeyDown(Scene& scene, int keycode);
    void notifyMeshDeleted(Mesh* mesh);

    MaskExtractParams& getParams() { return m_params; }
    const MaskExtractParams& getParams() const { return m_params; }
    Mesh* getPreviewMesh() const { return m_previewMesh; }
    Mesh* getBaseMesh() const { return m_baseMesh; }

private:
    SculptManager& m_sculpt;
    MaskExtractParams m_params;

    bool m_isPreviewActive = false;
    bool m_isDragging = false;
    bool m_isHovered = false;

    Mesh* m_baseMesh = nullptr;
    Mesh* m_previewMesh = nullptr;

    Mesh m_rawPatchMesh;
    Mesh m_smoothPatchMesh;
    std::vector<uint32_t> m_patchToBaseVertMap;
    std::vector<glm::vec3> m_patchBaseNormals;

    Thickness::ThicknessCache m_thickCache;
    std::vector<glm::vec3> m_fillBasePositions;
    std::vector<glm::vec3> m_fillNormals;

    glm::vec3 m_anchorWorld{0.0f};
    glm::vec3 m_normalWorld{0.0f, 1.0f, 0.0f};
    glm::vec3 m_handlePosWorld{0.0f};

    glm::vec2 m_dragStartMouse{0.0f};
    float m_dragStartThickness = 0.03f;

    bool extractRawPatch(const Mesh& baseMesh);
    void applySmoothing();
    void smoothSolidMesh(Mesh& mesh, int iters);
    void computeAnchorAndNormal();
    void rebuildPreviewMeshTopology();

    void buildShellTopology();
    void buildFillTopology();
    void buildNoneTopology();

    void updateShellGeometry(float newThickness);
    void updateFillGeometry(float newThickness);
    void updateNoneGeometry(float newThickness);
};
