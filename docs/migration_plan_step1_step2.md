# Подробный план по реализации Этапа 1 и Этапа 2 миграции SculptSP

В данном плане описаны шаги, необходимые для реализации недостающих инструментов измерения, деформации и градиентного маскирования в нативном C++ приложении `sculptsp-native`, приближая его к 100% паритета с legacy JavaScript-версией.

---

## Этап 1: Подключение Mask Gradient Blur

Цель: Реализовать полноценный интерактивный инструмент градиентного размытия маски с выводом пунктирного оверлея и ручек управления во вьюпорте.

### 1. Изменение перечислений и настроек
*   **Файл: `src/common/Enums.h`**
    *   Добавить `BRUSH_MASK_GRADIENT_BLUR` в конец перечисления `BrushType`.
*   **Файл: `src/editing/SculptManager.h`**
    *   Увеличить размер массива настроек кистей `m_brushSettings[18]` до `19`.
*   **Файл: `src/editing/SculptManager.cpp`**
    *   В функциях сохранения настроек `saveSettings` и загрузки `loadSettings` изменить верхнюю границу циклов с `18` на `19`.
    *   Инициализировать дефолтные параметры для нового инструмента в конструкторе `SculptManager` (по аналогии с остальными кистями).

### 2. Добавление переменных состояния жеста в `SculptManager`
В секцию `private` класса `SculptManager` добавить:
```cpp
// Координаты конечных точек градиентной линии (в пикселях экрана)
glm::vec2 m_gradPointA{0.0f};
glm::vec2 m_gradPointB{0.0f};
bool m_gradActive = false;
char m_gradActivePoint = '\0'; // 'A' (маскировано), 'B' (не маскировано) или '\0'
bool m_gradIsDrawing = false;

// Буферы для предварительного расчета размытия маски (Laplacian Blur)
std::vector<float> m_origMasks;
std::vector<float> m_blurredMasks;
std::vector<uint32_t> m_gradActiveVerts;
```

### 3. Обработка логики ввода в `SculptManager::handleEvent`
В функцию `handleEvent` добавить ветвление для `BRUSH_MASK_GRADIENT_BLUR`:
*   **При клике (SDL_MOUSEBUTTONDOWN)**:
    1.  Проверить расстояние от курсора мыши до текущих точек `m_gradPointA` и `m_gradPointB` (если `m_gradActive` равен `true`).
    2.  Если дистанция `< 20` пикселей, активировать перетаскивание соответствующей точки (`m_gradActivePoint = 'A'` или `'B'`), запустить запись истории отката (`pushHistoryState()`).
    3.  Если клик произошел в стороне: начать рисование новой линии. Установить `m_gradPointA = mousePos`, `m_gradPointB = mousePos`, `m_gradActivePoint = 'B'`, `m_gradIsDrawing = true`, `m_gradActive = true`.
    4.  Выполнить предварительный расчет размытия (Precompute): скопировать исходную маску из `mesh->materials` во временный буфер и запустить `blurMask` на заданное число итераций. Записать индексы вершин, у которых маска изменилась, в `m_gradActiveVerts`.
*   **При движении (SDL_MOUSEMOTION)**:
    1.  Если `m_gradActivePoint` не равен `\0`, обновить координаты выбранной точки на текущие координаты мыши.
    2.  Вызвать `applyGradientMask(...)` из движка с передачей матриц проекции камеры и координат линии для интерполяции маски по вершинам в реальном времени.
*   **При отпускании кнопки (SDL_MOUSEBUTTONUP)**:
    1.  Если рисовалась новая линия и расстояние между точками A и B `< 5` пикселей, сбросить линию в горизонтальный отрезок по умолчанию вокруг точки клика (длина 150 пикселей) и применить маску.
    2.  Сбросить флаг перетаскивания `m_gradActivePoint = '\0'`.

