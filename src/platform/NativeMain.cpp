#define SDL_MAIN_HANDLED
#define _USE_MATH_DEFINES
#include <cmath>
#include <iostream>
#include <csignal>
#include <stdexcept>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <chrono>
#include <limits>

#include <SDL2/SDL.h>
#include <GLES3/gl3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <ImGuizmo.h>

#include "render/AngleRenderer.h"
#include "mesh/Octree.h"
#include "mesh/NormalCalc.h"
#include "sculpt/SculptEngine.h"
#include "mesh/Topology.h"
#include "scene/Scene.h"
#include "mesh/Mesh.h"
#include "scene/Camera.h"
#include "editing/SculptManager.h"
#include "gui/GuiManager.h"
#include "platform/HotkeyDispatcher.h"
#include "render/RenderSettings.h"
#include "common/IniFile.h"
#include "brushes/BrushPresetManager.h"

#ifdef _WIN32
#include <windows.h>
#include <SDL2/SDL_syswm.h>
#include "platform/TabletInput.h"
#include "common/Logger.h"
LONG WINAPI windowsExceptionFilter(struct _EXCEPTION_POINTERS* ExceptionInfo) {
    sculpt_log("\n=============================================\n");
    sculpt_log("[CRITICAL ERROR] Windows Unhandled Exception! Code: 0x%X\n", (unsigned int)ExceptionInfo->ExceptionRecord->ExceptionCode);
    sculpt_log("=============================================\n");
    system("pause");
    return EXCEPTION_EXECUTE_HANDLER;
}

#ifndef PT_PEN
#define PT_PEN 3
#endif

#ifndef POINTER_FLAG_INCONTACT
#define POINTER_FLAG_INCONTACT 0x00020000
#endif

struct POINTER_INFO_LITE {
    DWORD pointerType;
    UINT32 pointerId;
    void* frameId;
    DWORD pointerFlags;
    void* sourceDevice;
    HWND hwndTarget;
    POINT ptPixelLocation;
    POINT ptHimetricLocation;
    POINT ptPixelLocationRaw;
    POINT ptHimetricLocationRaw;
    DWORD dwTime;
    UINT32 historyCount;
    INT32 InputData;
    DWORD dwKeyStates;
    UINT64 PerformanceCount;
    DWORD ButtonChangeType;
};

struct POINTER_PEN_INFO_LITE {
    POINTER_INFO_LITE pointerInfo;
    DWORD penFlags;
    DWORD penMask;
    UINT32 pressure;
    UINT32 rotation;
    INT32 tiltX;
    INT32 tiltY;
};

typedef BOOL(WINAPI* GetPointerTypePFN)(UINT32 pointerId, DWORD* pointerType);
typedef BOOL(WINAPI* GetPointerPenInfoPFN)(UINT32 pointerId, POINTER_PEN_INFO_LITE* penInfo);

static GetPointerTypePFN pfnGetPointerType = nullptr;
static GetPointerPenInfoPFN pfnGetPointerPenInfo = nullptr;
static bool s_pointerAPIsLoaded = false;

static void loadPointerAPIs() {
    if (s_pointerAPIsLoaded) return;
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        pfnGetPointerType = (GetPointerTypePFN)GetProcAddress(user32, "GetPointerType");
        pfnGetPointerPenInfo = (GetPointerPenInfoPFN)GetProcAddress(user32, "GetPointerPenInfo");
    }
    s_pointerAPIsLoaded = true;
}

