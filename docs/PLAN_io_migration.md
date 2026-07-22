# ПЛАН: Портирование загрузки/сохранения объектов JS → C++ Native

## Обзор задачи

JS-версия (`src/files/`) содержит 15 файлов с полной поддержкой импорта/экспорта.  
C++ версия пока что имеет только `RenderSettings` (INI-файл настроек рендера).  
Нужно портировать **все форматы** в C++ с учётом нативного API (`Mesh`, `Scene`, `Camera`).

---

## Архитектурное решение

### Новая директория: `src/files/`

```
src/files/
  FileManager.h / FileManager.cpp   — диспетчер (выбор формата по расширению)
  ImportSGL.h / ImportSGL.cpp       — нативный формат сцены (приоритет #1)
  ExportSGL.h / ExportSGL.cpp
  ImportOBJ.h / ImportOBJ.cpp       — общий обменный формат (приоритет #2)
  ExportOBJ.h / ExportOBJ.cpp
  ImportSTL.h / ImportSTL.cpp       — печать
  ExportSTL.h / ExportSTL.cpp
  ImportPLY.h / ImportPLY.cpp       — цветовые данные
  ExportPLY.h / ExportPLY.cpp
  ImportGLTF.h / ImportGLTF.cpp     — современный формат
  ExportGLTF.h / ExportGLTF.cpp
```

### Константы (аналог `Utils.js`)

```cpp
// src/common/Constants.h
static constexpr uint32_t TRI_INDEX = 0xFFFFFFFF; // ~0u
```

---

## Этап 1 — Формат SGL (Нативный формат сцены) ⚡ Приоритет 1

### Что делает JS-версия

**ExportSGL.js** — бинарный формат v6:
- Глобальные флаги: `showGrid`, `showSymmetryLine`, `showContour` (u32 × 3)
- Камера: `projectionType`, `mode`, `fov`, `usePivot` (u32,u32,f32,u32)
- Для каждого меша: шейдер, matcap, wireframe, flatShading, alpha, visibility × 2
- Центр (f32×3) + матрица (f32×16) + scale (f32)
- Вершины (f32×3×N), vertVisible (u8×N, padded to 4B)
- Цвета (f32×3×N или 0), материалы (f32×3×N или 0)
- Грани (u32×4×F)
- UV текстурные координаты + UV-грани
- Measure tool segments (v6+)
- Divider tool segments (v6+)

### Маппинг JS → C++

| JS | C++ |
|---|---|
| `main._showGrid` | `renderer.getShowGrid()` / `renderer.setShowGrid()` |
| `ShaderBase.showSymmetryLine` | `renderer.getShowSymmetryLine()` |
| `main._showContour` | `renderer.getShowContour()` |
| `cam.getProjectionType()` | `(uint32_t)camera.getProjectionType()` |
| `cam.getMode()` | `(uint32_t)camera.getMode()` |
| `cam.getFov()` | `camera.getFov()` |
| `cam.getUsePivot()` | `(uint32_t)camera.getUsePivot()` |
| `mesh.getShaderType()` | `mesh->shaderType` |
| `mesh.getMatcap()` | `mesh->matcapIdx` |
| `mesh.getShowWireframe()` | `mesh->showWireframe` |
| `mesh.getFlatShading()` | `mesh->flatShading` |
| `mesh.getOpacity()` | `mesh->alpha` |
| `mesh.isVisible(0/1)` | `mesh->visibleV1 / visibleV2` (добавить поля) |
| `mesh.getCenter()` | нет — использовать bbox center |
| `mesh.getMatrix()` | `glm::value_ptr(mesh->matrix)` |
| `mesh.getScale()` | `1.0f` (нет scale в C++, записать 1.0f) |
| `mesh.getNbVertices()` | `mesh->nbVerts` |
| `mesh.getVertices()` | `mesh->verts.data()` |
| `mesh._meshData._vertVisible` | `mesh->vertVisible.data()` |
| `mesh.getColors()` | `mesh->colors.data()` |
| `mesh.getMaterials()` | `mesh->materials.data()` |
| `mesh.getNbFaces()` | `mesh->nbFaces` |
| `mesh.getFaces()` | `mesh->faces.data()` |

### Шаги реализации ExportSGL.cpp

```cpp
// Сигнатура
bool ExportSGL::exportSGL(const std::string& path,
                           const std::vector<Mesh*>& meshes,
                           const AngleRenderer& renderer,
                           const Camera& camera);
```

