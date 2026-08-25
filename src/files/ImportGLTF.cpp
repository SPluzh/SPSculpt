#include "files/ImportGLTF.h"
#include "files/MeshUtils.h"
#include "files/Base64.h"
#include "common/Constants.h"
#include "mesh/Topology.h"
#include "common/StringUtils.h"
#include "common/Logger.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <iostream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <GLES3/gl3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include "../third_party/stb_image.h"

using json = nlohmann::json;

namespace ImportGLTF {

static GLuint loadTextureFromFile(const std::string& path) {
    auto tStart = std::chrono::high_resolution_clock::now();
    sculpt_log("[GLTF Texture] Loading texture from file: '%s'\n", path.c_str());

    int w = 0, h = 0, ch = 0;
    stbi_set_flip_vertically_on_load(0);
    stbi_uc* data = nullptr;

#ifdef _WIN32
    std::wstring wpath = utf8ToWide(path);
    FILE* f = _wfopen(wpath.c_str(), L"rb");
    if (f) {
        data = stbi_load_from_file(f, &w, &h, &ch, 4);
        fclose(f);
    }
#else
    FILE* f = fopen(path.c_str(), "rb");
    if (f) {
        data = stbi_load_from_file(f, &w, &h, &ch, 4);
        fclose(f);
    }
#endif

    if (!data) {
        sculpt_log("[GLTF Texture ERROR] Failed to load image via stb_image: '%s'\n", path.c_str());
        return 0;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);
    stbi_image_free(data);

    double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStart).count();
    sculpt_log("[GLTF Texture] Successfully loaded %dx%d image (%d channels), GL texID=%u in %.2f ms\n",
              w, h, ch, tex, ms);
    return tex;
}

static std::vector<uint8_t> readExternalFile(const std::string& basePath,
                                              const std::string& uri) {
    auto tStart = std::chrono::high_resolution_clock::now();
    std::string fullPath;
    if (basePath.empty()) {
        fullPath = uri;
    } else if (basePath.back() == '/' || basePath.back() == '\\') {
        fullPath = basePath + uri;
    } else {
        fullPath = basePath + "/" + uri;
    }

    sculpt_log("[GLTF External] Opening external binary file: '%s'\n", fullPath.c_str());

#ifdef _WIN32
    std::ifstream file(utf8ToWide(fullPath).c_str(), std::ios::binary | std::ios::ate);
#else
    std::ifstream file(fullPath, std::ios::binary | std::ios::ate);
#endif
    if (!file.is_open()) {
        sculpt_log("[GLTF External ERROR] Cannot open external file: '%s'\n", fullPath.c_str());
        return {};
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    if (file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStart).count();
        double mb = static_cast<double>(size) / (1024.0 * 1024.0);
        sculpt_log("[GLTF External] Read %.2f MB from disk in %.2f ms\n", mb, ms);
        return buffer;
    }
    sculpt_log("[GLTF External ERROR] Failed reading %lld bytes from file: '%s'\n", (long long)size, fullPath.c_str());
    return {};
}

struct Property {
    std::string type;
    std::string type2;
    std::string name;
    int offsetOctet = 0;
    int id = 0;
};

template <typename T>
static T readValueAt(const uint8_t* ptr, int componentType) {
    if (componentType == 5120) { // BYTE
        return static_cast<T>(*reinterpret_cast<const int8_t*>(ptr));
    }
    if (componentType == 5121) { // UNSIGNED_BYTE
        return static_cast<T>(*reinterpret_cast<const uint8_t*>(ptr));
    }
    if (componentType == 5122) { // SHORT
        return static_cast<T>(*reinterpret_cast<const int16_t*>(ptr));
    }
    if (componentType == 5123) { // UNSIGNED_SHORT
        return static_cast<T>(*reinterpret_cast<const uint16_t*>(ptr));
    }
    if (componentType == 5125) { // UNSIGNED_INT
        return static_cast<T>(*reinterpret_cast<const uint32_t*>(ptr));
    }
    if (componentType == 5126) { // FLOAT
        return static_cast<T>(*reinterpret_cast<const float*>(ptr));
    }
    return 0;
}