static void SDLCALL TabletMessageHook(void* userdata, void* hWnd, unsigned int message, Uint64 wParam, Sint64 lParam) {
    SculptManager* sculpt = static_cast<SculptManager*>(userdata);
    if (!sculpt) return;

    loadPointerAPIs();
    if (!pfnGetPointerType || !pfnGetPointerPenInfo) return;

    if (message == 0x0245 || message == 0x0246) { // WM_POINTERUPDATE, WM_POINTERDOWN
        UINT32 pointerId = (UINT32)(wParam & 0xFFFF);
        DWORD pointerType = 0;
        if (pfnGetPointerType(pointerId, &pointerType)) {
            if (pointerType == PT_PEN) {
                POINTER_PEN_INFO_LITE penInfo;
                std::memset(&penInfo, 0, sizeof(penInfo));
                if (pfnGetPointerPenInfo(pointerId, &penInfo)) {
                    bool inContact = (penInfo.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
                    float pressure = 0.0f;
                    if (inContact) {
                        pressure = (float)penInfo.pressure / 1024.0f;
                    }
                    float tiltX = (float)penInfo.tiltX;
                    float tiltY = (float)penInfo.tiltY;
                    g_tablet.onWinInkUpdate(pressure, tiltX, tiltY, inContact);
                    sculpt->setStylusPressure(pressure);
                }
            }
        }
    } else if (message == 0x0247) { // WM_POINTERUP
        g_tablet.onWinInkUp();
        sculpt->setStylusPressure(0.0f);
    }
}
#endif

void crashHandler(int signum) {
    sculpt_log("\n=============================================\n");
    const char* sigName = "Unknown";
    switch (signum) {
        case SIGSEGV: sigName = "SIGSEGV (Segmentation Fault / Access Violation)"; break;
        case SIGABRT: sigName = "SIGABRT (Abort / Assertion Failure)"; break;
        case SIGFPE:  sigName = "SIGFPE (Floating Point Exception)"; break;
        case SIGILL:  sigName = "SIGILL (Illegal Instruction)"; break;
        case SIGINT:  sigName = "SIGINT (Interrupt)"; break;
        case SIGTERM: sigName = "SIGTERM (Termination Request)"; break;
    }
    sculpt_log("[CRITICAL ERROR] Application crashed! Signal: %d (%s)\n", signum, sigName);
    sculpt_log("=============================================\n");
    #ifdef _WIN32
    system("pause");
    #else
    std::cout << "Press Enter to exit..." << std::endl;
    std::cin.get();
    #endif
    exit(signum);
}

GLuint generateClayMatcapTexture() {
    const int width = 256;
    const int height = 256;
    std::vector<uint8_t> pixels(width * height * 4);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Normalized coordinates from -1 to 1
            float nx = (x - width * 0.5f) / (width * 0.5f);
            float ny = (height * 0.5f - y) / (height * 0.5f);
            float r2 = nx * nx + ny * ny;
            
            float r, g, b, a;
            if (r2 <= 1.0f) {
                float nz = std::sqrt(1.0f - r2);
                glm::vec3 normal(nx, ny, nz);
                
                // Diffuse lighting from top-right-front light source
                glm::vec3 lightDir = glm::normalize(glm::vec3(0.5f, 0.8f, 1.0f));
                float diffuse = std::max(glm::dot(normal, lightDir), 0.0f);
                glm::vec3 lightColor = glm::vec3(0.9f, 0.85f, 0.8f) * diffuse + glm::vec3(0.18f, 0.18f, 0.22f);
                
                // Clay base color
                glm::vec3 clayColor(0.72f, 0.52f, 0.45f);
                glm::vec3 color = clayColor * lightColor;
                
                // Specular highlight
                glm::vec3 rVec = glm::reflect(glm::vec3(0.0f, 0.0f, -1.0f), normal);
                float spec = std::pow(std::max(glm::dot(rVec, lightDir), 0.0f), 16.0f);
                color += glm::vec3(0.15f) * spec;
                
                // Clamp
                color = glm::clamp(color, 0.0f, 1.0f);
                
                r = color.r;
                g = color.g;
                b = color.b;
                a = 1.0f;
            } else {
                // Background color outside sphere (alpha 0 or black)
                r = 0.08f;
                g = 0.09f;
                b = 0.1f;
                a = 0.0f;
            }
            
            int idx = (y * width + x) * 4;
            pixels[idx] = static_cast<uint8_t>(r * 255.0f);
            pixels[idx + 1] = static_cast<uint8_t>(g * 255.0f);
            pixels[idx + 2] = static_cast<uint8_t>(b * 255.0f);
            pixels[idx + 3] = static_cast<uint8_t>(a * 255.0f);
        }
    }

    GLuint texId;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    return texId;
}

int main(int argc, char* argv[]) {
    bool showConsole = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--console" || arg == "-console" || arg == "-c" || arg == "--show-console") {
            showConsole = true;
            break;
        }
    }

#ifdef _WIN32
    if (showConsole) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
            AllocConsole();
        }
        FILE* fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        freopen_s(&fp, "CONIN$", "r", stdin);
        std::cout.clear();
        std::cerr.clear();
        std::cin.clear();
    }
    SetUnhandledExceptionFilter(windowsExceptionFilter);
    // Configure Windows process-level DPI awareness to avoid system-level scaling blur
    SetProcessDPIAware();
