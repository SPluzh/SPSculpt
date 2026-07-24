#ifdef _WIN32
#include "TabletInput.h"
#include <thread>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <algorithm>

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

float TabletInput::getPressureRaw() const {
    TabletMode mode = getActiveMode();
    if (mode == TabletMode::WINTAB) {
        return _pressure.load();
    } else if (mode == TabletMode::WININK) {
        return _inkPressure.load();
    }
    return 1.0f;
}

float TabletInput::getPressure() const {
    if (!_usePressure) return 1.0f;
    return evaluateCurve(getPressureRaw());
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

float TabletInput::evaluateCurve(float x) const {
    if (_pressureCurve.empty()) return x;
    x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    if (x <= _pressureCurve.front().x) return _pressureCurve.front().y;
    if (x >= _pressureCurve.back().x) return _pressureCurve.back().y;

    size_t n = _pressureCurve.size();
    if (n < 2) return x;

    // Find the segment containing x
    size_t idx = 0;
    for (size_t i = 0; i < n - 1; ++i) {
        if (x >= _pressureCurve[i].x && x <= _pressureCurve[i + 1].x) {
            idx = i;
            break;
        }
    }

    InterpolationType type = _interpolationType.load();

    if (type == InterpolationType::LINEAR) {
        // Piecewise linear interpolation
        const auto& p1 = _pressureCurve[idx];
        const auto& p2 = _pressureCurve[idx + 1];
        if (p2.x - p1.x < 1e-5f) return p1.y;
        float t = (x - p1.x) / (p2.x - p1.x);
        return p1.y + t * (p2.y - p1.y);
    } else if (type == InterpolationType::MONOTONE_SPLINE) {
        // Monotone Cubic Hermite Spline interpolation (Fritsch-Carlson algorithm)
        // 1. Calculate secant slopes m_i
        std::vector<float> m(n - 1, 0.0f);
        for (size_t i = 0; i < n - 1; ++i) {
            float dx = _pressureCurve[i + 1].x - _pressureCurve[i].x;
            m[i] = dx > 1e-5f ? (_pressureCurve[i + 1].y - _pressureCurve[i].y) / dx : 0.0f;
        }

        // 2. Initialize tangents d_i as average of secants
        std::vector<float> d(n, 0.0f);
        for (size_t i = 1; i < n - 1; ++i) {
            d[i] = 0.5f * (m[i - 1] + m[i]);
        }
        d[0] = m[0];
        d[n - 1] = m[n - 2];

        // 3. Apply monotonicity constraints
        for (size_t i = 0; i < n - 1; ++i) {
            if (std::abs(m[i]) < 1e-5f) {
                d[i] = 0.0f;
                d[i + 1] = 0.0f;
            } else {
                float a = d[i] / m[i];
                float b = d[i + 1] / m[i];
                float h = a * a + b * b;
                if (h > 9.0f) {
                    float t_factor = 3.0f / std::sqrt(h);
                    d[i] = t_factor * a * m[i];
                    d[i + 1] = t_factor * b * m[i];
                }
            }
        }

        // 4. Evaluate spline for the selected segment
        const auto& p1 = _pressureCurve[idx];
        const auto& p2 = _pressureCurve[idx + 1];
        float h_x = p2.x - p1.x;
        if (h_x < 1e-5f) return p1.y;
        
        float t = (x - p1.x) / h_x;
        float t2 = t * t;
        float t3 = t2 * t;

        // Hermite basis functions
        float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
        float h10 = t3 - 2.0f * t2 + t;
        float h01 = -2.0f * t3 + 3.0f * t2;
        float h11 = t3 - t2;

        float val = h00 * p1.y + h10 * h_x * d[idx] + h01 * p2.y + h11 * h_x * d[idx + 1];
        return val < 0.0f ? 0.0f : (val > 1.0f ? 1.0f : val);
    } else if (type == InterpolationType::CATMULL_ROM) {
        // Centripetal Catmull-Rom Spline
        // Interpolate over segment idx (between P_idx and P_{idx+1})
        auto evaluateSegment = [&](int i, float t) -> CurvePoint {
            CurvePoint cp1 = _pressureCurve[i];
            CurvePoint cp2 = _pressureCurve[i + 1];
            CurvePoint cp0 = (i > 0) ? _pressureCurve[i - 1] : CurvePoint{ 2.0f * cp1.x - cp2.x, 2.0f * cp1.y - cp2.y };
            CurvePoint cp3 = (i < (int)n - 2) ? _pressureCurve[i + 2] : CurvePoint{ 2.0f * cp2.x - cp1.x, 2.0f * cp2.y - cp1.y };

            auto getT = [&](float tPrev, const CurvePoint& pA, const CurvePoint& pB) -> float {
                float dx = pB.x - pA.x;
                float dy = pB.y - pA.y;
                float dist = std::sqrt(dx * dx + dy * dy);
                return tPrev + std::sqrt(dist); // alpha = 0.5 (centripetal)
            };

            float t0 = 0.0f;
            float t1 = getT(t0, cp0, cp1);
            float t2 = getT(t1, cp1, cp2);
            float t3 = getT(t2, cp2, cp3);

            // Parameter value on the interval [t1, t2]
            float valT = t1 + t * (t2 - t1);

            auto interpolate = [&](const CurvePoint& pA, const CurvePoint& pB, float tA, float tB, float currT) -> CurvePoint {
                if (std::abs(tB - tA) < 1e-5f) return pA;
                float f = (currT - tA) / (tB - tA);
                return { pA.x + f * (pB.x - pA.x), pA.y + f * (pB.y - pA.y) };
            };

            CurvePoint a1 = interpolate(cp0, cp1, t0, t1, valT);
            CurvePoint a2 = interpolate(cp1, cp2, t1, t2, valT);
            CurvePoint a3 = interpolate(cp2, cp3, t2, t3, valT);

            CurvePoint b1 = interpolate(a1, a2, t0, t2, valT);
            CurvePoint b2 = interpolate(a2, a3, t1, t3, valT);

            return interpolate(b1, b2, t1, t2, valT);
        };

        // Binary search parameter t in [0, 1] to match input x
        float low = 0.0f;
        float high = 1.0f;
        float resY = _pressureCurve[idx].y;
        for (int iter = 0; iter < 16; ++iter) {
            float mid = 0.5f * (low + high);
            CurvePoint pt = evaluateSegment(idx, mid);
            resY = pt.y;
            if (pt.x < x) {
                low = mid;
            } else {
                high = mid;
            }
        }
        return resY < 0.0f ? 0.0f : (resY > 1.0f ? 1.0f : resY);
    }
    return x;
}

std::string TabletInput::getPressureCurveString() const {
    std::string s;
    for (size_t i = 0; i < _pressureCurve.size(); ++i) {
        if (i > 0) s += ";";
        s += std::to_string(_pressureCurve[i].x) + "," + std::to_string(_pressureCurve[i].y);
    }
    return s;
}

void TabletInput::setPressureCurveFromString(const std::string& str) {
    if (str.empty()) return;
    std::vector<CurvePoint> pts;
    size_t start = 0;
    while (start < str.size()) {
        size_t end = str.find(';', start);
        std::string pair = str.substr(start, end == std::string::npos ? std::string::npos : end - start);
        size_t comma = pair.find(',');
        if (comma != std::string::npos) {
            try {
                float x = std::stof(pair.substr(0, comma));
                float y = std::stof(pair.substr(comma + 1));
                pts.push_back({x, y});
            } catch (...) {
                // Ignore parse errors
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (pts.size() >= 2) {
        std::sort(pts.begin(), pts.end(), [](const CurvePoint& a, const CurvePoint& b) {
            return a.x < b.x;
        });
        _pressureCurve = pts;
    }
}

TabletInput g_tablet;

#endif // _WIN32
