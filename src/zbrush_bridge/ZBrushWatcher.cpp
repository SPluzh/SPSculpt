#include "ZBrushWatcher.h"
#include <SDL2/SDL.h>
#include "ZBrushBridgeConfig.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

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

        // Try exclusive open (dwShareMode = 0) to ensure writer has completed and closed the file
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
            if (GetFileSizeEx(hFile, &size) && size.QuadPart >= 32) {
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
            if (!ec && sz >= 32) {
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
            // Stop requested
            CancelIo(hDir);
            break;
        }

        if (waitRes == WAIT_OBJECT_0 + 1) {
            DWORD transferred = 0;
            if (GetOverlappedResult(hDir, &ov, &transferred, FALSE) && transferred > 0) {
                BYTE* pCurrent = notifyBuffer;
                bool checkMesh = false;
                bool checkCamera = false;

                while (pCurrent) {
                    auto* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(pCurrent);
                    std::wstring fileName(fni->FileName, fni->FileNameLength / sizeof(WCHAR));

                    // Lowercase conversion for case-insensitive check
                    std::string narrowName;
                    narrowName.reserve(fileName.size());
                    for (WCHAR c : fileName) {
                        narrowName.push_back(static_cast<char>(std::tolower(static_cast<char>(c))));
                    }

                    if (narrowName == "mesh.goz") {
                        checkMesh = true;
                    } else if (narrowName == "camera.txt") {
                        checkCamera = true;
                    }

                    if (fni->NextEntryOffset == 0) break;
                    pCurrent += fni->NextEntryOffset;
                }

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
                            // Read binary GoZ into buffer
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
    while (!m_stopRequested.load()) {
        std::string meshPath = getWatchDirectory() + "mesh.GoZ";
        if (fs::exists(meshPath)) {
            auto curTime = fs::last_write_time(meshPath);
            if (curTime != lastMeshTime) {
                lastMeshTime = curTime;
                if (isFileReady(meshPath)) {
                    std::ifstream file(meshPath, std::ios::binary | std::ios::ate);
                    if (file.is_open()) {
                        std::streamsize size = file.tellg();
                        if (size >= 32) {
                            file.seekg(0, std::ios::beg);
                            std::vector<uint8_t> buffer(size);
                            if (file.read(reinterpret_cast<char*>(buffer.data()), size)) {
                                std::lock_guard<std::mutex> lock(m_meshMutex);
                                m_pendingMeshBuffer = std::move(buffer);
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

} // namespace ZBrushBridge
