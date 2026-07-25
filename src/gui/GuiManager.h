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
    bool m_showUndoDiagPanel = false;
    bool m_showFloatingIsland = true;

    // FPS calculation variables
    std::deque<std::chrono::steady_clock::time_point> m_fpsTimes;
    std::chrono::steady_clock::time_point m_fpsLastUpdate = std::chrono::steady_clock::now();
    int m_fpsValue = 0;

    // settings
    float m_dyntopoDetail = 100.0f;
    int m_remeshResolution = 150;
    bool m_imguiInitialized = false;
    bool m_editPivot = false;
    float m_dpiScale = 1.0f;
    float m_uiScale = 1.0f;
    bool m_pendingUiScaleRefresh = false;
    SDL_Window* m_window = nullptr;

    // Window settings
    int m_winWidth = 1280;
    int m_winHeight = 720;
    int m_winX = SDL_WINDOWPOS_CENTERED;
    int m_winY = SDL_WINDOWPOS_CENTERED;
    bool m_winMaximized = false;

    void rebuildFontsAndStyles();
    float getUiScale() const { return m_dpiScale * m_uiScale; }

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

    // Screenshot settings
    int m_screenshotPreset = 1; // 0: Viewport, 1: 1080p, 2: 2K, 3: 4K, 4: Custom
    int m_screenshotWidth = 1920;
    int m_screenshotHeight = 1080;
    bool m_screenshotShowGrid = false;
    bool m_screenshotShowContour = false;
    BrushType m_lastBrushType = BRUSH_MOVE;
    bool m_previewingPaint = false;
    float m_savedAlbedo[3] = {0.72f, 0.52f, 0.45f};
    float m_savedRoughness = 0.5f;
    float m_savedMetallic = 0.0f;
    bool m_savedUseVertexColors = false;
    bool m_savedUseVertexMaterials = false;
    void takeScreenshot(const Scene& scene, AngleRenderer& renderer);
    void drawFloatingIslandHUD(SculptManager& sculpt, Scene& scene, AngleRenderer& renderer);
    void drawUndoDiagPanel(Scene& scene);

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
    void toggleUndoDiagPanel() { m_showUndoDiagPanel = !m_showUndoDiagPanel; }
    void toggleFloatingIsland() { m_showFloatingIsland = !m_showFloatingIsland; }

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

    int getWindowWidth() const { return m_winWidth; }
    int getWindowHeight() const { return m_winHeight; }
    int getWindowX() const { return m_winX; }
    int getWindowY() const { return m_winY; }
    bool getWindowMaximized() const { return m_winMaximized; }

    void updateWindowBounds(int x, int y, int w, int h, bool maximized) {
        m_winMaximized = maximized;
        if (!maximized) {
            m_winX = x;
            m_winY = y;
            m_winWidth = w;
            m_winHeight = h;
        }
    }

    // Context popup request
    bool m_openContextPopup = false;
};
