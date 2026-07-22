# SculptSP → C++ Migration: Полный план (Фаза 2+)

Этот документ дополняет `PLAN_cpp_migration.md.resolved` и описывает **полный** перенос всего функционала на C++/ImGui/SDL2/OpenGL ES 3.

---

## Архитектура системы ввода и GUI

```
SDL2 Event Loop
    │
    ├─► ImGui_ImplSDL2_ProcessEvent()
    │       └─► [ImGui panels] GuiManager::render()
    │
    └─► [io.WantCaptureMouse/Keyboard == false]
            ├─► InputManager::dispatch(event)
            │       ├─► HotkeyDispatcher
            │       ├─► CameraController
            │       └─► SculptManager::handleEvent()
            └─► SculptManager::processFrame()  ← brush cursor update
```

---

## Раздел 1. Управление камерой (Camera Controls)

**Текущее состояние:** `CameraCpp` содержит математику (rotate/translate/zoom/resetView). В `SculptManager` есть флаг `m_isOrbiting`. Привязка кнопок мыши жёстко зашита в `SculptManager::handleEvent`.

### 1.1 Выделить `CameraController` из `SculptManager`

Создать `src-wasm/editing/CameraController.h/.cpp`:

```cpp
class CameraController {
public:
    void handleEvent(const SDL_Event& e, CameraCpp& camera);

private:
    enum class DragMode { None, Orbit, Pan };
    DragMode m_drag = DragMode::None;
    int m_prevX = 0, m_prevY = 0;
};
```

Логика маппинга кнопок:

| Действие | Кнопки |
|---|---|
| Orbit (вращение) | RMB drag  **или**  LMB + Alt |
| Pan (панорама) | MMB drag  **или**  Shift + RMB |
| Zoom | Scroll wheel  **или**  Ctrl + RMB vertical |
| Frame (сброс к мешу) | клавиша `F` |
| Reset View | двойной клик MMB |

### 1.2 Параметры камеры в GuiManager (Camera Panel)

Расширить существующую панель "Camera Settings":

```cpp
// speedRotate, speedTranslate, speedZoom — слайдеры
float rot = camera.getSpeedRotate();
if (ImGui::SliderFloat("Rotate Speed",   &rot,   0.1f, 5.0f)) camera.setSpeedRotate(rot);

float pan = camera.getSpeedTranslate();
if (ImGui::SliderFloat("Pan Speed",      &pan,   0.1f, 5.0f)) camera.setSpeedTranslate(pan);

float zm  = camera.getSpeedZoom();
if (ImGui::SliderFloat("Zoom Speed",     &zm,    0.1f, 5.0f)) camera.setSpeedZoom(zm);

// Режим проекции + FOV уже есть
// Добавить: UsePivot checkbox, кнопки Front/Top/Right/Left/Bottom/Back
```

### 1.3 Snap-кнопки ортографических видов

В Camera Panel добавить 6 кнопок:

```
[Front] [Back] [Top] [Bottom] [Left] [Right]
```

Каждая вызывает `camera.setOrbitAngles(rx, ry)` + `camera.setProjectionType(ORTHOGRAPHIC)`.

### 1.4 Навигационный куб (Gizmo Cube) — Выполнено

Мини-рендер 3D-куба в правом верхнем углу вьюпорта. Реализован с помощью `ImDrawList` в `GuiManager::render` (что обеспечивает идеальное сглаживание, простую отрисовку текста, автоматическую обработку кликов/наведения и предотвращает просачивание кликов во вьюпорт). Клик по грани выравнивает камеру по соответствующей ортогональной проекции.

### 1.5 Референсные изображения (Reference Images)

Новый класс `src-wasm/render/ReferenceImage.h`:

```cpp
struct ReferenceImage {
    GLuint texId = 0;
    float  opacity = 0.5f;
    float  scale   = 1.0f;
    float  offsetX = 0.0f, offsetY = 0.0f;
    bool   visible = true;
    bool   pinned2D = true; // true=overlay, false=3D plane
};
```

Загрузка через `stb_image.h` (уже может быть в `third_party`). Рендер в `AngleRenderer::drawReferenceImages()` — quad с alpha-blend перед рендером меша или поверх.

---

## Раздел 2. Курсор кисти (Brush Cursor)

