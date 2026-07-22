# План оптимизации Remesh + Progress Window

## 1. Анализ текущей архитектуры

### Пайплайн `doRemesh()` (вызывается из `GuiManager::performRemesh`)

```
performRemesh()           ← main/render thread, синхронно!
  └─ doRemesh()
       ├─ voxelize()       ← O(nbTris × voxels_per_tri) — самый тяжёлый этап
       ├─ floodFill()      ← O(datalen) с BFS — умеренно тяжёлый
       └─ marchingCubesReconstruct() / surfaceNetsReconstruct()
            └─ unordered_map<string,…> lookup для дедупликации вершин
```

### Что тормозит

| Этап | Сложность | Проблемы |
|------|-----------|----------|
| `voxelize()` | O(nbTris × bbox_voxels) | **1 поток**, тройной вложенный цикл, `sqrt` на каждый воксел |
| `floodFill()` | O(datalen) | Single-thread BFS, `std::vector<bool>` — медленнее `vector<uint8_t>` |
| `marchingCubesReconstruct()` | O(datalen) | **`std::unordered_map<string,uint32_t>`** — тяжелейший узкий момент: `sprintf` + string alloc + hash per vertex |
| `surfaceNetsReconstruct()` | O(datalen) | Аналогично, но проще — индекс через `buffer[]` |
| `computeTopology()` после remesh | O(nbFaces) | Уже отдельная функция, но тоже синхронно |
| **Всё на главном потоке** | — | UI фризится до завершения |

---

## 2. Нужно ли менять архитектуру?

**Нет, архитектуру менять не нужно.** Текущая схема (VoxelGrid → voxelize → floodFill → reconstruct) правильная и соответствует стандартному SDF-remesh подходу.

Нужны **три независимых улучшения**:

1. **Параллелизм** — распараллелить `voxelize()` по треугольникам  
2. **Алгоритмические оптимизации** — убрать `string`-хэш-мап в Marching Cubes, заменить `vector<bool>`, убрать `sqrt` там, где не нужен  
3. **Асинхронность + UI** — запустить `doRemesh` в отдельном потоке, показать прогресс-модал в ImGui

---

## 3. Шаг 1 — Параллелизм `voxelize()` через OpenMP / std::thread

### 3.1 Добавить OpenMP в CMakeLists.txt

```cmake
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
    target_link_libraries(sculptsp-native PRIVATE OpenMP::OpenMP_CXX)
endif()
```

### 3.2 Параллельный `voxelize()` с OpenMP

Цикл по треугольникам (`for (int iTri = 0; iTri < nbTris; ++iTri)`) **идеально параллелен** — каждый треугольник работает с непересекающимися вокселями (за исключением записи в `distanceField`/`colorField`/`crossedEdges`, требующей атомарных операций).

**Вариант A — атомарное обновление (proton-safe, рекомендуется):**

```cpp
// distanceField меняется на: std::vector<std::atomic<float>>
// crossedEdges меняется на: std::vector<std::atomic<uint8_t>>

#pragma omp parallel for schedule(dynamic, 64)
for (int iTri = 0; iTri < nbTris; ++iTri) {
    // ...вся текущая логика треугольника...
    // При записи:
    float old = atomicMin(voxels.distanceField[n], newDist);
    if (newDist < old) {
        // обновить color/material через mutex или compare-exchange
    }
    voxels.crossedEdges[n].fetch_or(bit);
}
```

**Вариант B — tile-based partitioning (без атомарных, быстрее для больших сеток):**

Разбить вокселевую сетку на тайлы по Z-слоям. Каждый тайл обрабатывается одним потоком — треугольник пишет только в свой диапазон вокселей. Треугольники, пересекающие несколько тайлов, обрабатываются в нескольких тайлах.

```cpp
// Псевдокод
int numThreads = std::thread::hardware_concurrency();
int slicePerThread = rz / numThreads;

#pragma omp parallel for schedule(static)
for (int threadId = 0; threadId < numThreads; ++threadId) {
    int zMin = threadId * slicePerThread;
    int zMax = (threadId == numThreads-1) ? rz : zMin + slicePerThread;
    voxelizeSlice(verts, nbVerts, tris, nbTris, colors, materials, voxels, zMin, zMax);
}
```

> **Рекомендация:** начать с Варианта B — он быстрее и не требует атомарных операций.

### 3.3 Параллельный `floodFill()` — сложно параллелить BFS

BFS последовательен по природе. Оптимизации:
- Заменить `std::vector<bool>` на `std::vector<uint8_t>` (~2× быстрее)  
- Зарезервировать стек сразу на `datalen / 4`
- Итеративная frontier-based параллелизация (сложно, отложить)

---

## 4. Шаг 2 — Алгоритмические оптимизации

### 4.1 Убрать `string`-хэш в `marchingCubesReconstruct()`

Текущий код:
```cpp
char hashBuf[128];
std::sprintf(hashBuf, "%.7g+%.7g+%.7g", tmpV[0], tmpV[1], tmpV[2]);
std::string hash(hashBuf);
auto it = mapVertices.find(hash);
```

