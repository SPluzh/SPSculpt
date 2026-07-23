# C++ план внедрения ZBrush кистей (JSON-пресеты)

## Анализ текущей архитектуры

### Что есть сейчас
| Файл | Роль |
|---|---|
| `common/Enums.h` | `BrushType` enum (22 типа, хардкод) |
| `editing/SculptManager.h` | `BrushSettings[22]` — один struct на все кисти |
| `sculpt/SculptEngine.h` | Свободные функции (`strokeFlatten`, `strokeSmooth`…) |
| `gui/GuiManager.cpp` | UI панели с хардкод параметрами |
| `ZBrushes/*.json` | 28 файлов — уже в репозитории, не используются |
| `CMakeLists.txt` | `nlohmann_json` уже подключён ✓ |

### Ключевые проблемы текущей архитектуры

1. **`BrushSettings` не расширяемый** — нет полей для `stroke`, `deformMode`, `smoothTaubin`, `grabRadius`, `depthFilter`, `flattenLockNormal` и т.д.
2. **`BrushType` enum = фиксированный список** — нет концепции «пресета».
3. **`SculptManager` хранит `BrushSettings[22]`** — нет места для динамических пресетов.
4. **`SculptEngine` не знает о `deformMode`** — логика `strokeFlatten` не учитывает `flattenLockNormal`, `onlyFrontFace`.
5. **Нет `BrushPresetManager`** — нет загрузки/сохранения/нормализации JSON.

---

## Нужны ли изменения архитектуры?

**Да, но минимальные — эволюционные, без сноса старого.**

Стратегия: **Parallel track** — добавить новый слой пресетов поверх существующего, не ломая ничего.

```
До:  SculptManager → BrushSettings[22] → SculptEngine::stroke*()
После: BrushPresetManager → BrushPreset → SculptManager → FullBrushParams → SculptEngine::stroke*()
```

---

## Новые файлы и структуры

### Этап 0 — BrushPreset + BrushPresetManager

#### `src/brushes/BrushPreset.h`

```cpp
#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

enum class StrokeMode { Dot, Roll, Grab, GrabDynamicRadius };
enum class DeformMode { Normal, Clay, Inflate, Pinch, Crease, Flatten, Smooth, Move };

struct FalloffCurve {
    std::string preset = "smoothstep"; // "smoothstep","linear","out_power_2"
    std::vector<std::array<float,2>> points;
};

struct DepthFilter {
    bool   enable  = false;
    bool   falloff = true;
    float  min     = 1.0f;
    float  max     = 1.0f;
    float  offset  = 0.0f;
};

struct BrushPreset {
    // Meta
    std::string name;
    std::string icon;
    std::string color;       // "#rrggbb"
    std::string uid;

    // Topology
    DeformMode  deformMode  = DeformMode::Normal;
    StrokeMode  strokeMode  = StrokeMode::Dot;

    // Base params
    float radius        = 35.0f;
    float intensity     = 1.0f;
    float spacing       = 0.08f;
    float hardness      = 0.75f;
    float focalShift    = 0.0f;
    bool  focalShiftFalloff = true;
    bool  negative      = false;
    bool  culling       = false;
    bool  accumulate    = false;
    bool  lockPosition  = false;
    bool  altmode       = false;
    int   idAlpha       = -1;

    // Lazy
    float lazyRadius    = 0.0f;
    float lazySmooth    = 0.0f;

    // Falloff
    FalloffCurve falloff;

    // Grab (Move / RoundEdge)
    bool  grabRadius      = false;
    float grabRadiusScale = 0.28f;

    // Area
    float areaNormalRadius = 0.4f;
    float areaPointRadius  = 0.0f;
    float areaSharp        = 0.0f;
    bool  areaSampling     = true;

    // Flatten
    bool  flattenLockNormal = false;
    bool  flattenLockOrigin = false;

    // Smooth — Taubin
    bool  smoothTaubin        = false;
    float smoothTaubinInflate = 0.53f;
    float smoothTaubinShrink  = 0.75f;
    bool  smoothRelax         = false;
    bool  smoothStable        = false;
    bool  smoothStickyBorder  = false;
    bool  tangent             = false;

    // Depth filter
    DepthFilter depthFilter;

    // Topology
    bool connectedTopology = false;
    bool onlyFrontFace     = false;
    bool topoCheck         = false;
    bool useDynamicTopology = false;
    float elasticity       = 1.5f;

    // Paint
    std::array<float,3> paintColor{1.0f, 0.766f, 0.336f};
    float roughness  = 0.3f;
    float metallic   = 0.0f;
    bool  writeAlbedo    = true;
    bool  writeRoughness = true;
    bool  writeMetalness = false;

    // Pressure
    bool  pressureIntensity   = true;
    bool  pressureRadius      = false;
    bool  useGlobalPressure   = false;

    // DynTopo
    float subdivFactor = 0.0f;
    float decimFactor  = 0.0f;
};

// Нормализация: depth_filter / depth_filter_enable → depthFilterEnable
BrushPreset normalizeBrushJSON(const nlohmann::json& raw);
BrushPreset loadBrushPresetFromFile(const std::string& path);
```