**Текущее состояние:** `AngleRenderer` принимает готовые матрицы `circleMVP/innerCircleMVP/dotMVP` через `setCursorParameters()`. Вычисление этих матриц сейчас происходит в JS.

### 2.1 Перенести вычисление курсора в C++

Создать `src-wasm/editing/BrushCursor.h/.cpp`:

```cpp
struct BrushCursorState {
    bool     visible     = false;
    glm::vec3 hitPoint  {0.0f};
    glm::vec3 hitNormal {0.0f, 1.0f, 0.0f};
    float    radius      = 8.0f;   // в мировых единицах
    glm::vec3 color      {1.0f, 0.3f, 0.1f};

    // Вычисленные MVP для AngleRenderer
    glm::mat4 circleMVP    {1.0f};
    glm::mat4 innerCircleMVP{1.0f};
    glm::mat4 dotMVP       {1.0f};
};

class BrushCursor {
public:
    // Вызывается при каждом движении мыши над вьюпортом
    void update(int mouseX, int mouseY,
                const SceneCpp& scene,
                float brushRadius);

    void applyToRenderer(AngleRenderer& renderer) const;

    const BrushCursorState& getState() const { return m_state; }

private:
    BrushCursorState m_state;

    glm::mat4 buildCircleMVP(const glm::vec3& center,
                              const glm::vec3& normal,
                              float radius,
                              const CameraCpp& cam) const;
};
```

Алгоритм `update()`:
1. Построить луч через `camera.getRay(mouseX, mouseY)`.
2. Пересечение луча с мешом через Octree (`Octree::intersect()`).
3. Если попадание: `m_state.visible = true`, сохранить `hitPoint`, `hitNormal`.
4. Вычислить `circleMVP` = матрица трансформации окружности на поверхности меша, ориентированной по нормали.
5. Вызвать `renderer.setCursorParameters(...)`.

### 2.2 Динамический радиус курсора

`SculptManager` хранит `m_brushRadius` в **экранных** единицах (пикселях). Для корректного отображения в 3D нужно конвертировать:

```cpp
float worldRadius = brushRadius * hitDepth * tan(fov_rad * 0.5f) * 2.0f / screenHeight;
```

Добавить в `SculptManager` метод `float getWorldRadius(const CameraCpp&, float depth) const`.

### 2.3 Курсор симметрии

Если включена симметрия (X/Y/Z), вычислять отражённые `symMVPs` и передавать в `setCursorParametersFast()`.

---

## Раздел 3. Все параметры и настройки (Settings System)

Для точного соответствия JS-версии параметры кистей должны храниться индивидуально для каждого инструмента, а не глобально. При переключении на `SMOOTH`, `MASKING` или `TOPOLOGY` радиус копируется из предыдущего активного инструмента. Режимы отображения, материалы и настройки рендера также должны полностью дублировать интерфейс `GuiRendering` из JS.

### 3.1 Параметры кистей (Brush Settings) — на основе JS-версии

#### 3.1.1 Структура индивидуального состояния инструмента

Вместо глобальных полей в `SculptManager` создаётся структура `ToolState`, и менеджер хранит карту состояний для всех типов кистей:

```cpp
struct ToolState {
    float radius = 50.0f;          // 5..500
    float intensity = 0.5f;        // 0..1 (0..100% в GUI)
    float focalShift = 0.0f;       // -1..1 (-100..100% в GUI)
    float hardness = 0.5f;         // 0..1 (0..100% в GUI)
    float spacing = 0.05f;         // 0..2 (0..200% в GUI)
    bool negative = false;
    bool clay = false;
    bool accumulate = false;
    bool culling = true;
    bool dynTopoInfluence = true;
    bool lockPosition = false;
    int idAlpha = 0;               // Индекс в Picking.ALPHAS_NAMES

    // Специализированные настройки для конкретных инструментов
    bool topoCheck = false;        // Для Move, Elastic (Topological Check)
    bool tangent = false;          // Для Smooth (Tangential Smoothing)
    int maskSteps = 3;             // Для Masking (Masking Steps, 1..100)
    float sharpenFactor = 1.0f;    // Для Masking (Sharpen Factor, 0.1..5.0)
    bool useLasso = false;         // Для Masking (Lasso Selection)
    float thickness = 0.0f;        // Для Masking (Extract thickness)
    float elasticity = 0.5f;       // Для Elastic (Elasticity, 10..300%)
    int divisions = 3;             // Для Divider (Divisions, 2..6)
    bool useDistanceThickness = false; // Для Measure, Divider (Thickness check)
    
    // Специализированные настройки Paint
    glm::vec3 paintColor{0.72f, 0.52f, 0.45f};
    float paintRoughness = 0.5f;
    float paintMetalness = 0.0f;
    bool writeAlbedo = true;
    bool writeRoughness = true;
    bool writeMetalness = true;
    bool pickColor = false;

    // Специализированные настройки Mask Gradient Blur
    int sharpenBlurIterations = 3;
    bool blurMaskedOnly = false;
};
```

