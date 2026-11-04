#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <memory>
#include "mesh/Mesh.h"

namespace ImportGoZ {

struct GoZInfo {
    std::string name;
    size_t nbVerts = 0;
    size_t nbFaces = 0;
    size_t nbQuads = 0;
    size_t nbTriangles = 0;
    bool hasPaint = false;
    bool hasGroups = false;
    bool hasMask = false;
    bool hasUVs = false;
    std::string diffuseMap;
    std::string normalMap;
    std::string displacementMap;
    int subdivisionLevel = 0;
};

// Imports a binary .GoZ file from memory buffer.
// If triangulateQuads is true, quads (a, b, c, d) are split into triangles (a, b, c) and (a, c, d).
// If triangulateQuads is false (default), native quads are preserved for Quad Cap/sculpting tools.
std::vector<Mesh*> importGoZ(const std::vector<uint8_t>& buffer, bool triangulateQuads = false, GoZInfo* outInfo = nullptr);
std::vector<Mesh*> importGoZ(const uint8_t* data, size_t size, bool triangulateQuads = false, GoZInfo* outInfo = nullptr);

} // namespace ImportGoZ
