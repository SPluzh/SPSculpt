# План миграции кистей и скульпта из JavaScript/WASM в Standalone C++ Native (Обновленный)

Этот документ содержит обновленное техническое руководство по переносу скульптурного движка и кистей из оригинальной JS-версии (`sculptsp`) в нативное C++ приложение (`sculptsp-native`) с учетом ваших приоритетов.

---

## 1. Архитектурный рефакторинг (Type-Safety) — Приоритет №1

На данный момент в `sculptsp-native` код скульптинга (`SculptEngine.cpp`) сохраняет сигнатуры функций, изначально спроектированные под Emscripten/WASM (принимают raw-указатели в виде `uintptr_t`).

**Первоочередная задача**: Перейти на строгую типизацию с использованием `float*`, `uint32_t*`, `uint8_t*` или ссылок на `std::vector` напрямую.

### Пример изменения сигнатуры:
```cpp
// БЫЛО:
int strokeFlatten(uintptr_t vertsPtr, uintptr_t vertProxyPtr, ...);

// СТАЛО:
int strokeFlatten(
    float* verts,
    const float* vertProxy,
    const float* materials,
    const uint32_t* iVerts, int nbIVerts,
    const glm::vec3& center,
    const glm::vec3& areaCenter,
    const glm::vec3& areaNormal,
    float radius, float intensity,
    bool negative, bool accumulate, bool lockPosition,
    float focalShift, bool focalShiftFalloff,
    const AlphaParams& alpha
);
```

---

## 2. Анализ покрытия кистей (Адаптированный)

Из плана полностью исключены инструменты: `ZSphereTool` (Z-сферы), `Topology` (динамическая топология), `CurveDeformTool` (деформация по кривой), `Measure` (линейка) и `Divider` (делитель).

### Группа A: Базовые кисти (Реализованы в C++, требуется рефакторинг типов)
*   **Flatten (Плоская)** — `strokeFlatten`.
*   **Smooth (Сглаживание)** — `strokeSmooth`.
*   **Inflate (Вздутие)** — `strokeInflate`.
*   **Pinch (Сжатие)** — `strokePinch`.
*   **Crease (Складка)** — `strokeCrease`.
*   **Move (Перенос)** — `strokeMove`.
*   **Drag (Тяга)** — `strokeDrag`.
*   **Elastic (Эластичная)** — `strokeElastic`.

### Группа B: Частично реализованные кисти (Требуется подключение в Enums/SculptManager и GUI)
*   **Masking (Маскирование)** — `strokeMask` (записывает маску в канал `materials[i*3 + 2]`).
*   **Paint (Рисование)** — `strokePaint` и `strokePaintAll` (запись цвета в `colors` + шероховатость/металличность).
*   **LocalScale (Локальный масштаб)** — `strokeLocalScale`.
*   **Twist (Скручивание)** — `strokeTwist`.

### Группа C: Отсутствующие кисти (Необходимо портировать из JS)
1.  **Clay & ClayBuildup**
    *   *Логика*: Базируется на кисти `Flatten`, но смещает центр плоскости сжатия (`areaCenter`) наружу/внутрь вдоль нормали на расстояние `offset = radius * 0.1`.
    *   *ClayBuildup*: Использует квадратный альфа-штамп (`alphaSquare`) по умолчанию и уменьшенную в 10 раз интенсивность.
2.  **DamStandard**
    *   *Логика*: Sharp V-groove. Сочетает в себе притяжение к центру (Pinch) и углубление (Brush Modifier) с особым профилем штампа Catmull-Rom.
3.  **VTool**
    *   *Логика*: Линейная борозда под углом 90 градусов с резким стягиванием в центре.
4.  **SquareBrush**
    *   *Логика*: Стандартный оффсет, но с жестким квадратным затуханием (falloff).
5.  **Visibility**
    *   *Логика*: Скрытие/показ вершин (запись в `vertVisible`).

---

## 3. Поддержка графического планшета (WinTab API + Windows Ink)

Для качественного скульптинга на Windows нативно реализуется поддержка графического планшета с возможностью выбора источника ввода через GUI настройки.

