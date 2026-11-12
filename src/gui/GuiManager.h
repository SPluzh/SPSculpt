#pragma once
#include <SDL2/SDL.h>
#include <imgui.h>
#include "platform/TabletInput.h"
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
#include "mesh/Decimation.h"
#include <unordered_map>
#include <string>
#include "timelapse/TimelapsePlayer.h"

#include "common/RecentFiles.h"
#include "common/SystemStats.h"
#include "common/WorkTimer.h"
#include "common/CrashReporter.h"
#include "common/ErrorReporter.h"
#include "files/ImportSGL.h"
#include "platform/HotkeyPreset.h"
#include "files/GoZProfiler.h"
#include "gui/HudCustomization.h"

namespace ZBrushBridge {
    class ZBrushWatcher;
}

class AngleRenderer;
class AutosaveManager;
class IniFile;
class HotkeyManager;

class GuiManager {
public:
    enum class RemeshState { Idle, Running, Done, Error };

    struct RemeshProgress {
        std::atomic<RemeshState> state { RemeshState::Idle };
        std::atomic<int>  stage     { 0 };   // 0=voxelize, 1=floodfill, 2=reconstruct
        std::atomic<int>  progress  { 0 };   // 0–100
        RemeshResult      result;            // written by worker, read by main after Done
        std::mutex        errMutex;
        std::string       errorMessage;
        std::thread       worker;
    };

    struct DecimationProgress {
        std::atomic<RemeshState> state { RemeshState::Idle };
        std::atomic<int>  progress  { 0 };   // 0–100
        std::unique_ptr<Mesh> resultMesh;
        DecimationResult  resultInfo;
        std::mutex        errMutex;
        std::string       errorMessage;
        std::thread       worker;
    };

    float getUiScale() const { return m_dpiScale * m_uiScale; }

private:
    bool m_showToolbar = false;
    bool m_showSculptingPanel = false;
    bool m_showScenePanel = true;
    bool m_showTopologyPanel = false;
    bool m_showDeformPanel = false;
    bool m_showFilesPanel = true;
    bool m_showCameraPanel = false;
    bool m_showRenderingPanel = false;
    bool m_showMaskingPanel = true;
    bool m_showMultiresPanel = false;
    bool m_showLayersPanel = false;
    bool m_showZSpheresPanel = true;
    bool m_showReferenceImagesPanel = false;
    bool m_showGizmoCube = true;
    bool m_showMeshInfo = true;
    bool m_showTabletDiagPanel = false;
    bool m_showUndoDiagPanel = false;
    bool m_showDebugLogPanel = false;
    bool m_showFloatingIsland = true;
    bool m_showTimelapsePanel = false;
    bool m_showPreferencesPanel = false;
    bool m_showHotkeyHUD = true;
    bool m_showCameraBookmarksPanel = false;
    bool m_showComponentPanel = false;

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

    // FPS & performance telemetry calculation variables
    std::deque<std::chrono::steady_clock::time_point> m_fpsTimes;
    std::chrono::steady_clock::time_point m_fpsLastUpdate = std::chrono::steady_clock::now();
    int m_fpsValue = 0;
    SystemMemoryStats m_perfStats;
    std::chrono::steady_clock::time_point m_perfLastSampleTime{};
    bool m_perfHasSample = false;

    // FPS limiter
    bool m_fpsLimitEnabled = true;
    int  m_fpsLimit = 31;

    // settings
    float m_dyntopoDetail = 100.0f;
    int m_remeshResolution = 310;
    bool m_remeshKeepPolyGroups = true;
    bool m_remeshAlignSymmetry = false;
    bool m_remeshRegularizeQuads = true;
    bool m_remeshKeepSharpEdges = false;
    bool m_remeshKeepMasked = false;
    bool m_animateBookmarks = false;
    bool m_savePngNextToProject = true;
    bool m_saveUndoHistoryInProject = false;
    bool m_createDefaultSphereOnStartup = true;
    bool m_imguiInitialized = false;
    bool m_editPivot = false;
    float m_gizmoSize = 0.10f;
    float m_dpiScale = 1.0f;
    float m_uiScale = 1.5f;
    float m_floatingIslandScale = 1.7f;
    bool m_pendingUiScaleRefresh = false;
    SDL_Window* m_window = nullptr;