#### 3.1.2 Расширение `SculptManager`

Добавить в `SculptManager.h`:

```cpp
private:
    std::unordered_map<int, ToolState> m_toolStates; // Хранилище настроек для каждого Enums.Tools
    
    // Глобальные флаги скульптинга
    bool m_continuous = false;
    bool m_dynamicBrushSize = false;
    
    // AccuCurve глобальные настройки
    bool  m_accuCurve = false;
    float m_accuCurveLUT[256];
    float m_accuCurveExponent = 1.7f;
    std::string m_accuCurveType = "sharp"; // или "bezier"
    float m_accuCurveP1[2] = {0.25f, 0.75f};
    float m_accuCurveP2[2] = {0.75f, 0.25f};

    // Временные переключения (modifier stack)
    int  m_savedToolIndex = -1;
    bool m_shiftOverride = false;  // Shift -> Smooth
    bool m_ctrlOverride = false;   // Ctrl -> Masking
```

При переключении инструмента через `setToolIndex(int newValue)` радиус для Smooth, Masking и Topology наследуется от предыдущего активного инструмента:

```cpp
void SculptManager::setToolIndex(int newValue) {
    int oldToolIndex = m_currentBrush;
    m_currentBrush = (BrushType)newValue;

    if (newValue == BRUSH_SMOOTH || newValue == BRUSH_MASKING || newValue == BRUSH_TOPOLOGY) {
        m_toolStates[newValue].radius = m_toolStates[oldToolIndex].radius;
    }
}
```

### 3.2 Параметры рендера (Render Settings) — точная копия JS

Панель "Rendering Quality" / "Rendering" в `GuiManager` должна полностью дублировать структуру `GuiRendering.js`:

```cpp
struct RenderSettings {
    int   shaderType = 5;         // Enums.Shader (0=PBR, 1=Flat, 2=Normal, 3=Wireframe, 4=UV, 5=Matcap, 14=WetClay)
    float curvature = 20.0f;      // Curvature (0..100) -> делить на 20.0f для mesh.setCurvature()
    bool  filmic = true;          // Filmic tonemapping
    int   envId = 0;              // Выбранный ID окружения PBR
    int   matcapId = 0;           // Выбранный ID маткапа
    float exposure = 1.0f;        // PBR Exposure (0..5)
    float transparency = 0.0f;    // Transparency (0..100%) -> opacity = 1.0f - trans / 100.0f
    bool  flatShading = false;    // Flat Shading (применяется к выбранным мешам)
    bool  showWireframe = false;  // Wireframe (применяется к выбранным мешам)
    bool  antialias = true;       // Сглаживание (включение FXAA)

    // Параметры мокрой глины (Wet Clay)
    float wetClayWetness = 0.5f;
    float wetClayBump = 0.5f;
    float wetClayNoiseScale = 10.0f;
    float wetClaySSS = 0.5f;
};
```

Каждое изменение в GUI должно обновлять соответствующий шейдер в `ShaderLib` или свойства выбранных мешей (`mesh->setFlatShading(val)`, `mesh->setShowWireframe(val)`, `mesh->setOpacity(val)`), как это сделано в JS.

### 3.3 Система сохранения настроек (App Config) — на основе localStorage

Настройки должны сохраняться в файл `sculptsp.cfg` при выходе и загружаться при старте. Конфигурация включает:

