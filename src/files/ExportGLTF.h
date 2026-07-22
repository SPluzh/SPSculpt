#pragma once
#include <vector>
#include <cstdint>
#include "mesh/Mesh.h"

namespace ExportGLTF {

std::vector<uint8_t> exportGLB(const std::vector<Mesh*>& meshes);

} // namespace ExportGLTF
