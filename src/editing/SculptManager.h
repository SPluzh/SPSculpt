#pragma once
#include "common/Enums.h"
#include <SDL2/SDL.h>
#include <glm/glm.hpp>
#include "scene/Scene.h"
#include "editing/CameraController.h"
#include "editing/BrushCursor.h"
#include <vector>
#include <string>

#include "brushes/BrushPreset.h"
#include "editing/PolyGroupTool.h"

class ArmatureTool;
class IniFile;

glm::vec3 reflectPointSymmetry(const glm::vec3& pt, const glm::vec3& sScale, const Mesh* mesh, SymmetryMode mode);
glm::vec3 reflectVectorSymmetry(const glm::vec3& vec, const glm::vec3& sScale, const Mesh* mesh, SymmetryMode mode);


struct BrushSettings {
    float radius = 50.0f;
    float intensity = 0.5f;
    float focalShift = 0.0f;
    bool focalShiftFalloff = true;
    float hardness = 0.5f;
    float spacing = 0.15f; // stroke spacing as fraction of radius
    bool negative = false;
    bool culling = false;  // backface culling
    bool accumulate = true;
    bool lockPosition = false;
    int idAlpha = 0; // alpha texture ID

    bool singlePolyGroup = false; // Affect only target polygroup under stroke start

    // Brush-specific
    bool clay = true; // For Brush / SquareBrush
    bool tangent = false; // For Smooth (tangential smoothing)
    bool topoCheck = false; // For Move / Elastic
    float elasticity = 1.0f; // For Elastic
    
    // Paint settings
    glm::vec3 paintColor{0.72f, 0.52f, 0.45f};
    float paintRoughness = 0.5f;
    float paintMetallic = 0.0f;
    bool writeAlbedo = true;
    bool writeRoughness = true;
    bool writeMetalness = true;
    bool pickColor = false;

    // Mask settings
    int maskSharpenBlurIterations = 4;
    float maskSharpenFactor = 1.0f;
    float maskExtractThickness = 0.05f;
    bool blurMaskedOnly = false;

    // --- ZBrush Preset Extension Fields ---
    StrokeMode strokeMode = StrokeMode::Dot;
    DeformMode deformMode = DeformMode::Normal;
    bool altmode = false;
    float lazyRadius = 0.0f;
    float lazySmooth = 0.0f;
    bool grabRadius = false;
    float grabRadiusScale = 0.28f;
    float areaNormalRadius = 0.4f;
    float areaPointRadius = 0.0f;
    float areaSharp = 0.0f;
    bool areaSampling = true;
    bool flattenLockNormal = false;
    bool flattenLockOrigin = false;
    bool smoothTaubin = false;
    float smoothTaubinInflate = 0.53f;
    float smoothTaubinShrink = 0.75f;
    bool smoothRelax = false;
    bool smoothStable = false;
    bool smoothStickyBorder = false;
    DepthFilter depthFilter;
    bool connectedTopology = false;
    bool onlyFrontFace = false;
    bool useDynamicTopology = false;
    float subdivFactor = 1.0f;
    float decimFactor = 1.0f;
    bool pressureIntensity = false;
    bool pressureRadius = false;
    bool useGlobalPressure = false;
    FalloffCurve falloff;

    // Parametric stamp settings
    int stampType = 0; // 0 = Circle, 1 = Polygon, 2 = Star, 3 = Ring, 4 = Rectangle
    int stampSides = 5;
    float stampInnerRatio = 0.5f;
    float stampAngle = 0.0f; // in degrees
    float stampBlur = 0.0f;  // edge blur, 0.0 to 1.0
    bool stampLockRotation = false;
    bool stampUseTilt = false;
};

struct MeasurementAnchor {
    enum Type { VERTEX, FREE } type = FREE;
    Mesh* mesh = nullptr;
    uint32_t vertIdx = 0;       // Если привязано к вершине
    glm::vec3 worldPos{0.0f};   // Координаты в мировом пространстве
};

