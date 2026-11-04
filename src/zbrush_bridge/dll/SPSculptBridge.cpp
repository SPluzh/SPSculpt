#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>

// Standard ZScript & C-ABI export macro
#ifdef _WIN32
#define SPSCULPT_EXPORT extern "C" __declspec(dllexport)
#else
#define SPSCULPT_EXPORT extern "C"
#endif

namespace {

constexpr UINT_PTR SPSCULPT_SUBCLASS_ID = 0x5053434C; // 'PSCL'
constexpr UINT_PTR SPSCULPT_TIMER_ID    = 0x5350544D; // 'SPTM'

HWND g_mainHwnd = NULL;
std::vector<HWND> g_subclassedWindows;
bool g_hookInstalled = false;

bool g_isLButtonDown = false;
int g_downX = 0;
int g_downY = 0;
float g_maxDragDist = 0.0f;

int g_debounceDelayMs = 350;
std::string g_dataDir = "";

std::string NormalizeDir(std::string path) {
    if (path.empty()) return path;
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    if (path.back() != '/') {
        path += '/';
    }
    return path;
}

std::string GetDefaultDataDir() {
    char pubDocs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_COMMON_DOCUMENTS, NULL, 0, pubDocs))) {
        std::string s(pubDocs);
        s += "\\SPSculptData\\";
        return NormalizeDir(s);
    }
    return "C:/Users/Public/Documents/SPSculptData/";
}

std::string ResolveDataDir() {
    if (!g_dataDir.empty()) {
        return NormalizeDir(g_dataDir);
    }
    const char* env = getenv("SPSCULPT_DATA_DIR");
    if (env && strlen(env) > 0) {
        return NormalizeDir(env);
    }
    return GetDefaultDataDir();
}

void LogDebug(const std::string& msg) {
    std::string dataDir = ResolveDataDir();
    CreateDirectoryA(dataDir.c_str(), NULL);
    std::ofstream ofs(dataDir + "bridge_debug.log", std::ios::app);
    if (ofs.is_open()) {
        ofs << "[" << GetTickCount() << "] " << msg << "\n";
        ofs.close();
    }
}


void TriggerExportScript(HWND targetHwnd) {
    if (!targetHwnd) {
        targetHwnd = g_mainHwnd;
    }
    if (!targetHwnd) return;

    std::string dataDir = ResolveDataDir();
    CreateDirectoryA(dataDir.c_str(), NULL);

    std::string meshPath = dataDir + "mesh.GoZ";
    std::string camPath = dataDir + "camera.txt";
    std::string cmdPath = dataDir + "cmd_sync.txt";

    // Write command script with ZScript instructions
    std::ofstream ofs(cmdPath);
    if (ofs.is_open()) {
        ofs << "// SPSculpt Auto-Sync Command Script\n";
        ofs << "[If, [IsEnabled, \"ZPlugin:SPSculpt:CheckSync\"],\n";
        ofs << "    [IPress, \"ZPlugin:SPSculpt:CheckSync\"]\n";
        ofs << ",\n";
        ofs << "    [If, [IsEnabled, \"Tool:Export\"],\n";
        ofs << "        [FileNameSetNext, \"" << meshPath << "\"][IPress, \"Tool:Export\"]\n";
        ofs << "    ]\n";
        ofs << "]\n";
        ofs.close();
    }

    // Windows native path format for COPYDATA payload
    std::string winCmdPath = cmdPath;
    for (char& c : winCmdPath) {
        if (c == '/') c = '\\';
    }

    COPYDATASTRUCT cds;
    cds.dwData = 0;
    cds.cbData = static_cast<DWORD>(winCmdPath.length() + 1);
    cds.lpData = const_cast<char*>(winCmdPath.c_str());

    // Send synchronously to ZBrush message loop
    SendMessageA(targetHwnd, WM_COPYDATA, reinterpret_cast<WPARAM>(targetHwnd), reinterpret_cast<LPARAM>(&cds));
}

LRESULT CALLBACK ZBrushSubclassProc(
    HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    HWND timerOwner = g_mainHwnd ? g_mainHwnd : hWnd;

    switch (uMsg) {
    case WM_LBUTTONDOWN: {
        g_isLButtonDown = true;
        g_downX = GET_X_LPARAM(lParam);
        g_downY = GET_Y_LPARAM(lParam);
        g_maxDragDist = 0.0f;
        KillTimer(timerOwner, SPSCULPT_TIMER_ID);
        break;
    }

    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: {
        // Cancel debounce timer if user starts camera navigation with RMB or MMB
        KillTimer(timerOwner, SPSCULPT_TIMER_ID);
        break;
    }

    case WM_MOUSEMOVE: {
        if (g_isLButtonDown) {
            int cx = GET_X_LPARAM(lParam);
            int cy = GET_Y_LPARAM(lParam);
            float dist = std::hypot(static_cast<float>(cx - g_downX), static_cast<float>(cy - g_downY));
            if (dist > g_maxDragDist) {
                g_maxDragDist = dist;
            }
        }
        break;
    }

    case WM_LBUTTONUP: {
        g_isLButtonDown = false;
        // Check if movement indicates a sculpt stroke (>= 5 px)
        // Click on UI buttons typically has < 5 px delta
        if (g_maxDragDist >= 5.0f) {
            SetTimer(timerOwner, SPSCULPT_TIMER_ID, static_cast<UINT>(g_debounceDelayMs), NULL);
        }
        g_maxDragDist = 0.0f;
        break;
    }

    case WM_TIMER: {
        if (wParam == SPSCULPT_TIMER_ID) {
            KillTimer(timerOwner, SPSCULPT_TIMER_ID);
            TriggerExportScript(timerOwner);
            return 0;
        }
        break;
    }

    case WM_NCDESTROY: {
        RemoveWindowSubclass(hWnd, ZBrushSubclassProc, uIdSubclass);
        break;
    }

    default:
        break;
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

BOOL CALLBACK EnumChildWindowsProc(HWND hwnd, LPARAM lParam) {
    SetWindowSubclass(hwnd, ZBrushSubclassProc, SPSCULPT_SUBCLASS_ID, 0);
    g_subclassedWindows.push_back(hwnd);
    return TRUE;
}

HWND FindZBrushMainWindow() {
    DWORD currentPid = GetCurrentProcessId();
    HWND bestHwnd = NULL;

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) {
            char title[256] = {0};
            GetWindowTextA(hwnd, title, sizeof(title));
            LONG style = GetWindowLongA(hwnd, GWL_STYLE);
            if ((style & WS_CHILD) == 0 && strlen(title) > 0) {
                *(HWND*)lParam = hwnd;
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&bestHwnd));

    if (!bestHwnd) bestHwnd = GetActiveWindow();
    if (!bestHwnd) bestHwnd = GetForegroundWindow();

    return bestHwnd;
}

} // anonymous namespace

