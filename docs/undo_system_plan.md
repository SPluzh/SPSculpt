# План: Высокопроизводительная система Undo/Redo для sculptsp-native

## Диагностика текущей системы

### Что есть сейчас

В `Scene.cpp` реализован простой full-snapshot подход:

```cpp
// Scene.cpp:58 — каждое pushHistoryState() копирует ВСЁ
HistoryState Scene::saveCurrentState() const {
    for (auto* m : m_meshes) {
        MeshState ms;
        ms.verts = m->verts;          // nbVerts * 3 * 4 байт
        ms.colors = m->colors;        // nbVerts * 3 * 4 байт
        ms.materials = m->materials;  // nbVerts * 3 * 4 байт
        ms.faces = m->faces;          // nbFaces * 4 * 4 байт
        ms.vrfStartCount = m->vrfStartCount;
        ms.vertRingFace = m->vertRingFace;
        ms.vrvStartCount = m->vrvStartCount;
        ms.vertRingVert = m->vertRingVert;
        ms.vertOnEdge = m->vertOnEdge;
        ms.vertVisible = m->vertVisible;
        // ...
    }
}
```

`pushHistoryState()` вызывается **~30 раз** в `SculptManager.cpp` — в том числе внутри горячих путей обработки инпута.

### Критические проблемы

| Проблема | Масштаб |
|---|---|
| Сфера 100×100 = ~10k вершин | ~1.2 МБ на snapshot |
| После ремеша (high-poly) | **10–40 МБ** на snapshot |
| 30 состояний в стеке | до **1.2 ГБ** для dense mesh |
| Копирование topology при каждом мазке | бессмысленно — topology меняется только при ремеше |
| `erase(begin())` при overflow | O(N) сдвиг всего стека |

> [!CAUTION]
> При активной sculpt-сессии с high-poly мешом система способна съесть гигабайт RAM менее чем за 10 минут, вызывая деградацию производительности через pressure на аллокатор.

---

## Нужны ли изменения в архитектуре?

**Да, но без кардинального переписывания.** Текущая архитектура Scene + SculptManager хорошо разделена. Нужно:

1. **Ввести уровни истории** — разные типы операций требуют разных стратегий хранения
2. **Сделать хранилище умным** — дельты вместо снапшотов для sculpt-операций
3. **Убрать undo из Scene** — выделить в отдельную подсистему `UndoManager`
4. **Добавить аннотацию операций** — чтобы история знала, что именно изменилось

---

## Классификация операций по типу

```
┌─────────────────────────────────────────────────────────────┐
│                    ТИПЫ ОПЕРАЦИЙ                            │
├──────────────────┬──────────────────┬───────────────────────┤
│  SCULPT (paint)  │  TOPOLOGY CHANGE │  META / SCENE CHANGE  │
│                  │                  │                        │
│ • brush stroke   │ • remesh         │ • add/remove mesh      │
│ • mask           │ • subdivide      │ • visibility           │
│ • paint color    │ • merge          │ • rename               │
│                  │ • duplicate      │ • matrix transform     │
├──────────────────┼──────────────────┼───────────────────────┤
│ СТРАТЕГИЯ:       │ СТРАТЕГИЯ:       │ СТРАТЕГИЯ:             │
│ Только дельта    │ Полный снапшот   │ Лёгкий снапшот         │
│ verts/colors/mat │ topology+verts   │ meta-данные            │
│ затронутых вершин│                  │ только                 │
└──────────────────┴──────────────────┴───────────────────────┘
```

---

## Предлагаемая архитектура

### Новая иерархия файлов

```
src/
  editing/
    undo/
      UndoManager.h          ← центральный менеджер (новый)
      UndoManager.cpp
      UndoEntry.h            ← базовый класс записи (новый)
      SculptUndoEntry.h      ← дельта для sculpt (новый)
      TopologyUndoEntry.h    ← снапшот topology (новый)
      SceneMetaUndoEntry.h   ← мета-изменения (новый)
  scene/
    Scene.h                  ← убрать m_undoStack/m_redoStack
    Scene.cpp
```

### Базовый класс UndoEntry

```cpp
// src/editing/undo/UndoEntry.h
#pragma once
#include <cstddef>
#include <string>

enum class UndoEntryType {
    Sculpt,      // дельта вершин
    Topology,    // полный снапшот с topology
    SceneMeta    // мета-данные сцены
};

class UndoEntry {
public:
    virtual ~UndoEntry() = default;
    virtual UndoEntryType getType() const = 0;
    virtual size_t getMemoryUsage() const = 0;
    virtual std::string getDescription() const = 0;
};
```

