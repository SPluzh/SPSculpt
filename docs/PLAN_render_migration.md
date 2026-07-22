# План миграции рендера: SculptSP JS → C++ Native

> **Статус на 2026-07-22**  
> Пайплайн полностью мигрирован на C++ Native с достижением паритета с JS (5-RTT многопроходный рендеринг, RGBM кодировка/декодировка, FXAA, Contour, IBL фон, сетка и Split Viewport).  
> **Исправлен критический баг:** В вертексных шейдерах постпроцессинга (`bgVert`, `selVert`, `refVert`, `mergeVert`, `fxaaVert`, `vpVert`, `contourVert`) отсутствовал спецификатор `layout(location = 0)` для атрибута `aVertex`. Из-за этого драйвер назначал случайный индекс локации, мешая передаче вершин из полноэкранного квада/буферов. После добавления `layout(location = 0)` все проходы корректно сливаются и геометрия полностью отображается на экране.

---

## Архитектура JS-пайплайна (референс)

Порядок вызовов в `Scene.js` → `_drawScene()` + `_applyRenderSingle()`:

```
_drawScene():
  [CONTOUR 1/2]  → rttContour.FBO   (flat color selected meshes, no depth)
  [OPAQUE PASS]  → rttOpaque.FBO    (background IBL + opaque meshes, RGBM encoded)
  [TRANSPARENT]  → rttTransparent.FBO (wireframes + transparent meshes, RGBA)
  [CONTOUR 2/2]  → rttTransparent.FBO (sobel outline от rttContour поверх transparent)

_applyRenderSingle():
  [MERGE]        → rttMerge.FBO     (ShaderMerge: decodeRGBM(opaque) + blend transparent)
  [FXAA]         → rttComposite.FBO (ShaderFxaa: anti-alias от rttMerge)
  [REF IMAGES]   → rttComposite.FBO (ref images поверх, с blend)
  [FINAL BLIT]   → screen (null FBO) (ShaderViewport2D: FXAA от rttComposite + 2D pan/zoom)
  [OVERLAYS]     → screen (sculpt cursor, measure, divider)
```

**Критическая деталь:** `rttOpaque` использует RGBM кодировку (через `encodeRGBM`).  
`ShaderMerge` делает `decodeRGBM(opaque)` при слиянии.

---

## Шаг 1. Класс `RenderTarget` (аналог `Rtt.js`)

**Файл:** `src/render/RenderTarget.h` + `RenderTarget.cpp`

```cpp
struct RenderTarget {
    GLuint fbo      = 0;
    GLuint texture  = 0;
    GLuint depth    = 0; // Renderbuffer (GL_DEPTH_STENCIL или 0 если shared)
    float  invW     = 0, invH = 0;
    bool   ownsDepth = true;

    bool init(int w, int h, bool hasDepth = true, GLuint sharedDepth = 0);
    void resize(int w, int h);
    void release();
};
```

**Детали `init()`:**
- `glGenFramebuffers / glGenTextures / glGenRenderbuffers`
- Текстура: `GL_RGBA`, `GL_UNSIGNED_BYTE`, фильтр `GL_LINEAR`, wrap `GL_CLAMP_TO_EDGE`
- Depth Renderbuffer: `GL_DEPTH24_STENCIL8` (если `hasDepth && !sharedDepth`)
- `glFramebufferTexture2D(GL_COLOR_ATTACHMENT0, ...)`
- `glFramebufferRenderbuffer(GL_DEPTH_STENCIL_ATTACHMENT, ...)` если depth есть

**Детали `resize()`:**
- Пересоздать текстуру (`glTexImage2D` с новым размером)
- Пересоздать renderbuffer (`glRenderbufferStorage`)
- `invW = 1.0f/w; invH = 1.0f/h;`

**Fullscreen quad VAO:**
- Singleton VAO/VBO с тремя вершинами: `{-1,-1}, {4,-1}, {-1,4}` (large triangle trick — как в JS `Rtt.js`)
- Используется для всех постпроцесс-шейдеров

---

## Шаг 2. RTT-таргеты в `AngleRenderer`

В `AngleRenderer.h` добавить 5 полей:

```cpp
RenderTarget m_rttContour;    // flat color selected meshes (no depth RBO)
RenderTarget m_rttOpaque;     // RGBM encoded scene (with depth RBO, shared с transparent)
RenderTarget m_rttTransparent;// RGBA transparent + wireframe (shared depth от rttOpaque)
RenderTarget m_rttMerge;      // merged result (no depth)
RenderTarget m_rttComposite;  // FXAA + ref images (no depth)

// Fullscreen quad
GLuint m_fsqVao = 0;
GLuint m_fsqVbo = 0;
```

