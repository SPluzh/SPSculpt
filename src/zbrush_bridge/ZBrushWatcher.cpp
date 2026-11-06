#include "ZBrushWatcher.h"
#include <SDL2/SDL.h>
#include "ZBrushBridgeConfig.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace ZBrushBridge {

namespace fs = std::filesystem;

ZBrushWatcher::ZBrushWatcher() {
    m_watchDir = Config::getDataDir();
#ifdef _WIN32
    m_stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
#endif
}

ZBrushWatcher::~ZBrushWatcher() {
    stop();
#ifdef _WIN32
    if (m_stopEvent) {
        CloseHandle(static_cast<HANDLE>(m_stopEvent));
        m_stopEvent = nullptr;
    }
#endif
}

void ZBrushWatcher::setWatchDirectory(const std::string& dir) {
    bool wasRunning = m_running.load();
    if (wasRunning) {
        stop();
    }
    {
        std::lock_guard<std::mutex> lock(m_dirMutex);
        m_watchDir = dir;
    }
    if (wasRunning) {
        start();
    }
}

std::string ZBrushWatcher::getWatchDirectory() const {
    std::lock_guard<std::mutex> lock(m_dirMutex);
    return m_watchDir;
}

void ZBrushWatcher::start() {
    if (m_running.load()) return;

    std::string dir = getWatchDirectory();
    try {
        if (!fs::exists(dir)) {
            fs::create_directories(dir);
        }
    } catch (...) {
        m_state.store(WatcherState::Error);
    }

    m_stopRequested.store(false);
#ifdef _WIN32
    if (m_stopEvent) {
        ResetEvent(static_cast<HANDLE>(m_stopEvent));
    }
#endif

    m_running.store(true);
    m_state.store(WatcherState::Watching);
    m_thread = std::thread(&ZBrushWatcher::watchThreadFunc, this);
}

void ZBrushWatcher::stop() {
    if (!m_running.load()) {
        m_state.store(WatcherState::Paused);
        return;
    }

    m_stopRequested.store(true);
#ifdef _WIN32
    if (m_stopEvent) {
        SetEvent(static_cast<HANDLE>(m_stopEvent));
    }
#endif

    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_running.store(false);
    m_state.store(WatcherState::Paused);
}

bool ZBrushWatcher::consumePendingMesh(std::vector<uint8_t>& outBuffer, WatcherMeshStats* outStats) {
    if (!m_meshPending.load()) return false;
    std::lock_guard<std::mutex> lock(m_meshMutex);
    if (m_pendingMeshBuffer.empty()) return false;
    outBuffer = std::move(m_pendingMeshBuffer);
    m_pendingMeshBuffer.clear();
    m_pendingMeshBuffer.shrink_to_fit();
    if (outStats) {
        *outStats = m_pendingMeshStats;
    }
    m_meshPending.store(false);
    return true;
}

bool ZBrushWatcher::consumePendingScene(SceneSnapshot& outScene) {
    if (!m_scenePending.load()) return false;
    std::lock_guard<std::mutex> lock(m_sceneMutex);
    if (m_pendingScene.subtools.empty()) return false;
    outScene = std::move(m_pendingScene);
    m_pendingScene.subtools.clear();
    m_pendingScene.subtools.shrink_to_fit();
    m_scenePending.store(false);
    return true;
}

bool ZBrushWatcher::consumePendingCamera(CameraSnapshot& outCamera) {
    if (!m_cameraPending.load()) return false;
    std::lock_guard<std::mutex> lock(m_cameraMutex);
    if (!m_pendingCamera.valid) return false;
    outCamera = m_pendingCamera;
    m_pendingCamera.valid = false;
    m_cameraPending.store(false);
    return true;
}