1. Вычислить размер буфера (как в JS):
   ```cpp
   size_t nbBytes = 4 * (1 + 3 + 4 + 1 + nbMeshes * perMeshWords);
   // + данные вершин, цветов, граней
   ```
2. Записать версию `VERSION = 6` (u32)
3. Записать глобальные флаги из `AngleRenderer`
4. Записать параметры камеры
5. Цикл по мешам: записать все поля
6. **Секция measure/divider**: для начала записать 0-сегменты (заглушка)
7. Записать в файл с помощью `std::ofstream` в бинарном режиме

### Шаги реализации ImportSGL.cpp

```cpp
// Сигнатура
std::vector<Mesh*> ImportSGL::importSGL(const std::string& path,
                                         AngleRenderer& renderer,
                                         Camera& camera,
                                         Scene& scene);
```

1. Считать файл в `std::vector<uint8_t>`
2. Наложить `float*` / `uint32_t*` view на буфер
3. Прочитать версию, проверить `version <= VERSION`
4. Если `version >= 2`: читать флаги, параметры камеры
5. Читать `nbMeshes`, для каждого:
   - создать `new Mesh()`
   - читать shader/matcap/wire/flat/alpha
   - читать visibility (v4+)
   - читать center (игнорировать или использовать как начальный pivot)
   - читать matrix → `mesh->matrix`
   - пропустить scale
   - читать вершины → `mesh->verts`
   - читать vertVisible (v5+) → `mesh->vertVisible`
   - читать цвета, материалы
   - читать грани → `mesh->faces`
   - читать UV (пропустить если не поддерживается)
6. Если `version >= 6`: читать measure/divider (пропустить или сохранить)
7. Для каждого меша вызвать `mesh->postInit()` (топология + октодерево)
8. Добавить в `scene`

### Поля `Mesh`, которых не хватает (добавить в Mesh.h)

```cpp
bool visibleV1 = true;   // видимость в viewport 1
bool visibleV2 = true;   // видимость в viewport 2
glm::vec3 center{0.0f};  // центр (для SGL)
float scale = 1.0f;       // масштаб (для SGL)
// UV данные (если нужны)
std::vector<float>    texCoords;    // nbTexCoords * 2
std::vector<uint32_t> facesTexCoord; // nbFaces * 4
bool hasUV = false;
```

---

## Этап 2 — Формат OBJ ⚡ Приоритет 2

### Что делает JS-версия

**ImportOBJ.js**:
- Парсит `v`, `vt`, `f` строки
- Полигоны → квадруполяция (до 4 вершин)
- `#MRGB` (ZBrush vertex color), `#MAT` (ZBrush material)
- `o` → новый объект/меш

**ExportOBJ.js**:
- `s 0\n`, `o mesh_N\n`
- `v x y z\n` (с трансформацией matrix)
- `#MRGB` блоки по 64 вершины
- `#MAT` блоки по 46 вершин
- `vt u v\n`
- `f i1 i2 i3 [i4]\n` (1-based)

### Маппинг JS → C++

| JS | C++ |
|---|---|
| `vec3.transformMat4(ver, ver, matrix)` | `glm::vec3 worldVert = glm::vec3(mesh->matrix * glm::vec4(v, 1.0f))` |
| `cAr[j] * 255` | `(int)(mesh->colors[j] * 255.0f)` |
| `parseFloat(split[1])` | `std::stof(token)` |
| `parseInt(split[0], 10)` | `std::stoi(token)` |
| `Utils.TRI_INDEX` | `TRI_INDEX = 0xFFFFFFFFu` |

### Шаги реализации ImportOBJ.cpp

```cpp
std::vector<Mesh*> ImportOBJ::importOBJ(const std::string& path);
```

1. Считать файл построчно (`std::ifstream`)
2. Вести `std::vector<float> vAr, cAr, cArMrgb, mAr, mArMat, texAr`
3. Вести `std::vector<uint32_t> fAr, uvfAr`
4. Парсить строки:
   - `v x y z [r g b]` → vAr, cAr
   - `vt u v` → texAr
   - `f ...` → фанификация полигона (алгоритм из JS: `id3 = nbVerts - id1 - 1`)
   - `#MRGB hex...` → cArMrgb (декодировать hex по 8 символов: `ff RRGGBB`)
   - `#MAT hex...` → mArMat (по 6 символов: `RRGGBB`)
   - `o name` → зафиксировать предыдущий меш, начать новый