- Настройки графического планшета: `tabletControls` (bool), `useWintab` (bool).
- Глобальные настройки рендера: `antialias` (bool), `exposure` (float), `filmic` (bool), `shaderType` (int), `envId` (int), `matcapId` (int).
- Индивидуальные параметры для каждой кисти (радиус, интенсивность, culling, spacing и т. д.), чтобы пользователь не терял настроенные пресеты при переключениях между сессиями.

---

## Раздел 4. Рендер-пайплайн (Render Pipeline)

**Текущее состояние:** `AngleRenderer` рендерит меши с matcap/PBR/flat шейдерами, фон, wireframe, курсор.

### 4.1 Перенести вычисление cursor MVPs в C++

(Описано в Разделе 2 — `BrushCursor::applyToRenderer()`)

### 4.2 PBR: окружающее освещение через IBL / SPH

`AngleRenderer` уже принимает `m_sph[27]`. Задача — вычислять SPH-коэффициенты из простой процедурной HDRI (или встроенной заглушки), а не получать из JS:

```cpp
// src-wasm/render/EnvironmentSPH.h
void computeDefaultSPH(float sph[27], float exposure = 1.0f);
void computeSPHFromEquirect(float sph[27], const uint8_t* pixels,
                             int w, int h, float exposure);
```

Вызывать при старте в `NativeMain.cpp` после инициализации рендерера.

### 4.3 Постпроцессинг: FXAA

Добавить опциональный fullscreen pass FXAA антиалиасинга. В `AngleRenderer`:

```cpp
// Добавить поля:
GLuint m_fboMSAA = 0, m_fboColor = 0;
bool   m_useFXAA = false;
GLuint m_fxaaProgram = 0;

// В init():
void initOffscreenFBO(int w, int h);

// В render():
// 1. Рендерим в FBO
// 2. Применяем FXAA пасс
// 3. Blitting на экран
```

В GuiManager → Rendering Quality: `[ ] FXAA Antialiasing`.

### 4.4 Wireframe Overlay

Wireframe рендер уже реализован (`m_wireframeProgram`, `eboWireframe`). Убедиться, что:
- Wireframe работает поверх всех шейдеров (GL_POLYGON_OFFSET_LINE).
- Цвет wireframe настраивается из GUI.

### 4.5 Рендер глубины / Shadow map — фаза 3

Простой directional shadow map (single cascade) для PBR шейдера. Добавить в `AngleRenderer`:
- `GLuint m_shadowFBO, m_shadowTex`
- Shadow pass перед основным рендером
- Uniform `u_shadowMap` в PBR шейдере

---

## Раздел 5. Диспетчер горячих клавиш (Hotkey System)

**Текущее состояние:** горячие клавиши разбросаны по нескольким JS-модулям (`GuiSculpting`, `GuiCamera`, `GuiStates`, `GuiFiles`, `GuiScene`, `GuiTopology`, `SculptSP`). В C++ нужна единая централизованная система.

### 5.1 Создать `HotkeyDispatcher`

`src-wasm/platform/HotkeyDispatcher.h/.cpp`:

```cpp
enum class HKAction {
    // Инструменты (точное соответствие Enums.Tools в JS)
    ToolBrush, ToolInflate, ToolTwist, ToolSmooth, ToolFlatten,
    ToolPinch, ToolCrease, ToolDrag, ToolPaint, ToolMove,
    ToolMasking, ToolLocalScale, ToolTransform, ToolClayBuildup,
    ToolZSphere, ToolTopology, ToolMeasure, ToolElastic,
    ToolCurveDeform, ToolDivider, ToolVisibility, ToolDamStandard,

    // Brush modal controls (hold + horizontal drag)
    ModalRadius,           // S
    ModalIntensity,        // A
    ModalFocalShift,       // D
    ModalTopologyDetail,   // Z (только при активном Topology)
    ModalRemeshResolution, // X
    ModalCameraFov,        // G

    // Brush toggles
    ToggleNegative,        // N
    PickerMode,            // I (hold/release)

    // Camera
    CameraFrame,           // F  (on keyUp)
    CameraProjection,      // P  (on keyUp)
    CameraLeft,            // L  (on keyUp)
    CameraUndo,            // Alt+Z
    CameraRedo,            // Alt+Shift+Z
    StrifeLeft, StrifeRight, StrifeUp, StrifeDown, // Arrows

    // Scene
    DeleteMesh,            // Del
    ToggleIsolate,         // C
    DuplicateSelection,    // Ctrl+D
    ClearScene,            // Ctrl+Alt+N

    // Undo/Redo (sculpt history)
    Undo,                  // Ctrl+Z
    Redo,                  // Ctrl+Y или Ctrl+Shift+Z

    // Files
    OpenFile,              // Ctrl+O или Ctrl+I
    ExportOBJ,             // Ctrl+E

    // Topology
    ToggleDynamic,         // Ctrl+T
    Remesh,                // Ctrl+X

    // Misc
    OpenContextPopup,      // F1
};

class HotkeyDispatcher {
public:
    bool dispatch(const SDL_KeyboardEvent& key,
                  SculptManager& sculpt,
                  SceneCpp& scene,
                  GuiManager& gui);

    // Modifier stack — вызывать на каждый KEYDOWN/KEYUP
    void updateModifiers(SDL_Keycode sym, bool isDown,
                         SculptManager& sculpt);
};
```