Это **главный bottleneck** Marching Cubes. Вершины на рёбрах вокселей имеют предсказуемые индексы — можно использовать целочисленный ключ.

**Замена на плотный массив edge→vertex:**

Каждое ребро куба определяется парой (`cell_index`, `edge_direction`). Для воксельной сетки rx×ry×rz существует ровно:
- `(rx-1)*ry*rz` рёбер вдоль X
- `rx*(ry-1)*rz` рёбер вдоль Y  
- `rx*ry*(rz-1)` рёбер вдоль Z

```cpp
// Массив индексов вершин по рёбрам (uint32_t, инициализирован 0xFFFFFFFF)
std::vector<uint32_t> edgeVertX((rx-1)*ry*rz,   0xFFFFFFFF);
std::vector<uint32_t> edgeVertY( rx*(ry-1)*rz,   0xFFFFFFFF);
std::vector<uint32_t> edgeVertZ( rx*ry*(rz-1),   0xFFFFFFFF);

// Вместо string hash:
int edgeId = getEdgeId(x, y, z, edgeDir, rx, ry, rz);
uint32_t& slot = edgeVert[edgeId];
if (slot == 0xFFFFFFFF) {
    slot = newVertexIndex;
    // push vertex...
}
edges[k] = slot;
```

**Результат:** O(1) lookup без аллокации строк — ускорение Marching Cubes в 5–20×.

### 4.2 Убрать `sqrt` в `voxelize()` там, где не нужен

Текущий код:
```cpp
double newDist = distance2PointTriangleEdges(...);
newDist = std::sqrt(newDist);  // ← дорого
if (newDist < voxels.distanceField[n]) { ... }
```

Заменить на сравнение квадратов расстояний, `sqrt` брать только при записи:

```cpp
double newDistSq = distance2PointTriangleEdges(...);
double& storedDist = voxels.distanceField[n];
double storedDistSq = storedDist * storedDist;

if (newDistSq < storedDistSq) {
    voxels.distanceField[n] = (float)std::sqrt(newDistSq);
    // update color/material
}
```

> Инициализировать `distanceField` значением `+inf` — `sqrt(inf) = inf`, так что начальное сравнение `inf² > newDistSq` всегда верно.

### 4.3 Убрать `colorField`/`materialField` как `unordered_map`

Текущая реализация хранит цвет/материал в sparse `unordered_map<int, uint32_t>` — каждый lookup O(1) среднее, но с большой константой и cache miss.

**Заменить на плотный массив с флагом:**

```cpp
std::vector<uint32_t> colorField;   // datalen элементов, 0 = нет
std::vector<uint32_t> materialField;
```

Инициализировать нулями (нет цвета). При записи просто `colorField[n] = packed_color`. При чтении `if (colorField[n] != 0) { ... }`.

---

## 5. Шаг 3 — Асинхронный remesh + ImGui Progress Modal

### 5.1 Прогресс-стейт (thread-safe)

Добавить в `GuiManager.h`:

```cpp
// Async remesh state
enum class RemeshState { Idle, Running, Done, Error };

struct RemeshProgress {
    std::atomic<RemeshState> state { RemeshState::Idle };
    std::atomic<int>  stage     { 0 };   // 0=voxelize, 1=floodfill, 2=reconstruct
    std::atomic<int>  progress  { 0 };   // 0–100
    std::string       stageLabel;        // read only from main thread after Done
    RemeshResult      result;            // written by worker, read by main after Done
    std::thread       worker;
    std::mutex        labelMutex;
};

RemeshProgress m_remeshAsync;
```

### 5.2 Callback-прогресс в `doRemesh()`

Изменить сигнатуру:

```cpp
RemeshResult doRemesh(
    ...,
    std::function<void(int stage, int progress)> onProgress = nullptr
);
```

Внутри `voxelize()` — каждые N треугольников:
```cpp
if (onProgress && (iTri % 1000 == 0)) {
    int pct = (iTri * 100) / nbTris;
    onProgress(0, pct);
}
```

В `floodFill()` — после окончания BFS:
```cpp
if (onProgress) onProgress(1, 100);
```

В `marchingCubesReconstruct()` / `surfaceNetsReconstruct()` — по слоям Z:
```cpp
if (onProgress && (z % 10 == 0)) {
    int pct = (z * 100) / (rz - 1);
    onProgress(2, pct);
}
```

### 5.3 Запуск в отдельном потоке

