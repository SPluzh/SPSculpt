# Trim Tool — OpenVDB Pipeline: Подробный план

> **Проект:** SculptSP Native  
> **Цель:** лассо с проекцией из камеры → удалить выделенную часть → закрыть дыру регулярной квад-сеткой (VolumeToMesh с adaptivity=0).

---

## Архитектура пайплайна

```
Lasso 2D (screen)
    │
    ▼
[Phase 1] Camera Frustum Builder
    │  unproject каждой точки лассо → секущие плоскости
    ▼
[Phase 2] Mesh → OpenVDB FloatGrid (SDF)
    │  openvdb::tools::meshToLevelSet()
    ▼
[Phase 3] Frustum Mask Grid
    │  buildFrustumSDF() — булев объём frustum'a в VDB
    ▼
[Phase 4] CSG Difference
    │  openvdb::tools::csgDifference(meshSDF, maskSDF)
    ▼
[Phase 5] VolumeToMesh (adaptivity=0)
    │  регулярные квады + треугольники только на краях
    ▼
[Phase 6] Laplacian Border Smoothing
    │  сглаживание граничного края патча (2–3 итерации)
    ▼
[Phase 7] Mesh Update + Undo Snapshot
```

---

## Phase 0: Интеграция OpenVDB через MSYS2 (ucrt64)

Проект уже использует **MSYS2 ucrt64** (`build.bat` прописывает `C:\msys64\ucrt64\bin` в PATH, компилятор GCC 15.2.0). OpenVDB **12.1.1** доступен напрямую через `pacman`.

### 0.1 — Установка пакета

Запустить **один раз** в терминале (MSYS2 ucrt64 shell или обычный cmd/powershell):

```bash
C:\msys64\usr\bin\pacman.exe -S mingw-w64-ucrt-x86_64-openvdb
```

Эта команда автоматически подтянет все зависимости:

| Зависимость | Пакет pacman |
|:-----------|:------------|
| **OpenVDB 12.1.1** | `mingw-w64-ucrt-x86_64-openvdb` |
| Intel TBB | `mingw-w64-ucrt-x86_64-tbb` (auto) |
| Blosc (сжатие) | `mingw-w64-ucrt-x86_64-blosc` (auto) |
| Imath / Half | `mingw-w64-ucrt-x86_64-imath` (auto) |
| jemalloc | `mingw-w64-ucrt-x86_64-jemalloc` (auto) |
| zlib | `mingw-w64-ucrt-x86_64-zlib` (auto) |

После установки заголовки окажутся в `C:\msys64\ucrt64\include\openvdb\`,  
библиотека — `C:\msys64\ucrt64\lib\libopenvdb.dll.a`, DLL — `C:\msys64\ucrt64\bin\libopenvdb.dll`.

### 0.2 — CMakeLists.txt

```cmake
# 1. Указать CMake где искать пакеты MSYS2
set(CMAKE_PREFIX_PATH "C:/msys64/ucrt64" ${CMAKE_PREFIX_PATH})

# 2. Найти OpenVDB
find_package(OpenVDB REQUIRED COMPONENTS openvdb)

# 3. Добавить TrimToolVDB в список исходников
add_executable(sculptsp
    ...
    src/editing/TrimToolVDB.cpp   # <-- новый файл
)

