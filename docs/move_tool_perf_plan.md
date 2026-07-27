# Plan: Move Tool Performance Fix

## Диагностика — почему тормозит при ~70k вершин

После анализа кода выявлены следующие причины замедления:

### 1. Spacing/step loop вызывает `executeStroke` несколько раз за одно движение мыши

`handleEvent → SDL_MOUSEMOTION` (строки 2697–2711):

```cpp
float step = 1.0f / std::floor(dist / minSpacing);
for (float i = step; i <= 1.01f; i += step) {
    executeStroke(...);   // вызывается N раз за один mousemove
}
```

Для Move/Drag/Elastic **spacing бессмысленен** — граб-браш применяет смещение **от начальной точки** к текущей, а не аккумулирует мазки. Несколько вызовов за кадр дают **многократный overshoot** и лишнюю работу на пересчёт нормалей + octree.

### 2. Каждый `executeStroke` → `doStrokePass` → пересчитывает нормали и обновляет octree

Блок в `doStrokePass` после деформации (строки 952–996):
- `getFacesFromVerticesFast` — обход ring-face для всех pickedVertices
- `updateFaceNormalsAndBoxes` — пересчёт нормалей всех затронутых граней
- `updateVertexNormals` — пересчёт вершинных нормалей
- `mesh->octree.update(...)` — перестройка части октодерева

При 70k точек под кистью это **очень тяжёлые операции, повторяемые N раз за кадр**.

### 3. Grab-кэш `m_grabbedVertices` работает правильно, но `pickedVertices` при симметрии ищется заново через octree на каждом кадре

Строки 1294–1297: симметрийный набор каждый раз запрашивается через `octree.pickVerticesInSphere()`.  
У исходного набора кэш есть (`m_grabbedVerticesSym`), но он используется только для grab-пути — и правильно. Проблема не здесь.

### 4. `glm::inverse(mesh->matrix)` вызывается в каждом `executeStroke`

Строка 1047: матричная инверсия на каждый вызов (при spacing-loop — N раз за кадр).

---

## План исправлений

### Шаг 1 — Bypass spacing для grab-кистей *(главное)*

**Файл:** `SculptManager.cpp`, строки ~2691–2712

Граб-кисти (Move/Drag/Elastic) не должны использовать spacing. Они должны вызывать `executeStroke` **ровно один раз** за событие мыши — с текущей позицией.

```cpp
// Определить до блока spacing:
bool isGrabBrushActive = (activeBrush == BRUSH_MOVE ||
                          activeBrush == BRUSH_DRAG ||
                          activeBrush == BRUSH_ELASTIC);

if (isGrabBrushActive || minSpacing <= 0.0f) {
    // Граб-кисти: всегда один вызов, без spacing
    executeStroke(scene, mesh, camera, (float)mouseX, (float)mouseY, currentPressure);
    m_lastStrokeX = mouseX;
    m_lastStrokeY = mouseY;
} else if (dist > minSpacing) {
    // Обычные кисти: spacing loop
    ...
}
```

> [!IMPORTANT]
> Это устраняет основную причину — многократный вызов тяжёлого пайплайна за один mousemove.

---

### Шаг 2 — Вынести пересчёт нормалей и octree из `doStrokePass` наружу

Сейчас нормали/octree пересчитываются **внутри** `doStrokePass` после каждого мазка.  
При симметрии — это два отдельных пересчёта (primary + sym pass), хотя оба затрагивают одну mesh.

**Рефакторинг в `executeStroke`:**
- `doStrokePass` возвращает список затронутых вершин/граней, **не пересчитывая** нормали сам
- После обоих проходов (primary + sym) делается **единый** пересчёт нормалей и octree

Это сократит число тяжёлых операций вдвое при включённой симметрии.

---

### Шаг 3 — Кэшировать `glm::inverse(mesh->matrix)` на время строка

**Файл:** `SculptManager.h` — добавить `glm::mat4 m_cachedInvMatrix` и `bool m_invMatrixDirty`.

В `executeStroke` (строка 1047):
```cpp
// Вместо:
glm::mat4 invMatrix = glm::inverse(mesh->matrix);
// Использовать кэшированную, обновляемую только при первом кадре строка:
if (m_firstStrokeFrame) m_cachedInvMatrix = glm::inverse(mesh->matrix);
glm::mat4& invMatrix = m_cachedInvMatrix;
```

Для граб-кистей матрица меша не меняется — кэш действителен всё время строка.

---

### Шаг 4 — Параллелизация `strokeMove` через OpenMP (опционально)

**Файл:** `SculptEngine.cpp` — в функции `strokeMove`:

```cpp
#pragma omp parallel for schedule(static) if(pickedCount > 5000)
for (int i = 0; i < pickedCount; ++i) {
    // применение смещения к вершине
}
```

Это напрямую ускорит саму деформацию при большом числе вершин.

---

### Шаг 5 — Убрать `step`/`spacing` из UI для Move/Drag/Elastic

**Файл:** `GuiManager.cpp` — в секции настроек кисти Move/Drag/Elastic не показывать слайдер `spacing`.

Предположительно: в блоке отрисовки панели настроек проверить `m_currentBrush` и скрыть `ImGui::SliderFloat("Spacing", ...)` если инструмент — граб-тип.

---

## Порядок реализации

| Приоритет | Шаг | Файл | Ожидаемый прирост |
|-----------|-----|------|-------------------|
| 🔴 Критично | 1 — Bypass spacing | `SculptManager.cpp` | ×3–10 на типичном движении |
| 🟠 Важно   | 2 — Единый пересчёт норм | `SculptManager.cpp` | ×1.5–2 с симметрией |
| 🟡 Средне  | 3 — Кэш invMatrix | `SculptManager.cpp/h` | Незначительно |
| 🟢 Плюс    | 4 — OpenMP | `SculptEngine.cpp` | ×2–4 на 70k+ при многоядерном CPU |
| ⚪ Чистота  | 5 — Убрать UI spacing | `GuiManager.cpp` | UX |

---

## Что **не** нужно менять

- Логика `m_grabbedVertices` — кэш вершин работает правильно
- Формула смещения в `BRUSH_MOVE` (`doStrokePass`) — математически верна
- Октодерево — его структура и `update()` оптимальны для локальных изменений
