#include "files/ImportGLTF.h"
#include "files/Base64.h"
#include "common/Constants.h"
#include "mesh/Topology.h"
#include "common/StringUtils.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <iostream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

using json = nlohmann::json;

namespace ImportGLTF {

static std::vector<uint8_t> readExternalFile(const std::string& basePath,
                                              const std::string& uri) {
    std::string fullPath;
    if (basePath.empty()) {
        fullPath = uri;
    } else if (basePath.back() == '/' || basePath.back() == '\\') {
        fullPath = basePath + uri;
    } else {
        fullPath = basePath + "/" + uri;
    }

#ifdef _WIN32
    std::ifstream file(utf8ToWide(fullPath).c_str(), std::ios::binary | std::ios::ate);
#else
    std::ifstream file(fullPath, std::ios::binary | std::ios::ate);
#endif
    if (!file.is_open()) return {};
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    if (file.read(reinterpret_cast<char*>(buffer.data()), size))
        return buffer;
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
    if (accessorIdx < 0 || accessorIdx >= (int)j["accessors"].size()) return {};
    const auto& accessor = j["accessors"][accessorIdx];
    
    int bvIdx = accessor.value("bufferView", -1);
    if (bvIdx < 0 || bvIdx >= (int)j["bufferViews"].size()) return {};
    const auto& bv = j["bufferViews"][bvIdx];
    
    int bufIdx = bv.value("buffer", -1);
    if (bufIdx < 0 || bufIdx >= (int)binaryBuffers.size()) return {};
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
    if (accessorIdx < 0 || accessorIdx >= (int)j["accessors"].size()) return {};
    const auto& accessor = j["accessors"][accessorIdx];
    
    int bvIdx = accessor.value("bufferView", -1);
    if (bvIdx < 0 || bvIdx >= (int)j["bufferViews"].size()) return {};
    const auto& bv = j["bufferViews"][bvIdx];
    
    int bufIdx = bv.value("buffer", -1);
    if (bufIdx < 0 || bufIdx >= (int)binaryBuffers.size()) return {};
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
    if (!primitive.contains("attributes")) return nullptr;
    const auto& attrs = primitive["attributes"];
    if (!attrs.contains("POSITION")) return nullptr;
    
    int posAccessor = attrs["POSITION"];
    std::vector<float> vAr = getAccessorFloatArray(j, posAccessor, binaryBuffers);
    if (vAr.empty()) return nullptr;
    
    Mesh* mesh = new Mesh();
    mesh->verts = vAr;
    mesh->nbVerts = vAr.size() / 3;
    
    if (attrs.contains("NORMAL")) {
        mesh->normals = getAccessorFloatArray(j, attrs["NORMAL"], binaryBuffers);
    }
    
    if (attrs.contains("COLOR_0")) {
        std::vector<float> cAr = getAccessorFloatArray(j, attrs["COLOR_0"], binaryBuffers);
        if (!cAr.empty()) {
            mesh->colors = cAr;
        }
    }
    if (mesh->colors.size() != mesh->nbVerts * 3) {
        mesh->colors.assign(mesh->nbVerts * 3, 1.0f);
    }
    
    mesh->materials.resize(mesh->nbVerts * 3);
    for (int k = 0; k < mesh->nbVerts; ++k) {
        mesh->materials[k * 3]     = 0.5f; // roughness
        mesh->materials[k * 3 + 1] = 0.0f; // metalness
        mesh->materials[k * 3 + 2] = 1.0f; // mask
    }
    
    std::vector<float> uvAr;
    if (attrs.contains("TEXCOORD_0")) {
        uvAr = getAccessorFloatArray(j, attrs["TEXCOORD_0"], binaryBuffers);
    }
    
    std::vector<uint32_t> iAr;
    if (primitive.contains("indices")) {
        iAr = getAccessorUintArray(j, primitive["indices"], binaryBuffers);
    } else {
        iAr.resize(mesh->nbVerts);
        for (int i = 0; i < mesh->nbVerts; ++i) {
            iAr[i] = i;
        }
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
    mesh->nbFaces = numTris;
    
    if (!uvAr.empty()) {
        mesh->initTexCoordsDataFromOBJData(uvAr, mesh->faces);
    }
    
    // Compute topology
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
    
    mesh->postInit();
    
    return mesh;
}

static void traverseNode(const json& j, int nodeIdx, const glm::mat4& parentMatrix,
                         const std::vector<std::vector<uint8_t>>& binaryBuffers,
                         std::vector<Mesh*>& meshes) {
    if (nodeIdx < 0 || nodeIdx >= (int)j["nodes"].size()) return;
    const auto& node = j["nodes"][nodeIdx];
    
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
        if (meshIdx >= 0 && meshIdx < (int)j["meshes"].size()) {
            const auto& gltfMesh = j["meshes"][meshIdx];
            if (gltfMesh.contains("primitives")) {
                for (const auto& prim : gltfMesh["primitives"]) {
                    Mesh* meshObj = parsePrimitive(j, prim, binaryBuffers);
                    if (meshObj) {
                        meshObj->setMatrix(worldMatrix);
                        if (gltfMesh.contains("name")) {
                            meshObj->outlinerName = gltfMesh["name"].get<std::string>();
                        } else if (node.contains("name")) {
                            meshObj->outlinerName = node["name"].get<std::string>();
                        }
                        meshes.push_back(meshObj);
                    }
                }
            }
        }
    }
    
    if (node.contains("children")) {
        for (int childIdx : node["children"]) {
            traverseNode(j, childIdx, worldMatrix, binaryBuffers, meshes);
        }
    }
}

std::vector<Mesh*> importGLTF(const std::string& data, const std::string& basePath) {
    json j = json::parse(data, nullptr, false);
    if (j.is_discarded()) {
        std::cerr << "GLTF JSON parsing failed" << std::endl;
        return {};
    }
    
    std::vector<std::vector<uint8_t>> binaryBuffers;
    if (j.contains("buffers")) {
        for (const auto& buf : j["buffers"]) {
            std::string uri = buf.value("uri", "");
            if (uri.rfind("data:", 0) == 0) {
                binaryBuffers.push_back(Base64::decodeUri(uri));
            } else if (!uri.empty() && !basePath.empty()) {
                binaryBuffers.push_back(readExternalFile(basePath, uri));
            } else {
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
        traverseNode(j, nodeIdx, identity, binaryBuffers, meshes);
    }
    return meshes;
}

std::vector<Mesh*> importGLB(const std::vector<uint8_t>& buffer) {
    if (buffer.size() < 20) return {};
    
    uint32_t magic = *reinterpret_cast<const uint32_t*>(buffer.data());
    uint32_t version = *reinterpret_cast<const uint32_t*>(buffer.data() + 4);
    uint32_t totalLength = *reinterpret_cast<const uint32_t*>(buffer.data() + 8);
    
    if (magic != 0x46546C67) {
        std::cerr << "Invalid GLB magic" << std::endl;
        return {};
    }
    if (version != 2) {
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
        
        if (chunkType == 0x4E4F534A) {
            jsonStr = std::string(reinterpret_cast<const char*>(buffer.data() + offset), chunkLength);
        } else if (chunkType == 0x004E4942) {
            binBuffer = std::vector<uint8_t>(buffer.begin() + offset, buffer.begin() + offset + chunkLength);
        }
        offset += chunkLength;
    }
    
    if (jsonStr.empty()) return {};
    
    json j = json::parse(jsonStr, nullptr, false);
    if (j.is_discarded()) return {};
    
    std::vector<std::vector<uint8_t>> binaryBuffers;
    if (!binBuffer.empty()) {
        binaryBuffers.push_back(binBuffer);
    }
    
    if (j.contains("buffers")) {
        for (size_t i = binaryBuffers.size(); i < j["buffers"].size(); ++i) {
            std::string uri = j["buffers"][i].value("uri", "");
            if (uri.rfind("data:", 0) == 0) {
                binaryBuffers.push_back(Base64::decodeUri(uri));
            } else {
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
        traverseNode(j, nodeIdx, identity, binaryBuffers, meshes);
    }
    return meshes;
}

} // namespace ImportGLTF