5. `initMeshOBJ()` → создать Mesh, заполнить verts/faces/colors/materials
6. Вызвать `mesh->postInit()`

### Шаги реализации ExportOBJ.cpp

```cpp
bool ExportOBJ::exportOBJ(const std::string& path,
                            const std::vector<Mesh*>& meshes,
                            bool colorZbrush = true);
```

1. Открыть файл `std::ofstream`
2. Записать `s 0\n`
3. Для каждого меша:
   - `o mesh_N\n`
   - Цикл вершин: трансформировать через matrix, `v x y z\n`
   - `#MRGB` блоки (64 вершины каждый)
   - `#MAT` блоки (46 вершин каждый)
   - UV строки `vt u v\n`
   - Грани `f i1[/uv1] i2[/uv2] i3[/uv3] [i4[/uv4]]\n` (1-based offset)

---

## Этап 3 — Формат STL

### Что делает JS-версия

**ImportSTL.js**:
- Определяет бинарный/ASCII по размеру файла
- Бинарный: заголовок 80B, 4B nbTriangles, блоки по 50B (12B normal + 36B verts + 2B color)
- ASCII: парсит `facet normal`, `vertex` строки
- Дедупликация вершин через хэш-map `"x+y+z"` → index
- Цвет из 5-bit RGB (VisCAM/Materialise)

**ExportSTL.js** (бинарный):
- 80B заголовок (опционально с `COLOR=255,255,255,255`)
- 4B nbTriangles (little-endian)
- Для каждого треугольника: 12B normal + 36B verts + 2B color (5-bit RGB packed)
- Опция `swapXY` для сервисов 3D печати

### Маппинг JS → C++

| JS | C++ |
|---|---|
| `new Map()` для дедупликации | `std::unordered_map<std::string, uint32_t>` |
| `Remesh.mergeArrays(meshes)` | объединить все `mesh->verts`/`faces`, триангулировать (quad→tri) |
| `vec3.cross/normalize` | `glm::cross`, `glm::normalize` |

### Шаги реализации ImportSTL.cpp

```cpp
std::vector<Mesh*> ImportSTL::importSTL(const std::string& path);
```

1. Считать весь файл в `std::vector<uint8_t> buf`
2. Проверить размер: `nbTris = *(uint32_t*)&buf[80]`; `isBinary = (80 + 4 + nbTris*50 == buf.size())`
3. **Бинарный путь**:
   - Проверить хедер на `COLOR=` (Materialise magic)
   - Цикл по треугольникам: извлечь 9 float (3 вершины по 3 float)
   - Извлечь 2-байтовый color, декодировать 5-bit RGB
   - Дедупликация: `hash = to_string(x)+"+"+to_string(y)+"+"+to_string(z)`
4. **ASCII путь**:
   - Строчный парсинг `facet normal` / `vertex x y z`
5. Создать `Mesh*`, заполнить, вызвать `postInit()`

### Шаги реализации ExportSTL.cpp

```cpp
bool ExportSTL::exportBinarySTL(const std::string& path,
                                  const std::vector<Mesh*>& meshes,
                                  bool colorMagic = false,
                                  bool swapXY = false);
```

1. Триангулировать все меши (quads → 2 tris): хранить как `std::vector<uint32_t> triIndices`
2. Вычислить нормали граней через cross product
3. Написать `84 + nbTris * 50` байт:
   - 80B заголовок
   - 4B nbTris (LE)
   - Блоки: normal(12B) + v1(12B) + v2(12B) + v3(12B) + color(2B)

---

## Этап 4 — Формат PLY

### Что делает JS-версия

**ImportPLY.js**:
- Парсит ASCII заголовок: `element vertex/face`, `property type name`
- Поддерживает бинарный (LE/BE) и ASCII форматы
- `typeToOctet()` — размер типа
- Читает vertex (x,y,z,red,green,blue), faces (list uint vertex_indices)

**ExportPLY.js** (бинарный):
- Заголовок: `ply\nformat binary_little_endian 1.0\n...end_header\n`
- Вершины: 3×float + 3×uchar (RGB)
- Грани: 1×uchar count + N×uint32 indices

### Шаги реализации ImportPLY.cpp

```cpp
std::vector<Mesh*> ImportPLY::importPLY(const std::string& path);
```