### 5.2 Полная таблица горячих клавиш (точное соответствие JS)

> **Источники:** `src/misc/getOptionsURL.js` (`readShortcuts`), `GuiSculpting.onKeyDown/Up`, `GuiCamera.onKeyDown/Up`, `GuiStates.onKeyDown`, `GuiFiles.onKeyDown`, `GuiScene.onKeyDown`, `GuiTopology.onKeyDown`.

#### Инструменты скульптинга

| Клавиша | Инструмент |
|---|---|
| `0` | Paint |
| `1` | Brush |
| `2` | Inflate |
| `3` | Twist |
| `4` | Transform |
| `5` | Smooth |
| `6` | Flatten |
| `7` | Pinch |
| `8` | Crease |
| `9` | Drag |
| `Q` | Move |
| `W` | ClayBuildup |
| `E` | DamStandard |
| `R` | Pinch (альтернатива) |
| `T` | Topology (повторное нажатие = возврат к предыдущему) |

#### Модальные параметры (hold + drag по горизонтали)

| Клавиша | Параметр | Примечание |
|---|---|---|
| `S` | Radius | drag px за px |
| `A` | Intensity | 0–100 |
| `D` | FocalShift **или** Hardness | зависит от инструмента |
| `Z` | TopologyDetail | только при Topology tool |
| `X` | RemeshResolution | 8–2000, scale x2 |
| `G` | Camera FOV | 10–200 мм |

#### Переключатели кисти

| Клавиша | Действие |
|---|---|
| `N` | Toggle negative (инвертировать знак) |
| `I` | Режим пипетки (hold = включён, release = выключен) |

#### Modifier stack (hold)

| Клавиша | При зажатии | При отпускании |
|---|---|---|
| `Shift` | Временно → Smooth tool | Вернуть предыдущий инструмент |
| `Ctrl` | Временно → Masking tool | Вернуть предыдущий инструмент |
| `Alt` | Инвертировать знак кисти (`_invertSign`) | Вернуть нормальный знак |

> **Важно:** `Alt` НЕ переключает орбиту камеры на LMB через горячую клавишу — это обрабатывается в `onDeviceDown` по флагу `_isAltDown`, который устанавливается в `SculptSP.onKeyDown`.

#### Камера

| Клавиша | Действие | Момент |
|---|---|---|
| `F` | Frame Camera (обрамить меш) | keyUp |
| `P` | Toggle Perspective / Orthographic | keyUp |
| `L` | Camera Left view | keyUp |
| `G` | Модальное FOV (drag) | keyDown |
| `Alt+Z` | Undo камеры | keyDown |
| `Alt+Shift+Z` | Redo камеры | keyDown |
| Arrow keys | Strife Left/Right/Up/Down | keyDown/Up |

#### Сцена и меши

| Клавиша | Действие |
|---|---|
| `Del` | Удалить выбранный меш |
| `C` | Toggle isolate (показать/скрыть невыделенные) |
| `Ctrl+D` | Дублировать выделение |
| `Ctrl+Alt+N` | Очистить сцену |

#### Undo / Redo (история скульптинга)

| Клавиша | Действие |
|---|---|
| `Ctrl+Z` | Undo |
| `Ctrl+Y` или `Ctrl+Shift+Z` | Redo |

