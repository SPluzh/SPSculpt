#include "TabletInput.h"
#include <thread>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <sstream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef _WIN32
// Define package format
#define PACKETDATA (PK_X | PK_Y | PK_BUTTONS | PK_NORMAL_PRESSURE | PK_ORIENTATION)
#define PACKETMODE 0
#include "TabletInputPktDef.h"

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
#else
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <cstring>

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#define test_bit(bit, array) (((array[(bit)/BITS_PER_LONG]) >> ((bit)%BITS_PER_LONG)) & 1UL)

static int findTabletDevice(std::string& outName, int& outMaxPressure) {
    DIR* dir = opendir("/dev/input");
    if (!dir) return -1;
    struct dirent* entry;
    int bestFd = -1;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        std::string devPath = std::string("/dev/input/") + entry->d_name;
        int fd = open(devPath.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        char name[256] = "Unknown";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        
        unsigned long evBits[NBITS(EV_MAX)] = {0};
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), evBits) >= 0) {
            bool hasKey = test_bit(EV_KEY, evBits);
            bool hasAbs = test_bit(EV_ABS, evBits);
            if (hasKey && hasAbs) {
                unsigned long keyBits[NBITS(KEY_MAX)] = {0};
                ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits);
                bool hasPen = test_bit(BTN_TOOL_PEN, keyBits);
                bool hasTouch = test_bit(BTN_TOUCH, keyBits);
                
                unsigned long absBits[NBITS(ABS_MAX)] = {0};
                ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits);
                bool hasPressure = test_bit(ABS_PRESSURE, absBits);
                
                if (hasPressure && hasPen) {
                    struct input_absinfo absInfo{};
                    int maxP = 2047;
                    if (ioctl(fd, EVIOCGABS(ABS_PRESSURE), &absInfo) >= 0) {
                        maxP = absInfo.maximum > 0 ? absInfo.maximum : 2047;
                    }
                    outName = name;
                    outMaxPressure = maxP;
                    bestFd = fd;
                    break;
                }
            }
        }
        close(fd);
    }
    closedir(dir);
    return bestFd;
}
#endif

TabletInput::TabletInput() {
    _pressure.store(1.0f);
    _pressureRaw.store(1.0f);
    _tiltX.store(0.0f);
    _tiltY.store(0.0f);
    _penDown.store(false);
    _packetsPerSec.store(0);
    _lastPenTime.store(0);
    _usePressureSize.store(true);
    _usePressureCursor.store(true);
}

TabletInput::~TabletInput() {
    stopPolling();
#ifdef _WIN32
    wintabClose();
#endif
}

