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
#include <imgui_internal.h>
#include <imgui_impl_sdl2.h>
#include <ImGuizmo.h>

#include "render/AngleRenderer.h"
#include "render/RenderBenchmark.h"
#include "common/OptFlags.h"
#include "common/FileUtils.h"
#include "mesh/BVH.h"
#include "mesh/BVHTest.h"
#include "mesh/SculptBenchmark.h"
#include "mesh/TrimClipBenchmark.h"
#include "mesh/VoxelRemeshBenchmark.h"
#include "mesh/NormalCalc.h"
#include "sculpt/SculptEngine.h"
#include "mesh/Topology.h"
#include "scene/Scene.h"
#include "mesh/Mesh.h"
#include "scene/Camera.h"
#include "editing/SculptManager.h"
#include "editing/undo/UndoManager.h"
#include "files/AutosaveManager.h"
#include "gui/GuiManager.h"

#include "platform/HotkeyDispatcher.h"
#include "platform/HotkeyManager.h"
#include "platform/TabletInput.h"
#include "platform/HardwareCursor.h"
#include "render/RenderSettings.h"
#include "common/IniFile.h"
#include "common/RecentFiles.h"
#include "common/Version.h"
#include "common/FormatConstants.h"
#include "common/StringUtils.h"
#include "common/Logger.h"
#include "common/CrashReporter.h"
#include "common/ErrorReporter.h"
#include "brushes/BrushPresetManager.h"
#include "files/FileManager.h"
#include "files/ImportGoZ.h"
#include "files/GoZBenchmark.h"
#include "files/GoZProfiler.h"
#include "zbrush_bridge/ZBrushBridgeConfig.h"
#include "zbrush_bridge/ZBrushWatcher.h"
#include "zbrush_bridge/ZBrushSender.h"
#include <unordered_set>
#include "zbrush_bridge/ZBrushBridgeBenchmark.h"
#include "stb_image.h"
#include "editing/PrimitiveTool.h"
#include "mesh/dynamic/DynamicTopologyTest.h"
#include "mesh/ClayBuildupBenchmark.h"
#include "mesh/SnakeHookBenchmark.h"
#include <cctype>
#ifndef _WIN32
#include <unistd.h>
#endif

static bool isImageFile(const std::string& path) {
    size_t dotPos = path.find_last_of('.');
    if (dotPos == std::string::npos) return false;
    std::string ext = path.substr(dotPos);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || 
            ext == ".tga" || ext == ".webp" || ext == ".gif" || ext == ".psd" || 
            ext == ".hdr" || ext == ".pic" || ext == ".pnm");
}

static bool is3DModelFile(const std::string& path) {
    size_t dotPos = path.find_last_of('.');
    if (dotPos == std::string::npos) return false;
    std::string ext = path.substr(dotPos);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return (ext == ".obj" || ext == ".stl" || ext == ".ply" || 
            ext == ".glb" || ext == ".gltf" || ext == ".fbx" ||
            ext == ".goz" ||
            ext == Format::PROJECT_DOT_EXT || ext == Format::LEGACY_DOT_EXT);
}

static void setAppWindowIcon(SDL_Window* window, const char* iconPath) {
    int w = 0, h = 0, channels = 0;
    stbi_set_flip_vertically_on_load(false);
    unsigned char* pixels = stbi_load(iconPath, &w, &h, &channels, 4);
    if (pixels) {
        SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(
            pixels, w, h, 32, w * 4,
            0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000
        );
        if (surface) {
            SDL_SetWindowIcon(window, surface);
            SDL_FreeSurface(surface);
        }
        stbi_image_free(pixels);
    }
}

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#include <fcntl.h>
#include <SDL2/SDL_syswm.h>

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

// Global SEH / signal / terminate handlers are managed by CrashReporter.

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
#ifdef _WIN32
    {
        std::string exeDir = FileUtils::getExecutableDir();
        if (!exeDir.empty()) {
            SetCurrentDirectoryW(utf8ToWide(exeDir).c_str());
        }
    }
#else
    // In standard Linux runs, change CWD to executable directory.
    // In AppImage, AppRun manages working directory in ~/.config/spsculpt (writable).
    if (!getenv("APPIMAGE") && !getenv("APPDIR")) {
        std::string exeDir = FileUtils::getExecutableDir();
        if (!exeDir.empty()) {
            if (chdir(exeDir.c_str()) != 0) {
                // ignore
            }
        }
    }
#endif

    bool showConsole = false;
    bool runBench = false;
    bool runLargeBrushBench = false;
    bool runTrimClipBench = false;
    bool runVoxelBench = false;
    bool runVoxelMemBench = false;
    bool runGozBench = false;
    bool runBridgeBench = false;
    bool runGozProfile = false;
    bool runRenderBench = false;
    bool runStage1Test = false;
    bool runStage2Test = false;
    bool runStage3Test = false;
    bool runStage4Test = false;
    bool runBVHTest = false;
    bool runDyntopoTest = false;
    bool runDyntopoBench = false;
    bool runDyntopoBench1000 = false;
    bool runDyntopoTool02Bench = false;
    bool runClayBench1000 = false;
    bool runClayDyntopoBench1000 = false;
    bool runSnakeHookBench = false;
    int snakeHookStrokes = 10;
    bool runCrashReporterTest = false;
    std::string gozBenchPath = "";
    std::string gozProfilePath = "";
    int benchFaces = 4000000;
    bool benchFacesOverridden = false;
    int benchRemeshRes = 0;
    std::string fileToOpen = "";

#ifdef _WIN32
    int numArgs = 0;
    LPWSTR* szArgList = CommandLineToArgvW(GetCommandLineW(), &numArgs);
    if (szArgList) {
        for (int i = 1; i < numArgs; ++i) {
            std::string arg = wideToUtf8(szArgList[i]);
            if (arg == "--console" || arg == "-console" || arg == "-c" || arg == "--show-console") {
                showConsole = true;
            } else if (arg == "--test-crash-reporter") {
                runCrashReporterTest = true;
                showConsole = true;
            } else if (arg == "--test-bvh" || arg == "--bench-bvh") {
                runBVHTest = true;
                showConsole = true;
            } else if (arg == "--test-dyntopo" || arg == "--test-dynamic-topology") {
                runDyntopoTest = true;
                showConsole = true;
            } else if (arg == "--bench-dyntopo-1000" || arg == "--bench-dyntopo1000" || arg == "--bench-1000") {
                runDyntopoBench1000 = true;
                showConsole = true;
            } else if (arg == "--bench-clay-1000" || arg == "--bench-claybuildup-1000" || arg == "--bench-clay") {
                runClayBench1000 = true;
                showConsole = true;
            } else if (arg == "--bench-clay-dyntopo-1000" || arg == "--bench-claybuildup-dyntopo-1000") {
                runClayDyntopoBench1000 = true;
                showConsole = true;
            } else if (arg == "--bench-snakehook" || arg == "--test-snakehook" || arg == "--bench-snakehook-dyntopo") {
                runSnakeHookBench = true;
                showConsole = true;
                if (i + 1 < numArgs && szArgList[i + 1][0] != L'-') {
                    snakeHookStrokes = std::clamp(std::stoi(wideToUtf8(szArgList[++i])), 1, 1000);
                }
            } else if (arg == "--bench-dyntopo" || arg == "--bench-dynamic-topology") {
                runDyntopoBench = true;
                showConsole = true;
            } else if (arg == "--bench-dyntopo-02" || arg == "--bench-tool-02" || arg == "--bench-dyntopo-tool-02") {
                runDyntopoTool02Bench = true;
                showConsole = true;
            } else if (arg == "--bench-bridge" || arg == "--bench-zbrush-bridge") {
                runBridgeBench = true;
                showConsole = true;
            } else if (arg == "--profile-goz" || arg == "--profile-goz-import" || arg == "--goz-profile") {
                runGozProfile = true;
                showConsole = true;
                if (i + 1 < numArgs && szArgList[i + 1][0] != L'-') {
                    gozProfilePath = wideToUtf8(szArgList[++i]);
                }
            } else if (arg == "--bench-goz" || arg == "--bench-goz-import") {
                runGozBench = true;
                showConsole = true;
                if (i + 1 < numArgs && szArgList[i + 1][0] != L'-') {
                    gozBenchPath = wideToUtf8(szArgList[++i]);
                }
            } else if (arg == "--test-claybuildup-50m") {
                runBench = true;
                benchFaces = 50000000;
                showConsole = true;
            } else if (arg == "--bench-trim" || arg == "--bench-clip" || arg == "--bench-trim-clip" || arg == "--bench-curves") {
                runTrimClipBench = true;
                showConsole = true;
            } else if (arg == "--bench-remesh" || arg == "--bench-voxel" || arg == "--bench-voxel-remesh") {
                runVoxelBench = true;
                showConsole = true;
            } else if (arg == "--bench-remesh-mem" || arg == "--bench-voxel-mem") {
                runVoxelBench = true;
                runVoxelMemBench = true;
                showConsole = true;
            } else if ((arg == "--bench-remesh-res" || arg == "--remesh-res") && i + 1 < numArgs) {
                showConsole = true;
                benchRemeshRes = std::atoi(wideToUtf8(szArgList[++i]).c_str());
            } else if (arg == "--bench-all") {
                runBench = true;
                runTrimClipBench = true;
                runVoxelBench = true;
                showConsole = true;
            } else if (arg == "--test-claybuildup-4m" || arg == "--bench-claybuildup" || arg == "--bench-multitool" || arg == "--benchmark") {
                runBench = true;
                showConsole = true;
            } else if (arg == "--bench-large-brush" || arg == "--bench-large") {
                runLargeBrushBench = true;
                showConsole = true;
            } else if (arg == "--bench-render" || arg == "--render-bench") {
                runRenderBench = true;
                showConsole = true;
            } else if (arg == "--test-stage1" || arg == "--stage1-test") {
                runStage1Test = true;
                showConsole = true;
            } else if (arg == "--test-stage2" || arg == "--stage2-test") {
                runStage2Test = true;
                showConsole = true;
            } else if (arg == "--test-stage3" || arg == "--stage3-test") {
                runStage3Test = true;
                showConsole = true;
            } else if (arg == "--test-stage4" || arg == "--stage4-test") {
                runStage4Test = true;
                showConsole = true;
            } else if ((arg == "--bench-render-faces" || arg == "--bench-faces" || arg == "-bf") && i + 1 < numArgs) {
                showConsole = true;
                benchFaces = std::atoi(wideToUtf8(szArgList[++i]).c_str());
                benchFacesOverridden = true;
            } else if (fileToOpen.empty() && !arg.empty() && arg[0] != '-') {
                fileToOpen = arg;
            }
        }
        LocalFree(szArgList);
    }
