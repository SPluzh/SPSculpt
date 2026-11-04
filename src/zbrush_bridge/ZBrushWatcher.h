#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ZBrushBridge {

struct CameraSnapshot {
    float xP = 0.0f, yP = 0.0f, zP = 0.0f;
    float xR = 0.0f, yR = 0.0f, zR = 0.0f;
    float sc = 1.0f;
    bool valid = false;
};

class ZBrushWatcher {
public:
    ZBrushWatcher();
    ~ZBrushWatcher();

    void start();
    void stop();
    bool isRunning() const { return m_running.load(); }

    bool hasPendingMesh() const { return m_meshPending.load(); }
    bool consumePendingMesh(std::vector<uint8_t>& outBuffer);

    bool hasPendingCamera() const { return m_cameraPending.load(); }
    bool consumePendingCamera(CameraSnapshot& outCamera);

    void setWatchDirectory(const std::string& dir);
    std::string getWatchDirectory() const;

private:
    void watchThreadFunc();
    bool isFileReady(const std::string& filePath, int maxRetries = 25, int delayMs = 15);

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_meshPending{false};
    std::atomic<bool> m_cameraPending{false};

    std::string m_watchDir;
    mutable std::mutex m_dirMutex;

    std::vector<uint8_t> m_pendingMeshBuffer;
    std::mutex m_meshMutex;

    CameraSnapshot m_pendingCamera;
    std::mutex m_cameraMutex;

    std::thread m_thread;

#ifdef _WIN32
    void* m_stopEvent = nullptr;
#endif
};

} // namespace ZBrushBridge