    // Window settings
    int m_winWidth = 2560;
    int m_winHeight = 1566;
    int m_winX = 0;
    int m_winY = 34;
    bool m_winMaximized = true;

    void rebuildFontsAndStyles();
    float getFloatingIslandScaleMultiplier() const { return m_floatingIslandScale; }
    void setFloatingIslandScaleMultiplier(float val) { m_floatingIslandScale = std::max(0.5f, std::min(val, 2.5f)); }
    float getFloatingIslandScale() const { return m_dpiScale * m_floatingIslandScale; }

    bool m_isOverBrushSliders = false;
    bool m_slidersValid = false;
    ImVec2 m_sliderRadiusMin{0.0f, 0.0f};
    ImVec2 m_sliderRadiusMax{0.0f, 0.0f};
    ImVec2 m_sliderIntensityMin{0.0f, 0.0f};
    ImVec2 m_sliderIntensityMax{0.0f, 0.0f};
    ImVec2 m_sliderFocalShiftMin{0.0f, 0.0f};
    ImVec2 m_sliderFocalShiftMax{0.0f, 0.0f};

    // File path state
    std::string m_currentScenePath = "";
    char m_importPath[256] = "model.obj";
    char m_exportPath[256] = "output.obj";
    char m_refImagePath[512] = "";

    RemeshProgress m_remeshAsync;
    HistoryState m_remeshBeforeState;
    DecimationProgress m_decimAsync;
    HistoryState m_decimBeforeState;
    AngleRenderer* m_renderer = nullptr;

    // Decimation settings
    int m_decimMode = 0;              // 0 = Ratio, 1 = Target Faces
    float m_decimRatio = 0.5f;        // 0.01 .. 0.99
    int m_decimTargetFaces = 10000;
    float m_decimVolumeWeight = 1.0f; // 0.0 .. 10.0
    bool m_decimPreservePolyGroups = true;
    bool m_decimPreserveBoundary = true;
    bool m_decimLockSymmetry = true;

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
    float m_savedAlbedo[3] = {1.0f, 1.0f, 1.0f};
    float m_savedRoughness = 0.5f;
    float m_savedMetallic = 0.0f;
    bool m_savedUseVertexColors = true;
    bool m_savedUseVertexMaterials = true;
    // Window title & Unsaved exit dialog state
    std::string m_lastWindowTitle = "";
    bool m_showUnsavedModal = false;
    bool m_unsavedModalOpen = false;
    bool m_pendingQuit = false;

    // Resilient error banner & previous crash report modal
    AppErrorEvent m_activeBannerError;
    bool m_hasBannerError = false;
    size_t m_unreadErrorCount = 0;
    CrashReporter::PreviousCrashInfo m_prevCrashInfo;
    bool m_showCrashModal = false;
    AutosaveManager* m_autosaveManager = nullptr;
    bool m_autosaveRestoreModalOpened = false;

    void takeScreenshot(const Scene& scene, AngleRenderer& renderer);
    void drawFloatingIslandHUD(SculptManager& sculpt, Scene& scene, AngleRenderer& renderer);
    void initHudRegistry();
    void drawHudIslandSlots(HudIslandId islandId, SculptManager& sculpt, Scene& scene, AngleRenderer& renderer, const HudRenderCtx& baseCtx, float maxRowWidth = -1.0f);
    void drawHudCustomizeBanner();
    void drawHudCustomizeCatalog(SculptManager& sculpt, Scene& scene);
    void drawHudQuickAddPopup(HudIslandId islandId, float fontScale);
    void drawBrushHoverSlidersPopover(SculptManager& sculpt, const HudRenderCtx& baseCtx);
    void applyPendingHudOps();
    void enterHudCustomizeMode();
    void exitHudCustomizeMode();

    HudRegistry     m_hudRegistry;
    HudLayoutConfig m_hudLayout;
    bool            m_hudRegistryInitialized = false;
    bool            m_customizeHudMode       = false;
    bool            m_showHudCatalog         = false;
    char            m_hudCatalogSearch[64]   = "";
    int             m_hudCatalogCategory     = 0;
    std::vector<PendingHudOp> m_pendingHudOps;
    bool            m_hudDropHandledThisFrame = false;
    bool            m_hudWasDragging          = false;
    bool            m_hudDragCancelledByEsc   = false;
    HudDragPayload  m_hudLastActiveDrag{};
    bool            m_hudHasLastActiveDrag    = false;

