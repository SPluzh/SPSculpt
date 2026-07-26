# Plan: Porting Multiresolution System JS → C++ Native

## 1. Что портируется (объём работ)

| JS-файл | Размер | Роль |
|---|---|---|
| `MeshResolution.js` | 293 строки | Один уровень мульти-разрешения — хранит detail-векторы, апсинтез/анализ |
| `Multimesh.js` | 307 строк | Контейнер для стека уровней, переключение уровней, рендер-хинты |
| `Subdivision.js` | 589 строк | Loop / Catmull-Clark подразбиение (even + odd smoothing, UV) |
| `Reversion.js` | 581 строка | Обратное подразбиение (детектирование even-вершин, воссоздание coarse mesh) |

Итого ~1770 строк JavaScript → C++.

---

## 2. Нужны ли изменения архитектуры?

### 2.1 Текущая архитектура C++ (sculptsp-native)

```
Mesh  ──────── хранит flat std::vector<float/uint32_t>
   └── Octree
   └── ArmatureGraph

SculptEngine ── работает непосредственно с Mesh*
Remesh       ── отдельный namespace/module
```

Сейчас `SculptEngine` получает `Mesh*` напрямую. Мультиразрешение вводит **стек** мешей и понятие «текущего» меша. Это требует нескольких архитектурных решений.

### 2.2 Необходимые изменения

#### 2.2.1 Новый класс `MeshResolution` (extends Mesh)

```cpp
// src/mesh/MeshResolution.h
class MeshResolution : public Mesh {
public:
    std::vector<float>    detailsXYZ;   // detail displacement (local frame)
    std::vector<float>    detailsRGB;   // color deltas
    std::vector<float>    detailsPBR;   // material deltas
    std::vector<uint32_t> vertMapping;  // mapping to higher-res vertices
    bool                  evenMapping = false;

    void higherSynthesis(MeshResolution& meshDown);
    void lowerAnalysis(MeshResolution& meshUp);
    void applyDetails();
    void computeDetails(const std::vector<float>& subdVerts,
                        const std::vector<float>& subdColors,
                        const std::vector<float>& subdMaterials);
private:
    void copyDataFromHigherRes(MeshResolution& meshUp);
    void computePartialSubdivision(std::vector<float>& subdVerts,
                                   std::vector<float>& subdColors,
                                   std::vector<float>& subdMaterials);
};
```

#### 2.2.2 Новый класс `Multimesh`

```cpp
// src/mesh/Multimesh.h
enum class RenderHint { NONE, SCULPT, CAMERA, PICKING };

class Multimesh {
public:
    std::vector<std::unique_ptr<MeshResolution>> meshes;
    int  sel = 0;          // current level index
    static RenderHint renderHint;

    MeshResolution* getCurrentMesh();
    MeshResolution* addLevel();        // fullSubdivision → push
    MeshResolution* computeReverse();  // Reversion → unshift
    MeshResolution* lowerLevel();
    MeshResolution* higherLevel();
    void            selectResolution(int sel);
    void            deleteLower();
    void            deleteHigher();

    // Render
    int  getLowIndexRender() const;
    void updateResolution();

private:
    void syncVisibility(int fromSel, int toSel);
};
```

#### 2.2.3 Адаптация SculptEngine

Сейчас: `SculptEngine` держит `Mesh* activeMesh`.
После порта: нужен `Multimesh* activeMultimesh` + метод `getMesh()` → `activeMultimesh->getCurrentMesh()`.

**Минимально инвазивный вариант** — обёртка-адаптер:
```cpp
// SculptEngine пока что не трогаем полностью
// Добавляем:
Mesh* SculptEngine::getMesh() {
    if (activeMultimesh)
        return activeMultimesh->getCurrentMesh();
    return activeMesh; // legacy
}
```

#### 2.2.4 Новый модуль `Subdivision`