static size_t getComponentTypeByteSize(int componentType) {
    if (componentType == 5120 || componentType == 5121) return 1;
    if (componentType == 5122 || componentType == 5123) return 2;
    if (componentType == 5125 || componentType == 5126) return 4;
    return 0;
}

static std::vector<float> getAccessorFloatArray(const json& j, int accessorIdx, const std::vector<std::vector<uint8_t>>& binaryBuffers) {
    if (accessorIdx < 0 || accessorIdx >= (int)j["accessors"].size()) {
        sculpt_log("[GLTF Accessor ERROR] Invalid accessor index: %d\n", accessorIdx);
        return {};
    }
    const auto& accessor = j["accessors"][accessorIdx];
    
    int bvIdx = accessor.value("bufferView", -1);
    if (bvIdx < 0 || bvIdx >= (int)j["bufferViews"].size()) {
        sculpt_log("[GLTF Accessor ERROR] Invalid bufferView index: %d in accessor %d\n", bvIdx, accessorIdx);
        return {};
    }
    const auto& bv = j["bufferViews"][bvIdx];
    
    int bufIdx = bv.value("buffer", -1);
    if (bufIdx < 0 || bufIdx >= (int)binaryBuffers.size()) {
        sculpt_log("[GLTF Accessor ERROR] Invalid buffer index: %d (available: %zu) in bufferView %d\n", bufIdx, binaryBuffers.size(), bvIdx);
        return {};
    }
    const auto& bufferData = binaryBuffers[bufIdx];
    
    size_t byteOffset = bv.value("byteOffset", 0) + accessor.value("byteOffset", 0);
    int componentType = accessor["componentType"];
    size_t count = accessor["count"];
    
    std::string type = accessor["type"];
    size_t elementSize = 1;
    if (type == "VEC2") elementSize = 2;
    else if (type == "VEC3") elementSize = 3;
    else if (type == "VEC4") elementSize = 4;
    
    size_t compSize = getComponentTypeByteSize(componentType);
    size_t byteStride = bv.value("byteStride", 0);
    if (byteStride == 0) {
        byteStride = elementSize * compSize;
    }
    
    std::vector<float> result(count * elementSize);
    for (size_t i = 0; i < count; ++i) {
        size_t elemOffset = byteOffset + i * byteStride;
        for (size_t k = 0; k < elementSize; ++k) {
            size_t off = elemOffset + k * compSize;
            if (off + compSize <= bufferData.size()) {
                result[i * elementSize + k] = readValueAt<float>(bufferData.data() + off, componentType);
            }
        }
    }
    
    if (componentType != 5126) {
        if (componentType == 5121) {
            for (auto& val : result) val /= 255.0f;
        } else if (componentType == 5123) {
            for (auto& val : result) val /= 65535.0f;
        }
    }
    
    return result;
}

