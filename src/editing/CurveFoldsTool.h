#pragma once

#include "sculpt/CurveSpline.h"
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <string>

class Scene;
class Mesh;
class Camera;
class SculptManager;

struct FoldCurve {
    CurveSpline spline;            // Control points, Catmull-Rom interpolation, radius per point
    float depthMultiplier = 1.0f;  // Individual fold depth multiplier
    bool inverted = false;         // Invert (crease / ridge)
    bool isClosed = false;         // Closed loop or open line
};

class CurveFoldsTool {
public:
    CurveFoldsTool(SculptManager& sm);
    ~CurveFoldsTool();

    void onActivate(Scene& scene);
    void onDeactivate();

    bool handleMouseDown(Scene& scene, const Camera& camera, float mouseX, float mouseY, bool isCtrl, bool isAlt, bool isShift);
    bool handleMouseMove(Scene& scene, const Camera& camera, float mouseX, float mouseY);
    bool handleMouseUp(Scene& scene);

    void renderOverlay(Scene& scene, const Camera& camera, float viewportW, float viewportH);

    // Curve management
    void startNewCurve();
    void deleteActiveCurve();
    void clearAllCurves();

    // Apply / Cancel / Live Preview
    void applyFolds(Scene& scene);
    void cancelFolds(Scene& scene);
    void updateLivePreview(Scene& scene, bool force = false);

    void notifyMeshDeleted(Mesh* mesh);

    // Global fold parameters
    enum class FoldType {
        Ridge,       // Convex cloth roll/fold
        Valley,      // Concave crease / valley
        Overlap,     // Overlap (S-shaped profile with sideways shift)
        DoubleFold   // Double parallel fold with center trough
    };

    enum class CreationMode {
        FreehandStroke,
        ClickPoints
    };

    CreationMode creationMode = CreationMode::FreehandStroke;
    FoldType foldType = FoldType::Ridge;
    float baseRadius = 15.0f;       // Base fold influence width
    float foldDepth = 1.0f;         // Deformation height / depth
    float pinchStrength = 0.5f;     // Cloth pinch towards central ridge
    float sharpness = 1.0f;         // Ridge sharpness exponent
    float endTaperFalloff = 1.0f;   // Smooth falloff exponent at curve ends
    bool livePreviewEnabled = true;

    // Stroke smoothing & tolerance
    int strokeSmoothing = 2;
    float curveFitTolerance = 1.0f;

    // State query
    int getActiveCurveIndex() const { return m_activeCurveIdx; }
    void setActiveCurveIndex(int idx);
    size_t getCurveCount() const { return m_curves.size(); }
    const std::vector<FoldCurve>& getCurves() const { return m_curves; }
    std::vector<FoldCurve>& getCurves() { return m_curves; }

    bool isInteracting() const {
        return m_isDrawingStroke || m_isDraggingPoint || m_isDraggingRadius;
    }

private:
    SculptManager& m_sculpt;
    std::vector<FoldCurve> m_curves;
    int m_activeCurveIdx = -1;
    int m_selectedPointIdx = -1;
    int m_hoveredCurveIdx = -1;
    int m_hoveredPointIdx = -1;
    int m_hoveredRadiusHandleCurve = -1;
    int m_hoveredRadiusHandlePoint = -1;
    int m_hoveredSegmentCurve = -1;
    float m_hoveredSegmentT = 0.0f;

    // Interaction state
    bool m_isDrawingStroke = false;
    bool m_isDraggingPoint = false;
    bool m_isDraggingRadius = false;
    glm::vec2 m_dragStartMouse{0.0f};
    glm::vec3 m_dragStartPointPos{0.0f};
    float m_dragStartRadius = 1.0f;
    std::vector<CurvePoint> m_rawStroke;

    // Cached base geometry for non-destructive live preview
    Mesh* m_targetMesh = nullptr;
    std::vector<float> m_baseVerts;       // Base vertex positions (x, y, z)
    std::vector<float> m_baseNormals;     // Base vertex normals (x, y, z)
    std::vector<uint32_t> m_affectedVerts;// Vertex indices deformed in preview
    bool m_previewDirty = false;

    // Raycast & geometric helpers
    bool raycastSurface(
        const Scene& scene, const Camera& camera, float mouseX, float mouseY,
        glm::vec3& outPos, glm::vec3& outNorm, Mesh** outHitMesh = nullptr
    ) const;

    glm::vec3 intersectRayPlane(
        const Camera& camera, float mouseX, float mouseY,
        const glm::vec3& planePoint, const glm::vec3& planeNormal
    ) const;

    int hitTestAllCurves(const Camera& camera, float mouseX, float mouseY, int& outCurveIdx, int& outPointIdx, float thresholdPx = 14.0f) const;
    int hitTestCurveRadiusHandle(const Camera& camera, float mouseX, float mouseY, int& outCurveIdx, int& outPointIdx, float thresholdPx = 12.0f) const;
    int findClosestCurveSegment(const Camera& camera, float mouseX, float mouseY, int& outCurveIdx, float& outT, float thresholdPx = 12.0f) const;

    bool getPointHandleInfo(
        const Camera& camera, const CurveSpline& spline, int ptIdx,
        glm::vec3& outCenterWorld, glm::vec3& outHandleWorld,
        glm::vec2& outCenterScreen, glm::vec2& outHandleScreen,
        glm::vec3& outSideDir
    ) const;

    void ensureTargetMeshInitialized(Scene& scene);
    void applyDeformationInternal(Mesh* mesh);
    void renderFloatingHUD(Scene& scene, float viewportW, float viewportH);
};

