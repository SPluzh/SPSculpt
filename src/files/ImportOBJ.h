#pragma once
#include <vector>
#include <string>
#include "mesh/Mesh.h"

namespace ImportOBJ {

std::vector<Mesh*> importOBJ(const std::string& data);

} // namespace ImportOBJ