1. Считать до `end_header`, запомнить байт-смещение
2. Разобрать элементы: `vertex` count, `face` count, типы свойств
3. Если binary: читать вершины побайтово с правильным cast
4. Если ascii: парсить строки `split by whitespace`
5. Читать faces: `uchar count + uint32... indices`
6. Создать Mesh, postInit()

### Шаги реализации ExportPLY.cpp

```cpp
bool ExportPLY::exportBinaryPLY(const std::string& path,
                                  const std::vector<Mesh*>& meshes,
                                  bool swapXY = false);
```

1. Объединить меши в один массив вершин/граней
2. Записать ASCII заголовок
3. Вершины: `float x, float y, float z, uint8 r, uint8 g, uint8 b`
4. Грани: `uint8 count, uint32 i0, uint32 i1, uint32 i2 [, uint32 i3]`

---

## Этап 5 — Формат GLB/GLTF

### Что делает JS-версия

**ImportGLTF.js**:
- Парсит JSON `.gltf` (с data URI буферами)
- Парсит бинарный `.glb` (magic `0x46546C67`, chunks JSON+BIN)
- Обходит `scenes → nodes → meshes → primitives`
- `getAccessorArray()` — извлекает данные с учётом byteStride, componentType
- Атрибуты: `POSITION`, `NORMAL`, `COLOR_0`, `TEXCOORD_0`, `INDICES`
- Квадруполяция: GLTF-треугольники → SculptSP quad-faces с `TRI_INDEX`

**ExportGLTF.js** (GLB):
- Создаёт JSON структуру (scenes, nodes, meshes, accessors, bufferViews)
- bufferView target: 34962 (ARRAY_BUFFER), 34963 (ELEMENT_ARRAY_BUFFER)
- componentType: 5126 (float32), 5125 (uint32), 5123 (uint16)
- GLB: magic `0x46546C67` + v2 + 12B header + JSON chunk + BIN chunk

### Маппинг JS → C++

| JS | C++ |
|---|---|
| `window.atob(base64)` | `base64_decode()` (написать helper) |
| `new TextDecoder().decode()` | `std::string(bytes.begin(), bytes.end())` |
| `JSON.parse()` | `nlohmann::json` или минимальный парсер |
| `mat4.fromRotationTranslationScale` | `glm::translate * glm::mat4_cast * glm::scale` |
| `mesh.getTriangles()` | триангулировать из `mesh->faces` |

### Зависимость: JSON парсер