    bool            m_brushHoverBtnDrawnThisFrame = false;
    bool            m_brushHoverBtnHovered        = false;
    bool            m_brushHoverPopoverOpen       = false;
    float           m_brushHoverCloseTimer        = 0.0f;
    bool            m_brushHoverPopoverHovered    = false;
    bool            m_brushHoverSliderActive      = false;
    ImVec2          m_brushHoverBtnMin{0.0f, 0.0f};
    ImVec2          m_brushHoverBtnMax{0.0f, 0.0f};
    ImVec2          m_brushHoverPopMin{0.0f, 0.0f};
    ImVec2          m_brushHoverPopMax{0.0f, 0.0f};
    HudIslandId     m_brushHoverIsland            = HudIslandId::LeftVert;

    void drawAppMenuItems(SculptManager& sculpt, Scene& scene, AngleRenderer& renderer);
    void drawModelSnapshotWindow(const Scene& scene, AngleRenderer& renderer);
    void drawDebugLogPanel();
    void drawSafeFramesOverlay(const AngleRenderer& renderer, const Scene& scene);
    void updateWindowTitle(SDL_Window* window, bool isModified);
    void drawUnsavedChangesModal(Scene& scene, bool& quitApp);
    void drawErrorBanner(SculptManager& sculpt, Scene& scene);
    void drawPreviousCrashModal();
    void drawAutosaveRestoreModal(Scene& scene, SculptManager& sculpt, AngleRenderer& renderer);
    void drawTimelapsePanel(Scene& scene, AngleRenderer& renderer);
    void drawPreferencesPanel(SculptManager& sculpt, Scene& scene, AngleRenderer& renderer, SDL_Window* window = nullptr);
    void drawHotkeyHUD();
    void drawToastHUD();
    void updatePerformanceMetrics(const Scene& scene, const AngleRenderer& renderer);
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
    void drawComponentToolsPanel(SculptManager& sculpt, Scene& scene, AngleRenderer& renderer);

public:
    enum PrefTab {
        PrefTab_Interface = 0,
        PrefTab_Camera = 1,
        PrefTab_Rendering = 2,
        PrefTab_ToolOverlays = 3,
        PrefTab_Performance = 4,
        PrefTab_Undo = 5,
        PrefTab_DebugLog = 6,
        PrefTab_Tablet = 7,
        PrefTab_Hotkeys = 8,
        PrefTab_ZBrushBridge = 9,
        PrefTab_Autosave = 10,
    };

private:
    int m_preferencesActiveTab = -1;
    int m_preferencesSelectedTab = 0;
    HotkeyManager* m_hotkeyManager = nullptr;
    HKAction m_capturingAction = HKAction::None;
    SDL_Keycode m_selectedVirtualKey = SDLK_UNKNOWN;
    int m_keyboardModifierLayer = 0; // 0=None, 1=Ctrl, 2=Alt, 3=Shift, 4=Ctrl+Shift, 5=Ctrl+Alt, 6=Alt+Shift
    char m_hotkeySearchFilter[64] = "";
    char m_newPresetNameBuf[64] = "";
    bool m_showNewPresetDialog = false;
    ZBrushBridge::ZBrushWatcher* m_zbrushWatcher = nullptr;
    GoZProfiler::ProfileRecord m_lastLiveSyncProfile;
    bool m_hasLiveSyncProfile = false;
    char m_zbrushDirBuf[512] = {0};
    bool m_zbrushDirBufInitialized = false;

    void drawHotkeysTab(Scene& scene, SculptManager& sculpt);
    void drawVirtualKeyboard(ImDrawList* dl, ImVec2 origin, float scale, const HotkeyPreset& preset);

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
    static void showToast(const std::string& message, float duration = 3.0f);
    static void showErrorToast(const std::string& message, float duration = 5.0f);
    static bool hasActiveToasts();
    void setPreviousCrashInfo(const CrashReporter::PreviousCrashInfo& info) {
        m_prevCrashInfo = info;
        m_showCrashModal = info.found;
    }
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

