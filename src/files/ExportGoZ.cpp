#include "files/ExportGoZ.h"
#include "common/Constants.h"
#include "common/StringUtils.h"
#include "common/Logger.h"

#include <cstring>
#include <cmath>
#include <array>
#include <algorithm>
#include <fstream>
#include <chrono>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace ExportGoZ {

namespace {

constexpr uint32_t TAG_END              = 0;
constexpr uint32_t TAG_OBJECT_NAME      = 1;
constexpr uint32_t TAG_MESH_NAME        = 0x1389; // 5001
constexpr uint32_t TAG_SUBDIVISION      = 0x138A; // 5002
constexpr uint32_t TAG_VERTICES         = 0x2711; // 10001
constexpr uint32_t TAG_FACES            = 0x4E21; // 20001
constexpr uint32_t TAG_UVS              = 0x61A9; // 25001
constexpr uint32_t TAG_MASK             = 0x7532; // 30002
constexpr uint32_t TAG_PAINT            = 0x88B9; // 35001
constexpr uint32_t TAG_POLYGROUPS       = 0x9C41; // 40001
constexpr uint32_t NO_VERTEX            = 0xFFFFFFFFu;

inline void writeU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

inline void writeU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

inline void writeU64(uint8_t* p, uint64_t v) {
    writeU32(p, static_cast<uint32_t>(v & 0xFFFFFFFFULL));
    writeU32(p + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFULL));
}

inline void writeChunkHeader(uint8_t* p, uint32_t tag, uint32_t chunkSize, uint64_t count) {
    writeU32(p + 0, tag);
    writeU32(p + 4, chunkSize);
    writeU64(p + 8, count);
}

// 4096-entry lookup table for ultra-fast linear-to-sRGB conversion (eliminates std::pow)
static const auto linToSrgbLut = []() {
    std::array<uint8_t, 4096> lut{};
    for (int i = 0; i < 4096; ++i) {
        float c = static_cast<float>(i) / 4095.0f;
        float s = (c <= 0.0031308f) ? (c * 12.92f) : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
        int val = static_cast<int>(s * 255.0f + 0.5f);
        lut[i] = static_cast<uint8_t>(std::clamp(val, 0, 255));
    }
    return lut;
}();

inline uint8_t fastLinearToSrgb(float c) {
    if (c <= 0.0f) return 0;
    if (c >= 1.0f) return 255;
    int idx = static_cast<int>(c * 4095.0f + 0.5f);
    return linToSrgbLut[idx];
}

std::string sanitizeMeshName(const std::string& rawName) {
    std::string clean;
    clean.reserve(rawName.size());
    for (char c : rawName) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
            clean.push_back(c);
        } else if (c == ' ' || c == '-' || c == '.') {
            clean.push_back('_');
        }
    }
    if (clean.empty()) clean = "Mesh";
    return clean;
}

} // anonymous namespace

