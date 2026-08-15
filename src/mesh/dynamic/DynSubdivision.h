#pragma once

#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

class Mesh;

namespace DynSubdivision {

/**
 * @brief Performs local dynamic topology subdivision on a mesh region.
 *
 * Finds edges longer than detail2 (within the sphere defined by center and radius2)
 * and splits them into smaller triangles. Keeps topology manifold and updates
 * Octree leaves and dynamic ring arrays.
 *
 * @param mesh Reference to the dynamic Mesh.
 * @param iTris Vector of triangle indices inside the brush influence area.
 * @param center Center of the brush sphere in mesh local space.
 * @param radius2 Squared radius of the brush sphere in mesh local space.
 * @param detail2 Squared maximum allowed edge length.
 * @param linear Whether to linearly interpolate new vertex positions.
 * @return std::vector<uint32_t> Updated list of active triangle indices in the region.
 */
std::vector<uint32_t> subdivision(
    Mesh& mesh,
    const std::vector<uint32_t>& iTris,
    const glm::vec3& center,
    float radius2,
    float detail2,
    bool linear = true
);

} // namespace DynSubdivision
