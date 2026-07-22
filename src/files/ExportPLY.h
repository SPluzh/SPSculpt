#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include "mesh/Mesh.h"

namespace ExportPLY {

std::string exportAsciiPLY(const std::vector<Mesh*>& meshes);
std::vector<uint8_t> exportBinaryPLY(const std::vector<Mesh*>& meshes);

} // namespace ExportPLY
