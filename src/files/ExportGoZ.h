#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include "mesh/Mesh.h"

namespace ExportGoZ {

// Serializes a single Mesh into a binary .GoZ file in memory
std::vector<uint8_t> exportGoZ(const Mesh* mesh);

// Serializes a single Mesh and writes it to disk at the specified path
bool exportGoZ(const std::string& path, const Mesh* mesh);

// Serializes meshes and writes to disk at the specified path (exports active or first mesh)
bool exportGoZ(const std::string& path, const std::vector<Mesh*>& meshes);

} // namespace ExportGoZ