Использовать [nlohmann/json](https://github.com/nlohmann/json) — single-header.  
Добавить в `CMakeLists.txt`:
```cmake
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  include(FetchContent)
  FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp DOWNLOAD_NO_EXTRACT TRUE)
  FetchContent_MakeAvailable(json)
endif()
```

### Шаги реализации ImportGLTF.cpp

```cpp
std::vector<Mesh*> ImportGLTF::importGLB(const std::string& path);
std::vector<Mesh*> ImportGLTF::importGLTF(const std::string& path);
```

1. `.glb`: проверить magic 0x46546C67, прочитать JSON-chunk и BIN-chunk
2. `.gltf`: прочитать JSON, decode data URI буферы (base64)
3. `parseGLTF(json, buffers)`:
   - обойти scene → nodes рекурсивно, накапливая world matrix
   - для каждой primitive: извлечь POSITION, NORMAL, COLOR_0, TEXCOORD_0, INDICES
4. `getAccessorArray()`: учесть byteOffset, byteStride, componentType
5. Квадруполяция: `fAr[i*4+3] = TRI_INDEX`
6. Вызвать `mesh->postInit()`

### Шаги реализации ExportGLTF.cpp

```cpp
bool ExportGLTF::exportGLB(const std::string& path,
                             const std::vector<Mesh*>& meshes);
```

1. Для каждого меша:
   - Триангулировать из quad faces → `iAr` (Uint32Array)
   - Собрать нормали: `mesh->normals`
   - Добавить bufferView для POSITION, NORMAL, COLOR_0, INDICES
2. Собрать `nlohmann::json` структуру
3. Паддинг JSON до кратного 4
4. Записать GLB: header(12B) + JSON chunk(8B+data) + BIN chunk(8B+data)

---

## Этап 6 — FileManager (диспетчер)

```cpp
// src/files/FileManager.h
class FileManager {
public:
    // Import: определяет формат по расширению, возвращает вектор новых мешей
    static std::vector<Mesh*> import(const std::string& path,
                                      AngleRenderer* renderer = nullptr,
                                      Camera* camera = nullptr,
                                      Scene* scene = nullptr);

    // Export: определяет формат по расширению
    static bool exportMeshes(const std::string& path,
                              const std::vector<Mesh*>& meshes,
                              const AngleRenderer* renderer = nullptr,
                              const Camera* camera = nullptr);

    // Save/Load SGL scene
    static bool saveScene(const std::string& path, const Scene& scene,
                           const AngleRenderer& renderer);
    static bool loadScene(const std::string& path, Scene& scene,
                           AngleRenderer& renderer);

    static std::string getExtension(const std::string& path);
};
```

**Логика диспетчера**:
```
.sgl  → ImportSGL / ExportSGL
.obj  → ImportOBJ / ExportOBJ
.stl  → ImportSTL / ExportSTL (binary)
.ply  → ImportPLY / ExportPLY (binary)
.glb  → ImportGLTF / ExportGLTF
.gltf → ImportGLTF / ExportGLTF
```

---

## Этап 7 — Интеграция в GuiManager

В `GuiManager.cpp` в панели "Files":

```cpp
// ИМПОРТ
if (ImGui::Button("Import##file")) {
    auto meshes = FileManager::import(m_importPath, &renderer, &scene.getCamera(), &scene);
    for (auto* m : meshes) scene.addMesh(m);
    scene.pushHistoryState();
}

// ЭКСПОРТ
if (ImGui::Button("Export##file")) {
    FileManager::exportMeshes(m_exportPath, scene.getMeshes(), &renderer, &scene.getCameraPtr());
}

// SAVE/LOAD SCENE (.sgl)
if (ImGui::Button("Save Scene")) {
    FileManager::saveScene(m_scenePath, scene, renderer);
}
if (ImGui::Button("Load Scene")) {
    scene.clear();
    FileManager::loadScene(m_scenePath, scene, renderer);
}
```

Добавить в `GuiManager.h`:
```cpp
char m_scenePath[256] = "scene.sgl";
```

---

## Порядок реализации (рекомендуемый)

| # | Задача | Сложность | Зависимости |
|---|--------|-----------|-------------|
| 1 | Добавить поля `Mesh.h` (visibleV1/V2, center, scale, hasUV, texCoords) | Low | — |
| 2 | `common/Constants.h` с `TRI_INDEX` | Low | — |
| 3 | `ImportOBJ` + `ExportOBJ` | Medium | Mesh.h |
| 4 | `ImportSGL` + `ExportSGL` | Medium | Mesh.h, Camera.h, AngleRenderer |
| 5 | `ImportSTL` + `ExportSTL` | Medium | MeshUtils (triangulate) |
| 6 | `ImportPLY` + `ExportPLY` | Medium | — |
| 7 | Добавить nlohmann/json в CMake | Low | — |
| 8 | `ImportGLTF` + `ExportGLTF` | High | json, base64 |
| 9 | `FileManager` диспетчер | Low | все импортёры |
| 10 | Интеграция в `GuiManager` | Low | FileManager |

---

## Вспомогательные утилиты (нужно написать)

### `MeshUtils::triangulate(const Mesh& mesh) → vector<uint32_t>`

Конвертирует quad-faces в треугольные индексы для STL/GLTF export:
```cpp
for each face[i*4 .. i*4+3]:
    tri 1: [f0, f1, f2]
    if f3 != TRI_INDEX:
        tri 2: [f0, f2, f3]
```

### `MeshUtils::mergeMeshes(const vector<Mesh*>& meshes)` 

Для STL/PLY export: объединить все меши в один flat массив с offset индексов.

### `Base64::decode(const std::string& b64) → vector<uint8_t>`

Для GLB/GLTF data URI буферов.

---

## Проверка бинарной совместимости SGL с JS-версией

После реализации проверить:
1. Сохранить `.sgl` из JS-версии
2. Загрузить в C++ — геометрия и настройки должны совпасть
3. Сохранить из C++, загрузить в JS — должно открыться корректно
4. Версия файла `VERSION = 6` должна быть одинаковой

---

## Что НЕ портируется (в первой итерации)

- `ExportMaterialise.js` — специфичный для 3D-сервиса
- `ExportSculpteo.js` — специфичный для 3D-сервиса
- `ExportSketchfab.js` — требует API ключа
- Measure tool / Divider tool в SGL v6 (записать заглушки: 0 сегментов)