#else
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--console" || arg == "-console" || arg == "-c" || arg == "--show-console") {
            showConsole = true;
        } else if (arg == "--test-crash-reporter") {
            runCrashReporterTest = true;
            showConsole = true;
        } else if (arg == "--test-bvh" || arg == "--bench-bvh") {
            runBVHTest = true;
            showConsole = true;
        } else if (arg == "--test-dyntopo" || arg == "--test-dynamic-topology") {
            runDyntopoTest = true;
            showConsole = true;
        } else if (arg == "--bench-dyntopo-1000" || arg == "--bench-dyntopo1000" || arg == "--bench-1000") {
            runDyntopoBench1000 = true;
            showConsole = true;
        } else if (arg == "--bench-clay-1000" || arg == "--bench-claybuildup-1000" || arg == "--bench-clay") {
            runClayBench1000 = true;
            showConsole = true;
        } else if (arg == "--bench-clay-dyntopo-1000" || arg == "--bench-claybuildup-dyntopo-1000") {
            runClayDyntopoBench1000 = true;
            showConsole = true;
        } else if (arg == "--bench-snakehook" || arg == "--test-snakehook" || arg == "--bench-snakehook-dyntopo") {
            runSnakeHookBench = true;
            showConsole = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                snakeHookStrokes = std::clamp(std::stoi(argv[++i]), 1, 1000);
            }
        } else if (arg == "--bench-dyntopo" || arg == "--bench-dynamic-topology") {
            runDyntopoBench = true;
            showConsole = true;
        } else if (arg == "--bench-dyntopo-02" || arg == "--bench-tool-02" || arg == "--bench-dyntopo-tool-02") {
            runDyntopoTool02Bench = true;
            showConsole = true;
        } else if (arg == "--bench-bridge" || arg == "--bench-zbrush-bridge") {
            runBridgeBench = true;
            showConsole = true;
        } else if (arg == "--profile-goz" || arg == "--profile-goz-import" || arg == "--goz-profile") {
            runGozProfile = true;
            showConsole = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                gozProfilePath = argv[++i];
            }
        } else if (arg == "--bench-goz" || arg == "--bench-goz-import") {
            runGozBench = true;
            showConsole = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                gozBenchPath = argv[++i];
            }
        } else if (arg == "--test-claybuildup-50m") {
            runBench = true;
            benchFaces = 50000000;
            showConsole = true;
        } else if (arg == "--bench-trim" || arg == "--bench-clip" || arg == "--bench-trim-clip" || arg == "--bench-curves") {
            runTrimClipBench = true;
            showConsole = true;
        } else if (arg == "--bench-remesh" || arg == "--bench-voxel" || arg == "--bench-voxel-remesh") {
            runVoxelBench = true;
            showConsole = true;
        } else if (arg == "--bench-remesh-mem" || arg == "--bench-voxel-mem") {
            runVoxelBench = true;
            runVoxelMemBench = true;
            showConsole = true;
        } else if ((arg == "--bench-remesh-res" || arg == "--remesh-res") && i + 1 < argc) {
            showConsole = true;
            benchRemeshRes = std::atoi(argv[++i]);
        } else if (arg == "--bench-all") {
            runBench = true;
            runTrimClipBench = true;
            runVoxelBench = true;
            showConsole = true;
        } else if (arg == "--test-claybuildup-4m" || arg == "--bench-claybuildup" || arg == "--bench-multitool" || arg == "--benchmark") {
            runBench = true;
            showConsole = true;
        } else if (arg == "--bench-large-brush" || arg == "--bench-large") {
            runLargeBrushBench = true;
            showConsole = true;
        } else if (arg == "--bench-render" || arg == "--render-bench") {
            runRenderBench = true;
            showConsole = true;
        } else if (arg == "--test-stage1" || arg == "--stage1-test") {
            runStage1Test = true;
            showConsole = true;
        } else if (arg == "--test-stage2" || arg == "--stage2-test") {
            runStage2Test = true;
            showConsole = true;
        } else if (arg == "--test-stage3" || arg == "--stage3-test") {
            runStage3Test = true;
            showConsole = true;
        } else if (arg == "--test-stage4" || arg == "--stage4-test") {
            runStage4Test = true;
            showConsole = true;
        } else if ((arg == "--bench-render-faces" || arg == "--bench-faces" || arg == "-bf") && i + 1 < argc) {
            showConsole = true;
            benchFaces = std::atoi(argv[++i]);
            benchFacesOverridden = true;
        } else if (fileToOpen.empty() && !arg.empty() && arg[0] != '-') {
            fileToOpen = arg;
        }
    }
#endif

#ifdef _WIN32
    if (showConsole) {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD outType = GetFileType(hOut);
        if (hOut == NULL || hOut == INVALID_HANDLE_VALUE || (outType != FILE_TYPE_PIPE && outType != FILE_TYPE_DISK)) {
            if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
                AllocConsole();
            }
            FILE* fp;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
            freopen_s(&fp, "CONIN$", "r", stdin);
        } else {
            int fdOut = _open_osfhandle((intptr_t)hOut, _O_TEXT);
            if (fdOut >= 0) {
                _dup2(fdOut, 1);
            }
            HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
            if (hErr && hErr != INVALID_HANDLE_VALUE) {
                int fdErr = _open_osfhandle((intptr_t)hErr, _O_TEXT);
                if (fdErr >= 0) {
                    _dup2(fdErr, 2);
                }
            }
        }
        std::cout.clear();
        std::cerr.clear();
        std::cin.clear();
    }
    // Configure Windows process-level DPI awareness to avoid system-level scaling blur
    SetProcessDPIAware();
