#ifdef _WIN32
#include "TabletInput.h"
#include <thread>
#include <chrono>
#include <cstdio>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Define package format
#define PACKETDATA (PK_X | PK_Y | PK_BUTTONS | PK_NORMAL_PRESSURE | PK_ORIENTATION)
#define PACKETMODE 0
#include "TabletInputPktDef.h"

// Helper window class / WndProc for Helper Window
static ATOM g_wndClass = 0;

static LRESULT CALLBACK WintabHelperWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND CreateHelperWindow() {
    if (!g_wndClass) {
        WNDCLASSA wc = {};
        wc.lpfnWndProc   = WintabHelperWndProc;
        wc.hInstance     = GetModuleHandleA(nullptr);
        wc.lpszClassName = "WintabHelperClass";
        g_wndClass = RegisterClassA(&wc);
    }
    return CreateWindowExA(
        0, "WintabHelperClass", "Wintab Helper",
        0, 0, 0, 0, 0,
        nullptr, nullptr, GetModuleHandleA(nullptr), nullptr
    );
}

TabletInput::TabletInput() {
    _pressure.store(1.0f);
    _tiltX.store(0.0f);
    _tiltY.store(0.0f);
    _penDown.store(false);
    _packetsPerSec.store(0);
    _lastPenTime.store(0);
    _usePressureSize.store(true);
    _usePressureCursor.store(true);
}

TabletInput::~TabletInput() {
    wintabClose();
}

bool TabletInput::wintabLoad() {
    printf("[TabletInput] Loading Wintab32.dll...\n");
    _hLib = LoadLibraryA("Wintab32.dll");
    if (!_hLib) {
        printf("[TabletInput] LoadLibraryA failed. GetLastError=%lu\n", GetLastError());
        return false;
    }

    _WTInfo       = (WTINFOA)      GetProcAddress(_hLib, "WTInfoA");
    _WTOpen       = (WTOPENA)      GetProcAddress(_hLib, "WTOpenA");
    _WTClose      = (WTCLOSE)      GetProcAddress(_hLib, "WTClose");
    _WTPacketsGet = (WTPACKETSGET) GetProcAddress(_hLib, "WTPacketsGet");
    _WTEnable     = (WTENABLE)     GetProcAddress(_hLib, "WTEnable");

    if (!_WTInfo || !_WTOpen || !_WTClose || !_WTPacketsGet) {
        printf("[TabletInput] GetProcAddress failed.\n");
        FreeLibrary(_hLib); _hLib = nullptr;
        return false;
    }

    // Check service
    if (!_WTInfo(0, 0, nullptr)) {
        printf("[TabletInput] WTInfo(0, 0, nullptr) returned 0.\n");
        FreeLibrary(_hLib); _hLib = nullptr;
        return false;
    }

    // Get max pressure
    AXIS pressureAxis = {};
    if (_WTInfo(WTI_DEVICES, DVC_NPRESSURE, &pressureAxis) > 0) {
        _maxPressure = pressureAxis.axMax > 0 ? pressureAxis.axMax : 1023;
    } else {
        _maxPressure = 1023;
    }

    printf("[TabletInput] Wintab loaded. Max pressure: %u\n", _maxPressure);
    return true;
}

bool TabletInput::wintabOpen(HWND hwnd) {
    if (!_WTOpen) {
        printf("[TabletInput] _WTOpen is NULL\n");
        return false;
    }

    _hHelper = CreateHelperWindow();
    if (!_hHelper) {
        printf("[TabletInput] CreateHelperWindow failed. GetLastError=%lu\n", GetLastError());
    } else {
        printf("[TabletInput] CreateHelperWindow succeeded. HWND = %p\n", _hHelper);
    }

    LOGCONTEXTA lc = {};
    if (_WTInfo(WTI_DEFSYSCTX, 0, &lc) == 0) {
        if (_WTInfo(WTI_DEFCONTEXT, 0, &lc) == 0) {
            printf("[TabletInput] WTInfo for context failed.\n");
            if (_hHelper) { DestroyWindow(_hHelper); _hHelper = nullptr; }
            return false;
        }
    }

    lc.lcPktData    = PACKETDATA;
    lc.lcPktMode    = PACKETMODE;
    lc.lcMoveMask   = PACKETDATA;
    lc.lcBtnUpMask  = lc.lcBtnDnMask;
    lc.lcOptions   |= CXO_MESSAGES;

    if (_hHelper) {
        _hCtx = _WTOpen(_hHelper, &lc, TRUE);
    }
    if (!_hCtx) {
        _hCtx = _WTOpen(hwnd, &lc, TRUE);
    }
    if (!_hCtx) {
        _hCtx = _WTOpen(nullptr, &lc, TRUE);
    }

    if (_hCtx) {
        printf("[TabletInput] WTOpen succeeded! hCtx = %p\n", _hCtx);
        _wintabAvailable = true;
    } else {
        printf("[TabletInput] WTOpen failed. GetLastError=%lu\n", GetLastError());
        if (_hHelper) {
            DestroyWindow(_hHelper);
            _hHelper = nullptr;
        }
    }

    return _hCtx != nullptr;
}

void TabletInput::wintabClose() {
    stopPolling();
    if (_hCtx && _WTClose)  { _WTClose(_hCtx); _hCtx = nullptr; }
    if (_hHelper) { DestroyWindow(_hHelper); _hHelper = nullptr; }
    if (_hLib)  { FreeLibrary(_hLib); _hLib = nullptr; }
    _wintabAvailable = false;
}