    void setHotkeyManager(HotkeyManager* hm) { m_hotkeyManager = hm; }
    HotkeyManager* getHotkeyManager() const { return m_hotkeyManager; }
    void setAutosaveManager(AutosaveManager* am) { m_autosaveManager = am; }
    AutosaveManager* getAutosaveManager() const { return m_autosaveManager; }

    bool isCapturingHotkey() const { return m_capturingAction != HKAction::None; }
    bool handleCapturedKeyEvent(const SDL_Event& e);
    bool isHotkeysTabActive() const { return m_showPreferencesPanel && (m_preferencesSelectedTab == PrefTab_Hotkeys); }
    void getActiveModifierLayer(bool& outCtrl, bool& outAlt, bool& outShift) const;

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
    bool restoreAutosaveFromPath(const std::string& path,
                                 Scene& scene,
                                 SculptManager* sculpt = nullptr,
                                 AngleRenderer* renderer = nullptr,
                                 const std::string& originalScenePath = "",
                                 bool wasUnsaved = true);
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
    void setShowSculptingPanel(bool show) { m_showSculptingPanel = show; }
    bool isSculptingPanelVisible() const { return m_showSculptingPanel; }
    void toggleScenePanel() { m_showScenePanel = !m_showScenePanel; }
    void toggleTopologyPanel() { m_showTopologyPanel = !m_showTopologyPanel; }
    void toggleDeformPanel() { m_showDeformPanel = !m_showDeformPanel; }
    void toggleCameraPanel() { m_showPreferencesPanel = true; m_preferencesActiveTab = PrefTab_Camera; }
    void toggleRenderingPanel() { m_showPreferencesPanel = true; m_preferencesActiveTab = PrefTab_Rendering; }
    void toggleMaskingPanel() { m_showMaskingPanel = !m_showMaskingPanel; }
    void toggleMultiresPanel() { m_showMultiresPanel = !m_showMultiresPanel; }
    void toggleLayersPanel() { m_showLayersPanel = !m_showLayersPanel; }
    void toggleZSpheresPanel() { m_showZSpheresPanel = !m_showZSpheresPanel; }
    void toggleReferenceImagesPanel() { m_showReferenceImagesPanel = !m_showReferenceImagesPanel; }
    void toggleGizmoCube() { m_showGizmoCube = !m_showGizmoCube; }
    void toggleMeshInfo() { m_showMeshInfo = !m_showMeshInfo; }
    void toggleUndoDiagPanel() { m_showPreferencesPanel = true; m_preferencesActiveTab = PrefTab_Undo; }
    void toggleDebugLogPanel() { m_showPreferencesPanel = true; m_preferencesActiveTab = PrefTab_DebugLog; }
    void toggleTabletDiagPanel() { m_showPreferencesPanel = true; m_preferencesActiveTab = PrefTab_Tablet; }
    void toggleFloatingIsland() { m_showFloatingIsland = !m_showFloatingIsland; }
    void toggleHudCustomizeMode() { if (m_customizeHudMode) exitHudCustomizeMode(); else enterHudCustomizeMode(); }
    bool isHudCustomizeMode() const { return m_customizeHudMode; }

    static const char* getBrushName(BrushType brush);
    static const char* getBrushHotkeyStr(BrushType brush);
    static const char* getBrushIconKeyStr(BrushType brush);
    static void renderRemeshEstimationUI(const Mesh* selectedMesh, int resolution, bool keepSharpEdges);
    void toggleTimelapsePanel() { m_showTimelapsePanel = !m_showTimelapsePanel; }
    void togglePreferencesPanel() { m_showPreferencesPanel = !m_showPreferencesPanel; }
    void toggleZBrushBridgePanel() { m_showPreferencesPanel = true; m_preferencesActiveTab = PrefTab_ZBrushBridge; }
    void setZBrushWatcher(ZBrushBridge::ZBrushWatcher* watcher) { m_zbrushWatcher = watcher; }
    ZBrushBridge::ZBrushWatcher* getZBrushWatcher() const { return m_zbrushWatcher; }
    void setLastLiveSyncProfile(const GoZProfiler::ProfileRecord& record) {
        m_lastLiveSyncProfile = record;
        m_hasLiveSyncProfile = true;
    }
    const GoZProfiler::ProfileRecord& getLastLiveSyncProfile() const { return m_lastLiveSyncProfile; }
    bool hasLiveSyncProfile() const { return m_hasLiveSyncProfile; }
    void toggleCameraBookmarksPanel() { m_showCameraBookmarksPanel = !m_showCameraBookmarksPanel; }
    void toggleBrushIconCapturePanel();
    void toggleRecentFilesWindow() { m_showRecentFilesWindow = !m_showRecentFilesWindow; }
    void toggleComponentPanel() { m_showComponentPanel = !m_showComponentPanel; }
    bool isOverBrushSliders() const;
    bool getShowComponentPanel() const { return m_showComponentPanel; }
    void setShowComponentPanel(bool show) { m_showComponentPanel = show; }