#### `src/brushes/BrushPresetManager.h`

```cpp
#pragma once
#include "brushes/BrushPreset.h"
#include <vector>
#include <string>
#include <optional>

class BrushPresetManager {
public:
    static BrushPresetManager& instance();

    // Загрузка
    void loadDefaults();                          // 28 встроенных пресетов
    bool loadFromFile(const std::string& path);   // один .json
    int  loadFromFolder(const std::string& dir);  // вся папка ZBrushes/

    // Доступ
    const std::vector<BrushPreset>& presets() const { return m_presets; }
    const BrushPreset* findByName(const std::string& name) const;
    const BrushPreset* findByUid(const std::string& uid) const;
    BrushPreset*       findByNameMut(const std::string& name);

    // Управление
    void addPreset(BrushPreset p);
    void removePreset(const std::string& uid);
    bool savePreset(const BrushPreset& p, const std::string& path) const;

    // Активный пресет
    void            setActive(const std::string& uid);
    const BrushPreset* active() const;
    BrushPreset*       activeMut();

private:
    BrushPresetManager() = default;
    std::vector<BrushPreset> m_presets;
    std::string              m_activeUid;
};
```

---

## Маппинг JSON → BrushPreset

### `normalizeBrushJSON()` логика

```cpp
BrushPreset normalizeBrushJSON(const nlohmann::json& j) {
    BrushPreset p;

    // --- depth_filter совместимость ---
    bool dfEnable = false;
    if (j.contains("depth_filter_enable"))
        dfEnable = j["depth_filter_enable"].get<bool>();
    else if (j.contains("depth_filter"))
        dfEnable = j["depth_filter"].get<bool>();
    p.depthFilter.enable = dfEnable;

    // --- type → deformMode ---
    static const std::unordered_map<std::string, DeformMode> typeMap = {
        {"brush",   DeformMode::Normal},
        {"crease",  DeformMode::Crease},
        {"flatten", DeformMode::Flatten},
        {"pinch",   DeformMode::Pinch},
        {"smooth",  DeformMode::Smooth},
        {"move",    DeformMode::Move},
    };
    // clay/inflate определяется по деталям json (smooth_taubin → Smooth и т.д.)

    // --- stroke ---
    std::string stroke = j.value("stroke", "dot");
    if      (stroke == "roll")               p.strokeMode = StrokeMode::Roll;
    else if (stroke == "grab")               p.strokeMode = StrokeMode::Grab;
    else if (stroke == "grab_dynamic_radius") p.strokeMode = StrokeMode::GrabDynamicRadius;

    // ... остальные поля по snake_case → camelCase
    return p;
}
```

---

## Изменения в существующих файлах

### `SculptManager.h` — минимальные изменения

