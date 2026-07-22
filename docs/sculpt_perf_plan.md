# План оптимизации производительности скульптинга (C++ Native)

## Диагностика: Что показывают логи

```
[C++ computeAreaNormalAndCenter] 65864 verts took: 0.54ms  ← КАЖДЫЙ ФРЕЙМ
[uploadIfDirty] Binding VAO...                             ← ПОЛНАЯ ПЕРЕЗАЛИВКА GPU КАЖДЫЙ ФРЕЙМ
```

Два лога чередуются — это значит, что **на каждый мазок кисти**:
1. `computeAreaNormalAndCenter` обходит ~65–75k вершин (весь mesh в радиусе!)
2. `uploadIfDirty` заново заливает **все** VBO (verts + normals + colors + materials + indices) в GPU

> [!CAUTION]
> `generateTriangleIndices` и `generateWireframeIndices` вызываются при **каждом** `isDirty`, полностью перебирая все грани. Это катастрофическая потеря производительности на больших мешах.

---

## Корневые причины замедления

### 1. 🔴 КРИТИЧНО: Полная перегрузка GPU при каждом мазке

**Файл:** `AngleRenderer.cpp → uploadIfDirty()`

```cpp
// Сейчас: перегружает ВСЕ данные каждый фрейм
glBufferData(GL_ARRAY_BUFFER, mesh->verts.size() * sizeof(float), ...); // ~800KB
glBufferData(GL_ARRAY_BUFFER, mesh->normals.size() * sizeof(float), ...);
// + generateTriangleIndices() — O(nbFaces), каждый раз
// + generateWireframeIndices() — O(nbFaces), каждый раз
```

**В JS-версии:** использовался `glBufferSubData` только для dirty-диапазона вершин.

**Решение:** Разделить `isDirty` на два флага:
- `isVertexDirty` — только позиции/нормали (subData по диапазону)
- `isTopologyDirty` — изменилась видимость (требует rebuild EBO)

### 2. 🔴 КРИТИЧНО: `computeAreaNormalAndCenter` вызывается слишком часто

**Файл:** `SculptManager.cpp → handleEvent()`

`computeAreaNormalAndCenter` вызывается **трижды** для Clay, ClayBuildup, и Flatten, и **каждый раз обходит все `pickedVertices`** (~65k). При высокодетализированном меше это 0.5–1ms x3 = 1.5–3ms только на этот вызов.

**Решение:**
- Вызывать один раз, кешировать результат на время мазка
- Если `accumulate = false`, результат устаревает только при изменении позиции кисти

### 3. 🟡 ВАЖНО: `getFacesFromVerticesFast` не используется в текущем коде

В `SculptManager.cpp` грани собираются **вручную через цикл** (строки 699–711) с флаговым массивом `dirtyFaces(mesh->nbFaces, 0)` — аллокация `nbFaces * sizeof(uint32_t)` **каждый фрейм!**

```cpp
// Сейчас (строки 699–711): аллокация нового вектора каждый мазок
std::vector<uint32_t> dirtyFaces(mesh->nbFaces, 0); // ← АЛЛОКАЦИЯ каждый фрейм!
```

**Решение:** Использовать готовый `getFacesFromVerticesFast` с epoch-тегами, или кешировать `dirtyFaces` в `SculptManager`.

### 4. 🟡 ВАЖНО: `pickVerticesInSphere` создаёт `std::vector<bool>` размером с весь меш

**Файл:** `Octree.cpp → pickVerticesInSphere()`

```cpp
std::vector<bool> visited(nbVerts, false); // ← АЛЛОКАЦИЯ ~nbVerts/8 bytes каждый фрейм
```

**Решение:** Epoch-теги (как в `getFacesFromVerticesFast`), или постоянный cached-буфер.

### 5. 🟡 ВАЖНО: Нет частичного GPU-апдейта (SubBuffer)

JS-версия через WebGL `bufferSubData` обновляла только диапазон изменившихся вершин. В C++ аналог — `glBufferSubData`. Нужно хранить `[minDirtyVert, maxDirtyVert]` диапазон.

### 6. 🟢 MINOR: `glGetUniformLocation` вызывается каждый фрейм

**Файл:** `AngleRenderer.cpp → drawMeshSolid()`

```cpp
// 10+ вызовов glGetUniformLocation каждый фрейм для каждого меша
glUniformMatrix4fv(glGetUniformLocation(program, "uMV"), ...);
```

Это cache miss в GL-драйвере. Нужно кешировать локации при компиляции шейдера.

---

## Дополнительные логи для подтверждения гипотез

Добавить замеры в `SculptManager::handleEvent()`:

```cpp
// После pickVerticesInSphere:
auto t0 = std::chrono::high_resolution_clock::now();

// ...brush logic...

auto t1 = std::chrono::high_resolution_clock::now();
auto t2 = std::chrono::high_resolution_clock::now(); // после updateFaceNormals
auto t3 = std::chrono::high_resolution_clock::now(); // после octree.update
auto t4 = std::chrono::high_resolution_clock::now(); // после setDirty (uploadIfDirty следует в render)

printf("[PERF] pick=%.2fms brush=%.2fms faceNorm=%.2fms octree=%.2fms\n",
    dur(t0,t1), dur(t1,t2), dur(t2,t3), dur(t3,t4));
```

