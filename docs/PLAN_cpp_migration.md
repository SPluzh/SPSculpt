# SculptSP → C++ Migration (Always-Working Plan)

> **Принцип**: после каждого шага приложение **полностью работает** — скульптинг, GUI, файлы.
> JS постепенно заменяется C++, а не всё сразу.

---

## Карта замен

```
Шаг 1 ── Dual-build CI         → текущий JS-app + новый native exe работают параллельно
Шаг 2 ── Mesh.cpp              → WasmBridge.js исчезает, скульпт работает через C++
Шаг 3 ── Scene/Camera C++      → JS-Scene делегирует в C++, GUI не трогаем
Шаг 4 ── Renderer C++          → один рендерер для WASM и native
Шаг 5 ── WasmMain entrypoint   → первый полный WASM-build без JS-оркестрации
Шаг 6 ── ImGui GUI             → заменяем JS-панели одну за одной, остальные живут
Шаг 7 ── Удаление JS           → выбрасываем мёртвый код
```

---

## Шаг 0 — Подготовка (1 день)
**✅ Рабочее**: всё как сейчас, плюс нативный exe запускается рядом.

### Задачи
- `git tag baseline-js` — зафиксировать точку отката
- Настроить `CMakeLists.txt` dual-target (уже почти готов):
  - `EMSCRIPTEN` → `sculptsp_core.wasm` (текущий)
  - `else` → `sculptsp.exe` (native SDL2+ANGLE, `NativeMain.cpp`)
- Добавить `FetchContent` для ImGui и nlohmann_json в CMake (они пока не собираются в app)
- Убедиться что оба target собираются без ошибок: `build_wasm.bat` + `build_native.bat`

### Проверка работоспособности
```
browser: открыть index.html → скульптинг работает (JS + старый WASM)
native:  run_native.bat → открывается окно со сферой, кисти работают
```

---

## Шаг 1 — `Mesh.cpp`: убираем malloc из JS (3–5 дней)
**✅ Рабочее**: браузер — полный скульптинг. WasmBridge.js перестаёт управлять памятью.

### Проблема сейчас
`WasmBridge.allocMeshBuffers()` делает ~30 вызовов `_malloc` на каждый меш.
JS хранит `uintptr_t` и вручную проверяет detached ArrayBuffer каждый кадр.

### Что делаем

**1.1** Выделить `Topology.cpp` из `NativeMain.cpp`:
```cpp
// src-wasm/core/Topology.h
void computeTopology(int nbVerts, const uint32_t* faces, int nbFaces,
    std::vector<uint32_t>& vrfStartCount, std::vector<uint32_t>& vertRingFace,
    std::vector<uint32_t>& vrvStartCount, std::vector<uint32_t>& vertRingVert,
    std::vector<uint8_t>& vertOnEdge);
```

**1.2** Создать `MeshCpp` — C++ класс меша со всеми буферами в `std::vector`:
```cpp
// src-wasm/mesh/MeshCpp.h
class MeshCpp {
public:
    std::vector<float>    verts, normals, colors, materials, faceNormals, faceBoxes, faceCenters, vertProxy;
    std::vector<uint32_t> faces, vrfStartCount, vertRingFace, vrvStartCount, vertRingVert;
    std::vector<uint8_t>  vertOnEdge, vertVisible;
    // Методы работают с этими буферами напрямую — нет копирования
    void updateGeometry(const uint32_t* iFaces, int nFaces, const uint32_t* iVerts, int nVerts);
    void buildOctree();
    // ...
};
```

**1.3** Экспортировать через Emscripten как объект (не функции):
```cpp
// Bindings.cpp — добавить
class_<MeshCpp>("MeshCpp")
    .constructor<>()
    .function("init",           &MeshCpp::init)
    .function("updateGeometry", &MeshCpp::updateGeometry)
    .function("getVertsPtr",    &MeshCpp::getVertsPtr)   // uintptr_t
    .function("getFacesPtr",    &MeshCpp::getFacesPtr)
    // ... остальные getXxxPtr()
```

**1.4** Упростить `WasmBridge.js`:
```javascript
// БЫЛО: allocMeshBuffers() — 30 malloc, ручные typed array views
// СТАЛО:
this._meshCpp = new this._wasm.MeshCpp();
this._meshCpp.init(nbVerts, nbFaces, vrfData, vrfStartCount, ...);
// Вся память теперь в C++ std::vector — нет detached buffer проблем
// JS читает данные только через getVertsPtr() когда нужно для GPU upload
```