// ==============================================================================
// Exported C Functions (callable by ZBrush ZScript [FileExecute] and Python 3 ctypes)
// ==============================================================================

SPSCULPT_EXPORT float InstallHook(char* pText, double optValue, char* pOptBuffer1, int* pOptBuffer1Size, char* pOptBuffer2, int* pOptBuffer2Size) {
    LogDebug("InstallHook invoked");
    if (g_hookInstalled) {
        return 1.0f; // Already installed
    }

    g_mainHwnd = FindZBrushMainWindow();
    if (!g_mainHwnd) {
        LogDebug("InstallHook: ZBrush main HWND not found");
        return 0.0f;
    }

    // Subclass the main window
    SetWindowSubclass(g_mainHwnd, ZBrushSubclassProc, SPSCULPT_SUBCLASS_ID, 0);
    g_subclassedWindows.push_back(g_mainHwnd);

    // Subclass child/canvas windows
    EnumChildWindows(g_mainHwnd, EnumChildWindowsProc, 0);

    g_hookInstalled = true;
    LogDebug("InstallHook: success");
    return 1.0f;
}

SPSCULPT_EXPORT float RemoveHook(char* pText, double optValue, char* pOptBuffer1, int* pOptBuffer1Size, char* pOptBuffer2, int* pOptBuffer2Size) {
    LogDebug("RemoveHook invoked");
    if (!g_hookInstalled) {
        return 1.0f;
    }

    for (HWND hwnd : g_subclassedWindows) {
        if (IsWindow(hwnd)) {
            KillTimer(hwnd, SPSCULPT_TIMER_ID);
            RemoveWindowSubclass(hwnd, ZBrushSubclassProc, SPSCULPT_SUBCLASS_ID);
        }
    }
    g_subclassedWindows.clear();
    g_hookInstalled = false;
    LogDebug("RemoveHook: success");
    return 1.0f;
}

SPSCULPT_EXPORT float SetDataDir(char* pText, double optValue, char* pOptBuffer1, int* pOptBuffer1Size, char* pOptBuffer2, int* pOptBuffer2Size) {
    if (pText && strlen(pText) > 0) {
        g_dataDir = pText;
        LogDebug(std::string("SetDataDir: ") + pText);
        return 1.0f;
    }
    return 0.0f;
}

SPSCULPT_EXPORT float SetDebounceDelay(char* pText, double optValue, char* pOptBuffer1, int* pOptBuffer1Size, char* pOptBuffer2, int* pOptBuffer2Size) {
    int delay = static_cast<int>(optValue);
    if (delay >= 50 && delay <= 2000) {
        g_debounceDelayMs = delay;
        LogDebug(std::string("SetDebounceDelay: ") + std::to_string(delay));
        return 1.0f;
    }
    return 0.0f;
}

SPSCULPT_EXPORT float TriggerSync(char* pText, double optValue, char* pOptBuffer1, int* pOptBuffer1Size, char* pOptBuffer2, int* pOptBuffer2Size) {
    LogDebug("TriggerSync invoked");
    HWND target = g_mainHwnd ? g_mainHwnd : FindZBrushMainWindow();
    if (target) {
        TriggerExportScript(target);
        return 1.0f;
    }
    return 0.0f;
}

SPSCULPT_EXPORT float IsHookInstalled(char* pText, double optValue, char* pOptBuffer1, int* pOptBuffer1Size, char* pOptBuffer2, int* pOptBuffer2Size) {
    return g_hookInstalled ? 1.0f : 0.0f;
}

uint64_t g_lastExportTick = 0;

SPSCULPT_EXPORT float CanExportNow(char* pText, double minIntervalMs, char* pOptBuffer1, int* pOptBuffer1Size, char* pOptBuffer2, int* pOptBuffer2Size) {
    uint64_t now = GetTickCount64();
    if (minIntervalMs < 50.0) minIntervalMs = 500.0;
    if (g_lastExportTick == 0 || (now - g_lastExportTick >= static_cast<uint64_t>(minIntervalMs))) {
        g_lastExportTick = now;
        return 1.0f; // Export allowed
    }
    return 0.0f; // Throttled
}

SPSCULPT_EXPORT float MarkExportDone(char* pText, double optValue, char* pOptBuffer1, int* pOptBuffer1Size, char* pOptBuffer2, int* pOptBuffer2Size) {
    g_lastExportTick = GetTickCount64();
    return 1.0f;
}

// DLL Entry Point
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_DETACH) {
        if (g_hookInstalled) {
            RemoveHook(NULL, 0, NULL, NULL, NULL, NULL);
        }
    }
    return TRUE;
}
