# Plan: Tablet Integration — WinTab + Windows Ink

## Текущее состояние

| Компонент | Статус |
|---|---|
| **Windows Ink** (WM_POINTER API) | ✅ Уже реализован в `NativeMain.cpp` (`TabletMessageHook`) |
| `SculptManager::setStylusPressure()` | ✅ Уже есть |
| `SculptManager::m_stylusPressure` | ✅ Уже есть |
| **WinTab** (Wintab32.dll) | ❌ Не подключён |
| **UI диагностики планшета** | ❌ Нет |
| Tilt X/Y из планшета | ❌ Не используется нигде |

---

## Что нужно сделать

### Фаза 1 — Копирование и адаптация WinTab из JS-проекта

**Файлы-источники** (уже готовы, из `javascript/sculptsp/native/wintab/`):
- `wintab.h` — SDK-заголовок (без изменений)
- `pktdef.h` — генератор структуры PACKET (без изменений)
- `wintab_context.h/.cpp` — обёртка с polling-потоком

**Целевая структура:**
```
src/platform/
    TabletInput.h          ← переработанный wintab_context.h
    TabletInput.cpp        ← переработанный wintab_context.cpp
    TabletInputWinTab.h    ← wintab.h (SDK-заголовок)
    TabletInputPktDef.h    ← pktdef.h
    NativeMain.cpp         ← уже есть, будет расширен
```

**Изменения по сравнению с JS-версией:**
1. Убрать Node.js / NAPI — только чистый C++
2. Переименовать класс `WintabContext` → `TabletInput`
3. Добавить поля `tiltX`, `tiltY` как `float` (в градусах)
4. Добавить метод `getTabletMode()` — возвращает `WINTAB` / `WININK` / `NONE`
5. Добавить `WTPacket` + `WTEnable` для приостановки при потере фокуса

---

### Фаза 2 — Создание класса `TabletInput`

**`src/platform/TabletInput.h`:**
```cpp
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <atomic>

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
    // Вызывается прямо из TabletMessageHook (уже существует в NativeMain.cpp)
    void onWinInkUpdate(float pressure);
    void onWinInkUp();

    // --- Общий интерфейс ---
    float getPressure() const;
    float getTiltX() const;
    float getTiltY() const;
    bool  isPenDown() const;
    bool  isAvailable() const;
    TabletMode getActiveMode() const;

    // Диагностика
    struct DiagInfo {
        bool wintabLoaded;
        bool wintabContextOpen;
        bool winInkAvailable;
        int  maxPressure;
        int  packetsLastSecond;
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
    void* _WTInfo      = nullptr;
    void* _WTOpen      = nullptr;
    void* _WTClose     = nullptr;
    void* _WTPacketsGet = nullptr;

    // Shared state (atomic для thread-safety)
    std::atomic<float> _pressure{1.0f};
    std::atomic<float> _tiltX{0.0f};
    std::atomic<float> _tiltY{0.0f};
    std::atomic<bool>  _penDown{false};
    std::atomic<int>   _packetsPerSec{0};

    // Windows Ink state (пишется в message hook, читается из рендер-потока)
    std::atomic<float> _inkPressure{1.0f};
    std::atomic<bool>  _inkActive{false};

    TabletMode _activeMode = TabletMode::NONE;
    bool _winInkAvailable = false;
};

#endif // _WIN32
```

---

### Фаза 3 — Интеграция `TabletInput` в `SculptManager`

**Изменения в `SculptManager.h`:**
```cpp
#include "platform/TabletInput.h"

class SculptManager {
    // ...
    // Уже есть:
    float m_stylusPressure = 1.0f;
    bool  m_usingStylus = false;
    uint32_t m_lastStylusTime = 0;
    
    // Добавить:
    float m_stylusTiltX = 0.0f;
    float m_stylusTiltY = 0.0f;
    
public:
    float getStylusTiltX() const { return m_stylusTiltX; }
    float getStylusTiltY() const { return m_stylusTiltY; }
    void  setStylusTilt(float tx, float ty) { m_stylusTiltX = tx; m_stylusTiltY = ty; }
};
```

**Изменения в `NativeMain.cpp`:**
```cpp
// Вместо raw Windows Ink hook — делегировать в TabletInput:
static TabletInput g_tablet;

static void SDLCALL TabletMessageHook(void* userdata, void* hWnd, ...) {
    SculptManager* sculpt = static_cast<SculptManager*>(userdata);
    // Windows Ink часть — как сейчас, но вызываем g_tablet.onWinInkUpdate(pressure)
    // WinTab читается через polling поток
}

// В main():
g_tablet.wintabLoad();
g_tablet.wintabOpen(hwndNative);
g_tablet.startPolling();

// В главном цикле:
if (g_tablet.isAvailable()) {
    sculpt.setStylusPressure(g_tablet.getPressure());
    sculpt.setStylusTilt(g_tablet.getTiltX(), g_tablet.getTiltY());
}
```

---

### Фаза 4 — Окно диагностики планшета (ImGui)

**Расположение:** `GuiManager::render()` — новый раздел в меню или отдельная панель.

