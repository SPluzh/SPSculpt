# План: Advanced Rendering для SculptSP Native

## Анализ текущей архитектуры

| Компонент | Текущее состояние |
|---|---|
| Шейдеры | `pbr.frag` — IBL через SH + panorama lookup, без источников света |
| Проходы | Bevel → Contour → Opaque → SSAO → Transparent → Merge → FXAA |
| RTT | `m_rttOpaque`, `m_rttNormals`, `m_rttSsao`, `m_rttSsaoBlur` |
| OpenGL | ES 3.0 (GLES3), `#version 300 es` в шейдерах |
| Свет | Только 1 захардкоженный `vec3(0.5, 0.8, 1.0)` в `pbr.frag:104` |

---

## Фаза 1 — Enhanced PBR (1–2 дня)

### 1.1 Система источников света

**`src/scene/LightSource.h`** — новый файл:
```cpp
enum class LightType { DIRECTIONAL, POINT, SPOT };

struct LightSource {
    LightType   type        = LightType::DIRECTIONAL;
    glm::vec3   position    = {0.f, 5.f, 5.f};
    glm::vec3   direction   = glm::normalize(glm::vec3(-0.5f, -0.8f, -1.f));
    glm::vec3   color       = {1.f, 1.f, 1.f};
    float       intensity   = 1.f;
    float       range       = 50.f;        // point/spot
    float       innerAngle  = 15.f;        // spot (degrees)
    float       outerAngle  = 30.f;        // spot
    bool        castShadow  = true;
    bool        enabled     = true;
    std::string name        = "Light";
};
```

**`src/scene/Scene.h`** — добавить:
```cpp
#include "scene/LightSource.h"
// в класс Scene:
std::vector<LightSource>& getLights()       { return m_lights; }
const std::vector<LightSource>& getLights() const { return m_lights; }
void addLight(LightType type);
void removeLight(int idx);
private:
    std::vector<LightSource> m_lights;
```

Инициализировать в `Scene::Scene()` одним дефолтным directional light.

---

### 1.2 Shadow Map Pass

**Новые RTT в `AngleRenderer.h`:**
```cpp
// Shadow map (2048×2048, depth-only texture)
RenderTarget m_rttShadow;           // 1 направленный свет
// Для point lights — можно отложить до Фазы 2
```

**`RenderTarget.h`** — добавить метод:
```cpp
bool initDepthOnly(int w, int h);   // только depth texture, без color attachment
```

**`RenderTarget.cpp`** — реализация:
```cpp
bool RenderTarget::initDepthOnly(int w, int h) {
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &depth);
    glBindTexture(GL_TEXTURE_2D, depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, w, h, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER_NV); // если доступно
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
    // GLES3: нужен dummy color attachment или расширение GL_NV_framebuffer_no_attachments
    // Альтернатива: создать 1x1 color renderbuffer и прикрепить
    depthAsTexture = true;
    ownsDepth = true;
    invW = 1.f / w; invH = 1.f / h;
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}
```

> [!NOTE]
> ANGLE (Windows/GLES3) поддерживает FBO без color attachment через `GL_EXT_framebuffer_no_attachments` или можно прикрепить dummy 1×1 `GL_R8` renderbuffer.

**Новые шейдеры:**

`shadow.vert`:
```glsl
#version 300 es
layout(location=0) in vec3 aVertex;
layout(location=3) in vec3 aMaterial;
uniform mat4 uLightMVP;
uniform mat4 uEM;
void main() {
    vec4 v = vec4(aVertex, 1.0);
    v = mix(v, uEM * v, aMaterial.z);
    gl_Position = uLightMVP * v;
}
```

`shadow.frag`:
```glsl
#version 300 es
precision highp float;
out vec4 fragColor;
void main() {
    fragColor = vec4(gl_FragCoord.z, 0.0, 0.0, 1.0);
}
```

**В `AngleRenderer.h`** добавить:
```cpp
GLuint m_shadowProgram = 0;
glm::mat4 m_shadowLightMVP{1.f};
static constexpr int SHADOW_MAP_SIZE = 2048;
```

