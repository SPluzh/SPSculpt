#pragma once
#include "common/Enums.h"
#include <SDL2/SDL.h>
#include <glm/glm.hpp>
#include "scene/Scene.h"
#include "editing/CameraController.h"
#include "editing/BrushCursor.h"



class SculptManager {
private:
    BrushType m_currentBrush = BRUSH_FLATTEN;
    float m_brushRadius = 8.0f;
    float m_brushIntensity = 0.5f;

    CameraController m_cameraController;
    bool m_isSculpting = false;

    glm::vec3 m_initialIntersection{0.0f};
    glm::vec3 m_initialIntersectionNormal{0.0f};
    glm::vec3 m_currentIntersection{0.0f};
    glm::vec3 m_currentIntersectionNormal{0.0f};

    int m_prevMouseX = 0;
    int m_prevMouseY = 0;

    BrushCursor m_cursor;
    bool m_useSym = false;
    int m_symAxis = 0; // 0=X, 1=Y, 2=Z

public:
    SculptManager();
    ~SculptManager() = default;

    void handleEvent(const SDL_Event& event, Scene& scene);
    void processFrame(Scene& scene);

    BrushType getBrush() const { return m_currentBrush; }
    void setBrush(BrushType brush) { 
        m_currentBrush = brush; 
        m_negative = (brush == BRUSH_FLATTEN || brush == BRUSH_CREASE || brush == BRUSH_VTOOL || brush == BRUSH_DAMSTANDARD);
    }
    void setTool(BrushType brush) { 
        m_currentBrush = brush; 
        m_negative = (brush == BRUSH_FLATTEN || brush == BRUSH_CREASE || brush == BRUSH_VTOOL || brush == BRUSH_DAMSTANDARD);
    }

    float getBrushRadius() const { return m_brushRadius; }
    void setBrushRadius(float radius) { m_brushRadius = radius; }

    float getBrushIntensity() const { return m_brushIntensity; }
    void setBrushIntensity(float intensity) { m_brushIntensity = intensity; }

    float getFocalShift() const { return m_focalShift; }
    void setFocalShift(float val) { m_focalShift = val; }

    float getHardness() const { return m_hardness; }
    void setHardness(float val) { m_hardness = val; }

    glm::vec3 getPaintColor() const { return m_paintColor; }
    void setPaintColor(const glm::vec3& color) { m_paintColor = color; }

    float getPaintRoughness() const { return m_paintRoughness; }
    void setPaintRoughness(float val) { m_paintRoughness = val; }

    float getPaintMetallic() const { return m_paintMetallic; }
    void setPaintMetallic(float val) { m_paintMetallic = val; }

    float getStylusPressure() const { return m_stylusPressure; }
    void setStylusPressure(float p) {
        m_stylusPressure = p;
        m_usingStylus = true;
        m_lastStylusTime = SDL_GetTicks();
    }

    bool getNegative() const { return m_negative; }
    void setNegative(bool val) { m_negative = val; }
    void toggleNegative() { m_negative = !m_negative; }

    bool getUseSym() const { return m_useSym; }
    void setUseSym(bool val) { m_useSym = val; }

    int getSymAxis() const { return m_symAxis; }
    void setSymAxis(int val) { m_symAxis = val; }

    const BrushCursor& getCursor() const { return m_cursor; }
    BrushCursor& getCursor() { return m_cursor; }

    CameraController& getCameraController() { return m_cameraController; }
    const CameraController& getCameraController() const { return m_cameraController; }

    bool isLassoActive() const { return m_isLassoActive; }
    const std::vector<glm::vec2>& getLassoPoints() const { return m_lassoPoints; }
    bool getLassoAlt() const { return m_lassoAlt; }

    std::vector<uint32_t> getVerticesInLasso(Mesh* mesh, const Camera& camera);

private:
    float m_focalShift = 0.0f;
    float m_hardness = 0.5f;
    glm::vec3 m_paintColor{1.0f, 0.8f, 0.6f};
    float m_paintRoughness = 0.5f;
    float m_paintMetallic = 0.0f;
    float m_stylusPressure = 1.0f;
    bool m_usingStylus = false;
    uint32_t m_lastStylusTime = 0;
    bool m_negative = false;

    bool m_isLassoActive = false;
    std::vector<glm::vec2> m_lassoPoints;
    bool m_lassoAlt = false;

    // Cache buffers for fast dirty faces lookup
    std::vector<uint32_t> m_tagFlags;
    uint32_t m_tagEpoch = 0;
    std::vector<uint32_t> m_iFacesCache;

    // Cache for computeAreaNormalAndCenter
    bool m_firstStrokeFrame = false;
    glm::vec3 m_cachedAreaNormal{0.0f};
    glm::vec3 m_cachedAreaCenter{0.0f};
};