### SculptUndoEntry — дельта для sculpt-операций

```cpp
// src/editing/undo/SculptUndoEntry.h
#pragma once
#include "UndoEntry.h"
#include <vector>
#include <cstdint>

// Хранит только изменённые вершины
struct VertexDelta {
    uint32_t meshId;
    std::vector<uint32_t> indices;    // индексы изменённых вершин

    // Данные ДО (для undo)
    std::vector<float> prevVerts;     // indices.size() * 3
    std::vector<float> prevColors;    // indices.size() * 3 (если изменены)
    std::vector<float> prevMaterials; // indices.size() * 3 (если изменены)

    // Данные ПОСЛЕ (для redo)
    std::vector<float> nextVerts;
    std::vector<float> nextColors;
    std::vector<float> nextMaterials;

    bool hasColors    = false;
    bool hasMaterials = false;
};

class SculptUndoEntry : public UndoEntry {
public:
    std::vector<VertexDelta> deltas; // по одному на меш

    UndoEntryType getType() const override { return UndoEntryType::Sculpt; }

    size_t getMemoryUsage() const override {
        size_t total = 0;
        for (const auto& d : deltas) {
            total += d.indices.size() * sizeof(uint32_t);
            total += d.prevVerts.size() * sizeof(float) * 2; // prev + next
            if (d.hasColors)    total += d.prevColors.size() * sizeof(float) * 2;
            if (d.hasMaterials) total += d.prevMaterials.size() * sizeof(float) * 2;
        }
        return total;
    }

    std::string getDescription() const override { return "Sculpt stroke"; }
};
```

### TopologyUndoEntry — полный снапшот при topology change

```cpp
// src/editing/undo/TopologyUndoEntry.h
#pragma once
#include "UndoEntry.h"
#include "scene/Scene.h" // для HistoryState

class TopologyUndoEntry : public UndoEntry {
public:
    HistoryState before; // полное состояние сцены
    HistoryState after;

    UndoEntryType getType() const override { return UndoEntryType::Topology; }

    size_t getMemoryUsage() const override {
        size_t total = 0;
        for (const auto& ms : before.meshes)
            total += (ms.verts.size() + ms.colors.size() + ms.materials.size() +
                      ms.faces.size() + ms.vrfStartCount.size() + ms.vertRingFace.size() +
                      ms.vrvStartCount.size() + ms.vertRingVert.size()) * sizeof(float);
        // x2 за after
        return total * 2;
    }

    std::string getDescription() const override { return "Topology change"; }
};
```

### UndoManager — центральный менеджер

```cpp
// src/editing/undo/UndoManager.h
#pragma once
#include "UndoEntry.h"
#include <deque>
#include <memory>
#include <cstddef>

class Scene;
class Mesh;

class UndoManager {
public:
    // Настройки
    static constexpr size_t DEFAULT_MAX_MEMORY = 512 * 1024 * 1024; // 512 МБ
    static constexpr size_t DEFAULT_MAX_ENTRIES = 100;

    UndoManager();

    // --- API для вызова перед операцией ---

    // Для sculpt: передаём список вершин которые БУДУТ изменены
    void beginSculptStroke(Scene& scene,
                           uint32_t meshId,
                           const std::vector<uint32_t>& affectedVerts,
                           bool affectsColors = false,
                           bool affectsMaterials = true);

    // Завершить запись мазка (записать состояние ПОСЛЕ)
    void endSculptStroke(Scene& scene, uint32_t meshId);

    // Для topology-меняющих операций
    void pushTopologyChange(Scene& scene,
                            std::function<void()> operation); // атомарно!

    // Для мета-изменений (имя, видимость, матрица)
    void pushMetaChange(Scene& scene,
                        std::function<void()> operation);

    // --- Undo / Redo ---
    void undo(Scene& scene);
    void redo(Scene& scene);

    bool canUndo() const { return !m_undoStack.empty(); }
    bool canRedo() const { return !m_redoStack.empty(); }

    // --- Диагностика ---
    size_t getTotalMemoryUsage() const;
    size_t getUndoCount() const { return m_undoStack.size(); }
    size_t getRedoCount() const { return m_redoStack.size(); }
    std::string getUndoDescription() const;

    void clear();
    void setMaxMemory(size_t bytes) { m_maxMemory = bytes; }
    void setMaxEntries(size_t n)    { m_maxEntries = n; }

private:
    // deque быстрее vector для pop_front / push_back
    std::deque<std::unique_ptr<UndoEntry>> m_undoStack;
    std::deque<std::unique_ptr<UndoEntry>> m_redoStack;

    size_t m_maxMemory  = DEFAULT_MAX_MEMORY;
    size_t m_maxEntries = DEFAULT_MAX_ENTRIES;

    // Текущий открытый sculpt entry (для begin/end pattern)
    std::unique_ptr<SculptUndoEntry> m_activeSculptEntry;
    uint32_t m_activeMeshId = 0;

    void pushEntry(std::unique_ptr<UndoEntry> entry);
    void trimToMemoryLimit();
    void applyEntry(UndoEntry* entry, Scene& scene, bool isUndo);
};
```