**Shadow pass в `render()`** — вставить ПЕРЕД шагом Bevel (шаг 0b):
```cpp
// 0a. Shadow Map Pass (для первого directional light с castShadow)
const auto& lights = scene.getLights();
int shadowLightIdx = -1;
for (int i = 0; i < (int)lights.size(); ++i)
    if (lights[i].enabled && lights[i].castShadow && lights[i].type == LightType::DIRECTIONAL)
        { shadowLightIdx = i; break; }

if (shadowLightIdx >= 0) {
    // Вычислить ortho матрицу для directional light
    glm::vec3 lightDir = glm::normalize(lights[shadowLightIdx].direction);
    // Построить lightView из сцены AABB (или фиксированный fit)
    glm::mat4 lightView = glm::lookAt(-lightDir * 20.f, glm::vec3(0.f), glm::vec3(0,1,0));
    glm::mat4 lightProj = glm::ortho(-15.f, 15.f, -15.f, 15.f, 0.1f, 100.f);
    m_shadowLightMVP = lightProj * lightView;

    glBindFramebuffer(GL_FRAMEBUFFER, m_rttShadow.fbo);
    glViewport(0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0f, 4.0f);
    glUseProgram(m_shadowProgram);
    glUniformMatrix4fv(glGetUniformLocation(m_shadowProgram, "uLightMVP"), 1, GL_FALSE,
                       glm::value_ptr(m_shadowLightMVP));
    // drawPassGeometry(...) для всех мешей
    glDisable(GL_POLYGON_OFFSET_FILL);
}
```

**Обновить `pbr.frag`** — добавить PCF тени:
```glsl
uniform sampler2DShadow uShadowMap;
uniform mat4 uLightMVP;
uniform mat4 uInvView;           // view→world
uniform int  uShadowEnabled;

float PCF(vec3 worldPos) {
    vec4 lp = uLightMVP * vec4(worldPos, 1.0);
    lp.xyz = lp.xyz / lp.w * 0.5 + 0.5;
    if (lp.x < 0.0 || lp.x > 1.0 || lp.y < 0.0 || lp.y > 1.0) return 1.0;
    float shadow = 0.0;
    vec2 texelSize = vec2(1.0 / 2048.0);
    for (int x = -1; x <= 1; ++x)
    for (int y = -1; y <= 1; ++y) {
        shadow += texture(uShadowMap, vec3(lp.xy + vec2(x,y)*texelSize, lp.z - 0.001));
    }
    return shadow / 9.0;
}

// В main() использовать shadowFactor при вычислении direct lighting
```

---

### 1.3 Multi-light Direct Lighting в PBR

Обновить `pbr.frag` — заменить захардкоженный свет:
```glsl
#define MAX_LIGHTS 8

struct Light {
    vec3  position;    // view-space
    vec3  direction;   // view-space, для directional
    vec3  color;
    float intensity;
    float range;
    float innerCos;
    float outerCos;
    int   type;        // 0=directional, 1=point, 2=spot
    int   castShadow;
    int   enabled;
};
uniform Light uLights[MAX_LIGHTS];
uniform int   uNumLights;
```

GGX BRDF функции добавить в `common.glsl`:
```glsl
float D_GGX(float NoH, float a) { ... }
float V_SmithGGXCorrelated(float NoV, float NoL, float a) { ... }
vec3  F_Schlick(float VoH, vec3 f0) { ... }

vec3 BRDF(vec3 N, vec3 V, vec3 L, vec3 albedo, float roughness, vec3 specular) {
    float a = roughness * roughness;
    vec3 H = normalize(V + L);
    float NoH = max(dot(N,H), 0.0);
    float NoV = max(dot(N,V), 0.0);
    float NoL = max(dot(N,L), 0.0);
    float VoH = max(dot(V,H), 0.0);
    float D = D_GGX(NoH, a);
    float Vis = V_SmithGGXCorrelated(NoV, NoL, a);
    vec3  F = F_Schlick(VoH, specular);
    vec3 Fr = D * Vis * F;
    vec3 Fd = albedo / 3.14159;
    return (Fd * (1.0 - F) + Fr) * NoL;
}
```

---

### 1.4 SSR — Screen Space Reflections

**Новый RTT в `AngleRenderer.h`:**
```cpp
RenderTarget m_rttSsr;
GLuint m_ssrProgram = 0;
bool m_useSsr = false;
float m_ssrIntensity = 0.5f;
float m_ssrMaxDistance = 5.0f;
```

