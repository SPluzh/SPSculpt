#pragma once

#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

class Mesh;

namespace DynDecimation {

/**
 * @brief Performs local dynamic topology decimation on a mesh region.
 *
 * Finds edges shorter than detail2 (within the sphere defined by center and radius2)
 * and collapses them to reduce local vertex density. Ensures manifold integrity
 * and updates Octree leaves and dynamic ring arrays.
 *
 * @param mesh Reference to the dynamic Mesh.
 * @param iTris Vector of triangle indices inside the brush influence area.
 * @param center Center of the brush sphere in mesh local space.
 * @param radius2 Squared radius of the brush sphere in mesh local space.
 * @param detail2 Squared minimum allowed edge length threshold for decimation.
 * @return std::vector<uint32_t> Updated list of active triangle indices in the region.
 */
std::vector<uint32_t> decimation(
    Mesh& mesh,
    const std::vector<uint32_t>& iTris,
    const glm::vec3& center,
    float radius2,
    float detail2
);

std::vector<uint32_t> decimation(
    Mesh& mesh,
    const std::vector<uint32_t>& iTris,
    const glm::vec3& center,
    float radius2,
    float detail2,
    std::vector<uint8_t>& isVertDead
);

/**
 * @brief Performs standalone compaction of vertices and faces, clearing tombstones.
 *
 * Remaps dynVRV, dynVRF, faces, face data, and octree leaf references in-place.
 *
 * @param mesh Reference to the dynamic Mesh.
 * @param isVertDead Vector marking dead (tombstoned) vertices.
 * @param ioModifiedTris Optional pointer to vector of modified triangle indices to remap.
 */
void compactMesh(
    Mesh& mesh,
    const std::vector<uint8_t>& isVertDead,
    std::vector<uint32_t>* ioModifiedTris = nullptr
);

} // namespace DynDecimation