#ifdef _WIN32
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

    if (!_WTInfo(0, 0, nullptr)) {
        printf("[TabletInput] WTInfo(0, 0, nullptr) returned 0.\n");
        FreeLibrary(_hLib); _hLib = nullptr;
        return false;
    }

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
    if (!_WTOpen) return false;

    _hHelper = CreateHelperWindow();
    LOGCONTEXTA lc = {};
    if (_WTInfo(WTI_DEFSYSCTX, 0, &lc) == 0) {
        if (_WTInfo(WTI_DEFCONTEXT, 0, &lc) == 0) {
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
        _wintabAvailable = true;
    } else if (_hHelper) {
        DestroyWindow(_hHelper);
        _hHelper = nullptr;
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

void TabletInput::onWinInkUpdate(float pressure, float tiltX, float tiltY, bool inContact) {
    auto nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    _lastPenTime.store(nowMs);
    _inkPressure.store(pressure);
    _inkTiltX.store(tiltX);
    _inkTiltY.store(tiltY);
    _inkActive.store(inContact);
    _winInkAvailable.store(true);
}

void TabletInput::onWinInkUp() {
    _inkActive.store(false);
    _inkPressure.store(0.0f);
}

void TabletInput::setWinInkAvailable(bool available) {
    _winInkAvailable.store(available);
}
#endif

void TabletInput::startPolling() {
    if (_running) return;
    _running = true;
    std::thread([this]() { pollLoop(); }).detach();
}

void TabletInput::stopPolling() {
    _running = false;
}

void TabletInput::pollLoop() {
#ifdef _WIN32
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
                auto nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                _lastPenTime.store(nowMs);
                localCounter += got;
                auto& p = pkts[got - 1];
                
                float rawPressure = (float)p.pkNormalPressure / (float)_maxPressure;
                _pressureRaw.store(rawPressure);
                _pressure.store(evaluateCurve(rawPressure));
                
                double az = (double)p.pkOrientation.orAzimuth / 10.0 * (M_PI / 180.0);
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
#else
    // Linux evdev poll loop
    int fd = -1;
    std::string devName;
    int maxP = 2047;
    int packetCount = 0;
    auto lastRateTime = std::chrono::steady_clock::now();

    while (_running) {
        if (fd < 0) {
            fd = findTabletDevice(devName, maxP);
            if (fd >= 0) {
                _maxPressure = maxP;
                _deviceName = devName;
                _evdevAvailable.store(true);
                printf("[TabletInput] Linux evdev tablet connected: %s (max pressure: %d)\n", devName.c_str(), maxP);
            } else {
                _evdevAvailable.store(false);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
        }

        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 100);
        if (ret < 0) {
            if (errno == EINTR) continue;
            close(fd);
            fd = -1;
            _evdevAvailable.store(false);
            continue;
        }
        if (ret == 0) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastRateTime).count() >= 1) {
                _packetsPerSec.store(packetCount);
                packetCount = 0;
                lastRateTime = now;
            }
            continue;
        }

        struct input_event evs[64];
        ssize_t bytes = read(fd, evs, sizeof(evs));
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            close(fd);
            fd = -1;
            _evdevAvailable.store(false);
            continue;
        } else if (bytes == 0) {
            continue;
        }

        size_t count = bytes / sizeof(struct input_event);
        auto nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        for (size_t i = 0; i < count; ++i) {
            const auto& ev = evs[i];
            if (ev.type == EV_KEY) {
                if (ev.code == BTN_TOUCH) {
                    bool down = (ev.value != 0);
                    _penDown.store(down);
                    if (!down) {
                        _pressureRaw.store(0.0f);
                        _pressure.store(0.0f);
                    }
                    _lastPenTime.store(nowMs);
                    packetCount++;
                } else if (ev.code == BTN_TOOL_PEN || ev.code == BTN_TOOL_RUBBER) {
                    _lastPenTime.store(nowMs);
                    packetCount++;
                }
            } else if (ev.type == EV_ABS) {
                if (ev.code == ABS_PRESSURE) {
                    float raw = (float)ev.value / (float)_maxPressure;
                    raw = std::clamp(raw, 0.0f, 1.0f);
                    _pressureRaw.store(raw);
                    _pressure.store(evaluateCurve(raw));
                    _penDown.store(ev.value > 0);
                    _lastPenTime.store(nowMs);
                    packetCount++;
                } else if (ev.code == ABS_TILT_X) {
                    // Normalize tilt angle in degrees (-90 to +90)
                    float tx = (float)ev.value * (90.0f / 64.0f);
                    _tiltX.store(tx);
                    _lastPenTime.store(nowMs);
                } else if (ev.code == ABS_TILT_Y) {
                    float ty = (float)ev.value * (90.0f / 64.0f);
                    _tiltY.store(ty);
                    _lastPenTime.store(nowMs);
                }
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastRateTime).count() >= 1) {
            _packetsPerSec.store(packetCount);
            packetCount = 0;
            lastRateTime = now;
        }
    }

    if (fd >= 0) {
        close(fd);
    }
#endif
}

void TabletInput::setForcedMode(TabletMode mode) {
    _forcedMode = mode;
}

