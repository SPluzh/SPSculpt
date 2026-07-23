# ZBrush Brushes — Полный аудит 28 кистей и план поддержки

## 1. Каталог всех 28 кистей

### type → инструмент sculptsp

| Файл | type | stroke | Уникальные поля |
|---|---|---|---|
| DamStandard | brush | dot | accumulate, altmode, depth_filter_*, catmull-rom falloff, alpha DS-1.jpg |
| Standard | brush | dot | baseline |
| Clay2 | brush | dot | — |
| ClayBuildup | brush | dot | clay mode, spacing=0.05 |
| SoftClay | brush | dot | soft clay |
| FillForms | brush | dot | — |
| Inflate | brush | dot | inflate mode |
| InflateLazy | brush | dot | lazy_radius высокий |
| InflateZ | brush | dot | inflate + Z-axis |
| GIO_Forms | crease | dot | crease, use_dynamic_topology |
| DamCrease | crease | dot | острый crease |
| Orb slash curve | crease | dot | большой alpha |
| Slash2 | crease | dot | — |
| Slash3 | crease | dot | — |
| FormSoft | brush | dot | мягкий brush |
| OrbClayTubes | brush | dot | clay tubes |
| **HPolish** | **flatten** | **roll** | flatten_lock_normal, area_point_radius=0.5, only_front_face |
| **HPolish2** | **flatten** | **roll** | аналог HPolish |
| **orb flatten edge** | **flatten** | dot | depth filter защита рёбер |
| **Selwy_Pinch** | **pinch** | dot | area_sharp |
| Cloth | brush | dot | area_sharp — ткань |
| Hair | brush | dot | — |
| Folly_Mech | brush | dot | — |
| Orb_Cracks | brush | dot | cracks alpha |
| **SmoothAlt** | **smooth** | dot | smooth_taubin, taubin_inflate/shrink |
| **RoundEdge** | **smooth** | **grab_dynamic_radius** | area_sharp, grab_radius |
| **MoveZ** | **move** | **grab** | grab_radius, grab_radius_scale, connected_topology |
| **SnakeHookIsh** | **move** | **grab** | snake hook style |

> [!NOTE]
> **7 уникальных type:** brush, crease, flatten, pinch, smooth, move + inflate (brush submode)

---

## 2. Новые поля (не было в предыдущем плане)

### 2.1 stroke — режим хода кисти
- `"dot"` — стандарт
- `"roll"` — HPolish: кисть "катится" независимо каждый тик
- `"grab"` — Move/SnakeHookIsh
- `"grab_dynamic_radius"` — RoundEdge: grab с динамическим радиусом

### 2.2 grab_radius + grab_radius_scale
MoveZ, SnakeHookIsh, RoundEdge:
```json
"grab_radius": true,
"grab_radius_scale": 0.28
```

### 2.3 smooth_taubin — Taubin Smooth (SmoothAlt)
```json
"smooth_taubin": true,
"smooth_taubin_inflate": 0.53,
"smooth_taubin_shrink": 0.75,
"smooth_relax": false,
"smooth_stable": false,
"smooth_sticky_border": false
```

### 2.4 area_sharp + area_sharp_smooth
RoundEdge, Selwy_Pinch, Cloth — параметр остроты нормали зоны.

### 2.5 flatten_lock_normal + flatten_lock_origin
HPolish — блокировка нормали/точки проекции.

### 2.6 depth_filter vs depth_filter_enable
- Большинство: `"depth_filter_enable": true/false`
- RoundEdge: `"depth_filter": false` (другое имя!)
- Нужна нормализация при чтении.

### 2.7 altmode
DamStandard: `"altmode": true` — инвертирует при Alt.

### 2.8 connected_topology
MoveZ: `"connected_topology": true` — только связная компонента.

### 2.9 only_front_face
HPolish: `"only_front_face": true`.

### 2.10 lazy_radius + lazy_smooth
DamStandard: `lazy_radius: 3`, RoundEdge: `lazy_radius: 10`.

### 2.11 useGlobalPressure
Все кисти: `"useGlobalPressure": false`.

---

## 3. JSON-схема пресета v2

```json
{
  "$schema": "sculptsp-brush-preset/v2",
  "name": "DamStandard",
  "icon": "PenLine",
  "color": "#e05252",
  "brushFamily": "ParametricBrush",
  "params": {
    "radius": 35,
    "intensity": 1.4,
    "spacing": 0.08,
    "negative": false,
    "culling": false,
    "accumulate": false,
    "lockPosition": false,
    "altmode": false,
    "idAlpha": null,

    "stroke": "dot",
    "lazyRadius": 0,
    "lazySmooth": 0,

    "focalShift": 0.0,
    "falloffPreset": "smoothstep",
    "falloffCurvePoints": null,

    "grabRadius": false,
    "grabRadiusScale": 0.28,

    "deformMode": "normal",

    "flattenLockNormal": false,
    "flattenLockOrigin": false,

    "tangent": false,
    "smoothTaubin": false,
    "smoothTaubinInflate": 0.53,
    "smoothTaubinShrink": 0.75,
    "smoothRelax": false,
    "smoothStable": false,
    "smoothStickyBorder": false,

    "areaNormalRadius": 0.4,
    "areaPointRadius": 0.0,
    "areaSharp": 0.0,
    "areaSampling": true,

    "depthFilterEnable": false,
    "depthFilterFalloff": true,
    "depthFilterMin": 1.0,
    "depthFilterMax": 1.0,
    "depthFilterOffset": 0.0,

    "connectedTopology": false,
    "onlyFrontFace": false,
    "topoCheck": false,
    "elasticity": 1.5,

    "color": [1.0, 0.766, 0.336],
    "roughness": 0.3,
    "metallic": 0.0,
    "hardness": 0.75,
    "writeAlbedo": true,
    "writeRoughness": true,
    "writeMetalness": false,

    "useDynamicTopology": false,
    "dynTopoInfluence": false,
    "subdivFactor": 0.0,
    "decimFactor": 0.0,

    "pressureIntensity": true,
    "pressureRadius": false,
    "useGlobalPressure": false
  }
}
```