#### Файлы

| Клавиша | Действие |
|---|---|
| `Ctrl+O` или `Ctrl+I` | Открыть файл (Import) |
| `Ctrl+E` | Экспорт OBJ |

#### Топология

| Клавиша | Действие |
|---|---|
| `Ctrl+T` | Toggle Dynamic Topology |
| `Ctrl+X` | Запустить Remesh |

#### Прочее

| Клавиша | Действие |
|---|---|
| `F1` | Открыть контекстное popup-меню |

> **Не реализовано / закомментировано в JS:** `Space` (сброс камеры — закомментирован), `[`/`]` (радиус кисти — отсутствуют), `Ctrl+S` (сохранение — не реализовано), Numpad 1/3/7 (виды — не реализованы).

### 5.3 Modifier Stack для Shift/Ctrl/Alt

```cpp
// В HotkeyDispatcher::updateModifiers():
void HotkeyDispatcher::updateModifiers(SDL_Keycode sym, bool isDown,
                                       SculptManager& sculpt) {
    switch (sym) {
    case SDLK_LSHIFT: case SDLK_RSHIFT:
        if (isDown && !m_shiftOverride) {
            if (sculpt.getCurrentBrush() != BRUSH_SMOOTH) {
                m_savedBrush = sculpt.getCurrentBrush();
                sculpt.setCurrentBrush(BRUSH_SMOOTH);
                m_shiftOverride = true;
            }
        } else if (!isDown && m_shiftOverride) {
            sculpt.setCurrentBrush(m_savedBrush);
            m_shiftOverride = false;
        }
        break;

    case SDLK_LCTRL: case SDLK_RCTRL:
        if (isDown && !m_ctrlOverride) {
            if (sculpt.getCurrentBrush() != BRUSH_MASKING) {
                m_savedBrush = sculpt.getCurrentBrush();
                sculpt.setCurrentBrush(BRUSH_MASKING);
                m_ctrlOverride = true;
            }
        } else if (!isDown && m_ctrlOverride) {
            sculpt.setCurrentBrush(m_savedBrush);
            m_ctrlOverride = false;
        }
        break;

    case SDLK_LALT: case SDLK_RALT:
        if (isDown && !m_invertSign) {
            m_invertSign = true;
            sculpt.toggleNegative();
        } else if (!isDown && m_invertSign) {
            m_invertSign = false;
            sculpt.toggleNegative();
        }
        break;
    }
}
```

### 5.4 Блокировка горячих клавиш в ImGui полях

Эквивалент JS-проверки `document.activeElement.tagName === 'INPUT'`:

```cpp
// В начале dispatch():
ImGuiIO& io = ImGui::GetIO();
if (io.WantCaptureKeyboard) {
    // ImGui поглощает ввод (активно текстовое поле)
    // Исключение: Ctrl+Z / Ctrl+Y обрабатываем всегда
    if (!(key.keysym.mod & KMOD_CTRL))
        return false;
    if (key.keysym.sym != SDLK_z && key.keysym.sym != SDLK_y)
        return false;
}
```

---

## Раздел 6. Оставшиеся панели GUI

### 6.1 Панель Files (Импорт / Экспорт)

Базовый `importOBJ` / `exportOBJ` уже реализован в `GuiManager.cpp`. Расширить:

- **Импорт:** добавить нативный диалог файлов через `SDL_OpenURL` (Windows: `GetOpenFileName` через `<windows.h> / COMDLG32`).
- **Экспорт STL:** binary STL (простой triangle dump).
- **Очистка сцены:** кнопка "Clear Scene" → `scene.clear()`.
- **Автосохранение:** фоновый таймер каждые N минут экспортирует в `autosave.obj`.

```
[Import OBJ...] [Import STL...]
[Export OBJ...] [Export STL...]
[Clear Scene]
─────────────────────────────
Auto-save: [ ] every [5 ▲▼] min
Last save: 12:34:01
```

### 6.2 Панель маскирования (Masking Panel)

Кнопки операций над масками вершин (`materials[]` хранит маску в компоненте w/alpha):