**Инициализация в `AngleRenderer::init()`:**
```
m_rttOpaque.init(w, h, /*hasDepth=*/true)
m_rttTransparent.init(w, h, /*hasDepth=*/false, /*sharedDepth=*/m_rttOpaque.depth)
m_rttContour.init(w, h, /*hasDepth=*/false)
m_rttMerge.init(w, h, /*hasDepth=*/false)
m_rttComposite.init(w, h, /*hasDepth=*/false)
```

**`AngleRenderer::resize()`** — вызывать `.resize(w, h)` для всех 5 таргетов.

---

## Шаг 3. Шейдеры постпроцессинга

Все шейдеры постпроцессинга используют fullscreen large-triangle VAO.  
GLSL версия: `#version 300 es`, uniform-сэмплер через `layout(binding=N)` или `glUniform1i`.

### 3.1. ShaderMerge (слияние opaque + transparent)

JS-оригинал: `ShaderMerge.js` + `colorSpace.glsl`

**Vert:**
```glsl
in vec2 aVertex;
out vec2 vTexCoord;
void main() {
    vTexCoord = aVertex * 0.5 + 0.5;
    gl_Position = vec4(aVertex, 0.5, 1.0);
}
```

**Frag:**
```glsl
uniform sampler2D uOpaque;      // rttOpaque texture (RGBM encoded)
uniform sampler2D uTransparent; // rttTransparent texture (RGBA)
uniform int uFilmic;
// RANGE=5.0 для decodeRGBM
vec3 decodeRGBM(vec4 col) { return 5.0 * col.rgb * col.a; }
// linearToSRGB
void main() {
    vec4 transp = texture(uTransparent, vTexCoord);
    vec3 color  = decodeRGBM(texture(uOpaque, vTexCoord)) * (1.0 - transp.a) + transp.rgb;
    if (uFilmic == 1) {
        vec3 x = max(vec3(0.0), color - 0.004);
        fragColor = vec4((x*(6.2*x+0.5))/(x*(6.2*x+1.7)+0.06), 1.0);
    } else {
        fragColor = vec4(linearToSRGB(color), 1.0);
    }
}
```

C++ поле: `m_mergeProgram`, `bool m_filmic = false`

### 3.2. ShaderFxaa (anti-aliasing)

JS-оригинал: `ShaderFxaa.js` + `fxaa.glsl`

Перенести `fxaa.glsl` дословно.  
**Vert** вычисляет 5 UV-смещений (NW/NE/SW/SE/M) через `uInvSize`.  
**Frag** вызывает `fxaa(uTexture0, uvNW, uvNE, uvSW, uvSE, uvM, uInvSize)`.

Вход: текстура `rttMerge`.  
Выход: `rttComposite`.

C++ поле: `m_fxaaProgram`

### 3.3. ShaderViewport2D (финальный blit)

JS-оригинал: `ShaderViewport2D.js`  
Аналог FXAA, но вертексный шейдер учитывает 2D pan/zoom камеры:

**Vert:**
```glsl
uniform vec2 uView2DOffset;
uniform float uView2DZoom;
// ...
vec2 ndc = (aVertex - uView2DOffset) / uView2DZoom;
vUVM = ndc * 0.5 + 0.5;
// + 4 смещённых UV для FXAA
```

Вход: текстура `rttComposite`.  
Выход: default framebuffer (null FBO).

Если 2D-режим камеры не реализован — можно передавать `uView2DOffset={0,0}, uView2DZoom=1.0`.

C++ поле: `m_viewport2DProgram`

### 3.4. ShaderBackground (IBL/ENV фон в Opaque pass)

JS-оригинал: `ShaderBackground.js` + `pbr.glsl` + `mainBackground.glsl`

**Важно:** Background рендерится ВНУТРИ `rttOpaque.FBO` и выводит RGBM (через `encodeRGBM`).

Три режима фона (uniform `uBackgroundType`):
- `0` = flat gradient/image → `encodeRGBM(sRGBToLinear(texture))`
- `1` = env panorama specular → `encodeRGBM(texturePanoramaLod(dir, blur*blur))`
- `2` = env ambient (SH) → `encodeRGBM(sphericalHarmonics(dir))`

Для режима 0 (нативный дефолт): texture = 1×1 пиксель rgb(50,50,50).  
Среда (environments): изначально можно хардкодить SPH коэффициенты и текстуру из `ShaderPBR.js`.