### 4. Визуализация направляющих через ImGui во вьюпорте
*   **Файл: `src/gui/GuiManager.cpp`**
    *   В цикле отрисовки оверлея вьюпорта (или отдельном рендер-проходе) проверить: если выбран инструмент `BRUSH_MASK_GRADIENT_BLUR` и `m_gradActive == true`:
        *   Получить `ImDrawList* drawList = ImGui::GetForegroundDrawList();`.
        *   Нарисовать пунктирную линию (`AddLine` с флагом пунктира или отрисовывая сегменты) между `m_gradPointA` and `m_gradPointB` цветом `#00E5FF`.
        *   Нарисовать круг-ручку A (Белый с голубой обводкой).
        *   Нарисовать круг-ручку B (Голубой с белой обводкой).
        *   При наведении курсора увеличивать радиус ручек на `1.5x` для обратной связи.

### 5. Интеграция настроек в боковую панель GUI
Добавить секцию параметров для `Gradient Mask` в `GuiManager.cpp`:
*   Слайдер итераций размытия (`maskSharpenBlurIterations`).
*   Чекбокс `Blur Masked Only` (размывать только предварительно замаскированные области).
*   Кнопку очистки/сброса градиента.

---

## Этап 2: Инструменты измерения (Measure & Divider) и деформации (Transform)

### А. Инструменты измерения Measure & Divider

Цель: Дать пользователю возможность расставлять 3D-точки на меше или в пространстве, измерять расстояния и делить отрезки на пропорциональные части с выводом 2D-текста.

#### 1. Расширение Enum
*   Добавить `BRUSH_MEASURE` и `BRUSH_DIVIDER` в `BrushType` (`src/common/Enums.h`).

#### 2. Структуры данных
Определить в `SculptManager.h` структуры для хранения 3D-точек (анкоров):
```cpp
struct MeasurementAnchor {
    enum Type { VERTEX, FREE } type = FREE;
    Mesh* mesh = nullptr;
    uint32_t vertIdx = 0;       // Если привязано к вершине
    glm::vec3 worldPos{0.0f};   // Координаты в мировом пространстве
};

struct MeasurementSegment {
    MeasurementAnchor vertA;
    MeasurementAnchor vertB;
    bool isReference = false;   // Для Measure: является ли отрезок эталоном
};
```

В класс `SculptManager` добавить списки отрезков и состояния редактирования:
```cpp
std::vector<MeasurementSegment> m_measureSegments;
std::vector<MeasurementSegment> m_dividerSegments;

MeasurementAnchor m_pendingAnchorA;
MeasurementAnchor m_pendingAnchorB;
bool m_hasPending = false;

MeasurementSegment* m_draggedSegment = nullptr;
std::string m_draggedVertexKey = ""; // "vertA" или "vertB"
MeasurementSegment* m_hoveredSegment = nullptr;
std::string m_hoveredVertexKey = "";

int m_dividerDivisions = 3; // От 2 до 6
bool m_measureUseDistanceThickness = true;
```

#### 3. Реализация логики (по аналогии с JS-прототипами)
*   **Привязка (Picking & Snapping)**:
    При клике во вьюпорте запускать луч Picking Ray.
    *   Если луч пересекает меш, находить ближайшую вершину на полигоне пересечения и привязывать анкор к ней (`type = VERTEX`, сохраняя указатель на меш и индекс вершины).
    *   Если луч промахивается мимо меша, проецировать точку в свободное пространство на глубине центра камеры или плоскости другого анкора (`type = FREE`).
*   **Интерактивное редактирование**:
    *   При движении мыши без зажатой кнопки сканировать экранные координаты концов отрезков. При сближении менее чем на 15 пикселей подсвечивать ручку и менять курсор на `Pointer`.
    *   При зажатии на ручке перетаскивать её с перерасчетом проекции луча.
    *   При рисовании нового отрезка динамически строить линию до курсора мыши.
    *   Если один конец отрезка перетаскивается на другой (дистанция стремится к нулю), удалять отрезок при отпускании кнопки.