struct MeasurementSegment {
    MeasurementAnchor vertA;
    MeasurementAnchor vertB;
    bool isReference = false;   // Для Measure: является ли отрезок эталоном
    std::string name = "";
};

class SculptManager {
private:
    BrushType m_currentBrush = BRUSH_MOVE;
    BrushSettings m_brushSettings[32]; // Increased to 32 to support BRUSH_COUNT safely

    std::unique_ptr<ArmatureTool> m_armatureTool;

    CameraController m_cameraController;
    bool m_isSculpting = false;
    bool m_cursorHidden = false;
    bool m_currentIntersectionValid = false;
    int m_rawMouseX = 0;
    int m_rawMouseY = 0;

    glm::vec3 m_initialIntersection{0.0f};
    glm::vec3 m_initialIntersectionNormal{0.0f};
    glm::vec3 m_currentIntersection{0.0f};
    glm::vec3 m_currentIntersectionNormal{0.0f};

    glm::vec3 m_lastValidIntersection{0.0f};
    glm::vec3 m_lastValidIntersectionNormal{0.0f, 1.0f, 0.0f};
    bool      m_hasAnyValidIntersection = false;
    glm::vec3 m_initialSymIntersection{0.0f};

    int m_prevMouseX = 0;
    int m_prevMouseY = 0;

    int m_lastStrokeX = 0;
    int m_lastStrokeY = 0;

    glm::vec3 m_alphaOrigin{0.0f};
    bool      m_hasAlphaOrigin = false;

    int m_mouseDownX = 0;
    int m_mouseDownY = 0;

    BrushCursor m_cursor;
    bool m_useSym = false;
    bool m_symX = true;
    bool m_symY = false;
    bool m_symZ = false;
    SymmetryMode m_symmetryMode = SymmetryMode::Local;
    ModalMode m_modalMode = ModalMode::NONE;

    std::vector<std::vector<uint32_t>> m_grabbedVerticesSyms;
    std::vector<glm::vec3>             m_initialSymIntersections;

    PolyGroupTool m_polyGroupTool;
    uint32_t m_activeGroupId = 1;

public:
    SculptManager();
    ~SculptManager();

    void handleEvent(const SDL_Event& event, Scene& scene);
    void processFrame(Scene& scene);
    void executeStroke(Scene& scene, Mesh* mesh, Camera& camera, float mouseX, float mouseY, float currentPressure);
    void cancelStroke();

    bool saveSettings(IniFile& ini);
    bool loadSettings(const IniFile& ini);

    bool isSculpting() const { return m_isSculpting; }
    void setRawMousePos(int x, int y) {
        m_rawMouseX = x;
        m_rawMouseY = y;
    }

    BrushSettings& getCurrentSettings() { return m_brushSettings[m_currentBrush]; }
    const BrushSettings& getCurrentSettings() const { return m_brushSettings[m_currentBrush]; }
    BrushSettings& getSettings(BrushType type) { return m_brushSettings[type]; }
    const BrushSettings& getSettings(BrushType type) const { return m_brushSettings[type]; }

    PolyGroupTool& getPolyGroupTool() { return m_polyGroupTool; }
    const PolyGroupTool& getPolyGroupTool() const { return m_polyGroupTool; }
    uint32_t getActiveGroupID() const { return m_activeGroupId; }
    void setActiveGroupID(uint32_t id) { m_activeGroupId = id; }

    BrushType getBrush() const { return m_currentBrush; }
    void setBrush(BrushType brush) { 
        m_currentBrush = brush; 
        m_gradActivePoint = '\0';
        m_gradIsDrawing = false;
    }
    void setTool(BrushType brush) { 
        m_currentBrush = brush; 
        m_gradActivePoint = '\0';
        m_gradIsDrawing = false;
    }

