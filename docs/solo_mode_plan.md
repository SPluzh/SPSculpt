# Plan: Solo Mode

> **Суть:** нажатие кнопки/хоткея «Solo» изолирует выбранный меш —
> все остальные объекты скрываются из рендера. Повторное нажание
> или снятие выделения возвращает исходное состояние.  
> **`visibleV1` / `visibleV2`** (глазики аутлайнера) **не трогаем** —
> они продолжают работать независимо.

---

## Концепция состояния

Solo — это **временное наложение** поверх `visibleV1`/`visibleV2`.
Рендер уже спрашивает `mesh->visibleV1` и `mesh->visibleV2`,
поэтому самый чистый способ — завести на уровне `Scene` флаг
`m_soloMeshId` (id солированного меша, или `0` = не активно)
и **модифицировать не сами поля меша**, а добавить метод
`Scene::isMeshVisibleInSolo(Mesh*)`, который рендер будет вызывать.

Таким образом:
- Аутлайнер показывает/скрывает глазики как всегда.
- Solo не мутирует `visibleV1`/`visibleV2`.
- При отключении solo каждый меш остаётся в том состоянии,
  в котором был до solo.

---

## Шаг 1 — `Scene.h` / `Scene.cpp`

### 1.1 Добавить поле и методы в `Scene`

**Файл:** `src/scene/Scene.h`

```cpp
// В секцию private:
uint32_t m_soloMeshId = 0; // 0 = solo не активно

// В секцию public:
bool isSoloActive() const { return m_soloMeshId != 0; }
uint32_t getSoloMeshId() const { return m_soloMeshId; }

/// Солирует mesh, если он ещё не солирован; иначе выходит из solo.
void toggleSolo(Mesh* mesh);

/// Возвращает true, если меш должен быть виден с учётом solo-наложения.
/// Рендер должен вызывать эту функцию ВМЕСТО прямого чтения visibleV1.
bool isMeshRenderVisible(const Mesh* mesh) const;
```

### 1.2 Реализация `toggleSolo` и `isMeshRenderVisible`

**Файл:** `src/scene/Scene.cpp`

```cpp
void Scene::toggleSolo(Mesh* mesh) {
    if (!mesh) {
        m_soloMeshId = 0;
        return;
    }
    if (m_soloMeshId == mesh->getID()) {
        m_soloMeshId = 0; // выход из solo
    } else {
        m_soloMeshId = mesh->getID(); // вход в solo
    }
}

bool Scene::isMeshRenderVisible(const Mesh* mesh) const {
    if (!mesh) return false;
    // Если solo активно — виден только солированный меш
    if (m_soloMeshId != 0) {
        return mesh->getID() == m_soloMeshId;
    }
    // Иначе — стандартные флаги аутлайнера
    return mesh->visibleV1;
}
```

> **Примечание о `visibleV2`:** `visibleV2` используется как второй
> независимый слой видимости (например, X-ray или маска). Его solo
> тоже не трогает — рендер уже имеет отдельную ветку для V2.

---

## Шаг 2 — Интеграция в `AngleRenderer`

Найти все места в `AngleRenderer.cpp`, где меш пропускается через
`mesh->visibleV1` (или аналогичную проверку перед рендером),
и заменить на вызов `scene.isMeshRenderVisible(mesh)`.

**Паттерн поиска:** `visibleV1` в `AngleRenderer.cpp`
(по результатам первичного grep — там 0 результатов,
значит рендер, скорее всего, итерирует `scene.getMeshes()`
без прямого чтения флага *либо* читает его через Mesh*.
Нужно найти точное место итерации и добавить guard).

Типичный вид изменения:
```cpp
// БЫЛО:
for (Mesh* mesh : scene.getMeshes()) {
    if (!mesh->visibleV1) continue;
    // ... render mesh
}

// СТАЛО:
for (Mesh* mesh : scene.getMeshes()) {
    if (!scene.isMeshRenderVisible(mesh)) continue;
    // ... render mesh
}
```

**Действие:** `Select-String AngleRenderer.cpp -Pattern "visibleV1|getMeshes"`
чтобы найти конкретные строки перед реализацией.

---

## Шаг 3 — Кнопка Solo в Floating Island HUD

**Файл:** `src/gui/GuiManager.cpp`  
**Функция:** `drawFloatingIslandHUD()` (~строка 4207)

Кнопка добавляется **в конец HUD**, после wireframe-кнопки,
с разделителем `|`.