---

## План реализации (по приоритету)

### Фаза 1 — Быстрые победы (1–2 часа, ожидаемый прирост: 3–5×)

#### 1.1 Убрать лог из uploadIfDirty (немедленно)
```cpp
// Удалить строку:
std::cout << "[uploadIfDirty] Binding VAO..." << std::endl;
// (cout с flush блокирует поток!)
```

#### 1.2 Разделить dirty-флаги + использовать SubData
```cpp
// В Mesh.h добавить:
bool isVertexDirty = false;   // только verts/normals
bool isTopologyDirty = false; // visibility изменилась → rebuild EBO
uint32_t dirtyVertMin = 0;
uint32_t dirtyVertMax = 0;

// В uploadIfDirty:
if (mesh->isVertexDirty) {
    size_t offset = mesh->dirtyVertMin * 3 * sizeof(float);
    size_t size   = (mesh->dirtyVertMax - mesh->dirtyVertMin + 1) * 3 * sizeof(float);
    
    glBindBuffer(GL_ARRAY_BUFFER, bufs->vboVertices);
    glBufferSubData(GL_ARRAY_BUFFER, offset, size,
                    mesh->verts.data() + mesh->dirtyVertMin * 3);
    
    glBindBuffer(GL_ARRAY_BUFFER, bufs->vboNormals);
    glBufferSubData(GL_ARRAY_BUFFER, offset, size,
                    mesh->normals.data() + mesh->dirtyVertMin * 3);
    
    mesh->isVertexDirty = false;
}
// EBO rebuild только при isTopologyDirty
if (mesh->isTopologyDirty) {
    // generateTriangleIndices + bufferData
    mesh->isTopologyDirty = false;
}
```

#### 1.3 Убрать аллокацию `dirtyFaces` из hot-path
```cpp
// В SculptManager.h добавить:
std::vector<uint32_t> m_tagFlags;   // persistent tag array
uint32_t m_tagEpoch = 0;

// В handleEvent, заменить:
std::vector<uint32_t> dirtyFaces(mesh->nbFaces, 0); // ← УДАЛИТЬ
// На:
if (m_tagFlags.size() < (size_t)mesh->nbFaces)
    m_tagFlags.assign(mesh->nbFaces, 0);
// использовать epoch-тег вместо сброса
```

### Фаза 2 — Средние оптимизации (2–4 часа, ещё 2×)

#### 2.1 Dirty-диапазон вершин в SculptManager
После каждого brush-stroke: отслеживать min/max изменённых вершин и передавать это в `setDirty`.

#### 2.2 Epoch-tag в `pickVerticesInSphere`
Заменить `std::vector<bool> visited(nbVerts)` на постоянный uint32_t epoch-массив в `Octree`.

#### 2.3 Кешировать `computeAreaNormalAndCenter` результат
Для Clay/ClayBuildup: пересчитывать только при первом вызове нового мазка, кешировать на время drag.

### Фаза 3 — Архитектурные улучшения (4–8 часов)

#### 3.1 Кеш uniform locations
```cpp
struct ShaderUniforms {
    GLint uMV, uMVP, uN, uFlat; // ...
};
std::unordered_map<GLuint, ShaderUniforms> m_uniformCache;
```

#### 3.2 Partial octree update — уже есть!
`Octree::update()` уже поддерживает частичное обновление. Убедиться что в `handleEvent` передаётся только `iFaces` (грани в radius), а не `pickedVertices`.

#### 3.3 Параллелизм (опционально)
Brush kernel (stroke*) и normal recalc (`updateFaceNormalsAndBoxes`) — идеальные кандидаты для `std::for_each(std::execution::par_unseq, ...)`. Требует линковки TBB или `/std:c++17`.

---

## Нужна ли смена архитектуры?

**Нет.** Текущая архитектура правильная. Проблема — в отсутствии инкрементальных обновлений:

| Подход | JS (рабочий) | C++ (текущий) | C++ (цель) |
|--------|-------------|----------------|------------|
| GPU upload | SubData по диапазону | Full BufferData | SubData по диапазону |
| EBO rebuild | Только при hide/show | Каждый фрейм | Только при hide/show |
| Visited-set | Epoch/typed array | `vector<bool>` alloc | Epoch uint32_t array |
| dirtyFaces | Epoch typed array | `vector<uint32_t>` alloc | Epoch persistent |

---

## Ожидаемые результаты

| Фаза | Экономия на фрейм | FPS (было/стало) |
|------|-------------------|-----------------|
| До оптимизации | — | ~15–25 fps при скульптинге |
| Фаза 1 | 5–8ms | ~40–60 fps |
| Фаза 1+2 | 8–12ms | ~60–90 fps |
| Фаза 1+2+3 | 12–15ms | parity с JS |

---

## Порядок действий прямо сейчас

1. **Добавить перфо-логи** в `SculptManager::handleEvent()` чтобы замерить каждый этап
2. **Реализовать Фазу 1.1** (убрать cout/flush из hot-path)  
3. **Реализовать Фазу 1.2** (SubData + dual dirty flags)
4. **Реализовать Фазу 1.3** (убрать аллокацию dirtyFaces)
5. Замерить снова и перейти к Фазе 2
