#pragma once
#ifdef _WIN32
#include <windows.h>
#include <atomic>
#include "TabletInputWinTab.h"

enum class TabletMode { NONE, WININK, WINTAB };

class TabletInput {
public:
    TabletInput();
    ~TabletInput();

    // --- WinTab ---
    bool wintabLoad();              // LoadLibrary("Wintab32.dll")
    bool wintabOpen(HWND hwnd);     // WTOpen
    void wintabClose();
    void startPolling();
    void stopPolling();

    // --- Windows Ink ---
    void onWinInkUpdate(float pressure, float tiltX, float tiltY, bool inContact);
    void onWinInkUp();
    void setWinInkAvailable(bool available);

    // --- Active Mode override & controls ---
    void setForcedMode(TabletMode mode);
    TabletMode getForcedMode() const;
    TabletMode getActiveMode() const;

    // --- General Interface ---
    float getPressure() const;
    float getTiltX() const;
    float getTiltY() const;
    bool  isPenDown() const;
    bool  isAvailable() const;

    // Toggles for Sculpting
    bool isPressureEnabled() const { return _usePressure; }
    void setPressureEnabled(bool enable) { _usePressure = enable; }
    bool isTiltEnabled() const { return _useTilt; }
    void setTiltEnabled(bool enable) { _useTilt = enable; }

    bool isPenActive() const {
        return (GetTickCount() - _lastPenTime.load()) < 1000;
    }

    // Diagnostics
    struct DiagInfo {
        bool wintabLoaded;
        bool wintabContextOpen;
        bool winInkAvailable;
        int  maxPressure;
        int  packetsLastSecond;
        float currentPressure;
        float currentTiltX;
        float currentTiltY;
        bool isPenDown;
        TabletMode activeMode;
        bool isPenActive;
    };
    DiagInfo getDiagInfo() const;

private:
    void pollLoop();

    // WinTab
    HMODULE _hLib    = nullptr;
    HCTX    _hCtx    = nullptr;
    HWND    _hHelper = nullptr;
    bool    _running = false;
    UINT    _maxPressure = 1023;

    // WinTab function pointers
    typedef UINT (WINAPI *WTINFOA)(UINT, UINT, LPVOID);
    typedef HCTX (WINAPI *WTOPENA)(HWND, LPLOGCONTEXTA, BOOL);
    typedef BOOL (WINAPI *WTCLOSE)(HCTX);
    typedef int (WINAPI *WTPACKETSGET)(HCTX, int, LPVOID);
    typedef BOOL (WINAPI *WTENABLE)(HCTX, BOOL);

    WTINFOA      _WTInfo      = nullptr;
    WTOPENA      _WTOpen      = nullptr;
    WTCLOSE      _WTClose     = nullptr;
    WTPACKETSGET _WTPacketsGet = nullptr;
    WTENABLE     _WTEnable     = nullptr;

    // Thread/Metrics
    std::atomic<int>   _packetCounter{0};
    std::atomic<int>   _packetsPerSec{0};

    // Shared state (atomic for thread-safety)
    std::atomic<float> _pressure{1.0f};
    std::atomic<float> _tiltX{0.0f};
    std::atomic<float> _tiltY{0.0f};
    std::atomic<bool>  _penDown{false};

    // Windows Ink state (written in msg hook, read in render thread)
    std::atomic<float> _inkPressure{1.0f};
    std::atomic<float> _inkTiltX{0.0f};
    std::atomic<float> _inkTiltY{0.0f};
    std::atomic<bool>  _inkActive{false};

    TabletMode _forcedMode = TabletMode::NONE; // NONE = Auto/Default
    std::atomic<bool> _winInkAvailable{false};
    std::atomic<bool> _wintabAvailable{false};

    std::atomic<bool> _usePressure{true};
    std::atomic<bool> _useTilt{true};
    std::atomic<uint32_t> _lastPenTime{0};
};

extern TabletInput g_tablet;
#endif // _WIN32
