#include "files/AutosaveManager.h"

#include "common/FileUtils.h"
#include "common/IniFile.h"
#include "common/Logger.h"
#include "common/StringUtils.h"
#include "editing/SculptManager.h"
#include "editing/undo/UndoManager.h"
#include "files/ExportSGL.h"
#include "render/AngleRenderer.h"
#include "scene/Scene.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace {

std::filesystem::path toFsPath(const std::string& utf8Path) {
#ifdef _WIN32
    return std::filesystem::path(utf8ToWide(utf8Path));
#else
    return std::filesystem::path(utf8Path);
#endif
}

std::string fromFsPath(const std::filesystem::path& p) {
#ifdef _WIN32
    return wideToUtf8(p.wstring());
#else
    return p.string();
#endif
}

uint64_t getCurrentPid() {
#ifdef _WIN32
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

bool isProcessAlive(uint64_t pid) {
    if (pid == 0 || pid == getCurrentPid()) return false;
#ifdef _WIN32
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!hProc) return false;
    DWORD exitCode = 0;
    BOOL ok = GetExitCodeProcess(hProc, &exitCode);
    CloseHandle(hProc);
    return ok && exitCode == STILL_ACTIVE;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

std::string formatCurrentTimeStr() {
    std::time_t now = std::time(nullptr);
    std::tm tmBuf{};
#ifdef _WIN32
    localtime_s(&tmBuf, &now);
#else
    localtime_r(&now, &tmBuf);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
                  tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
    return std::string(buf);
}

std::string sanitizeFilenameStem(const std::string& utf8Stem) {
    std::string out;
    out.reserve(utf8Stem.size());
    for (unsigned char c : utf8Stem) {
        if (c < 32 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            out.push_back('_');
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    if (out.size() > 80) {
        out.resize(80);
    }
    return out;
}

std::string makeSlotPrefix(const std::string& scenePath) {
    if (scenePath.empty()) {
        return "autosave";
    }
    std::filesystem::path p = toFsPath(scenePath);
    std::string stem = sanitizeFilenameStem(fromFsPath(p.stem()));
    if (stem.empty()) {
        return "autosave";
    }
    return stem + "_autosave";
}

bool isAutosaveFilename(const std::string& name, bool includeTmp) {
    const bool validExt = name.ends_with(".spsculpt") || (includeTmp && name.ends_with(".tmp"));
    if (!validExt) return false;
    return name.rfind("autosave_", 0) == 0 || name.find("_autosave_") != std::string::npos;
}

std::string makeSlotFilename(const std::string& prefix, int index) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "_%02d.spsculpt", index);
    return prefix + buf;
}

} // namespace

AutosaveManager::~AutosaveManager() {
    if (m_writeFuture.valid()) {
        m_writeFuture.wait();
    }
}

void AutosaveManager::setIntervalMinutes(int minutes) {
    m_intervalMinutes = std::clamp(minutes, 1, 60);
}

void AutosaveManager::setMaxCopies(int copies) {
    m_maxCopies = std::clamp(copies, 1, 10);
}

void AutosaveManager::loadSettings(const IniFile& ini) {
    const char* sec = "Autosave";
    if (ini.hasKey(sec, "enabled")) m_enabled = ini.getBool(sec, "enabled", true);
    if (ini.hasKey(sec, "intervalMinutes")) setIntervalMinutes(ini.getInt(sec, "intervalMinutes", 5));
    if (ini.hasKey(sec, "maxCopies")) setMaxCopies(ini.getInt(sec, "maxCopies", 3));
    if (ini.hasKey(sec, "saveUndoHistory")) m_saveUndoHistory = ini.getBool(sec, "saveUndoHistory", false);
    if (ini.hasKey(sec, "directory")) m_customDirectory = ini.get(sec, "directory", "");
}