static std::vector<uint32_t> getAccessorUintArray(const json& j, int accessorIdx, const std::vector<std::vector<uint8_t>>& binaryBuffers) {
    if (accessorIdx < 0 || accessorIdx >= (int)j["accessors"].size()) {
        sculpt_log("[GLTF Accessor ERROR] Invalid accessor index: %d\n", accessorIdx);
        return {};
    }
    const auto& accessor = j["accessors"][accessorIdx];
    
    int bvIdx = accessor.value("bufferView", -1);
    if (bvIdx < 0 || bvIdx >= (int)j["bufferViews"].size()) {
        sculpt_log("[GLTF Accessor ERROR] Invalid bufferView index: %d in accessor %d\n", bvIdx, accessorIdx);
        return {};
    }
    const auto& bv = j["bufferViews"][bvIdx];
    
    int bufIdx = bv.value("buffer", -1);
    if (bufIdx < 0 || bufIdx >= (int)binaryBuffers.size()) {
        sculpt_log("[GLTF Accessor ERROR] Invalid buffer index: %d (available: %zu) in bufferView %d\n", bufIdx, binaryBuffers.size(), bvIdx);
        return {};
    }
    const auto& bufferData = binaryBuffers[bufIdx];
    
    size_t byteOffset = bv.value("byteOffset", 0) + accessor.value("byteOffset", 0);
    int componentType = accessor["componentType"];
    size_t count = accessor["count"];
    
    std::string type = accessor["type"];
    size_t elementSize = 1;
    if (type == "VEC2") elementSize = 2;
    else if (type == "VEC3") elementSize = 3;
    else if (type == "VEC4") elementSize = 4;
    
    size_t compSize = getComponentTypeByteSize(componentType);
    size_t byteStride = bv.value("byteStride", 0);
    if (byteStride == 0) {
        byteStride = elementSize * compSize;
    }
    
    std::vector<uint32_t> result(count * elementSize);
    for (size_t i = 0; i < count; ++i) {
        size_t elemOffset = byteOffset + i * byteStride;
        for (size_t k = 0; k < elementSize; ++k) {
            size_t off = elemOffset + k * compSize;
            if (off + compSize <= bufferData.size()) {
                result[i * elementSize + k] = readValueAt<uint32_t>(bufferData.data() + off, componentType);
            }
        }
    }
    return result;
}