---

## Алгоритм работы для sculpt-мазков

```
Pen Down
    │
    ▼
UndoManager::beginSculptStroke(scene, meshId, affectedVerts)
    │  → Читает ТЕКУЩИЕ значения verts[affectedVerts] → prevVerts
    │  → Создаёт m_activeSculptEntry
    │
    ▼
[ SculptManager делает N шагов кисти ... ]
    │  (vert positions меняются непосредственно в Mesh)
    │
    ▼
Pen Up
    │
    ▼
UndoManager::endSculptStroke(scene, meshId)
    │  → Читает НОВЫЕ значения verts[affectedVerts] → nextVerts
    │  → Если prevVerts == nextVerts (нет изменений) → отбрасывает
    │  → Иначе pushEntry(m_activeSculptEntry)
    │
    ▼
m_undoStack.push_back(entry)
trimToMemoryLimit()
```

### Экономия памяти на практике

Для сферы 100×100 (~10k вершин), типичный мазок затрагивает ~500-2000 вершин:

| Параметр | Старый подход | Новый (дельта) |
|---|---|---|
| Полный снапшот | 1.2 МБ | — |
| 500 вершин, verts only | — | **24 КБ** |
| 2000 вершин, verts+mat | — | **144 КБ** |
| Экономия | 100% | **~95–98%** |

---

## Изменения в архитектуре

### Scene.h — упростить

```diff
- std::vector<HistoryState> m_undoStack;
- std::vector<HistoryState> m_redoStack;
- size_t m_maxHistoryStates = 30;
- HistoryState saveCurrentState() const;
- void restoreState(const HistoryState& state);

+ // Оставить только saveCurrentState() / restoreState()
+ // для использования TopologyUndoEntry
+ friend class UndoManager; // доступ к internals
```

Публичные методы `undo()`, `redo()`, `pushHistoryState()`, `canUndo()`, `canRedo()` — **убрать из Scene**, перенести в `UndoManager`.

### SculptManager.cpp — главные изменения

```diff
- scene.pushHistoryState();   // УДАЛИТЬ все ~25 вызовов

+ // В начале pen-down обработки:
+ g_undoManager.beginSculptStroke(scene, mesh->getID(), iVerts, ...);

+ // В конце pen-up обработки:
+ g_undoManager.endSculptStroke(scene, mesh->getID());
```

### GuiManager.cpp — для topology-операций

```diff
- scene.pushHistoryState();
- scene.addSphere();

+ g_undoManager.pushTopologyChange(scene, [&]() {
+     scene.addSphere();
+ });
```

---

## Быстрое применение дельты при Undo/Redo

```cpp
void UndoManager::applyEntry(UndoEntry* entry, Scene& scene, bool isUndo) {
    if (entry->getType() == UndoEntryType::Sculpt) {
        auto* e = static_cast<SculptUndoEntry*>(entry);
        for (auto& delta : e->deltas) {
            Mesh* mesh = scene.getMeshById(delta.meshId);
            if (!mesh) continue;
            const auto& srcVerts = isUndo ? delta.prevVerts : delta.nextVerts;
            // Прямая запись по индексам — O(N_affected), не O(N_total)
            for (size_t i = 0; i < delta.indices.size(); ++i) {
                uint32_t vi = delta.indices[i];
                mesh->verts[vi*3+0] = srcVerts[i*3+0];
                mesh->verts[vi*3+1] = srcVerts[i*3+1];
                mesh->verts[vi*3+2] = srcVerts[i*3+2];
            }
            // То же для colors / materials если нужно
            mesh->isDirty = true;
            // Нужно пересчитать нормали для затронутых вершин
            // Можно сделать через dirtyVertMin/Max range
            mesh->dirtyVertMin = *std::min_element(delta.indices.begin(), delta.indices.end());
            mesh->dirtyVertMax = *std::max_element(delta.indices.begin(), delta.indices.end());
            mesh->isVertexDirty = true;
        }
    }
    else if (entry->getType() == UndoEntryType::Topology) {
        auto* e = static_cast<TopologyUndoEntry*>(entry);
        scene.restoreState(isUndo ? e->before : e->after);
    }
    // SceneMeta — аналогично
}
```

