#include "files/MeshUtils.h"
#include "common/Constants.h"

namespace MeshUtils {

std::vector<uint32_t> triangulate(const Mesh& mesh) {
    std::vector<uint32_t> triIndices;
    triIndices.reserve(mesh.nbFaces * 6);
    for (int i = 0; i < mesh.nbFaces; ++i) {
        uint32_t v0 = mesh.faces[i * 4];
        uint32_t v1 = mesh.faces[i * 4 + 1];
        uint32_t v2 = mesh.faces[i * 4 + 2];
        uint32_t v3 = mesh.faces[i * 4 + 3];
        
        triIndices.push_back(v0);
        triIndices.push_back(v1);
        triIndices.push_back(v2);
        
        if (v3 != TRI_INDEX) {
            triIndices.push_back(v0);
            triIndices.push_back(v2);
            triIndices.push_back(v3);
        }
    }
    return triIndices;
}

MergedMesh mergeMeshes(const std::vector<Mesh*>& meshes) {
    MergedMesh mm;
    int vertOffset = 0;
    
    for (const auto* mesh : meshes) {
        if (!mesh) continue;
        int mv = mesh->nbVerts;
        int mf = mesh->nbFaces;
        
        // Transform vertices to world space
        glm::mat4 m = mesh->matrix;
        for (int i = 0; i < mv; ++i) {
            glm::vec4 p(mesh->verts[i * 3], mesh->verts[i * 3 + 1], mesh->verts[i * 3 + 2], 1.0f);
            glm::vec4 wp = m * p;
            mm.verts.push_back(wp.x);
            mm.verts.push_back(wp.y);
            mm.verts.push_back(wp.z);
        }
        
        // Colors & materials
        if (!mesh->colors.empty()) {
            mm.colors.insert(mm.colors.end(), mesh->colors.begin(), mesh->colors.end());
        } else {
            // Default color (white or gray) if empty
            for (int i = 0; i < mv; ++i) {
                mm.colors.push_back(1.0f);
                mm.colors.push_back(1.0f);
                mm.colors.push_back(1.0f);
            }
        }
        
        if (!mesh->materials.empty()) {
            mm.materials.insert(mm.materials.end(), mesh->materials.begin(), mesh->materials.end());
        } else {
            for (int i = 0; i < mv; ++i) {
                mm.materials.push_back(0.5f);
                mm.materials.push_back(0.0f);
                mm.materials.push_back(1.0f);
            }
        }
        
        // Faces (shifting index by vertOffset)
        for (int i = 0; i < mf; ++i) {
            for (int j = 0; j < 4; ++j) {
                uint32_t idx = mesh->faces[i * 4 + j];
                if (idx != TRI_INDEX) {
                    mm.faces.push_back(idx + vertOffset);
                } else {
                    mm.faces.push_back(TRI_INDEX);
                }
            }
        }
        
        mm.nbVerts += mv;
        mm.nbFaces += mf;
        vertOffset += mv;
    }
    
    return mm;
}

} // namespace MeshUtils