std::vector<uint8_t> exportGoZ(const Mesh* mesh) {
    if (!mesh || mesh->nbVerts <= 0 || mesh->nbFaces <= 0) {
        sculpt_log("[GoZ Export ERROR] Mesh is null or has no geometry\n");
        return {};
    }

    auto tStart = std::chrono::high_resolution_clock::now();

    const size_t nbVerts = static_cast<size_t>(mesh->nbVerts);
    const size_t nbFaces = static_cast<size_t>(mesh->nbFaces);

    // Prepare Object Name chunk (padded to 16-byte boundary)
    std::string meshName = sanitizeMeshName(mesh->outlinerName);
    std::string objName = "GoZMesh_" + meshName;
    size_t namePayloadLen = std::max<size_t>(16, ((objName.length() + 15) / 16) * 16);
    uint32_t objChunkSize = static_cast<uint32_t>(16 + namePayloadLen);

    // Chunk sizes
    uint32_t tagMeshNameChunkSize = 20; // 16 header + 4 payload
    uint32_t tagSubdivChunkSize = 32;   // 16 header + 16 payload
    uint32_t vertsPayloadLen = static_cast<uint32_t>(nbVerts * 3 * sizeof(float));
    uint32_t vertsChunkSize = 16 + vertsPayloadLen;
    uint32_t facesPayloadLen = static_cast<uint32_t>(nbFaces * 16); // 4 * uint32_t
    uint32_t facesChunkSize = 16 + facesPayloadLen;
    uint32_t paintPayloadLen = static_cast<uint32_t>(nbVerts * 4);  // B, G, R, 0
    uint32_t paintChunkSize = 16 + paintPayloadLen;
    uint32_t maskPayloadLen = static_cast<uint32_t>(nbVerts * 2);   // uint16_t
    uint32_t maskChunkSize = 16 + maskPayloadLen;
    uint32_t groupsPayloadLen = static_cast<uint32_t>(nbFaces * 2); // uint16_t
    uint32_t groupsChunkSize = 16 + groupsPayloadLen;
    uint32_t endChunkSize = 16;         // 16 header (tag=0, size=0, count=0)

    // Calculate total buffer size upfront
    size_t totalBytes = 32 // Magic 32-byte header
                      + objChunkSize
                      + tagMeshNameChunkSize
                      + tagSubdivChunkSize
                      + vertsChunkSize
                      + facesChunkSize
                      + paintChunkSize
                      + maskChunkSize
                      + groupsChunkSize
                      + endChunkSize;

    std::vector<uint8_t> buffer(totalBytes);
    uint8_t* p = buffer.data();

    // 1. Magic Header (32 bytes)
    // "GoZb 1.0 ZBrush GoZ Binary......"
    static const char GOZ_MAGIC[32] = {
        'G', 'o', 'Z', 'b', ' ', '1', '.', '0', ' ', 'Z', 'B', 'r', 'u', 's', 'h', ' ',
        'G', 'o', 'Z', ' ', 'B', 'i', 'n', 'a', 'r', 'y', '.', '.', '.', '.', '.', '.'
    };
    std::memcpy(p, GOZ_MAGIC, 32);
    p += 32;

    // 2. TAG_OBJECT_NAME (1)
    writeChunkHeader(p, TAG_OBJECT_NAME, objChunkSize, 1);
    p += 16;
    std::memset(p, 0, namePayloadLen);
    std::memcpy(p, objName.data(), std::min(objName.length(), namePayloadLen));
    p += namePayloadLen;

    // 3. TAG_MESH_NAME (0x1389 = 5001)
    writeChunkHeader(p, TAG_MESH_NAME, tagMeshNameChunkSize, 1);
    p += 16;
    writeU32(p, 0x0806); // Standard GoZ mesh flag (6, 8, 0, 0)
    p += 4;

    // 4. TAG_SUBDIVISION (0x138A = 5002)
    writeChunkHeader(p, TAG_SUBDIVISION, tagSubdivChunkSize, 1);
    p += 16;
    std::memset(p, 0, 16);
    p += 16;

    // 5. TAG_VERTICES (0x2711 = 10001)
    writeChunkHeader(p, TAG_VERTICES, vertsChunkSize, nbVerts);
    p += 16;
    bool hasTransform = (mesh->matrix != glm::mat4(1.0f));
    if (hasTransform) {
        float* outVerts = reinterpret_cast<float*>(p);
        const float* inVerts = mesh->verts.data();
        const glm::mat4& mat = mesh->matrix;
        #pragma omp parallel for schedule(static) if(nbVerts > 5000)
        for (size_t i = 0; i < nbVerts; ++i) {
            glm::vec4 wp = mat * glm::vec4(inVerts[i * 3 + 0], inVerts[i * 3 + 1], inVerts[i * 3 + 2], 1.0f);
            outVerts[i * 3 + 0] = wp.x;
            outVerts[i * 3 + 1] = wp.y;
            outVerts[i * 3 + 2] = wp.z;
        }
    } else {
        std::memcpy(p, mesh->verts.data(), vertsPayloadLen);
    }
    p += vertsPayloadLen;

    // 6. TAG_FACES (0x4E21 = 20001)
    writeChunkHeader(p, TAG_FACES, facesChunkSize, nbFaces);
    p += 16;
    // In SPSculpt, faces are stored as nbFaces * 4 uint32_t:
    // Quads: (v0, v1, v2, v3), Triangles: (v0, v1, v2, TRI_INDEX) where TRI_INDEX == 0xFFFFFFFFu
    // This matches GoZ RawFace layout exactly.
    std::memcpy(p, mesh->faces.data(), facesPayloadLen);
    p += facesPayloadLen;

    // 7. TAG_PAINT (0x88B9 = 35001)
    writeChunkHeader(p, TAG_PAINT, paintChunkSize, nbVerts);
    p += 16;
    if (mesh->colors.size() == nbVerts * 3) {
        const float* inColors = mesh->colors.data();
        #pragma omp parallel for schedule(static) if(nbVerts > 5000)
        for (size_t i = 0; i < nbVerts; ++i) {
            uint8_t r = fastLinearToSrgb(inColors[i * 3 + 0]);
            uint8_t g = fastLinearToSrgb(inColors[i * 3 + 1]);
            uint8_t b = fastLinearToSrgb(inColors[i * 3 + 2]);
            p[i * 4 + 0] = b;
            p[i * 4 + 1] = g;
            p[i * 4 + 2] = r;
            p[i * 4 + 3] = 0;
        }
    } else {
        // Default white polypaint
        #pragma omp parallel for schedule(static) if(nbVerts > 5000)
        for (size_t i = 0; i < nbVerts; ++i) {
            p[i * 4 + 0] = 255;
            p[i * 4 + 1] = 255;
            p[i * 4 + 2] = 255;
            p[i * 4 + 3] = 0;
        }
    }
    p += paintPayloadLen;

    // 8. TAG_MASK (0x7532 = 30002)
    writeChunkHeader(p, TAG_MASK, maskChunkSize, nbVerts);
    p += 16;
    if (mesh->materials.size() == nbVerts * 3) {
        const float* inMat = mesh->materials.data();
        #pragma omp parallel for schedule(static) if(nbVerts > 5000)
        for (size_t i = 0; i < nbVerts; ++i) {
            // In SPSculpt materials: channel 2 is mask (1.0 = unmasked, 0.0 = fully masked)
            // In GoZ mask: 65535 = unmasked, 0 = fully masked
            float mVal = inMat[i * 3 + 2];
            int mInt = static_cast<int>(mVal * 65535.0f + 0.5f);
            uint16_t mU16 = static_cast<uint16_t>(std::clamp(mInt, 0, 65535));
            writeU16(p + i * 2, mU16);
        }
    } else {
        // Default unmasked (65535)
        #pragma omp parallel for schedule(static) if(nbVerts > 5000)
        for (size_t i = 0; i < nbVerts; ++i) {
            writeU16(p + i * 2, 65535);
        }
    }
    p += maskPayloadLen;

    // 9. TAG_POLYGROUPS (0x9C41 = 40001)
    writeChunkHeader(p, TAG_POLYGROUPS, groupsChunkSize, nbFaces);
    p += 16;
    if (mesh->faceGroups.size() == nbFaces) {
        const uint32_t* inGroups = mesh->faceGroups.data();
        #pragma omp parallel for schedule(static) if(nbFaces > 5000)
        for (size_t i = 0; i < nbFaces; ++i) {
            uint16_t gid16 = static_cast<uint16_t>(inGroups[i] & 0xFFFF);
            writeU16(p + i * 2, gid16);
        }
    } else {
        std::memset(p, 0, groupsPayloadLen);
    }
    p += groupsPayloadLen;

    // 10. TAG_END (0)
    writeChunkHeader(p, TAG_END, 0, 0);
    p += 16;

    auto tEnd = std::chrono::high_resolution_clock::now();
    double msExport = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    sculpt_log("[GoZ Export] Serialized '%s' (%zu verts, %zu faces) to %zu bytes in %.2f ms\n",
               meshName.c_str(), nbVerts, nbFaces, buffer.size(), msExport);

    return buffer;
}

bool exportGoZ(const std::string& path, const Mesh* mesh) {
    if (!mesh) return false;
    std::vector<uint8_t> buffer = exportGoZ(mesh);
    if (buffer.empty()) return false;

#ifdef _WIN32
    std::ofstream file(utf8ToWide(path).c_str(), std::ios::binary);
#else
    std::ofstream file(path, std::ios::binary);
#endif
    if (!file.is_open()) {
        sculpt_log("[GoZ Export ERROR] Failed to open '%s' for writing\n", path.c_str());
        return false;
    }

    file.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    return file.good();
}

bool exportGoZ(const std::string& path, const std::vector<Mesh*>& meshes) {
    if (meshes.empty()) return false;
    // Export first non-null mesh
    for (const Mesh* m : meshes) {
        if (m) {
            return exportGoZ(path, m);
        }
    }
    return false;
}

} // namespace ExportGoZ