**1.5** JS-GPU upload — единственный оставшийся мост:
```javascript
// В AngleRenderer.js (JS) или напрямую:
const ptr = this._meshCpp.getVertsPtr();
const verts = new Float32Array(this._wasm.HEAPF32.buffer, ptr, nbVerts * 3);
gl.bufferData(gl.ARRAY_BUFFER, verts, gl.DYNAMIC_DRAW); // zero-copy
```

### Проверка работоспособности
```
browser: скульптинг работает, нет "checkMemoryGrowth" предупреждений в консоли
         WasmBridge.js — уменьшился с 82 KB до ~20 KB (нет allocMeshBuffers)
native:  exe запускается, кисти работают со сферой
```

---

## Шаг 2 — `Scene.cpp` + `Camera.cpp` (2–3 дня)
**✅ Рабочее**: скульптинг, GUI, undo/redo работают. JS-Scene делегирует в C++.

### Стратегия: делегирование, не замена
JS `Scene.js` остаётся, но его методы вызывают C++-объект:
```javascript
// Scene.js — добавить
this._cppScene = new this._wasm.SceneCpp();

addMesh(mesh) {
    // ...существующий JS код...
    this._cppScene.addMesh(mesh._meshCpp); // новое
}
```

### Что переносим в C++

**2.1** `SceneCpp` — список мешей, selection:
```cpp
class SceneCpp {
    std::vector<MeshCpp*> meshes;
    int selectedIdx = -1;
public:
    void addMesh(MeshCpp* m);
    MeshCpp* getSelected();
};
```

**2.2** `CameraCpp` — trackball + `getRay()`:
- Порт `Camera.js` (~500 LOC) → `Camera.h/cpp`
- **Критично**: `getRay(mouseX, mouseY)` — closed-form без matrix inversion (из gemini.md)
- Сначала: C++ Camera используется только в native exe
- Потом: JS Camera вызывает `CameraCpp` через bindings

**2.3** `StateSnapshot` — undo/redo дельты:
```cpp
struct VertDelta { uint32_t id; float before[3]; float after[3]; };
struct StateSnapshot { std::vector<VertDelta> deltas; };
```

### Проверка работоспособности
```
browser: полный скульптинг, undo/redo (Ctrl+Z/Y), переключение мешей
native:  камера работает trackball-ом, кисти деформируют сферу
```

---

## Шаг 3 — Renderer C++ (3–4 дня)
**✅ Рабочее**: WASM и native используют один рендерер. GPU-upload без JS.

### Стратегия
Текущий `AngleRenderer.cpp` (7.5 KB) расширяем до полного рендерера.
JS WebGL код (`src/render/`) остаётся как fallback до конца Шага 5.

### Что добавляем в `AngleRenderer.cpp`

**3.1** PBR шейдеры (порт из `src/render/shaders/`):
```cpp
// Шейдеры как constexpr string (или --embed-file для WASM)
static const char* PBR_VERT_SRC = R"glsl(
    attribute vec3 a_pos;
    attribute vec3 a_normal;
    // ...
)glsl";
```

**3.2** Multi-mesh render loop:
```cpp
void AngleRenderer::render(const SceneCpp& scene) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    for (auto* mesh : scene.getMeshes()) {
        uploadIfDirty(mesh);      // incremental VBO upload
        drawMesh(mesh, scene.getCamera());
    }
    drawSelectionCursor();        // cursor circle
    drawBackground();             // gradient bg
}
```

**3.3** `uploadIfDirty()` — incremental GPU upload:
```cpp
void uploadIfDirty(MeshCpp* mesh) {
    if (!mesh->isDirty) return;
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo);
    // Если только часть вершин изменилась — glBufferSubData
    if (mesh->dirtyRange.valid()) {
        glBufferSubData(GL_ARRAY_BUFFER, mesh->dirtyRange.offset,
                        mesh->dirtyRange.size, mesh->verts.data() + ...);
    } else {
        glBufferData(GL_ARRAY_BUFFER, ...);
    }
    mesh->isDirty = false;
}
```

**3.4** Новый `buildDrawArraysFast` через C++ — убрать из `Bindings.cpp`, переместить в `AngleRenderer`.

