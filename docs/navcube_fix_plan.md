# Fix: Navigation Cube — повторный клик переворачивает камеру

## Диагноз

### Корневая причина

Для нормали грани `(x, y, z)` грань **видна** из позиции камеры `rotX`, когда:

```
z' = y*sin(rotX) + z*cos(rotX) > 0
```

Пример — **Top** face, нормаль `(0, 1, 0)`:

| rotX | z' | Видна? |
|------|----|--------|
| `-1.53938` (в коде) | `sin(-90°) = -1` | ❌ нет |
| `+1.53938` (правильно) | `sin(+90°) = +1` | ✅ да |

Пример — **Top-Front edge**, нормаль `(0, 0.707, 0.707)`:

| rotX | z' | Видна? |
|------|----|--------|
| `-0.785398` (в коде) | `0` — граница | ❌ нет |
| `+0.785398` (правильно) | `1.0` | ✅ да |

**Вывод:** У всех частей с `rotX ≠ 0` знак инвертирован. `toggleViewAngles(part.rotX, ...)` отправляет камеру в противоположную сторону, после чего симметричная грань оказывается прямо перед пользователем — и повторный клик переворачивает камеру снова.

Почему `rotX = 0` работает: знак нуля не важен.

---

## Исправление

### Файл: `src/gui/GuiManager.cpp`

**Строка 1568** — инвертировать `rotX` при записи pending click:

```diff
- m_gizmoClickPartRotX = part.rotX;
+ m_gizmoClickPartRotX = -part.rotX;
  m_gizmoClickPartRotY = part.rotY;
```

Больше ничего менять не нужно. `toggleViewAngles` и сравнение `diffX` в `alreadyMatch` уже используют `m_gizmoClickPartRotX` — теперь с правильным знаком.

---

## Трассировка после фикса

### Top face (`part.rotX = -1.53938`)

1. Клик Top → `m_gizmoClickPartRotX = +1.53938`
2. `toggleViewAngles(+1.53938, 0)` → камера в `rotX = +1.53938`
3. Nav cube: Top face `viewNormal.z = +1` → **видна** ✅
4. Повторный клик Top → `targetRotX = +1.53938`, `clickedRotX = +1.53938`
5. `diffX = 0` → `alreadyMatch: yes` → камера не двигается ✅

### Edge (`part.rotX = +0.785398`)

1. Клик ребра → `m_gizmoClickPartRotX = -0.785398`
2. `toggleViewAngles(-0.785398, 0)` → камера в `rotX = -0.785398`
3. Nav cube: это ребро `viewNormal.z = +1` → **видна** ✅
4. Повторный клик → `diffX = 0` → `alreadyMatch: yes` ✅

### Front/Back/Left/Right (`rotX = 0`)

Без изменений: `-0 = 0`, поведение идентично.

---

## Scope изменений

| Файл | Строка | Изменение |
|------|--------|-----------|
| `src/gui/GuiManager.cpp` | 1568 | `part.rotX` → `-part.rotX` |