    float getBrushRadius() const { return getCurrentSettings().radius; }
    void setBrushRadius(float radius) { getCurrentSettings().radius = radius; }

    float getBrushIntensity() const { return getCurrentSettings().intensity; }
    void setBrushIntensity(float intensity) { getCurrentSettings().intensity = intensity; }

    float getFocalShift() const { return getCurrentSettings().focalShift; }
    void setFocalShift(float val) { getCurrentSettings().focalShift = val; }

    float getHardness() const { return getCurrentSettings().hardness; }
    void setHardness(float val) { getCurrentSettings().hardness = val; }

    glm::vec3 getPaintColor() const { return getCurrentSettings().paintColor; }
    void setPaintColor(const glm::vec3& color) { getCurrentSettings().paintColor = color; }

    float getPaintRoughness() const { return getCurrentSettings().paintRoughness; }
    void setPaintRoughness(float val) { getCurrentSettings().paintRoughness = val; }

    float getPaintMetallic() const { return getCurrentSettings().paintMetallic; }
    void setPaintMetallic(float val) { getCurrentSettings().paintMetallic = val; }

    float getStylusPressure() const { return m_stylusPressure; }
    void setStylusPressure(float p) {
        m_stylusPressure = p;
        m_usingStylus = true;
        m_lastStylusTime = SDL_GetTicks();
    }

    bool getNegative() const { return getCurrentSettings().negative; }
    void setNegative(bool val) { getCurrentSettings().negative = val; }
    void toggleNegative() { getCurrentSettings().negative = !getCurrentSettings().negative; }

    bool getUseSym() const { return m_useSym && (m_symX || m_symY || m_symZ); }
    void setUseSym(bool val) {
        m_useSym = val;
        if (val && !m_symX && !m_symY && !m_symZ) {
            m_symX = true;
        }
    }

    bool getSymX() const { return m_symX; }
    void setSymX(bool val) {
        m_symX = val;
        if (m_symX || m_symY || m_symZ) m_useSym = true;
    }

    bool getSymY() const { return m_symY; }
    void setSymY(bool val) {
        m_symY = val;
        if (m_symX || m_symY || m_symZ) m_useSym = true;
    }

    bool getSymZ() const { return m_symZ; }
    void setSymZ(bool val) {
        m_symZ = val;
        if (m_symX || m_symY || m_symZ) m_useSym = true;
    }

    int getSymAxis() const {
        if (m_symX) return 0;
        if (m_symY) return 1;
        if (m_symZ) return 2;
        return 0;
    }
    void setSymAxis(int val) {
        m_symX = (val == 0);
        m_symY = (val == 1);
        m_symZ = (val == 2);
        m_useSym = true;
    }

    SymmetryMode getSymmetryMode() const { return m_symmetryMode; }
    void setSymmetryMode(SymmetryMode mode) {
        m_symmetryMode = mode;
    }

    std::vector<glm::vec3> getActiveSymmetryScales() const;

    ModalMode getModalMode() const { return m_modalMode; }
    void setModalMode(ModalMode mode) { m_modalMode = mode; }

    const BrushCursor& getCursor() const { return m_cursor; }
    BrushCursor& getCursor() { return m_cursor; }

    CameraController& getCameraController() { return m_cameraController; }
    const CameraController& getCameraController() const { return m_cameraController; }

    bool isLassoActive() const { return m_isLassoActive; }
    const std::vector<glm::vec2>& getLassoPoints() const { return m_lassoPoints; }
    bool getLassoAlt() const { return m_lassoAlt; }
    bool isMaskLasso() const { return m_isMaskLasso; }

    std::vector<uint32_t> getVerticesInLasso(Mesh* mesh, const Camera& camera);

    void clearMask(Mesh* mesh, Scene* scene = nullptr);
    void invertMask(Mesh* mesh);
    void blurMask(Mesh* mesh, int iterations = 2);
    void sharpenMask(Mesh* mesh, int iterations = 2);