```cpp
if (ImGui::Button("Clear Mask",   {-1,0})) { /* обнулить masks */ }
if (ImGui::Button("Invert Mask",  {-1,0})) { /* 1.0f - mask[i] */ }
if (ImGui::Button("Blur Mask",    {-1,0})) { blurMask(..., 3); }
if (ImGui::Button("Sharpen Mask", {-1,0})) { /* усилить контраст */ }
if (ImGui::Button("Hide Masked",  {-1,0})) { /* vertVisible[i]=0 */ }
if (ImGui::Button("Show All",     {-1,0})) { /* vertVisible[i]=1 */ }
```

### 6.3 Панель мультиразрешения (Multiresolution)

Добавить в `SceneCpp` хранение уровней LOD:

```cpp
// В SceneCpp:
std::vector<MeshCpp*> m_lodLevels; // [0]=base, [1]=sub1, ...
int m_currentLod = 0;
```

Панель в GUI:

```
Level: [0 ───●────── 3]  (slider)
[Subdivide]  [Reconstruct Lower]
[Delete Lower] [Delete Higher]
```

`Subdivide` → вызов `Remesh::catmullClark(mesh)` или Loop subdivision (уже есть в `Remesh.cpp`?).

### 6.4 Панель Z-сфер (ZSpheres)

Новый класс `src-wasm/editing/ZSphereRig.h`:

```cpp
struct ZNode {
    glm::vec3 position;
    float     radius;
    int       parent; // -1 = root
};

class ZSphereRig {
public:
    bool active = false;
    std::vector<ZNode> nodes;

    int  addNode(const glm::vec3& pos, float r, int parent);
    void removeNode(int idx);
    MeshCpp* generateSkin(int resolution) const;
};
```

Режим ZSpheres: клик по вьюпорту → добавить ноду, drag → перемещение, RMB → удаление. Кнопка "Create Adaptive Skin" → `generateSkin()` → добавить меш в сцену.

---

## Раздел 7. Дополнительный функционал

### 7.1 Undo/Redo — улучшение

**Текущее состояние:** `SceneCpp` имеет `pushHistoryState() / undo() / redo()` с хранением полных копий `MeshState`. Это дорого для больших мешей.

Улучшение — дельта-компрессия:
```cpp
struct DeltaState {
    std::vector<uint32_t> changedVertIdx;
    std::vector<float>    oldVerts;  // только изменённые
    std::vector<float>    newVerts;
};
```

Реализовать как `SceneCpp::pushDelta(const std::vector<uint32_t>& dirtyVerts)`.

### 7.2 Трансформация меша (Mesh Transform Gizmo)

Когда выбрана кисть Move/Drag — или активирован режим Transform — рисовать 3-осевой гизмо в центре меша. Реализация в `AngleRenderer::drawTransformGizmo()`. Управление через отдельный режим в `SculptManager` (State Machine: Sculpt | Transform | ZSpheres).

### 7.3 Статистика производительности (Performance HUD)

В GUI — опциональный HUD в углу вьюпорта:
```
FPS: 120  Frame: 8.3ms
Verts: 49152  Faces: 49152
Octree nodes: 1024
```

Реализовать через `SDL_GetPerformanceCounter()`.

### 7.4 Инструмент Lasso Selection

В `SculptManager`: новый режим `BRUSH_LASSO`. При зажатии Ctrl + drag по вьюпорту — рисуется контур лассо (2D полигон). После отпускания — все вершины, проекция которых попадает внутрь контура, получают маску.

```cpp
// В AngleRenderer — рисовать лассо как LineLoop:
void drawLassoContour(const std::vector<glm::vec2>& points);
```

### 7.5 Поддержка планшетного ввода (Stylus Pressure)

SDL2 не поддерживает давление нативно на Windows. Варианты:
- Использовать `SDL_HINT_MOUSE_RELATIVE_MODE_CENTER` + WinTab API (`wintab.h`) в `#ifdef _WIN32` блоке.
- Давление маппить на `brushIntensity * pressure`.

Добавить в `SculptManager`:
```cpp
float m_tabletPressure = 1.0f; // [0..1]
bool  m_usePressure    = false;
```

---

## Порядок реализации (приоритеты)

