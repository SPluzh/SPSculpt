#pragma once
#include <SDL2/SDL.h>
#include "editing/SculptManager.h"
#include "scene/Scene.h"

class AngleRenderer;

class GuiManager {
private:
    bool m_showToolbar = true;
    bool m_showSculptingPanel = true;
    bool m_showScenePanel = true;
    bool m_showTopologyPanel = true;
    bool m_showFilesPanel = true;
    bool m_showCameraPanel = true;
    bool m_showRenderingPanel = true;
    bool m_showMaskingPanel = true;
    bool m_showMultiresPanel = true;
    bool m_showZSpheresPanel = true;
    bool m_showReferenceImagesPanel = true;
    bool m_showGizmoCube = true;

    // settings
    float m_dyntopoDetail = 100.0f;
    int m_remeshResolution = 150;
    bool m_imguiInitialized = false;

    // File path buffers
    char m_importPath[256] = "model.obj";
    char m_exportPath[256] = "output.obj";
    char m_refImagePath[256] = "";

public:
    GuiManager();
    ~GuiManager();

    void init(SDL_Window* window, SDL_GLContext glContext);
    void shutdown();
    void render(SculptManager& sculpt, Scene& scene, AngleRenderer& renderer, SDL_Window* window);
    
    // Fallback for empty calls
    void render() {}

    // Panel toggles
    void toggleToolbar() { m_showToolbar = !m_showToolbar; }
    void toggleSculptingPanel() { m_showSculptingPanel = !m_showSculptingPanel; }
    void toggleScenePanel() { m_showScenePanel = !m_showScenePanel; }
    void toggleTopologyPanel() { m_showTopologyPanel = !m_showTopologyPanel; }
    void toggleFilesPanel() { m_showFilesPanel = !m_showFilesPanel; }
    void toggleCameraPanel() { m_showCameraPanel = !m_showCameraPanel; }
    void toggleRenderingPanel() { m_showRenderingPanel = !m_showRenderingPanel; }
    void toggleMaskingPanel() { m_showMaskingPanel = !m_showMaskingPanel; }
    void toggleMultiresPanel() { m_showMultiresPanel = !m_showMultiresPanel; }
    void toggleZSpheresPanel() { m_showZSpheresPanel = !m_showZSpheresPanel; }
    void toggleReferenceImagesPanel() { m_showReferenceImagesPanel = !m_showReferenceImagesPanel; }
    void toggleGizmoCube() { m_showGizmoCube = !m_showGizmoCube; }

    bool getShowTopologyPanel() const { return m_showTopologyPanel; }
    void setShowTopologyPanel(bool show) { m_showTopologyPanel = show; }

    // Settings accessors
    float getDyntopoDetail() const { return m_dyntopoDetail; }
    void setDyntopoDetail(float val) { m_dyntopoDetail = val; }

    int getRemeshResolution() const { return m_remeshResolution; }
    void setRemeshResolution(int val) { m_remeshResolution = val; }

    // Context popup request
    bool m_openContextPopup = false;
};
