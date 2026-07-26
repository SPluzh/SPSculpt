# План: Отделение второго вьюпорта в отдельное окно

## Контекст

Сейчас split-режим делит одно SDL-окно пополам: левая половина — основная камера, правая — вторая. Вся логика живёт в `NativeMain.cpp` (координатный пересчёт) и в `AngleRenderer` (рендер двух viewport'ов через `glViewport`).

**Цель** — добавить режим, при котором второй вьюпорт рендерится в отдельное SDL-окно, sharing тот же OpenGL-контекст.

---

## Архитектурные ограничения

> [!IMPORTANT]
> SDL2 + OpenGL ES: второе окно должно **разделять GL-контекст** основного окна (`SDL_GL_MakeCurrent`). Оба окна принадлежат одному процессу и одному потоку рендеринга — переключение контекста между `SwapWindow` вызовами.

> [!WARNING]
> ImGui рендерится только в **основное** окно. Второе окно — чистый 3D viewport без ImGui.

---

## Шаги реализации

### Шаг 1 — Новый режим в Scene / GuiManager

**Файлы:** `Scene.h`, `GuiManager.cpp/.h`

Добавить флаг отдельного окна параллельно с существующим `SplitMode`:

```cpp
// Scene.h (или GuiManager.h)
enum class SecondViewportMode {
    Embedded,   // текущее поведение — split внутри одного окна
    Detached    // новый режим — второй вьюпорт в своём окне
};
```

- В `GuiManager` добавить `m_secondVpMode` + кнопку переключения в меню панели камеры / рендера.
- При смене режима вызвать колбэк в `NativeMain`.

---

### Шаг 2 — Создание второго SDL-окна

**Файл:** `NativeMain.cpp`

```cpp
SDL_Window*   g_secondWindow  = nullptr;
SDL_GLContext g_secondContext  = nullptr; // НЕ новый контекст — shared alias
```

Функция `openSecondViewportWindow(SDL_Window* mainWin, SDL_GLContext mainCtx)`:

```cpp
SDL_Window* openSecondViewportWindow(SDL_Window* mainWin) {
    // Создать окно без OpenGL-флага сначала, потом назначить shared context
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    SDL_Window* win2 = SDL_CreateWindow(
        "SculptSP — Second Viewport",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        800, 600,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN
    );
    // Контекст создаётся на том же share group что и mainCtx
    SDL_GLContext ctx2 = SDL_GL_CreateContext(win2);
    SDL_GL_MakeCurrent(mainWin, mainCtx); // восстановить основной
    return win2;
}
```

Хранить `g_secondWindow` и `g_secondCtx` как переменные в `main()`.

---

### Шаг 3 — Рендеринг второго вьюпорта в новом окне

**Файл:** `NativeMain.cpp` (render loop), `AngleRenderer.cpp`

В конце кадра (после `renderer.render(scene)` для основного окна):

```cpp
if (secondVpDetached && g_secondWindow) {
    SDL_GL_MakeCurrent(g_secondWindow, g_secondCtx);

    int sw, sh;
    SDL_GL_GetDrawableSize(g_secondWindow, &sw, &sh);

    // Рендерим правую камеру на весь второй экран
    renderer.renderViewport(scene, *scene.getCameraRight(), 0, 0, sw, sh);

    SDL_GL_SwapWindow(g_secondWindow);
    SDL_GL_MakeCurrent(mainWindow, mainCtx); // вернуть контекст
}
```

В `AngleRenderer` добавить перегрузку:

```cpp
void AngleRenderer::renderViewport(const Scene& scene, const Camera& cam,
                                   int x, int y, int w, int h);
```

Внутри — `glViewport(x, y, w, h)`, затем стандартный pipeline с переданной камерой.

---

### Шаг 4 — Маршрутизация событий второго окна

**Файл:** `NativeMain.cpp` (event loop)

SDL2 помечает события `event.window.windowID`. Добавить ветку:

```cpp
SDL_WindowID mainID   = SDL_GetWindowID(mainWindow);
SDL_WindowID second2ID = g_secondWindow ? SDL_GetWindowID(g_secondWindow) : 0;

// В SDL_WINDOWEVENT:
if (event.window.windowID == second2ID) {
    if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
        // Закрытие второго окна → переключиться обратно в Embedded
        switchToEmbeddedViewport();
    }
    if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
        // Обновить размер второй камеры
        int w2 = event.window.data1, h2 = event.window.data2;
        scene.getCameraRight()->onResize(w2, h2);
    }
}

// Mouse/keyboard во втором окне → маршрутировать как viewport 1
if (event.type == SDL_MOUSEBUTTONDOWN && event.button.windowID == second2ID) {
    scene.setActiveViewport(1);
    // координаты уже локальные — пересчёт halfW не нужен
    sculpt.handleEvent(event, scene);
}
```

> [!NOTE]
> В detached-режиме координатный сдвиг `eventCopy.button.x -= halfW` убрать — координаты второго окна уже локальные.

---

### Шаг 5 — Split-режим в embedded при переключении

Когда пользователь переключается обратно в Embedded:

```cpp
void switchToEmbeddedViewport() {
    if (g_secondWindow) {
        SDL_GL_MakeCurrent(g_secondWindow, g_secondCtx);
        SDL_GL_DeleteContext(g_secondCtx);
        SDL_DestroyWindow(g_secondWindow);
        g_secondWindow = nullptr;
        g_secondCtx = nullptr;
        SDL_GL_MakeCurrent(mainWindow, mainCtx);
    }
    scene.getSplitMode(); // восстановить embedded split
}
```

---

### Шаг 6 — Кнопка / пункт меню в GuiManager

**Файл:** `GuiManager.cpp`

В панели рендеринга или Camera Panel:

```cpp
if (scene.getSplitMode() != Scene::SplitMode::OFF) {
    ImGui::SameLine();
    if (ImGui::Button(ICON_LC_MAXIMIZE_2 " Pop Out##vpDetach")) {
        m_requestDetachSecondViewport = true; // флаг для NativeMain
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Open second viewport in separate window");
}
```

`NativeMain` проверяет `gui.isRequestingDetach()` раз в кадр и вызывает `openSecondViewportWindow`.

---

### Шаг 7 — Сохранение состояния

**Файл:** `GuiManager.cpp` → `saveSettings` / `loadSettings`

```ini
[Viewport]
secondVpDetached=false
secondVpX=100
secondVpY=100
secondVpWidth=800
secondVpHeight=600
```

При запуске: если `secondVpDetached=true` и `SplitMode != OFF` — автоматически открыть второе окно.

---

## Резюме изменений по файлам

| Файл | Изменение |
|---|---|
| `NativeMain.cpp` | Создание/уничтожение второго окна, shared context, event routing, render loop |
| `AngleRenderer.cpp/.h` | `renderViewport(scene, cam, x, y, w, h)` overload |
| `GuiManager.cpp/.h` | Флаг `m_requestDetachSecondViewport`, кнопка в UI, save/load настроек |
| `Scene.h` | (опционально) `SecondViewportMode` enum |

## Порядок работы

1. `AngleRenderer::renderViewport` — изолированный, тестируемый первым
2. SDL второе окно + shared context в `NativeMain`
3. Render loop для второго окна
4. Event routing
5. GUI кнопка + сохранение состояния
