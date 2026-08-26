#pragma once
#include <SDL2/SDL.h>
#include <imgui.h>
#ifdef _WIN32
#include "platform/TabletInput.h"
#endif
#include <deque>
#include <chrono>
#include <thread>
#include <future>
#include <atomic>
#include <mutex>
#include <array>
#include "editing/SculptManager.h"
#include "scene/Scene.h"
#include "sculpt/Remesh.h"
#include <unordered_map>
#include <string>
#include "timelapse/TimelapsePlayer.h"

#include "common/RecentFiles.h"
#include "common/WorkTimer.h"
#include "files/ImportSGL.h"

class AngleRenderer;
class IniFile;

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
    bool m_showLayersPanel = true;
    bool m_showZSpheresPanel = true;
    bool m_showReferenceImagesPanel = true;
    bool m_showGizmoCube = true;
    bool m_showMeshInfo = true;
    bool m_showTabletDiagPanel = false;
    bool m_showUndoDiagPanel = false;
    bool m_showDebugLogPanel = false;
    bool m_showFloatingIsland = true;
    bool m_showTimelapsePanel = false;
    bool m_showPreferencesPanel = false;
    bool m_showHotkeyHUD = false;
    bool m_showCameraBookmarksPanel = false;

    // Brush Icon Capture settings
    bool  m_showBrushIconCapture = false;
    int   m_brushIconSize        = 256;
    float m_iconFrameCenterX     = 0.5f;
    float m_iconFrameCenterY     = 0.5f;
    char  m_iconFileName[128]    = "";

    // Timelapse export settings
    int m_exportStepsPerFrame = 1;
    int m_exportWidth = 1920;
    int m_exportHeight = 1080;
    char m_exportDir[256] = "timelapse_frames";
    TimelapsePlayer m_timelapsePlayer;

    int m_mirrorAxis = 0; // 0: X, 1: Y, 2: Z
    bool m_mirrorPositiveToNegative = true;

    // FPS calculation variables
    std::deque<std::chrono::steady_clock::time_point> m_fpsTimes;
    std::chrono::steady_clock::time_point m_fpsLastUpdate = std::chrono::steady_clock::now();
    int m_fpsValue = 0;

    // FPS limiter
    bool m_fpsLimitEnabled = true;
    int  m_fpsLimit = 60;

    // settings
    float m_dyntopoDetail = 100.0f;
    int m_remeshResolution = 150;
    bool m_remeshKeepPolyGroups = true;
    bool m_remeshAlignSymmetry = false;
    bool m_animateBookmarks = true;
    bool m_savePngNextToProject = false;
    bool m_imguiInitialized = false;
    bool m_editPivot = false;
    float m_gizmoSize = 0.10f;
    float m_dpiScale = 1.0f;
    float m_uiScale = 1.0f;
    float m_floatingIslandScale = 1.0f;
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
    float getFloatingIslandScaleMultiplier() const { return m_floatingIslandScale; }
    void setFloatingIslandScaleMultiplier(float val) { m_floatingIslandScale = std::max(0.5f, std::min(val, 2.5f)); }
    float getFloatingIslandScale() const { return m_dpiScale * m_floatingIslandScale; }

    // File path state
    std::string m_currentScenePath = "";
    char m_importPath[256] = "model.obj";
    char m_exportPath[256] = "output.obj";
    char m_refImagePath[512] = "";

    RemeshProgress m_remeshAsync;
    HistoryState m_remeshBeforeState;
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
    // Window title & Unsaved exit dialog state
    std::string m_lastWindowTitle = "";
    bool m_showUnsavedModal = false;
    bool m_unsavedModalOpen = false;
    bool m_pendingQuit = false;

    void takeScreenshot(const Scene& scene, AngleRenderer& renderer);
    void drawFloatingIslandHUD(SculptManager& sculpt, Scene& scene, AngleRenderer& renderer);
    void drawAppMenuItems(SculptManager& sculpt, Scene& scene, AngleRenderer& renderer);
    void drawModelSnapshotWindow(const Scene& scene, AngleRenderer& renderer);
    void drawDebugLogPanel();
    void drawSafeFramesOverlay(const AngleRenderer& renderer, const Scene& scene);
    void updateWindowTitle(SDL_Window* window, bool isModified);
    void drawUnsavedChangesModal(Scene& scene, bool& quitApp);
    void drawTimelapsePanel(Scene& scene, AngleRenderer& renderer);
    void drawPreferencesPanel(SculptManager& sculpt, Scene& scene, AngleRenderer& renderer, SDL_Window* window = nullptr);
    void drawHotkeyHUD();
    void drawBrushIconCapturePanel(const Scene& scene, AngleRenderer& renderer);
    void drawBrushIconFrameOverlay();
    void captureBrushIcon(const Scene& scene, AngleRenderer& renderer);

    enum class RefDragTarget { None, Move, ScaleTL, ScaleTR, ScaleBR, ScaleBL, ScaleT, ScaleR, ScaleB, ScaleL, Rotate };
    int m_selectedRefImageIdx = 0;
    RefDragTarget m_activeRefDragTarget = RefDragTarget::None;
    int m_draggingRefImageIdx = -1;
    ImVec2 m_refDragStartMouse{0.0f, 0.0f};
    float m_refDragStartOffsetX = 0.0f;
    float m_refDragStartOffsetY = 0.0f;
    float m_refDragStartScale = 1.0f;
    float m_refDragStartRotation = 0.0f;
    float m_refDragStartAngleMouse = 0.0f;
    glm::vec2 m_refDragStartIntersect3D{0.0f, 0.0f};

    void drawReferenceImageManipulator(SculptManager& sculpt, Scene& scene, AngleRenderer& renderer);
    void drawCameraBookmarksPanel(Scene& scene, AngleRenderer& renderer);

    int m_preferencesActiveTab = -1;

    std::unordered_map<std::string, GLuint> m_iconCache;
    GLuint getIconTexture(const std::string& iconName);
    GLuint getXrayIconTexture();

    struct RecentThumbTask {
        std::string path;
        std::future<ImportSGL::ProjectMetadata> future;
        bool submitted = false;
    };

    RecentFiles* m_recentFiles = nullptr;
    bool m_showRecentFilesWindow = false;
    int m_recentSelectedIdx = -1;
    std::unordered_map<std::string, GLuint> m_recentThumbCache;
    std::unordered_map<std::string, ImVec2> m_recentThumbSizes;
    std::unordered_map<std::string, std::filesystem::file_time_type> m_recentThumbTimestamps;
    std::unordered_map<std::string, uint64_t> m_recentFileWorkTimes;
    std::unordered_map<std::string, std::string> m_recentFileDateCache;
    std::unordered_map<std::string, RecentThumbTask> m_recentLoadTasks;

    GLuint getRecentFileThumbnail(const std::string& path);
    uint64_t getRecentFileWorkTime(const std::string& path);
    GLuint uploadRecentThumbnailToGPU(const std::string& path, const std::vector<uint8_t>& pngData);
    std::string getRecentFileDateCached(const std::string& path);
    void prefetchRecentFileMetadata(const RecentFiles& recentFiles);
    void pollRecentLoadTasks();
    void invalidateRecentFileThumbnail(const std::string& path);
    void clearRecentThumbCache();
    void drawRecentFilesWindow(Scene& scene, SculptManager* sculpt, RecentFiles& recentFiles);
    void drawWorkTimerIsland(Scene& scene);

    WorkTimer m_workTimer;

