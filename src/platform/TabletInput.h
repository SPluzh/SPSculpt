#pragma once
#include <atomic>
#include <vector>
#include <string>
#include <chrono>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include "TabletInputWinTab.h"
#endif

enum class TabletMode { NONE, WININK, WINTAB, LINUX_EVDEV };

class TabletInput {
public:
    TabletInput();
    ~TabletInput();

#ifdef _WIN32
    // --- WinTab ---
    bool wintabLoad();              // LoadLibrary("Wintab32.dll")
    bool wintabOpen(HWND hwnd);     // WTOpen
    void wintabClose();

    // --- Windows Ink ---
    void onWinInkUpdate(float pressure, float tiltX, float tiltY, bool inContact);
    void onWinInkUp();
    void setWinInkAvailable(bool available);
#endif

    void startPolling();
    void stopPolling();

    // --- Active Mode override & controls ---
    void setForcedMode(TabletMode mode);
    TabletMode getForcedMode() const;
    TabletMode getActiveMode() const;

    // --- General Interface ---
    float getPressure() const;
    float getPressureRaw() const;
    float getTiltX() const;
    float getTiltY() const;
    bool  isPenDown() const;
    bool  isAvailable() const;

    // --- Pressure Curve ---
    struct CurvePoint {
        float x;
        float y;
    };
    std::vector<CurvePoint> getPressureCurve() const { return _pressureCurve; }
    void setPressureCurve(const std::vector<CurvePoint>& points) { _pressureCurve = points; }
    float evaluateCurve(float x) const;
    std::string getPressureCurveString() const;
    void setPressureCurveFromString(const std::string& str);
    enum class InterpolationType {
        LINEAR = 0,
        MONOTONE_SPLINE = 1,
        CATMULL_ROM = 2
    };
    InterpolationType getInterpolationType() const { return _interpolationType.load(); }
    void setInterpolationType(InterpolationType type) { _interpolationType.store(type); }
    bool isSplineEnabled() const { return _interpolationType.load() != InterpolationType::LINEAR; }
    void setSplineEnabled(bool enable) { _interpolationType.store(enable ? InterpolationType::MONOTONE_SPLINE : InterpolationType::LINEAR); }

    // Toggles for Sculpting
    bool isPressureEnabled() const { return _usePressure.load(); }
    void setPressureEnabled(bool enable) { _usePressure.store(enable); }
    bool isPressureSizeEnabled() const { return _usePressureSize.load(); }
    void setPressureSizeEnabled(bool enable) { _usePressureSize.store(enable); }
    bool isPressureCursorEnabled() const { return _usePressureCursor.load(); }
    void setPressureCursorEnabled(bool enable) { _usePressureCursor.store(enable); }
    bool isTiltEnabled() const { return _useTilt.load(); }
    void setTiltEnabled(bool enable) { _useTilt.store(enable); }

    bool isPenActive() const {
        auto now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t last = _lastPenTime.load();
        if (last == 0) return false;
        return (now - last) < 2000;
    }

    // Diagnostics
    struct DiagInfo {
        bool wintabLoaded = false;
        bool wintabContextOpen = false;
        bool winInkAvailable = false;
        int  maxPressure = 1023;
        int  packetsLastSecond = 0;
        float currentPressure = 1.0f;
        float currentTiltX = 0.0f;
        float currentTiltY = 0.0f;
        bool isPenDown = false;
        TabletMode activeMode = TabletMode::NONE;
        bool isPenActive = false;
        std::string deviceName;
    };
    DiagInfo getDiagInfo() const;

private:
    void pollLoop();

    std::atomic<bool>     _running{false};
    uint32_t              _maxPressure = 1023;
    std::atomic<float>    _pressure{1.0f};
    std::atomic<float>    _pressureRaw{1.0f};
    std::atomic<float>    _tiltX{0.0f};
    std::atomic<float>    _tiltY{0.0f};
    std::atomic<bool>     _penDown{false};
    std::atomic<int>      _packetsPerSec{0};
    std::atomic<uint64_t> _lastPenTime{0};

#ifdef _WIN32
    // WinTab
    HMODULE _hLib    = nullptr;
    HCTX    _hCtx    = nullptr;
    HWND    _hHelper = nullptr;

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

    std::atomic<int>   _packetCounter{0};
    std::atomic<float> _inkPressure{1.0f};
    std::atomic<float> _inkTiltX{0.0f};
    std::atomic<float> _inkTiltY{0.0f};
    std::atomic<bool>  _inkActive{false};

    std::atomic<bool> _winInkAvailable{false};
    std::atomic<bool> _wintabAvailable{false};
#else
    std::atomic<bool> _evdevAvailable{false};
    std::string       _deviceName;
#endif

    TabletMode _forcedMode = TabletMode::NONE;

    std::atomic<bool> _usePressure{true};
    std::atomic<bool> _usePressureSize{false};
    std::atomic<bool> _usePressureCursor{false};
    std::atomic<bool> _useTilt{true};
    std::atomic<InterpolationType> _interpolationType{InterpolationType::LINEAR};
    std::vector<CurvePoint> _pressureCurve = {{0.0f, 0.0f}, {1.0f, 1.0f}};
};

extern TabletInput g_tablet;