| Приоритет | Задача | Зависимости |
|---|---|---|
| 🔴 P0 | `BrushCursor` — вычисление MVP в C++ | `CameraCpp::getRay`, `Octree` |
| 🔴 P0 | `CameraController` — выделить из `SculptManager` | — |
| 🔴 P0 | `HotkeyDispatcher` + Modifier Stack | `SculptManager` |
| 🟡 P1 | Brush Settings: hardness, focalShift, sym, negative | `SculptManager`, `GuiManager` |
| 🟡 P1 | Camera Panel: speed sliders, snap views | `CameraCpp` |
| 🟡 P1 | Render: SPH computation, exposure в GuiManager | `AngleRenderer` |
| 🟡 P1 | Files Panel: нативный диалог, STL export | `GuiManager` |
| 🟢 P2 | Masking Panel: операции над масками | `SculptEngine::blurMask` |
| 🟢 P2 | `AppConfig` — сохранение/загрузка настроек | — |
| 🟢 P2 | Multiresolution Panel: subdivide/reconstruct | `Remesh.cpp` |
| 🔵 P3 | Transform Gizmo | `AngleRenderer` |
| 🔵 P3 | FXAA postprocess | `AngleRenderer` FBO |
| 🔵 P3 | ZSpheres Panel + `ZSphereRig` | — |
| 🔵 P3 | Lasso Selection | `AngleRenderer`, Octree |
| 🔵 P3 | Reference Images | `stb_image`, `AngleRenderer` |
| ⚪ P4 | Delta Undo | `SceneCpp` |
| ⚪ P4 | Shadow map | `AngleRenderer` |
| ⚪ P4 | Tablet pressure (WinTab) | `SculptManager`, `NativeMain` |
| ✅ Done | Navigation Cube Gizmo | `GuiManager::render` |

---

## Чек-лист проверки

### Камера
- [ ] RMB drag → вращение без рывков
- [ ] MMB drag → панорамирование
- [ ] Scroll → зум
- [ ] `F` → камера обрамляет выбранный меш
- [ ] Слайдеры скорости камеры работают

### Курсор кисти
- [ ] Курсор следует за поверхностью меша (ориентирован по нормали)
- [ ] Радиус курсора в мировых единицах соответствует `brushRadius`
- [ ] Симметричный курсор отображается при включённой симметрии
- [ ] Курсор исчезает, когда мышь вне меша

### Параметры кистей
- [ ] `[` / `]` изменяют радиус и обновляют слайдер в GUI
- [ ] Shift → временно переключает на Smooth
- [ ] Ctrl → инвертирует кисть (негативный режим)
- [ ] Параметры сохраняются в `sculptsp.cfg` при выходе

### Рендер
- [ ] PBR шейдер корректно использует SPH-освещение
- [ ] Wireframe отображается корректно для всех шейдеров
- [ ] Прозрачность (alpha) работает для выбранного меша
- [ ] FXAA включается/отключается из GUI без артефактов

### Файлы
- [ ] OBJ импортируется и добавляется в сцену
- [ ] OBJ экспортируется корректно
- [ ] STL экспортируется в binary формате
- [ ] "Clear Scene" удаляет все меши

### Горячие клавиши
- [ ] `0`–`9`, `Q`, `W`, `E`, `R`, `T` выбирают инструменты (Paint, Brush, Inflate, Twist, Transform, Smooth, Flatten, Pinch, Crease, Drag, Move, ClayBuildup, DamStandard, Pinch2, Topology)
- [ ] `S` (modal radius), `A` (modal intensity), `D` (modal focal shift/hardness) работают с drag
- [ ] `Shift` hold → временно Smooth, `Ctrl` hold → временно Masking, `Alt` hold → инвертирует знак
- [ ] `F` → Frame Camera, `P` → Toggle проекции, `L` → Left view
- [ ] `C` → Toggle isolate, `Del` → удалить меш
- [ ] `Ctrl+D` → дублировать, `Ctrl+T` → toggle dynamic topology
- [ ] `Ctrl+O`/`I` → импорт, `Ctrl+E` → экспорт OBJ
- [ ] Ввод в ImGui полях не перехватывается HotkeyDispatcher (кроме Ctrl+Z/Y)
- [ ] `Ctrl+Z` / `Ctrl+Y` работают как Undo/Redo

### Маски
- [ ] "Clear Mask" сбрасывает все маски
- [ ] "Invert Mask" инвертирует корректно
- [ ] "Blur Mask" размывает маску (3+ итерации)