    void setRecentFiles(RecentFiles* rf) { m_recentFiles = rf; }
    RecentFiles* getRecentFiles() const { return m_recentFiles; }
    bool hasPendingRecentThumbTasks() const { return !m_recentLoadTasks.empty(); }
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
    bool isTimelapsePlaying() const {
        auto st = m_timelapsePlayer.getState();
        return st == TimelapsePlayer::State::PLAYING || st == TimelapsePlayer::State::EXPORTING;
    }
    bool hasPendingGizmoClick() const { return m_gizmoClickPending; }

    bool getShowScenePanel() const { return m_showScenePanel; }
    void setShowScenePanel(bool show) { m_showScenePanel = show; }

    bool getShowReferenceImagesPanel() const { return m_showScenePanel; }
    void setShowReferenceImagesPanel(bool show) { m_showScenePanel = show; }
    void setSelectedRefImageIndex(int idx) { m_selectedRefImageIdx = idx; }
    int getSelectedRefImageIndex() const { return m_selectedRefImageIdx; }

    bool getShowTopologyPanel() const { return m_showTopologyPanel; }
    void setShowTopologyPanel(bool show) { m_showTopologyPanel = show; }

    bool getShowDeformPanel() const { return m_showDeformPanel; }
    void setShowDeformPanel(bool show) { m_showDeformPanel = show; }

    bool getShowMultiresPanel() const { return m_showMultiresPanel; }
    void setShowMultiresPanel(bool show) { m_showMultiresPanel = show; }

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

    bool getRemeshRegularizeQuads() const { return m_remeshRegularizeQuads; }
    void setRemeshRegularizeQuads(bool val) { m_remeshRegularizeQuads = val; }

    bool getRemeshKeepSharpEdges() const { return m_remeshKeepSharpEdges; }
    void setRemeshKeepSharpEdges(bool val) { m_remeshKeepSharpEdges = val; }

    bool getRemeshKeepMasked() const { return m_remeshKeepMasked; }
    void setRemeshKeepMasked(bool val) { m_remeshKeepMasked = val; }

    bool getAnimateBookmarks() const { return m_animateBookmarks; }
    void setAnimateBookmarks(bool val) { m_animateBookmarks = val; }

    bool getSavePngNextToProject() const { return m_savePngNextToProject; }
    void setSavePngNextToProject(bool val) { m_savePngNextToProject = val; }

    bool getSaveUndoHistoryInProject() const { return m_saveUndoHistoryInProject; }
    void setSaveUndoHistoryInProject(bool val) { m_saveUndoHistoryInProject = val; }

    bool getCreateDefaultSphereOnStartup() const { return m_createDefaultSphereOnStartup; }
    void setCreateDefaultSphereOnStartup(bool val) { m_createDefaultSphereOnStartup = val; }

    float getGizmoSize() const { return m_gizmoSize; }
    void setGizmoSize(float val) { m_gizmoSize = val; }

    void performRemesh(Scene& scene);
    void applyRemeshResult(Scene& scene, const RemeshResult& r);
    void drawRemeshProgressModal();
    bool isRemeshRunning() const { return m_remeshAsync.state == RemeshState::Running; }

    void performDecimation(Scene& scene);
    void applyDecimationResult(Scene& scene);
    void drawDecimationProgressModal();
    bool isDecimationRunning() const { return m_decimAsync.state == RemeshState::Running; }

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
    static void invalidateThumbnail(uint32_t meshId);
    static void invalidateAllThumbnails();

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
