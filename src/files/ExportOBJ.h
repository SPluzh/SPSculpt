#pragma once
#include <vector>
#include <string>
#include "mesh/Mesh.h"

namespace ExportOBJ {

std::string exportOBJ(const std::vector<Mesh*>& meshes, bool colorZbrush = true, bool colorAppend = false);

} // namespace ExportOBJ