### Схема интеграции:
```
               [ Окно приложения (SDL2 / HWND) ]
                                │
          ┌─────────────────────┴─────────────────────┐
          ▼                                           ▼
   [ WinTab API ]                             [ Windows Ink ]
- Загрузка Wintab32.dll                   - Обработка WM_POINTER events
- Контекст WTOpen                         - Считывание POINTER_INFO
- Прямой опрос координат                  - Поддержка Windows Ink систем
  и давления пера                           и сенсорных экранов
          │                                           │
          └─────────────────────┬─────────────────────┘
                                ▼
                   [ Tablet::setPressure() ]
```

### Реализация:
1.  **Интерфейс Tablet**: Создать класс `src/platform/TabletInput.h/.cpp` для переключения контекстов ввода.
2.  **WinTab**: Динамическая загрузка библиотеки `Wintab32.dll` (через `GetProcAddress`), чтобы избежать сбоев при запуске на устройствах без Wacom-совместимых драйверов.
3.  **Windows Ink**: Перехват сообщений Windows (`SDL_EventFilter` или интеграция с оконной процедурой SDL2 через `SDL_SetWindowsMessageHook`) для обработки пакетов `WM_POINTERDOWN` / `WM_POINTERUPDATE` и извлечения `pointerInfo.pressure`.

---

## 4. Индивидуальные параметры кистей (Brush Settings)

Вместо глобальных настроек кисти в `SculptManager` создается структура пресетов `ToolState`, которая хранит настройки индивидуально для каждого инструмента.

```cpp
struct ToolState {
    float radius = 50.0f;
    float intensity = 0.5f;
    float focalShift = 0.0f;
    float hardness = 0.5f;
    float spacing = 0.05f;
    bool negative = false;
    bool clay = false;
    bool accumulate = false;
    bool culling = true;
    bool lockPosition = false;
    int idAlpha = 0;

    // Специфичные настройки
    bool tangent = false;         // Для Smooth
    float elasticity = 0.5f;      // Для Elastic
    glm::vec3 paintColor{0.72f, 0.52f, 0.45f};
    float paintRoughness = 0.5f;
    float paintMetalness = 0.0f;
};
```

---

## 5. Пошаговый план миграции (Stages)

### Этап 1: Рефакторинг типов и сигнатур (Type-Safety)
*   [ ] Изменить типы аргументов во всех функциях `SculptEngine.h/.cpp` с `uintptr_t` на реальные C++ указатели (`float*`, `uint32_t*`, `uint8_t*`).
*   [ ] Отрефакторить вызовы функций в `SculptManager.cpp`, удалив `reinterpret_cast`.
*   [ ] Убедиться в успешной компиляции проекта с новыми сигнатурами.

### Этап 2: Подключение кистей группы B (Mask, Paint, Twist, LocalScale)
*   [ ] Добавить кисти в `BrushType` (`common/Enums.h`).
*   [ ] Реализовать вызов `strokeMask`, `strokePaint`, `strokeTwist` и `strokeLocalScale` в `SculptManager::handleEvent`.
*   [ ] Вывести параметры кистей на панель настроек кисти в ImGui GUI (`GuiManager`).

### Этап 3: Добавление планшетного ввода (WinTab + Windows Ink)
*   [ ] Добавить `WindowsMessageHook` в `NativeMain.cpp`.
*   [ ] Написать менеджер `TabletInput` для динамической загрузки `Wintab32.dll` и перехвата `WM_POINTER`.
*   [ ] Добавить в `GuiManager` селектор: `[Ввод давления: Выкл / WinTab / Windows Ink]`.
*   [ ] Привязать полученную силу нажатия к расчету интенсивности деформации кисти.

### Этап 4: Портирование кистей группы C (Clay, ClayBuildup, DamStandard)
*   [ ] Реализовать смещение плоскости деформации вдоль нормали в `SculptManager.cpp` (для Clay/ClayBuildup).
*   [ ] Перенести формулу профиля штампа Catmull-Rom кривой для `DamStandard` в `SculptEngine.cpp`.
*   [ ] Добавить кисти в GUI.

---

## 6. Чек-лист тестирования

1.  **Компиляция**: Приложение успешно собирается без предупреждений о приведении указателей к целым числам.
2.  **Пресеты**: Переключение между кистями сохраняет их индивидуальные настройки (радиус, интенсивность, текстуру альфа).
3.  **Давление пера**: Сила нажатия пера корректно регулирует размер кисти и глубину мазка как в режиме WinTab, так и в режиме Windows Ink.
4.  **Маски и цвета**: Корректно работают кисти маскирования (Mask) и раскраски (Paint) без утечек памяти.