```cpp
// Добавить:
#include "brushes/BrushPresetManager.h"

// В public:
void applyPreset(const BrushPreset& preset);   // копирует в BrushSettings
void applyActivePreset();

// Приватно:
static BrushSettings presetToSettings(const BrushPreset& p);
```

> `BrushSettings` расширяется новыми полями постепенно (не сносится).

### Новые поля в `BrushSettings`

```cpp
// Добавить в существующий BrushSettings:
StrokeMode  strokeMode  = StrokeMode::Dot;
DeformMode  deformMode  = DeformMode::Normal;
bool  altmode           = false;
float lazyRadius        = 0.0f;
float lazySmooth        = 0.0f;
bool  grabRadius        = false;
float grabRadiusScale   = 0.28f;
float areaNormalRadius  = 0.4f;
float areaPointRadius   = 0.0f;
float areaSharp         = 0.0f;
bool  areaSampling      = true;
bool  flattenLockNormal = false;
bool  flattenLockOrigin = false;
bool  smoothTaubin      = false;
float smoothTaubinInflate = 0.53f;
float smoothTaubinShrink  = 0.75f;
bool  smoothRelax       = false;
bool  smoothStable      = false;
bool  smoothStickyBorder = false;
DepthFilter depthFilter;
bool  connectedTopology = false;
bool  onlyFrontFace     = false;
float areaSampling      = true;
```

### `SculptEngine.h` — новые перегрузки

Не ломать старые функции. Добавить расширенные версии:

```cpp
// Этап 1 — flatten с новыми параметрами
int strokeFlattenEx(
    float* verts, const float* vertProxy, const float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float ax, float ay, float az,
    float anx, float any, float anz,
    float radius, float intensity,
    bool negative, bool accumulate, bool lockPosition,
    bool lockNormal, bool lockOrigin,    // NEW
    bool onlyFrontFace,                  // NEW
    const DepthFilterParams& df,         // NEW
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, const uint8_t* alphaTex,
    int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym
);

// Этап 2 — smooth Taubin
int strokeSmoothTaubin(
    float* verts, const float* normals, const float* materials,
    const uint32_t* vrvStartCount, const uint32_t* vertRingVert,
    const uint8_t* vertOnEdge,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float radius, float intensity,
    float taubinInflate, float taubinShrink, // NEW
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, const uint8_t* alphaTex,
    int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym
);

// Этап 3 — move с connectedTopology
int strokeMoveEx(
    float* verts, const float* vertProxy, const float* materials,
    uint32_t* iVerts, int nbIVerts,
    float cx, float cy, float cz,
    float dirx, float diry, float dirz,
    float radius, bool connectedTopology, // NEW
    float grabRadiusScale,               // NEW
    float focalShift, bool focalShiftFalloff,
    bool hasAlpha, const uint8_t* alphaTex,
    int alphaWidth, int alphaHeight,
    float alphaRatioX, float alphaRatioY, float alphaSide,
    const float* alphaLookAt, bool alphaXSym,
    bool useAccuCurve, const float* accuCurveLut
);
```

---

## Stroke режимы в `SculptManager::executeStroke()`

### Roll (HPolish)
```cpp
// В executeStroke(): если strokeMode == Roll
// Каждый кадр = полностью независимый strokeFlattenEx()
// НЕТ накопления intersectionDelta между кадрами
// areaCenter и areaNormal пересчитываются каждый тик
if (settings.strokeMode == StrokeMode::Roll) {
    m_firstStrokeFrame = true; // форсировать пересчёт
    // вызов strokeFlattenEx без lockPosition
}
```

### Grab / GrabDynamicRadius (Move, RoundEdge)
```cpp
// grabDynamic: radius определяется при mouseDown и фиксируется
if (settings.strokeMode == StrokeMode::GrabDynamicRadius) {
    if (m_firstStrokeFrame) {
        m_grabRadius = settings.radius * settings.grabRadiusScale;
    }
    localRadius = m_grabRadius;
}
```