**`ssr.frag`** — raymarching по depth buffer:
```glsl
#version 300 es
precision highp float;
in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uColorTex;    // rttOpaque (до тонмаппинга)
uniform sampler2D uDepthTex;    // rttOpaque.depth
uniform sampler2D uNormalsTex;  // rttNormals
uniform mat4 uProjection;
uniform mat4 uInvProjection;
uniform float uMaxDistance;
uniform float uThickness;
uniform int uSplitMode;

// Raymarching в view space:
// 1. Восстановить view-space позицию и нормаль
// 2. Вычислить направление отражения
// 3. Шагать вдоль луча, проецируя в screen-space
// 4. Сравнивать глубину с depth buffer
// 5. При попадании — sample цвет
```

**Порядок в пайплайне** (после Opaque pass, до Merge):
```
Shadow → Bevel → Contour → Opaque → SSAO → SSR → Transparent → Merge → FXAA
```

В `merge.frag` добавить смешивание SSR результата:
```glsl
uniform sampler2D uSsrTexture;
uniform float uSsrIntensity;
uniform int uSsrEnabled;
// в main(): opaqueColor = mix(opaqueColor, ssrColor.rgb, ssrColor.a * uSsrIntensity);
```

---

### 1.5 G-Buffer для MRT (подготовка к Фазе 2)

Переписать `m_rttNormals` в полноценный G-Buffer с MRT:

**`AngleRenderer.h`:**
```cpp
// G-Buffer (MRT: 3 текстуры)
GLuint m_gBufferFbo = 0;
GLuint m_gAlbedo    = 0;   // RGB = albedo/basecolor, A = metallic
GLuint m_gNormal    = 0;   // RGB = view-space normal (октаэдральное кодирование)
GLuint m_gMaterial  = 0;   // R = roughness, G = AO-mask, B = depth-linearized
GLuint m_gDepth     = 0;   // shared depth texture (используется SSAO и SSR)
```

**`gbuffer.frag`** (MRT output):
```glsl
#version 300 es
precision highp float;
layout(location = 0) out vec4 gAlbedo;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gMaterial;

in vec3 vVertex;
in vec3 vNormal;
in vec3 vColor;
in vec3 vMaterial;

void main() {
    gAlbedo   = vec4(vColor, vMaterial.y);           // rgb=albedo, a=metallic
    gNormal   = vec4(vNormal * 0.5 + 0.5, 1.0);
    gMaterial = vec4(vMaterial.x, 0.0, 0.0, 1.0);   // r=roughness
}
```

---

## Фаза 2 — SSPT: Screen-Space Path Tracing (3–5 дней)

### 2.1 Render Mode

**`AngleRenderer.h`:**
```cpp
enum class RenderMode {
    PBR  = 0,   // текущий режим
    SSPT = 1,   // path tracing
};
RenderMode m_renderMode = RenderMode::PBR;
void setRenderMode(RenderMode m) { m_renderMode = m; resetAccumulation(); }
```

### 2.2 Temporal Accumulation Buffer (Ping-Pong)

**`AngleRenderer.h`:**
```cpp
RenderTarget m_rttAccumA;           // ping
RenderTarget m_rttAccumB;           // pong
bool m_accumPing = true;            // текущий активный
int  m_accumFrameCount = 0;
glm::mat4 m_prevViewMatrix{1.f};    // для определения движения камеры
void resetAccumulation() { m_accumFrameCount = 0; }
```

**Инициализация в `init()`:**
```cpp
m_rttAccumA.init(width, height, false);  // RGBA16F
m_rttAccumB.init(width, height, false);
```

> [!IMPORTANT]
> `RenderTarget::init()` нужно расширить параметром формата: `GL_RGBA16F` для accum buffers (float precision для накопления).

### 2.3 SSPT Shader