```cpp
// src/editing/Subdivision.h
namespace Subdivision {
    bool LINEAR = false;

    // Полное подразбиение (с обновлением топологии)
    void fullSubdivision(MeshResolution& base, MeshResolution& newMesh);

    // Частичное (только геометрия, топология уже известна)
    void partialSubdivision(MeshResolution& base,
                            std::vector<float>& vertsOut,
                            std::vector<float>& colorsOut,
                            std::vector<float>& materialsOut);
}
```

#### 2.2.5 Новый модуль `Reversion`

```cpp
// src/editing/Reversion.h
namespace Reversion {
    bool computeReverse(MeshResolution& base, MeshResolution& newMesh);
}
```

#### 2.2.6 Edge / FaceEdge массивы

JS-код активно использует `getEdges()` и `getFaceEdges()` — их нет в текущем C++ `Mesh`.

```cpp
// Добавить в Mesh.h:
std::vector<uint32_t> edges;        // edge array (valence per edge)
std::vector<uint32_t> faceEdges;    // edge index per face-vertex
int nbEdges = 0;
```

И метод вычисления `initEdges()` в `Topology.cpp`.

---

## 3. Полный список файлов к созданию/изменению

```
src/
  mesh/
    MeshResolution.h       ← новый
    MeshResolution.cpp     ← новый
    Multimesh.h            ← новый
    Multimesh.cpp          ← новый
    Mesh.h                 ← добавить edges, faceEdges, nbEdges
    Topology.h             ← добавить initEdges()
    Topology.cpp           ← реализация initEdges()
  editing/
    Subdivision.h          ← новый
    Subdivision.cpp        ← новый (~600 строк)
    Reversion.h            ← новый
    Reversion.cpp          ← новый (~580 строк)
  sculpt/
    SculptEngine.h         ← добавить activeMultimesh, getMesh()
    SculptEngine.cpp       ← адаптер
  gui/
    GuiManager.cpp         ← UI кнопки для смены уровня
```

---

## 4. Алгоритмы — точная трансляция JS → C++

### 4.1 `applyDetails()` / `computeDetails()`

Оба метода вычисляют локальный фрейм (normal + tangent + binormal) для каждой вершины и:
- `computeDetails` → проецирует delta-вектор (vUp - subdVert) в локальный фрейм
- `applyDetails` → реконструирует позицию из сохранённого вектора

Всё это — прямая математика без зависимостей, легко портируется с GLM:

```cpp
glm::vec3 n = glm::normalize(glm::vec3(norms[j], norms[j+1], norms[j+2]));
glm::vec3 t = glm::vec3(vTemp[k] - vx, vTemp[k+1] - vy, vTemp[k+2] - vz);
t -= n * glm::dot(t, n);
t = glm::normalize(t);
glm::vec3 bi = glm::cross(n, t);
// project:
dXYZ[j]   = glm::dot(n,  delta);
dXYZ[j+1] = glm::dot(t,  delta);
dXYZ[j+2] = glm::dot(bi, delta);
```

### 4.2 `applyEvenSmooth()` (Subdivision)

Loop / Catmull-Clark smoothing для corner, interior-tri, interior-quad, mixed. Прямой порт — не требует сторонних библиотек. 

> [!IMPORTANT]
> Единственная сложность — в JS используется `Utils.TRI_INDEX = 0xFFFFFFFF`. В C++ использовать константу `UINT32_MAX` или `static constexpr uint32_t TRI_INDEX = 0xFFFFFFFFu`.

### 4.3 `applyOddSmooth()` + `OddVertexComputer`

Вспомогательный класс → превратить в `struct OddVertexComputer` с inline-методами. Логика tagEdges — битовый аккумулятор, хорошо работает с `std::vector<int32_t>`.

### 4.4 `Reversion::computeReverse()`

Алгоритм детектирования even-вершин через BFS (tagVertices + stack). Стек в JS — Uint32Array с ручным curStack, в C++ заменить на `std::vector<uint32_t>` как стек с `push_back/pop_back`.

---

## 5. Готовые библиотеки — рекомендации

