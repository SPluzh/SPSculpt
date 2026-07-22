# Plan: Fix Symmetry Point Jitter & Symmetry Sculpting

## Диагноз проблем

### Проблема 1: Точка симметрии движется рывками

**Место:** `BrushCursor::update()` → `BrushCursor.cpp:215-241`

**Корень:** Во время активного скульптинга (`isSculpting == true`), курсор читает
`activeStrokeHitPt` — это `m_currentIntersection` из `SculptManager`. Эта точка
обновляется только когда ray-cast в `executeStroke()` попадает в меш (строка 296).
При быстром движении мыши `executeStroke` может **не** делать рейкаст каждый кадр
(spacing throttle), а `processFrame` -> `cursor.update()` вызывается каждый кадр.
В результате курсор прыгает назад к старой `m_currentIntersection`, пока spacing
не позволит следующий удар — отсюда «рывки».

**Дополнительно:** При `!hitMesh` в `executeStroke` (строка 290-293) —
`m_currentIntersectionValid` сбрасывается в `false`, и `processFrame` (строка 1800)
передаёт `false` в `cursor.update()`. Это тоже приводит к рывку — курсор
переключается в screenspace режим.

---

### Проблема 2: Симметрия не работает при скульптинге

**Место:** `SculptManager::executeStroke()` — строки 242-779

**Корень:** В `executeStroke` **никогда** не выполняется второй проход с
симметричными координатами. `m_useSym` нигде в этой функции не используется.
`m_useSym` используется только:
- В `BrushCursor::update()` — только для **отображения** зеркального кружка
- В `applyGradientMask()` — только для инструмента градиентной маски

Все вызовы stroke-функций в `executeStroke` принимают `/*alphaXSym*/ false`
жёстко закодированным — симметрия для альфа тоже не передаётся.

---

## План исправлений

### Шаг 1 — Устранить рывки курсора

**Файл:** `src/editing/SculptManager.h` + `SculptManager.cpp`

**1a.** В `SculptManager.h` добавить поля (private section, после строки 73):
```cpp
glm::vec3 m_lastValidIntersection{0.0f};
glm::vec3 m_lastValidIntersectionNormal{0.0f, 1.0f, 0.0f};
bool      m_hasAnyValidIntersection = false;
```

**1b.** В `executeStroke()` при успешном hit (после строки 301) сохранять:
```cpp
m_lastValidIntersection       = m_currentIntersection;
m_lastValidIntersectionNormal = m_currentIntersectionNormal;
m_hasAnyValidIntersection     = true;
```

**1c.** Сбрасывать `m_hasAnyValidIntersection = false` при mouse up (где `m_isSculpting = false`).

**1d.** В `processFrame()` (строки 1792-1803) передавать lastValid при скульптинге:
```cpp
bool useLastValid = m_isSculpting && m_hasAnyValidIntersection;
m_cursor.update(
    m_rawMouseX, m_rawMouseY,
    scene,
    getBrushRadius(),
    m_useSym,
    m_symAxis,
    m_isSculpting,
    activeBrush,
    useLastValid,
    useLastValid ? m_lastValidIntersection       : m_currentIntersection,
    useLastValid ? m_lastValidIntersectionNormal : m_currentIntersectionNormal
);
```

---

### Шаг 2 — Реализовать симметричный проход в executeStroke

**Файл:** `src/editing/SculptManager.cpp`

После основного прохода (после строки 777, до конца функции), добавить блок:

```cpp
// ======== Symmetry pass ========
if (m_useSym && !pickedVertices.empty()) {
    // Отразить центр удара в local-пространстве меша
    glm::vec3 symCenter = m_currentIntersection;
    glm::vec3 symNormal = m_currentIntersectionNormal;
    if      (m_symAxis == 0) { symCenter.x = -symCenter.x; symNormal.x = -symNormal.x; }
    else if (m_symAxis == 1) { symCenter.y = -symCenter.y; symNormal.y = -symNormal.y; }
    else if (m_symAxis == 2) { symCenter.z = -symCenter.z; symNormal.z = -symNormal.z; }

    // Собрать вершины вокруг симметричной точки
    std::vector<uint32_t> symVerts = mesh->octree.pickVerticesInSphere(
        symCenter.x, symCenter.y, symCenter.z,
        radius2, mesh->vertVisible.data()
    );

    if (getCurrentSettings().culling && !symVerts.empty()) {
        glm::vec3 symRayDir = localRayDir;
        if      (m_symAxis == 0) symRayDir.x = -symRayDir.x;
        else if (m_symAxis == 1) symRayDir.y = -symRayDir.y;
        else if (m_symAxis == 2) symRayDir.z = -symRayDir.z;
        filterCullingVertices(symVerts, mesh, symRayDir);
    }

    if (!symVerts.empty()) {
        // Повторить stroke switch с symCenter/symNormal вместо
        // m_currentIntersection/m_currentIntersectionNormal,
        // и m_initialSymIntersection вместо m_initialIntersection (drag-кисти)
        int symDeformedCount = 0;
        switch (activeBrush) {
            // ... (идентичный switch, но с symCenter, symNormal, symVerts)
        }

        if (symDeformedCount > 0) {
            // updateFaceNormals, updateVertexNormals, octree.update для symVerts
        }
    }
}
```

