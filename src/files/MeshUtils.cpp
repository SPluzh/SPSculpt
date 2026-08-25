#include "files/MeshUtils.h"
#include "common/Constants.h"
#include <glm/glm.hpp>
#include <cmath>
#include <algorithm>

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

void computeTangents(Mesh& mesh) {
    if (mesh.nbVerts == 0 || mesh.nbFaces == 0) return;
    if (mesh.uvFlat.size() != (size_t)mesh.nbVerts * 2) return;
    if (mesh.normals.size() != (size_t)mesh.nbVerts * 3) return;

    mesh.tangents.assign(mesh.nbVerts * 4, 0.0f);
    std::vector<glm::vec3> tan1(mesh.nbVerts, glm::vec3(0.0f));
    std::vector<glm::vec3> tan2(mesh.nbVerts, glm::vec3(0.0f));

    const auto& texFaceArray = (!mesh.facesTexCoord.empty() && mesh.facesTexCoord.size() == (size_t)mesh.nbFaces * 4)
                               ? mesh.facesTexCoord
                               : mesh.faces;

    for (int i = 0; i < mesh.nbFaces; ++i) {
        uint32_t i1 = texFaceArray[i * 4];
        uint32_t i2 = texFaceArray[i * 4 + 1];
        uint32_t i3 = texFaceArray[i * 4 + 2];
        uint32_t i4 = texFaceArray[i * 4 + 3];
        if (i1 >= (uint32_t)mesh.nbVerts || i2 >= (uint32_t)mesh.nbVerts || i3 >= (uint32_t)mesh.nbVerts) continue;

        auto accumulateTri = [&](uint32_t idx1, uint32_t idx2, uint32_t idx3) {
            glm::vec3 v1(mesh.verts[idx1 * 3], mesh.verts[idx1 * 3 + 1], mesh.verts[idx1 * 3 + 2]);
            glm::vec3 v2(mesh.verts[idx2 * 3], mesh.verts[idx2 * 3 + 1], mesh.verts[idx2 * 3 + 2]);
            glm::vec3 v3(mesh.verts[idx3 * 3], mesh.verts[idx3 * 3 + 1], mesh.verts[idx3 * 3 + 2]);

            glm::vec2 w1(mesh.uvFlat[idx1 * 2], mesh.uvFlat[idx1 * 2 + 1]);
            glm::vec2 w2(mesh.uvFlat[idx2 * 2], mesh.uvFlat[idx2 * 2 + 1]);
            glm::vec2 w3(mesh.uvFlat[idx3 * 2], mesh.uvFlat[idx3 * 2 + 1]);

            float x1 = v2.x - v1.x;
            float x2 = v3.x - v1.x;
            float y1 = v2.y - v1.y;
            float y2 = v3.y - v1.y;
            float z1 = v2.z - v1.z;
            float z2 = v3.z - v1.z;

            float s1 = w2.x - w1.x;
            float s2 = w3.x - w1.x;
            float t1 = w2.y - w1.y;
            float t2 = w3.y - w1.y;

            float denom = (s1 * t2 - s2 * t1);
            float r = (std::abs(denom) > 1e-8f) ? (1.0f / denom) : 0.0f;

            glm::vec3 sdir((t2 * x1 - t1 * x2) * r, (t2 * y1 - t1 * y2) * r, (t2 * z1 - t1 * z2) * r);
            glm::vec3 tdir((s1 * x2 - s2 * x1) * r, (s1 * y2 - s2 * y1) * r, (s1 * z2 - s2 * z1) * r);

            tan1[idx1] += sdir; tan1[idx2] += sdir; tan1[idx3] += sdir;
            tan2[idx1] += tdir; tan2[idx2] += tdir; tan2[idx3] += tdir;
        };

        accumulateTri(i1, i2, i3);
        if (i4 != TRI_INDEX && i4 < (uint32_t)mesh.nbVerts) {
            accumulateTri(i1, i3, i4);
        }
    }

    for (int a = 0; a < mesh.nbVerts; ++a) {
        glm::vec3 n(mesh.normals[a * 3], mesh.normals[a * 3 + 1], mesh.normals[a * 3 + 2]);
        glm::vec3 t = tan1[a];

        glm::vec3 projT = t - n * glm::dot(n, t);
        if (glm::length(projT) > 1e-6f) {
            projT = glm::normalize(projT);
        } else {
            glm::vec3 c1 = glm::cross(n, glm::vec3(0.0f, 0.0f, 1.0f));
            glm::vec3 c2 = glm::cross(n, glm::vec3(0.0f, 1.0f, 0.0f));
            projT = (glm::length(c1) > glm::length(c2)) ? glm::normalize(c1) : glm::normalize(c2);
        }

        float w = (glm::dot(glm::cross(n, t), tan2[a]) < 0.0f) ? -1.0f : 1.0f;

        mesh.tangents[a * 4]     = projT.x;
        mesh.tangents[a * 4 + 1] = projT.y;
        mesh.tangents[a * 4 + 2] = projT.z;
        mesh.tangents[a * 4 + 3] = w;
    }
}

} // namespace MeshUtils
