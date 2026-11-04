#pragma once

#include <string>

namespace ZBrushBridge {

class Config {
public:
    static std::string getDataDir();
    static void setDataDir(const std::string& dir);

    static std::string getMeshGoZPath();
    static std::string getCameraPath();
    static std::string getCommandScriptPath();

    static int getDebounceDelayMs();
    static void setDebounceDelayMs(int ms);

    static bool isAutoSyncEnabled();
    static void setAutoSyncEnabled(bool enabled);

    static void loadConfig();
    static void saveConfig();

    static std::string getDefaultDataDir();
    static std::string getConfigFileLocalAppdata();

private:
    static std::string s_customDataDir;
    static int s_debounceDelayMs;
    static bool s_autoSyncEnabled;
    static bool s_loaded;
};

} // namespace ZBrushBridge
