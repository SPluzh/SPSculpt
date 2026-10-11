#include "files/ImportFBX.h"
#include "mesh/Mesh.h"
#include "mesh/Topology.h"
#include "common/Constants.h"
#include "common/StringUtils.h"
#include "common/Logger.h"
#include "ufbx/ufbx.h"
#include "../third_party/stb_image.h"

#include <GLES3/gl3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <chrono>
#include <cmath>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include <iostream>

namespace ImportFBX {

static GLuint loadTextureFromMemory(const void* data, size_t size) {
    if (!data || size == 0) return 0;
    int w = 0, h = 0, ch = 0;
    stbi_set_flip_vertically_on_load(0);
    stbi_uc* img = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(data), static_cast<int>(size), &w, &h, &ch, 4);
    if (!img) return 0;

    GLuint texId = 0;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(img);
    return texId;
}

static GLuint loadTextureFromFile(const std::string& path) {
    int w = 0, h = 0, ch = 0;
    stbi_set_flip_vertically_on_load(0);
    stbi_uc* img = nullptr;
#ifdef _WIN32
    std::wstring wpath = utf8ToWide(path);
    FILE* f = _wfopen(wpath.c_str(), L"rb");
    if (f) {
        img = stbi_load_from_file(f, &w, &h, &ch, 4);
        fclose(f);
    }
#else
    FILE* f = fopen(path.c_str(), "rb");
    if (f) {
        img = stbi_load_from_file(f, &w, &h, &ch, 4);
        fclose(f);
    }
#endif
    if (!img) return 0;

    GLuint texId = 0;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(img);
    return texId;
}