### 5.1 Для качества подразделения

| Библиотека | Что даёт | Лицензия |
|---|---|---|
| **OpenSubdiv** (Pixar) | Production-grade Catmull-Clark/Loop, GPU-accelerated, поддержка creases | Apache 2.0 |
| **libIGL** | Subdivision, mesh processing, eigen-based | MPL 2.0 |

> [!TIP]
> **OpenSubdiv** — наилучший выбор если нужна совместимость с индустриальным стандартом. Поддерживает Osd (OpenSubdiv Subdivider) для GPU-вычислений через OpenGL/Vulkan.
> Репозиторий: https://github.com/PixarAnimationStudios/OpenSubdiv

**Однако важно понимать:** JS-реализация использует **гибридный алгоритм** (не чистый Loop/Catmull-Clark) — частичное подразбиение с vertex mapping для detail-preservation. OpenSubdiv не предоставляет partial subdivision out of the box, поэтому:

- `fullSubdivision` → можно делегировать OpenSubdiv  
- `partialSubdivision` + `applyDetails/computeDetails` → **портировать вручную**, они специфичны для этого sculpting-pipeline

### 5.2 Для ускорения (параллелизм)

| Библиотека | Что даёт | Статус в проекте |
|---|---|---|
| **Intel TBB** (oneTBB) | `parallel_for` для applyEvenSmooth / applyDetails | Не используется, легко добавить |
| **OpenMP** | `#pragma omp parallel for` — минимальные изменения кода | Поддерживается MSVC /openmp |
| **GLM SIMD** | SSE/AVX для vec3 операций | GLM уже есть в проекте |

> [!TIP]
> Самый быстрый выигрыш — `#pragma omp parallel for` в `applyEvenSmooth()` и `applyDetails()`. Обе функции итерируют по всем вершинам независимо. На 4-8 ядрах — x3-5 ускорение.

```cpp
// Пример для applyEvenSmooth:
#pragma omp parallel for schedule(static)
for (int i = 0; i < nbVerts; ++i) {
    // ... vertex smoothing
}
```

### 5.3 Рекомендуемый минимальный стек библиотек

```
GLM           — уже есть    (векторная математика)
OpenMP        — добавить    (параллелизм, встроен в MSVC)
OpenSubdiv    — опционально (fullSubdivision quality upgrade)
```

---

## 6. Поэтапный план реализации

### Фаза 1: Фундамент (2-3 дня)

1. **Добавить edge-структуры в Mesh**
   - `edges[]`, `faceEdges[]`, `nbEdges`
   - Реализовать `initEdges()` в `Topology.cpp`
   - Добавить `verticesTagFlags[]` (для Reversion BFS)
   - Добавить `hasOnlyTriangles()` helper

2. **Проверить** что `Topology.cpp::initTopology()` строит `vrfStartCount`, `vrvStartCount`, `vertRingFace`, `vertRingVert` — они уже есть в `Mesh.h` ✓

### Фаза 2: Subdivision (2-3 дня)

3. **Реализовать `Subdivision.cpp`**
   - `struct OddVertexComputer` — порт JS-класса
   - `applyEvenSmooth()` — ~170 строк
   - `applyOddSmooth()` — ~100 строк  
   - `fullSubdivision()` — оркестратор
   - `partialSubdivision()` — без перестройки топологии

4. **Тест**: взять сферу из сцены → subdivide → сравнить счётчики вершин/граней с ожидаемыми (tri: nbV*2+2, nbF*4 / quad: nbV+nbE+nbF, nbF*4)

### Фаза 3: Reversion (2-3 дня)

5. **Реализовать `Reversion.cpp`**
   - `detectExtraordinaryVertices()`
   - `tagEvenVertices()` + BFS `tagVertices()`
   - `createFaces()` + `createVertices()`
   - `copyVerticesData()`

6. **Тест**: subdivide → revert → сравнить с оригиналом

### Фаза 4: MeshResolution (1-2 дня)

