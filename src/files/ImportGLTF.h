#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include "mesh/Mesh.h"

namespace ImportGLTF {

std::vector<Mesh*> importGLTF(const std::string& data);
std::vector<Mesh*> importGLB(const std::vector<uint8_t>& buffer);

} // namespace ImportGLTF
