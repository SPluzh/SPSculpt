#include "ZBrushBridgeConfig.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace ZBrushBridge {

namespace fs = std::filesystem;

std::string Config::s_customDataDir = "";
int Config::s_debounceDelayMs = 250;
bool Config::s_autoSyncEnabled = true;
bool Config::s_importPolyPaint = true;
bool Config::s_importPolyGroups = true;
bool Config::s_keepNativeQuads = true;
int Config::s_fileReadyRetries = 25;
bool Config::s_loaded = false;

static std::string normalizeDir(std::string path) {
    if (path.empty()) return path;
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    if (path.back() != '/') {
        path += '/';
    }
    return path;
}

std::string Config::getDefaultDataDir() {
#ifdef _WIN32
    char pubDocs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_COMMON_DOCUMENTS, NULL, 0, pubDocs))) {
        fs::path p(pubDocs);
        p /= "SPSculptData";
        return normalizeDir(p.string());
    }
    return "C:/Users/Public/Documents/SPSculptData/";
#else
    const char* home = std::getenv("HOME");
    if (home) {
        fs::path p(home);
        p /= ".spsculpt/bridge";
        return normalizeDir(p.string());
    }
    return "/tmp/SPSculptData/";
#endif
}

std::string Config::getConfigFileLocalAppdata() {
#ifdef _WIN32
    char appData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appData))) {
        fs::path p(appData);
        p /= "SPSculpt";
        p /= "bridge_config.json";
        return p.string();
    }
#endif
    return "";
}

void Config::loadConfig() {
    s_loaded = true;

    // 1. Try reading %LOCALAPPDATA%\SPSculpt\bridge_config.json
    std::string appDataCfg = getConfigFileLocalAppdata();
    if (!appDataCfg.empty() && fs::exists(appDataCfg)) {
        try {
            std::ifstream f(appDataCfg);
            if (f.is_open()) {
                nlohmann::json j;
                f >> j;
                if (j.contains("data_dir") && j["data_dir"].is_string()) {
                    s_customDataDir = normalizeDir(j["data_dir"].get<std::string>());
                }
                if (j.contains("debounce_delay_ms") && j["debounce_delay_ms"].is_number_integer()) {
                    s_debounceDelayMs = j["debounce_delay_ms"].get<int>();
                }
                if (j.contains("auto_sync_enabled") && j["auto_sync_enabled"].is_boolean()) {
                    s_autoSyncEnabled = j["auto_sync_enabled"].get<bool>();
                }
                if (j.contains("import_polypaint") && j["import_polypaint"].is_boolean()) {
                    s_importPolyPaint = j["import_polypaint"].get<bool>();
                }
                if (j.contains("import_polygroups") && j["import_polygroups"].is_boolean()) {
                    s_importPolyGroups = j["import_polygroups"].get<bool>();
                }
                if (j.contains("keep_native_quads") && j["keep_native_quads"].is_boolean()) {
                    s_keepNativeQuads = j["keep_native_quads"].get<bool>();
                }
                if (j.contains("file_ready_retries") && j["file_ready_retries"].is_number_integer()) {
                    s_fileReadyRetries = j["file_ready_retries"].get<int>();
                }
                return;
            }
        } catch (...) {
            // Ignore parse errors, fallback to env / defaults
        }
    }

    // 2. Try environment variable SPSCULPT_DATA_DIR
    const char* envDir = std::getenv("SPSCULPT_DATA_DIR");
    if (envDir && strlen(envDir) > 0) {
        s_customDataDir = normalizeDir(envDir);
    }
}

void Config::saveConfig() {
    std::string appDataCfg = getConfigFileLocalAppdata();
    if (appDataCfg.empty()) return;

    try {
        fs::path cfgPath(appDataCfg);
        if (cfgPath.has_parent_path()) {
            fs::create_directories(cfgPath.parent_path());
        }

        nlohmann::json j;
        j["data_dir"] = getDataDir();
        j["debounce_delay_ms"] = s_debounceDelayMs;
        j["auto_sync_enabled"] = s_autoSyncEnabled;
        j["import_polypaint"] = s_importPolyPaint;
        j["import_polygroups"] = s_importPolyGroups;
        j["keep_native_quads"] = s_keepNativeQuads;
        j["file_ready_retries"] = s_fileReadyRetries;

        std::ofstream f(appDataCfg);
        if (f.is_open()) {
            f << j.dump(4);
        }
    } catch (...) {
        // Suppress filesystem exceptions
    }
}

std::string Config::getDataDir() {
    if (!s_loaded) {
        loadConfig();
    }

    std::string dir = s_customDataDir;
    if (dir.empty()) {
        const char* envDir = std::getenv("SPSCULPT_DATA_DIR");
        if (envDir && strlen(envDir) > 0) {
            dir = normalizeDir(envDir);
        } else {
            dir = getDefaultDataDir();
        }
    }

    // Ensure directory exists on disk
    try {
        fs::create_directories(dir);
    } catch (...) {}

    return dir;
}

void Config::setDataDir(const std::string& dir) {
    s_customDataDir = normalizeDir(dir);
    saveConfig();
}

std::string Config::getMeshGoZPath() {
    return getDataDir() + "mesh.GoZ";
}

std::string Config::getCameraPath() {
    return getDataDir() + "camera.txt";
}

std::string Config::getCommandScriptPath() {
    return getDataDir() + "cmd_sync.txt";
}

int Config::getDebounceDelayMs() {
    if (!s_loaded) loadConfig();
    return s_debounceDelayMs;
}

void Config::setDebounceDelayMs(int ms) {
    if (ms < 50) ms = 50;
    if (ms > 2000) ms = 2000;
    s_debounceDelayMs = ms;
    saveConfig();
}

bool Config::isAutoSyncEnabled() {
    if (!s_loaded) loadConfig();
    return s_autoSyncEnabled;
}

void Config::setAutoSyncEnabled(bool enabled) {
    s_autoSyncEnabled = enabled;
    saveConfig();
}

bool Config::isImportPolyPaintEnabled() {
    if (!s_loaded) loadConfig();
    return s_importPolyPaint;
}

void Config::setImportPolyPaintEnabled(bool enabled) {
    s_importPolyPaint = enabled;
    saveConfig();
}

bool Config::isImportPolyGroupsEnabled() {
    if (!s_loaded) loadConfig();
    return s_importPolyGroups;
}

void Config::setImportPolyGroupsEnabled(bool enabled) {
    s_importPolyGroups = enabled;
    saveConfig();
}

bool Config::isKeepNativeQuadsEnabled() {
    if (!s_loaded) loadConfig();
    return s_keepNativeQuads;
}

void Config::setKeepNativeQuadsEnabled(bool enabled) {
    s_keepNativeQuads = enabled;
    saveConfig();
}

int Config::getFileReadyRetries() {
    if (!s_loaded) loadConfig();
    return s_fileReadyRetries;
}

void Config::setFileReadyRetries(int retries) {
    if (retries < 10) retries = 10;
    if (retries > 100) retries = 100;
    s_fileReadyRetries = retries;
    saveConfig();
}

} // namespace ZBrushBridge