TabletMode TabletInput::getForcedMode() const {
    return _forcedMode;
}

TabletMode TabletInput::getActiveMode() const {
#ifdef _WIN32
    if (_forcedMode == TabletMode::WININK && _winInkAvailable.load()) return TabletMode::WININK;
    if (_forcedMode == TabletMode::WINTAB && _wintabAvailable.load()) return TabletMode::WINTAB;
    if (_forcedMode == TabletMode::NONE) {
        if (_winInkAvailable.load()) return TabletMode::WININK;
        if (_wintabAvailable.load()) return TabletMode::WINTAB;
    }
    return TabletMode::NONE;
#else
    if (_evdevAvailable.load()) return TabletMode::LINUX_EVDEV;
    return TabletMode::NONE;
#endif
}

float TabletInput::getPressure() const {
    if (!_usePressure.load()) return 1.0f;
    return evaluateCurve(getPressureRaw());
}

float TabletInput::getPressureRaw() const {
    TabletMode mode = getActiveMode();
#ifdef _WIN32
    if (mode == TabletMode::WINTAB) return _pressureRaw.load();
    if (mode == TabletMode::WININK) return _inkPressure.load();
#else
    if (mode == TabletMode::LINUX_EVDEV) return _pressureRaw.load();
#endif
    return 1.0f;
}

float TabletInput::getTiltX() const {
    if (!_useTilt.load()) return 0.0f;
    TabletMode mode = getActiveMode();
#ifdef _WIN32
    if (mode == TabletMode::WINTAB) return _tiltX.load();
    if (mode == TabletMode::WININK) return _inkTiltX.load();
#else
    if (mode == TabletMode::LINUX_EVDEV) return _tiltX.load();
#endif
    return 0.0f;
}

float TabletInput::getTiltY() const {
    if (!_useTilt.load()) return 0.0f;
    TabletMode mode = getActiveMode();
#ifdef _WIN32
    if (mode == TabletMode::WINTAB) return _tiltY.load();
    if (mode == TabletMode::WININK) return _inkTiltY.load();
#else
    if (mode == TabletMode::LINUX_EVDEV) return _tiltY.load();
#endif
    return 0.0f;
}

bool TabletInput::isPenDown() const {
    TabletMode mode = getActiveMode();
#ifdef _WIN32
    if (mode == TabletMode::WINTAB) return _penDown.load();
    if (mode == TabletMode::WININK) return _inkActive.load();
#else
    if (mode == TabletMode::LINUX_EVDEV) return _penDown.load();
#endif
    return false;
}

bool TabletInput::isAvailable() const {
    return getActiveMode() != TabletMode::NONE;
}

