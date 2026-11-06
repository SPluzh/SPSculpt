#include "platform/FileDialog.h"
#include "common/StringUtils.h"
#include "common/FormatConstants.h"
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <shlobj.h>
#endif

namespace FileDialog {

std::vector<FilterSpec> getImportFilters() {
    return {
        { "All Supported Files (*.spsculpt, *.sgl, *.obj, *.stl, *.ply, *.glb, *.gltf, *.fbx, *.goz)", "*.spsculpt;*.sgl;*.obj;*.stl;*.ply;*.glb;*.gltf;*.fbx;*.goz" },
        { "SPSculpt Project (*.spsculpt)", "*.spsculpt" },
        { "Legacy SculptGL Scene (*.sgl)", "*.sgl" },
        { "Wavefront OBJ (*.obj)", "*.obj" },
        { "STL (*.stl)", "*.stl" },
        { "PLY (*.ply)", "*.ply" },
        { "GLTF / GLB (*.glb, *.gltf)", "*.glb;*.gltf" },
        { "ZBrush GoZ (*.goz)", "*.goz" },
        { "Autodesk FBX (*.fbx)", "*.fbx" },
        { "ZBrush GoZ (*.goz)", "*.goz" },
        { "All Files (*.*)", "*.*" }
    };
}

std::vector<FilterSpec> getExportFilters() {
    return {
        { "SPSculpt Project (*.spsculpt)", "*.spsculpt" },
        { "Legacy SculptGL Scene (*.sgl)", "*.sgl" },
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
#else
    std::vector<FilterSpec> effectiveFilters = filters.empty() ? getImportFilters() : filters;
    std::string cmd = "zenity --file-selection";
    if (!title.empty()) {
        cmd += " --title=\"" + title + "\"";
    }
    for (const auto& f : effectiveFilters) {
        std::string pat = f.pattern;
        std::replace(pat.begin(), pat.end(), ';', ' ');
        cmd += " --file-filter=\"" + f.name + " | " + pat + "\"";
    }
    cmd += " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        char buf[1024];
        std::string res;
        while (fgets(buf, sizeof(buf), pipe) != nullptr) {
            res += buf;
        }
        pclose(pipe);
        while (!res.empty() && (res.back() == '\n' || res.back() == '\r')) {
            res.pop_back();
        }
        if (!res.empty()) return res;
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
#else
    std::vector<FilterSpec> effectiveFilters = filters.empty() ? getExportFilters() : filters;
    std::string cmd = "zenity --file-selection --save --confirm-overwrite";
    if (!title.empty()) {
        cmd += " --title=\"" + title + "\"";
    }
    for (const auto& f : effectiveFilters) {
        std::string pat = f.pattern;
        std::replace(pat.begin(), pat.end(), ';', ' ');
        cmd += " --file-filter=\"" + f.name + " | " + pat + "\"";
    }
    cmd += " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        char buf[1024];
        std::string res;
        while (fgets(buf, sizeof(buf), pipe) != nullptr) {
            res += buf;
        }
        pclose(pipe);
        while (!res.empty() && (res.back() == '\n' || res.back() == '\r')) {
            res.pop_back();
        }
        if (!res.empty()) {
            if (!defaultExt.empty() && res.find('.') == std::string::npos) {
                std::string ext = defaultExt;
                if (ext.front() != '.') ext = "." + ext;
                res += ext;
            }
            return res;
        }
    }
#endif
    return "";
}

std::string pickFolder(const std::string& title) {
#ifdef _WIN32
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    IFileOpenDialog* pFolderDlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFolderDlg));
    if (SUCCEEDED(hr) && pFolderDlg) {
        DWORD dwOptions = 0;
        if (SUCCEEDED(pFolderDlg->GetOptions(&dwOptions))) {
            pFolderDlg->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        }
        if (!title.empty()) {
            std::wstring wTitle = utf8ToWide(title);
            pFolderDlg->SetTitle(wTitle.c_str());
        }
        HWND hwndOwner = GetActiveWindow();
        if (SUCCEEDED(pFolderDlg->Show(hwndOwner))) {
            IShellItem* pItem = nullptr;
            if (SUCCEEDED(pFolderDlg->GetResult(&pItem)) && pItem) {
                PWSTR pszFilePath = nullptr;
                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath)) && pszFilePath) {
                    std::string result = wideToUtf8(pszFilePath);
                    CoTaskMemFree(pszFilePath);
                    pItem->Release();
                    pFolderDlg->Release();
                    if (SUCCEEDED(hrCo)) CoUninitialize();
                    for (char& c : result) {
                        if (c == '\\') c = '/';
                    }
                    if (!result.empty() && result.back() != '/') {
                        result += '/';
                    }
                    return result;
                }
                pItem->Release();
            }
        }
        pFolderDlg->Release();
    }

    // Fallback: SHBrowseForFolderW
    BROWSEINFOW bi = { 0 };
    bi.hwndOwner = GetActiveWindow();
    std::wstring titleW = utf8ToWide(title);
    bi.lpszTitle = titleW.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl != 0) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) {
            CoTaskMemFree(pidl);
            if (SUCCEEDED(hrCo)) CoUninitialize();
            std::string res = wideToUtf8(path);
            for (char& c : res) {
                if (c == '\\') c = '/';
            }
            if (!res.empty() && res.back() != '/') {
                res += '/';
            }
            return res;
        }
        CoTaskMemFree(pidl);
    }

    if (SUCCEEDED(hrCo)) CoUninitialize();
    return "";
#else
    std::string cmd = "zenity --file-selection --directory";
    if (!title.empty()) {
        cmd += " --title=\"" + title + "\"";
    }
    cmd += " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        char buf[1024];
        std::string res;
        while (fgets(buf, sizeof(buf), pipe) != nullptr) {
            res += buf;
        }
        pclose(pipe);
        while (!res.empty() && (res.back() == '\n' || res.back() == '\r')) {
            res.pop_back();
        }
        if (!res.empty()) {
            for (char& c : res) {
                if (c == '\\') c = '/';
            }
            if (res.back() != '/') res += '/';
            return res;
        }
    }
    return "";
#endif
}

} // namespace FileDialog