    void paintAll(Scene& scene, Mesh* mesh);
    void pickColor(Mesh* mesh, uint32_t faceId, const glm::vec3& interPoint);

    bool getGradActive() const { return m_gradActive; }
    void setGradActive(bool val) { m_gradActive = val; }
    glm::vec2 getGradPointA() const { return m_gradPointA; }
    glm::vec2 getGradPointB() const { return m_gradPointB; }
    void setGradPointA(glm::vec2 p) { m_gradPointA = p; }
    void setGradPointB(glm::vec2 p) { m_gradPointB = p; }
    bool isGradDrawing() const { return m_gradIsDrawing; }

    const std::vector<MeasurementSegment>& getMeasureSegments() const { return m_measureSegments; }
    std::vector<MeasurementSegment>& getMeasureSegments() { return m_measureSegments; }

    const std::vector<MeasurementSegment>& getDividerSegments() const { return m_dividerSegments; }
    std::vector<MeasurementSegment>& getDividerSegments() { return m_dividerSegments; }

    const MeasurementAnchor& getPendingAnchorA() const { return m_pendingAnchorA; }
    MeasurementAnchor& getPendingAnchorA() { return m_pendingAnchorA; }

    const MeasurementAnchor& getPendingAnchorB() const { return m_pendingAnchorB; }
    MeasurementAnchor& getPendingAnchorB() { return m_pendingAnchorB; }

    bool hasPending() const { return m_hasPending; }
    void setHasPending(bool val) { m_hasPending = val; }

    int getDividerDivisions() const { return m_dividerDivisions; }
    void setDividerDivisions(int val) { m_dividerDivisions = val; }

    bool getMeasureUseDistanceThickness() const { return m_measureUseDistanceThickness; }
    void setMeasureUseDistanceThickness(bool val) { m_measureUseDistanceThickness = val; }

    bool isMeasureVisible(int vpIdx = 0) const { return vpIdx == 1 ? m_isMeasureVisibleV2 : m_isMeasureVisibleV1; }
    void setMeasureVisible(bool vis, int vpIdx = 0) {
        if (vpIdx == 1) m_isMeasureVisibleV2 = vis;
        else m_isMeasureVisibleV1 = vis;
    }
    bool getMeasureVisibleV1() const { return m_isMeasureVisibleV1; }
    void setMeasureVisibleV1(bool vis) { m_isMeasureVisibleV1 = vis; }
    bool getMeasureVisibleV2() const { return m_isMeasureVisibleV2; }
    void setMeasureVisibleV2(bool vis) { m_isMeasureVisibleV2 = vis; }

    bool isDividerVisible(int vpIdx = 0) const { return vpIdx == 1 ? m_isDividerVisibleV2 : m_isDividerVisibleV1; }
    void setDividerVisible(bool vis, int vpIdx = 0) {
        if (vpIdx == 1) m_isDividerVisibleV2 = vis;
        else m_isDividerVisibleV1 = vis;
    }
    bool getDividerVisibleV1() const { return m_isDividerVisibleV1; }
    void setDividerVisibleV1(bool vis) { m_isDividerVisibleV1 = vis; }
    bool getDividerVisibleV2() const { return m_isDividerVisibleV2; }
    void setDividerVisibleV2(bool vis) { m_isDividerVisibleV2 = vis; }

    void clearMeasurements() {
        m_measureSegments.clear();
        m_dividerSegments.clear();
        m_hasPending = false;
        m_draggedSegment = nullptr;
        m_hoveredSegment = nullptr;
    }

    const MeasurementSegment* getHoveredSegment() const { return m_hoveredSegment; }
    std::string getHoveredVertexKey() const { return m_hoveredVertexKey; }
    const MeasurementSegment* getDraggedSegment() const { return m_draggedSegment; }
    std::string getDraggedVertexKey() const { return m_draggedVertexKey; }