### Для WASM target
Добавить в `WasmMain.cpp` (новый файл-заглушка):
```cpp
// platform/WasmMain.cpp — пока просто инициализация
extern "C" void sculptsp_render_frame() {
    g_renderer.render(g_scene);
}
```
JS вызывает `_sculptsp_render_frame()` в своём `requestAnimationFrame`.

### Проверка работоспособности
```
browser: рендер через C++ AngleRenderer (JS RAf вызывает C++ render)
         GUI по-прежнему JS, скульптинг работает
native:  полный рендер со сферой, PBR материалы, wireframe
```

---

## Шаг 4 — WasmMain.cpp: первый полный WASM без JS-оркестрации (2–3 дня)
**✅ Рабочее**: браузер открывает полноценный C++ app. JS — 50 строк.

### Что делаем

**4.1** Новый `platform/WasmMain.cpp` — полный event loop:
```cpp
#include <emscripten.h>
#include <SDL2/SDL.h>
#include "../scene/SceneCpp.h"
#include "../render/AngleRenderer.h"
#include "../editing/SculptManager.h"
#include "../gui/GuiManager.h"   // ImGui — пока пустой

static SceneCpp       g_scene;
static AngleRenderer  g_renderer;
static SculptManager  g_sculpt;
static GuiManager     g_gui;    // подключается в Шаге 5

static void mainLoop() {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        g_sculpt.handleEvent(e, g_scene);
    g_sculpt.processFrame(g_scene);
    g_renderer.render(g_scene);
    g_gui.render();              // пустой до Шага 5
    SDL_GL_SwapWindow(g_renderer.window);
}

int main() {
    SDL_Init(SDL_INIT_VIDEO);
    // создать WebGL2 контекст через SDL2/Emscripten
    g_renderer.init(1280, 720);
    g_scene.loadDefaultSphere();
    emscripten_set_main_loop(mainLoop, 0, 1);
}
```

**4.2** Тонкий JS-слой (`index_new.js` — работает параллельно со старым):
```javascript
import SculptSPApp from './wasm/sculptsp_app.js';

SculptSPApp().then(m => {
    // canvas resize
    new ResizeObserver(() => {
        m._sculptsp_resize(canvas.clientWidth | 0, canvas.clientHeight | 0);
    }).observe(canvas);
    // file open
    fileInput.onchange = async (e) => {
        const buf = await e.target.files[0].arrayBuffer();
        const ptr = m._malloc(buf.byteLength);
        m.HEAPU8.set(new Uint8Array(buf), ptr);
        m._sculptsp_load_file(ptr, buf.byteLength);
        m._free(ptr);
    };
});
```

**4.3** Переключение через URL-параметр (для тестирования):
```javascript
// index.html
const useCpp = new URLSearchParams(location.search).has('cpp');
if (useCpp) { import('./index_new.js'); }
else         { import('./src/SculptSP.js'); } // старый JS app
```

### Проверка работоспособности
```
browser ?cpp: C++ app работает (скульптинг есть, GUI временно нет)
browser (без ?cpp): старый JS app работает как всегда
native: полноценный exe со всеми кистями
```

---

## Шаг 5 — ImGui GUI (5–7 дней, панели по одной) — ✅ ВЫПОЛНЕНО
**✅ Рабочее на каждой подфазе**: заменяем панели одну за одной.

### Стратегия подключения
ImGui рисует поверх WebGL-canvas через тот же OpenGL контекст — нет HTML-элементов.

```cmake
# CMakeLists.txt
add_subdirectory(third_party/imgui)
target_link_libraries(sculptsp_app PRIVATE imgui)
```

### Порядок замены панелей (по критичности)

| Приоритет | JS-файл | C++ замена | Работает без |
|---|---|---|---|
| 1 | `VerticalToolbar.js` (15 KB) | `Toolbar.cpp` — выбор кисти | Нельзя менять кисть |
| 2 | `GuiSculpting.js` (21 KB) | `GuiSculpting.cpp` — размер/сила | Нет настройки кисти |
| 3 | `GuiScene.js` (39 KB) | `GuiScene.cpp` — список мешей | Нет UI сцены |
| 4 | `GuiTopology.js` (21 KB) | `GuiTopology.cpp` — dyntopo/remesh | Нет топологии |
| 5 | `GuiFiles.js` (9 KB) | `GuiFiles.cpp` — import/export | Нет файлов |
| 6 | `GuiCamera.js` (22 KB) | `GuiCamera.cpp` — проекция | Нет камера-UI |
| 7 | Остальные 4 панели | — | Минорные |

