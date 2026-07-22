#pragma once
#include <vector>
#include <cstdint>
#include "mesh/Mesh.h"

namespace ImportPLY {

std::vector<Mesh*> importPLY(const std::vector<uint8_t>& buffer);

} // namespace ImportPLY