void AutosaveManager::saveSettings(IniFile& ini) const {
    const char* sec = "Autosave";
    ini.setBool(sec, "enabled", m_enabled);
    ini.setInt(sec, "intervalMinutes", m_intervalMinutes);
    ini.setInt(sec, "maxCopies", m_maxCopies);
    ini.setBool(sec, "saveUndoHistory", m_saveUndoHistory);
    ini.set(sec, "directory", m_customDirectory);
}

std::string AutosaveManager::getDefaultDirectory() {
    std::string exeDir = FileUtils::getExecutableDir();
    std::filesystem::path base = exeDir.empty() ? std::filesystem::current_path() : toFsPath(exeDir);
    return fromFsPath(base / "autosaves");
}

std::string AutosaveManager::getEffectiveDirectory() const {
    if (!m_customDirectory.empty()) {
        return m_customDirectory;
    }
    return getDefaultDirectory();
}

std::string AutosaveManager::getLatestAutosavePath() const {
    std::filesystem::path dir = toFsPath(getEffectiveDirectory());
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return {};
    }

    IniFile meta;
    if (meta.load(fromFsPath(dir / "autosave_meta.ini"))) {
        std::string latest = meta.get("Meta", "latestFile", "");
        if (!latest.empty()) {
            std::filesystem::path lp = toFsPath(latest);
            if (std::filesystem::exists(lp, ec) && std::filesystem::is_regular_file(lp, ec)) {
                return latest;
            }
        }
    }

    // Fallback: pick the most recently modified autosave_*.spsculpt in the directory
    std::filesystem::path newestPath;
    std::filesystem::file_time_type newestTime{};
    bool found = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string name = fromFsPath(entry.path().filename());
        if (isAutosaveFilename(name, false)) {
            auto t = entry.last_write_time(ec);
            if (!ec && (!found || t > newestTime)) {
                newestTime = t;
                newestPath = entry.path();
                found = true;
            }
        }
    }
    return found ? fromFsPath(newestPath) : std::string();
}

void AutosaveManager::openDirectoryInShell() const {
    std::string dirStr = getEffectiveDirectory();
    std::filesystem::path dir = toFsPath(dirStr);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open \"" + dirStr + "\" >/dev/null 2>&1 &";
    int rc = std::system(cmd.c_str());
    (void)rc;
#endif
}

int AutosaveManager::clearAllAutosaves() {
    pollFinishedWrite();
    if (m_writing.load()) return 0;

    std::filesystem::path dir = toFsPath(getEffectiveDirectory());
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        m_pendingRestore = {};
        m_lastSaveTimeStr.clear();
        m_lastStatusMessage = "All autosaves cleared";
        return 0;
    }

    int removedCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string name = fromFsPath(entry.path().filename());
        bool isAutosaveFile = isAutosaveFilename(name, true);
        if (isAutosaveFile || name == "autosave_meta.ini" || name == "clean_exit") {
            std::error_code rmEc;
            if (std::filesystem::remove(entry.path(), rmEc)) {
                ++removedCount;
            }
        }
    }

    m_pendingRestore = {};
    m_lastSaveTimeStr.clear();
    m_lastStatusMessage = "All autosaves cleared";
    sculpt_log("[Autosave] Cleared %d autosave file(s) from '%s'\n", removedCount, fromFsPath(dir).c_str());
    return removedCount;
}

float AutosaveManager::getSecondsUntilNextAutosave() const {
    float intervalSec = static_cast<float>(m_intervalMinutes * 60);
    return std::max(0.0f, intervalSec - m_elapsedSeconds);
}