# 4. Линковать
target_link_libraries(sculptsp PRIVATE
    ... # существующие
    OpenVDB::openvdb
)
```

> [!IMPORTANT]
> `CMAKE_PREFIX_PATH` нужно добавить **до** первого вызова `find_package`. В текущем `CMakeLists.txt` это строки после `project(sculptsp CXX)` и до `find_package(SDL2)`.

### 0.3 — build.bat: добавить PREFIX_PATH

Текущий `build.bat` уже добавляет `C:\msys64\ucrt64\bin` в PATH. Нужно передать `CMAKE_PREFIX_PATH` при генерации:

```bat
@echo off
set PATH=C:\msys64\ucrt64\bin;%PATH%
if not exist build mkdir build
cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH="C:/msys64/ucrt64"
cmake --build .
cd ..
if exist build\sculptsp.exe (
    if not exist dist mkdir dist
    copy /Y build\sculptsp.exe dist\
)
```

### 0.4 — Копирование DLL в dist/

Для запуска `sculptsp.exe` вне MSYS2-окружения все DLL должны лежать рядом. Добавить в `build.bat` или в `package.bat`:

```bat
set UCRT=C:\msys64\ucrt64\bin
copy /Y %UCRT%\libopenvdb.dll   dist\
copy /Y %UCRT%\libtbb12.dll     dist\
copy /Y %UCRT%\libblosc.dll     dist\
copy /Y %UCRT%\libImath-3_2.dll dist\
copy /Y %UCRT%\libjemalloc.dll  dist\
copy /Y %UCRT%\libzlib1.dll     dist\  
REM (точные имена уточнить после pacman -Ql mingw-w64-ucrt-x86_64-openvdb)
```

> [!TIP]
> Точный список DLL после установки: `C:\msys64\usr\bin\pacman.exe -Ql mingw-w64-ucrt-x86_64-openvdb | findstr ".dll"`

### 0.5 — Проверка сборки (smoke test)

Добавить временно в `NativeMain.cpp`:
```cpp
#include <openvdb/openvdb.h>
// в main():
openvdb::initialize();
openvdb::FloatGrid::Ptr g = openvdb::FloatGrid::create();
printf("OpenVDB OK, grid class: %s\n", g->gridClassToString(g->getGridClass()).c_str());
```

Если собирается и печатает `OpenVDB OK` — фаза 0 завершена.

### 0.6 — Минимальные заголовки для TrimToolVDB.cpp

```cpp
#include <openvdb/openvdb.h>
#include <openvdb/tools/MeshToVolume.h>
#include <openvdb/tools/VolumeToMesh.h>
#include <openvdb/tools/Composite.h>     // csgDifference
#include <openvdb/math/Transform.h>
```

**Инициализация (в `NativeMain.cpp`, один раз при старте):**
```cpp
openvdb::initialize();
```

---

## Phase 1: Лассо → Frustum (секущие плоскости)

Добавить `BRUSH_TRIM` в `common/Enums.h`.

Лассо уже есть (`m_lassoPoints`, `m_isLassoActive`). При mouse-up строим frustum:

```cpp
struct TrimFrustum {
    std::vector<glm::vec4> planes; // ax+by+cz+d, inside: d<0
};

// Для каждой пары точек лассо p[i], p[i+1]:
glm::vec3 nearA = unproject(p[i],   -1.0f, invVP, W, H);
glm::vec3 farA  = unproject(p[i],   +1.0f, invVP, W, H);
glm::vec3 nearB = unproject(p[i+1], -1.0f, invVP, W, H);
// Нормаль плоскости = cross(farA-nearA, nearB-nearA)
// Plane = {normal, -dot(normal, nearA)}
```

**unproject:**
```cpp
glm::vec3 unproject(glm::vec2 s, float z, const glm::mat4& invVP, int W, int H) {
    glm::vec4 ndc = { (s.x/W)*2-1, 1-(s.y/H)*2, z, 1 };
    glm::vec4 w = invVP * ndc;
    return glm::vec3(w) / w.w;
}
```

> [!IMPORTANT]
> Учитывать DPI scaling: `s *= SDL_GetWindowScale()` перед unproject.

Добавить **near cap** (плоскость по камере) и **far cap** (глубину на ~2× bbox меша) чтобы frustum не был бесконечным.

---

## Phase 2: Mesh → SDF (MeshToVolume)

Наш Mesh хранит квады (4 uint32 на грань). VDB принимает треугольники/квады через адаптер:

```cpp
struct VdbMeshAdapter {
    const Mesh* m;
    size_t pointCount()   const { return m->nbVerts; }
    size_t polygonCount() const { return m->nbFaces; }
    
    void getIndexSpacePoint(size_t n, openvdb::Vec3d& p) const {
        // localspace: verts[n*3..n*3+2]
        // worldspace через mesh->matrix
        glm::vec4 lp = { m->verts[n*3], m->verts[n*3+1], m->verts[n*3+2], 1 };
        glm::vec3 wp = glm::vec3(m->matrix * lp);
        // перевести в index space через transform->worldToIndex()
    }
    
    void getPolygon(size_t n, openvdb::Vec4I& q) const {
        q[0] = m->faces[n*4]; q[1] = m->faces[n*4+1];
        q[2] = m->faces[n*4+2]; q[3] = m->faces[n*4+3];
        // если треугольник: q[3] = openvdb::util::INVALID_IDX
    }
};