### Пример — `Toolbar.cpp`:
```cpp
void Toolbar::render(SculptManager& sculpt) {
    ImGui::SetNextWindowPos({0, 0});
    ImGui::Begin("##toolbar", nullptr, ImGuiWindowFlags_NoDecoration);
    
    const char* tools[] = { "Flatten","Smooth","Inflate","Pinch","Crease","Move","Drag" };
    for (int i = 0; i < 7; i++) {
        if (ImGui::Button(tools[i]))
            sculpt.setTool((BrushType)i);
        ImGui::SameLine();
    }
    ImGui::End();
}
```

### Проверка работоспособности после каждой подфазы
```
После Toolbar.cpp:  можно переключать кисти через ImGui toolbar
После Sculpting.cpp: можно менять размер/силу кисти
После Scene.cpp:    можно добавлять/удалять меши
... и так далее
```

---

## Шаг 6 — Удаление JS (2–3 дня)
**✅ Рабочее**: C++ app полностью работает. JS-файлы удаляются.

### Порядок удаления (от безопасного к рискованному)

```
1. Удалить src/gui/*.js           — ImGui уже заменил (Шаг 5)
2. Удалить src/editing/WasmBridge.js  — MeshCpp уже заменил (Шаг 1)
3. Удалить src/mesh/Mesh.js       — MeshCpp заменил (Шаг 1)
4. Удалить src/render/*.js        — AngleRenderer заменил (Шаг 3)
5. Удалить src/math3d/            — glm заменил
6. Удалить src/states/            — StateSnapshot заменил (Шаг 2)
7. Удалить src/SculptSP.js, src/Scene.js — WasmMain заменил (Шаг 4)
8. Удалить vite.config.js, webpack.config.js — нет бандлера
9. Удалить node_modules           — нет npm
```

### Финальная структура
```
sculptsp/
  src-wasm/          ← весь C++ код
  index.html         ← <canvas> + <script type=module src=index.js>
  index.js           ← 50 строк JS (resize + file open)
  wasm/sculptsp_app.js  ← Emscripten glue (авто-генерируется)
  wasm/sculptsp_app.wasm
```

---

## Состояние работоспособности на каждом шаге

| Шаг | WASM (браузер) | Native (.exe) | Скульптинг | GUI | Файлы |
|---|---|---|---|---|---|
| 0 (baseline) | ✅ JS | ✅ прототип | ✅ JS | ✅ JS | ✅ JS |
| 1 (Mesh.cpp) | ✅ JS+C++ mesh | ✅ C++ | ✅ C++ | ✅ JS | ✅ JS |
| 2 (Scene) | ✅ JS delegates C++ | ✅ C++ | ✅ C++ | ✅ JS | ✅ JS |
| 3 (Renderer) | ✅ C++ render | ✅ C++ | ✅ C++ | ✅ JS | ✅ JS |
| 4 (WasmMain) | ✅ C++ app | ✅ C++ | ✅ C++ | ⚠️ нет панелей | ⚠️ только базово |
| 5.1 (Toolbar) | ✅ C++ | ✅ C++ | ✅ C++ | ✅ toolbar | ⚠️ |
| 5.N (все GUI) | ✅ C++ | ✅ C++ | ✅ C++ | ✅ C++ | ✅ C++ |
| 6 (cleanup) | ✅ C++ | ✅ C++ | ✅ C++ | ✅ C++ | ✅ C++ |

> ⚠️ Шаг 4 — единственное место где GUI временно отсутствует. 
> Решение: держать `?cpp` URL-параметр и переключаться на старый JS-app для тестирования.

---

## Зависимости (добавить в CMake)

```cmake
FetchContent_Declare(imgui        GIT_REPOSITORY https://github.com/ocornut/imgui GIT_TAG v1.91.0)
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json GIT_TAG v3.11.3)
FetchContent_Declare(tinyobjloader GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader GIT_TAG v2.0.0rc13)
# glm — уже есть
# SDL2 — уже есть
```

**Emscripten**: `3.1.50+` — SDL2 + WebGL2 + SIMD  
**C++ standard**: C++20  
**SIMD**: `-msimd128` (WASM) / AVX2 (native) для скульптинг-циклов
