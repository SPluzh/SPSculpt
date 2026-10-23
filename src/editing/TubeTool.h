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
class GuiManager;

#include "sculpt/TubeData.h"

class TubeTool {
public:
    TubeTool(SculptManager& sm);
    ~TubeTool();

    void onActivate(Scene& scene);
    void onDeactivate();

    bool handleMouseDown(Scene& scene, const Camera& camera, float mouseX, float mouseY, bool isCtrl, bool isAlt, bool isShift);
    bool handleMouseMove(Scene& scene, const Camera& camera, float mouseX, float mouseY);
    bool handleMouseUp(Scene& scene);

    void renderOverlay(Scene& scene, const Camera& camera, float viewportW, float viewportH);

    // Multi-instance & Procedural Tube Persistence
    void syncToActiveMesh();
    bool loadFromMesh(Mesh* mesh, bool forceReload = false);
    void startNewTube(Scene& scene);
    Mesh* bakeToMesh(Scene& scene);
    Mesh* bakeMesh(Scene& scene, Mesh* mesh);
    void bakeAllTubes(Scene& scene);
    void notifyMeshDeleted(Mesh* mesh);

    void clearCurve(Scene* scene = nullptr);

    // Geometry update
    void markGeometryDirty() { m_geometryDirty = true; }
    void updateGeometryIfNeeded(Scene& scene);
    void setBaseRadius(float newRadius);

    // Parameters
    TubeCreationMode creationMode = TubeCreationMode::FreehandStroke;
    TubeProfileType profileType = TubeProfileType::Round;
    TubeCapType capType = TubeCapType::Rounded;

    // Freehand stroke smoothing & simplification
    int strokeSmoothing = 2;       // Smoothing passes to average out hand tremors (default: 2)
    float curveFitTolerance = 1.0f;// Simplification tolerance (higher = fewer points, lower = more points)

    float baseRadius = 2.5f;
    float startRadiusScale = 1.0f; // Scale at start
    float endRadiusScale = 1.0f;   // Scale at end (1.0 = uniform cylinder by default)
    int curveSegments = 64;        // Ring samples along length
    bool autoDensity = true;       // Adaptive segment count based on curve length & radius
    int cornerSegments = 8;        // Cross-sections at sharp corners (prevents tube creasing/pinching)
    int radialSegments = 16;       // Circumference sides
    float ribbonThickness = 0.25f; // Relative thickness for flat ribbon
    float twistDegrees = 0.0f;     // Roll/twist along length
    bool snapToSurface = true;     // Stick to underlying mesh
    float surfaceOffset = 0.0f;    // Distance from mesh surface

    // Longitudinal Radius Profile Graph
    bool useRadiusProfile = false;
    std::vector<ProfilePoint2D> radiusProfile = { {0.0f, 1.0f, false}, {1.0f, 1.0f, false} };
    bool radiusProfileSmooth = true;

    // Cross-Section 2D Profile Contour
    std::vector<ProfilePoint2D> crossSectionProfile;
    bool crossSectionSmooth = true;
    bool showProfileAtEnd = true;  // Interactively edit cross-section contour at the tube end in viewport

    // Profile Preset & Reset Helpers
    void setRadiusProfilePreset(int presetIdx);
    void resetRadiusProfile();
    void setCrossSectionPreset(int presetIdx);
    void resetCrossSectionProfile();

    CurveSpline& getSpline() { return m_spline; }
    const CurveSpline& getSpline() const { return m_spline; }
    bool hasCurve() const { return m_spline.getPointCount() >= 2; }
    Mesh* getActiveTubeMesh() const { return m_activeTubeMesh; }

    int getSelectedPointIndex() const { return m_selectedPointIdx; }
    void setSelectedPointIndex(int idx) { m_selectedPointIdx = idx; }

    bool isInteracting() const {
        return m_isDrawingStroke || m_isDraggingPoint || m_isDraggingRadius || m_isDraggingProfilePoint;
    }

    void resetActiveTube() {
        syncToActiveMesh();
        m_activeTubeMesh = nullptr;
        m_spline.clear();
        m_rawStroke.clear();
        m_strokeStartHitMesh = nullptr;
        m_selectedPointIdx = -1;
        m_hoveredPointIdx = -1;
        m_hoveredProfilePtIdx = -1;
        m_selectedProfilePtIdx = -1;
    }

