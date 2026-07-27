#include "platform/FileDialog.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>

static std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int count = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.length(), NULL, 0);
    if (count <= 0) return L"";
    std::wstring wstr(count, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.length(), &wstr[0], count);
    return wstr;
}

static std::string wideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int count = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.length(), NULL, 0, NULL, NULL);
    if (count <= 0) return "";
    std::string str(count, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.length(), &str[0], count, NULL, NULL);
    return str;
}
#endif

namespace FileDialog {

std::vector<FilterSpec> getImportFilters() {
    return {
        { "All Supported Files (*.sgl, *.obj, *.stl, *.ply, *.glb, *.gltf)", "*.sgl;*.obj;*.stl;*.ply;*.glb;*.gltf" },
        { "SculptGL Scene (*.sgl)", "*.sgl" },
        { "Wavefront OBJ (*.obj)", "*.obj" },
        { "STL (*.stl)", "*.stl" },
        { "PLY (*.ply)", "*.ply" },
        { "GLTF / GLB (*.glb, *.gltf)", "*.glb;*.gltf" },
        { "All Files (*.*)", "*.*" }
    };
}

std::vector<FilterSpec> getExportFilters() {
    return {
        { "SculptGL Scene (*.sgl)", "*.sgl" },
        { "Wavefront OBJ (*.obj)", "*.obj" },
        { "STL (*.stl)", "*.stl" },
        { "PLY (*.ply)", "*.ply" },
        { "GLTF / GLB (*.glb, *.gltf)", "*.glb;*.gltf" },
        { "All Files (*.*)", "*.*" }
    };
}

std::string openFile(const std::vector<FilterSpec>& filters, const std::string& title) {
#ifdef _WIN32
    OPENFILENAMEW ofn;
    wchar_t szFile[MAX_PATH] = { 0 };

    std::vector<FilterSpec> effectiveFilters = filters.empty() ? getImportFilters() : filters;

    std::wstring wFilter;
    for (const auto& f : effectiveFilters) {
        std::wstring nameW = utf8ToWide(f.name);
        std::wstring patternW = utf8ToWide(f.pattern);
        wFilter += nameW;
        wFilter.push_back(L'\0');
        wFilter += patternW;
        wFilter.push_back(L'\0');
    }
    wFilter.push_back(L'\0');

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    ofn.lpstrFilter = wFilter.c_str();
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;

    std::wstring titleW;
    if (!title.empty()) {
        titleW = utf8ToWide(title);
        ofn.lpstrTitle = titleW.c_str();
    }

    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;

    if (GetOpenFileNameW(&ofn)) {
        return wideToUtf8(szFile);
    }
#endif
    return "";
}

std::string saveFile(const std::vector<FilterSpec>& filters, const std::string& defaultExt, const std::string& title) {
#ifdef _WIN32
    OPENFILENAMEW ofn;
    wchar_t szFile[MAX_PATH] = { 0 };

    std::vector<FilterSpec> effectiveFilters = filters.empty() ? getExportFilters() : filters;

    std::wstring wFilter;
    for (const auto& f : effectiveFilters) {
        std::wstring nameW = utf8ToWide(f.name);
        std::wstring patternW = utf8ToWide(f.pattern);
        wFilter += nameW;
        wFilter.push_back(L'\0');
        wFilter += patternW;
        wFilter.push_back(L'\0');
    }
    wFilter.push_back(L'\0');

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    ofn.lpstrFilter = wFilter.c_str();
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;

    std::wstring titleW;
    if (!title.empty()) {
        titleW = utf8ToWide(title);
        ofn.lpstrTitle = titleW.c_str();
    }

    std::wstring defExtW;
    if (!defaultExt.empty()) {
        defExtW = utf8ToWide(defaultExt);
        ofn.lpstrDefExt = defExtW.c_str();
    }

    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_EXPLORER;

    if (GetSaveFileNameW(&ofn)) {
        return wideToUtf8(szFile);
    }
#endif
    return "";
}

} // namespace FileDialog