---

## Taubin Smooth алгоритм (`SculptEngine.cpp`)

```cpp
// applyTaubinSmooth() — без усадки объёма
// Шаг 1: shrink  — v += λ * laplacian(v),  λ =  taubinShrink
// Шаг 2: inflate — v += μ * laplacian(v),  μ = -taubinInflate
//
// Результат: сглаживание без уменьшения объёма (в отличие от обычного Laplacian)
void applyTaubinSmooth(float* verts, const uint32_t* vrvStartCount,
                       const uint32_t* vertRingVert, const uint32_t* iVerts,
                       int nbIVerts, float lambda, float mu, float weight) {
    // pass 1: shrink
    for each vert i in iVerts:
        vec3 lap = laplacian(verts, vrvStartCount, vertRingVert, i);
        verts[i*3..] += lambda * weight * lap;
    // pass 2: inflate
    for each vert i in iVerts:
        vec3 lap = laplacian(verts, vrvStartCount, vertRingVert, i);
        verts[i*3..] += mu * weight * lap;
}
```

---

## GUI — панель пресетов (`GuiManager.cpp`)

### Новые элементы

```
┌─────────────────────────────────────────┐
│ BRUSH PRESETS                    [+ Load]│
│ Filter: [All▼] [Normal] [Clay] [Flatten] │
│ ┌───┐ ┌───┐ ┌───┐ ┌───┐ ┌───┐          │
│ │Dam│ │Std│ │Cly│ │HPo│ │Smh│  ← иконки│
│ └───┘ └───┘ └───┘ └───┘ └───┘          │
│ Active: DamStandard                      │
│ Stroke: dot  |  Mode: normal  |  r=35   │
└─────────────────────────────────────────┘
```

```cpp
// В GuiManager::renderBrushPresetsPanel():
void renderBrushPresetsPanel() {
    auto& mgr = BrushPresetManager::instance();
    // Горизонтальный scroll + фильтр по deformMode
    // При клике на иконку: mgr.setActive(uid); manager.applyActivePreset();
    // Кнопка "Import ZBrushes/": mgr.loadFromFolder("ZBrushes/");
    // Кнопка "Export .spbrush": mgr.savePreset(*mgr.active(), path);
}
```

---

## Этапный план внедрения (C++)

### Этап 0 — BrushPreset + BrushPresetManager (1 сессия)