C++ поле: `m_bgIblProgram`, `int m_backgroundType = 0`, `float m_bgBlur = 0.0f`

---

## Шаг 4. ShaderContour (обводка выделения)

JS-оригинал: `ShaderContour.js` + `outline.glsl`

**Два этапа:**

**Этап 4a** (внутри `_drawScene`, перед Opaque pass):
- Привязать `rttContour.FBO`
- `glClear(GL_COLOR_BUFFER_BIT)` (без depth)
- Для каждого выделенного меша: рендер flat-color шейдером (белый = `vec4(1.0)`)

**Этап 4b** (внутри Transparent pass, в конце):
- Всё ещё привязан `rttTransparent.FBO`
- Использовать `ShaderContour`: sobel-детектор краёв от `rttContour.texture`
- Рисовать поверх с blend: `glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`

**Frag `outline.glsl`** (перенести дословно):
```glsl
// 8-neighbourhood sobel
float outlineDistance(vec2 uv, sampler2D tex, vec2 invSize) { ... }
// если mag < 1.5 → discard
// иначе gl_FragColor = uColor (vec4, configurable outline color)
```

C++ поля: `m_contourProgram`, `glm::vec4 m_contourColor{1.0f, 0.75f, 0.1f, 1.0f}`  
Флаг: `bool m_showContour = true`

---

## Шаг 5. Новый `render()` — полный пайплайн

Рефакторинг `AngleRenderer::render()` по образцу JS:

```cpp
void AngleRenderer::render(const Scene& scene) {
    // === DRAWSCENE ===
    // [1] CONTOUR PASS 1/2
    if (m_showContour && hasSelectedMeshes(scene)) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_rttContour.fbo);
        glDisable(GL_DEPTH_TEST);
        glClear(GL_COLOR_BUFFER_BIT);
        for (auto* mesh : scene.getSelectedMeshes())
            drawMeshFlatColor(mesh, scene, glm::vec4(1.0f));
        glEnable(GL_DEPTH_TEST);
    }

    // [2] OPAQUE PASS → rttOpaque (RGBM encoded)
    glBindFramebuffer(GL_FRAMEBUFFER, m_rttOpaque.fbo);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    drawBackground(scene);          // ShaderBackground → encodeRGBM
    if (m_showGrid) drawGrid(scene);
    for (auto* mesh : opaqueMeshes)
        drawMeshSolid(mesh, scene); // PBR/Matcap/Flat → encodeRGBM output

    // [3] TRANSPARENT PASS → rttTransparent (shared depth)
    glBindFramebuffer(GL_FRAMEBUFFER, m_rttTransparent.fbo);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    // wireframes (depthFunc GL_LESS)
    glDepthFunc(GL_LESS);
    for (auto* mesh : allMeshes) if (mesh->showWireframe) drawWireframe(mesh, scene);
    glDepthFunc(GL_LEQUAL);
    // transparent back→front
    glDepthMask(GL_FALSE);
    glEnable(GL_CULL_FACE);
    for (auto* mesh : transparentMeshes) {
        glCullFace(GL_FRONT); drawMeshSolid(mesh, scene);
        glCullFace(GL_BACK);  drawMeshSolid(mesh, scene);
    }
    glDisable(GL_CULL_FACE);
    // CONTOUR PASS 2/2
    if (m_showContour && hasSelectedMeshes(scene))
        drawContourOverlay(); // sobel от rttContour → rttTransparent
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    // === APPLYRENDER ===
    glDisable(GL_DEPTH_TEST);

    // [4] MERGE → rttMerge
    glBindFramebuffer(GL_FRAMEBUFFER, m_rttMerge.fbo);
    drawFullscreenMerge(); // decodeRGBM(rttOpaque) + blend(rttTransparent)

    // [5] FXAA → rttComposite
    glBindFramebuffer(GL_FRAMEBUFFER, m_rttComposite.fbo);
    drawFullscreenFxaa();  // от rttMerge

    // [6] Ref Images поверх rttComposite
    glEnable(GL_BLEND);
    drawReferenceImages(scene); // target = rttComposite
    glDisable(GL_BLEND);

    // [7] Final blit → screen
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    drawFullscreenViewport2D(); // FXAA от rttComposite + 2D offset/zoom

    glEnable(GL_DEPTH_TEST);

    // [8] Overlays (cursor, etc.) → screen напрямую
    drawSelectionCursor();
}
```

---

## Шаг 6. RGBM в Opaque Pass