float voxelSize = meshBboxDiag / settings.voxelDivisor; // default 256
auto xform = openvdb::math::Transform::createLinearTransform(voxelSize);
VdbMeshAdapter adapter(mesh);
auto meshSDF = openvdb::tools::meshToLevelSet<openvdb::FloatGrid>(
    *xform, adapter, 3.0f, 3.0f); // band 3 voxels
```

---

## Phase 3: Frustum → Mask SDF Grid

```cpp
auto maskGrid = openvdb::FloatGrid::create(3.0f * voxelSize);
maskGrid->setTransform(meshSDF->transformPtr()->copy());

auto acc = maskGrid->getAccessor();
openvdb::CoordBBox bbox = meshSDF->evalActiveVoxelBoundingBox();

for (auto it = bbox.begin(); it != bbox.end(); ++it) {
    openvdb::Vec3d wp = meshSDF->indexToWorld(*it);
    
    // SDF frustum = max из всех полупространств (пересечение)
    float d = -1e9f;
    for (auto& pl : frustum.planes)
        d = std::max(d, pl.x*wp.x() + pl.y*wp.y() + pl.z*wp.z() + pl.w);
    
    acc.setValue(*it, d);
}
```

> [!WARNING]
> Оба грида должны иметь **идентичный transform** (одинаковый voxelSize и origin), иначе csgDifference выдаст мусор.

---

## Phase 4: CSG Вычитание

```cpp
openvdb::tools::csgDifference(*meshSDF, *maskGrid);
// meshSDF теперь = меш минус frustum
// maskGrid разрушен (in-place операция), не использовать далее
```

---

## Phase 5: VolumeToMesh → регулярная квад-сетка

```cpp
openvdb::tools::VolumeToMesh mesher(
    0.0,  // isovalue = нулевая изоповерхность SDF
    0.0   // adaptivity = 0 → чистые квады, треугольники только на угловых ячейках
);
mesher(*meshSDF);

const auto& pts   = mesher.pointList();       // вершины
const auto& pools = mesher.polygonPoolList(); // PolygonPool с квадами и треугольниками
```

> [!NOTE]
> `adaptivity=0.0` гарантирует регулярную квадратную сетку на крышке среза (шаг = voxelSize). Треугольники появляются только в угловых ячейках контура — это нормально по ТЗ.

### Конвертация обратно в Mesh:

```cpp
void applyVdbResult(Mesh* mesh, const openvdb::tools::VolumeToMesh& mesher) {
    mesh->verts.clear(); mesh->faces.clear();
    mesh->nbVerts = 0;   mesh->nbFaces = 0;
    
    for (auto& p : mesher.pointList()) {
        mesh->verts.push_back(p.x());
        mesh->verts.push_back(p.y());
        mesh->verts.push_back(p.z());
        mesh->nbVerts++;
    }
    
    for (size_t pi = 0; pi < mesher.polygonPoolListSize(); ++pi) {
        const auto& pool = mesher.polygonPoolList()[pi];
        for (size_t qi = 0; qi < pool.numQuads(); ++qi) {
            auto& q = pool.quad(qi);
            mesh->faces.insert(mesh->faces.end(), {q[0],q[1],q[2],q[3]});
            mesh->nbFaces++;
        }
        for (size_t ti = 0; ti < pool.numTriangles(); ++ti) {
            auto& t = pool.triangle(ti);
            // Квад с повтором последней вершины
            mesh->faces.insert(mesh->faces.end(), {t[0],t[1],t[2],t[2]});
            mesh->nbFaces++;
        }
    }
    
    mesh->initTopology();      // пересчитать ring/edge структуры
    mesh->isTopologyDirty = true;
    mesh->isDirty = true;
    // NormalCalc::recomputeNormals(*mesh);
}
```

---

## Phase 6: Сглаживание граничного края Cap-патча

После VolumeToMesh на краю среза возникает "лесенка" (voxel aliasing). Сглаживаем Laplacian-ом только граничные вершины:

```cpp
void smoothCapBorder(Mesh* mesh, int iterations) {
    // 1. Определить граничные вершины: vertOnEdge[i] == 1 (после initTopology)
    // 2. Собрать только те, что лежат в плоскости среза (z≈const или по normal≈camDir)
    // 3. Laplacian smoothing:
    for (int it = 0; it < iterations; ++it) {
        for (uint32_t vi : borderVerts) {
            glm::vec3 avg = {0,0,0};
            uint32_t cnt = 0;
            // обход соседей через vertRingVert
            for (uint32_t ni : neighbors(vi)) {
                avg += getVert(mesh, ni); cnt++;
            }
            if (cnt > 0) setVert(mesh, vi, avg / float(cnt));
        }
    }
}
```

Параметры: `iterations = 2`, только граничные вершины — тело меша не трогаем.

---

## Phase 7: Интеграция в SculptManager

### Новый файл: `src/editing/TrimToolVDB.h`

```cpp
#pragma once
#include "mesh/Mesh.h"
#include <glm/glm.hpp>
#include <vector>
class Camera;

