#pragma once
#include <vector>
#include <cstdint>
#include "mesh/Mesh.h"

struct MergedMesh {
    std::vector<float> verts; // world space
    std::vector<float> colors;
    std::vector<float> materials;
    std::vector<uint32_t> faces;
    int nbVerts = 0;
    int nbFaces = 0;
};

namespace MeshUtils {

std::vector<uint32_t> triangulate(const Mesh& mesh);

MergedMesh mergeMeshes(const std::vector<Mesh*>& meshes);

void computeTangents(Mesh& mesh);

} // namespace MeshUtils