---

## Управление памятью — стратегия вытеснения

```cpp
void UndoManager::trimToMemoryLimit() {
    // Сначала по количеству
    while (m_undoStack.size() > m_maxEntries) {
        m_undoStack.pop_front(); // O(1) для deque
    }

    // Затем по памяти — вытесняем старые
    while (getTotalMemoryUsage() > m_maxMemory && !m_undoStack.empty()) {
        m_undoStack.pop_front();
    }
}

size_t UndoManager::getTotalMemoryUsage() const {
    size_t total = 0;
    for (const auto& e : m_undoStack)  total += e->getMemoryUsage();
    for (const auto& e : m_redoStack)  total += e->getMemoryUsage();
    return total;
}
```

---

## Дополнительные оптимизации (опционально)

### 1. Сжатие дельт (lz4 / zstd)
Вершинные дельты хорошо сжимаются — float-данные повторяются.
```cpp
// Можно добавить сжатие при push, распаковку при apply
// lz4 даёт x3-5 на float буферах, latency < 1ms на 100k вершин
```

### 2. Ленивое копирование (Copy-on-Write для редких операций)
Topology-снапшоты можно хранить как `shared_ptr<MeshState>` и копировать только при реальном изменении.

### 3. Объединение мазков (stroke merging)
Несколько мелких мазков подряд по той же области → объединять в один entry. Реализуется через `timeThreshold` и `spatialThreshold`.

### 4. Пересчёт нормалей только для dirty-диапазона
Уже есть `dirtyVertMin/Max` в `Mesh`. При undo нужно пересчитать нормали только для конкретного диапазона, а не для всего меша.

---

## Поэтапный план реализации

### Этап 1 — Фундамент (без поломки текущего) [~2-4 ч]
1. Создать `src/editing/undo/` директорию
2. Написать `UndoEntry.h`, `SculptUndoEntry.h`, `TopologyUndoEntry.h`
3. Написать `UndoManager.h` и `UndoManager.cpp` (скелет без интеграции)
4. Оставить старый `Scene::pushHistoryState()` работающим

### Этап 2 — Интеграция для sculpt-мазков [~3-5 ч]
1. Добавить `getMeshById(uint32_t id)` в `Scene`
2. Найти в `SculptManager.cpp` места начала/конца `pen down/up` (события ввода)
3. Вставить `beginSculptStroke` / `endSculptStroke` вместо `pushHistoryState()`
4. Удалить старые вызовы `pushHistoryState()` из sculpt-кода
5. Протестировать: должно работать для sculpt

### Этап 3 — Интеграция для topology-операций [~2-3 ч]
1. Обернуть `addSphere`, `remesh`, `duplicate`, `merge` и т.д. в `pushTopologyChange`
2. Обернуть мета-изменения в `pushMetaChange`
3. Удалить `pushHistoryState()` из `GuiManager.cpp`

### Этап 4 — Удаление старой системы [~1 ч]
1. Убрать `m_undoStack`, `m_redoStack` из `Scene`
2. Оставить `saveCurrentState()` / `restoreState()` как private helpers для `UndoManager`
3. Убрать `canUndo()`, `canRedo()`, `undo()`, `redo()` из `Scene`
4. Обновить `GuiManager.cpp` — использовать `g_undoManager.canUndo()` и т.д.

### Этап 5 — UI и диагностика [~1-2 ч]
1. Добавить в GUI панель "История" с описанием операций и объёмом памяти
2. Настройка лимита памяти в `gui_settings.cfg`

---

## Итоговые ожидаемые результаты

| Метрика | До | После |
|---|---|---|
| RAM для 30 sculpt-состояний (10k verts) | ~36 МБ | **~4 МБ** |
| RAM для 30 sculpt-состояний (high-poly) | ~1.2 ГБ | **~50 МБ** |
| Время `pushHistoryState()` sculpt | ~5-20 мс | **< 0.5 мс** |
| Время `undo()` для sculpt | ~10-50 мс | **< 2 мс** |
| Topology undo | без изменений | без изменений |
| Код `Scene` | сложнее | **проще** |