void AutosaveManager::initOnStartup() {
    m_pendingRestore = {};
    m_lastSavedRevision = g_undoManager.getRevision();
    m_savedAtCurrentRevision = false;
    m_prevSceneModified = false;
    m_elapsedSeconds = 0.0f;

    std::filesystem::path dir = toFsPath(getEffectiveDirectory());
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return;
    }

    std::filesystem::path metaPath = dir / "autosave_meta.ini";
    IniFile meta;
    bool hasMeta = meta.load(fromFsPath(metaPath));

    std::string latestFileStr = hasMeta ? meta.get("Meta", "latestFile", "") : "";
    std::filesystem::path slot1 = latestFileStr.empty() ? (dir / makeSlotFilename("autosave", 1)) : toFsPath(latestFileStr);
    if (!std::filesystem::exists(slot1, ec) || !std::filesystem::is_regular_file(slot1, ec)) {
        return;
    }

    if (hasMeta) {
        uint64_t ownerPid = static_cast<uint64_t>(std::strtoull(meta.get("Meta", "pid", "0").c_str(), nullptr, 10));
        if (isProcessAlive(ownerPid)) {
            sculpt_log("[Autosave] Autosave directory is currently owned by live PID %llu, skipping restore prompt.\n",
                       static_cast<unsigned long long>(ownerPid));
            return;
        }
    }

    std::filesystem::path cleanExitPath = dir / "clean_exit";
    bool cleanExitExists = std::filesystem::exists(cleanExitPath, ec);
    if (cleanExitExists) {
        auto cleanTime = std::filesystem::last_write_time(cleanExitPath, ec);
        std::error_code slotEc;
        auto slotTime = std::filesystem::last_write_time(slot1, slotEc);
        if (!ec && !slotEc && cleanTime >= slotTime) {
            // Normal exit occurred after the last autosave
            return;
        }
    }

    PendingAutosaveRestore restore;
    restore.available = true;
    restore.filePath = fromFsPath(slot1);

    std::string slotPrefix = hasMeta ? meta.get("Meta", "slotPrefix", "autosave") : "autosave";
    std::filesystem::path slot2 = dir / makeSlotFilename(slotPrefix, 2);
    if (std::filesystem::exists(slot2, ec) && std::filesystem::is_regular_file(slot2, ec)) {
        restore.fallbackFilePath = fromFsPath(slot2);
    }

    if (hasMeta) {
        restore.originalScenePath = meta.get("Meta", "originalScenePath", "");
        restore.wasUnsaved = meta.getBool("Meta", "wasUnsaved", restore.originalScenePath.empty());
        restore.hasUndoHistory = meta.getBool("Meta", "hasUndoHistory", false);
        restore.savedTimeStr = meta.get("Meta", "savedTimeStr", "");
        restore.restoreAttempts = meta.getInt("Meta", "restoreAttempts", 0);
    }

    m_pendingRestore = restore;
    sculpt_log("[Autosave] Pending autosave recovery found: %s (attempts=%d)\n",
               restore.filePath.c_str(), restore.restoreAttempts);
}

void AutosaveManager::markRestoreAttempt(const std::string& filePath) {
    std::filesystem::path dir = toFsPath(getEffectiveDirectory());
    std::filesystem::path metaPath = dir / "autosave_meta.ini";
    IniFile meta;
    meta.load(fromFsPath(metaPath));
    int attempts = meta.getInt("Meta", "restoreAttempts", 0) + 1;
    meta.setInt("Meta", "restoreAttempts", attempts);
    meta.set("Meta", "lastRestoreAttemptFile", filePath);
    meta.save(fromFsPath(metaPath));
}

void AutosaveManager::markRestoreSuccess() {
    m_pendingRestore.available = false;
    std::filesystem::path dir = toFsPath(getEffectiveDirectory());
    std::filesystem::path metaPath = dir / "autosave_meta.ini";
    IniFile meta;
    if (meta.load(fromFsPath(metaPath))) {
        meta.setInt("Meta", "restoreAttempts", 0);
        meta.save(fromFsPath(metaPath));
    }
    m_lastSavedRevision = g_undoManager.getRevision();
    m_savedAtCurrentRevision = true;
    m_prevSceneModified = true;
    m_elapsedSeconds = 0.0f;
}

void AutosaveManager::discardPendingRestore() {
    m_pendingRestore.available = false;
    std::filesystem::path dir = toFsPath(getEffectiveDirectory());
    std::filesystem::path metaPath = dir / "autosave_meta.ini";
    IniFile meta;
    if (meta.load(fromFsPath(metaPath))) {
        meta.setInt("Meta", "restoreAttempts", 0);
        meta.save(fromFsPath(metaPath));
    }
    writeCleanExitMarker();
}