#endif

    const bool isHeadlessRun = runDyntopoTest || runDyntopoBench || runDyntopoTool02Bench ||
                               runDyntopoBench1000 || runClayBench1000 || runClayDyntopoBench1000 ||
                               runSnakeHookBench ||
                               runBVHTest || runBridgeBench || runGozBench || runVoxelBench ||
                               runTrimClipBench || runBench || runGozProfile || runRenderBench ||
                               runStage1Test || runStage2Test || runCrashReporterTest;

    CrashReporter::init(isHeadlessRun);
    sculpt_log_init(isHeadlessRun);

    setvbuf(stdout, NULL, _IONBF, 0);
    if (runCrashReporterTest) {
        CrashReporter::ScopedStage testStage("SelfTest::CrashReporter");
        CrashReporter::recordBreadcrumb("TEST", "Triggered --test-crash-reporter CLI check");
        CrashReporter::updateStrokeContext(
            static_cast<int>(BRUSH_CLAYBUILDUP),
            getBrushNameStr(BRUSH_CLAYBUILDUP),
            42ULL,
            7u,
            12345u,
            24000u,
            320u,
            610u,
            false,
            true,
            0.25f,
            0.5f,
            1.0f,
            2.0f,
            3.0f
        );
        sculpt_log_warn("CrashReporter self-test warning sample");
        sculpt_log_error("CrashReporter self-test error sample");
        CrashReporter::DiagnosticPaths paths = CrashReporter::writeDiagnosticSnapshot(
            "SelfTest Diagnostic Snapshot",
            "Triggered via --test-crash-reporter CLI flag",
            true,
            false
        );
        CrashReporter::clearStrokeContext();
        std::cout << "[CrashReporter SelfTest] SessionLog=" << Logger::instance().getSessionLogPath() << std::endl;
        std::cout << "[CrashReporter SelfTest] Report=" << paths.reportPath << std::endl;
        std::cout << "[CrashReporter SelfTest] Dump=" << paths.dumpPath << std::endl;
        bool ok = !paths.reportPath.empty();
#ifdef _WIN32
        ok = ok && !paths.dumpPath.empty();
#endif
        fflush(stdout);
        fflush(stderr);
#ifdef _WIN32
        ExitProcess(ok ? 0 : 1);
#else
        _exit(ok ? 0 : 1);
#endif
    }
    if (runDyntopoTest) {
        bool success = dynamic::DynamicTopologyTest::runAllTests();
        fflush(stdout);
        fflush(stderr);
#ifdef _WIN32
        ExitProcess(success ? 0 : 1);
#else
        _exit(success ? 0 : 1);
#endif
    }

    if (runDyntopoBench) {
        bool success = dynamic::DynamicTopologyTest::runBenchmark100Strokes();
        fflush(stdout);
        fflush(stderr);
#ifdef _WIN32
        ExitProcess(success ? 0 : 1);
#else
        _exit(success ? 0 : 1);
#endif
    }

    if (runDyntopoTool02Bench) {
        bool success = dynamic::DynamicTopologyTest::runBenchmarkTool02();
        fflush(stdout);
        fflush(stderr);
#ifdef _WIN32
        ExitProcess(success ? 0 : 1);
#else
        _exit(success ? 0 : 1);
#endif
    }

    if (runDyntopoBench1000) {
        bool success = dynamic::DynamicTopologyTest::runBenchmark1000Strokes();
        fflush(stdout);
        fflush(stderr);
#ifdef _WIN32
        ExitProcess(success ? 0 : 1);
#else
        _exit(success ? 0 : 1);
#endif
    }

    if (runClayBench1000) {
        bool success = ClayBuildupBenchmark::run1000Strokes(false, 1000);
        fflush(stdout);
        fflush(stderr);
#ifdef _WIN32
        ExitProcess(success ? 0 : 1);
#else
        _exit(success ? 0 : 1);
#endif
    }

    if (runClayDyntopoBench1000) {
        bool success = ClayBuildupBenchmark::run1000Strokes(true, 1000);
        fflush(stdout);
        fflush(stderr);
#ifdef _WIN32
        ExitProcess(success ? 0 : 1);
#else
        _exit(success ? 0 : 1);
#endif
    }

    if (runSnakeHookBench) {
        bool success = SnakeHookBenchmark::run(snakeHookStrokes, true, true);
        fflush(stdout);
        fflush(stderr);
#ifdef _WIN32
        ExitProcess(success ? 0 : 1);
#else
        _exit(success ? 0 : 1);
#endif
    }

    if (runBVHTest) {
        bool success = BVHValidation::run();
        fflush(stdout);
        fflush(stderr);
#ifdef _WIN32
        ExitProcess(success ? 0 : 1);
#else
        _exit(success ? 0 : 1);
#endif
    }

    if (runBridgeBench) {
        bool success = ZBrushBridge::runZBrushBridgeBenchmark();
        fflush(stdout);
        fflush(stderr);
#ifdef _WIN32
        ExitProcess(success ? 0 : 1);
#else
        _exit(success ? 0 : 1);
#endif
    }

    if (runGozBench) {
        bool success = GoZBenchmark::run(gozBenchPath);
        fflush(stdout);
        fflush(stderr);
#ifdef _WIN32
        ExitProcess(success ? 0 : 1);
#else
        _exit(success ? 0 : 1);
#endif
    }

    if (runVoxelBench && !runBench && !runTrimClipBench) {
        bool success = VoxelRemeshBenchmark::run(benchRemeshRes, runVoxelMemBench);
        fflush(stdout);
        fflush(stderr);
#ifdef _WIN32
        ExitProcess(success ? 0 : 1);
#else
        _exit(success ? 0 : 1);
#endif
    }

    if (runTrimClipBench && !runBench) {
        bool success = TrimClipBenchmark::run(benchFaces);
        if (runVoxelBench) {
            success = VoxelRemeshBenchmark::run(benchRemeshRes, runVoxelMemBench) && success;
        }
        fflush(stdout);
        fflush(stderr);
#ifdef _WIN32
        ExitProcess(success ? 0 : 1);
#else
        _exit(success ? 0 : 1);
#endif
    }

    if (runBench || runLargeBrushBench) {
        if (runLargeBrushBench && !benchFacesOverridden) {
            benchFaces = 10000000;
        }
        bool success = true;
        if (runBench) {
            success = SculptBenchmark::run(benchFaces) && success;
        }
        if (runLargeBrushBench) {
            success = SculptBenchmark::runLargeBrush(benchFaces) && success;
        }
        if (runTrimClipBench) {
            success = TrimClipBenchmark::run(benchFaces) && success;
        }
        if (runVoxelBench) {
            success = VoxelRemeshBenchmark::run(benchRemeshRes, runVoxelMemBench) && success;
        }
        fflush(stdout);
        fflush(stderr);
#ifdef _WIN32
        ExitProcess(success ? 0 : 1);
#else
        _exit(success ? 0 : 1);
#endif
    }

    try {
        CrashReporter::ScopedStage initStage("Startup::Init");
        SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    std::cout << "Starting SPSculpt Desktop Core v" << Version::STRING << "..." << std::endl;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL initialization failed: " << SDL_GetError() << std::endl;
        return -1;
    }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    // Request OpenGL ES 3.0 Context
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    IniFile appSettings;
    appSettings.load("app_settings.cfg");

    AutosaveManager autosaveManager;
    GuiManager gui;
    gui.setAutosaveManager(&autosaveManager);
    gui.loadSettings(appSettings);
    RecentFiles recentFiles;
    recentFiles.loadFromIni(appSettings);
    gui.setRecentFiles(&recentFiles);

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
        (std::string("SPSculpt v") + Version::STRING).c_str(),
        posX, posY,
        width, height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | (startMaximized ? SDL_WINDOW_MAXIMIZED : 0)
    );

    if (!window) {
        std::cerr << "Failed to create SDL window: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return -1;
    }

    setAppWindowIcon(window, "resources/icons/app_icon.png");

    // Retrieve actual size in case the window started maximized or OS adjusted it
    SDL_GetWindowSize(window, &width, &height);

    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    if (!glContext) {
        std::cerr << "Failed to create OpenGL ES context: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    // Use adaptive vsync (0) so our SDL_Delay-based frame cap can actually control timing,
    // and the early draw does not block inside the driver waiting for vertical sync.
    SDL_GL_SetSwapInterval(0);

    // Early draw: clear background immediately to eliminate black screen during initialization
    glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(window);

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
    CrashReporter::reinstallHandlers();

    Scene scene;
    scene.setOnMeshDeletedCallback([&renderer](Mesh* m) {
        renderer.releaseMeshBuffers(m);
    });
    scene.getCamera().onResize(width, height);
    if (fileToOpen.empty() && !runRenderBench && !runStage1Test && !runStage2Test && !runStage3Test && !runGozProfile) {
        if (gui.getCreateDefaultSphereOnStartup()) {
            scene.loadDefaultSphere();
        }
    }

    if (runGozProfile) {
        bool ok = GoZProfiler::runCliBenchmark(gozProfilePath, scene, renderer, window);
        fflush(stdout);
        fflush(stderr);
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        SDL_Quit();
#ifdef _WIN32
        ExitProcess(ok ? 0 : 1);
#else
        _exit(ok ? 0 : 1);
#endif
    }


    // Apply default matcap preset globally to the renderer
    int defaultFvIdx = renderer.findMatcapIndexByName("Matcap FV");
    renderer.setMatcap(defaultFvIdx >= 0 ? defaultFvIdx : 3); // Default to "Matcap FV"
    renderer.setShaderType(1); // MATCAP
    renderer.setTextureId(0);

    (void)OptFlags::get();

    if (runRenderBench) {
        BrushPresetManager::instance().loadDefaults();
        SculptManager sculpt;
        scene.setSculptManager(&sculpt);
        g_undoManager.setSculptManager(&sculpt);
        sculpt.setRenderer(&renderer);
        bool ok = RenderBenchmark::run(benchFaces, scene, sculpt, renderer, window);
        scene.setSculptManager(nullptr);
        fflush(stdout);
        fflush(stderr);
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        SDL_Quit();
#ifdef _WIN32
        ExitProcess(ok ? 0 : 1);
#else
        _exit(ok ? 0 : 1);
#endif
    }

    if (runStage1Test) {
        BrushPresetManager::instance().loadDefaults();
        SculptManager sculpt;
        scene.setSculptManager(&sculpt);
        g_undoManager.setSculptManager(&sculpt);
        sculpt.setRenderer(&renderer);
        bool ok = RenderBenchmark::runStage1Tests(scene, sculpt, renderer, window);
        scene.setSculptManager(nullptr);
        fflush(stdout);
        fflush(stderr);
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        SDL_Quit();
#ifdef _WIN32
        ExitProcess(ok ? 0 : 1);
#else
        _exit(ok ? 0 : 1);
#endif
    }

    if (runStage2Test) {
        BrushPresetManager::instance().loadDefaults();
        SculptManager sculpt;
        scene.setSculptManager(&sculpt);
        g_undoManager.setSculptManager(&sculpt);
        sculpt.setRenderer(&renderer);
        bool ok = RenderBenchmark::runStage2Tests(scene, sculpt, renderer, window);
        scene.setSculptManager(nullptr);
        fflush(stdout);
        fflush(stderr);
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        SDL_Quit();
#ifdef _WIN32
        ExitProcess(ok ? 0 : 1);
#else
        _exit(ok ? 0 : 1);
#endif
    }

    if (runStage3Test) {
        BrushPresetManager::instance().loadDefaults();
        SculptManager sculpt;
        scene.setSculptManager(&sculpt);
        g_undoManager.setSculptManager(&sculpt);
        sculpt.setRenderer(&renderer);
        bool ok = RenderBenchmark::runStage3Tests(scene, sculpt, renderer, window);
        scene.setSculptManager(nullptr);
        fflush(stdout);
        fflush(stderr);
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        SDL_Quit();
#ifdef _WIN32
        ExitProcess(ok ? 0 : 1);
#else
        _exit(ok ? 0 : 1);
#endif
    }

    if (runStage4Test) {
        BrushPresetManager::instance().loadDefaults();
        SculptManager sculpt;
        scene.setSculptManager(&sculpt);
        g_undoManager.setSculptManager(&sculpt);
        sculpt.setRenderer(&renderer);
        bool ok = RenderBenchmark::runStage4Tests(scene, sculpt, renderer, window);
        scene.setSculptManager(nullptr);
        fflush(stdout);
        fflush(stderr);
        SDL_GL_DeleteContext(glContext);
        SDL_DestroyWindow(window);
        SDL_Quit();
#ifdef _WIN32
        ExitProcess(ok ? 0 : 1);
#else
        _exit(ok ? 0 : 1);
#endif
    }

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
    scene.setSculptManager(&sculpt);
    g_undoManager.setSculptManager(&sculpt);
    gui.init(window, glContext);
    CrashReporter::registerCurrentThread("MainThread");
    CrashReporter::reinstallHandlers();
    {
        CrashReporter::PreviousCrashInfo prevCrash;
        if (CrashReporter::consumePreviousCrash(prevCrash)) {
            sculpt_log_warn("[Startup] Previous crash detected (%s): %s\n",
                            prevCrash.timestamp.c_str(), prevCrash.summary.c_str());
            gui.setPreviousCrashInfo(prevCrash);
        }
    }
    autosaveManager.initOnStartup();

    HardwareCursorManager hardwareCursor;
    hardwareCursor.init(gui.getUiScale());

    gui.setRenderer(&renderer);
    sculpt.setRenderer(&renderer);
    HotkeyDispatcher dispatcher;
    HotkeyManager hotkeyManager;
    hotkeyManager.init();

    // Legacy migration: if older app_settings has Camera.cameraNavigationStyle and no Hotkeys.activePreset
    if (appSettings.hasKey("Camera", "cameraNavigationStyle") && !appSettings.hasKey("Hotkeys", "activePreset")) {
        int oldStyle = appSettings.getInt("Camera", "cameraNavigationStyle", 0);
        if (oldStyle == 1) {
            hotkeyManager.setActivePreset("Maya");
        } else if (oldStyle == 2) {
            hotkeyManager.setActivePreset("Blender");
        } else {
            hotkeyManager.setActivePreset("ZBrush");
        }
    } else {
        hotkeyManager.loadActivePresetName(appSettings);
    }

    // Sync camera navigation style with active preset
    {
        const std::string& name = hotkeyManager.getActivePreset().name;
        CameraEnums::NavigationStyle syncStyle = CameraEnums::NavigationStyle::ZBRUSH;
        if (name == "Maya") syncStyle = CameraEnums::NavigationStyle::MAYA;
        else if (name == "Blender") syncStyle = CameraEnums::NavigationStyle::BLENDER;
        scene.getCamera().setNavigationStyle(syncStyle);
        for (int cIdx = 0; cIdx < 2; ++cIdx) {
            Camera* cam = scene.getCameraByIndex(cIdx);
            if (cam) cam->setNavigationStyle(syncStyle);
        }
    }

    dispatcher.setHotkeyManager(&hotkeyManager);
    sculpt.setHotkeyManager(&hotkeyManager);
    gui.setHotkeyManager(&hotkeyManager);

    if (!fileToOpen.empty()) {
        std::cout << "Opening file from command line parameter: " << fileToOpen << std::endl;
        gui.openSceneFromPath(fileToOpen, scene, &sculpt, &renderer);
        if (scene.getMeshes().empty()) {
            std::cerr << "Failed to open scene or file was empty." << std::endl;
            if (gui.getCreateDefaultSphereOnStartup()) {
                std::cerr << "Restoring default sphere." << std::endl;
                scene.loadDefaultSphere();
            }
        }
    }

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
#else
    g_tablet.startPolling();
#endif

    auto lastFrameTime = std::chrono::high_resolution_clock::now();
    auto lastActivityTime = std::chrono::high_resolution_clock::now(); // for idle throttle
    GoZProfiler::ProfileRecord pendingLiveSyncProfile;
    bool pendingLiveSyncActive = false;
    std::chrono::high_resolution_clock::time_point tLiveSyncPipelineStart;

    bool quit = false;
    SDL_Event event;
    std::cout << "[Debug] Entering main loop." << std::endl;
    bool wasRemeshRunning = false;
    bool wasDecimRunning = false;
    bool pendingWakeActivity = false;
    int settleFramesRemaining = 2;
    int lastMotionX = std::numeric_limits<int>::min();
    int lastMotionY = std::numeric_limits<int>::min();
    uint64_t lastWorkTimerSecond = gui.getWorkTimer().getSessionSeconds();
    int consecutiveRenderErrors = 0;

    // Start background watcher for ZBrush live companion viewport
    std::unique_ptr<ZBrushBridge::ZBrushWatcher> zbrushWatcher = std::make_unique<ZBrushBridge::ZBrushWatcher>();
    if (ZBrushBridge::Config::isAutoSyncEnabled()) {
        zbrushWatcher->start();
        sculpt_log("[ZBrush Bridge] Live companion watcher started on: '%s'\n",
                   zbrushWatcher->getWatchDirectory().c_str());
    } else {
        sculpt_log("[ZBrush Bridge] Live companion watcher initialized (auto-sync paused) on: '%s'\n",
                   zbrushWatcher->getWatchDirectory().c_str());
    }
    gui.setZBrushWatcher(zbrushWatcher.get());
    CrashReporter::setStage("MainLoop");

    while (!quit) {
        auto currentFrameTime = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(currentFrameTime - lastFrameTime).count();
        lastFrameTime = currentFrameTime;

        bool hadActivity = pendingWakeActivity; // set true on any user input this frame
        pendingWakeActivity = false;

        // Check for live ZBrush full scene sync updates
        if (zbrushWatcher && zbrushWatcher->hasPendingScene()) {
            ZBrushBridge::SceneSnapshot sceneSnapshot;
            if (zbrushWatcher->consumePendingScene(sceneSnapshot)) {
                try {
                    CrashReporter::ScopedStage zSyncStage("ZBrush::SceneSync");
                    CrashReporter::recordBreadcrumb("ZBRUSH", "SceneSync subtools=%zu", sceneSnapshot.subtools.size());
                    auto tSceneSyncStart = std::chrono::high_resolution_clock::now();
                    bool triangulate = !ZBrushBridge::Config::isKeepNativeQuadsEnabled();
                    std::unordered_set<std::string> incomingNames;

                    int updatedCount = 0;
                    int addedCount = 0;

                    for (auto& st : sceneSnapshot.subtools) {
                        incomingNames.insert(st.name);

                        // Find matching mesh in scene by outlinerName
                        Mesh* existing = nullptr;
                        for (Mesh* m : scene.getMeshes()) {
                            if (m && m->outlinerName == st.name) {
                                existing = m;
                                break;
                            }
                        }

                        if (st.isNewOrModified && !st.buffer.empty()) {
                            ImportGoZ::GoZInfo info;
                            ImportGoZ::GoZParserStats parserStats;
                            auto meshes = ImportGoZ::importGoZ(st.buffer, triangulate, &info, &parserStats);
                            st.buffer.clear();
                            st.buffer.shrink_to_fit();

                            if (!meshes.empty() && meshes[0]) {
                                Mesh* incomingMesh = meshes[0];
                                incomingMesh->outlinerName = st.name;
                                incomingMesh->visibleV1 = st.visible;
                                incomingMesh->visibleV2 = st.visible;

                                if (!ZBrushBridge::Config::isImportPolyPaintEnabled()) {
                                    incomingMesh->colors.assign(incomingMesh->nbVerts * 3, 1.0f);
                                }
                                if (!ZBrushBridge::Config::isImportPolyGroupsEnabled()) {
                                    incomingMesh->initFaceGroups();
                                }

                                if (existing) {
                                    renderer.notifyMeshReplaced(existing, incomingMesh);
                                    scene.replaceMesh(existing, incomingMesh);
                                    updatedCount++;
                                } else {
                                    scene.addMesh(incomingMesh);
                                    addedCount++;
                                }
                            }
                        } else if (existing) {
                            // Geometry unchanged, update visibility only
                            existing->visibleV1 = st.visible;
                            existing->visibleV2 = st.visible;
                        }
                    }

                    // Auto-delete removed subtools if enabled
                    if (ZBrushBridge::Config::isAutoDeleteRemovedSubTools()) {
                        std::vector<Mesh*> toDelete;
                        for (Mesh* m : scene.getMeshes()) {
                            if (m && incomingNames.find(m->outlinerName) == incomingNames.end() &&
                                m->outlinerName != "Default Sphere") {
                                toDelete.push_back(m);
                            }
                        }
                        for (Mesh* m : toDelete) {
                            scene.deleteMesh(m);
                        }
                    }

                    // Update active selection
                    if (sceneSnapshot.activeIndex >= 0 && sceneSnapshot.activeIndex < static_cast<int>(sceneSnapshot.subtools.size())) {
                        const std::string& actName = sceneSnapshot.subtools[sceneSnapshot.activeIndex].name;
                        for (Mesh* m : scene.getMeshes()) {
                            if (m && m->outlinerName == actName) {
                                scene.selectMesh(m);
                                break;
                            }
                        }
                    }

                    scene.setModified(true);
                    hadActivity = true;
                    settleFramesRemaining = 2;
                    lastActivityTime = std::chrono::high_resolution_clock::now();
                    auto tSceneSyncEnd = std::chrono::high_resolution_clock::now();
                    double msSync = std::chrono::duration<double, std::milli>(tSceneSyncEnd - tSceneSyncStart).count();
                    sculpt_log("[ZBrush Bridge] Full scene synced in %.2f ms (%d updated, %d added, total meshes: %zu)\n",
                               msSync, updatedCount, addedCount, scene.getMeshes().size());
                    gui.showToast("ZBrush Scene Synced: " + std::to_string(sceneSnapshot.subtools.size()) + " SubTools", 2.5f);
                } catch (const std::exception& e) {
                    sceneSnapshot.subtools.clear();
                    ErrorReporter::instance().reportError("ZBrush Scene Sync Failed", "Failed to synchronize ZBrush scene update.", e.what(), true);
                } catch (...) {
                    sceneSnapshot.subtools.clear();
                    ErrorReporter::instance().reportError("ZBrush Scene Sync Failed", "Unknown error synchronizing ZBrush scene update.", "", true);
                }
            }
        }

        // Check for live ZBrush companion sync updates
        if (zbrushWatcher && zbrushWatcher->hasPendingMesh()) {
            std::vector<uint8_t> gozBuffer;
            ZBrushBridge::WatcherMeshStats watcherStats;
            if (zbrushWatcher->consumePendingMesh(gozBuffer, &watcherStats)) {
                try {
                    CrashReporter::ScopedStage zMeshStage("ZBrush::MeshSync");
                    CrashReporter::recordBreadcrumb("ZBRUSH", "MeshSync bytes=%zu", gozBuffer.size());
                    auto tGoZStart = std::chrono::high_resolution_clock::now();
                    ImportGoZ::GoZInfo info;
                    ImportGoZ::GoZParserStats parserStats;
                    bool triangulate = !ZBrushBridge::Config::isKeepNativeQuadsEnabled();
                    auto meshes = ImportGoZ::importGoZ(gozBuffer, triangulate, &info, &parserStats);
                    gozBuffer.clear();
                    gozBuffer.shrink_to_fit();
                    if (!meshes.empty() && meshes[0]) {
                        Mesh* incomingMesh = meshes[0];
                        if (!ZBrushBridge::Config::isImportPolyPaintEnabled()) {
                            incomingMesh->colors.assign(incomingMesh->nbVerts * 3, 1.0f);
                        }
                        if (!ZBrushBridge::Config::isImportPolyGroupsEnabled()) {
                            incomingMesh->initFaceGroups();
                        }
                        Mesh* targetMesh = nullptr;
                        for (Mesh* m : scene.getMeshes()) {
                            if (m && m->outlinerName == "ZBrush Companion") {
                                targetMesh = m;
                                break;
                            }
                        }
                        auto tSceneStart = std::chrono::high_resolution_clock::now();
                        if (targetMesh) {
                            incomingMesh->outlinerName = targetMesh->outlinerName;
                            renderer.notifyMeshReplaced(targetMesh, incomingMesh);
                            scene.replaceMesh(targetMesh, incomingMesh);
                        } else if (!scene.getMeshes().empty() && scene.getSelected()) {
                            Mesh* sel = scene.getSelected();
                            incomingMesh->outlinerName = sel->outlinerName.empty() ? "ZBrush Companion" : sel->outlinerName;
                            renderer.notifyMeshReplaced(sel, incomingMesh);
                            scene.replaceMesh(sel, incomingMesh);
                        } else {
                            incomingMesh->outlinerName = "ZBrush Companion";
                            scene.addMesh(incomingMesh);
                            scene.selectMesh(incomingMesh);
                            scene.getCamera().resetViewToMeshes({incomingMesh}, false);
                        }
                        auto tSceneEnd = std::chrono::high_resolution_clock::now();
                        double msScene = std::chrono::duration<double, std::milli>(tSceneEnd - tSceneStart).count();

                        // Prepare live sync profile record
                        pendingLiveSyncProfile = GoZProfiler::ProfileRecord();
                        pendingLiveSyncProfile.filePath = watcherStats.filePath.empty() ? (zbrushWatcher->getWatchDirectory() + "mesh.GoZ") : watcherStats.filePath;
                        pendingLiveSyncProfile.fileSizeBytes = watcherStats.fileSizeBytes;
                        pendingLiveSyncProfile.fileReadyWaitMs = watcherStats.fileReadyWaitMs;
                        pendingLiveSyncProfile.diskReadMs = watcherStats.diskReadMs;
                        auto tConsumed = std::chrono::high_resolution_clock::now();
                        pendingLiveSyncProfile.queueTransferLatencyMs = std::chrono::duration<double, std::milli>(tConsumed - watcherStats.pushTime).count();
                        pendingLiveSyncProfile.watcherDetectionLatencyMs = std::max(0.0, std::chrono::duration<double, std::milli>(watcherStats.pushTime - watcherStats.detectTime).count() - watcherStats.fileReadyWaitMs - watcherStats.diskReadMs);

                        pendingLiveSyncProfile.nbVerts = incomingMesh->nbVerts;
                        pendingLiveSyncProfile.nbFaces = incomingMesh->nbFaces;
                        pendingLiveSyncProfile.nbQuads = info.nbQuads;
                        pendingLiveSyncProfile.nbTriangles = info.nbTriangles;
                        pendingLiveSyncProfile.hasPaint = info.hasPaint && ZBrushBridge::Config::isImportPolyPaintEnabled();
                        pendingLiveSyncProfile.nbPaintColors = pendingLiveSyncProfile.hasPaint ? incomingMesh->nbVerts : 0;
                        pendingLiveSyncProfile.hasGroups = info.hasGroups && ZBrushBridge::Config::isImportPolyGroupsEnabled();
                        pendingLiveSyncProfile.nbGroups = pendingLiveSyncProfile.hasGroups ? incomingMesh->nbFaces : 0;
                        pendingLiveSyncProfile.hasMask = info.hasMask;

                        pendingLiveSyncProfile.parseHeaderMs = parserStats.headerScanMs;
                        pendingLiveSyncProfile.parseVertsMs = parserStats.vertsExtractionMs;
                        pendingLiveSyncProfile.parseFacesMs = parserStats.facesExtractionMs;
                        pendingLiveSyncProfile.parsePaintMs = parserStats.paintExtractionMs;
                        pendingLiveSyncProfile.parseGroupsMs = parserStats.groupsExtractionMs;
                        pendingLiveSyncProfile.parseMaskMs = parserStats.maskExtractionMs;
                        pendingLiveSyncProfile.parseFacesConversionMs = parserStats.facesConversionMs;

                        pendingLiveSyncProfile.meshAllocMoveMs = parserStats.meshAllocMoveMs;
                        pendingLiveSyncProfile.normalsComputationMs = incomingMesh->lastPostInitStats.msFaceNormals + incomingMesh->lastPostInitStats.msVertexNormals;
                        pendingLiveSyncProfile.bboxAndCleanupMs = incomingMesh->lastPostInitStats.msBbox;
                        pendingLiveSyncProfile.topologyBVHCheckMs = incomingMesh->lastPostInitStats.msTopology + incomingMesh->lastPostInitStats.msBVH;
                        pendingLiveSyncProfile.sceneReplaceMs = msScene;

                        tLiveSyncPipelineStart = watcherStats.detectTime.time_since_epoch().count() > 0 ? watcherStats.detectTime : tConsumed;
                        pendingLiveSyncActive = true;
                        gui.setLastLiveSyncProfile(pendingLiveSyncProfile);

                        scene.setModified(true);
                        hadActivity = true;
                        settleFramesRemaining = 2;
                        lastActivityTime = std::chrono::high_resolution_clock::now();
                        double msUpdate = std::chrono::duration<double, std::milli>(
                            std::chrono::high_resolution_clock::now() - tGoZStart).count();
                        sculpt_log("[ZBrush Bridge] Live mesh synced in %.2f ms (Verts: %d, Faces: %d, Quads: %d)\n",
                                   msUpdate, incomingMesh->nbVerts, incomingMesh->nbFaces, incomingMesh->getNbQuads());
                    }
                } catch (const std::exception& e) {
                    gozBuffer.clear();
                    gozBuffer.shrink_to_fit();
                    pendingLiveSyncActive = false;
                    ErrorReporter::instance().reportError("ZBrush Live Sync Failed", "Failed to import GoZ mesh update.", e.what(), true);
                } catch (...) {
                    gozBuffer.clear();
                    gozBuffer.shrink_to_fit();
                    pendingLiveSyncActive = false;
                    ErrorReporter::instance().reportError("ZBrush Live Sync Failed", "Unknown error importing GoZ mesh update.", "", true);
                }
            }
        }

        // Update cameras and controllers
        scene.getCamera().update(deltaTime);
        sculpt.getCameraController().update(deltaTime);
        if (scene.getSplitMode() != Scene::SplitMode::OFF) {
            auto camRight = scene.getCameraRight();
            if (camRight) {
                camRight->update(deltaTime);
            }
        }
        auto tPollEventsStart = std::chrono::high_resolution_clock::now();
        int sculptMotionEvents = 0;
        bool wasSculptingInFrame = sculpt.isSculpting();
        auto processSdlEvent = [&](const SDL_Event& rawEvent) {
            SDL_Event eventCopyLocal = rawEvent;
            // Track user/window activity for idle throttle & event-driven sleep
            bool isWakeActivity = false;
            if (eventCopyLocal.type == SDL_MOUSEMOTION) {
                if (eventCopyLocal.motion.x != lastMotionX || eventCopyLocal.motion.y != lastMotionY) {
                    lastMotionX = eventCopyLocal.motion.x;
                    lastMotionY = eventCopyLocal.motion.y;
                    isWakeActivity = true;
                }
            } else if (eventCopyLocal.type == SDL_MOUSEBUTTONDOWN ||
                       eventCopyLocal.type == SDL_MOUSEBUTTONUP ||
                       eventCopyLocal.type == SDL_MOUSEWHEEL ||
                       eventCopyLocal.type == SDL_KEYDOWN ||
                       eventCopyLocal.type == SDL_KEYUP ||
                       eventCopyLocal.type == SDL_TEXTINPUT ||
                       eventCopyLocal.type == SDL_TEXTEDITING ||
                       eventCopyLocal.type == SDL_FINGERDOWN ||
                       eventCopyLocal.type == SDL_FINGERUP ||
                       eventCopyLocal.type == SDL_FINGERMOTION ||
                       eventCopyLocal.type == SDL_MULTIGESTURE ||
                       eventCopyLocal.type == SDL_DROPFILE ||
                       (eventCopyLocal.type == SDL_USEREVENT &&
                        (eventCopyLocal.user.code == 0x676F7A ||
                         eventCopyLocal.user.code == 0x676F73 ||
                         eventCopyLocal.user.code == 0x676F63))) {
                isWakeActivity = true;
            } else if (eventCopyLocal.type == SDL_WINDOWEVENT) {
                if (eventCopyLocal.window.event == SDL_WINDOWEVENT_RESIZED ||
                    eventCopyLocal.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    eventCopyLocal.window.event == SDL_WINDOWEVENT_EXPOSED ||
                    eventCopyLocal.window.event == SDL_WINDOWEVENT_MOVED ||
                    eventCopyLocal.window.event == SDL_WINDOWEVENT_MAXIMIZED ||
                    eventCopyLocal.window.event == SDL_WINDOWEVENT_RESTORED ||
                    eventCopyLocal.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
                    eventCopyLocal.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                    eventCopyLocal.window.event == SDL_WINDOWEVENT_ENTER ||
                    eventCopyLocal.window.event == SDL_WINDOWEVENT_LEAVE ||
                    eventCopyLocal.window.event == SDL_WINDOWEVENT_CLOSE) {
                    isWakeActivity = true;
                }
            }
            if (isWakeActivity) {
                hadActivity = true;
                settleFramesRemaining = 2;
                lastActivityTime = std::chrono::high_resolution_clock::now();
            }

            if (eventCopyLocal.type == SDL_QUIT || (eventCopyLocal.type == SDL_WINDOWEVENT && eventCopyLocal.window.event == SDL_WINDOWEVENT_CLOSE)) {
                if (scene.isModified()) {
                    gui.requestExit();
                } else {
                    quit = true;
                }
            } else if (eventCopyLocal.type == SDL_WINDOWEVENT) {
                if (eventCopyLocal.window.event == SDL_WINDOWEVENT_RESIZED) {
                     width = eventCopyLocal.window.data1;
                     height = eventCopyLocal.window.data2;
                     
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
                } else if (eventCopyLocal.window.event == SDL_WINDOWEVENT_MOVED) {
                     int wx = eventCopyLocal.window.data1;
                     int wy = eventCopyLocal.window.data2;
                     Uint32 flags = SDL_GetWindowFlags(window);
                     bool isMax = (flags & SDL_WINDOW_MAXIMIZED) != 0;
                     int ww, wh;
                     SDL_GetWindowSize(window, &ww, &wh);
                     gui.updateWindowBounds(wx, wy, ww, wh, isMax);
                } else if (eventCopyLocal.window.event == SDL_WINDOWEVENT_MAXIMIZED) {
                     int wx, wy, ww, wh;
                     SDL_GetWindowPosition(window, &wx, &wy);
                     SDL_GetWindowSize(window, &ww, &wh);
                     gui.updateWindowBounds(wx, wy, ww, wh, true);
                } else if (eventCopyLocal.window.event == SDL_WINDOWEVENT_RESTORED) {
                     int wx, wy, ww, wh;
                     SDL_GetWindowPosition(window, &wx, &wy);
                     SDL_GetWindowSize(window, &ww, &wh);
                     gui.updateWindowBounds(wx, wy, ww, wh, false);
                }
            } else if (eventCopyLocal.type == SDL_DROPFILE) {
                char* droppedFile = eventCopyLocal.drop.file;
                if (droppedFile) {
                    std::string droppedPath(droppedFile);
                    SDL_free(droppedFile);

                    hadActivity = true;
                    settleFramesRemaining = 2;
                    lastActivityTime = std::chrono::high_resolution_clock::now();

                    HistoryState dropRollback = scene.saveCurrentState(false);
                    try {
                        CrashReporter::ScopedStage dropStage("FileIO::DropFile");
                        CrashReporter::recordBreadcrumb("FILE", "DropFile %s", droppedPath.c_str());

                        if (isImageFile(droppedPath)) {
                            scene.addReferenceImage(droppedPath);
                            scene.getCamera().setRefDragEnabled(true);
                            if (scene.getCameraRight()) {
                                scene.getCameraRight()->setRefDragEnabled(true);
                            }
                            if (!scene.getReferenceImages().empty()) {
                                gui.setSelectedRefImageIndex(static_cast<int>(scene.getReferenceImages().size()) - 1);
                            }
                            std::cout << "[DragNDrop] Loaded reference image: " << droppedPath << std::endl;
                        } else if (FileManager::getExtension(droppedPath) == Format::PROJECT_EXT || FileManager::getExtension(droppedPath) == Format::LEGACY_EXT) {
                            gui.openSceneFromPath(droppedPath, scene, &sculpt, &renderer);
                            std::cout << "[DragNDrop] Opened project file: " << droppedPath << std::endl;
                        } else if (is3DModelFile(droppedPath)) {
                            auto tStart = std::chrono::high_resolution_clock::now();
                            auto newMeshes = FileManager::importFiles(droppedPath, &scene, &renderer, &sculpt);
                            auto tImportEnd = std::chrono::high_resolution_clock::now();
                            if (newMeshes.empty()) {
                                ErrorReporter::instance().reportError("Drag & Drop Import", "Failed to load geometry from '" + droppedPath + "'.", "", true);
                            } else {
                                for (auto* mesh : newMeshes) {
                                    scene.addMesh(mesh);
                                }
                                auto tHistStart = std::chrono::high_resolution_clock::now();
                                scene.selectMesh(newMeshes.front());
                                scene.getCamera().resetViewToMeshes(newMeshes, false);
                                if (scene.getCameraRight()) {
                                    scene.getCameraRight()->resetViewToMeshes(newMeshes, false);
                                }
                                scene.pushHistoryState();
                                auto tHistEnd = std::chrono::high_resolution_clock::now();
                                scene.setModified(true);

                                double msImport = std::chrono::duration<double, std::milli>(tImportEnd - tStart).count();
                                double msHist = std::chrono::duration<double, std::milli>(tHistEnd - tHistStart).count();
                                double msTotal = std::chrono::duration<double, std::milli>(tHistEnd - tStart).count();
                                sculpt_log("[DragNDrop] 3D model import '%s' completed in %.2f ms (ImportFiles: %.2fms, HistoryPush: %.2fms)\n",
                                          droppedPath.c_str(), msTotal, msImport, msHist);
                                std::cout << "[DragNDrop] Imported 3D model file: " << droppedPath << std::endl;
                            }
                        } else {
                            sculpt_log("[DragNDrop] Warning: Unsupported file format dropped: '%s'\n", droppedPath.c_str());
                            ErrorReporter::instance().reportWarning("Unsupported File", "Unsupported file format dropped: " + droppedPath, "", false);
                        }
                    } catch (const std::exception& e) {
                        scene.restoreState(dropRollback);
                        ErrorReporter::instance().reportError("Drag & Drop Failed", "Exception loading dropped file '" + droppedPath + "'.", e.what(), true);
                    } catch (...) {
                        scene.restoreState(dropRollback);
                        ErrorReporter::instance().reportError("Drag & Drop Failed", "Unknown error loading dropped file '" + droppedPath + "'.", "", true);
                    }
                }
            } else {
                // If split viewport is active, determine active viewport based on the event mouse coordinates
                if (scene.getSplitMode() != Scene::SplitMode::OFF) {
                    if (!sculpt.isSculpting() && !sculpt.getCameraController().isDragging()) {
                        int mx = -1;
                        if (eventCopyLocal.type == SDL_MOUSEBUTTONDOWN || eventCopyLocal.type == SDL_MOUSEBUTTONUP) {
                            mx = eventCopyLocal.button.x;
                        } else if (eventCopyLocal.type == SDL_MOUSEMOTION) {
                            mx = eventCopyLocal.motion.x;
                        }
                        if (mx >= 0) {
                            int halfW = width / 2;
                            int activeVp = (mx >= halfW) ? 1 : 0;
                            scene.setActiveViewport(activeVp);
                        }
                    }
                }

                // If active viewport is 1 (right viewport), translate coordinates to local space for sculpt events
                SDL_Event eventCopy = eventCopyLocal;
                if (scene.getSplitMode() != Scene::SplitMode::OFF && scene.getActiveViewport() == 1) {
                    int halfW = width / 2;
                    if (eventCopy.type == SDL_MOUSEBUTTONDOWN || eventCopy.type == SDL_MOUSEBUTTONUP) {
                        eventCopy.button.x -= halfW;
                    } else if (eventCopy.type == SDL_MOUSEMOTION) {
                        eventCopy.motion.x -= halfW;
                    }
                }

                ImGui_ImplSDL2_ProcessEvent(&eventCopyLocal);

                if (gui.isCapturingHotkey()) {
                    if (gui.handleCapturedKeyEvent(eventCopy)) {
                        return;
                    }
                }

                try {
                    CrashReporter::ScopedStage eventStage("MainLoop::Events");
                    bool handledByHotkey = dispatcher.processEvent(eventCopy, sculpt, scene, gui, &renderer);

                    if (!handledByHotkey) {
                        ImGuiIO& io = ImGui::GetIO();
                        bool skipSculpt = false;
                        
                        bool isPrimBusy = (sculpt.getPrimitiveTool() && 
                                          (sculpt.getPrimitiveTool()->isInteracting() || sculpt.getPrimitiveTool()->isHoveringHandle()));

                        // Prioritize ImGuizmo over camera orbit when hovering/interacting with the gizmo
                        if (!isPrimBusy && (sculpt.getBrush() == BRUSH_TRANSFORM || sculpt.getBrush() == BRUSH_POSE) && (ImGuizmo::IsOver() || ImGuizmo::IsUsing())) {
                            if (!sculpt.getCameraController().isDragging()) {
                                bool isCameraEvent = sculpt.getCameraController().isNavigationEvent(eventCopy, scene.getCamera().getRef2DMode());
                                bool isAltClickSettingPivot = (eventCopy.type == SDL_MOUSEBUTTONDOWN && eventCopy.button.button == SDL_BUTTON_LEFT &&
                                    (hotkeyManager.getModifierBehavior().ctrlAltSubToolSelect
                                        ? (((SDL_GetModState() & KMOD_ALT) != 0) && ((SDL_GetModState() & KMOD_CTRL) != 0))
                                        : (((SDL_GetModState() & KMOD_ALT) != 0) && ((SDL_GetModState() & KMOD_CTRL) == 0)))
                                    && !ImGuizmo::IsUsing());
                                if (!isCameraEvent && !isAltClickSettingPivot) {
                                    skipSculpt = true;
                                }
                            }
                        }

                        if (!skipSculpt && io.WantCaptureMouse && (eventCopy.type == SDL_MOUSEBUTTONDOWN || eventCopy.type == SDL_MOUSEBUTTONUP || eventCopy.type == SDL_MOUSEMOTION || eventCopy.type == SDL_MOUSEWHEEL)) {
                            // Never skip mouse events if the camera controller is actively dragging/navigating,
                            // otherwise mouse up or mouse motion events will be swallowed by ImGui and lock navigation state.
                            if (!sculpt.getCameraController().isDragging()) {
                                bool isCameraEvent = sculpt.getCameraController().isNavigationEvent(eventCopy, scene.getCamera().getRef2DMode());
                                
                                bool isAltClickSettingPivot = ((sculpt.getBrush() == BRUSH_TRANSFORM || sculpt.getBrush() == BRUSH_POSE) && eventCopy.type == SDL_MOUSEBUTTONDOWN && eventCopy.button.button == SDL_BUTTON_LEFT &&
                                    (hotkeyManager.getModifierBehavior().ctrlAltSubToolSelect
                                        ? (((SDL_GetModState() & KMOD_ALT) != 0) && ((SDL_GetModState() & KMOD_CTRL) != 0))
                                        : (((SDL_GetModState() & KMOD_ALT) != 0) && ((SDL_GetModState() & KMOD_CTRL) == 0)))
                                    && !ImGuizmo::IsUsing());
                                if (isCameraEvent) {
                                    ImVec2 mpos(io.MousePos.x, io.MousePos.y);
                                    if (eventCopy.type == SDL_MOUSEWHEEL && gui.isPointOverWindow(mpos)) {
                                        skipSculpt = true;
                                    }
                                } else if (!isAltClickSettingPivot) {
                                    skipSculpt = true;
                                }
                            }
                        }
                        if (io.WantCaptureKeyboard && (eventCopy.type == SDL_KEYDOWN || eventCopy.type == SDL_KEYUP)) {
                            skipSculpt = true;
                        }
                        if (gui.getTimelapsePlayer().isOpen()) {
                            skipSculpt = true;
                            bool isCameraEvent = sculpt.getCameraController().isNavigationEvent(eventCopy, scene.getCamera().getRef2DMode());
                            bool isMotionAndDragging = (eventCopy.type == SDL_MOUSEMOTION || eventCopy.type == SDL_MOUSEBUTTONUP) &&
                                                       sculpt.getCameraController().isDragging();

                            if ((!io.WantCaptureMouse && isCameraEvent) || isMotionAndDragging) {
                                sculpt.getCameraController().handleEvent(eventCopy, scene.getCamera(), scene.getMeshes(), true, &scene, &renderer);
                            }
                        }

                        if (!skipSculpt) {
                            if (eventCopy.type == SDL_MOUSEMOTION && sculpt.isSculpting()) {
                                sculptMotionEvents++;
                            }
                            sculpt.handleEvent(eventCopy, scene);
                            if (sculpt.isSculpting()) {
                                wasSculptingInFrame = true;
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    g_undoManager.rollbackActiveSculptStroke(scene);
                    sculpt.cancelStroke();
                    ErrorReporter::instance().reportError("Input / Tool Error", "Recovered from an error during input handling.", e.what(), true);
                } catch (...) {
                    g_undoManager.rollbackActiveSculptStroke(scene);
                    sculpt.cancelStroke();
                    ErrorReporter::instance().reportError("Input / Tool Error", "Recovered from an unknown error during input handling.", "", true);
                }
            }
        };
        while (SDL_PollEvent(&event)) {
            processSdlEvent(event);
        }
        double pollEventsMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tPollEventsStart).count();

        // Manage hardware / system / software cursor
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

        auto tProcessFrameStart = std::chrono::high_resolution_clock::now();
        if (gui.isRemeshRunning()) {
            sculpt.getCursor().hide();
        } else {
            try {
                CrashReporter::ScopedStage sculptFrameStage("MainLoop::SculptProcessFrame");
                sculpt.setModalMode(dispatcher.getModalMode());
                sculpt.processFrame(scene);
            } catch (const std::exception& e) {
                g_undoManager.rollbackActiveSculptStroke(scene);
                sculpt.cancelStroke();
                ErrorReporter::instance().reportError("Sculpt Stroke Error", "Recovered from a sculpt error; stroke rolled back.", e.what(), true);
            } catch (...) {
                g_undoManager.rollbackActiveSculptStroke(scene);
                sculpt.cancelStroke();
                ErrorReporter::instance().reportError("Sculpt Stroke Error", "Recovered from an unknown sculpt error; stroke rolled back.", "", true);
            }
        }
        double processFrameMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tProcessFrameStart).count();

        // Manage hardware / system / software cursor (activated ONLY over the mesh or during sculpt, or when hovering brush sliders)
        bool sculptingNow = sculpt.isSculpting();
        ImGuiIO& io = ImGui::GetIO();
        Uint32 winFlags = SDL_GetWindowFlags(window);
        bool appHasFocus = (winFlags & SDL_WINDOW_INPUT_FOCUS) != 0;
        const auto& cursorState = sculpt.getCursor().getState();
        bool isOverSliders = gui.isOverBrushSliders();
        bool isMouseOverUi = io.WantCaptureMouse && !isOverSliders;
        bool isOverMesh = cursorState.hitMesh || isOverSliders;

        hardwareCursor.updateScale(gui.getUiScale());
        hardwareCursor.setMode(static_cast<HardwareCursorMode>(renderer.getHardwareCursorMode()));
        hardwareCursor.update(sculptingNow, isOverMesh, isMouseOverUi, appHasFocus, cursorState.color);
        renderer.setHardwareCursorActive(hardwareCursor.isHardwareCursorActive());

        sculpt.getCursor().applyToRenderer(renderer);
        renderer.setLassoParameters(sculpt.isLassoActive(), sculpt.getLassoPoints(), sculpt.getLassoAlt(), sculpt.isMaskLasso());
        renderer.setClipCurveOverlay(sculpt.isClipCurveActive(), sculpt.getClipCurvePoints(), sculpt.getClipCurveAlt());
        renderer.setTrimLassoOverlay(sculpt.isTrimLassoActive(), sculpt.getTrimLassoPoints(), sculpt.getTrimLassoAlt());
        renderer.setSplitLassoOverlay(sculpt.isSplitLassoActive(), sculpt.getSplitLassoPoints(), sculpt.getSplitLassoAlt(), sculpt.getSettings(BRUSH_SPLIT).splitMode);
        renderer.setActiveBrush(sculpt.getBrush());

        static std::vector<SymmetryPlaneData> s_lastSymmetryPlanes;

        if (sculpt.consumeSymmetryLineTrigger()) {
            renderer.triggerTempSymmetryLine(1.8f);
        }

        if (renderer.isSymmetryLineVisible()) {
            Mesh* activeMesh = scene.getSelected();
            bool permanentShow = sculpt.getUseSym() && renderer.getShowSymmetryLine();
            if (activeMesh && sculpt.getUseSym()) {
                std::vector<SymmetryPlaneData> planes;
                SymmetryMode mode = sculpt.getSymmetryMode();
                if (sculpt.getSymX()) {
                    planes.push_back({
                        activeMesh->getSymmetryOriginForAxis(0, mode),
                        activeMesh->getSymmetryNormalForAxis(0, mode),
                        glm::vec3(1.0f, 0.25f, 0.25f)
                    });
                }
                if (sculpt.getSymY()) {
                    planes.push_back({
                        activeMesh->getSymmetryOriginForAxis(1, mode),
                        activeMesh->getSymmetryNormalForAxis(1, mode),
                        glm::vec3(0.25f, 0.95f, 0.3f)
                    });
                }
                if (sculpt.getSymZ()) {
                    planes.push_back({
                        activeMesh->getSymmetryOriginForAxis(2, mode),
                        activeMesh->getSymmetryNormalForAxis(2, mode),
                        glm::vec3(0.25f, 0.6f, 1.0f)
                    });
                }
                if (!planes.empty()) {
                    s_lastSymmetryPlanes = planes;
                }
                renderer.setSymmetryPlanes(permanentShow, planes);
            } else if (!sculpt.getUseSym() && renderer.isTempSymmetryLineActive()) {
                const auto& currentPlanes = renderer.getSymmetryPlanes();
                if (!currentPlanes.empty()) {
                    s_lastSymmetryPlanes = currentPlanes;
                    renderer.setSymmetryPlanes(false, currentPlanes);
                } else if (!s_lastSymmetryPlanes.empty()) {
                    renderer.setSymmetryPlanes(false, s_lastSymmetryPlanes);
                } else if (activeMesh) {
                    std::vector<SymmetryPlaneData> planes;
                    SymmetryMode mode = sculpt.getSymmetryMode();
                    if (sculpt.getSymX() || (!sculpt.getSymY() && !sculpt.getSymZ())) {
                        planes.push_back({
                            activeMesh->getSymmetryOriginForAxis(0, mode),
                            activeMesh->getSymmetryNormalForAxis(0, mode),
                            glm::vec3(1.0f, 0.25f, 0.25f)
                        });
                    }
                    if (sculpt.getSymY()) {
                        planes.push_back({
                            activeMesh->getSymmetryOriginForAxis(1, mode),
                            activeMesh->getSymmetryNormalForAxis(1, mode),
                            glm::vec3(0.25f, 0.95f, 0.3f)
                        });
                    }
                    if (sculpt.getSymZ()) {
                        planes.push_back({
                            activeMesh->getSymmetryOriginForAxis(2, mode),
                            activeMesh->getSymmetryNormalForAxis(2, mode),
                            glm::vec3(0.25f, 0.6f, 1.0f)
                        });
                    }
                    s_lastSymmetryPlanes = planes;
                    renderer.setSymmetryPlanes(false, planes);
                } else {
                    renderer.setSymmetryPlanes(false, {});
                }
            } else {
                renderer.setSymmetryPlanes(permanentShow, {});
            }
        } else {
            bool permanentShow = sculpt.getUseSym() && renderer.getShowSymmetryLine();
            renderer.setSymmetryPlanes(permanentShow, {});
        }
        bool isSculptingActive = sculpt.isSculpting();
        if (isSculptingActive && renderer.getTaaResetOnStroke()) {
            renderer.resetTaaHistory();
        }
        bool orbitActive = (sculpt.getCameraController().isDragging() &&
                            sculpt.getCameraController().getDragMode() != CameraController::DragMode::RotateLight) ||
                           scene.getCamera().isAnimating() ||
                           (scene.getCameraRight() && scene.getCameraRight()->isAnimating());
        renderer.setOrbitInteractionHint(orbitActive, isSculptingActive);
        renderer.resetLastUploadTiming();
        auto tRenderStart = std::chrono::high_resolution_clock::now();
        try {
            CrashReporter::ScopedStage renderStage("MainLoop::Render");
            renderer.render(scene);
            consecutiveRenderErrors = 0;
        } catch (const std::exception& e) {
            ++consecutiveRenderErrors;
            ErrorReporter::instance().reportError("Renderer Error", "An error occurred while rendering the scene.", e.what(), true);
            if (consecutiveRenderErrors == 3) {
                CrashReporter::writeDiagnosticSnapshot("Repeated Renderer Exception", e.what(), false, false);
            }
        } catch (...) {
            ++consecutiveRenderErrors;
            ErrorReporter::instance().reportError("Renderer Error", "Unknown error occurred while rendering the scene.", "", true);
            if (consecutiveRenderErrors == 3) {
                CrashReporter::writeDiagnosticSnapshot("Repeated Renderer Exception", "Unknown exception during renderer.render()", false, false);
            }
        }
        double renderMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tRenderStart).count();

        winFlags = SDL_GetWindowFlags(window);
        appHasFocus = (winFlags & SDL_WINDOW_INPUT_FOCUS) != 0;
        bool activeActivity = hadActivity || isSculptingActive || sculpt.getCameraController().isDragging();
        gui.getWorkTimer().tick(deltaTime, appHasFocus, activeActivity);
        lastWorkTimerSecond = gui.getWorkTimer().getSessionSeconds();

        auto tGuiStart = std::chrono::high_resolution_clock::now();
        try {
            CrashReporter::ScopedStage guiStage("MainLoop::GUI");
            gui.render(sculpt, scene, renderer, window);
        } catch (const std::exception& e) {
            if (ImGuiContext* ctx = ImGui::GetCurrentContext(); ctx && ctx->WithinFrameScope) {
                ImGui::EndFrame();
            }
            ErrorReporter::instance().reportError("UI Error", "Recovered from an error while rendering the UI.", e.what(), true);
        } catch (...) {
            if (ImGuiContext* ctx = ImGui::GetCurrentContext(); ctx && ctx->WithinFrameScope) {
                ImGui::EndFrame();
            }
            ErrorReporter::instance().reportError("UI Error", "Recovered from an unknown error while rendering the UI.", "", true);
        }
        double guiMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tGuiStart).count();
        
        if (gui.isPendingQuit() && !scene.isModified() && !gui.isUnsavedModalOpen()) {
            quit = true;
        }
        
        bool isRemeshRunning = gui.isRemeshRunning();
        if (wasRemeshRunning && !isRemeshRunning) {
            dispatcher.resetModifiers(sculpt, &renderer);
            settleFramesRemaining = std::max(settleFramesRemaining, 2);
        }
        wasRemeshRunning = isRemeshRunning;

        bool isDecimRunning = gui.isDecimationRunning();
        if (wasDecimRunning && !isDecimRunning) {
            settleFramesRemaining = std::max(settleFramesRemaining, 2);
        }
        wasDecimRunning = isDecimRunning;

        // If actively sculpting, mark as active
        if (isSculptingActive || sculpt.getCameraController().isDragging()) {
            hadActivity = true;
            settleFramesRemaining = 2;
            lastActivityTime = std::chrono::high_resolution_clock::now();
        }

        auto tSwapStart = std::chrono::high_resolution_clock::now();
        SDL_GL_SwapWindow(window);
        double swapMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tSwapStart).count();

        if (settleFramesRemaining > 0) {
            --settleFramesRemaining;
        }

        if (pendingLiveSyncActive) {
            auto tFrameDone = std::chrono::high_resolution_clock::now();
            const auto& uploadTiming = renderer.getLastUploadTiming();
            pendingLiveSyncProfile.gpuUploadVboMs = uploadTiming.msVbo;
            pendingLiveSyncProfile.gpuUploadEboMs = uploadTiming.msEbo;
            double totalFrameMs = std::chrono::duration<double, std::milli>(tFrameDone - tRenderStart).count();
            pendingLiveSyncProfile.gpuFirstFrameRenderSwapMs = std::max(0.0, totalFrameMs - uploadTiming.msTotal);
            pendingLiveSyncProfile.totalPipelineMs = std::chrono::duration<double, std::milli>(tFrameDone - tLiveSyncPipelineStart).count();
            pendingLiveSyncProfile.timestamp = GoZProfiler::getCurrentTimestamp();
            pendingLiveSyncProfile.cpuThreads = GoZProfiler::getCpuThreadCount();
            pendingLiveSyncProfile.availableRamMB = GoZProfiler::getAvailableRamMB();
            pendingLiveSyncProfile.peakRssMB = GoZProfiler::getPeakRssMB();

            pendingLiveSyncProfile.saveReportToFile("goz_profile_report.txt", true);
            pendingLiveSyncProfile.printToConsole();
            gui.setLastLiveSyncProfile(pendingLiveSyncProfile);
            pendingLiveSyncActive = false;
        }

        if ((isSculptingActive || wasSculptingInFrame) && (pollEventsMs + processFrameMs + renderMs + guiMs + swapMs > 5.0)) {
            const auto& uploadTiming = renderer.getLastUploadTiming();
            sculpt_log("[SCULPT RENDER FRAME] PollEvents: %.2fms (%d motion) | ProcessFrame: %.2fms | Render: %.2fms (VBO Upload: %.2fms) | GUI: %.2fms | Swap: %.2fms\n",
                       pollEventsMs, sculptMotionEvents, processFrameMs, renderMs, uploadTiming.msVbo, guiMs, swapMs);
        }

        {
            const bool autosaveBusy = sculpt.isSculpting() ||
                                      g_undoManager.hasActiveSculptStroke() ||
                                      gui.isRemeshRunning() ||
                                      gui.isDecimationRunning();
            autosaveManager.tick(deltaTime, scene, renderer, sculpt,
                                 gui.getWorkTimer().getTotalSeconds(),
                                 gui.getCurrentScenePath(), autosaveBusy);
        }

        // ---- GPU load limiter & Event-Driven Idle Sleep (Stage 1) ----
        {
            auto frameEnd = std::chrono::high_resolution_clock::now();
            float frameMs = std::chrono::duration<float, std::milli>(frameEnd - currentFrameTime).count();

            float idleMs = std::chrono::duration<float, std::milli>(
                               frameEnd - lastActivityTime).count();

            const bool canDeepSleep = OptFlags::get().idleSleep &&
                                      !quit &&
                                      !hadActivity &&
                                      settleFramesRemaining <= 0 &&
                                      idleMs > 500.0f &&
                                      !sculpt.isSculpting() &&
                                      !sculpt.getCameraController().isDragging() &&
                                      !scene.getCamera().isAnimating() &&
                                      !(scene.getCameraRight() && scene.getCameraRight()->isAnimating()) &&
                                      !gui.isRemeshRunning() &&
                                      !gui.isDecimationRunning() &&
                                      !gui.isTimelapsePlaying() &&
                                      !gui.hasActiveToasts() &&
                                      !gui.hasPendingRecentThumbTasks() &&
                                      !gui.isCapturingHotkey() &&
                                      !gui.hasPendingGizmoClick() &&
                                      !ImGui::GetIO().WantTextInput &&
                                      !renderer.isSelectionFlashActive() &&
                                      !renderer.isSymmetryLineAnimating() &&
                                      !renderer.isOrbitScaleSettling() &&
                                      !(renderer.getRenderMode() == RenderMode::SSPT && renderer.getAccumFrameCount() < 128) &&
                                      !(zbrushWatcher && (zbrushWatcher->hasPendingScene() || zbrushWatcher->hasPendingMesh()));

            if (canDeepSleep) {
                bool wokeForRender = false;
                while (!quit) {
                    if (zbrushWatcher && (zbrushWatcher->hasPendingScene() || zbrushWatcher->hasPendingMesh())) {
                        hadActivity = true;
                        settleFramesRemaining = std::max(settleFramesRemaining, 2);
                        lastActivityTime = std::chrono::high_resolution_clock::now();
                        wokeForRender = true;
                        break;
                    }

                    const int waitTimeoutMs = gui.getWorkTimer().isRunning() ? 250 : 500;
                    SDL_Event waitEvent;
                    auto sleepStart = std::chrono::high_resolution_clock::now();
                    int gotEvent = SDL_WaitEventTimeout(&waitEvent, waitTimeoutMs);
                    auto sleepNow = std::chrono::high_resolution_clock::now();
                    float sleepDt = std::chrono::duration<float>(sleepNow - sleepStart).count();

                    Uint32 sleepWinFlags = SDL_GetWindowFlags(window);
                    bool sleepHasFocus = (sleepWinFlags & SDL_WINDOW_INPUT_FOCUS) != 0;
                    gui.getWorkTimer().tick(sleepDt, sleepHasFocus, false);
                    if (autosaveManager.tick(sleepDt, scene, renderer, sculpt,
                                             gui.getWorkTimer().getTotalSeconds(),
                                             gui.getCurrentScenePath(), false)) {
                        wokeForRender = true;
                        break;
                    }

                    if (gotEvent == 1) {
                        processSdlEvent(waitEvent);
                        while (SDL_PollEvent(&event)) {
                            processSdlEvent(event);
                        }
                        if (gui.isPendingQuit() && !scene.isModified() && !gui.isUnsavedModalOpen()) {
                            quit = true;
                            break;
                        }
                        if (hadActivity || quit) {
                            wokeForRender = !quit;
                            break;
                        }
                    }

                    uint64_t curSec = gui.getWorkTimer().getSessionSeconds();
                    if (curSec != lastWorkTimerSecond) {
                        lastWorkTimerSecond = curSec;
                        settleFramesRemaining = std::max(settleFramesRemaining, 1);
                        wokeForRender = true;
                        break;
                    }

                    if (zbrushWatcher && (zbrushWatcher->hasPendingScene() || zbrushWatcher->hasPendingMesh())) {
                        hadActivity = true;
                        settleFramesRemaining = std::max(settleFramesRemaining, 2);
                        lastActivityTime = std::chrono::high_resolution_clock::now();
                        wokeForRender = true;
                        break;
                    }

                    if (gui.isRemeshRunning() || gui.isDecimationRunning() || gui.isTimelapsePlaying() ||
                        gui.hasActiveToasts() || gui.hasPendingRecentThumbTasks()) {
                        wokeForRender = true;
                        break;
                    }
                }
                if (wokeForRender && !quit) {
                    pendingWakeActivity = hadActivity;
                    lastFrameTime = std::chrono::high_resolution_clock::now() - std::chrono::milliseconds(16);
                    continue;
                }
                if (quit) {
                    break;
                }
            }

            bool fpsLimitOn  = gui.getFpsLimitEnabled();
            int  targetFps   = gui.getFpsLimit();          // e.g. 60
            float targetMs   = 1000.0f / (float)targetFps;

            // Idle throttle: >500 ms since last activity → drop to 15 FPS
            bool isIdle = (idleMs > 500.0f) && !gui.isRemeshRunning();
            if (isIdle) {
                targetMs = 1000.0f / 15.0f; // 15 FPS while idle
                fpsLimitOn = true;
            }

            if (fpsLimitOn && frameMs < targetMs) {
                Uint32 sleepMs = static_cast<Uint32>(targetMs - frameMs);
                if (sleepMs > 0) {
                    Uint32 elapsed = 0;
                    while (elapsed < sleepMs) {
                        Uint32 step = std::min<Uint32>(5, sleepMs - elapsed);
                        SDL_Delay(step);
                        elapsed += step;
                        if (zbrushWatcher && (zbrushWatcher->hasPendingMesh() || zbrushWatcher->hasPendingScene())) {
                            hadActivity = true;
                            settleFramesRemaining = std::max(settleFramesRemaining, 2);
                            lastActivityTime = std::chrono::high_resolution_clock::now();
                            break;
                        }
                    }
                }
            }
        }
    }

    std::cout << "Cleaning up..." << std::endl;

    if (zbrushWatcher) {
        zbrushWatcher->stop();
        zbrushWatcher.reset();
    }

    hardwareCursor.shutdown();

    // Save application settings on exit
    IniFile saveApp;
    RenderSettings::save(saveApp, renderer, scene);
    sculpt.saveSettings(saveApp);
    gui.saveSettings(saveApp);
    hotkeyManager.saveActivePresetName(saveApp);
    hotkeyManager.saveAll();
    recentFiles.saveToIni(saveApp);
    saveApp.save("app_settings.cfg");
    autosaveManager.shutdown();

