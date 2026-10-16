#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <string>

class Mesh;

struct MeasurementAnchor {
    enum Type { VERTEX, FREE } type = FREE;
    Mesh* mesh = nullptr;
    uint32_t meshId = 0;
    uint32_t vertIdx = 0;       // Если привязано к вершине
    glm::vec3 worldPos{0.0f};   // Координаты в мировом пространстве
};

struct MeasurementSegment {
    MeasurementAnchor vertA;
    MeasurementAnchor vertB;
    bool isReference = false;   // Для Measure: является ли отрезок эталоном
    int divisions = 3;          // Индивидуальное количество делений (для Divider)
    std::string name = "";
};
