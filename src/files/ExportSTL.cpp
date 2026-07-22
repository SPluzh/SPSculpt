#include "files/ExportSTL.h"
#include "files/MeshUtils.h"
#include "common/Constants.h"
#include <sstream>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <glm/glm.hpp>

namespace ExportSTL {

std::string exportAsciiSTL(const std::vector<Mesh*>& meshes) {
    MergedMesh mm = MeshUtils::mergeMeshes(meshes);
    const auto& vAr = mm.verts;
    const auto& cAr = mm.colors;
    
    std::vector<uint32_t> iAr;
    iAr.reserve(mm.nbFaces * 6);
    for (int i = 0; i < mm.nbFaces; ++i) {
        uint32_t v0 = mm.faces[i * 4];
        uint32_t v1 = mm.faces[i * 4 + 1];
        uint32_t v2 = mm.faces[i * 4 + 2];
        uint32_t v3 = mm.faces[i * 4 + 3];
        
        iAr.push_back(v0);
        iAr.push_back(v1);
        iAr.push_back(v2);
        
        if (v3 != TRI_INDEX) {
            iAr.push_back(v0);
            iAr.push_back(v2);
            iAr.push_back(v3);
        }
    }
    
    size_t nbTriangles = iAr.size() / 3;
    std::stringstream ss;
    ss << "solid mesh\n";
    for (size_t i = 0; i < nbTriangles; ++i) {
        size_t id = i * 3;
        size_t i1 = iAr[id] * 3;
        size_t i2 = iAr[id + 1] * 3;
        size_t i3 = iAr[id + 2] * 3;
        
        glm::vec3 p1(vAr[i1], vAr[i1 + 1], vAr[i1 + 2]);
        glm::vec3 p2(vAr[i2], vAr[i2 + 1], vAr[i2 + 2]);
        glm::vec3 p3(vAr[i3], vAr[i3 + 1], vAr[i3 + 2]);
        
        glm::vec3 normal(0.0f);
        glm::vec3 edge1 = p2 - p1;
        glm::vec3 edge2 = p3 - p1;
        float len = glm::length(glm::cross(edge1, edge2));
        if (len > 0.0f) {
            normal = glm::cross(edge1, edge2) / len;
        }
        
        ss << " facet normal " << normal.x << " " << normal.y << " " << normal.z << "\n";
        ss << "  outer loop\n";
        ss << "   vertex " << p1.x << " " << p1.y << " " << p1.z << "\n";
        ss << "   vertex " << p2.x << " " << p2.y << " " << p2.z << "\n";
        ss << "   vertex " << p3.x << " " << p3.y << " " << p3.z << "\n";
        ss << "  endloop\n";
        ss << " endfacet\n";
    }
    ss << "endsolid mesh\n";
    return ss.str();
}

std::vector<uint8_t> exportBinarySTL(const std::vector<Mesh*>& meshes, bool colorMagic) {
    MergedMesh mm = MeshUtils::mergeMeshes(meshes);
    const auto& vAr = mm.verts;
    const auto& cAr = mm.colors;
    
    std::vector<uint32_t> iAr;
    iAr.reserve(mm.nbFaces * 6);
    for (int i = 0; i < mm.nbFaces; ++i) {
        uint32_t v0 = mm.faces[i * 4];
        uint32_t v1 = mm.faces[i * 4 + 1];
        uint32_t v2 = mm.faces[i * 4 + 2];
        uint32_t v3 = mm.faces[i * 4 + 3];
        
        iAr.push_back(v0);
        iAr.push_back(v1);
        iAr.push_back(v2);
        
        if (v3 != TRI_INDEX) {
            iAr.push_back(v0);
            iAr.push_back(v2);
            iAr.push_back(v3);
        }
    }
    
    uint32_t nbTriangles = static_cast<uint32_t>(iAr.size() / 3);
    std::vector<uint8_t> data(84 + nbTriangles * 50, 0);
    
    if (colorMagic) {
        const uint8_t hdr[10] = {67, 79, 76, 79, 82, 61, 255, 255, 255, 255};
        std::memcpy(data.data(), hdr, 10);
    }
    
    std::memcpy(data.data() + 80, &nbTriangles, 4);
    
    size_t offset = 84;
    float mulc = 31.0f / 3.0f;
    uint16_t colorActivate = colorMagic ? 0 : (1 << 15);
    
    for (uint32_t i = 0; i < nbTriangles; ++i) {
        size_t id = i * 3;
        size_t i1 = iAr[id] * 3;
        size_t i2 = iAr[id + 1] * 3;
        size_t i3 = iAr[id + 2] * 3;
        
        glm::vec3 p1(vAr[i1], vAr[i1 + 1], vAr[i1 + 2]);
        glm::vec3 p2(vAr[i2], vAr[i2 + 1], vAr[i2 + 2]);
        glm::vec3 p3(vAr[i3], vAr[i3 + 1], vAr[i3 + 2]);
        
        glm::vec3 normal(0.0f);
        glm::vec3 edge1 = p2 - p1;
        glm::vec3 edge2 = p3 - p1;
        float len = glm::length(glm::cross(edge1, edge2));
        if (len > 0.0f) {
            normal = glm::cross(edge1, edge2) / len;
        }
        
        std::memcpy(data.data() + offset, &normal.x, 4);
        std::memcpy(data.data() + offset + 4, &normal.y, 4);
        std::memcpy(data.data() + offset + 8, &normal.z, 4);
        offset += 12;
        
        std::memcpy(data.data() + offset, &p1.x, 4);
        std::memcpy(data.data() + offset + 4, &p1.y, 4);
        std::memcpy(data.data() + offset + 8, &p1.z, 4);
        offset += 12;
        
        std::memcpy(data.data() + offset, &p2.x, 4);
        std::memcpy(data.data() + offset + 4, &p2.y, 4);
        std::memcpy(data.data() + offset + 8, &p2.z, 4);
        offset += 12;
        
        std::memcpy(data.data() + offset, &p3.x, 4);
        std::memcpy(data.data() + offset + 4, &p3.y, 4);
        std::memcpy(data.data() + offset + 8, &p3.z, 4);
        offset += 12;
        
        uint16_t r = 31, g = 31, b = 31;
        if (cAr.size() >= i3 + 3) {
            r = static_cast<uint16_t>(std::clamp<int>(std::round((cAr[i1] + cAr[i2] + cAr[i3]) * mulc), 0, 31));
            g = static_cast<uint16_t>(std::clamp<int>(std::round((cAr[i1 + 1] + cAr[i2 + 1] + cAr[i3 + 1]) * mulc), 0, 31));
            b = static_cast<uint16_t>(std::clamp<int>(std::round((cAr[i1 + 2] + cAr[i2 + 2] + cAr[i3 + 2]) * mulc), 0, 31));
        }
        
        g = g << 5;
        if (colorMagic) {
            b = b << 10;
        } else {
            r = r << 10;
        }
        
        uint16_t col = r + g + b + colorActivate;
        std::memcpy(data.data() + offset, &col, 2);
        offset += 2;
    }
    
    return data;
}

} // namespace ExportSTL
