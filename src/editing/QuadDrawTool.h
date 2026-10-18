#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <unordered_set>
#include <memory>
#include <string>
#include <algorithm>
#include "imgui.h"
#include "retopo/SurfaceSnapper.h"
#include "retopo/QuadDrawMeshBuilder.h"
#include "scene/Scene.h"
#include "scene/Camera.h"

class SculptManager;

namespace retopo {

enum class QuadDrawSubTool {
    Extrude = 0,        // Place points & border edge extrude
    CreatePolygon,      // Quad / polygon creation
    Tweak,              // Move components with weld
    Knife,              // Insert edge loops
    MultiCut,           // Point-to-point and screen slice cut
    Delete,             // Component & loop deletion
    Grab,               // Soft move brush
    Pin,                // Lock/freeze components
    Select,             // Select border edges for extrude
    Strip               // Draw stroke opposite selected edges to build quads
};

enum class DragMode {
    None = 0,
    ExtrudeDrag,
    ExtrudeVertexDrag,
    TweakDrag,
    GrabDrag,
    RelaxDrag,
    PinDrag,
    SliceDrag,
    BrushResize,
    StrokeDrag
};

class QuadDrawTool {
public:
    explicit QuadDrawTool(SculptManager& sm);
    ~QuadDrawTool();

    void onActivate(Scene& scene);
    void onDeactivate();

    bool handleMouseDown(Scene& scene, const Camera& camera, float mouseX, float mouseY, bool ctrl, bool alt, bool shift, int button);
    bool handleMouseMove(Scene& scene, const Camera& camera, float mouseX, float mouseY, bool ctrl, bool alt, bool shift);
    bool handleMouseUp(Scene& scene, const Camera& camera, int button);
    bool handleKeyDown(Scene& scene, int keycode);

    void renderOverlay(Scene& scene, const Camera& camera, float vW, float vH);
    void ensureTargetAndRetopo(Scene& scene, bool createIfMissing = false);
    void onSceneTopologyRestored(Scene& scene);
    void notifyMeshDeleted(Mesh* mesh);

    // Getters and setters for settings
    QuadDrawSubTool getSubTool() const { return m_subTool; }
    void setSubTool(QuadDrawSubTool st) { m_subTool = st; }

    Mesh* getTargetMesh();
    void setTargetMesh(Mesh* target);

    Mesh* getRetopoMesh();
    void setRetopoMesh(Mesh* retopo);

    float getBrushRadius() const { return m_brushRadius; }
    void setBrushRadius(float r) { m_brushRadius = std::clamp(r, 5.0f, 500.0f); }

    float getBrushStrength() const { return m_brushStrength; }
    void setBrushStrength(float s) { m_brushStrength = std::clamp(s, 0.01f, 1.0f); }

    int getBrushFalloff() const { return m_brushFalloff; }
    void setBrushFalloff(int f) { m_brushFalloff = f; }

    float getWeldRadiusPixels() const { return m_weldRadiusPixels; }
    void setWeldRadiusPixels(float r) { m_weldRadiusPixels = r; }

    bool getRelaxAutoLock() const { return m_relaxAutoLock; }
    void setRelaxAutoLock(bool val) { m_relaxAutoLock = val; }

    bool getRelaxLockBorder() const { return m_relaxLockBorder; }
    void setRelaxLockBorder(bool val) { m_relaxLockBorder = val; }

    bool getRelaxLockInterior() const { return m_relaxLockInterior; }
    void setRelaxLockInterior(bool val) { m_relaxLockInterior = val; }

    bool getShowWireframe() const { return m_showWireframe; }
    void setShowWireframe(bool val) { m_showWireframe = val; }

    const ImVec4& getWireColor() const { return m_wireColor; }
    void setWireColor(const ImVec4& col) { m_wireColor = col; }

    float getPointSize() const { return m_pointSize; }
    void setPointSize(float s) { m_pointSize = s; }

    float getLineWidth() const { return m_lineWidth; }
    void setLineWidth(float w) { m_lineWidth = w; }

    bool getXRay() const { return m_xRay; }
    void setXRay(bool val) { m_xRay = val; }

    // Utilities
    void snapAllPointsToTarget(Scene& scene);
    void deleteAllUnconnectedPoints(Scene& scene);
    void pinBorderVertices();
    void pinAll();
    void unpinAll();
    void invertPins();
    size_t getPinnedCount() const { return m_pinnedVertices.size(); }
    const std::unordered_set<uint32_t>& getPinnedVertices() const { return m_pinnedVertices; }