    void setBaseMesh(Mesh* mesh) { m_baseMesh = mesh; }
    Mesh* getBaseMesh() const { return m_baseMesh; }

    int hitTestControlPoints(const Camera& camera, float mouseX, float mouseY, float thresholdPx = 14.0f) const;
    int hitTestTubeControlPoints(const Camera& camera, const CurveSpline& spline, float mouseX, float mouseY, float thresholdPx = 14.0f) const;
    int hitTestRadiusHandle(const Camera& camera, float mouseX, float mouseY, float thresholdPx = 12.0f) const;
    int findClosestSegmentOnCurve(const Camera& camera, float mouseX, float mouseY, float thresholdPx = 12.0f, float* outT = nullptr) const;
    int findClosestSegmentOnSpline(const Camera& camera, const CurveSpline& spline, float mouseX, float mouseY, float thresholdPx = 12.0f, float* outT = nullptr) const;
    int hitTestEndProfilePoint(const Camera& camera, float mouseX, float mouseY, float thresholdPx = 14.0f) const;
    void rebuildMeshGeometry(Scene& scene);
    void rebuildTubeMesh(Scene& scene, Mesh* tubeMesh);

private:
    SculptManager& m_sculpt;
    CurveSpline m_spline;
    Mesh* m_activeTubeMesh = nullptr;
    Mesh* m_baseMesh = nullptr; // Target mesh for union/remesh
    bool m_geometryDirty = false;

    // Interaction state
    bool m_isDrawingStroke = false;
    std::vector<CurvePoint> m_rawStroke;
    Mesh* m_strokeStartHitMesh = nullptr;

    int m_hoveredPointIdx = -1;
    int m_selectedPointIdx = -1;
    int m_snapTargetPointIdx = -1;
    int m_hoveredCurveSeg = -1;
    float m_hoveredCurveT = 0.5f;
    glm::vec2 m_hoveredCurveScreenPos{0.0f, 0.0f};

    int m_hoveredRadiusHandleIdx = -1;
    bool m_hoveredRadiusRing = false;
    bool m_isDraggingPoint = false;
    bool m_isDraggingRadius = false;
    bool m_pointWasMoved = false;
    bool m_isExtrudingPoint = false;
    int m_extrudeEndIdx = -1;

    // Viewport End Profile Interaction
    int m_hoveredProfilePtIdx = -1;
    int m_selectedProfilePtIdx = -1;
    int m_snapTargetProfilePtIdx = -1;
    int m_hoveredProfileSeg = -1;
    glm::vec2 m_hoveredProfile2DPos{0.0f, 0.0f};
    glm::vec2 m_hoveredProfileScreenPos{0.0f, 0.0f};
    bool m_isDraggingProfilePoint = false;
    bool m_profilePointWasMoved = false;
    glm::vec2 m_dragProfileStartMouse{0.0f, 0.0f};

    // Drag plane & starting info
    glm::vec3 m_dragPlanePoint{0.0f};
    glm::vec3 m_dragPlaneNormal{0.0f};
    glm::vec2 m_dragStartMouse{0.0f};
    glm::vec3 m_dragStartPointPos{0.0f};
    float m_dragStartRadius = 1.0f;

    // Helpers
    bool raycastSurface(const Scene& scene, const Camera& camera, float mouseX, float mouseY, glm::vec3& outPos, glm::vec3& outNorm, Mesh** outHitMesh = nullptr) const;
    glm::vec3 intersectRayPlane(const Camera& camera, float mouseX, float mouseY, const glm::vec3& planePoint, const glm::vec3& planeNormal) const;
    bool getPointHandleInfo(const Camera& camera, int ptIdx, glm::vec3& outCenterWorld, glm::vec3& outHandleWorld, glm::vec2& outCenterScreen, glm::vec2& outHandleScreen, glm::vec3& outSideDir) const;
    bool hitTestRadiusRing(const Camera& camera, float mouseX, float mouseY, int ptIdx, float thresholdPx = 8.0f) const;

    bool getEndProfileFrame(EvaluatedFrame& outFrame, int* outPointIdx = nullptr) const;
    glm::vec2 evaluateCrossSectionContour(float u) const;
    bool hitTestEndProfileContour(
        const Camera& camera, float mouseX, float mouseY, float thresholdPx,
        int* outSeg = nullptr, glm::vec2* out2DPos = nullptr, glm::vec2* outScreenPos = nullptr
    ) const;
};
