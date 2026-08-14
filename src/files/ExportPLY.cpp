#include "files/ExportPLY.h"
#include "files/MeshUtils.h"
#include "common/Constants.h"
#include <sstream>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <glm/glm.hpp>

namespace ExportPLY {

static bool isLittleEndian() {
    uint32_t num = 1;
    return (*reinterpret_cast<const uint8_t*>(&num) == 1);
}

std::string exportAsciiPLY(const std::vector<Mesh*>& meshes) {
    MergedMesh res = MeshUtils::mergeMeshes(meshes);
    
    std::stringstream ss;
    ss << "ply\nformat ascii 1.0\ncomment created by SPSculpt\n";
    ss << "element vertex " << res.nbVerts << "\n";
    ss << "property float x\nproperty float y\nproperty float z\n";
    ss << "property uchar red\nproperty uchar green\nproperty uchar blue\n";
    ss << "element face " << res.nbFaces << "\n";
    ss << "property list uchar uint vertex_indices\nend_header\n";

    for (int i = 0; i < res.nbVerts; ++i) {
        int j = i * 3;
        int r = static_cast<int>(std::clamp<float>(std::round(res.colors[j] * 255.0f), 0.0f, 255.0f));
        int g = static_cast<int>(std::clamp<float>(std::round(res.colors[j + 1] * 255.0f), 0.0f, 255.0f));
        int b = static_cast<int>(std::clamp<float>(std::round(res.colors[j + 2] * 255.0f), 0.0f, 255.0f));
        
        ss << res.verts[j] << " " << res.verts[j + 1] << " " << res.verts[j + 2] << " "
           << r << " " << g << " " << b << "\n";
    }

    for (int i = 0; i < res.nbFaces; ++i) {
        int j = i * 4;
        uint32_t id3 = res.faces[j + 3];
        bool isQuad = (id3 != TRI_INDEX);
        if (isQuad) {
            ss << "4 " << res.faces[j] << " " << res.faces[j + 1] << " "
               << res.faces[j + 2] << " " << id3 << "\n";
        } else {
            ss << "3 " << res.faces[j] << " " << res.faces[j + 1] << " "
               << res.faces[j + 2] << "\n";
        }
    }

    return ss.str();
}

std::vector<uint8_t> exportBinaryPLY(const std::vector<Mesh*>& meshes) {
    MergedMesh res = MeshUtils::mergeMeshes(meshes);
    
    std::string endian = isLittleEndian() ? "little" : "big";
    std::stringstream headerSs;
    headerSs << "ply\nformat binary_" << endian << "_endian 1.0\ncomment created by SPSculpt\n";
    headerSs << "element vertex " << res.nbVerts << "\n";
    headerSs << "property float x\nproperty float y\nproperty float z\n";
    headerSs << "property uchar red\nproperty uchar green\nproperty uchar blue\n";
    headerSs << "element face " << res.nbFaces << "\n";
    headerSs << "property list uchar uint vertex_indices\nend_header\n";
    
    std::string header = headerSs.str();
    
    size_t nbQuads = 0;
    size_t nbTriangles = 0;
    for (int i = 0; i < res.nbFaces; ++i) {
        if (res.faces[i * 4 + 3] != TRI_INDEX) {
            nbQuads++;
        } else {
            nbTriangles++;
        }
    }
    
    size_t vertSize = res.verts.size() * 4 + res.colors.size();
    size_t indexSize = (nbQuads * 4 + nbTriangles * 3) * 4 + res.nbFaces;
    size_t totalSize = header.length() + vertSize + indexSize;
    
    std::vector<uint8_t> data(totalSize);
    std::memcpy(data.data(), header.data(), header.length());
    
    size_t posOc = header.length();
    for (int i = 0; i < res.nbVerts; ++i) {
        int j = i * 3;
        float vx = res.verts[j];
        float vy = res.verts[j + 1];
        float vz = res.verts[j + 2];
        
        std::memcpy(data.data() + posOc, &vx, 4);
        posOc += 4;
        std::memcpy(data.data() + posOc, &vy, 4);
        posOc += 4;
        std::memcpy(data.data() + posOc, &vz, 4);
        posOc += 4;
        
        uint8_t r = static_cast<uint8_t>(std::clamp<float>(std::round(res.colors[j] * 255.0f), 0.0f, 255.0f));
        uint8_t g = static_cast<uint8_t>(std::clamp<float>(std::round(res.colors[j + 1] * 255.0f), 0.0f, 255.0f));
        uint8_t b = static_cast<uint8_t>(std::clamp<float>(std::round(res.colors[j + 2] * 255.0f), 0.0f, 255.0f));
        
        data[posOc] = r;
        posOc += 1;
        data[posOc] = g;
        posOc += 1;
        data[posOc] = b;
        posOc += 1;
    }
    
    for (int i = 0; i < res.nbFaces; ++i) {
        int j = i * 4;
        uint32_t id3 = res.faces[j + 3];
        bool isQuad = (id3 != TRI_INDEX);
        
        data[posOc] = isQuad ? 4 : 3;
        posOc += 1;
        
        uint32_t iv0 = res.faces[j];
        uint32_t iv1 = res.faces[j + 1];
        uint32_t iv2 = res.faces[j + 2];
        
        std::memcpy(data.data() + posOc, &iv0, 4);
        posOc += 4;
        std::memcpy(data.data() + posOc, &iv1, 4);
        posOc += 4;
        std::memcpy(data.data() + posOc, &iv2, 4);
        posOc += 4;
        
        if (isQuad) {
            std::memcpy(data.data() + posOc, &id3, 4);
            posOc += 4;
        }
    }
    
    return data;
}

} // namespace ExportPLY