struct TrimToolSettings {
    float voxelDivisor = 256.0f;
    float bandWidth    = 3.0f;
    bool  smoothBorder = true;
    int   smoothIter   = 2;
};

class TrimToolVDB {
public:
    void execute(Mesh* mesh, const std::vector<glm::vec2>& lasso,
                 const Camera& cam, int W, int H,
                 const TrimToolSettings& s = {});
};
```

### SculptManager.cpp — обработка завершения лассо:

```cpp
// В SDL_MOUSEBUTTONUP, когда BRUSH_TRIM:
if (m_currentBrush == BRUSH_TRIM && m_isLassoActive && activeMesh) {
    scene.pushUndoSnapshot(activeMesh);  // Full Mesh snapshot
    m_trimToolVDB.execute(activeMesh, m_lassoPoints,
                          scene.getCamera(), viewW, viewH, m_trimSettings);
    m_isLassoActive = false;
    m_lassoPoints.clear();
}
```

---

## Phase 8: UI (ImGui)

```cpp
// В панели инструментов при BRUSH_TRIM:
if (ImGui::CollapsingHeader("Trim (VDB)")) {
    ImGui::SliderFloat("Voxel Density", &m_trimSettings.voxelDivisor, 64, 512);
    ImGui::Checkbox("Smooth Border", &m_trimSettings.smoothBorder);
    if (m_trimSettings.smoothBorder)
        ImGui::SliderInt("Smooth Iter", &m_trimSettings.smoothIter, 1, 5);
}
```

---

## Порядок реализации

| # | Задача | Файлы | Сложность |
|:-:|:-------|:------|:---------:|
| 1 | `pacman -S openvdb` → CMakeLists.txt → build.bat → smoke test | `CMakeLists.txt`, `build.bat` | ★☆☆ |
| 2 | `VdbMeshAdapter` (Mesh → VDB input) | `TrimToolVDB.cpp` | ★★☆ |
| 3 | `meshToSDF()` — вокселизация | `TrimToolVDB.cpp` | ★★☆ |
| 4 | `buildFrustum()` — unproject лассо | `TrimToolVDB.cpp` | ★★★ |
| 5 | `buildFrustumSDF()` — маска в VDB | `TrimToolVDB.cpp` | ★★★ |
| 6 | `csgDifference` + `VolumeToMesh` | `TrimToolVDB.cpp` | ★★☆ |
| 7 | `applyVdbResult()` — VDB → Mesh | `TrimToolVDB.cpp` | ★★☆ |
| 8 | `smoothCapBorder()` — Laplacian | `TrimToolVDB.cpp` | ★★☆ |
| 9 | Интеграция SculptManager + undo | `SculptManager.cpp` | ★★☆ |
| 10 | ImGui настройки | `GuiManager.cpp` | ★☆☆ |

---

## Ключевые факты

| Вопрос | Ответ |
|:-------|:------|
| Откуда берётся квад-патч? | `VolumeToMesh(adaptivity=0)` — автоматически, step=voxelSize |
| Треугольники на краях | Нормально, VDB их вставляет в угловых ячейках |
| Гарантия watertight? | Да — SDF по определению замкнута |
| Ровный край? | `smoothCapBorder()` — Laplacian 2-3 итерации по vertOnEdge |
| Лицензия OpenVDB | MPL 2.0 — безопасна для коммерческого проекта |
| voxelSize default | `bbox_diagonal / 256` — баланс качество/скорость |