public:
    WorkTimer& getWorkTimer() { return m_workTimer; }
    const WorkTimer& getWorkTimer() const { return m_workTimer; }
    GuiManager();
    ~GuiManager();

    void init(SDL_Window* window, SDL_GLContext glContext);
    void shutdown();
    void render(SculptManager& sculpt, Scene& scene, AngleRenderer& renderer, SDL_Window* window);
    
    bool saveSettings(IniFile& ini);
    bool loadSettings(const IniFile& ini);
    
    // Fallback for empty calls
    void render() {}

    bool isPointOverWindow(const ImVec2& pt) const;

    // Window title & Exit handling
    void requestExit(bool forceModalCheck = true) {
        m_pendingQuit = true;
        if (forceModalCheck) {
            m_showUnsavedModal = true;
        }
    }
    bool isPendingQuit() const { return m_pendingQuit; }
    void cancelQuit() { m_pendingQuit = false; m_showUnsavedModal = false; m_unsavedModalOpen = false; }
    bool isUnsavedModalOpen() const { return m_unsavedModalOpen; }

    // File operations
    void openScene(Scene& scene, SculptManager* sculpt = nullptr);
    bool openSceneFromPath(const std::string& path, Scene& scene, SculptManager* sculpt = nullptr, AngleRenderer* renderer = nullptr);
    void setRenderer(AngleRenderer* renderer) { m_renderer = renderer; }
    void saveScene(Scene& scene, SculptManager* sculpt = nullptr);
    void saveSceneAs(Scene& scene, SculptManager* sculpt = nullptr);
    void importFile(Scene& scene, SculptManager* sculpt = nullptr);
    void exportFile(Scene& scene, SculptManager* sculpt = nullptr);
    const std::string& getCurrentScenePath() const { return m_currentScenePath; }
    void setCurrentScenePath(const std::string& path) { m_currentScenePath = path; }

    // Panel toggles
    void toggleToolbar() { m_showToolbar = !m_showToolbar; }
    void toggleSculptingPanel() { m_showSculptingPanel = !m_showSculptingPanel; }
    void toggleScenePanel() { m_showScenePanel = !m_showScenePanel; }
    void toggleTopologyPanel() { m_showTopologyPanel = !m_showTopologyPanel; }
    void toggleCameraPanel() { m_showPreferencesPanel = true; m_preferencesActiveTab = 1; }
    void toggleRenderingPanel() { m_showPreferencesPanel = true; m_preferencesActiveTab = 2; }
    void toggleMaskingPanel() { m_showMaskingPanel = !m_showMaskingPanel; }
    void toggleMultiresPanel() { m_showMultiresPanel = !m_showMultiresPanel; }
    void toggleLayersPanel() { m_showLayersPanel = !m_showLayersPanel; }
    void toggleZSpheresPanel() { m_showZSpheresPanel = !m_showZSpheresPanel; }
    void toggleReferenceImagesPanel() { m_showReferenceImagesPanel = !m_showReferenceImagesPanel; }
    void toggleGizmoCube() { m_showGizmoCube = !m_showGizmoCube; }
    void toggleMeshInfo() { m_showMeshInfo = !m_showMeshInfo; }
    void toggleUndoDiagPanel() { m_showPreferencesPanel = true; m_preferencesActiveTab = 4; }
    void toggleDebugLogPanel() { m_showPreferencesPanel = true; m_preferencesActiveTab = 5; }
    void toggleTabletDiagPanel() { m_showPreferencesPanel = true; m_preferencesActiveTab = 6; }
    void toggleFloatingIsland() { m_showFloatingIsland = !m_showFloatingIsland; }
    void toggleTimelapsePanel() { m_showTimelapsePanel = !m_showTimelapsePanel; }
    void togglePreferencesPanel() { m_showPreferencesPanel = !m_showPreferencesPanel; }
    void toggleCameraBookmarksPanel() { m_showCameraBookmarksPanel = !m_showCameraBookmarksPanel; }
    void toggleBrushIconCapturePanel();
    void toggleRecentFilesWindow() { m_showRecentFilesWindow = !m_showRecentFilesWindow; }

    void setRecentFiles(RecentFiles* rf) { m_recentFiles = rf; }
    RecentFiles* getRecentFiles() const { return m_recentFiles; }
    bool getShowRecentFilesWindow() const { return m_showRecentFilesWindow; }
    void setShowRecentFilesWindow(bool show) { m_showRecentFilesWindow = show; }
    std::vector<uint8_t> renderSceneThumbnailPng(const Scene& scene, AngleRenderer& renderer, int w = 0, int h = 0);

    bool getShowCameraBookmarksPanel() const { return m_showCameraBookmarksPanel; }
    void setShowCameraBookmarksPanel(bool show) { m_showCameraBookmarksPanel = show; }

    bool getShowBrushIconCapture() const { return m_showBrushIconCapture; }
    void setShowBrushIconCapture(bool show) { m_showBrushIconCapture = show; }

    bool getShowPreferencesPanel() const { return m_showPreferencesPanel; }
    void setShowPreferencesPanel(bool show) { m_showPreferencesPanel = show; }

    bool getShowMeshInfo() const { return m_showMeshInfo; }
    void setShowMeshInfo(bool show) { m_showMeshInfo = show; }

    bool getShowTimelapsePanel() const { return m_showTimelapsePanel; }
    void setShowTimelapsePanel(bool show) { m_showTimelapsePanel = show; }
    TimelapsePlayer& getTimelapsePlayer() { return m_timelapsePlayer; }

    bool getShowScenePanel() const { return m_showScenePanel; }
    void setShowScenePanel(bool show) { m_showScenePanel = show; }

    bool getShowReferenceImagesPanel() const { return m_showScenePanel; }
    void setShowReferenceImagesPanel(bool show) { m_showScenePanel = show; }
    void setSelectedRefImageIndex(int idx) { m_selectedRefImageIdx = idx; }
    int getSelectedRefImageIndex() const { return m_selectedRefImageIdx; }

    bool getShowTopologyPanel() const { return m_showTopologyPanel; }
    void setShowTopologyPanel(bool show) { m_showTopologyPanel = show; }

    bool getShowLayersPanel() const { return m_showLayersPanel; }
    void setShowLayersPanel(bool show) { m_showLayersPanel = show; }

    // FPS limiter accessors
    bool getFpsLimitEnabled() const { return m_fpsLimitEnabled; }
    void setFpsLimitEnabled(bool v) { m_fpsLimitEnabled = v; }
    int  getFpsLimit() const { return m_fpsLimit; }
    void setFpsLimit(int v) { m_fpsLimit = std::max(1, std::min(v, 9999)); }

    // Settings accessors
    float getDyntopoDetail() const { return m_dyntopoDetail; }
    void setDyntopoDetail(float val) { m_dyntopoDetail = val; }

    int getRemeshResolution() const { return m_remeshResolution; }
    void setRemeshResolution(int val) { m_remeshResolution = val; }

    bool getRemeshKeepPolyGroups() const { return m_remeshKeepPolyGroups; }
    void setRemeshKeepPolyGroups(bool val) { m_remeshKeepPolyGroups = val; }

    bool getRemeshAlignSymmetry() const { return m_remeshAlignSymmetry; }
    void setRemeshAlignSymmetry(bool val) { m_remeshAlignSymmetry = val; }

    bool getAnimateBookmarks() const { return m_animateBookmarks; }
    void setAnimateBookmarks(bool val) { m_animateBookmarks = val; }

    bool getSavePngNextToProject() const { return m_savePngNextToProject; }
    void setSavePngNextToProject(bool val) { m_savePngNextToProject = val; }

    float getGizmoSize() const { return m_gizmoSize; }
    void setGizmoSize(float val) { m_gizmoSize = val; }

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
    int m_previousShaderType = 0;

    // Outliner Thumbnails
    static constexpr int THUMB_SIZE = 160;
    struct MeshThumbnail {
        GLuint fbo     = 0;
        GLuint texture = 0;
        bool   dirty   = true;
    };

    void thumbInvalidate(uint32_t meshId);
    void thumbInvalidateAll();

private:
    GLuint m_thumbSharedDepth = 0;
    std::unordered_map<uint32_t, MeshThumbnail> m_thumbCache;

    void thumbEnsureFbo(MeshThumbnail& t);
    void thumbRender(MeshThumbnail& t, Mesh* mesh, AngleRenderer& renderer);
    void thumbCleanup(const Scene& scene);

    // Bookmark Previews
    static constexpr int BOOKMARK_PREVIEW_SIZE = 160;
    struct BookmarkPreview {
        GLuint fbo     = 0;
        GLuint texture = 0;
        bool   dirty   = true;
    };
    GLuint m_bookmarkSharedDepth = 0;
    std::vector<BookmarkPreview> m_bookmarkPreviews;

    void bookmarkEnsureFbo(BookmarkPreview& p);
    void renderBookmarkPreview(int bmIdx, const Scene& scene, AngleRenderer& renderer);
};
