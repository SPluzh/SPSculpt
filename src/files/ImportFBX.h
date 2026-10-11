#pragma once
#include <vector>
#include <string>

class Mesh;

namespace ImportFBX {

std::vector<Mesh*> importFBX(const std::string& path);

} // namespace ImportFBX