void AutosaveManager::writeCleanExitMarker() const {
    std::filesystem::path dir = toFsPath(getEffectiveDirectory());
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return;
    }
    std::filesystem::path cleanExitPath = dir / "clean_exit";
    FileUtils::writeTextFileAtomic(fromFsPath(cleanExitPath), formatCurrentTimeStr() + "\n");
}

void AutosaveManager::shutdown() {
    if (m_writeFuture.valid()) {
        m_writeFuture.wait();
        pollFinishedWrite();
    }
    writeCleanExitMarker();
}

void AutosaveManager::onManualSave(const std::string& /*savedPath*/) {
    m_lastSavedRevision = g_undoManager.getRevision();
    m_savedAtCurrentRevision = true;
    m_prevSceneModified = false;
    m_elapsedSeconds = 0.0f;
    writeCleanExitMarker();
}

void AutosaveManager::onSceneResetOrOpened() {
    m_lastSavedRevision = g_undoManager.getRevision();
    m_savedAtCurrentRevision = false;
    m_prevSceneModified = false;
    m_elapsedSeconds = 0.0f;
}

bool AutosaveManager::pollFinishedWrite() {
    if (!m_writeFuture.valid()) return false;
    if (m_writeFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return false;
    }

    bool ok = m_writeFuture.get();
    m_writing.store(false);
    m_hasWriteNotification = true;
    m_lastWriteSuccess = ok;
    if (ok) {
        m_lastSaveTimeStr = m_pendingSaveTimeStr;
        m_lastStatusMessage = "Last autosave: " + m_lastSaveTimeStr;
    } else {
        m_lastStatusMessage = "Autosave failed (see log)";
    }
    return true;
}

bool AutosaveManager::consumeWriteCompleted(bool& outSuccess) {
    if (!m_hasWriteNotification) return false;
    m_hasWriteNotification = false;
    outSuccess = m_lastWriteSuccess;
    return true;
}

bool AutosaveManager::startAsyncWrite(std::vector<uint8_t>&& buffer,
                                      const std::string& currentScenePath,
                                      bool hasUndoHistory) {
    if (buffer.empty() || m_writing.load()) {
        return false;
    }

    const std::string dirStr = getEffectiveDirectory();
    const int maxCopies = m_maxCopies;
    const std::string timeStr = formatCurrentTimeStr();
    const uint64_t pid = getCurrentPid();
    const bool wasUnsaved = currentScenePath.empty();
    const std::string origPath = currentScenePath;
    const std::string slotPrefix = makeSlotPrefix(origPath);

    m_pendingSaveTimeStr = timeStr;
    m_writing.store(true);
    m_lastStatusMessage = "Autosaving...";

    m_writeFuture = std::async(std::launch::async,
        [buf = std::move(buffer), dirStr, maxCopies, timeStr, pid, wasUnsaved, origPath, slotPrefix, hasUndoHistory]() mutable -> bool {
            std::filesystem::path dir = toFsPath(dirStr);
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec) {
                sculpt_log_error("[Autosave] Failed to create directory '%s': %s\n", dirStr.c_str(), ec.message().c_str());
                return false;
            }

            // Write new snapshot to a staging temp file first so existing slots stay intact on failure
            std::filesystem::path stagingPath = dir / (slotPrefix + "_staging.spsculpt.tmp");
            if (!FileUtils::writeBinaryFileAtomic(fromFsPath(stagingPath), buf)) {
                sculpt_log_error("[Autosave] Failed to write staging file\n");
                return false;
            }

            // Rotate existing slots for this project/scene: (K-1 -> K) ... (1 -> 2)
            for (int i = maxCopies; i >= 2; --i) {
                std::filesystem::path src = dir / makeSlotFilename(slotPrefix, i - 1);
                std::filesystem::path dst = dir / makeSlotFilename(slotPrefix, i);
                if (std::filesystem::exists(src, ec)) {
                    std::filesystem::rename(src, dst, ec);
                    ec.clear();
                }
            }

            // Move staging into slot 1
            std::filesystem::path slot1 = dir / makeSlotFilename(slotPrefix, 1);
            std::filesystem::rename(stagingPath, slot1, ec);
            if (ec) {
                sculpt_log_error("[Autosave] Failed to finalize slot 1: %s\n", ec.message().c_str());
                std::filesystem::remove(stagingPath, ec);
                return false;
            }

            // Remove clean_exit marker if present (or leave older than slot1)
            std::filesystem::path cleanExitPath = dir / "clean_exit";
            if (std::filesystem::exists(cleanExitPath, ec)) {
                std::filesystem::remove(cleanExitPath, ec);
            }

            // Update metadata
            IniFile meta;
            meta.set("Meta", "latestFile", fromFsPath(slot1));
            meta.set("Meta", "slotPrefix", slotPrefix);
            meta.set("Meta", "originalScenePath", origPath);
            meta.setBool("Meta", "wasUnsaved", wasUnsaved);
            meta.set("Meta", "savedTimeStr", timeStr);
            meta.setBool("Meta", "hasUndoHistory", hasUndoHistory);
            meta.set("Meta", "pid", std::to_string(pid));
            meta.setInt("Meta", "restoreAttempts", 0);
            meta.save(fromFsPath(dir / "autosave_meta.ini"));

            sculpt_log("[Autosave] Saved %zu bytes to %s\n", buf.size(), fromFsPath(slot1).c_str());
            return true;
        });

    return true;
}