```cpp
void GuiManager::performRemeshAsync(Scene& scene) {
    if (m_remeshAsync.state == RemeshState::Running) return;

    Mesh* mesh = scene.getSelected();
    if (!mesh) return;

    scene.pushHistoryState();

    // Snapshot mesh data for worker thread (избегаем race conditions)
    auto verts     = mesh->verts;
    auto faces     = MeshUtils::triangulate(*mesh);
    auto colors    = mesh->colors;
    auto materials = mesh->materials;
    int  nbVerts   = mesh->nbVerts;
    float bbox[6]; mesh->computeBbox(bbox);
    float resolution = (float)m_remeshResolution;
    // ...прочие параметры

    m_remeshAsync.state    = RemeshState::Running;
    m_remeshAsync.stage    = 0;
    m_remeshAsync.progress = 0;

    m_remeshAsync.worker = std::thread([this, &scene, mesh,
        verts, faces, colors, materials, nbVerts,
        bboxArr = std::array<float,6>{bbox[0],bbox[1],bbox[2],bbox[3],bbox[4],bbox[5]},
        resolution]() mutable
    {
        try {
            auto result = doRemesh(
                verts.data(), nbVerts,
                faces.data(), faces.size()/3,
                colors.empty() ? nullptr : colors.data(),
                materials.empty() ? nullptr : materials.data(),
                bboxArr.data(),
                resolution,
                false, false, false,
                uniformColor, uniformMaterial,
                !colors.empty(), !materials.empty(),
                [this](int stage, int pct) {
                    m_remeshAsync.stage    = stage;
                    m_remeshAsync.progress = pct;
                }
            );
            m_remeshAsync.result = std::move(result);
            m_remeshAsync.state  = RemeshState::Done;
        } catch (...) {
            m_remeshAsync.state = RemeshState::Error;
        }
    });
    m_remeshAsync.worker.detach();
}
```

### 5.4 Применение результата на главном потоке

В начале `GuiManager::render()`:

```cpp
if (m_remeshAsync.state == RemeshState::Done) {
    m_remeshAsync.state = RemeshState::Idle;
    applyRemeshResult(scene, m_remeshAsync.result);
}
```

```cpp
void GuiManager::applyRemeshResult(Scene& scene, const RemeshResult& r) {
    Mesh* mesh = scene.getSelected();
    if (!mesh) return;
    mesh->verts     = r.vertices;
    mesh->faces     = r.faces;
    mesh->colors    = r.colors;
    mesh->materials = r.materials;
    mesh->nbVerts   = r.vertices.size() / 3;
    mesh->nbFaces   = r.faces.size() / 4;
    // recompute topology + postInit
    computeTopologyAndPost(mesh);
}
```

### 5.5 ImGui Progress Modal

```cpp
void GuiManager::drawRemeshProgressModal() {
    if (m_remeshAsync.state != RemeshState::Running) return;

    ImGui::OpenPopup("Remesh In Progress");
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Remesh In Progress", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
    {
        static const char* stageNames[] = {
            "Stage 1/3: Voxelizing...",
            "Stage 2/3: Flood Fill...",
            "Stage 3/3: Reconstructing surface..."
        };
        int stage = m_remeshAsync.stage.load();
        int pct   = m_remeshAsync.progress.load();

        ImGui::Text("%s", stageNames[std::clamp(stage, 0, 2)]);
        ImGui::Spacing();

        // Animated progress bar
        float fraction = pct / 100.0f;
        char overlay[32];
        std::snprintf(overlay, sizeof(overlay), "%d%%", pct);
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay);

        ImGui::Spacing();
        ImGui::TextDisabled("Please wait, do not close the application...");

        ImGui::EndPopup();
    }
}
```

Вызвать `drawRemeshProgressModal()` в конце `render()`, перед `ImGui::Render()`.

> **Важно:** пока идёт ремеш — блокировать все действия с мешем (sculpting, undo, remesh повторно). Это достигается проверкой `m_remeshAsync.state == Running` в соответствующих хендлерах.

---

## 6. Порядок реализации (приоритет)

| # | Задача | Сложность | Выигрыш |
|---|--------|-----------|---------|
| 1 | **Async remesh + progress modal** | Средняя | UI не фризится |
| 2 | **Убрать string hash в MC** | Низкая | 5–20× MC ускорение |
| 3 | **Заменить `vector<bool>` на `uint8_t`** | Минимальная | ~2× floodFill |
| 4 | **OpenMP в `voxelize()`** (Variant B по Z-слоям) | Средняя | N_cores× voxelize |
| 5 | **Убрать `sqrt` в voxelize inner loop** | Низкая | ~15–30% voxelize |
| 6 | **Плотный массив для color/material** | Низкая | уменьшает cache miss |

---

## 7. Ожидаемые результаты

- **UI**: Интерфейс не фризится — прогресс-модал показывает состояние.
- **Voxelize**: ×N_cores ускорение (4–16×) на многоядерных CPU.
- **Marching Cubes**: 5–20× ускорение после замены string hash.
- **Общее время remesh** при resolution=200: с ~5–15 сек → до ~0.5–2 сек.

---

## 8. Зависимости / Threading Safety

- Вся работа с `Mesh*` после remesh — **только на главном потоке** (в `applyRemeshResult`).
- Worker thread получает **snapshot** (копии векторов), не работает с живым `Mesh*`.
- Прогресс-атомики (stage, progress) безопасны для чтения с главного потока.
- `RemeshResult` (векторы) передаётся через `std::move` уже после `state = Done`, что даёт happens-before гарантию.
