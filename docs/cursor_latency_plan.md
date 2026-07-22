# План: Устранение отставания курсора при скульпте

## Диагностика проблемы

Текущий поток кадра:
```
SDL_PollEvent → sculpt.handleEvent() → sculpt.processFrame()
             → cursor.update() [ray-cast внутри!]
             → cursor.applyToRenderer()
             → renderer.render()
             → SDL_GL_SwapWindow()    ← VSync / SwapBuffers добавляет задержку
```

**Корень проблемы:** `BrushCursor::update()` вызывается **внутри `processFrame`** — уже после того, как все события обработаны. К моменту рендера мышь уже сдвинулась. Плюс `SDL_Delay(8)` принудительно замедляет цикл.

При активном скульпте дополнительная причина — `cursor.update()` делает **ray-cast через octree** даже когда уже есть готовая `m_currentIntersection` от `executeStroke`. Это лишняя работа.

---

## Шаг 1 — Скрыть системный курсор мыши при скульпте

**Файл:** `NativeMain.cpp` + `SculptManager.h/cpp`

### 1.1 Добавить флаг и методы в SculptManager

```cpp
// SculptManager.h — в секции private:
bool m_cursorHidden = false;

// публичные методы:
bool isSculpting() const { return m_isSculpting; }
```

### 1.2 Управлять системным курсором в NativeMain.cpp

В главном цикле, после `sculpt.handleEvent()` и перед рендером:

```cpp
// В main loop, сразу после SDL_PollEvent блока:
bool sculptingNow = sculpt.isSculpting();
SDL_ShowCursor(sculptingNow ? SDL_DISABLE : SDL_ENABLE);
```

> **Почему здесь, а не внутри SculptManager?** SculptManager не должен знать о SDL-окне. Управление курсором — это платформенная ответственность NativeMain.

---

## Шаг 2 — Устранить отставание: опросить мышь прямо перед рендером

**Файл:** `NativeMain.cpp`

SDL события могут быть «устаревшими» к моменту рендера. SDL предоставляет `SDL_GetMouseState()` — синхронный запрос **актуального** положения мыши прямо сейчас.

```cpp
// В main loop, ПОСЛЕ SDL_PollEvent блока, ПЕРЕД render:

int rawMouseX, rawMouseY;
SDL_GetMouseState(&rawMouseX, &rawMouseY);

// Передаём актуальные координаты напрямую в cursor.update()
// вместо того, чтобы полагаться на координаты из event
sculpt.setRawMousePos(rawMouseX, rawMouseY);

sculpt.processFrame(scene);
sculpt.getCursor().applyToRenderer(renderer);
```

### 2.1 Добавить `setRawMousePos` в SculptManager

```cpp
// SculptManager.h
void setRawMousePos(int x, int y) {
    m_rawMouseX = x;
    m_rawMouseY = y;
}

// private members:
int m_rawMouseX = 0;
int m_rawMouseY = 0;
```

В `processFrame` использовать `m_rawMouseX/Y` вместо `m_prevMouseX/Y` для обновления курсора.

---

## Шаг 3 — Убрать ray-cast из cursor.update() во время активного скульпта

**Файл:** `BrushCursor.cpp` — метод `update()`

Сейчас при `hasActiveStrokeHit == true` ray-cast пропускается (строка 64–67) — **это уже правильно**. Нужно убедиться, что `SculptManager` всегда передаёт `hasActiveStrokeHit = true` пока `m_isSculpting = true`.

**Файл:** `SculptManager.cpp` — место вызова `m_cursor.update()`

```cpp
// Найти вызов m_cursor.update(...) и убедиться что передаются:
bool hasHit = m_isSculpting && m_currentIntersectionValid;
m_cursor.update(
    m_rawMouseX, m_rawMouseY,
    scene,
    getCurrentSettings().radius,
    m_useSym, m_symAxis,
    m_isSculpting,
    m_currentBrush,
    hasHit,                        // ← не пересчитывать ray-cast
    m_currentIntersection,
    m_currentIntersectionNormal
);
```

---

## Шаг 4 — Режим "только точка" при скульпте (showCircle = false)

В `BrushCursor.cpp` уже есть `m_state.showCircle = !isSculpting` (строка 132). Это корректно. Дополнительно нужно убедиться, что:

- Dot-MVPstроится **в screen-space** когда `isSculpting = true`, используя `m_rawMouseX/Y` — чтобы точка не «прилипала» к старому 3D hit-point.

### 4.1 Screen-space dot во время скульпта

```cpp
// В BrushCursor::update(), в блоке isSculpting:
if (isSculpting) {
    // Dot всегда рендерится в screen-space по актуальным координатам мыши
    float w = camera.getWidth()  * 0.5f;
    float h = camera.getHeight() * 0.5f;
    glm::mat4 orthoProj = glm::ortho(-w, w, -h, h, -10.0f, 10.0f);
    glm::mat4 trans = glm::translate(glm::mat4(1.0f),
        glm::vec3(-w + (float)mouseX, h - (float)mouseY, 0.0f));
    m_state.dotMVP = orthoProj * glm::scale(trans, glm::vec3(3.5f, 3.5f, 1.0f));
}
```

---

## Шаг 5 — Убрать SDL_Delay или заменить на adaptive

**Файл:** `NativeMain.cpp` (строка 351)

```cpp
// БЫЛО:
SDL_Delay(8); // limit to ~120fps

// СТАЛО (вариант A — убрать совсем, пусть VSync регулирует):
// SDL_GL_SetSwapInterval(1); // включить VSync (обычно уже включён)

// СТАЛО (вариант B — adaptive, не задерживать если рендер сам медленный):
// Убрать SDL_Delay целиком. Добавить SDL_GL_SetSwapInterval(1) при инициализации.
```

> **Важно:** `SDL_Delay(8)` — это минимум 8мс задержки перед следующим опросом мыши. При 120fps каждый кадр = 8.3мс, а delay сжирает половину. С VSync задержка контролируется точнее.

---

## Итоговый порядок изменений

| # | Файл | Изменение |
|---|------|-----------|
| 1 | `SculptManager.h` | Добавить `isSculpting()`, `setRawMousePos()`, `m_rawMouseX/Y` |
| 2 | `SculptManager.cpp` | Использовать `m_rawMouseX/Y` в вызове `m_cursor.update()` |
| 3 | `BrushCursor.cpp` | Screen-space dot при `isSculpting = true` |
| 4 | `NativeMain.cpp` | `SDL_GetMouseState()` + `sculpt.setRawMousePos()` перед `processFrame`; `SDL_ShowCursor` в зависимости от `isSculpting()`; убрать/заменить `SDL_Delay` |

---

## Ожидаемый результат

- ✅ Системный курсор мыши скрывается при нажатии кнопки (начало скульпта)
- ✅ Восстанавливается при отпускании
- ✅ Точка курсора отображается в реальных координатах мыши (screen-space, без ray-cast задержки)
- ✅ Кольцо курсора исчезает при скульпте — остаётся только точка
- ✅ Нет лишних ray-cast операций во время stroke