static Mesh* parsePrimitive(const json& j, const json& primitive, const std::vector<std::vector<uint8_t>>& binaryBuffers) {
    auto tStart = std::chrono::high_resolution_clock::now();
    if (!primitive.contains("attributes")) {
        sculpt_log("[GLTF Primitive ERROR] Primitive contains no 'attributes' key\n");
        return nullptr;
    }
    const auto& attrs = primitive["attributes"];
    if (!attrs.contains("POSITION")) {
        sculpt_log("[GLTF Primitive ERROR] Primitive has no 'POSITION' attribute\n");
        return nullptr;
    }
    
    int posAccessor = attrs["POSITION"];
    std::vector<float> vAr = getAccessorFloatArray(j, posAccessor, binaryBuffers);
    if (vAr.empty()) {
        sculpt_log("[GLTF Primitive ERROR] POSITION accessor (%d) returned empty float array\n", posAccessor);
        return nullptr;
    }
    
    Mesh* mesh = new Mesh();
    mesh->verts = vAr;
    mesh->nbVerts = (int)(vAr.size() / 3);
    
    sculpt_log("[GLTF Primitive] Parsing primitive: nbVerts=%d\n", mesh->nbVerts);

    if (attrs.contains("NORMAL")) {
        mesh->normals = getAccessorFloatArray(j, attrs["NORMAL"], binaryBuffers);
        sculpt_log("[GLTF Primitive]   - NORMAL attribute loaded: %zu floats\n", mesh->normals.size());
    } else {
        sculpt_log("[GLTF Primitive]   - No NORMAL attribute found\n");
    }
    
    if (attrs.contains("COLOR_0")) {
        std::vector<float> cAr = getAccessorFloatArray(j, attrs["COLOR_0"], binaryBuffers);
        if (!cAr.empty()) {
            mesh->colors = cAr;
            sculpt_log("[GLTF Primitive]   - COLOR_0 attribute loaded: %zu floats\n", mesh->colors.size());
        }
    }
    if (mesh->colors.size() != (size_t)mesh->nbVerts * 3) {
        mesh->colors.assign(mesh->nbVerts * 3, 1.0f);
    }
    
    mesh->materials.resize(mesh->nbVerts * 3);
    for (int k = 0; k < mesh->nbVerts; ++k) {
        mesh->materials[k * 3]     = 0.5f; // roughness
        mesh->materials[k * 3 + 1] = 0.0f; // metalness
        mesh->materials[k * 3 + 2] = 1.0f; // mask
    }

    if (primitive.contains("material") && j.contains("materials")) {
        int matIdx = primitive["material"].get<int>();
        if (matIdx >= 0 && matIdx < (int)j["materials"].size()) {
            const auto& mat = j["materials"][matIdx];
            if (mat.contains("pbrMetallicRoughness")) {
                const auto& pbr = mat["pbrMetallicRoughness"];
                if (pbr.contains("baseColorFactor") && !attrs.contains("COLOR_0")) {
                    auto bcf = pbr["baseColorFactor"];
                    if (bcf.size() >= 3) {
                        float r = bcf[0].get<float>(), g = bcf[1].get<float>(), b = bcf[2].get<float>();
                        for (int k = 0; k < mesh->nbVerts; ++k) {
                            mesh->colors[k * 3]     = r;
                            mesh->colors[k * 3 + 1] = g;
                            mesh->colors[k * 3 + 2] = b;
                        }
                        sculpt_log("[GLTF Primitive]   - Applied baseColorFactor [%.2f, %.2f, %.2f]\n", r, g, b);
                    }
                }
                float rough = pbr.value("roughnessFactor", 0.5f);
                float metal = pbr.value("metallicFactor", 0.0f);
                for (int k = 0; k < mesh->nbVerts; ++k) {
                    mesh->materials[k * 3]     = rough;
                    mesh->materials[k * 3 + 1] = metal;
                }
                sculpt_log("[GLTF Primitive]   - Applied PBR factors: roughness=%.3f, metallic=%.3f\n", rough, metal);
            }
        }
    }
    
    std::vector<float> uvAr;
    if (attrs.contains("TEXCOORD_0")) {
        uvAr = getAccessorFloatArray(j, attrs["TEXCOORD_0"], binaryBuffers);
        if (uvAr.size() == (size_t)mesh->nbVerts * 2) {
            mesh->uvFlat = uvAr;
            mesh->hasUV = true;
            sculpt_log("[GLTF Primitive]   - TEXCOORD_0 attribute loaded: %zu UV pairs\n", uvAr.size() / 2);
        }
    }

    if (attrs.contains("TANGENT")) {
        std::vector<float> tangArr = getAccessorFloatArray(j, attrs["TANGENT"], binaryBuffers);
        if (tangArr.size() == (size_t)mesh->nbVerts * 4) {
            mesh->tangents = tangArr;
            sculpt_log("[GLTF Primitive]   - TANGENT attribute loaded: %zu tangent vectors\n", tangArr.size() / 4);
        }
    }

    if (mesh->tangents.empty() && !mesh->uvFlat.empty()) {
        sculpt_log("[GLTF Primitive]   - TANGENT attribute absent. Generating procedural tangents from UVs...\n");
        if (mesh->normals.size() != (size_t)mesh->nbVerts * 3) {
            mesh->postInit();
        }
        MeshUtils::computeTangents(*mesh);
        sculpt_log("[GLTF Primitive]   - Successfully generated %zu procedural tangent vectors\n", mesh->tangents.size() / 4);
    }
    
    std::vector<uint32_t> iAr;
    if (primitive.contains("indices")) {
        iAr = getAccessorUintArray(j, primitive["indices"], binaryBuffers);
        sculpt_log("[GLTF Primitive]   - Loaded %zu face indices\n", iAr.size());
    } else {
        iAr.resize(mesh->nbVerts);
        for (int i = 0; i < mesh->nbVerts; ++i) {
            iAr[i] = i;
        }
        sculpt_log("[GLTF Primitive]   - No indices specified; generated %d sequential indices\n", mesh->nbVerts);
    }
    
    size_t numTris = iAr.size() / 3;
    mesh->faces.resize(numTris * 4);
    for (size_t t = 0; t < numTris; ++t) {
        size_t idt = t * 4;
        size_t idi = t * 3;
        mesh->faces[idt] = iAr[idi];
        mesh->faces[idt + 1] = iAr[idi + 1];
        mesh->faces[idt + 2] = iAr[idi + 2];
        mesh->faces[idt + 3] = TRI_INDEX;
    }
    mesh->nbFaces = (int)numTris;
    
    // Bounds validation for vertex indices
    uint32_t maxVertIndex = (uint32_t)mesh->nbVerts;
    uint32_t outOfBoundsCount = 0;
    for (size_t i = 0; i < mesh->faces.size(); ++i) {
        uint32_t vid = mesh->faces[i];
        if (vid != TRI_INDEX && vid >= maxVertIndex) {
            outOfBoundsCount++;
            mesh->faces[i] = 0;
        }
    }
    if (outOfBoundsCount > 0) {
        sculpt_log("[GLTF Primitive WARNING] Corrected %u out-of-bounds face indices!\n", outOfBoundsCount);
    }

    if (!mesh->uvFlat.empty()) {
        mesh->texCoords = mesh->uvFlat;
        mesh->facesTexCoord = mesh->faces;
        mesh->hasUV = true;
    }
    
    // Compute topology
    sculpt_log("[GLTF Primitive]   - Computing topology (nbVerts=%d, nbFaces=%d)...\n", mesh->nbVerts, mesh->nbFaces);
    auto tTopoStart = std::chrono::high_resolution_clock::now();
    std::vector<uint32_t> vrvStartCount;
    std::vector<uint32_t> vertRingVert;
    std::vector<uint32_t> vrfStartCount;
    std::vector<uint32_t> vertRingFace;
    std::vector<uint8_t> vertOnEdge;
    
    computeTopology(
        mesh->nbVerts, mesh->faces.data(), mesh->nbFaces,
        vrfStartCount, vertRingFace, vrvStartCount, vertRingVert, vertOnEdge
    );

    mesh->vrfStartCount = vrfStartCount;
    mesh->vertRingFace = vertRingFace;
    mesh->vrvStartCount = vrvStartCount;
    mesh->vertRingVert = vertRingVert;
    mesh->vertOnEdge = vertOnEdge;
    double msTopo = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tTopoStart).count();
    sculpt_log("[GLTF Primitive]   - Topology computed in %.2f ms\n", msTopo);

    sculpt_log("[GLTF Primitive]   - Running mesh postInit (Octree & BBox)...\n");
    auto tPostStart = std::chrono::high_resolution_clock::now();
    mesh->postInit();
    double msPost = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tPostStart).count();
    sculpt_log("[GLTF Primitive]   - postInit completed in %.2f ms\n", msPost);

    double msTotal = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStart).count();
    sculpt_log("[GLTF Primitive] Primitive successfully built in %.2f ms\n", msTotal);
    
    return mesh;
}