    // Edge Selection
    bool isEdgeSelected(int32_t v0, int32_t v1) const;
    void selectEdge(int32_t v0, int32_t v1);
    void deselectEdge(int32_t v0, int32_t v1);
    void toggleEdgeSelection(int32_t v0, int32_t v1);
    void clearSelectedEdges();
    size_t getSelectedEdgeCount() const { return m_selectedEdges.size(); }
    const std::vector<std::pair<int32_t, int32_t>>& getSelectedEdges() const { return m_selectedEdges; }
    void pruneSelectedEdges();
    void deleteSelectedEdges(Scene& scene);

    QuadDrawMeshBuilder& getBuilder() { return m_builder; }
    SurfaceSnapper& getSnapper() { return m_snapper; }

private:
    SculptManager& m_sculpt;
    QuadDrawMeshBuilder m_builder;
    SurfaceSnapper m_snapper;

    QuadDrawSubTool m_subTool = QuadDrawSubTool::Extrude;
    DragMode m_dragMode = DragMode::None;

    Scene* m_scene = nullptr;
    Mesh* m_targetMesh = nullptr;
    Mesh* m_retopoMesh = nullptr;

    std::unordered_set<uint32_t> m_pinnedVertices;

    float m_brushRadius = 75.0f;
    float m_brushStrength = 0.5f;
    int m_brushFalloff = 0; // 0=Hermite, 1=Linear, 2=Dome, 3=Spike
    float m_weldRadiusPixels = 12.0f;

    bool m_relaxAutoLock = true;
    bool m_relaxLockBorder = false;
    bool m_relaxLockInterior = false;

    bool m_showWireframe = true;
    ImVec4 m_wireColor{0.2f, 0.9f, 0.35f, 0.9f};
    float m_pointSize = 5.0f;
    float m_lineWidth = 1.5f;
    bool m_xRay = false;

    // Hover detection state
    SnapResult m_hoverSnap;
    EdgeHit m_hoverEdge;
    QuadPreview m_hoverQuad;
    EdgeCutResult m_hoverCut;
    MultiCutPoint m_hoverMultiCutPoint;
    EdgeLoopResult m_hoverEdgeLoop;
    std::vector<int32_t> m_hoverPolyLoop;
    int32_t m_hoverVertex = -1;
    int32_t m_hoverPoly = -1;
    int32_t m_weldTargetVertex = -1;
    int32_t m_extrudeWeldTarget0 = -1;
    int32_t m_extrudeWeldTarget1 = -1;
    int32_t m_extrudeVertexWeldTarget = -1;

    // Dragging state
    float m_dragStartX = 0.0f;
    float m_dragStartY = 0.0f;
    float m_lastMouseX = 0.0f;
    float m_lastMouseY = 0.0f;
    int32_t m_dragVertex = -1;
    int32_t m_dragEdgeV0 = -1;
    int32_t m_dragEdgeV1 = -1;
    glm::vec3 m_dragEdgeInitialPos0{0.0f};
    glm::vec3 m_dragEdgeInitialPos1{0.0f};
    glm::vec3 m_dragStartWorldPos{0.0f};
    float m_dragEdgeScreenZ = 0.5f;
    int32_t m_extrudedV0 = -1;
    int32_t m_extrudedV1 = -1;
    int32_t m_extrudedSingleV = -1;
    int32_t m_extrudeCornerV = -1;
    int32_t m_extrudeCornerVA = -1;
    int32_t m_extrudeCornerVB = -1;

    struct ExtrudedVertexDragInfo {
        int32_t origV = -1;
        int32_t newV = -1;
        glm::vec3 initialWorldPos{0.0f};
        int32_t weldTarget = -1;
    };
    std::vector<ExtrudedVertexDragInfo> m_extrudedVerts;
    std::vector<std::pair<int32_t, int32_t>> m_extrudedFrontEdges;
    std::vector<std::pair<int32_t, int32_t>> m_selectedEdges;

    std::vector<GrabVertexInfo> m_grabbedVerts;
    glm::vec3 m_grabCenterWorld{0.0f};

    std::vector<MultiCutPoint> m_multiCutPoints;
    glm::vec2 m_sliceStartScreen{0.0f};
    glm::vec2 m_sliceEndScreen{0.0f};

    // Strip / Stroke Extrude state
    std::vector<glm::vec2> m_strokeScreenPoints;
    std::vector<glm::vec3> m_strokeWorldPoints;
    std::vector<QuadPreview> m_strokeQuadPreviews;
    bool m_edgeClickCandidate = false;
    EdgeHit m_candidateEdge;

    void updateHoverState(const Camera& camera, float mouseX, float mouseY, bool ctrl, bool alt, bool shift);
    void commitMultiCut(Scene& scene);
    void updateStrokePreview(const Camera& camera);
    void commitStrokeExtrude(Scene& scene, const Camera& camera);
    void cancelStroke();
};

} // namespace retopo