TabletInput::DiagInfo TabletInput::getDiagInfo() const {
    DiagInfo di;
#ifdef _WIN32
    di.wintabLoaded = (_hLib != nullptr);
    di.wintabContextOpen = (_hCtx != nullptr);
    di.winInkAvailable = _winInkAvailable.load();
    di.deviceName = "Wintab / Windows Ink";
#else
    di.wintabLoaded = false;
    di.wintabContextOpen = false;
    di.winInkAvailable = false;
    di.deviceName = _deviceName;
#endif
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

    size_t idx = 0;
    for (size_t i = 0; i < n - 1; ++i) {
        if (x >= _pressureCurve[i].x && x <= _pressureCurve[i + 1].x) {
            idx = i;
            break;
        }
    }

    InterpolationType type = _interpolationType.load();

    if (type == InterpolationType::LINEAR) {
        const auto& p1 = _pressureCurve[idx];
        const auto& p2 = _pressureCurve[idx + 1];
        if (p2.x - p1.x < 1e-5f) return p1.y;
        float t = (x - p1.x) / (p2.x - p1.x);
        return p1.y + t * (p2.y - p1.y);
    } else if (type == InterpolationType::MONOTONE_SPLINE) {
        std::vector<float> m(n - 1, 0.0f);
        for (size_t i = 0; i < n - 1; ++i) {
            float dx = _pressureCurve[i + 1].x - _pressureCurve[i].x;
            m[i] = dx > 1e-5f ? (_pressureCurve[i + 1].y - _pressureCurve[i].y) / dx : 0.0f;
        }

        std::vector<float> d(n, 0.0f);
        for (size_t i = 1; i < n - 1; ++i) {
            d[i] = 0.5f * (m[i - 1] + m[i]);
        }
        d[0] = m[0];
        d[n - 1] = m[n - 2];

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

        const auto& p1 = _pressureCurve[idx];
        const auto& p2 = _pressureCurve[idx + 1];
        float h_x = p2.x - p1.x;
        if (h_x < 1e-5f) return p1.y;
        
        float t = (x - p1.x) / h_x;
        float t2 = t * t;
        float t3 = t2 * t;

        float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
        float h10 = t3 - 2.0f * t2 + t;
        float h01 = -2.0f * t3 + 3.0f * t2;
        float h11 = t3 - t2;

        float val = h00 * p1.y + h10 * h_x * d[idx] + h01 * p2.y + h11 * h_x * d[idx + 1];
        return val < 0.0f ? 0.0f : (val > 1.0f ? 1.0f : val);
    } else if (type == InterpolationType::CATMULL_ROM) {
        auto evaluateSegment = [&](int i, float t) -> CurvePoint {
            CurvePoint cp1 = _pressureCurve[i];
            CurvePoint cp2 = _pressureCurve[i + 1];
            CurvePoint cp0 = (i > 0) ? _pressureCurve[i - 1] : CurvePoint{ 2.0f * cp1.x - cp2.x, 2.0f * cp1.y - cp2.y };
            CurvePoint cp3 = (i < (int)n - 2) ? _pressureCurve[i + 2] : CurvePoint{ 2.0f * cp2.x - cp1.x, 2.0f * cp2.y - cp1.y };

            auto getT = [&](float tPrev, const CurvePoint& pA, const CurvePoint& pB) -> float {
                float dx = pB.x - pA.x;
                float dy = pB.y - pA.y;
                float dist = std::sqrt(dx * dx + dy * dy);
                return tPrev + std::sqrt(dist);
            };

            float t0 = 0.0f;
            float t1 = getT(t0, cp0, cp1);
            float t2 = getT(t1, cp1, cp2);
            float t3 = getT(t2, cp2, cp3);

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

        const auto& p1 = _pressureCurve[idx];
        const auto& p2 = _pressureCurve[idx + 1];
        if (p2.x - p1.x < 1e-5f) return p1.y;
        float t = (x - p1.x) / (p2.x - p1.x);
        CurvePoint pt = evaluateSegment((int)idx, t);
        return pt.y < 0.0f ? 0.0f : (pt.y > 1.0f ? 1.0f : pt.y);
    }

    return x;
}

std::string TabletInput::getPressureCurveString() const {
    std::string s;
    for (size_t i = 0; i < _pressureCurve.size(); ++i) {
        if (i > 0) s += ";";
        char buf[64];
        snprintf(buf, sizeof(buf), "%.4f,%.4f", _pressureCurve[i].x, _pressureCurve[i].y);
        s += buf;
    }
    return s;
}

void TabletInput::setPressureCurveFromString(const std::string& str) {
    if (str.empty()) return;
    std::vector<CurvePoint> pts;
    size_t start = 0;
    while (start < str.size()) {
        size_t end = str.find(';', start);
        std::string token = (end == std::string::npos) ? str.substr(start) : str.substr(start, end - start);
        size_t comma = token.find(',');
        if (comma != std::string::npos) {
            try {
                float x = std::stof(token.substr(0, comma));
                float y = std::stof(token.substr(comma + 1));
                pts.push_back({x, y});
            } catch (...) {
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
