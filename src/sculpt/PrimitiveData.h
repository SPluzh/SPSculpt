#pragma once
#include <glm/glm.hpp>
#include <string>
#include <algorithm>
#include <cmath>

enum class PrimitiveType {
    Sphere = 0,
    Geosphere,
    Cube,
    Cylinder,
    Cone,
    Torus,
    Plane,
    Capsule
};

inline const char* getPrimitiveTypeName(PrimitiveType type) {
    switch (type) {
        case PrimitiveType::Sphere:    return "Sphere";
        case PrimitiveType::Geosphere: return "Geosphere";
        case PrimitiveType::Cube:      return "Cube";
        case PrimitiveType::Cylinder:  return "Cylinder";
        case PrimitiveType::Cone:      return "Cone";
        case PrimitiveType::Torus:     return "Torus";
        case PrimitiveType::Plane:     return "Plane";
        case PrimitiveType::Capsule:   return "Capsule";
    }
    return "Primitive";
}

struct PrimitiveData {
    PrimitiveType type = PrimitiveType::Sphere;

    // Dimensions
    float radius = 35.0f;           // Sphere, Geosphere, Capsule radius
    float radiusTop = 25.0f;        // Cylinder top, Cone apex (if truncated)
    float radiusBottom = 25.0f;     // Cylinder bottom, Cone base
    float innerRadius = 0.0f;       // Cylinder hollow inner radius
    float height = 70.0f;           // Cylinder, Cone, Capsule cylinder height
    glm::vec3 size{70.0f, 70.0f, 70.0f}; // Cube size (X, Y, Z), Plane (X, 0, Z)
    float cornerRadius = 0.0f;      // Cube fillet/chamfer
    float arcDegrees = 360.0f;      // Torus sweep angle
    float torusMajorRadius = 35.0f; // Torus main donut radius
    float torusTubeRadius = 10.0f;  // Torus cross-section tube radius
    glm::mat4 localTransform{1.0f}; // Transform of the primitive relative to the mesh's pivot matrix
    glm::vec3 centerOffset{0.0f};   // Offset of geometric center relative to pivot matrix

    // Topology & Resolution
    int segmentsX = 32;             // Cube / Plane X subdivs
    int segmentsY = 32;             // Cube Y subdivs
    int segmentsZ = 32;             // Cube / Plane Z subdivs
    int radialSegments = 64;        // Sphere sectors, Cylinder sides, Torus cross-section, Capsule sides
    int heightSegments = 32;        // Sphere rings, Cylinder height, Capsule body
    int capRings = 4;               // Cylinder/Cone caps
    int subdivisionLevel = 4;       // Geosphere subdivs (1..6)
    int cornerSegments = 4;         // Cube corner fillet segments

    // Caps & Generation Options
    bool topCap = true;
    bool bottomCap = true;
    bool showWireframe = false;
    bool autoDensity = true;        // Keep mesh quad density constant when resizing

    void recomputeAutoDensity() {
        if (!autoDensity) return;
        switch (type) {
            case PrimitiveType::Sphere: {
                radialSegments = std::clamp(static_cast<int>(std::round(64.0f * (radius / 35.0f))), 8, 256);
                heightSegments = std::clamp(static_cast<int>(std::round(32.0f * (radius / 35.0f))), 4, 128);
                break;
            }
            case PrimitiveType::Geosphere: {
                subdivisionLevel = std::clamp(static_cast<int>(std::round(4.0f * (radius / 35.0f))), 1, 6);
                break;
            }
            case PrimitiveType::Cube: {
                const float quadEdge = 60.0f / 32.0f; // 1.875
                segmentsX = std::clamp(static_cast<int>(std::round(size.x / quadEdge)), 2, 256);
                segmentsY = std::clamp(static_cast<int>(std::round(size.y / quadEdge)), 2, 256);
                segmentsZ = std::clamp(static_cast<int>(std::round(size.z / quadEdge)), 2, 256);
                break;
            }
            case PrimitiveType::Cylinder: {
                float maxR = std::max({radiusTop, radiusBottom, 0.1f});
                radialSegments = std::clamp(static_cast<int>(std::round(64.0f * (maxR / 25.0f))), 8, 256);
                const float heightStep = 70.0f / 32.0f; // 2.1875
                heightSegments = std::clamp(static_cast<int>(std::round(height / heightStep)), 1, 256);
                capRings = std::clamp(static_cast<int>(std::round(4.0f * (maxR / 25.0f))), 1, 32);
                break;
            }
            case PrimitiveType::Cone: {
                float maxR = std::max({radiusTop, radiusBottom, 0.1f});
                radialSegments = std::clamp(static_cast<int>(std::round(64.0f * (maxR / 35.0f))), 8, 256);
                const float heightStep = 70.0f / 32.0f; // 2.1875
                heightSegments = std::clamp(static_cast<int>(std::round(height / heightStep)), 1, 256);
                capRings = std::clamp(static_cast<int>(std::round(4.0f * (maxR / 35.0f))), 1, 32);
                break;
            }
            case PrimitiveType::Torus: {
                float sweepFrac = std::clamp(arcDegrees / 360.0f, 0.05f, 1.0f);
                heightSegments = std::clamp(static_cast<int>(std::round(64.0f * (torusMajorRadius / 35.0f) * sweepFrac)), 8, 256);
                radialSegments = std::clamp(static_cast<int>(std::round(32.0f * (torusTubeRadius / 10.0f))), 6, 128);
                break;
            }
            case PrimitiveType::Plane: {
                const float quadEdge = 80.0f / 32.0f; // 2.5
                segmentsX = std::clamp(static_cast<int>(std::round(size.x / quadEdge)), 2, 256);
                segmentsZ = std::clamp(static_cast<int>(std::round(size.z / quadEdge)), 2, 256);
                break;
            }
            case PrimitiveType::Capsule: {
                radialSegments = std::clamp(static_cast<int>(std::round(32.0f * (radius / 18.0f))), 8, 128);
                capRings = std::clamp(static_cast<int>(std::round(16.0f * (radius / 18.0f))), 4, 64);
                const float heightStep = 40.0f / 20.0f; // 2.0
                heightSegments = std::clamp(static_cast<int>(std::round(height / heightStep)), 1, 128);
                break;
            }
        }
    }
};