static Mesh* parseFBXMesh(ufbx_mesh* fbxMesh, ufbx_node* node, const std::string& basePath) {
    if (!fbxMesh || fbxMesh->num_vertices == 0 || fbxMesh->num_faces == 0) {
        return nullptr;
    }

    auto tStart = std::chrono::high_resolution_clock::now();
    std::string meshName = (node && node->name.length > 0) ? std::string(node->name.data, node->name.length) :
                           (fbxMesh->name.length > 0) ? std::string(fbxMesh->name.data, fbxMesh->name.length) : "FBX_Mesh";

    sculpt_log("[FBX Import] Parsing mesh '%s' (%zu verts, %zu faces)...\n",
               meshName.c_str(), fbxMesh->num_vertices, fbxMesh->num_faces);

    // Compute transformation matrix
    glm::mat4 worldMat(1.0f);
    bool isMirrored = false;
    if (node) {
        ufbx_matrix geomMat = node->geometry_to_world;
        worldMat[0] = glm::vec4(static_cast<float>(geomMat.cols[0].x), static_cast<float>(geomMat.cols[0].y), static_cast<float>(geomMat.cols[0].z), 0.0f);
        worldMat[1] = glm::vec4(static_cast<float>(geomMat.cols[1].x), static_cast<float>(geomMat.cols[1].y), static_cast<float>(geomMat.cols[1].z), 0.0f);
        worldMat[2] = glm::vec4(static_cast<float>(geomMat.cols[2].x), static_cast<float>(geomMat.cols[2].y), static_cast<float>(geomMat.cols[2].z), 0.0f);
        worldMat[3] = glm::vec4(static_cast<float>(geomMat.cols[3].x), static_cast<float>(geomMat.cols[3].y), static_cast<float>(geomMat.cols[3].z), 1.0f);
        isMirrored = (ufbx_matrix_determinant(&geomMat) < 0.0f);
    }

    size_t origNbVerts = fbxMesh->num_vertices;
    std::vector<float> origVerts(origNbVerts * 3);
    for (size_t i = 0; i < origNbVerts; ++i) {
        glm::vec4 p(
            static_cast<float>(fbxMesh->vertices.data[i].x),
            static_cast<float>(fbxMesh->vertices.data[i].y),
            static_cast<float>(fbxMesh->vertices.data[i].z),
            1.0f
        );
        glm::vec4 pWorld = worldMat * p;
        origVerts[i * 3 + 0] = pWorld.x;
        origVerts[i * 3 + 1] = pWorld.y;
        origVerts[i * 3 + 2] = pWorld.z;
    }

    // Determine if we need spatial welding (e.g. unindexed polygon soup where num_vertices == num_indices)
    bool isSoup = (fbxMesh->num_vertices == fbxMesh->num_indices);
    std::vector<uint32_t> remap(origNbVerts);
    std::vector<float> finalVerts;

    if (isSoup) {
        sculpt_log("[FBX Import] Mesh is unindexed triangle soup (%zu vertices); performing spatial welding...\n", origNbVerts);
        float minX = 1e30f, minY = 1e30f, minZ = 1e30f;
        float maxX = -1e30f, maxY = -1e30f, maxZ = -1e30f;
        for (size_t i = 0; i < origNbVerts; ++i) {
            minX = std::min(minX, origVerts[i * 3 + 0]);
            minY = std::min(minY, origVerts[i * 3 + 1]);
            minZ = std::min(minZ, origVerts[i * 3 + 2]);
            maxX = std::max(maxX, origVerts[i * 3 + 0]);
            maxY = std::max(maxY, origVerts[i * 3 + 1]);
            maxZ = std::max(maxZ, origVerts[i * 3 + 2]);
        }
        float diag = std::sqrt((maxX - minX)*(maxX - minX) + (maxY - minY)*(maxY - minY) + (maxZ - minZ)*(maxZ - minZ));
        float eps = std::max(1e-5f, diag * 1e-5f);
        float epsSq = eps * eps;
        float invCellSize = 1.0f / eps;

        struct PosKey {
            int64_t x, y, z;
            bool operator==(const PosKey& o) const { return x == o.x && y == o.y && z == o.z; }
        };
        struct PosKeyHash {
            size_t operator()(const PosKey& k) const {
                return std::hash<int64_t>{}(k.x) ^ (std::hash<int64_t>{}(k.y) << 1) ^ (std::hash<int64_t>{}(k.z) << 2);
            }
        };

        std::unordered_map<PosKey, std::vector<uint32_t>, PosKeyHash> gridMap;
        finalVerts.reserve(origNbVerts * 3);

        for (size_t i = 0; i < origNbVerts; ++i) {
            float vx = origVerts[i * 3 + 0];
            float vy = origVerts[i * 3 + 1];
            float vz = origVerts[i * 3 + 2];

            int64_t cx = static_cast<int64_t>(std::floor(vx * invCellSize));
            int64_t cy = static_cast<int64_t>(std::floor(vy * invCellSize));
            int64_t cz = static_cast<int64_t>(std::floor(vz * invCellSize));

            int32_t foundIdx = -1;
            for (int dx = -1; dx <= 1 && foundIdx < 0; ++dx) {
                for (int dy = -1; dy <= 1 && foundIdx < 0; ++dy) {
                    for (int dz = -1; dz <= 1 && foundIdx < 0; ++dz) {
                        PosKey key{cx + dx, cy + dy, cz + dz};
                        auto it = gridMap.find(key);
                        if (it != gridMap.end()) {
                            for (uint32_t wIdx : it->second) {
                                float wx = finalVerts[wIdx * 3 + 0];
                                float wy = finalVerts[wIdx * 3 + 1];
                                float wz = finalVerts[wIdx * 3 + 2];
                                float dSq = (vx - wx)*(vx - wx) + (vy - wy)*(vy - wy) + (vz - wz)*(vz - wz);
                                if (dSq <= epsSq) {
                                    foundIdx = static_cast<int32_t>(wIdx);
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            if (foundIdx < 0) {
                uint32_t newIdx = static_cast<uint32_t>(finalVerts.size() / 3);
                remap[i] = newIdx;
                finalVerts.push_back(vx);
                finalVerts.push_back(vy);
                finalVerts.push_back(vz);
                gridMap[{cx, cy, cz}].push_back(newIdx);
            } else {
                remap[i] = static_cast<uint32_t>(foundIdx);
            }
        }
        sculpt_log("[FBX Import] Welding complete: %zu -> %zu unique vertices\n", origNbVerts, finalVerts.size() / 3);
    } else {
        for (size_t i = 0; i < origNbVerts; ++i) {
            remap[i] = static_cast<uint32_t>(i);
        }
        finalVerts = std::move(origVerts);
    }

    size_t finalNbVerts = finalVerts.size() / 3;

    // Build faces (preserving triangles & quads, triangulating n-gons)
    bool hasUV = (fbxMesh->vertex_uv.exists && fbxMesh->vertex_uv.values.count > 0);
    std::vector<float> uvAr;
    if (hasUV) {
        uvAr.resize(fbxMesh->vertex_uv.values.count * 2);
        for (size_t i = 0; i < fbxMesh->vertex_uv.values.count; ++i) {
            uvAr[i * 2 + 0] = static_cast<float>(fbxMesh->vertex_uv.values.data[i].x);
            uvAr[i * 2 + 1] = static_cast<float>(fbxMesh->vertex_uv.values.data[i].y);
        }
    }

    std::vector<uint32_t> faces;
    std::vector<uint32_t> faceGroups;
    std::vector<uint32_t> uvfAr;
    faces.reserve(fbxMesh->num_faces * 4);
    faceGroups.reserve(fbxMesh->num_faces);
    if (hasUV) uvfAr.reserve(fbxMesh->num_faces * 4);

    bool hasMaterials = (fbxMesh->face_material.count > 0);

    for (size_t f = 0; f < fbxMesh->num_faces; ++f) {
        ufbx_face face = fbxMesh->faces.data[f];
        uint32_t numIndices = face.num_indices;
        uint32_t first = face.index_begin;
        if (numIndices < 3) continue;

        uint32_t gid = 0;
        if (hasMaterials && f < fbxMesh->face_material.count) {
            gid = fbxMesh->face_material.data[f] + 1; // 1-based polygroup
        }

        if (numIndices == 3) {
            uint32_t c0 = first + 0;
            uint32_t c1 = first + 1;
            uint32_t c2 = first + 2;
            if (isMirrored) std::swap(c1, c2);

            uint32_t v0 = remap[fbxMesh->vertex_indices.data[c0]];
            uint32_t v1 = remap[fbxMesh->vertex_indices.data[c1]];
            uint32_t v2 = remap[fbxMesh->vertex_indices.data[c2]];

            faces.push_back(v0);
            faces.push_back(v1);
            faces.push_back(v2);
            faces.push_back(TRI_INDEX);
            faceGroups.push_back(gid);

            if (hasUV) {
                uint32_t u0 = (c0 < fbxMesh->vertex_uv.indices.count) ? fbxMesh->vertex_uv.indices.data[c0] : 0;
                uint32_t u1 = (c1 < fbxMesh->vertex_uv.indices.count) ? fbxMesh->vertex_uv.indices.data[c1] : 0;
                uint32_t u2 = (c2 < fbxMesh->vertex_uv.indices.count) ? fbxMesh->vertex_uv.indices.data[c2] : 0;
                uvfAr.push_back(u0);
                uvfAr.push_back(u1);
                uvfAr.push_back(u2);
                uvfAr.push_back(TRI_INDEX);
            }
        } else if (numIndices == 4) {
            uint32_t c0 = first + 0;
            uint32_t c1 = first + 1;
            uint32_t c2 = first + 2;
            uint32_t c3 = first + 3;
            if (isMirrored) std::swap(c1, c3);

            uint32_t v0 = remap[fbxMesh->vertex_indices.data[c0]];
            uint32_t v1 = remap[fbxMesh->vertex_indices.data[c1]];
            uint32_t v2 = remap[fbxMesh->vertex_indices.data[c2]];
            uint32_t v3 = remap[fbxMesh->vertex_indices.data[c3]];

            faces.push_back(v0);
            faces.push_back(v1);
            faces.push_back(v2);
            faces.push_back(v3);
            faceGroups.push_back(gid);

            if (hasUV) {
                uint32_t u0 = (c0 < fbxMesh->vertex_uv.indices.count) ? fbxMesh->vertex_uv.indices.data[c0] : 0;
                uint32_t u1 = (c1 < fbxMesh->vertex_uv.indices.count) ? fbxMesh->vertex_uv.indices.data[c1] : 0;
                uint32_t u2 = (c2 < fbxMesh->vertex_uv.indices.count) ? fbxMesh->vertex_uv.indices.data[c2] : 0;
                uint32_t u3 = (c3 < fbxMesh->vertex_uv.indices.count) ? fbxMesh->vertex_uv.indices.data[c3] : 0;
                uvfAr.push_back(u0);
                uvfAr.push_back(u1);
                uvfAr.push_back(u2);
                uvfAr.push_back(u3);
            }
        } else {
            // N-gon fan triangulation
            for (uint32_t k = 1; k < numIndices - 1; ++k) {
                uint32_t c0 = first;
                uint32_t c1 = first + k;
                uint32_t c2 = first + k + 1;
                if (isMirrored) std::swap(c1, c2);

                uint32_t v0 = remap[fbxMesh->vertex_indices.data[c0]];
                uint32_t v1 = remap[fbxMesh->vertex_indices.data[c1]];
                uint32_t v2 = remap[fbxMesh->vertex_indices.data[c2]];

                faces.push_back(v0);
                faces.push_back(v1);
                faces.push_back(v2);
                faces.push_back(TRI_INDEX);
                faceGroups.push_back(gid);

                if (hasUV) {
                    uint32_t u0 = (c0 < fbxMesh->vertex_uv.indices.count) ? fbxMesh->vertex_uv.indices.data[c0] : 0;
                    uint32_t u1 = (c1 < fbxMesh->vertex_uv.indices.count) ? fbxMesh->vertex_uv.indices.data[c1] : 0;
                    uint32_t u2 = (c2 < fbxMesh->vertex_uv.indices.count) ? fbxMesh->vertex_uv.indices.data[c2] : 0;
                    uvfAr.push_back(u0);
                    uvfAr.push_back(u1);
                    uvfAr.push_back(u2);
                    uvfAr.push_back(TRI_INDEX);
                }
            }
        }
    }

    size_t finalNbFaces = faces.size() / 4;

    // Normals extraction
    std::vector<float> normals;
    bool hasNormals = fbxMesh->vertex_normal.exists;
    if (hasNormals) {
        normals.assign(finalNbVerts * 3, 0.0f);
        glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(worldMat)));
        for (size_t f = 0; f < fbxMesh->num_faces; ++f) {
            ufbx_face face = fbxMesh->faces.data[f];
            for (uint32_t k = 0; k < face.num_indices; ++k) {
                uint32_t c = face.index_begin + k;
                if (c < fbxMesh->vertex_indices.count) {
                    uint32_t origV = fbxMesh->vertex_indices.data[c];
                    uint32_t v = remap[origV];
                    if (c < fbxMesh->vertex_normal.indices.count) {
                        uint32_t nIdx = fbxMesh->vertex_normal.indices.data[c];
                        if (nIdx < fbxMesh->vertex_normal.values.count) {
                            ufbx_vec3 n = fbxMesh->vertex_normal.values.data[nIdx];
                            glm::vec3 nWorld = normalMat * glm::vec3(
                                static_cast<float>(n.x),
                                static_cast<float>(n.y),
                                static_cast<float>(n.z)
                            );
                            normals[v * 3 + 0] += nWorld.x;
                            normals[v * 3 + 1] += nWorld.y;
                            normals[v * 3 + 2] += nWorld.z;
                        }
                    }
                }
            }
        }
        for (size_t i = 0; i < finalNbVerts; ++i) {
            float nx = normals[i * 3 + 0];
            float ny = normals[i * 3 + 1];
            float nz = normals[i * 3 + 2];
            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-6f) {
                normals[i * 3 + 0] = nx / len;
                normals[i * 3 + 1] = ny / len;
                normals[i * 3 + 2] = nz / len;
            } else {
                normals[i * 3 + 0] = 0.0f;
                normals[i * 3 + 1] = 1.0f;
                normals[i * 3 + 2] = 0.0f;
            }
        }
    }

    // Vertex colors & material colors
    std::vector<float> colors(finalNbVerts * 3, 1.0f);
    if (fbxMesh->vertex_color.exists) {
        for (size_t f = 0; f < fbxMesh->num_faces; ++f) {
            ufbx_face face = fbxMesh->faces.data[f];
            for (uint32_t k = 0; k < face.num_indices; ++k) {
                uint32_t c = face.index_begin + k;
                if (c < fbxMesh->vertex_indices.count && c < fbxMesh->vertex_color.indices.count) {
                    uint32_t origV = fbxMesh->vertex_indices.data[c];
                    uint32_t v = remap[origV];
                    uint32_t cIdx = fbxMesh->vertex_color.indices.data[c];
                    if (cIdx < fbxMesh->vertex_color.values.count) {
                        ufbx_vec4 col = fbxMesh->vertex_color.values.data[cIdx];
                        colors[v * 3 + 0] = static_cast<float>(col.x);
                        colors[v * 3 + 1] = static_cast<float>(col.y);
                        colors[v * 3 + 2] = static_cast<float>(col.z);
                    }
                }
            }
        }
    } else if (fbxMesh->materials.count > 0 && fbxMesh->materials.data[0]) {
        ufbx_material* mat = fbxMesh->materials.data[0];
        float r = 0.8f, g = 0.8f, b = 0.8f;
        if (mat->pbr.base_color.has_value) {
            r = static_cast<float>(mat->pbr.base_color.value_vec3.x);
            g = static_cast<float>(mat->pbr.base_color.value_vec3.y);
            b = static_cast<float>(mat->pbr.base_color.value_vec3.z);
        } else if (mat->fbx.diffuse_color.has_value) {
            r = static_cast<float>(mat->fbx.diffuse_color.value_vec3.x);
            g = static_cast<float>(mat->fbx.diffuse_color.value_vec3.y);
            b = static_cast<float>(mat->fbx.diffuse_color.value_vec3.z);
        }
        if (r < 0.05f && g < 0.05f && b < 0.05f) {
            r = 0.8f; g = 0.8f; b = 0.8f;
        }
        for (size_t i = 0; i < finalNbVerts; ++i) {
            colors[i * 3 + 0] = r;
            colors[i * 3 + 1] = g;
            colors[i * 3 + 2] = b;
        }
    } else {
        for (size_t i = 0; i < finalNbVerts; ++i) {
            colors[i * 3 + 0] = 0.8f;
            colors[i * 3 + 1] = 0.8f;
            colors[i * 3 + 2] = 0.8f;
        }
    }

    // Material properties (roughness, metallic, mask)
    std::vector<float> materials(finalNbVerts * 3);
    float rough = 0.5f;
    float metal = 0.0f;
    if (fbxMesh->materials.count > 0 && fbxMesh->materials.data[0]) {
        ufbx_material* mat = fbxMesh->materials.data[0];
        if (mat->pbr.roughness.has_value) rough = static_cast<float>(mat->pbr.roughness.value_real);
        if (mat->pbr.metalness.has_value) metal = static_cast<float>(mat->pbr.metalness.value_real);
    }
    rough = std::clamp(rough, 0.2f, 1.0f);
    metal = std::clamp(metal, 0.0f, 1.0f);
    for (size_t i = 0; i < finalNbVerts; ++i) {
        materials[i * 3 + 0] = rough;
        materials[i * 3 + 1] = metal;
        materials[i * 3 + 2] = 1.0f;
    }

    // Allocate Mesh
    Mesh* mesh = new Mesh();
    mesh->verts = std::move(finalVerts);
    mesh->normals = std::move(normals);
    mesh->hasCustomNormals = hasNormals;
    mesh->colors = std::move(colors);
    mesh->materials = std::move(materials);
    mesh->faces = std::move(faces);
    mesh->faceGroups = std::move(faceGroups);
    mesh->nbVerts = static_cast<int>(finalNbVerts);
    mesh->nbFaces = static_cast<int>(finalNbFaces);

    // UV & Seam processing
    if (hasUV && !uvAr.empty() && uvfAr.size() == mesh->faces.size()) {
        sculpt_log("[FBX Import] Initializing UVs and seam vertices...\n");
        mesh->initTexCoordsDataFromOBJData(uvAr, uvfAr);
        mesh->uvFlat = mesh->texCoords;
        mesh->hasUV = true;
    }

    // Check for Normal Map texture
    if (fbxMesh->materials.count > 0 && fbxMesh->materials.data[0]) {
        ufbx_material* mat = fbxMesh->materials.data[0];
        ufbx_texture* nTex = nullptr;
        if (mat->pbr.normal_map.texture) nTex = mat->pbr.normal_map.texture;
        else if (mat->fbx.normal_map.texture) nTex = mat->fbx.normal_map.texture;
        else if (mat->fbx.bump.texture) nTex = mat->fbx.bump.texture;

        if (nTex) {
            GLuint texId = 0;
            if (nTex->content.size > 0 && nTex->content.data) {
                sculpt_log("[FBX Texture] Loading embedded normal map texture (%zu bytes)...\n", nTex->content.size);
                texId = loadTextureFromMemory(nTex->content.data, nTex->content.size);
            } else if (nTex->filename.length > 0) {
                std::string texPath(nTex->filename.data, nTex->filename.length);
                sculpt_log("[FBX Texture] Loading normal map texture from file: '%s'...\n", texPath.c_str());
                texId = loadTextureFromFile(texPath);
                if (!texId && !basePath.empty()) {
                    std::string localName = texPath;
                    size_t s = localName.find_last_of("/\\");
                    if (s != std::string::npos) localName = localName.substr(s + 1);
                    std::string combined = basePath + "/" + localName;
                    sculpt_log("[FBX Texture] Trying relative normal map texture: '%s'...\n", combined.c_str());
                    texId = loadTextureFromFile(combined);
                }
            }
            if (texId) {
                mesh->normalMapTexId = texId;
                sculpt_log("[FBX Texture] Normal map successfully bound (GL ID: %u)\n", texId);
            }
        }
    }

    // Compute Topology
    sculpt_log("[FBX Import] Computing topology (nbVerts=%d, nbFaces=%d)...\n", mesh->nbVerts, mesh->nbFaces);
    std::vector<uint32_t> vrvStartCount;
    std::vector<uint32_t> vertRingVert;
    std::vector<uint32_t> vrfStartCount;
    std::vector<uint32_t> vertRingFace;
    std::vector<uint8_t> vertOnEdge;

    computeTopology(
        mesh->nbVerts, mesh->faces.data(), mesh->nbFaces,
        vrfStartCount, vertRingFace, vrvStartCount, vertRingVert, vertOnEdge
    );

    mesh->vrfStartCount = std::move(vrfStartCount);
    mesh->vertRingFace = std::move(vertRingFace);
    mesh->vrvStartCount = std::move(vrvStartCount);
    mesh->vertRingVert = std::move(vertRingVert);
    mesh->vertOnEdge = std::move(vertOnEdge);

    // Post-Init & World transform
    mesh->postInit();
    mesh->setMatrix(glm::mat4(1.0f));
    mesh->outlinerName = meshName;

    auto tEnd = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    sculpt_log("[FBX Import] Mesh '%s' built in %.2f ms\n", meshName.c_str(), ms);

    return mesh;
}

std::vector<Mesh*> importFBX(const std::string& path) {
    auto tStart = std::chrono::high_resolution_clock::now();
    sculpt_log("[FBX Import] Starting FBX file import: '%s'\n", path.c_str());

    ufbx_load_opts opts = { 0 };
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.clean_skin_weights = true;
    opts.use_blender_pbr_material = true;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(path.c_str(), &opts, &error);
    if (!scene) {
        sculpt_log("[FBX Import ERROR] Failed to load '%s': %s (info: %s)\n",
                   path.c_str(), error.description.data, error.info);
        return {};
    }

    std::string basePath;
    size_t slashIdx = path.find_last_of("/\\");
    if (slashIdx != std::string::npos) {
        basePath = path.substr(0, slashIdx);
    }

    std::vector<Mesh*> meshes;

    // 1. Process meshes attached to nodes
    for (size_t i = 0; i < scene->nodes.count; ++i) {
        ufbx_node* node = scene->nodes.data[i];
        if (!node || !node->mesh) continue;
        Mesh* mesh = parseFBXMesh(node->mesh, node, basePath);
        if (mesh) {
            meshes.push_back(mesh);
        }
    }

    // 2. Fallback: if no nodes had meshes, process all meshes in scene
    if (meshes.empty()) {
        for (size_t i = 0; i < scene->meshes.count; ++i) {
            ufbx_mesh* fbxMesh = scene->meshes.data[i];
            if (!fbxMesh) continue;
            Mesh* mesh = parseFBXMesh(fbxMesh, nullptr, basePath);
            if (mesh) {
                meshes.push_back(mesh);
            }
        }
    }

    ufbx_free_scene(scene);

    auto tEnd = std::chrono::high_resolution_clock::now();
    double msTotal = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    sculpt_log("[FBX Import] FBX import completed in %.2f ms (%zu mesh(es))\n", msTotal, meshes.size());

    return meshes;
}

} // namespace ImportFBX
