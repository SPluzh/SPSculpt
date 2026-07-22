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
        m_negative = (brush == BRUSH_FLATTEN || brush == BRUSH_CREASE || brush == BRUSH_VTOOL);
    }
    void setTool(BrushType brush) { 
        m_currentBrush = brush; 
        m_negative = (brush == BRUSH_FLATTEN || brush == BRUSH_CREASE || brush == BRUSH_VTOOL);
    }

    float getBrushRadius() const { return m_brushRadius; }
    void setBrushRadius(float radius) { m_brushRadius = radius; }

    float getBrushIntensity() const { return m_brushIntensity; }
    void setBrushIntensity(float intensity) { m_brushIntensity = intensity; }

    float getFocalShift() const { return m_focalShift; }
    void setFocalShift(float val) { m_focalShift = val; }

    float getHardness() const { return m_hardness; }
    void setHardness(float val) { m_hardness = val; }

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

private:
    float m_focalShift = 0.0f;
    float m_hardness = 0.5f;
    bool m_negative = false;
};
