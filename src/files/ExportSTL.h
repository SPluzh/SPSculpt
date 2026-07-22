#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include "mesh/Mesh.h"

namespace ExportSTL {

std::string exportAsciiSTL(const std::vector<Mesh*>& meshes);
std::vector<uint8_t> exportBinarySTL(const std::vector<Mesh*>& meshes, bool colorMagic = false);

} // namespace ExportSTL
