#pragma once
#include "sculpt/PrimitiveData.h"
#include <vector>
#include <cstdint>

namespace PrimitiveGenerator {

void generate(
    const PrimitiveData& data,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
);

void generateSphere(
    float radius, int rings, int sectors,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
);

void generateGeosphere(
    float radius, int subdivision,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
);

void generateCube(
    const glm::vec3& size, int subX, int subY, int subZ, float cornerRadius,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
);

void generateCylinder(
    float radiusTop, float radiusBottom, float height,
    int radSegments, int heightSegments, bool topCap, bool bottomCap,
    int capRings, float innerRadius,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
);

void generateTorus(
    float radiusOut, float radiusWidth, float arcDegrees,
    int nbRadial, int nbTubular,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
);

void generatePlane(
    float sizeX, float sizeZ, int subX, int subZ,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
);

void generateCapsule(
    float radius, float cylinderHeight,
    int radSegments, int heightSegments, int capRings,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
);

} // namespace PrimitiveGenerator