**`sspt.frag`** — ключевой шейдер:
```glsl
#version 300 es
precision highp float;
in vec2 vTexCoord;
out vec4 fragColor;

// G-Buffer inputs
uniform sampler2D uGAlbedo;
uniform sampler2D uGNormal;
uniform sampler2D uGMaterial;
uniform sampler2D uDepthTex;
uniform sampler2D uEnvTex;           // HDR environment (для miss rays)

// Предыдущий accumulation buffer
uniform sampler2D uPrevAccum;
uniform mat4 uPrevViewProj;          // для reprojection
uniform int  uFrameIndex;            // для шума (хеш на номер кадра)
uniform mat4 uInvViewProj;
uniform mat4 uViewMatrix;

// Blue noise / stratified random (или Wang hash)
float hash(uint n) { ... }
vec3  sampleCosineHemisphere(vec2 u, vec3 N) { ... }

// GGX importance sampling
vec3  sampleGGX(vec2 u, vec3 N, vec3 V, float roughness) { ... }
float pdfGGX(vec3 N, vec3 H, vec3 V, float roughness) { ... }

// Screen-space raymarching (возвращает цвет + hit factor)
vec4 ssRaymarch(vec3 rayOrigin, vec3 rayDir, mat4 proj) { ... }

void main() {
    // 1. Восстановить world-space позицию из depth
    // 2. Прочитать G-Buffer (normal, albedo, roughness, metallic)
    // 3. Вычислить BSDF (diffuse + specular)
    // 4. Importance sample по GGX или cosine hemisphere
    // 5. Raymarching в screen-space
    //    hit → sample color из uGAlbedo (bounce)
    //    miss → sample HDR environment
    // 6. Temporal reprojection предыдущего кадра
    // 7. Blending: result = (prevAccum * n + newSample) / (n+1)
    fragColor = vec4(accumulatedColor, 1.0);
}
```

### 2.4 SVGF Denoiser

**Два прохода:**

`svgf_temporal.frag` — temporal filter:
```glsl
// Входы: current color, previous accum, motion vectors (из reprojection)
// Выход: blended color с rejection по luminance variance
// Если камера двигалась → уменьшить вес истории (alpha → 0.1 вместо 0.05)
```

`svgf_spatial.frag` — spatial bilateral filter (a-trous wavelet):
```glsl
// 5 итераций с увеличивающимся stride (1, 2, 4, 8, 16 пикселей)
// Edge-stopping functions:
//   w_z = exp(-|depth_p - depth_q| / sigma_z)
//   w_n = max(0, dot(n_p, n_q))^128
//   w_l = exp(-|lum_p - lum_q| / sigma_l)
// Финальный вес = w_z * w_n * w_l
```

**RTT для SVGF:**
```cpp
RenderTarget m_rttSvgfA;   // для a-trous ping-pong
RenderTarget m_rttSvgfB;
RenderTarget m_rttVariance;
```

### 2.5 Порядок пассов в SSPT режиме

```
render() {
  if (m_renderMode == RenderMode::PBR) {
      // Текущий пайплайн (Shadow → Bevel → ... → FXAA)
  } else {
      // 1. Shadow Map Pass (тот же)
      // 2. G-Buffer Pass (gbuffer.frag, MRT → gAlbedo/gNormal/gMaterial/depth)
      // 3. SSPT Pass (sspt.frag → m_rttAccumPing)
      // 4. SVGF Temporal (svgf_temporal.frag)
      // 5. SVGF Spatial a-trous x5 (svgf_spatial.frag, ping-pong)
      // 6. Tone mapping + FXAA (merge.frag → fxaa.frag)
      // 7. Overlays (cursor, lasso, contour)
      // 8. Blit to screen
      m_accumPing = !m_accumPing;
      m_accumFrameCount++;
  }
}
```

### 2.6 Сброс аккумуляции

```cpp
// В drawMeshSolid / uploadIfDirty — после загрузки на GPU:
//   renderer.resetAccumulation();
// В Camera event handler — при любом изменении view:
//   renderer.resetAccumulation();
// В GuiManager — при смене материала/настроек освещения:
//   renderer.resetAccumulation();
```

---

## Структура новых файлов

```
dist/resources/shaders/
├── shadow.vert / shadow.frag        # Фаза 1: shadow map
├── gbuffer.vert / gbuffer.frag      # Фаза 1+2: G-Buffer MRT
├── ssr.frag                         # Фаза 1: Screen-Space Reflections
├── sspt.frag                        # Фаза 2: Path Tracing
├── svgf_temporal.frag               # Фаза 2: SVGF denoiser temporal
├── svgf_spatial.frag                # Фаза 2: SVGF a-trous wavelet
└── common.glsl                      # обновить: GGX BRDF функции

src/scene/
└── LightSource.h                    # новый: LightSource struct

src/render/
├── AngleRenderer.h                  # + shadow/ssr/sspt/accum RTTs, LightSource uniforms
└── AngleRenderer.cpp                # + все новые пассы

src/gui/
└── GuiManager.cpp                   # + UI для источников света, RenderMode переключатель
```

---

## C++ GUI для источников света (GuiManager)