**Что показывать:**

```
╔══════════════════════════════════════╗
║   🖊  Tablet Diagnostics              ║
╠══════════════════════════════════════╣
║ Mode:       [WINTAB ▼]               ║
║ Status:     ● Connected              ║
╠══════════════════════════════════════╣
║ Pressure:   ████░░░░  0.72           ║
║ Tilt X:     ◄──●──►  -12°           ║
║ Tilt Y:     ◄──●──►   8°            ║
║ Pen Down:   YES                      ║
╠══════════════════════════════════════╣
║ Wintab32.dll  ● Loaded               ║
║ Context       ● Open                 ║
║ Windows Ink   ● Available            ║
║ Max Pressure  1023                   ║
║ Packets/sec   ~980                   ║
╠══════════════════════════════════════╣
║ [  Live Pressure Test  ]             ║
║  Draw here to test sensitivity:      ║
║  ┌──────────────────────────────┐    ║
║  │ ██░░░░░░░░░░░░░░░░░░░░░░░░ │    ║
║  └──────────────────────────────┘    ║
╚══════════════════════════════════════╝
```

**Реализация ImGui:**
- `ImGui::ProgressBar(pressure, ...)` — визуализация давления в реальном времени
- `ImGui::PlotLines(...)` — история давления (ringbuffer 256 сэмплов)
- Цветовая индикация: зелёный = OK, красный = нет связи
- Checkbox "Use Pressure" — включает/выключает давление для скульпта
- Checkbox "Use Tilt" — включает/выключает наклон
- Кнопка "Force WinTab" / "Force WinInk" — переключение режима вручную

**Доступ к диагностике:** через меню `Help → Tablet Diagnostics` или кнопку в настройках

---

### Фаза 5 — CMakeLists.txt

Никаких внешних зависимостей не нужно — WinTab загружается динамически через `LoadLibrary`.

```cmake
# В конец CMakeLists.txt добавить:
if(WIN32)
    # WinTab - динамическая загрузка, не нужно линковать Wintab32.lib
    # Windows Ink - через user32.dll (уже линкуется автоматически)
    # Просто добавить исходники:
    target_sources(sculptsp PRIVATE
        src/platform/TabletInput.cpp
    )
endif()
```

---

### Фаза 6 — Приоритет источника давления и fallback

```
WinTab доступен? → используем WinTab (более точный, ~1000Hz)
    ↓ НЕТ
Windows Ink доступен? → используем WM_POINTER (≥Win8)
    ↓ НЕТ
Мышь → pressure = 1.0f (без планшета)
```

Логика в главном цикле:
```cpp
float finalPressure = 1.0f;
if (g_tablet.getActiveMode() == TabletMode::WINTAB) {
    finalPressure = g_tablet.getPressure();
} else if (g_tablet.getActiveMode() == TabletMode::WININK) {
    finalPressure = g_tablet.getPressure(); // из ink атомика
}
sculpt.setStylusPressure(finalPressure);
```

---

## Порядок реализации

| # | Задача | Файлы | Сложность |
|---|---|---|---|
| 1 | Скопировать и адаптировать `wintab.h`, `pktdef.h` | `src/platform/` | 🟢 Легко |
| 2 | Создать `TabletInput.h/.cpp` (из WintabContext) | `src/platform/` | 🟡 Средне |
| 3 | Рефакторинг `NativeMain.cpp` — добавить `g_tablet` | `src/platform/NativeMain.cpp` | 🟡 Средне |
| 4 | Добавить tilt в `SculptManager` | `src/editing/SculptManager.h/.cpp` | 🟢 Легко |
| 5 | Создать ImGui окно диагностики | `src/gui/GuiManager.cpp` | 🟡 Средне |
| 6 | Обновить `CMakeLists.txt` | `CMakeLists.txt` | 🟢 Легко |
| 7 | Тест компиляции и тест с планшетом | — | 🔴 Важно |

---

## Ключевые технические нюансы

> [!IMPORTANT]
> **WinTab требует живого HWND.** SDL2 скрывает нативный HWND — нужно получить его через:
> ```cpp
> SDL_SysWMinfo wmInfo;
> SDL_VERSION(&wmInfo.version);
> SDL_GetWindowWMInfo(window, &wmInfo);
> HWND hwnd = wmInfo.info.win.window;
> ```

> [!WARNING]
> **Thread safety:** WinTab polling работает в отдельном потоке. Все общие данные (pressure, tiltX/Y) должны быть `std::atomic`. Уже сделано в JS-версии — нужно сохранить.

> [!NOTE]
> **WinTab vs Windows Ink совместимость:** Некоторые драйверы Wacom блокируют WM_POINTER при активном WinTab. Нужен механизм явного выбора режима + детект конфликта.

> [!TIP]
> **Polling vs Messages:** JS-версия использует polling поток + `PeekMessage`. Для native C++ с SDL2 можно также использовать `SDL_SetWindowsMessageHook` для перехвата `WT_PACKET` сообщений вместо отдельного потока — это проще и надёжнее.