**Для drag-based кистей (MOVE, DRAG, ELASTIC):**

В `SculptManager.h` добавить:
```cpp
glm::vec3 m_initialSymIntersection{0.0f};
```

В `handleEvent()` при `hitMesh` (после строки 1170) вычислять:
```cpp
m_initialSymIntersection = m_initialIntersection;
if      (m_symAxis == 0) m_initialSymIntersection.x = -m_initialSymIntersection.x;
else if (m_symAxis == 1) m_initialSymIntersection.y = -m_initialSymIntersection.y;
else if (m_symAxis == 2) m_initialSymIntersection.z = -m_initialSymIntersection.z;
```

**Для CLAY/CLAYBUILDUP:**

Зеркалить кешированный `m_cachedAreaCenter` при первом кадре симметричного прохода:
```cpp
glm::vec3 symAreaCenter = m_cachedAreaCenter;
glm::vec3 symAreaNormal = m_cachedAreaNormal;
if      (m_symAxis == 0) { symAreaCenter.x = -symAreaCenter.x; symAreaNormal.x = -symAreaNormal.x; }
else if (m_symAxis == 1) { symAreaCenter.y = -symAreaCenter.y; symAreaNormal.y = -symAreaNormal.y; }
else if (m_symAxis == 2) { symAreaCenter.z = -symAreaCenter.z; symAreaNormal.z = -symAreaNormal.z; }
```

---

### Шаг 3 — Рефакторинг (рекомендуется)

Чтобы не дублировать ~400 строк switch-кода, вынести его в статическую функцию:

```cpp
static int doStrokePass(
    SculptManager& sm,
    Mesh* mesh,
    BrushType activeBrush,
    bool negative,
    std::vector<uint32_t>& verts,
    const glm::vec3& center,
    const glm::vec3& normal,
    const glm::vec3& initialCenter,
    const glm::vec3& cachedAreaNormal,
    const glm::vec3& cachedAreaCenter,
    float localRadius,
    float intensity
);
```

Оба прохода (основной и симметричный) вызывают эту функцию.

---

### Шаг 4 — Проверка рендера symmetry dot в AngleRenderer

**Файл:** `src/render/AngleRenderer.cpp` строка ~1696

Убедиться что условие рисования symmetric dots проверяет `symCount > 0`,
а **не** `m_showSymmetryLine` (это флаг для плоскости симметрии, не для курсора).

---

## Порядок реализации

| # | Задача | Файл | Ориентир строки |
|---|--------|------|-----------------|
| 1 | Добавить `m_lastValidIntersection`, `m_hasAnyValidIntersection` | `SculptManager.h` | после стр. 73 |
| 2 | Добавить `m_initialSymIntersection` | `SculptManager.h` | private section |
| 3 | Сохранять lastValid при successful hit | `SculptManager.cpp` | после стр. 301 |
| 4 | Вычислять `m_initialSymIntersection` при mouse down | `SculptManager.cpp` | после стр. 1170 |
| 5 | Сбрасывать `m_hasAnyValidIntersection` при mouse up | `SculptManager.cpp` | ~стр. 1480 |
| 6 | Исправить `processFrame` — передавать lastValid | `SculptManager.cpp` | стр. 1792-1803 |
| 7 | Добавить симметричный проход в `executeStroke` | `SculptManager.cpp` | после стр. 777 |
| 8 | Проверить рендер symmetry dots в AngleRenderer | `AngleRenderer.cpp` | ~стр. 1696 |

---

## Ожидаемый результат

- Курсор кисти движется **плавно** без рывков во время скульптинга
- Dot симметричного курсора отображается корректно на зеркальной стороне
- Скульптинг **реально деформирует** обе стороны меша симметрично
- Drag-кисти (Move, Drag, Elastic) корректно работают в симметричном режиме