7. **Реализовать `MeshResolution.h/.cpp`**
   - `computeDetails()` — local frame projection
   - `applyDetails()` — reconstruction
   - `copyDataFromHigherRes()`
   - `computePartialSubdivision()`
   - `lowerAnalysis()` / `higherSynthesis()`

### Фаза 5: Multimesh + интеграция (2-3 дня)

8. **Реализовать `Multimesh.h/.cpp`**
   - Стек `meshes`, `sel`, `RenderHint`
   - `addLevel()`, `computeReverse()`
   - `lowerLevel()`, `higherLevel()`, `selectResolution()`
   - `syncVisibility()` — двунаправленная синхронизация масок видимости вершин (Low <-> High)
   - `getLowIndexRender()` — LOD-логика при навигации (лимит 500k треугольников)
   - `updateResolution()` — синхронизация VBO/IBO графических буферов на GPU при переключении уровней

9. **Адаптировать SculptEngine и UndoManager**
   - Добавить `Multimesh* activeMultimesh = nullptr`
   - Метод `Mesh* getActiveMesh()` — приоритет Multimesh
   - Подключить `setActiveMesh(Multimesh*)` из Scene
   - Интегрировать операцию отмены (Undo): сохранять детали только активного уровня разрешений или синхронизировать стек уровней

10. **UI в GuiManager**
    - Кнопки: Subdivide / Reverse / Level Up / Level Down
    - Отображение текущего уровня: `[2 / 4]`
    - Кнопки Delete Lower / Delete Higher

### Фаза 6: Оптимизация (1-2 дня)

11. **OpenMP параллелизм**
    - `applyEvenSmooth()`: parallel for по вершинам
    - `applyDetails()` / `computeDetails()`: parallel for по вершинам
    - Осторожно с `applyOddSmooth()` — есть write-конфликты по shared edges

12. **Memory layout**
    - Использовать `std::vector::reserve()` с предварительным расчётом размера
    - Избегать мелких аллокаций в hot path (как JS делает `new Float32Array` внутри цикла)

---

## 7. Ключевые переводы JS → C++

| JS | C++ |
|---|---|
| `Float32Array(n * 3)` | `std::vector<float>(n * 3, 0.0f)` |
| `Uint32Array(n)` | `std::vector<uint32_t>(n, 0)` |
| `Int8Array(n)` | `std::vector<int8_t>(n, 0)` |
| `Utils.TRI_INDEX` | `static constexpr uint32_t TRI_INDEX = 0xFFFFFFFFu` |
| `arr.subarray(0, n)` | `arr.data()`, использовать `std::memcpy` или `std::copy` |
| `arr.set(other)` | `std::copy(other.begin(), other.end(), arr.begin())` |
| `Utils.getMemory(n)` | `std::vector<float>(n / 4)` (убрать shared memory pool) |
| `mesh.getVerticesTagFlags()` | добавить `std::vector<uint32_t> vertTagFlags` в Mesh |
| `Utils.TAG_FLAG++` | `static uint32_t g_tagFlag = 0; ++g_tagFlag;` |
| `Math.sqrt` / `Math.min` / `Math.max` | `std::sqrt`, `std::min`, `std::max` или GLM |

---

## 8. Выявленные пробелы и важные нюансы (Gaps & Blind Spots)

### 8.1 Синхронизация масок видимости (`syncVisibility`)
В `Multimesh.js` реализован важный алгоритм передачи видимости вершин (`_vertVisible`) между уровнями:
- **Переход Low → High (Up):** Дочерние вершины наследуют статус родительских. Промежуточные (не-родительские) вершины анализируют свое кольцо соседей (`vertRingVert`): если хотя бы один соседний родитель скрыт (`isParentHidden`), промежуточная вершина также помечается как скрытая (`0`).
- **Переход High → Low (Down):** Видимость вершине нижнего уровня передается из соответствующей вершины верхнего уровня через `vertMap` (если `evenMapping === true`) или по прямому индексу.