static void traverseNode(const json& j, int nodeIdx, const glm::mat4& parentMatrix,
                         const std::vector<std::vector<uint8_t>>& binaryBuffers,
                         const std::string& basePath,
                         std::vector<Mesh*>& meshes) {
    if (nodeIdx < 0 || nodeIdx >= (int)j["nodes"].size()) return;
    const auto& node = j["nodes"][nodeIdx];
    
    std::string nodeName = node.value("name", "Node_" + std::to_string(nodeIdx));
    sculpt_log("[GLTF SceneGraph] Traversing node [%d] '%s'...\n", nodeIdx, nodeName.c_str());

    glm::mat4 localMatrix(1.0f);
    if (node.contains("matrix")) {
        std::vector<float> m = node["matrix"].get<std::vector<float>>();
        if (m.size() == 16) {
            std::memcpy(&localMatrix, m.data(), 16 * sizeof(float));
        }
    } else {
        glm::vec3 translation(0.0f);
        if (node.contains("translation")) {
            std::vector<float> t = node["translation"].get<std::vector<float>>();
            if (t.size() == 3) translation = glm::vec3(t[0], t[1], t[2]);
        }
        glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
        if (node.contains("rotation")) {
            std::vector<float> r = node["rotation"].get<std::vector<float>>();
            if (r.size() == 4) rotation = glm::quat(r[3], r[0], r[1], r[2]);
        }
        glm::vec3 scale(1.0f);
        if (node.contains("scale")) {
            std::vector<float> s = node["scale"].get<std::vector<float>>();
            if (s.size() == 3) scale = glm::vec3(s[0], s[1], s[2]);
        }
        localMatrix = glm::translate(glm::mat4(1.0f), translation) *
                      glm::mat4_cast(rotation) *
                      glm::scale(glm::mat4(1.0f), scale);
    }
    
    glm::mat4 worldMatrix = parentMatrix * localMatrix;
    
    if (node.contains("mesh")) {
        int meshIdx = node["mesh"];
        sculpt_log("[GLTF SceneGraph] Node '%s' references glTF mesh index %d\n", nodeName.c_str(), meshIdx);
        if (meshIdx >= 0 && meshIdx < (int)j["meshes"].size()) {
            const auto& gltfMesh = j["meshes"][meshIdx];
            if (gltfMesh.contains("primitives")) {
                size_t primCount = gltfMesh["primitives"].size();
                sculpt_log("[GLTF SceneGraph] glTF mesh index %d has %zu primitive(s)\n", meshIdx, primCount);
                for (size_t pIdx = 0; pIdx < primCount; ++pIdx) {
                    const auto& prim = gltfMesh["primitives"][pIdx];
                    sculpt_log("[GLTF SceneGraph] --- Building Primitive %zu / %zu ---\n", pIdx + 1, primCount);
                    Mesh* meshObj = parsePrimitive(j, prim, binaryBuffers);
                    if (meshObj) {
                        meshObj->setMatrix(worldMatrix);
                        if (gltfMesh.contains("name")) {
                            meshObj->outlinerName = gltfMesh["name"].get<std::string>();
                        } else if (node.contains("name")) {
                            meshObj->outlinerName = node["name"].get<std::string>();
                        }

                        if (prim.contains("material") && !basePath.empty()) {
                            int matIdx = prim["material"].get<int>();
                            sculpt_log("[GLTF Texture] Primitive %zu references material index %d\n", pIdx + 1, matIdx);
                            if (j.contains("materials") && matIdx >= 0 && matIdx < (int)j["materials"].size()) {
                                const auto& mat = j["materials"][matIdx];
                                if (mat.contains("normalTexture")) {
                                    int texIdx = mat["normalTexture"].value("index", -1);
                                    sculpt_log("[GLTF Texture] Material %d references normal texture index %d\n", matIdx, texIdx);
                                    if (texIdx >= 0 && j.contains("textures")
                                        && texIdx < (int)j["textures"].size()) {
                                        int srcIdx = j["textures"][texIdx].value("source", -1);
                                        sculpt_log("[GLTF Texture] Texture %d references image source index %d\n", texIdx, srcIdx);
                                        if (srcIdx >= 0 && j.contains("images")
                                            && srcIdx < (int)j["images"].size()) {
                                            std::string uri = j["images"][srcIdx].value("uri", "");
                                            sculpt_log("[GLTF Texture] Image source %d URI: '%s'\n", srcIdx, uri.c_str());
                                            if (!uri.empty() && uri.rfind("data:", 0) != 0) {
                                                std::string fullPath;
                                                if (basePath.back() == '/' || basePath.back() == '\\') {
                                                    fullPath = basePath + uri;
                                                } else {
                                                    fullPath = basePath + "/" + uri;
                                                }
                                                meshObj->normalMapTexId = loadTextureFromFile(fullPath);
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        meshes.push_back(meshObj);
                        sculpt_log("[GLTF SceneGraph] Added parsed mesh '%s' to scene list (total meshes so far: %zu)\n",
                                  meshObj->outlinerName.c_str(), meshes.size());
                    }
                }
            }
        }
    }
    
    if (node.contains("children")) {
        for (int childIdx : node["children"]) {
            traverseNode(j, childIdx, worldMatrix, binaryBuffers, basePath, meshes);
        }
    }
}

std::vector<Mesh*> importGLTF(const std::string& data, const std::string& basePath) {
    auto tStart = std::chrono::high_resolution_clock::now();
    sculpt_log("[GLTF Import] Beginning importGLTF (data size: %zu bytes, basePath: '%s')\n", data.size(), basePath.c_str());
    
    json j = json::parse(data, nullptr, false);
    if (j.is_discarded()) {
        sculpt_log("[GLTF Import ERROR] Failed to parse JSON document!\n");
        std::cerr << "GLTF JSON parsing failed" << std::endl;
        return {};
    }
    
    double msJson = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStart).count();
    sculpt_log("[GLTF Import] JSON parsed successfully in %.2f ms\n", msJson);

    std::vector<std::vector<uint8_t>> binaryBuffers;
    if (j.contains("buffers")) {
        size_t numBufs = j["buffers"].size();
        sculpt_log("[GLTF Import] Reading %zu buffer entry/entries...\n", numBufs);
        for (size_t bIdx = 0; bIdx < numBufs; ++bIdx) {
            const auto& buf = j["buffers"][bIdx];
            std::string uri = buf.value("uri", "");
            if (uri.rfind("data:", 0) == 0) {
                sculpt_log("[GLTF Import] Buffer [%zu]: decoding Base64 data URI...\n", bIdx);
                binaryBuffers.push_back(Base64::decodeUri(uri));
            } else if (!uri.empty() && !basePath.empty()) {
                sculpt_log("[GLTF Import] Buffer [%zu]: reading external file URI '%s'...\n", bIdx, uri.c_str());
                binaryBuffers.push_back(readExternalFile(basePath, uri));
            } else {
                sculpt_log("[GLTF Import WARNING] Buffer [%zu]: empty URI or missing basePath\n", bIdx);
                binaryBuffers.push_back({});
            }
        }
    }
    
    if (!j.contains("scenes") || j["scenes"].empty()) {
        sculpt_log("[GLTF Import ERROR] No 'scenes' array found in glTF JSON\n");
        return {};
    }
    int sceneIdx = j.value("scene", 0);
    if (sceneIdx < 0 || sceneIdx >= (int)j["scenes"].size()) {
        sculpt_log("[GLTF Import ERROR] Invalid scene index: %d\n", sceneIdx);
        return {};
    }
    const auto& scene = j["scenes"][sceneIdx];
    if (!scene.contains("nodes")) {
        sculpt_log("[GLTF Import ERROR] Selected scene %d has no 'nodes' array\n", sceneIdx);
        return {};
    }
    
    std::vector<Mesh*> meshes;
    glm::mat4 identity(1.0f);
    for (int nodeIdx : scene["nodes"]) {
        traverseNode(j, nodeIdx, identity, binaryBuffers, basePath, meshes);
    }

    double msTotal = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStart).count();
    sculpt_log("[GLTF Import] importGLTF completed successfully: %zu mesh(es) in %.2f ms\n", meshes.size(), msTotal);
    return meshes;
}

std::vector<Mesh*> importGLB(const std::vector<uint8_t>& buffer) {
    auto tStart = std::chrono::high_resolution_clock::now();
    sculpt_log("[GLTF Import] Beginning importGLB (buffer size: %zu bytes)\n", buffer.size());

    if (buffer.size() < 20) {
        sculpt_log("[GLTF Import ERROR] GLB buffer too small (%zu bytes)\n", buffer.size());
        return {};
    }
    
    uint32_t magic = *reinterpret_cast<const uint32_t*>(buffer.data());
    uint32_t version = *reinterpret_cast<const uint32_t*>(buffer.data() + 4);
    uint32_t totalLength = *reinterpret_cast<const uint32_t*>(buffer.data() + 8);
    
    sculpt_log("[GLTF Import] GLB header: magic=0x%X, version=%u, length=%u\n", magic, version, totalLength);

    if (magic != 0x46546C67) {
        sculpt_log("[GLTF Import ERROR] Invalid GLB magic 0x%X (expected 0x46546C67)\n", magic);
        std::cerr << "Invalid GLB magic" << std::endl;
        return {};
    }
    if (version != 2) {
        sculpt_log("[GLTF Import ERROR] Unsupported GLB version %u (expected 2)\n", version);
        std::cerr << "Only GLTF 2.0 is supported" << std::endl;
        return {};
    }
    
    std::string jsonStr;
    std::vector<uint8_t> binBuffer;
    
    size_t offset = 12;
    while (offset < totalLength && offset < buffer.size()) {
        if (offset + 8 > buffer.size()) break;
        uint32_t chunkLength = *reinterpret_cast<const uint32_t*>(buffer.data() + offset);
        uint32_t chunkType = *reinterpret_cast<const uint32_t*>(buffer.data() + offset + 4);
        offset += 8;
        
        if (offset + chunkLength > buffer.size()) break;
        
        if (chunkType == 0x4E4F534A) { // JSON
            jsonStr = std::string(reinterpret_cast<const char*>(buffer.data() + offset), chunkLength);
            sculpt_log("[GLTF Import] Extracted GLB JSON chunk (%u bytes)\n", chunkLength);
        } else if (chunkType == 0x004E4942) { // BIN
            binBuffer = std::vector<uint8_t>(buffer.begin() + offset, buffer.begin() + offset + chunkLength);
            sculpt_log("[GLTF Import] Extracted GLB BIN chunk (%u bytes, %.2f MB)\n",
                      chunkLength, (double)chunkLength / (1024.0 * 1024.0));
        }
        offset += chunkLength;
    }
    
    if (jsonStr.empty()) {
        sculpt_log("[GLTF Import ERROR] GLB missing JSON chunk!\n");
        return {};
    }
    
    json j = json::parse(jsonStr, nullptr, false);
    if (j.is_discarded()) {
        sculpt_log("[GLTF Import ERROR] Failed parsing GLB JSON chunk!\n");
        return {};
    }
    
    std::vector<std::vector<uint8_t>> binaryBuffers;
    if (!binBuffer.empty()) {
        binaryBuffers.push_back(binBuffer);
    }
    
    if (j.contains("buffers")) {
        for (size_t i = binaryBuffers.size(); i < j["buffers"].size(); ++i) {
            std::string uri = j["buffers"][i].value("uri", "");
            if (uri.rfind("data:", 0) == 0) {
                sculpt_log("[GLTF Import] Buffer [%zu]: decoding Base64 data URI...\n", i);
                binaryBuffers.push_back(Base64::decodeUri(uri));
            } else {
                sculpt_log("[GLTF Import WARNING] Buffer [%zu]: no embedded binary for URI '%s'\n", i, uri.c_str());
                binaryBuffers.push_back({});
            }
        }
    }
    
    if (!j.contains("scenes") || j["scenes"].empty()) return {};
    int sceneIdx = j.value("scene", 0);
    if (sceneIdx < 0 || sceneIdx >= (int)j["scenes"].size()) return {};
    const auto& scene = j["scenes"][sceneIdx];
    if (!scene.contains("nodes")) return {};
    
    std::vector<Mesh*> meshes;
    glm::mat4 identity(1.0f);
    for (int nodeIdx : scene["nodes"]) {
        traverseNode(j, nodeIdx, identity, binaryBuffers, "", meshes);
    }

    double msTotal = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStart).count();
    sculpt_log("[GLTF Import] importGLB completed successfully: %zu mesh(es) in %.2f ms\n", meshes.size(), msTotal);
    return meshes;
}

} // namespace ImportGLTF