```cpp
// В drawLightingPanel() или drawRenderSettings():
ImGui::SeparatorText("Light Sources");
auto& lights = scene.getLights();
for (int i = 0; i < (int)lights.size(); ++i) {
    ImGui::PushID(i);
    auto& L = lights[i];
    ImGui::Checkbox("##en", &L.enabled);
    ImGui::SameLine();
    ImGui::Text("%s", L.name.c_str());
    if (ImGui::BeginMenu("...")) {
        const char* types[] = { "Directional", "Point", "Spot" };
        int t = (int)L.type;
        if (ImGui::Combo("Type", &t, types, 3)) L.type = (LightType)t;
        ImGui::ColorEdit3("Color", glm::value_ptr(L.color));
        ImGui::SliderFloat("Intensity", &L.intensity, 0.f, 10.f);
        if (L.type != LightType::DIRECTIONAL)
            ImGui::SliderFloat("Range", &L.range, 0.1f, 500.f);
        if (L.type == LightType::SPOT) {
            ImGui::SliderFloat("Inner Angle", &L.innerAngle, 1.f, 89.f);
            ImGui::SliderFloat("Outer Angle", &L.outerAngle, L.innerAngle, 90.f);
        }
        ImGui::Checkbox("Cast Shadow", &L.castShadow);
        ImGui::EndMenu();
    }
    ImGui::PopID();
}
if (ImGui::Button("+ Add Light")) scene.addLight(LightType::DIRECTIONAL);

// Render Mode switcher:
ImGui::SeparatorText("Render Mode");
const char* modes[] = { "PBR", "Path Trace (SSPT)" };
int mode = (int)m_renderer->getRenderMode();
if (ImGui::Combo("##mode", &mode, modes, 2))
    m_renderer->setRenderMode((RenderMode)mode);
```

---

## Передача источников света в шейдер (pbr.frag)

В `drawMeshSolid()` добавить uniform upload:
```cpp
const auto& lights = scene.getLights();
int numLights = std::min((int)lights.size(), 8);
glUniform1i(glGetUniformLocation(m_pbrProgram, "uNumLights"), numLights);

for (int i = 0; i < numLights; ++i) {
    const auto& L = lights[i];
    std::string base = "uLights[" + std::to_string(i) + "].";

    // Преобразовать позицию/направление в view space
    glm::vec3 viewPos = glm::vec3(viewMatrix * glm::vec4(L.position, 1.f));
    glm::vec3 viewDir = glm::normalize(glm::vec3(viewMatrix * glm::vec4(L.direction, 0.f)));

    glUniform3fv(glGetUniformLocation(prog, (base + "position").c_str()), 1, glm::value_ptr(viewPos));
    glUniform3fv(glGetUniformLocation(prog, (base + "direction").c_str()), 1, glm::value_ptr(viewDir));
    glUniform3fv(glGetUniformLocation(prog, (base + "color").c_str()), 1, glm::value_ptr(L.color));
    glUniform1f(glGetUniformLocation(prog, (base + "intensity").c_str()), L.intensity);
    glUniform1f(glGetUniformLocation(prog, (base + "range").c_str()), L.range);
    glUniform1i(glGetUniformLocation(prog, (base + "type").c_str()), (int)L.type);
    glUniform1i(glGetUniformLocation(prog, (base + "enabled").c_str()), L.enabled ? 1 : 0);
}
```

---

## Приоритеты и риски

| Задача | Сложность | Приоритет | Риск |
|---|---|---|---|
| LightSource struct + Scene | Низкая | 🔴 Первое | — |
| Multi-light в pbr.frag | Средняя | 🔴 Первое | Производительность при 8 светах |
| Shadow map pass | Средняя | 🔴 Первое | GLES3: FBO без color attachment |
| PCF в pbr.frag | Низкая | 🔴 Первое | Артефакты peter-panning |
| SSR | Высокая | 🟡 Второе | Артефакты на граничных пикселях |
| G-Buffer MRT | Средняя | 🟡 Второе | Совместимость с bevel/contour |
| SSPT accumulation | Высокая | 🟠 Третье | Требует resetAccumulation везде |
| SVGF denoiser | Высокая | 🟠 Третье | Гало-артефакты на ребрах |

> [!TIP]
> Начать с `LightSource.h` → multi-light в шейдере → shadow map. SSR можно добавить как опцию (`m_useSsr`) не ломая текущий пайплайн. SSPT — отдельный `RenderMode`, не затрагивает PBR путь.
