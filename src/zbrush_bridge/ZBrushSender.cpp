#include "ZBrushSender.h"
#include "ZBrushBridgeConfig.h"
#include "files/ExportGoZ.h"
#include "common/Logger.h"

#include <chrono>
#include <fstream>
#include <filesystem>
#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef _OPENMP
#include <omp.h>
#endif

namespace ZBrushBridge {

namespace fs = std::filesystem;

namespace {

std::string sanitizeName(const std::string& raw) {
    std::string clean;
    clean.reserve(raw.size());
    for (char c : raw) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
            clean.push_back(c);
        } else if (c == ' ' || c == '-' || c == '.') {
            clean.push_back('_');
        }
    }
    if (clean.empty()) clean = "SubTool";
    return clean;
}

#ifdef _WIN32
HWND findZBrushHwnd() {
    HWND bestHwnd = NULL;
    // 1. High-precision check: verify process executable is zbrush.exe
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        if (IsWindowVisible(hwnd)) {
            LONG style = GetWindowLongA(hwnd, GWL_STYLE);
            if ((style & WS_CHILD) != 0) return TRUE;

            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == 0) return TRUE;

            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProc) {
                char exePath[MAX_PATH] = {0};
                DWORD sz = MAX_PATH;
                if (QueryFullProcessImageNameA(hProc, 0, exePath, &sz)) {
                    std::string p(exePath);
                    std::transform(p.begin(), p.end(), p.begin(), ::tolower);
                    if (p.find("zbrush.exe") != std::string::npos) {
                        *(HWND*)lParam = hwnd;
                        CloseHandle(hProc);
                        return FALSE;
                    }
                }
                CloseHandle(hProc);
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&bestHwnd));

    // 2. Fallback: match window title if process query failed due to permissions
    if (!bestHwnd) {
        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            if (IsWindowVisible(hwnd)) {
                char title[256] = {0};
                GetWindowTextA(hwnd, title, sizeof(title));
                LONG style = GetWindowLongA(hwnd, GWL_STYLE);
                if ((style & WS_CHILD) == 0 && strstr(title, "ZBrush") != nullptr) {
                    *(HWND*)lParam = hwnd;
                    return FALSE;
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&bestHwnd));
    }

    return bestHwnd;
}

bool sendCopyDataScript(HWND hwnd, const std::string& scriptPath) {
    if (!hwnd) return false;
    std::string winPath = scriptPath;
    for (char& c : winPath) {
        if (c == '/') c = '\\';
    }
    COPYDATASTRUCT cds;
    cds.dwData = 0;
    cds.cbData = static_cast<DWORD>(winPath.length() + 1);
    cds.lpData = const_cast<char*>(winPath.c_str());
    SendMessageA(hwnd, WM_COPYDATA, reinterpret_cast<WPARAM>(hwnd), reinterpret_cast<LPARAM>(&cds));
    // Immediately wake up ZBrush's background [Sleep] loop (event mask bit 2 = WM_MOUSEMOVE)
    PostMessageA(hwnd, WM_MOUSEMOVE, 0, 0);
    return true;
}
#endif

} // anonymous namespace

bool isZBrushRunning() {
#ifdef _WIN32
    return findZBrushHwnd() != NULL;
#else
    return false;
#endif
}

SendResult sendActiveMeshToZBrush(const Mesh* mesh) {
    auto tStart = std::chrono::high_resolution_clock::now();
    if (!mesh) {
        return { false, 0, 0.0, "No active mesh selected in scene" };
    }

    std::string outDir = Config::getOutSubtoolsDir();
    std::string meshName = sanitizeName(mesh->outlinerName);
    std::string gozPath = outDir + "mesh_active.GoZ";

    // 1. Export GoZ
    if (!ExportGoZ::exportGoZ(gozPath, mesh)) {
        return { false, 0, 0.0, "Failed to export mesh to GoZ" };
    }

    // 2. Generate ZScript command
    std::string cmdPath = Config::getDataDir() + "cmd_import_active.txt";
    std::ofstream ofs(cmdPath);
    if (!ofs.is_open()) {
        return { false, 0, 0.0, "Failed to write ZScript import command" };
    }

    ofs << "// SPSculpt Auto-Import Active Mesh\n";
    ofs << "[If, [IsEnabled, \"Tool:Import\"],\n";
    ofs << "    [FileNameSetNext, \"" << gozPath << "\"]\n";
    ofs << "    [IPress, \"Tool:Import\"]\n";
    ofs << "    [Note, \"SPSculpt: Updated active SubTool!\", , 1]\n";
    ofs << ",\n";
    ofs << "    [Note, \"SPSculpt: Tool:Import is disabled. Please enter 3D Edit mode.\", , 2]\n";
    ofs << "]\n";
    ofs.close();

#ifdef _WIN32
    HWND zbrushHwnd = findZBrushHwnd();
    if (!zbrushHwnd) {
        auto tEnd = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
        return { false, 1, ms, "Exported mesh, but ZBrush is not running" };
    }
    if (!sendCopyDataScript(zbrushHwnd, cmdPath)) {
        return { false, 0, 0.0, "Failed to send WM_COPYDATA to ZBrush" };
    }
#endif

    auto tEnd = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    sculpt_log("[ZBrush Bridge] Sent active mesh '%s' to ZBrush in %.2f ms\n", meshName.c_str(), ms);

    return { true, 1, ms, "Sent active mesh '" + meshName + "' to ZBrush (Auto-imported or click 'Pull Active')" };
}