bool ZBrushWatcher::isFileReady(const std::string& filePath, int maxRetries, int delayMs) {
#ifdef _WIN32
    std::wstring wpath(filePath.begin(), filePath.end());
    LONGLONG lastSize = -1;
    int stableCount = 0;

    for (int i = 0; i < maxRetries; ++i) {
        if (m_stopRequested.load()) return false;

        HANDLE hFile = CreateFileW(
            wpath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );

        if (hFile == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            if (err == ERROR_ACCESS_DENIED) {
                hFile = CreateFileW(
                    wpath.c_str(),
                    GENERIC_READ,
                    0,
                    NULL,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    NULL
                );
            }
        }

        if (hFile != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size;
            if (GetFileSizeEx(hFile, &size) && size.QuadPart >= 10) {
                if (lastSize > 0 && size.QuadPart == lastSize) {
                    stableCount++;
                    if (stableCount >= 2) {
                        CloseHandle(hFile);
                        return true;
                    }
                } else {
                    lastSize = size.QuadPart;
                    stableCount = 1;
                }
            } else {
                lastSize = -1;
                stableCount = 0;
            }
            CloseHandle(hFile);
        } else {
            lastSize = -1;
            stableCount = 0;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }
    return false;
#else
    uintmax_t lastSize = 0;
    int stableCount = 0;
    for (int i = 0; i < maxRetries; ++i) {
        if (m_stopRequested.load()) return false;
        if (fs::exists(filePath)) {
            std::error_code ec;
            auto sz = fs::file_size(filePath, ec);
            if (!ec && sz >= 10) {
                if (lastSize > 0 && sz == lastSize) {
                    stableCount++;
                    if (stableCount >= 2) return true;
                } else {
                    lastSize = sz;
                    stableCount = 1;
                }
            } else {
                lastSize = 0;
                stableCount = 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }
    return false;
#endif
}

void ZBrushWatcher::watchThreadFunc() {
#ifdef _WIN32
    std::string currentDir = getWatchDirectory();
    for (char& c : currentDir) {
        if (c == '/') c = '\\';
    }
    while (currentDir.length() > 3 && currentDir.back() == '\\') {
        currentDir.pop_back();
    }
    std::wstring wdir(currentDir.begin(), currentDir.end());

    HANDLE hDir = CreateFileW(
        wdir.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        NULL
    );

    if (hDir == INVALID_HANDLE_VALUE) {
        m_state.store(WatcherState::Error);
        while (!m_stopRequested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return;
    }

    OVERLAPPED ov = {0};
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    alignas(DWORD) BYTE notifyBuffer[4096];
    HANDLE waitHandles[2] = { static_cast<HANDLE>(m_stopEvent), ov.hEvent };
    FILETIME lastMeshTime = {0, 0};
    FILETIME lastManifestTime = {0, 0};

    while (!m_stopRequested.load()) {
        ResetEvent(ov.hEvent);
        DWORD bytesReturned = 0;

        BOOL success = ReadDirectoryChangesW(
            hDir,
            notifyBuffer,
            sizeof(notifyBuffer),
            FALSE, // no subtree
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
            &bytesReturned,
            &ov,
            NULL
        );

        if (!success) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (waitRes == WAIT_OBJECT_0) {
            CancelIo(hDir);
            break;
        }

        if (waitRes == WAIT_OBJECT_0 + 1) {
            DWORD transferred = 0;
            if (GetOverlappedResult(hDir, &ov, &transferred, FALSE) && transferred > 0) {
                BYTE* pCurrent = notifyBuffer;
                bool checkMesh = false;
                bool checkScene = false;
                bool checkCamera = false;

                while (pCurrent) {
                    auto* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(pCurrent);
                    std::wstring fileName(fni->FileName, fni->FileNameLength / sizeof(WCHAR));

                    std::string narrowName;
                    narrowName.reserve(fileName.size());
                    for (WCHAR c : fileName) {
                        narrowName.push_back(static_cast<char>(std::tolower(static_cast<char>(c))));
                    }

                    if (narrowName == "mesh.goz") {
                        checkMesh = true;
                    } else if (narrowName == "scene_manifest.json") {
                        checkScene = true;
                    } else if (narrowName == "camera.txt") {
                        checkCamera = true;
                    }

                    if (fni->NextEntryOffset == 0) break;
                    pCurrent += fni->NextEntryOffset;
                }

                // Handle Full Scene Sync Manifest
                if (checkScene) {
                    m_state.store(WatcherState::Importing);
                    int debounceMs = Config::getSceneDebounceDelayMs();
                    if (debounceMs > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(debounceMs));
                    }
                    auto tDetect = std::chrono::high_resolution_clock::now();
                    std::string manifestPath = getWatchDirectory() + "scene_manifest.json";
                    int retries = Config::getFileReadyRetries();
                    auto tWaitStart = std::chrono::high_resolution_clock::now();
                    bool ready = isFileReady(manifestPath, retries, 15);
                    auto tWaitEnd = std::chrono::high_resolution_clock::now();
                    double waitMs = std::chrono::duration<double, std::milli>(tWaitEnd - tWaitStart).count();

                    if (ready) {
                        std::wstring wManifestPath(manifestPath.begin(), manifestPath.end());
                        WIN32_FILE_ATTRIBUTE_DATA attrData;
                        bool isNew = true;
                        if (GetFileAttributesExW(wManifestPath.c_str(), GetFileExInfoStandard, &attrData)) {
                            if (lastManifestTime.dwLowDateTime == attrData.ftLastWriteTime.dwLowDateTime &&
                                lastManifestTime.dwHighDateTime == attrData.ftLastWriteTime.dwHighDateTime) {
                                isNew = false;
                            } else {
                                lastManifestTime = attrData.ftLastWriteTime;
                            }
                        }

                        if (isNew) {
                            std::ifstream f(manifestPath);
                            if (f.is_open()) {
                                try {
                                    nlohmann::json j;
                                    f >> j;
                                    f.close();

                                    SceneSnapshot sceneSnap;
                                    sceneSnap.version = j.value("version", 1);
                                    sceneSnap.activeIndex = j.value("activeIndex", 0);
                                    sceneSnap.timestamp = j.value("timestamp", 0ULL);
                                    sceneSnap.stats.filePath = manifestPath;
                                    sceneSnap.stats.detectTime = tDetect;
                                    sceneSnap.stats.fileReadyWaitMs = waitMs;

                                    if (j.contains("subtools") && j["subtools"].is_array()) {
                                        std::string baseDir = getWatchDirectory();
                                        for (const auto& item : j["subtools"]) {
                                            SubToolSnapshot st;
                                            st.id = item.value("id", 0);
                                            st.name = item.value("name", "");
                                            st.visible = item.value("visible", true);
                                            st.undoCounter = item.value("undoCounter", 0);
                                            std::string relFile = item.value("file", "");
                                            fs::path p(relFile);
                                            if (p.is_relative()) {
                                                st.filePath = (fs::path(baseDir) / p).string();
                                            } else {
                                                st.filePath = relFile;
                                            }

                                            // Check if subtool needs to be read
                                            auto cacheIt = m_subtoolUndoCache.find(st.name);
                                            bool needsRead = (cacheIt == m_subtoolUndoCache.end() || cacheIt->second != st.undoCounter);

                                            if (needsRead && fs::exists(st.filePath)) {
                                                if (isFileReady(st.filePath, 15, 10)) {
                                                    std::ifstream subFile(st.filePath, std::ios::binary | std::ios::ate);
                                                    if (subFile.is_open()) {
                                                        std::streamsize sz = subFile.tellg();
                                                        if (sz >= 32) {
                                                            subFile.seekg(0, std::ios::beg);
                                                            st.buffer.resize(sz);
                                                            subFile.read(reinterpret_cast<char*>(st.buffer.data()), sz);
                                                            st.isNewOrModified = true;
                                                            m_subtoolUndoCache[st.name] = st.undoCounter;
                                                        }
                                                    }
                                                }
                                            }
                                            sceneSnap.subtools.push_back(std::move(st));
                                        }
                                    }

                                    sceneSnap.stats.pushTime = std::chrono::high_resolution_clock::now();

                                    {
                                        std::lock_guard<std::mutex> lock(m_sceneMutex);
                                        m_pendingScene = std::move(sceneSnap);
                                        m_scenePending.store(true);
                                    }

                                    SDL_Event ev;
                                    std::memset(&ev, 0, sizeof(ev));
                                    ev.type = SDL_USEREVENT;
                                    ev.user.code = 0x676F73; // 'gos'
                                    SDL_PushEvent(&ev);

                                } catch (...) {}
                            }
                        }
                    }
                    m_state.store(WatcherState::Watching);
                }

                // Handle Single Active Mesh Sync
                if (checkMesh) {
                    m_state.store(WatcherState::Importing);
                    int debounceMs = Config::getDebounceDelayMs();
                    if (debounceMs > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(debounceMs));
                    }
                    auto tDetect = std::chrono::high_resolution_clock::now();
                    std::string meshPath = getWatchDirectory() + "mesh.GoZ";
                    auto tWaitStart = std::chrono::high_resolution_clock::now();
                    int retries = Config::getFileReadyRetries();
                    bool ready = isFileReady(meshPath, retries, 15);
                    auto tWaitEnd = std::chrono::high_resolution_clock::now();
                    double waitMs = std::chrono::duration<double, std::milli>(tWaitEnd - tWaitStart).count();

                    if (ready) {
                        std::wstring wMeshPath(meshPath.begin(), meshPath.end());
                        WIN32_FILE_ATTRIBUTE_DATA attrData;
                        bool isNew = true;
                        if (GetFileAttributesExW(wMeshPath.c_str(), GetFileExInfoStandard, &attrData)) {
                            if (lastMeshTime.dwLowDateTime == attrData.ftLastWriteTime.dwLowDateTime &&
                                lastMeshTime.dwHighDateTime == attrData.ftLastWriteTime.dwHighDateTime) {
                                isNew = false;
                            } else {
                                lastMeshTime = attrData.ftLastWriteTime;
                            }
                        }
                        if (isNew) {
                            auto tReadStart = std::chrono::high_resolution_clock::now();
                            std::ifstream file(meshPath, std::ios::binary | std::ios::ate);
                            if (file.is_open()) {
                                std::streamsize size = file.tellg();
                                if (size >= 32) {
                                    file.seekg(0, std::ios::beg);
                                    std::vector<uint8_t> buffer(size);
                                    if (file.read(reinterpret_cast<char*>(buffer.data()), size)) {
                                        auto tReadEnd = std::chrono::high_resolution_clock::now();
                                        double readMs = std::chrono::duration<double, std::milli>(tReadEnd - tReadStart).count();
                                        std::lock_guard<std::mutex> lock(m_meshMutex);
                                        m_pendingMeshBuffer = std::move(buffer);
                                        m_pendingMeshStats.filePath = meshPath;
                                        m_pendingMeshStats.fileSizeBytes = static_cast<size_t>(size);
                                        m_pendingMeshStats.fileReadyWaitMs = waitMs;
                                        m_pendingMeshStats.diskReadMs = readMs;
                                        m_pendingMeshStats.detectTime = tDetect;
                                        m_pendingMeshStats.pushTime = std::chrono::high_resolution_clock::now();
                                        m_meshPending.store(true);
                                        SDL_Event ev;
                                        std::memset(&ev, 0, sizeof(ev));
                                        ev.type = SDL_USEREVENT;
                                        ev.user.code = 0x676F7A; // 'goz'
                                        SDL_PushEvent(&ev);
                                    }
                                }
                            }
                        }
                    }
                    m_state.store(WatcherState::Watching);
                }

                if (checkCamera) {
                    std::string camPath = getWatchDirectory() + "camera.txt";
                    if (isFileReady(camPath, 10, 10)) {
                        std::ifstream file(camPath);
                        if (file.is_open()) {
                            std::string line;
                            if (std::getline(file, line) && !line.empty()) {
                                std::stringstream ss(line);
                                CameraSnapshot snap;
                                char comma;
                                if (ss >> snap.xP >> comma >> snap.yP >> comma >> snap.zP >> comma
                                       >> snap.xR >> comma >> snap.yR >> comma >> snap.zR >> comma
                                       >> snap.sc) {
                                    snap.valid = true;
                                    std::lock_guard<std::mutex> lock(m_cameraMutex);
                                    m_pendingCamera = snap;
                                    m_cameraPending.store(true);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    CloseHandle(ov.hEvent);
    CloseHandle(hDir);
#else
    // Non-Windows polling fallback
    fs::file_time_type lastMeshTime{};
    fs::file_time_type lastManifestTime{};
    while (!m_stopRequested.load()) {
        std::string manifestPath = getWatchDirectory() + "scene_manifest.json";
        if (fs::exists(manifestPath)) {
            auto curTime = fs::last_write_time(manifestPath);
            if (curTime != lastManifestTime) {
                lastManifestTime = curTime;
                forceReloadScene();
            }
        }

        std::string meshPath = getWatchDirectory() + "mesh.GoZ";
        if (fs::exists(meshPath)) {
            auto curTime = fs::last_write_time(meshPath);
            if (curTime != lastMeshTime) {
                lastMeshTime = curTime;
                forceReloadMesh();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
}

bool ZBrushWatcher::forceReloadMesh() {
    std::string meshPath = getWatchDirectory() + "mesh.GoZ";
    std::error_code ec;
    if (!fs::exists(meshPath, ec)) {
        return false;
    }

    WatcherState prevState = m_state.load();
    m_state.store(WatcherState::Importing);

    int retries = Config::getFileReadyRetries();
    auto tWaitStart = std::chrono::high_resolution_clock::now();
    bool ready = isFileReady(meshPath, retries, 15);
    auto tWaitEnd = std::chrono::high_resolution_clock::now();
    double waitMs = std::chrono::duration<double, std::milli>(tWaitEnd - tWaitStart).count();

    if (!ready) {
        m_state.store(prevState);
        return false;
    }

    auto tReadStart = std::chrono::high_resolution_clock::now();
    std::ifstream file(meshPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        m_state.store(prevState);
        return false;
    }

    std::streamsize size = file.tellg();
    if (size < 32) {
        m_state.store(prevState);
        return false;
    }

    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        m_state.store(prevState);
        return false;
    }

    auto tReadEnd = std::chrono::high_resolution_clock::now();
    double readMs = std::chrono::duration<double, std::milli>(tReadEnd - tReadStart).count();

    {
        std::lock_guard<std::mutex> lock(m_meshMutex);
        m_pendingMeshBuffer = std::move(buffer);
        m_pendingMeshStats.filePath = meshPath;
        m_pendingMeshStats.fileSizeBytes = static_cast<size_t>(size);
        m_pendingMeshStats.fileReadyWaitMs = waitMs;
        m_pendingMeshStats.diskReadMs = readMs;
        m_pendingMeshStats.detectTime = tWaitStart;
        m_pendingMeshStats.pushTime = std::chrono::high_resolution_clock::now();
        m_meshPending.store(true);
    }

    SDL_Event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_USEREVENT;
    ev.user.code = 0x676F7A; // 'goz'
    SDL_PushEvent(&ev);

    m_state.store(m_running.load() ? WatcherState::Watching : WatcherState::Paused);
    return true;
}

bool ZBrushWatcher::forceReloadScene() {
    std::string manifestPath = getWatchDirectory() + "scene_manifest.json";
    std::error_code ec;
    if (!fs::exists(manifestPath, ec)) {
        return false;
    }

    WatcherState prevState = m_state.load();
    m_state.store(WatcherState::Importing);

    int retries = Config::getFileReadyRetries();
    if (!isFileReady(manifestPath, retries, 15)) {
        m_state.store(prevState);
        return false;
    }

    std::ifstream f(manifestPath);
    if (!f.is_open()) {
        m_state.store(prevState);
        return false;
    }

    try {
        nlohmann::json j;
        f >> j;
        f.close();

        SceneSnapshot sceneSnap;
        sceneSnap.version = j.value("version", 1);
        sceneSnap.activeIndex = j.value("activeIndex", 0);
        sceneSnap.timestamp = j.value("timestamp", 0ULL);
        sceneSnap.stats.filePath = manifestPath;
        sceneSnap.stats.detectTime = std::chrono::high_resolution_clock::now();

        if (j.contains("subtools") && j["subtools"].is_array()) {
            std::string baseDir = getWatchDirectory();
            for (const auto& item : j["subtools"]) {
                SubToolSnapshot st;
                st.id = item.value("id", 0);
                st.name = item.value("name", "");
                st.visible = item.value("visible", true);
                st.undoCounter = item.value("undoCounter", 0);
                std::string relFile = item.value("file", "");
                fs::path p(relFile);
                if (p.is_relative()) {
                    st.filePath = (fs::path(baseDir) / p).string();
                } else {
                    st.filePath = relFile;
                }

                if (fs::exists(st.filePath) && isFileReady(st.filePath, 10, 10)) {
                    std::ifstream subFile(st.filePath, std::ios::binary | std::ios::ate);
                    if (subFile.is_open()) {
                        std::streamsize sz = subFile.tellg();
                        if (sz >= 32) {
                            subFile.seekg(0, std::ios::beg);
                            st.buffer.resize(sz);
                            subFile.read(reinterpret_cast<char*>(st.buffer.data()), sz);
                            st.isNewOrModified = true;
                            m_subtoolUndoCache[st.name] = st.undoCounter;
                        }
                    }
                }
                sceneSnap.subtools.push_back(std::move(st));
            }
        }

        sceneSnap.stats.pushTime = std::chrono::high_resolution_clock::now();

        {
            std::lock_guard<std::mutex> lock(m_sceneMutex);
            m_pendingScene = std::move(sceneSnap);
            m_scenePending.store(true);
        }

        SDL_Event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.type = SDL_USEREVENT;
        ev.user.code = 0x676F73; // 'gos'
        SDL_PushEvent(&ev);

    } catch (...) {
        m_state.store(prevState);
        return false;
    }

    m_state.store(m_running.load() ? WatcherState::Watching : WatcherState::Paused);
    return true;
}

} // namespace ZBrushBridge
