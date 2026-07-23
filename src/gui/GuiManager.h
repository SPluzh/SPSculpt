#pragma once
#include <SDL2/SDL.h>
#ifdef _WIN32
#include "platform/TabletInput.h"
#endif
#include <deque>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <array>
#include "editing/SculptManager.h"
#include "scene/Scene.h"
#include "sculpt/Remesh.h"

class AngleRenderer;

class GuiManager {
public:
    enum class RemeshState { Idle, Running, Done, Error };

    struct RemeshProgress {
        std::atomic<RemeshState> state { RemeshState::Idle };
        std::atomic<int>  stage     { 0 };   // 0=voxelize, 1=floodfill, 2=reconstruct
        std::atomic<int>  progress  { 0 };   // 0–100
        RemeshResult      result;            // written by worker, read by main after Done
        std::thread       worker;
    };

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
    bool m_showMeshInfo = true;
    bool m_showTabletDiagPanel = false;

    // FPS calculation variables
    std::deque<std::chrono::steady_clock::time_point> m_fpsTimes;
    std::chrono::steady_clock::time_point m_fpsLastUpdate = std::chrono::steady_clock::now();
    int m_fpsValue = 0;

    // settings
    float m_dyntopoDetail = 100.0f;
    int m_remeshResolution = 150;
    bool m_imguiInitialized = false;
    bool m_editPivot = false;

    // File path buffers
    char m_importPath[256] = "model.obj";
    char m_exportPath[256] = "output.obj";
    char m_refImagePath[256] = "";

    RemeshProgress m_remeshAsync;
    AngleRenderer* m_renderer = nullptr;

    ModalMode m_activeModalMode = ModalMode::NONE;
    int m_modalStartMouseX = 0;
    int m_modalStartMouseY = 0;

    // Gizmo Cube double-click prevention / delay state
    bool m_gizmoClickPending = false;
    std::chrono::steady_clock::time_point m_gizmoClickTime;
    float m_gizmoClickPartRotX = 0.0f;
    float m_gizmoClickPartRotY = 0.0f;

public:
    GuiManager();
    ~GuiManager();

    void init(SDL_Window* window, SDL_GLContext glContext);
    void shutdown();
    void render(SculptManager& sculpt, Scene& scene, AngleRenderer& renderer, SDL_Window* window);
    
    bool saveSettings(const std::string& filepath);
    bool loadSettings(const std::string& filepath);
    
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
    void toggleMeshInfo() { m_showMeshInfo = !m_showMeshInfo; }

    bool getShowTopologyPanel() const { return m_showTopologyPanel; }
    void setShowTopologyPanel(bool show) { m_showTopologyPanel = show; }

    // Settings accessors
    float getDyntopoDetail() const { return m_dyntopoDetail; }
    void setDyntopoDetail(float val) { m_dyntopoDetail = val; }

    int getRemeshResolution() const { return m_remeshResolution; }
    void setRemeshResolution(int val) { m_remeshResolution = val; }

    void performRemesh(Scene& scene);
    void applyRemeshResult(Scene& scene, const RemeshResult& r);
    void drawRemeshProgressModal();
    bool isRemeshRunning() const { return m_remeshAsync.state == RemeshState::Running; }

    void setModalMode(ModalMode mode, int startX, int startY) {
        m_activeModalMode = mode;
        m_modalStartMouseX = startX;
        m_modalStartMouseY = startY;
    }
    void drawModalIndicatorHUD(SculptManager& sculpt, Scene& scene);

    // Context popup request
    bool m_openContextPopup = false;
};
