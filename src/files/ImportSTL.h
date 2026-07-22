#pragma once
#include <vector>
#include <cstdint>
#include "mesh/Mesh.h"

namespace ImportSTL {

std::vector<Mesh*> importSTL(const std::vector<uint8_t>& buffer);

} // namespace ImportSTL