### 8.2 GPU VBO/IBO Обновления (`updateResolution`)
В отличие от JS/WebGL, где динамическое перенаправление буферов не требует перестроения графа вызовов отрисовки, в C++ рендерере (`RenderMesh` / OpenGL VAO):
- При вызове `updateResolution()` нужно инвалидировать или пересоздавать Vertex Buffer Object (VBO) и Index Buffer Object (IBO) текущего меша.
- Для функции фонового LOD-рендеринга (`_renderLow` / `getLowIndexRender`) необходимо поддерживать быстрый своп IBO без вызова полного `initRender()`.

### 8.3 Управление владением и памятью (`MeshResolution : public Mesh`)
- Каждый уровень `MeshResolution` наследует `Mesh` и держит собственные координаты вершин, цветов и материалов.
- Окружение `TransformData` (позиция, вращение, масштаб объекта) должно быть **общим** (shared pointer) для всех уровней `Multimesh`, чтобы трансформации объекта сразу применялись ко всему стеку разрешений.

### 8.4 Интеграция с Undo / Redo
- Скульптинг изменяет координаты только на активном уровне `getCurrentMesh()`.
- При вызове `lowerLevel()` или `higherLevel()` вершины синхронизируются через `lowerAnalysis` / `higherSynthesis`.
- UndoManager должен сохранять снапшот вершин активного меша до начала мазка (stroke) и восстанавливать его без повреждения сохраненных дельт на других уровнях.

### 8.5 Таблица потенциальных проблем и решений

| Проблема | Решение |
|---|---|
| `getEdges()` / `getFaceEdges()` отсутствуют в C++ Mesh | Добавить в Mesh + buildEdges() в Topology |
| `hasOnlyTriangles()` — нет в C++ | Добавить: `bool hasOnlyTriangles() const { return std::none_of(...TRI_INDEX); }` |
| `Utils.TAG_FLAG` — глобальный счётчик для пометки | Сделать `static thread_local uint32_t` или member в SculptEngine |
| `Utils.getMemory()` — переиспользуемый буфер | Заменить на обычный `std::vector` с reserved capacity |
| Memory safety: JS GC автоматически управляет | В C++ — RAII, `unique_ptr` для MeshResolution в стеке Multimesh |
| Thread safety при OpenMP + writeShared edges | `applyOddSmooth` — не параллелить или использовать edge-coloring |
| Reversion BFS: `Utils.TAG_FLAG` non-thread-safe | Сделать локальным параметром функции |
| Рассинхрон масок видимости при смене уровня | Реализовать `syncVisibility()` с проверкой `isParentHidden` |
| Провисание FPS при навигации на тяжелых сетках | Использовать `getLowIndexRender()` + подмену IBO при `RENDER_HINT == CAMERA` |

---

## 9. Итоговая структура файлов

```
cpp/sculptsp-native/src/
├── mesh/
│   ├── Mesh.h              (изменить: +edges, +faceEdges, +vertTagFlags)
│   ├── Mesh.cpp
│   ├── MeshResolution.h    (новый)
│   ├── MeshResolution.cpp  (новый, ~250 строк)
│   ├── Multimesh.h         (новый)
│   ├── Multimesh.cpp       (новый, ~280 строк)
│   ├── Topology.h          (изменить: +initEdges)
│   ├── Topology.cpp        (изменить: реализация initEdges)
│   ├── NormalCalc.h
│   ├── NormalCalc.cpp
│   └── Octree.h/cpp
├── editing/
│   ├── Subdivision.h       (новый)
│   ├── Subdivision.cpp     (новый, ~550 строк)
│   ├── Reversion.h         (новый)
│   └── Reversion.cpp       (новый, ~550 строк)
└── sculpt/
    ├── SculptEngine.h      (изменить)
    └── SculptEngine.cpp    (изменить)
```

**Итого новых строк C++:** ~1700–2000  
**Изменённых файлов:** 6  
**Новых файлов:** 8  
**Оценка времени:** 10–14 рабочих дней