```cpp
// После блока showWireframe (строка ~4420):
ImGui::SameLine();
ImGui::TextDisabled("|");
ImGui::SameLine();

bool soloActive = scene.isSoloActive();
Mesh* selectedMesh = scene.getSelected();

if (soloActive) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.85f, 0.55f, 0.05f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.65f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.70f, 0.40f, 0.00f, 1.0f));
}

// Кнопка активна только если есть выбранный меш ИЛИ solo уже включён
if (!selectedMesh && !soloActive) ImGui::BeginDisabled();
if (ImGui::Button(ICON_LC_EYE "##hudSolo")) {
    scene.toggleSolo(scene.getSelected());
}
if (!selectedMesh && !soloActive) ImGui::EndDisabled();
if (ImGui::IsItemHovered()) ImGui::SetTooltip("Solo Selected Object (C)");

if (soloActive) {
    ImGui::PopStyleColor(3);
}
```

> Цвет кнопки — янтарный/оранжевый, чтобы визуально отличаться от
> тилового цвета активных кнопок симметрии. Solo — это особый режим.

**Иконка:** `ICON_LC_EYE` (Lucide `eye`) уже подключён через `IconsLucide.h`.
Если нужна иконка "одиночного объекта" — альтернатива `ICON_LC_FOCUS`.

---

## Шаг 4 — Хоткей **C**

### 4.1 `HotkeyDispatcher.h` — добавить `HKAction::SoloSelected`

```cpp
// В enum HKAction, секция "Misc":
SoloSelected,          // C (toggle)
```

### 4.2 `HotkeyDispatcher.cpp` — `mapKeyToAction`

```cpp
// В секцию "Normal keys", switch(sym):
case SDLK_c: return HKAction::SoloSelected;
```

### 4.3 `HotkeyDispatcher.cpp` — `executeAction`, ветка `isDown`

```cpp
case HKAction::SoloSelected: {
    Mesh* sel = scene.getSelected();
    // Если solo уже активно на другом меше — выходим из solo.
    // Если sel == nullptr и solo активно — тоже выходим.
    scene.toggleSolo(sel);
    break;
}
```

> Клавиша **C** сейчас свободна (не занята ни одним HKAction).

---

## Шаг 5 — Индикатор Solo в аутлайнере *(опционально, рекомендуется)*

Чтобы пользователь видел, какой меш солирован, в таблице аутлайнера
(строка ~1424) добавить маленькую иконку `ICON_LC_EYE` в столбец Name
рядом с именем меша, если он является солированным:

```cpp
// Перед ImGui::Selectable в колонке Name:
if (scene.isSoloActive() && mesh->getID() == scene.getSoloMeshId()) {
    ImGui::TextColored(ImVec4(0.85f, 0.55f, 0.05f, 1.0f), ICON_LC_EYE " ");
    ImGui::SameLine();
}
```

---

## Порядок выполнения

| # | Файл | Изменение |
|---|------|-----------|
| 1 | `src/scene/Scene.h` | + поле `m_soloMeshId`, методы `toggleSolo`, `isSoloActive`, `getSoloMeshId`, `isMeshRenderVisible` |
| 2 | `src/scene/Scene.cpp` | + реализация `toggleSolo` и `isMeshRenderVisible` |
| 3 | `src/render/AngleRenderer.cpp` | заменить прямые `visibleV1`-гарды на `scene.isMeshRenderVisible(mesh)` |
| 4 | `src/platform/HotkeyDispatcher.h` | + `SoloSelected` в `enum HKAction` |
| 5 | `src/platform/HotkeyDispatcher.cpp` | + `SDLK_c` → `SoloSelected` в `mapKeyToAction`; + обработка в `executeAction` |
| 6 | `src/gui/GuiManager.cpp` | + кнопка Solo в `drawFloatingIslandHUD`; *(опц.)* иконка в аутлайнере |

---

## Инварианты

- Solo **не мутирует** `visibleV1`/`visibleV2`. Глазики аутлайнера работают независимо.
- Выход из Solo: повторное нажание **C** / кнопки, или удаление солированного меша из сцены (в `Scene::removeMesh` надо сбросить `m_soloMeshId`).
- Solo персистить в конфиге **не нужно** — это сессионное состояние.
- Если выбранный меш меняется, solo **не сбрасывается** автоматически (ZBrush-поведение: solo остаётся до явного выключения).