#endif

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    sculpt_log_init();

    // Register crash signal handlers
    std::signal(SIGSEGV, crashHandler);
    std::signal(SIGABRT, crashHandler);
    std::signal(SIGFPE, crashHandler);
    std::signal(SIGILL, crashHandler);
    std::signal(SIGINT, crashHandler);
    std::signal(SIGTERM, crashHandler);

    try {
        SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    std::cout << "Starting SPSculpt Desktop Core..." << std::endl;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL initialization failed: " << SDL_GetError() << std::endl;
        return -1;
    }

    // Request OpenGL ES 3.0 Context
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    IniFile appSettings;
    appSettings.load("app_settings.cfg");

    GuiManager gui;
    gui.loadSettings(appSettings);

    int width = gui.getWindowWidth();
    int height = gui.getWindowHeight();
    int posX = gui.getWindowX();
    int posY = gui.getWindowY();
    bool startMaximized = gui.getWindowMaximized();

    // Verify window placement to avoid opening off-screen (e.g. disconnected monitor)
    SDL_Rect winRect = { posX, posY, width, height };
    bool intersects = false;
    int numDisplays = SDL_GetNumVideoDisplays();
    for (int i = 0; i < numDisplays; ++i) {
        SDL_Rect displayRect;
        if (SDL_GetDisplayBounds(i, &displayRect) == 0) {
            SDL_Rect intersection;
            if (SDL_IntersectRect(&winRect, &displayRect, &intersection)) {
                intersects = true;
                break;
            }
        }
    }
    if (!intersects && posX != SDL_WINDOWPOS_CENTERED && posX != SDL_WINDOWPOS_UNDEFINED) {
        posX = SDL_WINDOWPOS_CENTERED;
        posY = SDL_WINDOWPOS_CENTERED;
    }

    SDL_Window* window = SDL_CreateWindow(
        "SPSculpt",
        posX, posY,
        width, height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | (startMaximized ? SDL_WINDOW_MAXIMIZED : 0)
    );

    if (!window) {
        std::cerr << "Failed to create SDL window: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return -1;
    }

    // Retrieve actual size in case the window started maximized or OS adjusted it
    SDL_GetWindowSize(window, &width, &height);

    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    if (!glContext) {
        std::cerr << "Failed to create OpenGL ES context: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    // Use adaptive vsync (0) so our SDL_Delay-based frame cap can actually control timing.
    // SwapInterval(1) would block inside the driver at monitor rate, making SDL_Delay useless.
    SDL_GL_SetSwapInterval(0);

    // Calculate initial DPI scale and physical drawable size
    int drawableWidth = width;
    int drawableHeight = height;
    SDL_GL_GetDrawableSize(window, &drawableWidth, &drawableHeight);
    float dpiScale = (width > 0) ? ((float)drawableWidth / (float)width) : 1.0f;

    // Initialize the renderer with physical/drawable dimensions
    AngleRenderer renderer;
    if (!renderer.init(drawableWidth, drawableHeight)) {
        std::cerr << "Failed to initialize renderer" << std::endl;
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }
    renderer.resize(drawableWidth, drawableHeight, dpiScale);

    Scene scene;
    scene.loadDefaultSphere();
    scene.getCamera().onResize(width, height);

    // Apply matcap clay preset globally to the renderer
    renderer.setMatcap(5); // "Clay" matcap preset index
    renderer.setShaderType(1); // MATCAP
    renderer.setTextureId(0);

    // Auto-load render and shading settings if they exist
    RenderSettings::load(appSettings, renderer, scene);
    if (scene.getSplitMode() != Scene::SplitMode::OFF) {
        int halfW = width / 2;
        scene.getCamera().onResize(halfW, height);
        if (scene.getCameraRight()) {
            scene.getCameraRight()->onResize(width - halfW, height);
        }
    } else {
        scene.getCamera().onResize(width, height);
    }

    BrushPresetManager::instance().loadDefaults();
    SculptManager sculpt;
    sculpt.loadSettings(appSettings);
    gui.init(window, glContext);
    HotkeyDispatcher dispatcher;

#ifdef _WIN32
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    HWND hwnd = nullptr;
    if (SDL_GetWindowWMInfo(window, &wmInfo)) {
        hwnd = wmInfo.info.win.window;
    }
    if (g_tablet.wintabLoad()) {
        g_tablet.wintabOpen(hwnd);
        g_tablet.startPolling();
    }
    SDL_SetWindowsMessageHook(TabletMessageHook, &sculpt);
#endif

    auto lastFrameTime = std::chrono::high_resolution_clock::now();
    auto lastActivityTime = std::chrono::high_resolution_clock::now(); // for idle throttle
    bool quit = false;
    SDL_Event event;
    std::cout << "[Debug] Entering main loop." << std::endl;
    bool wasRemeshRunning = false;
    while (!quit) {
        auto currentFrameTime = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(currentFrameTime - lastFrameTime).count();
        lastFrameTime = currentFrameTime;

        // Update cameras
        scene.getCamera().update(deltaTime);
        if (scene.getSplitMode() != Scene::SplitMode::OFF) {
            auto camRight = scene.getCameraRight();
            if (camRight) {
                camRight->update(deltaTime);
            }
        }

        bool hadActivity = false; // set true on any user input this frame
        while (SDL_PollEvent(&event)) {
            // Track user activity for idle throttle
            if (event.type == SDL_MOUSEMOTION || event.type == SDL_MOUSEBUTTONDOWN ||
                event.type == SDL_MOUSEBUTTONUP || event.type == SDL_MOUSEWHEEL ||
                event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                hadActivity = true;
                lastActivityTime = std::chrono::high_resolution_clock::now();
            }
            if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
                if (scene.isModified()) {
                    gui.requestExit();
                } else {
                    quit = true;
                }
            } else if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                     width = event.window.data1;
                     height = event.window.data2;
                     
                     int drawableW, drawableH;
                     SDL_GL_GetDrawableSize(window, &drawableW, &drawableH);
                     float currentDpiScale = (width > 0) ? ((float)drawableW / (float)width) : 1.0f;
                     renderer.resize(drawableW, drawableH, currentDpiScale);
                     if (scene.getSplitMode() != Scene::SplitMode::OFF) {
                         int halfW = width / 2;
                         scene.getCamera().onResize(halfW, height);
                         if (scene.getCameraRight()) {
                             scene.getCameraRight()->onResize(width - halfW, height);
                         }
                     } else {
                         scene.getCamera().onResize(width, height);
                     }

                     Uint32 flags = SDL_GetWindowFlags(window);
                     bool isMax = (flags & SDL_WINDOW_MAXIMIZED) != 0;
                     int wx, wy;
                     SDL_GetWindowPosition(window, &wx, &wy);
                     gui.updateWindowBounds(wx, wy, width, height, isMax);
                } else if (event.window.event == SDL_WINDOWEVENT_MOVED) {
                     int wx = event.window.data1;
                     int wy = event.window.data2;
                     Uint32 flags = SDL_GetWindowFlags(window);
                     bool isMax = (flags & SDL_WINDOW_MAXIMIZED) != 0;
                     int ww, wh;
                     SDL_GetWindowSize(window, &ww, &wh);
                     gui.updateWindowBounds(wx, wy, ww, wh, isMax);
                } else if (event.window.event == SDL_WINDOWEVENT_MAXIMIZED) {
                     int wx, wy, ww, wh;
                     SDL_GetWindowPosition(window, &wx, &wy);
                     SDL_GetWindowSize(window, &ww, &wh);
                     gui.updateWindowBounds(wx, wy, ww, wh, true);
                } else if (event.window.event == SDL_WINDOWEVENT_RESTORED) {
                     int wx, wy, ww, wh;
                     SDL_GetWindowPosition(window, &wx, &wy);
                     SDL_GetWindowSize(window, &ww, &wh);
                     gui.updateWindowBounds(wx, wy, ww, wh, false);
                }
            } else {
                // If split viewport is active, determine active viewport based on the event mouse coordinates
                if (scene.getSplitMode() != Scene::SplitMode::OFF) {
                    if (!sculpt.isSculpting() && !sculpt.getCameraController().isDragging()) {
                        int mx = -1;
                        if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
                            mx = event.button.x;
                        } else if (event.type == SDL_MOUSEMOTION) {
                            mx = event.motion.x;
                        }
                        if (mx >= 0) {
                            int halfW = width / 2;
                            int activeVp = (mx >= halfW) ? 1 : 0;
                            scene.setActiveViewport(activeVp);
                        }
                    }
                }

                // If active viewport is 1 (right viewport), translate coordinates to local space for sculpt events
                SDL_Event eventCopy = event;
                if (scene.getSplitMode() != Scene::SplitMode::OFF && scene.getActiveViewport() == 1) {
                    int halfW = width / 2;
                    if (eventCopy.type == SDL_MOUSEBUTTONDOWN || eventCopy.type == SDL_MOUSEBUTTONUP) {
                        eventCopy.button.x -= halfW;
                    } else if (eventCopy.type == SDL_MOUSEMOTION) {
                        eventCopy.motion.x -= halfW;
                    }
                }

                ImGui_ImplSDL2_ProcessEvent(&event);

                bool handledByHotkey = dispatcher.processEvent(eventCopy, sculpt, scene, gui, &renderer);

                if (!handledByHotkey) {
                    ImGuiIO& io = ImGui::GetIO();
                    bool skipSculpt = false;
                    
                    // Prioritize ImGuizmo over camera orbit when hovering/interacting with the gizmo
                    if (sculpt.getBrush() == BRUSH_TRANSFORM && (ImGuizmo::IsOver() || ImGuizmo::IsUsing())) {
                        if (!sculpt.getCameraController().isDragging()) {
                            bool isCameraEvent = false;
                            if (eventCopy.type == SDL_MOUSEBUTTONDOWN || eventCopy.type == SDL_MOUSEBUTTONUP) {
                                if (eventCopy.button.button == SDL_BUTTON_RIGHT || eventCopy.button.button == SDL_BUTTON_MIDDLE) {
                                    isCameraEvent = true;
                                }
                            } else if (eventCopy.type == SDL_MOUSEWHEEL) {
                                isCameraEvent = true;
                            }
                            bool isAltClickSettingPivot = (eventCopy.type == SDL_MOUSEBUTTONDOWN && eventCopy.button.button == SDL_BUTTON_LEFT && ((SDL_GetModState() & KMOD_ALT) != 0) && !ImGuizmo::IsUsing());
                            if (!isCameraEvent && !isAltClickSettingPivot) {
                                skipSculpt = true;
                            }
                        }
                    }

                    if (!skipSculpt && io.WantCaptureMouse && (eventCopy.type == SDL_MOUSEBUTTONDOWN || eventCopy.type == SDL_MOUSEBUTTONUP || eventCopy.type == SDL_MOUSEMOTION || eventCopy.type == SDL_MOUSEWHEEL)) {
                        // Never skip mouse events if the camera controller is actively dragging/navigating,
                        // otherwise mouse up or mouse motion events will be swallowed by ImGui and lock navigation state.
                        if (!sculpt.getCameraController().isDragging()) {
                            bool isCameraEvent = false;
                            if (eventCopy.type == SDL_MOUSEBUTTONDOWN || eventCopy.type == SDL_MOUSEBUTTONUP) {
                                if (eventCopy.button.button == SDL_BUTTON_RIGHT || eventCopy.button.button == SDL_BUTTON_MIDDLE) {
                                    isCameraEvent = true;
                                }
                            } else if (eventCopy.type == SDL_MOUSEWHEEL) {
                                isCameraEvent = true;
                            }
                            
                            bool isGizmoHovered = (sculpt.getBrush() == BRUSH_TRANSFORM && (ImGuizmo::IsOver() || ImGuizmo::IsUsing()));
                            if (!(isCameraEvent && isGizmoHovered)) {
                                bool isAltClickSettingPivot = (sculpt.getBrush() == BRUSH_TRANSFORM && eventCopy.type == SDL_MOUSEBUTTONDOWN && eventCopy.button.button == SDL_BUTTON_LEFT && ((SDL_GetModState() & KMOD_ALT) != 0) && !ImGuizmo::IsUsing());
                                if (!isAltClickSettingPivot) {
                                    skipSculpt = true;
                                }
                            }
                        }
                    }
                    if (io.WantCaptureKeyboard && (eventCopy.type == SDL_KEYDOWN || eventCopy.type == SDL_KEYUP)) {
                        skipSculpt = true;
                    }

                    if (!skipSculpt) {
                        sculpt.handleEvent(eventCopy, scene);
                    }
                }
            }
        }

        // Hide or show system cursor during active sculpting
        bool sculptingNow = sculpt.isSculpting();
        ImGuiIO& io = ImGui::GetIO();
        if (sculptingNow) {
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            SDL_ShowCursor(SDL_DISABLE);
        } else {
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        }

        // Poll raw mouse position right before rendering to eliminate input lag
        if (dispatcher.getModalMode() == ModalMode::NONE) {
            int rawMouseX, rawMouseY;
            SDL_GetMouseState(&rawMouseX, &rawMouseY);
            if (scene.getSplitMode() != Scene::SplitMode::OFF && scene.getActiveViewport() == 1) {
                int halfW = width / 2;
                sculpt.setRawMousePos(rawMouseX - halfW, rawMouseY);
            } else {
                sculpt.setRawMousePos(rawMouseX, rawMouseY);
            }
        }

        if (gui.isRemeshRunning()) {
            sculpt.getCursor().hide();
        } else {
            sculpt.setModalMode(dispatcher.getModalMode());
            sculpt.processFrame(scene);
        }
        sculpt.getCursor().applyToRenderer(renderer);
        renderer.setLassoParameters(sculpt.isLassoActive(), sculpt.getLassoPoints(), sculpt.getLassoAlt(), sculpt.isMaskLasso());
        renderer.setActiveBrush(sculpt.getBrush());
        renderer.render(scene);
        gui.render(sculpt, scene, renderer, window);
        
        if (gui.isPendingQuit() && !scene.isModified() && !gui.isUnsavedModalOpen()) {
            quit = true;
        }
        
        bool isRemeshRunning = gui.isRemeshRunning();
        if (wasRemeshRunning && !isRemeshRunning) {
            dispatcher.resetModifiers(sculpt, &renderer);
        }
        wasRemeshRunning = isRemeshRunning;

        // If actively sculpting, mark as active
        if (sculpt.isSculpting() || sculpt.getCameraController().isDragging()) {
            hadActivity = true;
            lastActivityTime = std::chrono::high_resolution_clock::now();
        }

        SDL_GL_SwapWindow(window);

        // ---- GPU load limiter ----
        {
            auto frameEnd = std::chrono::high_resolution_clock::now();
            float frameMs = std::chrono::duration<float, std::milli>(frameEnd - currentFrameTime).count();

            bool fpsLimitOn  = gui.getFpsLimitEnabled();
            int  targetFps   = gui.getFpsLimit();          // e.g. 60
            float targetMs   = 1000.0f / (float)targetFps;

            // Idle throttle: >500 ms since last activity → drop to 15 FPS
            float idleMs = std::chrono::duration<float, std::milli>(
                               frameEnd - lastActivityTime).count();
            bool isIdle = (idleMs > 500.0f) && !gui.isRemeshRunning();
            if (isIdle) {
                targetMs = 1000.0f / 15.0f; // 15 FPS while idle
                fpsLimitOn = true;
            }

            if (fpsLimitOn && frameMs < targetMs) {
                Uint32 sleepMs = static_cast<Uint32>(targetMs - frameMs);
                if (sleepMs > 0) SDL_Delay(sleepMs);
            }
        }
    }

    // Save application settings on exit
    IniFile saveApp;
    RenderSettings::save(saveApp, renderer, scene);
    sculpt.saveSettings(saveApp);
    gui.saveSettings(saveApp);
    saveApp.save("app_settings.cfg");

#ifdef _WIN32
    g_tablet.wintabClose();
#endif

    gui.shutdown();

    std::cout << "Cleaning up..." << std::endl;
    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
} catch (const std::exception& e) {
        std::cerr << "\n=============================================" << std::endl;
        std::cerr << "[UNHANDLED EXCEPTION] " << e.what() << std::endl;
        std::cerr << "=============================================" << std::endl;
        #ifdef _WIN32
        system("pause");
        #else
        std::cout << "Press Enter to exit..." << std::endl;
        std::cin.get();
        #endif
        return -1;
    } catch (...) {
        std::cerr << "\n=============================================" << std::endl;
        std::cerr << "[UNHANDLED EXCEPTION] Unknown exception occurred." << std::endl;
        std::cerr << "=============================================" << std::endl;
        #ifdef _WIN32
        system("pause");
        #else
        std::cout << "Press Enter to exit..." << std::endl;
        std::cin.get();
        #endif
        return -1;
    }
}
