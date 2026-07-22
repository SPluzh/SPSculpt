#include "files/ExportGLTF.h"
#include "files/MeshUtils.h"
#include "common/Constants.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <limits>
#include <cstring>
#include <algorithm>
#include <glm/gtc/type_ptr.hpp>

using json = nlohmann::json;

namespace ExportGLTF {

static std::vector<uint8_t> padBytes(const std::vector<uint8_t>& bytes) {
    size_t rem = bytes.size() % 4;
    if (rem == 0) return bytes;
    size_t pad = 4 - rem;
    std::vector<uint8_t> res = bytes;
    res.insert(res.end(), pad, 0);
    return res;
}

static std::pair<std::vector<float>, std::vector<float>> getMinMax(const float* data, size_t count, size_t itemSize) {
    if (count == 0) {
        return {std::vector<float>(itemSize, 0.0f), std::vector<float>(itemSize, 0.0f)};
    }
    std::vector<float> min(itemSize, std::numeric_limits<float>::infinity());
    std::vector<float> max(itemSize, -std::numeric_limits<float>::infinity());
    for (size_t j = 0; j < count; j += itemSize) {
        for (size_t k = 0; k < itemSize; ++k) {
            float v = data[j + k];
            if (v < min[k]) min[k] = v;
            if (v > max[k]) max[k] = v;
        }
    }
    return {min, max};
}

std::vector<uint8_t> exportGLB(const std::vector<Mesh*>& meshes) {
    std::vector<std::vector<uint8_t>> binData;
    size_t byteOffset = 0;

    json j;
    j["asset"] = { {"version", "2.0"}, {"generator", "SculptSP GLB Exporter"} };
    j["scenes"] = json::array({ { {"nodes", json::array()} } });
    j["scene"] = 0;
    j["nodes"] = json::array();
    j["meshes"] = json::array();
    j["accessors"] = json::array();
    j["bufferViews"] = json::array();
    j["buffers"] = json::array();

    auto addBufferView = [&](const uint8_t* rawData, size_t size, int target) -> int {
        std::vector<uint8_t> raw(rawData, rawData + size);
        std::vector<uint8_t> padded = padBytes(raw);
        
        json bv;
        bv["buffer"] = 0;
        bv["byteOffset"] = byteOffset;
        bv["byteLength"] = raw.size();
        if (target != 0) {
            bv["target"] = target;
        }
        
        j["bufferViews"].push_back(bv);
        binData.push_back(padded);
        byteOffset += padded.size();
        
        return j["bufferViews"].size() - 1;
    };

    int nodeCount = 0;
    for (const auto* mesh : meshes) {
        if (!mesh) continue;

        // Triangulate quads to get triangle indices for GLTF
        std::vector<uint32_t> iAr = MeshUtils::triangulate(*mesh);
        int nbVerts = mesh->nbVerts;
        int nbTris = iAr.size() / 3;

        if (nbVerts == 0 || nbTris == 0) continue;

        // Add node index to scene nodes
        j["scenes"][0]["nodes"].push_back(nodeCount++);
        
        json nodeObj;
        nodeObj["mesh"] = j["meshes"].size();
        
        // Matrix
        std::vector<float> m(16);
        std::memcpy(m.data(), glm::value_ptr(mesh->matrix), 16 * sizeof(float));
        nodeObj["matrix"] = m;
        j["nodes"].push_back(nodeObj);

        // POSITION
        auto posMinMax = getMinMax(mesh->verts.data(), mesh->verts.size(), 3);
        int posBv = addBufferView(reinterpret_cast<const uint8_t*>(mesh->verts.data()), mesh->verts.size() * 4, 34962);
        int posAccessor = j["accessors"].size();
        j["accessors"].push_back({
            {"bufferView", posBv},
            {"componentType", 5126}, // FLOAT
            {"count", nbVerts},
            {"type", "VEC3"},
            {"min", posMinMax.first},
            {"max", posMinMax.second}
        });

        // NORMAL
        int normAccessor = -1;
        if (!mesh->normals.empty()) {
            auto normMinMax = getMinMax(mesh->normals.data(), mesh->normals.size(), 3);
            int normBv = addBufferView(reinterpret_cast<const uint8_t*>(mesh->normals.data()), mesh->normals.size() * 4, 34962);
            normAccessor = j["accessors"].size();
            j["accessors"].push_back({
                {"bufferView", normBv},
                {"componentType", 5126},
                {"count", nbVerts},
                {"type", "VEC3"},
                {"min", normMinMax.first},
                {"max", normMinMax.second}
            });
        }

        // COLOR_0
        int colAccessor = -1;
        if (!mesh->colors.empty()) {
            auto colMinMax = getMinMax(mesh->colors.data(), mesh->colors.size(), 3);
            int colBv = addBufferView(reinterpret_cast<const uint8_t*>(mesh->colors.data()), mesh->colors.size() * 4, 34962);
            colAccessor = j["accessors"].size();
            j["accessors"].push_back({
                {"bufferView", colBv},
                {"componentType", 5126},
                {"count", nbVerts},
                {"type", "VEC3"},
                {"min", colMinMax.first},
                {"max", colMinMax.second}
            });
        }

        // TEXCOORD_0 (UVs)
        int uvAccessor = -1;
        bool hasUV = mesh->hasUV && !mesh->texCoords.empty();
        if (hasUV) {
            auto uvMinMax = getMinMax(mesh->texCoords.data(), mesh->texCoords.size(), 2);
            int uvBv = addBufferView(reinterpret_cast<const uint8_t*>(mesh->texCoords.data()), mesh->texCoords.size() * 4, 34962);
            uvAccessor = j["accessors"].size();
            j["accessors"].push_back({
                {"bufferView", uvBv},
                {"componentType", 5126},
                {"count", mesh->texCoords.size() / 2},
                {"type", "VEC2"},
                {"min", uvMinMax.first},
                {"max", uvMinMax.second}
            });
        }

        // INDICES
        int idxType = (nbVerts >= 65536) ? 5125 : 5123; // UNSIGNED_INT or UNSIGNED_SHORT
        int idxAccessor = j["accessors"].size();
        
        if (idxType == 5123) {
            std::vector<uint16_t> idx16(iAr.begin(), iAr.end());
            int idxBv = addBufferView(reinterpret_cast<const uint8_t*>(idx16.data()), idx16.size() * 2, 34963);
            j["accessors"].push_back({
                {"bufferView", idxBv},
                {"componentType", 5123},
                {"count", iAr.size()},
                {"type", "SCALAR"}
            });
        } else {
            int idxBv = addBufferView(reinterpret_cast<const uint8_t*>(iAr.data()), iAr.size() * 4, 34963);
            j["accessors"].push_back({
                {"bufferView", idxBv},
                {"componentType", 5125},
                {"count", iAr.size()},
                {"type", "SCALAR"}
            });
        }

        json attributes;
        attributes["POSITION"] = posAccessor;
        if (normAccessor != -1) attributes["NORMAL"] = normAccessor;
        if (colAccessor != -1) attributes["COLOR_0"] = colAccessor;
        if (uvAccessor != -1) attributes["TEXCOORD_0"] = uvAccessor;

        json prim;
        prim["attributes"] = attributes;
        prim["indices"] = idxAccessor;

        json meshObj;
        meshObj["name"] = "Mesh_" + std::to_string(j["meshes"].size());
        meshObj["primitives"] = json::array({ prim });

        j["meshes"].push_back(meshObj);
    }

    j["buffers"].push_back({ {"byteLength", byteOffset} });

    std::string jsonStr = j.dump();
    size_t jsonPad = (4 - (jsonStr.length() % 4)) % 4;
    jsonStr.append(jsonPad, ' ');

    size_t totalBinLength = 0;
    for (const auto& data : binData) {
        totalBinLength += data.size();
    }

    size_t glbLength = 12 + 8 + jsonStr.length() + 8 + totalBinLength;
    std::vector<uint8_t> glb(glbLength);

    // 1. Header
    uint32_t magic = 0x46546C67; // 'glTF'
    uint32_t version = 2;
    uint32_t lengthVal = static_cast<uint32_t>(glbLength);
    std::memcpy(glb.data(), &magic, 4);
    std::memcpy(glb.data() + 4, &version, 4);
    std::memcpy(glb.data() + 8, &lengthVal, 4);

    // 2. JSON Chunk Header
    uint32_t jsonChunkLen = static_cast<uint32_t>(jsonStr.length());
    uint32_t jsonChunkType = 0x4E4F534A; // 'JSON'
    std::memcpy(glb.data() + 12, &jsonChunkLen, 4);
    std::memcpy(glb.data() + 16, &jsonChunkType, 4);

    // 3. JSON Chunk Data
    std::memcpy(glb.data() + 20, jsonStr.data(), jsonStr.length());

    // 4. BIN Chunk Header
    size_t binChunkOffset = 20 + jsonStr.length();
    uint32_t binChunkLen = static_cast<uint32_t>(totalBinLength);
    uint32_t binChunkType = 0x004E4942; // 'BIN'
    std::memcpy(glb.data() + binChunkOffset, &binChunkLen, 4);
    std::memcpy(glb.data() + binChunkOffset + 4, &binChunkType, 4);

    // 5. BIN Chunk Data
    size_t offset = binChunkOffset + 8;
    for (const auto& chunk : binData) {
        std::memcpy(glb.data() + offset, chunk.data(), chunk.size());
        offset += chunk.size();
    }

    return glb;
}

} // namespace ExportGLTF