**Критически важно:** `rttOpaque` хранит HDR-цвет в RGBM кодировке (RANGE=5.0).  
Это значит, что **все шейдеры геометрии** (PBR, Matcap, Flat, Background), выводящие в `rttOpaque`, должны использовать `encodeRGBM` в `fragColor`.

```glsl
// colorSpace.glsl функции (перенести):
vec4 encodeRGBM(vec3 col) {
    vec4 rgbm;
    vec3 color = col / 5.0;
    rgbm.a = clamp(max(max(color.r,color.g),max(color.b,1e-6)),0.0,1.0);
    rgbm.a = ceil(rgbm.a * 255.0) / 255.0;
    rgbm.rgb = color / rgbm.a;
    return rgbm;
}
```

Текущие шейдеры `PBR`, `Matcap`, `Flat` в `AngleRenderer.cpp` выводят `vec4(finalColor, uAlpha)` — это **неверно** для Opaque RTT.  
**Нужно:** в Opaque pass выводить `encodeRGBM(finalColor)`, в Transparent pass — оставить `vec4(color, alpha)`.

**Решения (выбрать одно):**
- Вариант A: компилировать два варианта каждого шейдера (`#define ENCODE_RGBM`).
- Вариант B: шейдер всегда кодирует RGBM, Merge-шейдер и так декодирует → для transparent нужен отдельный шейдер.
- **Рекомендуется Вариант B** (как в JS): разные шейдеры для Opaque и Transparent, как и в оригинале.

---

## Шаг 7. Environment (IBL) — Управление пресетами

JS хранит 5 готовых пресетов в `ShaderPBR.js` с хардкоженными SPH-коэффициентами.  
**Для C++ native:**

1. Перенести все 5 пресетов как C++ `struct EnvironmentPreset { std::string name; std::string texPath; float sph[27]; float exposure; }` в `AngleRenderer.h`.
2. При инициализации загрузить текстуру `resources/environments/*.png` через `stb_image` (PNG, RGBA8).
3. SPH-коэффициенты берутся из хардкоженных массивов (как в JS).
4. Метод `setEnvironment(int idx)` — переключает текущий пресет, перебиндит текстуру.
5. Для Background IBL-режима: та же текстура env используется ShaderBackground.

**Форматы файлов:** JS загружает `.png` (не HDR). Формат внутри — LUV-encoded пирамида (см. `decodeLUV` в `pbr.glsl`). Файлы необходимо скопировать из JS-проекта в `dist/resources/environments/`.

---

## Шаг 8. Grid (сетка)

JS: `Primitives.createGrid()` + shader FLAT + рендер в начале Opaque pass.

**Задачи:**
1. Создать геометрию сетки (линии) в `AngleRenderer::initGrid()`:
   - 20×20 линий в плоскости XZ, центр в (0,0,0), шаг 0.05.
   - VBO с позициями линий, рендер `GL_LINES`.
2. Шейдер: `m_flatProgram` с `uAlbedo = vec3(0.04, 0.04, 0.04)`.
3. Трансформация: translate Y = -0.45, scale = 2.5 (как в `Scene.js::initGrid()`).
4. Рендер в начале Opaque pass с `glEnable(GL_DEPTH_TEST)`.
5. Флаг `bool m_showGrid = true`.

---

## Шаг 9. Прозрачность и сортировка мешей

JS сортирует `meshes` по `Mesh.sortFunction` (непрозрачные сначала, прозрачные потом).

**Задачи:**
1. В `AngleRenderer::render()` разделить список мешей на `opaque` и `transparent`.
   - Критерий: `mesh->alpha < 1.0f` → transparent.
2. Сортировать opaque front-to-back (оптимизация depth).
3. Transparent рендерить back-to-front (two-pass: FRONT cull, затем BACK cull).
4. Wireframe — для ВСЕХ мешей в начале transparent pass с `GL_DEPTH_FUNC = GL_LESS`.

---

## Шаг 10. Overlay-шейдеры

### 10.1. WetClay (ShaderWetClay.js)
Перенести шейдер мокрой глины (`wetClay.glsl`) — специализированный Matcap с SSS-эффектом.  
Добавить тип шейдера `shaderType == 2` → `m_wetClayProgram`.

### 10.2. VoxelChecker (ShaderVoxelChecker.js)
Рендер preview воксельного ремеша (шахматный паттерн).  
Когда `m_voxelPreviewMesh != nullptr` — рендерить в конце Opaque pass.  
Шейдер: uniforms `uMVP, uMV, uStep`; fragment: checkerboard по `vViewPos.xy`.

### 10.3. NormalShader (ShaderNormal.js)
Для отладки нормалей. Дополнительный режим отображения.

