#pragma once

#include <string>

namespace ZBrushBridge {

enum class SyncMode {
    ActiveOnly = 0,
    VisibleOnly = 1,
    FullScene = 2
};

class Config {
public:
    static std::string getDataDir();
    static void setDataDir(const std::string& dir);

    static std::string getMeshGoZPath();
    static std::string getCameraPath();
    static std::string getCommandScriptPath();
    static std::string getSceneManifestPath();
    static std::string getSubtoolsDir();
    static std::string getOutSubtoolsDir();
    static std::string getImportSceneScriptPath();

    static int getDebounceDelayMs();
    static void setDebounceDelayMs(int ms);

    static int getSceneDebounceDelayMs();
    static void setSceneDebounceDelayMs(int ms);

    static bool isAutoSyncEnabled();
    static void setAutoSyncEnabled(bool enabled);

    static SyncMode getSyncMode();
    static void setSyncMode(SyncMode mode);

    static bool isAutoDeleteRemovedSubTools();
    static void setAutoDeleteRemovedSubTools(bool val);

    static bool isImportPolyPaintEnabled();
    static void setImportPolyPaintEnabled(bool enabled);

    static bool isImportPolyGroupsEnabled();
    static void setImportPolyGroupsEnabled(bool enabled);

    static bool isKeepNativeQuadsEnabled();
    static void setKeepNativeQuadsEnabled(bool enabled);

    static int getFileReadyRetries();
    static void setFileReadyRetries(int retries);

    static void loadConfig();
    static void saveConfig();

    static std::string getDefaultDataDir();
    static std::string getConfigFileLocalAppdata();

private:
    static std::string s_customDataDir;
    static int s_debounceDelayMs;
    static int s_sceneDebounceDelayMs;
    static bool s_autoSyncEnabled;
    static SyncMode s_syncMode;
    static bool s_autoDeleteRemovedSubTools;
    static bool s_importPolyPaint;
    static bool s_importPolyGroups;
    static bool s_keepNativeQuads;
    static int s_fileReadyRetries;
    static bool s_loaded;
};

} // namespace ZBrushBridge