bool AutosaveManager::triggerAutosaveNow(const Scene& scene,
                                         const AngleRenderer& renderer,
                                         const SculptManager& sculpt,
                                         uint64_t workTimeSeconds,
                                         const std::string& currentScenePath) {
    pollFinishedWrite();
    if (m_writing.load()) return false;

    const auto& meshes = scene.getMeshes();
    if (meshes.empty()) return false;

    bool includeHistory = m_saveUndoHistory &&
        (!g_undoManager.getUndoStack().empty() || !g_undoManager.getRedoStack().empty());

    std::vector<uint8_t> buffer = ExportSGL::exportSGL(
        meshes, scene, renderer, sculpt, {}, workTimeSeconds, m_saveUndoHistory
    );
    if (buffer.empty()) return false;

    m_elapsedSeconds = 0.0f;
    m_lastSavedRevision = g_undoManager.getRevision();
    m_savedAtCurrentRevision = true;
    return startAsyncWrite(std::move(buffer), currentScenePath, includeHistory);
}

bool AutosaveManager::tick(float dtSeconds,
                           const Scene& scene,
                           const AngleRenderer& renderer,
                           const SculptManager& sculpt,
                           uint64_t workTimeSeconds,
                           const std::string& currentScenePath,
                           bool isBusy) {
    bool changed = pollFinishedWrite();

    if (!m_enabled) {
        return changed;
    }

    const bool isModified = scene.isModified();
    if (isModified && !m_prevSceneModified) {
        m_savedAtCurrentRevision = false;
    }
    m_prevSceneModified = isModified;

    if (!isModified) {
        m_elapsedSeconds = 0.0f;
        return changed;
    }

    const uint64_t rev = g_undoManager.getRevision();
    if (rev != m_lastSavedRevision) {
        m_savedAtCurrentRevision = false;
    }

    if (m_savedAtCurrentRevision) {
        m_elapsedSeconds = 0.0f;
        return changed;
    }

    m_elapsedSeconds += dtSeconds;
    const float intervalSec = static_cast<float>(m_intervalMinutes * 60);
    if (m_elapsedSeconds >= intervalSec && !isBusy && !m_writing.load()) {
        if (triggerAutosaveNow(scene, renderer, sculpt, workTimeSeconds, currentScenePath)) {
            return true;
        }
    }

    return changed;
}