void TabletInput::startPolling() {
    if (_running) return;
    _running = true;
    std::thread([this]() { pollLoop(); }).detach();
}

void TabletInput::stopPolling() {
    _running = false;
}

void TabletInput::pollLoop() {
    auto lastFpsTime = std::chrono::steady_clock::now();
    int localCounter = 0;

    while (_running) {
        if (_hHelper) {
            MSG msg;
            while (PeekMessageA(&msg, _hHelper, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
        }

        if (_hCtx && _WTPacketsGet) {
            PACKET pkts[32];
            int got = _WTPacketsGet(_hCtx, 32, pkts);
            if (got > 0) {
                _lastPenTime.store(GetTickCount());
                localCounter += got;
                auto& p = pkts[got - 1];
                
                float rawPressure = (float)p.pkNormalPressure / (float)_maxPressure;
                _pressure.store(rawPressure);
                
                // azimuth is 0 to 3600 (tenths of degrees)
                double az = (double)p.pkOrientation.orAzimuth / 10.0 * (M_PI / 180.0);
                
                // altitude is 0 to 900 (tenths of degrees)
                double alt = (double)p.pkOrientation.orAltitude / 10.0;
                double tilt = 90.0 - alt;
                if (tilt < 0.0) tilt = 0.0;
                
                float tx = (float)(tilt * sin(az));
                float ty = (float)(tilt * cos(az));
                _tiltX.store(tx);
                _tiltY.store(ty);

                _penDown.store((p.pkButtons & 1) != 0);
            }
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastFpsTime).count();
        if (elapsed >= 1) {
            _packetsPerSec.store(localCounter / (int)elapsed);
            localCounter = 0;
            lastFpsTime = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void TabletInput::onWinInkUpdate(float pressure, float tiltX, float tiltY, bool inContact) {
    _lastPenTime.store(GetTickCount());
    _inkPressure.store(pressure);
    _inkTiltX.store(tiltX);
    _inkTiltY.store(tiltY);
    _inkActive.store(inContact);
    _winInkAvailable.store(true);
}

void TabletInput::onWinInkUp() {
    _inkPressure.store(0.0f);
    _inkActive.store(false);
}

void TabletInput::setWinInkAvailable(bool available) {
    _winInkAvailable.store(available);
}

void TabletInput::setForcedMode(TabletMode mode) {
    _forcedMode = mode;
    // If forced to WINTAB and context is active, enable it; if forced to Ink, disable Wintab
    if (_hCtx && _WTEnable) {
        if (mode == TabletMode::WINTAB || mode == TabletMode::NONE) {
            _WTEnable(_hCtx, TRUE);
        } else {
            _WTEnable(_hCtx, FALSE);
        }
    }
}

TabletMode TabletInput::getForcedMode() const {
    return _forcedMode;
}

TabletMode TabletInput::getActiveMode() const {
    if (_forcedMode != TabletMode::NONE) {
        return _forcedMode;
    }
    if (_wintabAvailable) {
        return TabletMode::WINTAB;
    }
    if (_winInkAvailable) {
        return TabletMode::WININK;
    }
    return TabletMode::NONE;
}

float TabletInput::getPressure() const {
    if (!_usePressure) return 1.0f;
    TabletMode mode = getActiveMode();
    if (mode == TabletMode::WINTAB) {
        return _pressure.load();
    } else if (mode == TabletMode::WININK) {
        return _inkPressure.load();
    }
    return 1.0f;
}

float TabletInput::getTiltX() const {
    if (!_useTilt) return 0.0f;
    TabletMode mode = getActiveMode();
    if (mode == TabletMode::WINTAB) {
        return _tiltX.load();
    } else if (mode == TabletMode::WININK) {
        return _inkTiltX.load();
    }
    return 0.0f;
}

float TabletInput::getTiltY() const {
    if (!_useTilt) return 0.0f;
    TabletMode mode = getActiveMode();
    if (mode == TabletMode::WINTAB) {
        return _tiltY.load();
    } else if (mode == TabletMode::WININK) {
        return _inkTiltY.load();
    }
    return 0.0f;
}

bool TabletInput::isPenDown() const {
    TabletMode mode = getActiveMode();
    if (mode == TabletMode::WINTAB) {
        return _penDown.load();
    } else if (mode == TabletMode::WININK) {
        return _inkActive.load();
    }
    return false;
}

bool TabletInput::isAvailable() const {
    return getActiveMode() != TabletMode::NONE;
}

TabletInput::DiagInfo TabletInput::getDiagInfo() const {
    DiagInfo di;
    di.wintabLoaded = (_hLib != nullptr);
    di.wintabContextOpen = (_hCtx != nullptr);
    di.winInkAvailable = _winInkAvailable.load();
    di.maxPressure = (int)_maxPressure;
    di.packetsLastSecond = _packetsPerSec.load();
    di.currentPressure = getPressure();
    di.currentTiltX = getTiltX();
    di.currentTiltY = getTiltY();
    di.isPenDown = isPenDown();
    di.activeMode = getActiveMode();
    di.isPenActive = isPenActive();
    return di;
}

TabletInput g_tablet;

#endif // _WIN32