SendResult sendSceneToZBrush(const Scene& scene, bool visibleOnly) {
    auto tStart = std::chrono::high_resolution_clock::now();
    const auto& allMeshes = scene.getMeshes();

    std::vector<const Mesh*> targets;
    for (const Mesh* m : allMeshes) {
        if (!m) continue;
        if (visibleOnly && !m->visibleV1 && !m->visibleV2) continue;
        targets.push_back(m);
    }

    if (targets.empty()) {
        return { false, 0, 0.0, "No meshes matching filter to send" };
    }

    std::string outDir = Config::getOutSubtoolsDir();
    std::vector<std::string> gozPaths(targets.size());
    std::vector<std::string> meshNames(targets.size());

    // Export all target meshes in parallel with OpenMP
    #pragma omp parallel for schedule(dynamic) if(targets.size() > 1)
    for (size_t i = 0; i < targets.size(); ++i) {
        meshNames[i] = sanitizeName(targets[i]->outlinerName);
        gozPaths[i] = outDir + "subtool_" + std::to_string(i) + ".GoZ";
        ExportGoZ::exportGoZ(gozPaths[i], targets[i]);
    }

    // Generate full scene import script
    std::string cmdPath = Config::getImportSceneScriptPath();
    std::ofstream ofs(cmdPath);
    if (!ofs.is_open()) {
        return { false, 0, 0.0, "Failed to create scene import script" };
    }

    ofs << "// SPSculpt Full Scene Import Script\n";
    ofs << "[IFreeze,\n";
    ofs << "    [VarDef, totalSt, 0]\n";
    ofs << "    [VarSet, totalSt, [SubToolGetCount]]\n";
    ofs << "    [VarDef, origAct, 0]\n";
    ofs << "    [VarSet, origAct, [SubToolGetActiveIndex]]\n";
    ofs << "    [VarDef, rawTitle, \"\"]\n";
    ofs << "    [VarDef, cleanTitle, \"\"]\n";
    ofs << "    [VarDef, tLen, 0]\n";
    ofs << "    [VarDef, foundIdx, -1]\n\n";

    for (size_t i = 0; i < targets.size(); ++i) {
        const std::string& name = meshNames[i];
        const std::string& path = gozPaths[i];

        ofs << "    // Mesh " << i << ": " << name << "\n";
        ofs << "    [VarSet, foundIdx, -1]\n";
        ofs << "    [Loop, totalSt,\n";
        ofs << "        [SubToolSelect, n]\n";
        ofs << "        [VarSet, rawTitle, [IGetTitle, \"Tool:ItemInfo\"]]\n";
        ofs << "        [VarSet, tLen, [StrLength, rawTitle]]\n";
        ofs << "        [If, tLen > 1, [VarSet, cleanTitle, [StrExtract, rawTitle, 0, tLen - 2]], [VarSet, cleanTitle, \"\"]]\n";
        ofs << "        [If, [StrCompare, cleanTitle, \"" << name << "\"] == 0,\n";
        ofs << "            [VarSet, foundIdx, n]\n";
        ofs << "            [LoopExit]\n";
        ofs << "        ]\n";
        ofs << "    , n]\n";

        ofs << "    [If, foundIdx != -1,\n";
        ofs << "        [SubToolSelect, foundIdx]\n";
        ofs << "        [FileNameSetNext, \"" << path << "\"]\n";
        ofs << "        [IPress, \"Tool:Import\"]\n";
        ofs << "    ,\n";
        ofs << "        [If, [IsEnabled, \"Tool:SubTool:Append\"],\n";
        ofs << "            [IPress, \"Tool:SubTool:Append\"]\n";
        ofs << "            [SubToolSelect, [SubToolGetCount] - 1]\n";
        ofs << "            [FileNameSetNext, \"" << path << "\"]\n";
        ofs << "            [IPress, \"Tool:Import\"]\n";
        ofs << "        ]\n";
        ofs << "    ]\n\n";
    }

    ofs << "    [SubToolSelect, origAct]\n";
    ofs << "]\n";
    ofs << "[Note, \"SPSculpt: Full scene updated in ZBrush! (" << targets.size() << " SubTools)\", , 1]\n";
    ofs.close();

#ifdef _WIN32
    HWND zbrushHwnd = findZBrushHwnd();
    if (!zbrushHwnd) {
        auto tEnd = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
        return { false, static_cast<int>(targets.size()), ms, "Exported " + std::to_string(targets.size()) + " scene files, but ZBrush is not running" };
    }
    if (!sendCopyDataScript(zbrushHwnd, cmdPath)) {
        return { false, 0, 0.0, "Failed to send WM_COPYDATA to ZBrush" };
    }
#endif

    auto tEnd = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    sculpt_log("[ZBrush Bridge] Sent full scene (%zu meshes) to ZBrush in %.2f ms\n", targets.size(), ms);

    return { true, static_cast<int>(targets.size()), ms, "Sent " + std::to_string(targets.size()) + " meshes to ZBrush (Auto-imported or click 'Pull Scene')" };
}

} // namespace ZBrushBridge