    static glm::vec3 getAnchorWorldPos(const MeasurementAnchor& anchor);
    MeasurementAnchor pickAnchor(float mouseX, float mouseY, Scene& scene, const glm::vec3* referenceWorldPos);
    void validateSegments(Scene& scene);

    void applyPreset(const BrushPreset& preset);
    void applyActivePreset();

    ArmatureTool* getArmatureTool() const { return m_armatureTool.get(); }

private:
    float m_stylusPressure = 1.0f;
    bool m_usingStylus = false;
    uint32_t m_lastStylusTime = 0;

    bool m_isLassoActive = false;
    std::vector<glm::vec2> m_lassoPoints;
    bool m_lassoAlt = false;
    bool m_isMaskLasso = false;

    // Cache buffers for fast dirty faces lookup
    std::vector<uint32_t> m_tagFlags;
    uint32_t m_tagEpoch = 0;
    std::vector<uint32_t> m_iFacesCache;
    std::vector<uint32_t> m_pickedVerticesCache;
    std::vector<uint32_t> m_allAffectedVertsCache;

    // Cache for computeAreaNormalAndCenter & inverse mesh matrix
    bool m_firstStrokeFrame = false;
    glm::vec3 m_cachedAreaNormal{0.0f};
    glm::vec3 m_cachedAreaCenter{0.0f};
    glm::mat4 m_cachedInvMatrix{1.0f};
    glm::mat4 m_cachedCamWorldMatrix{1.0f};

    // Gradient mask state variables
    glm::vec2 m_gradPointA{0.0f};
    glm::vec2 m_gradPointB{0.0f};
    bool m_gradActive = false;
    char m_gradActivePoint = '\0'; // 'A' (masked), 'B' (unmasked), or '\0'
    bool m_gradIsDrawing = false;

    // Buffers for mask blurring (Laplacian Blur)
    std::vector<float> m_origMasks;
    std::vector<float> m_blurredMasks;
    std::vector<uint32_t> m_gradActiveVerts;

    // Grab/Move/Elastic brush cached vertices
    std::vector<uint32_t> m_grabbedVertices;
    std::vector<uint32_t> m_grabbedVerticesSym;

    // Measurement & Divider states
    std::vector<MeasurementSegment> m_measureSegments;
    std::vector<MeasurementSegment> m_dividerSegments;

    MeasurementAnchor m_pendingAnchorA;
    MeasurementAnchor m_pendingAnchorB;
    bool m_hasPending = false;

    MeasurementSegment* m_draggedSegment = nullptr;
    std::string m_draggedVertexKey = ""; // "vertA" или "vertB"
    MeasurementSegment* m_hoveredSegment = nullptr;
    std::string m_hoveredVertexKey = "";

    int m_dividerDivisions = 3; // От 2 до 6
    bool m_measureUseDistanceThickness = true;

    bool m_isMeasureVisibleV1 = true;
    bool m_isMeasureVisibleV2 = true;
    bool m_isDividerVisibleV1 = true;
    bool m_isDividerVisibleV2 = true;

    uint32_t m_strokeTargetPolyGroup = 0;
    void filterPolyGroupVertices(std::vector<uint32_t>& pickedVertices, const Mesh* mesh, uint32_t targetGroupId);

    int doStrokePass(
        Scene& scene,
        Mesh* mesh,
        BrushType activeBrush,
        bool negative,
        std::vector<uint32_t>& pickedVertices,
        const glm::vec3& currentIntersection,
        const glm::vec3& currentIntersectionNormal,
        const glm::vec3& initialIntersection,
        const glm::vec3& cachedAreaNormal,
        const glm::vec3& cachedAreaCenter,
        float localRadius,
        float intensity,
        float mouseX,
        bool isSymmetry = false,
        const glm::vec3& sScale = glm::vec3(1.0f)
    );
};