---

## Шаг 11. Screenshot Mode

JS реализует `takeScreenshot(w, h)` — временный resize всех RTT до нужного разрешения.

**Задачи:**
1. Метод `AngleRenderer::renderToBuffer(int w, int h) → std::vector<uint8_t>`:
   - Создать временный `RenderTarget tmpTarget(w, h)`
   - Рендерить туда весь пайплайн (Шаг 5)
   - `glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, data.data())`
   - Flip Y (OpenGL → image coords)
   - Вернуть пиксели для сохранения через `stb_image_write`
2. Флаг `m_isTakingScreenshot = true` — пропускает оверлеи (курсор, measure, divider).

---

## Шаг 12. `glBufferSubData` для VBO (оптимизация)

JS `Buffer.js` использует `updateSubData` — загрузка только изменённого диапазона.  
Текущий C++ `uploadIfDirty` делает `glBufferData` целиком.

**Задачи:**
1. Добавить в `Mesh` поля `int dirtyVertStart, dirtyVertEnd` (диапазон грязных вершин).
2. В `uploadIfDirty`: если `dirtyVertEnd > dirtyVertStart` → `glBufferSubData` только для диапазона.
3. Применять для `vboVertices`, `vboNormals`, `vboColors`, `vboMaterials` (позиции/нормали меняются при скульптинге).
4. `dirtyVertStart / dirtyVertEnd` обновляются в `SculptManager` после каждого brush stroke.

---

## Шаг 13. Split Viewport

JS: `_applyRenderSplit()` вызывает `_applyRenderSingle()` дважды с разными камерами.

**Задачи:**
1. Добавить поле `bool m_splitMode = false`, `Camera* m_cameraRight = nullptr`.
2. В `render()` — если `m_splitMode`:
   - Левая половина: `glViewport(0, 0, w/2, h); glScissor(0, 0, w/2, h);` + основная камера.
   - Правая половина: `glViewport(w/2, 0, w/2, h); glScissor(w/2, 0, w/2, h);` + `m_cameraRight`.
   - Оба вызова рендерят полный пайплайн (Шаг 5) с соответствующей камерой.
3. RTT-таргеты имеют полный размер `w×h`; scissor ограничивает запись в каждую половину.

---

## Приоритет реализации

| Шаг | Описание                          | Приоритет |
|-----|-----------------------------------|-----------|
| 1–2 | RenderTarget + RTT init/resize    | 🔴 Критично |
| 3   | ShaderMerge + ShaderFxaa          | 🔴 Критично |
| 5   | Новый render() пайплайн           | 🔴 Критично |
| 6   | RGBM кодировка в Opaque pass      | 🔴 Критично |
| 3.3 | ShaderViewport2D (финальный blit) | 🔴 Критично |
| 3.4 | ShaderBackground (IBL фон)        | 🟡 Важно   |
| 7   | Environment пресеты               | 🟡 Важно   |
| 4   | ShaderContour (обводка)           | 🟡 Важно   |
| 8   | Grid                              | 🟡 Важно   |
| 9   | Сортировка + прозрачность         | 🟡 Важно   |
| 12  | glBufferSubData оптимизация       | 🟢 Полезно |
| 10  | WetClay / VoxelChecker / Normal   | 🟢 Полезно |
| 11  | Screenshot mode                   | 🟢 Полезно |
| 13  | Split Viewport                    | 🟢 Полезно |

---

## Зависимости и файлы для переноса из JS

| C++ файл (создать)                   | JS источник                                 |
|--------------------------------------|---------------------------------------------|
| `src/render/RenderTarget.h/.cpp`     | `drawables/Rtt.js`                          |
| `src/render/shaders/merge.glsl`      | `ShaderMerge.js` + `colorSpace.glsl`        |
| `src/render/shaders/fxaa.glsl`       | `ShaderFxaa.js` + `glsl/fxaa.glsl`          |
| `src/render/shaders/viewport2d.glsl` | `ShaderViewport2D.js`                       |
| `src/render/shaders/contour.glsl`    | `ShaderContour.js` + `glsl/outline.glsl`    |
| `src/render/shaders/background.glsl` | `ShaderBackground.js` + `mainBackground.glsl` |
| `src/render/shaders/colorspace.glsl` | `glsl/colorSpace.glsl`                      |
| `src/render/shaders/pbr.glsl`        | `glsl/pbr.glsl` (уже частично перенесён)    |
| `dist/resources/environments/*.png`  | JS `resources/environments/` (5 файлов)     |
