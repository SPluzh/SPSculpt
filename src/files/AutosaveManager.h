#pragma once

#include <atomic>
#include <cstdint>
#include <future>
#include <string>
#include <vector>

class Scene;
class AngleRenderer;
class SculptManager;
class IniFile;

struct PendingAutosaveRestore {
    bool available = false;
    std::string filePath;
    std::string fallbackFilePath;
    std::string originalScenePath;
    bool wasUnsaved = true;
    bool hasUndoHistory = false;
    std::string savedTimeStr;
    int restoreAttempts = 0;
};

class AutosaveManager {
public:
    AutosaveManager() = default;
    ~AutosaveManager();

    AutosaveManager(const AutosaveManager&) = delete;
    AutosaveManager& operator=(const AutosaveManager&) = delete;

    // Settings persistence ([Autosave] section in app_settings.cfg)
    void loadSettings(const IniFile& ini);
    void saveSettings(IniFile& ini) const;

    // Lifecycle
    void initOnStartup();
    void shutdown();

    // Main loop tick: returns true if an autosave was triggered or finished this tick
    bool tick(float dtSeconds,
              const Scene& scene,
              const AngleRenderer& renderer,
              const SculptManager& sculpt,
              uint64_t workTimeSeconds,
              const std::string& currentScenePath,
              bool isBusy);

    // Manual trigger (e.g. "Autosave Now" button in Preferences)
    bool triggerAutosaveNow(const Scene& scene,
                            const AngleRenderer& renderer,
                            const SculptManager& sculpt,
                            uint64_t workTimeSeconds,
                            const std::string& currentScenePath);

    // Notify manager when user manually saves or opens/creates a scene
    void onManualSave(const std::string& savedPath);
    void onSceneResetOrOpened();

    // Crash / restore state management
    const PendingAutosaveRestore& getPendingRestore() const { return m_pendingRestore; }
    bool hasPendingRestore() const { return m_pendingRestore.available; }
    void markRestoreAttempt(const std::string& filePath);
    void markRestoreSuccess();
    void discardPendingRestore();

    // Directory helpers
    static std::string getDefaultDirectory();
    std::string getEffectiveDirectory() const;
    const std::string& getCustomDirectory() const { return m_customDirectory; }
    void setCustomDirectory(const std::string& dir) { m_customDirectory = dir; }
    void resetToDefaultDirectory() { m_customDirectory.clear(); }

    std::string getLatestAutosavePath() const;
    void openDirectoryInShell() const;
    int clearAllAutosaves();

    // Configuration accessors
    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool enabled) { m_enabled = enabled; }

    int getIntervalMinutes() const { return m_intervalMinutes; }
    void setIntervalMinutes(int minutes);

    int getMaxCopies() const { return m_maxCopies; }
    void setMaxCopies(int copies);

    bool getSaveUndoHistory() const { return m_saveUndoHistory; }
    void setSaveUndoHistory(bool saveUndo) { m_saveUndoHistory = saveUndo; }

    // Status for UI
    bool isWriting() const { return m_writing.load(); }
    bool consumeWriteCompleted(bool& outSuccess);
    const std::string& getLastSaveTimeStr() const { return m_lastSaveTimeStr; }
    const std::string& getLastStatusMessage() const { return m_lastStatusMessage; }
    float getSecondsUntilNextAutosave() const;

private:
    bool pollFinishedWrite();
    bool startAsyncWrite(std::vector<uint8_t>&& buffer,
                         const std::string& currentScenePath,
                         bool hasUndoHistory);
    void writeCleanExitMarker() const;

    // Config
    bool m_enabled = true;
    int m_intervalMinutes = 5;
    int m_maxCopies = 3;
    bool m_saveUndoHistory = false;
    std::string m_customDirectory;

    // Runtime state
    float m_elapsedSeconds = 0.0f;
    uint64_t m_lastSavedRevision = 0;
    bool m_savedAtCurrentRevision = false;
    bool m_prevSceneModified = false;

    std::atomic<bool> m_writing{false};
    std::future<bool> m_writeFuture;
    bool m_hasWriteNotification = false;
    bool m_lastWriteSuccess = false;
    std::string m_pendingSaveTimeStr;
    std::string m_lastSaveTimeStr;
    std::string m_lastStatusMessage = "No autosave yet";

    PendingAutosaveRestore m_pendingRestore;
};