| Задача | Файл |
|---|---|
| Создать `BrushPreset.h/.cpp` | `src/brushes/` |
| Создать `BrushPresetManager.h/.cpp` | `src/brushes/` |
| `normalizeBrushJSON()` — depth_filter совместимость | `BrushPreset.cpp` |
| `loadBrushPresetFromFile()` — читает ZBrushes/*.json | `BrushPreset.cpp` |
| `BrushPresetManager::loadFromFolder("ZBrushes/")` | `BrushPresetManager.cpp` |
| Добавить в `CMakeLists.txt`: `src/brushes/BrushPreset.cpp`, `BrushPresetManager.cpp` | `CMakeLists.txt` |
| Тест: загрузить все 28 json, вывести имена в лог | `NativeMain.cpp` |

**Изменения архитектуры:** только добавление новых файлов.

---

### Этап 1 — Расширение BrushSettings + applyPreset() (1 сессия)

| Задача | Файл |
|---|---|
| Добавить поля `StrokeMode`, `DeformMode`, `depthFilter`, `flattenLockNormal` и др. | `SculptManager.h` |
| `SculptManager::applyPreset(preset)` — маппинг preset → settings | `SculptManager.cpp` |
| `SculptManager::applyActivePreset()` | `SculptManager.cpp` |
| Roll stroke: в `executeStroke()` при `strokeMode==Roll` сбрасывать firstStrokeFrame | `SculptManager.cpp` |
| `strokeFlattenEx()` с `lockNormal`, `onlyFrontFace`, `depthFilter` | `SculptEngine.h/.cpp` |

**Изменения архитектуры:** расширение `BrushSettings` (обратно совместимо, defaults не меняют поведение).

---

### Этап 2 — Taubin Smooth + GrabDynamicRadius (1 сессия)

| Задача | Файл |
|---|---|
| `applyTaubinSmooth()` — helper function | `SculptEngine.cpp` |
| `strokeSmoothTaubin()` — публичная функция | `SculptEngine.h/.cpp` |
| Диспетчер в `SculptManager`: `smoothTaubin ? strokeSmoothTaubin : strokeSmooth` | `SculptManager.cpp` |
| `GrabDynamicRadius`: `m_grabRadius` фиксируется на mouseDown | `SculptManager.cpp` |
| Добавить `m_grabRadius` в приватные члены | `SculptManager.h` |

---

### Этап 3 — ProxyBrush (Grab stroke, connectedTopology) (1 сессия)

| Задача | Файл |
|---|---|
| `strokeMoveEx()` с `connectedTopology`, `grabRadiusScale` | `SculptEngine.h/.cpp` |
| Grab stroke — диспетч в `executeStroke()` | `SculptManager.cpp` |
| `connectedTopology`: flood-fill из hit-вершины, ограничить `iVerts` | `SculptEngine.cpp` |

---

### Этап 4 — GUI панель пресетов (1 сессия)

| Задача | Файл |
|---|---|
| `renderBrushPresetsPanel()` — горизонтальный scrollable список | `GuiManager.cpp` |
| Фильтрация по `DeformMode` | `GuiManager.cpp` |
| Кнопка «Import ZBrushes/» — `BrushPresetManager::loadFromFolder()` | `GuiManager.cpp` |
| Кнопка «Export .spbrush» | `GuiManager.cpp` |
| Отображение активных параметров пресета | `GuiManager.cpp` |
| Инициализация `BrushPresetManager::loadFromFolder("ZBrushes/")` при старте | `NativeMain.cpp` |

---

## Структура новых файлов

```
src/
└── brushes/
    ├── BrushPreset.h         ← struct BrushPreset, normalizeBrushJSON()
    ├── BrushPreset.cpp       ← реализация парсинга JSON
    ├── BrushPresetManager.h  ← singleton, API
    └── BrushPresetManager.cpp ← реализация загрузки/поиска
```

---

## Порядок внедрения

```
Этап 0: src/brushes/ — BrushPreset + Manager (только новые файлы, нет сайдэффектов)
   ↓
Этап 1: расширить BrushSettings + applyPreset + strokeFlattenEx
   ↓
Этап 4: GUI панель (раньше Этапа 2 — чтобы видеть пресеты в UI)
   ↓
Этап 2: Taubin Smooth + GrabDynamicRadius
   ↓
Этап 3: connectedTopology + grab stroke
```

---

## Итог по архитектурным изменениям

| Изменение | Масштаб | Риск |
|---|---|---|
| Новая папка `src/brushes/` | ✅ Только добавление | Нет |
| Расширение `BrushSettings` новыми полями | ✅ Обратно совместимо (defaults) | Низкий |
| Новые `strokeFlattenEx`, `strokeSmoothTaubin`, `strokeMoveEx` | ✅ Не меняют старые функции | Нет |
| `BrushPresetManager::instance()` в `NativeMain.cpp` | ✅ Инициализация при старте | Нет |
| `applyPreset()` в `SculptManager` | ✅ Новый метод | Нет |
| GUI: новая панель пресетов | ✅ Добавление | Нет |

> [!IMPORTANT]
> Старый код (`BrushSettings[22]`, `BrushType` enum, старые `stroke*()` функции) **не удаляется** — работает параллельно до полного перехода на пресеты.

> [!TIP]
> `nlohmann_json` уже в `CMakeLists.txt` — подключать не нужно. ZBrushes/*.json уже в репозитории — можно начинать Этап 0 прямо сейчас.