---

## 4. Маппинг файлов → brushFamily + deformMode

| Файл | brushFamily | deformMode | stroke |
|---|---|---|---|
| Standard/DamStandard | ParametricBrush | normal | dot |
| Clay2/SoftClay/ClayBuildup/FillForms/OrbClayTubes | ParametricBrush | clay | dot |
| Inflate/InflateLazy/InflateZ | ParametricBrush | inflate | dot |
| Selwy_Pinch | ParametricBrush | pinch | dot |
| GIO_Forms/DamCrease/Slash2/Slash3/OrbSlash | ParametricBrush | crease | dot |
| FormSoft/Hair/Cloth/Orb_Cracks/Folly_Mech | ParametricBrush | normal | dot |
| **HPolish/HPolish2** | **ParametricBrush** | **flatten** | **roll** |
| **orb flatten edge** | **ParametricBrush** | **flatten** | dot |
| **SmoothAlt** | **SmoothBrush** | smooth | dot |
| **RoundEdge** | **SmoothBrush** | smooth | **grab_dynamic_radius** |
| **MoveZ/SnakeHookIsh** | **ProxyBrush** | move | **grab** |

---

## 5. Архитектурные изменения

### 5.1 stroke: "roll" (HPolish)
```js
// В SculptBase.stroke():
if (brush.stroke === 'roll') {
  // Каждый тик = независимая операция, нет накопления
  // Проецируем на нормаль текущей точки контакта
}
```

### 5.2 Taubin Smooth
```js
applyTaubin(verts) {
  // 1. shrink: v += λ * laplacian(v)  (λ = shrink)
  // 2. inflate: v += μ * laplacian(v) (μ = -inflate)
  // Результат: сглаживание без усадки объёма
}
```

### 5.3 normalizeBrushJSON()
```js
function normalizeBrushJSON(raw) {
  raw.depth_filter_enable = raw.depth_filter_enable
    ?? raw.depth_filter ?? false;
  delete raw.depth_filter;
  return raw;
}
```

### 5.4 loadFromZBrushFile(json)
```js
// BrushPresetManager
loadFromZBrushFile(rawJSON) {
  const normalized = normalizeBrushJSON(rawJSON);
  const preset = mapZBrushToPreset(normalized);
  this.addPreset(preset);
}
```

---

## 6. Встроенные пресеты (28 штук)

```
Standard, DamStandard,
Clay2, ClayBuildup, SoftClay, FillForms, OrbClayTubes,
Inflate, InflateLazy, InflateZ,
Selwy_Pinch,
GIO_Forms, DamCrease, Slash2, Slash3, OrbSlashCurve,
FormSoft, Hair, Cloth, Orb_Cracks, Folly_Mech,
HPolish, HPolish2, OrbFlattenEdge,
SmoothAlt, RoundEdge,
MoveZ, SnakeHookIsh
```

---

## 7. Этапный план (обновлённый)

### Этап 0 — BrushPresetManager v2 (1 сессия)
- Schema v2 со всеми новыми полями
- `normalizeBrushJSON()` — depth_filter совместимость
- `loadFromZBrushFile(json)` — прямой импорт из ZBrushes/
- 28 встроенных пресетов in `default_presets.json`

### Этап 1 — ParametricBrush (2 сессии)
- deformMode: +**flatten** +**crease** (уже есть normal/clay/inflate/pinch)
- stroke: +**roll** для HPolish
- areaSampling параметры: areaSharp, areaPointRadius
- depthFilter полная поддержка
- altmode, onlyFrontFace, lazyRadius

### Этап 2 — SmoothBrush (1 session)
- **Taubin** алгоритм (smoothTaubin, inflate, shrink)
- stroke: **grab_dynamic_radius** для RoundEdge
- grabRadius + areaSharp

### Этап 3 — ProxyBrush (1 сессия)
- stroke: **grab** (move)
- grabRadius + grabRadiusScale
- connectedTopology

### Этап 4 — GUI панель пресетов (2 сессии)
- Горизонтальная полоса иконок (фильтр по deformMode)
- Импорт всей папки ZBrushes/ одной кнопкой
- Export .spbrush

---

## 8. Порядок реализации

```
Этап 0 (BrushPresetManager v2 + 28 пресетов)
    ↓
Этап 1 (ParametricBrush: flatten, crease, roll)
    ↓
Этап 4 (GUI: показывает все 28 пресетов)
    ↓
Этап 2 (SmoothBrush: Taubin, grab_dynamic_radius)
    ↓
Этап 3 (ProxyBrush: grab stroke, connectedTopology)
```

> [!IMPORTANT]
> **Топ-5 новых фич из анализа ZBrushes:**
> 1. `stroke: "roll"` — HPolish полировка без накопления
> 2. `smooth_taubin` — сглаживание без усадки объёма
> 3. `grab_radius` — масштабируемый grab для RoundEdge/Move
> 4. `area_sharp` — острота зоны нормалей (ткань, pinch)
> 5. `flatten_lock_normal` — фиксация нормали при полировке