#### 4. Рендеринг оверлеев в ImGui
Для рисования использовать `ImGui::GetForegroundDrawList()`. Координаты вершин проецировать через матрицу камеры `camera.project(worldPos)` в экранные пиксели.
*   **Для MeasureTool**:
    *   Рисовать линии между точками. Белый цвет — для эталонного отрезка (Reference), серый — для обычных измерений.
    *   Точки `VERTEX` рисовать в виде кружков, точки `FREE` — в виде ромбов.
    *   На не-эталонных отрезках выводить мелкие белые точки (Ticks) на расстоянии, кратном эталонной длине.
    *   В центре каждого отрезка рисовать полупрозрачную подложку со скругленными углами и текстовый ярлык. Ярлык показывает либо абсолютную длину отрезка `L`, либо относительный масштаб относительно эталона (например, `2.50x`), если эталон задан.
*   **Для DividerTool**:
    *   Рисовать отрезок.
    *   Разделять отрезок в пространстве на `N` равных частей (согласно `m_dividerDivisions`).
    *   Проецировать промежуточные точки на экран и рисовать на линии разделительные засечки (Ticks) с порядковыми номерами или долями.

---

### Б. Инструмент трансформации меша (Transform Tool & Gizmo)

Цель: Предоставить пользователю интерактивное 3D-геймдев-гизмо для перемещения, вращения и масштабирования меша.

#### 1. Интеграция ImGuizmo (Рекомендуемый подход)
Поскольку приложение использует Dear ImGui и OpenGL, наиболее эффективным решением будет интеграция популярной легковесной библиотеки **ImGuizmo** (header-only библиотека для манипуляции матрицами во вьюпорте).

#### 2. Добавление в `BrushType`
*   Добавить `BRUSH_TRANSFORM` в `src/common/Enums.h`.

#### 3. Отрисовка Гизмо во вьюпорте
В метод `GuiManager::render` (после рендеринга 3D сцены в буфер, но до вывода ImGui оверлеев) добавить код:
```cpp
#include "ImGuizmo.h"

void drawTransformGizmo(Scene& scene, const Camera& camera) {
    Mesh* selectedMesh = scene.getSelected();
    if (!selectedMesh || sculptManager.getBrush() != BRUSH_TRANSFORM) return;

    ImGuizmo::BeginFrame();
    
    // Настройка проекции гизмо под параметры нашей камеры
    ImGuizmo::SetOrthographic(camera.isOrthographic());
    ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
    ImGuizmo::SetRect(0, 0, camera.getWidth(), camera.getHeight());

    glm::mat4 view = camera.getViewMatrix();
    glm::mat4 proj = camera.getProjMatrix();
    glm::mat4 matrix = selectedMesh->matrix; // Матрица трансформации меша

    // Текущий режим (Translation, Rotation, Scale) регулируется горячими клавишами W, E, R
    static ImGuizmo::OPERATION currentOperation = ImGuizmo::TRANSLATE;
    if (ImGui::IsKeyPressed(ImGuiKey_W)) currentOperation = ImGuizmo::TRANSLATE;
    if (ImGui::IsKeyPressed(ImGuiKey_E)) currentOperation = ImGuizmo::ROTATE;
    if (ImGui::IsKeyPressed(ImGuiKey_R)) currentOperation = ImGuizmo::SCALE;

    // Отрисовка и манипуляция гизмо
    if (ImGuizmo::Manipulate(
        glm::value_ptr(view), glm::value_ptr(proj),
        currentOperation, ImGuizmo::LOCAL, glm::value_ptr(matrix)
    )) {
        // Если пользователь взаимодействовал с гизмо:
        // Обновляем матрицу трансформации меша
        selectedMesh->matrix = matrix;
        selectedMesh->isDirty = true; // Помечаем меш для обновления нормалей/буферов на GPU
    }
}
```

#### 4. Обновление нормалей
При изменении матрицы меша через гизмо необходимо следить за перерасчетом нормалей (особенно при неравномерном масштабировании). В движке C++ при отпускании гизмо (окончании манипуляции) вызвать `NormalCalc::recomputeNormals(selectedMesh)` для сохранения корректного освещения PBR/Matcap.