#ifdef _WIN32
    g_tablet.wintabClose();
#else
    g_tablet.stopPolling();
#endif

    gui.shutdown();

    // 1. Detach undo manager while sculpt and scene are still alive
    g_undoManager.clear();
    g_undoManager.setSculptManager(nullptr);

    // 2. Cleanly destroy all meshes and mesh GPU buffers while OpenGL context is still valid
    scene.setSculptManager(nullptr);
    scene.clear();
    scene.setOnMeshDeletedCallback(nullptr);
    sculpt.setRenderer(nullptr);
    gui.setRenderer(nullptr);

    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
#ifdef _WIN32
    ExitProcess(0);
#else
    return 0;
#endif
} catch (const std::exception& e) {
        std::cerr << "\n=============================================" << std::endl;
        std::cerr << "[UNHANDLED EXCEPTION] " << e.what() << std::endl;
        std::cerr << "=============================================" << std::endl;
        sculpt_log_error("[FATAL EXCEPTION] %s", e.what());
        CrashReporter::writeDiagnosticSnapshot("Unhandled C++ Exception in main()", e.what(), true, !isHeadlessRun);
#ifdef _WIN32
        ExitProcess(1);
#else
        return -1;
#endif
    } catch (...) {
        std::cerr << "\n=============================================" << std::endl;
        std::cerr << "[UNHANDLED EXCEPTION] Unknown exception occurred." << std::endl;
        std::cerr << "=============================================" << std::endl;
        sculpt_log_error("[FATAL EXCEPTION] Unknown C++ exception in main()");
        CrashReporter::writeDiagnosticSnapshot("Unknown C++ Exception in main()", nullptr, true, !isHeadlessRun);
#ifdef _WIN32
        ExitProcess(1);
#else
        return -1;
#endif
    }
}
