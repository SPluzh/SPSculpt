#include "files/ImportGoZ.h"
#include "common/Constants.h"
#include "common/Logger.h"
#include "mesh/Topology.h"

#include <cstring>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <array>
#include <iostream>
#ifdef _OPENMP
#include <omp.h>
#endif

namespace ImportGoZ {

namespace {

constexpr uint32_t TAG_END              = 0;
constexpr uint32_t TAG_OBJECT_NAME      = 1;
constexpr uint32_t TAG_MESH_NAME        = 0x1389; // 5001
constexpr uint32_t TAG_SUBDIVISION      = 0x138a; // 5002
constexpr uint32_t TAG_VERTICES         = 0x2711; // 10001
constexpr uint32_t TAG_FACES            = 0x4e21; // 20001
constexpr uint32_t TAG_UVS              = 0x61a9; // 25001
constexpr uint32_t TAG_MASK             = 0x7532; // 30002
constexpr uint32_t TAG_PAINT            = 0x88b9; // 35001
constexpr uint32_t TAG_POLYGROUPS       = 0x9c41; // 40001
constexpr uint32_t TAG_DIFFUSE_MAP      = 0xafc9; // 45001
constexpr uint32_t TAG_NORMAL_MAP       = 0xc351; // 50001
constexpr uint32_t TAG_DISPLACEMENT_MAP = 0xd6d9; // 55001
constexpr uint32_t NO_VERTEX            = 0xFFFFFFFFu;

inline uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t readU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t readU64(const uint8_t* p) {
    return static_cast<uint64_t>(readU32(p)) |
           (static_cast<uint64_t>(readU32(p + 4)) << 32);
}

inline float readF32(const uint8_t* p) {
    uint32_t bits = readU32(p);
    float f;
    std::memcpy(&f, &bits, sizeof(float));
    return f;
}

// ZBrush 3D coordinates: +Y is Up, +X is Right.
// We preserve native coordinates (x, y, z) so the model is upright and not mirrored.
inline glm::vec3 fromZBrush(float x, float y, float z) {
    return glm::vec3(x, y, z);
}

// Fast sRGB to linear conversion LUT for polypaint colors (eliminates millions of expensive std::pow calls)
static const auto srgbLut = []() {
    std::array<float, 256> lut{};
    for (int i = 0; i < 256; ++i) {
        float c = static_cast<float>(i) / 255.0f;
        lut[i] = (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    return lut;
}();

struct RawFace {
    uint32_t v[4];
    bool isQuad() const { return v[3] != NO_VERTEX; }
};

} // anonymous namespace

std::vector<Mesh*> importGoZ(const std::vector<uint8_t>& buffer, bool triangulateQuads, GoZInfo* outInfo) {
    return importGoZ(buffer.data(), buffer.size(), triangulateQuads, outInfo);
}

std::vector<Mesh*> importGoZ(const uint8_t* data, size_t size, bool triangulateQuads, GoZInfo* outInfo) {
    auto tStart = std::chrono::high_resolution_clock::now();

    if (!data || size < 36) {
        sculpt_log("[GoZ Import ERROR] Buffer is null or too small (%zu bytes)\n", size);
        return {};
    }

    if (std::memcmp(data, "GoZb", 4) != 0) {
        sculpt_log("[GoZ Import ERROR] Missing 'GoZb' binary magic header\n");
        return {};
    }

    GoZInfo info;
    std::vector<float> verts;
    std::vector<RawFace> rawFaces;
    std::vector<uint16_t> rawGroups;
    std::vector<float> rawPaint;
    std::vector<float> rawMask;
    std::vector<float> rawUVs; // per-corner: 4 corners * 2 floats per face
    bool hasPaintData = false;
    bool hasMaskData = false;
    bool hasGroupData = false;
    bool hasUVData = false;

    // Parse chunks starting after 32-byte header
    size_t p = 32;
    while (p + 16 <= size) {
        uint32_t tag = readU32(data + p);
        if (tag == TAG_END) break;

        uint32_t chunkSize = readU32(data + p + 4);
        if (chunkSize < 16 || p + chunkSize > size) {
            sculpt_log("[GoZ Import WARNING] Chunk 0x%X truncated (chunkSize=%u, remaining=%zu)\n",
                       tag, chunkSize, size - p);
            break;
        }

        uint64_t count = readU64(data + p + 8);
        const uint8_t* payload = data + p + 16;
        size_t payloadLen = chunkSize - 16;

        switch (tag) {
            case TAG_OBJECT_NAME: {
                if (payloadLen >= 8 && std::memcmp(payload, "GoZMesh_", 8) == 0) {
                    std::string name(reinterpret_cast<const char*>(payload + 8), payloadLen - 8);
                    while (!name.empty() && (name.back() == '\0' || name.back() == ' ')) {
                        name.pop_back();
                    }
                    info.name = name;
                }
                break;
            }

            case TAG_SUBDIVISION: {
                if (count > 0 && payloadLen >= 16) {
                    info.subdivisionLevel = static_cast<int>(readU32(payload + (count - 1) * 16));
                }
                break;
            }

            case TAG_VERTICES: {
                size_t nbV = std::min<size_t>(static_cast<size_t>(count), payloadLen / 12);
                verts.resize(nbV * 3);
                // In x86_64 IEEE-754 little-endian, binary representation matches GoZ layout directly
                std::memcpy(verts.data(), payload, nbV * 3 * sizeof(float));
                info.nbVerts = nbV;
                break;
            }

            case TAG_FACES: {
                size_t nbF = std::min<size_t>(static_cast<size_t>(count), payloadLen / 16);
                rawFaces.resize(nbF);
                // Block memcpy for 16-byte RawFace structures
                std::memcpy(rawFaces.data(), payload, nbF * sizeof(RawFace));
                info.nbFaces = nbF;
                break;
            }

            case TAG_PAINT: {
                size_t nbP = std::min<size_t>(static_cast<size_t>(count), payloadLen / 4);
                rawPaint.resize(nbP * 3);
                bool anyNonWhite = false;
                #pragma omp parallel for schedule(static) if(nbP > 10000) reduction(|:anyNonWhite)
                for (size_t i = 0; i < nbP; ++i) {
                    const uint8_t* pp = payload + i * 4; // B, G, R, 0 (sRGB)
                    uint8_t b = pp[0];
                    uint8_t g = pp[1];
                    uint8_t r = pp[2];
                    if (r != 255 || g != 255 || b != 255) {
                        anyNonWhite = true;
                    }
                    rawPaint[i * 3 + 0] = srgbLut[r];
                    rawPaint[i * 3 + 1] = srgbLut[g];
                    rawPaint[i * 3 + 2] = srgbLut[b];
                }
                if (anyNonWhite && nbP > 0) {
                    hasPaintData = true;
                    info.hasPaint = true;
                }
                break;
            }

            case TAG_POLYGROUPS: {
                size_t nbG = std::min<size_t>(static_cast<size_t>(count), payloadLen / 2);
                rawGroups.resize(nbG);
                std::memcpy(rawGroups.data(), payload, nbG * sizeof(uint16_t));
                hasGroupData = (!rawGroups.empty());
                info.hasGroups = hasGroupData;
                break;
            }

            case TAG_MASK: {
                size_t nbM = std::min<size_t>(static_cast<size_t>(count), payloadLen / 2);
                rawMask.resize(nbM);
                bool anyMasked = false;
                #pragma omp parallel for schedule(static) if(nbM > 10000) reduction(|:anyMasked)
                for (size_t i = 0; i < nbM; ++i) {
                    uint16_t mVal = readU16(payload + i * 2);
                    // In GoZ: 65535 = unmasked (1.0f in SPSculpt), 0 = fully masked (0.0f)
                    float mFloat = static_cast<float>(mVal) * (1.0f / 65535.0f);
                    rawMask[i] = mFloat;
                    if (mVal != 65535) anyMasked = true;
                }
                if (anyMasked && nbM > 0) {
                    hasMaskData = true;
                    info.hasMask = true;
                }
                break;
            }

            case TAG_UVS: {
                // 4 corners per face, 2 floats per corner = 32 bytes per face
                size_t nbU = std::min<size_t>(static_cast<size_t>(count), payloadLen / 32);
                rawUVs.resize(nbU * 8);
                #pragma omp parallel for schedule(static) if(nbU > 5000)
                for (size_t i = 0; i < nbU * 4; ++i) {
                    const uint8_t* up = payload + i * 8;
                    rawUVs[i * 2 + 0] = readF32(up);
                    rawUVs[i * 2 + 1] = 1.0f - readF32(up + 4); // GoZ stores V downwards
                }
                hasUVData = (!rawUVs.empty());
                info.hasUVs = hasUVData;
                break;
            }

            case TAG_DIFFUSE_MAP:
            case TAG_NORMAL_MAP:
            case TAG_DISPLACEMENT_MAP: {
                std::string path(reinterpret_cast<const char*>(payload), payloadLen);
                while (!path.empty() && (path.back() == '\0' || path.back() == ' ')) {
                    path.pop_back();
                }
                if (tag == TAG_DIFFUSE_MAP) info.diffuseMap = path;
                else if (tag == TAG_NORMAL_MAP) info.normalMap = path;
                else if (tag == TAG_DISPLACEMENT_MAP) info.displacementMap = path;
                break;
            }

            default:
                break;
        }

        p += chunkSize;
    }

    if (verts.empty() || rawFaces.empty()) {
        sculpt_log("[GoZ Import ERROR] Mesh has no %s\n", verts.empty() ? "vertices" : "faces");
        return {};
    }

    uint32_t nbTotalVerts = static_cast<uint32_t>(verts.size() / 3);
    size_t rawCount = rawFaces.size();

    // Process faces (quad preservation or triangulation)
    std::vector<uint32_t> outFaces;
    std::vector<uint32_t> outGroups;
    std::vector<float> uvAr;
    std::vector<uint32_t> uvfAr;

    size_t nbQuads = 0;
    size_t nbTris = 0;

    if (triangulateQuads) {
        std::vector<uint32_t> triOffsets(rawCount + 1, 0);
        #pragma omp parallel for schedule(static) if(rawCount > 10000)
        for (size_t fi = 0; fi < rawCount; ++fi) {
            triOffsets[fi + 1] = (rawFaces[fi].isQuad() && rawFaces[fi].v[3] < nbTotalVerts) ? 2 : 1;
        }
        for (size_t fi = 0; fi < rawCount; ++fi) {
            triOffsets[fi + 1] += triOffsets[fi];
        }
        size_t totalTris = triOffsets[rawCount];
        outFaces.resize(totalTris * 4);
        if (hasGroupData) outGroups.resize(totalTris);

        size_t localQuads = 0;
        size_t localTris = 0;

        #pragma omp parallel for schedule(static) if(rawCount > 10000) reduction(+:localQuads, localTris)
        for (size_t fi = 0; fi < rawCount; ++fi) {
            const auto& f = rawFaces[fi];
            if (f.v[0] >= nbTotalVerts || f.v[1] >= nbTotalVerts || f.v[2] >= nbTotalVerts) continue;

            uint32_t gid = (fi < rawGroups.size()) ? static_cast<uint32_t>(rawGroups[fi]) : 0;
            size_t outIdx = triOffsets[fi];

            if (f.isQuad() && f.v[3] < nbTotalVerts) {
                localQuads++;
                // Triangle 1: (v0, v1, v2)
                outFaces[outIdx * 4 + 0] = f.v[0];
                outFaces[outIdx * 4 + 1] = f.v[1];
                outFaces[outIdx * 4 + 2] = f.v[2];
                outFaces[outIdx * 4 + 3] = TRI_INDEX;
                if (hasGroupData) outGroups[outIdx] = gid;

                // Triangle 2: (v0, v2, v3)
                outFaces[(outIdx + 1) * 4 + 0] = f.v[0];
                outFaces[(outIdx + 1) * 4 + 1] = f.v[2];
                outFaces[(outIdx + 1) * 4 + 2] = f.v[3];
                outFaces[(outIdx + 1) * 4 + 3] = TRI_INDEX;
                if (hasGroupData) outGroups[outIdx + 1] = gid;
            } else {
                localTris++;
                outFaces[outIdx * 4 + 0] = f.v[0];
                outFaces[outIdx * 4 + 1] = f.v[1];
                outFaces[outIdx * 4 + 2] = f.v[2];
                outFaces[outIdx * 4 + 3] = TRI_INDEX;
                if (hasGroupData) outGroups[outIdx] = gid;
            }
        }
        nbQuads = localQuads;
        nbTris = localTris;
    } else {
        outFaces.resize(rawCount * 4);
        std::memcpy(outFaces.data(), rawFaces.data(), rawCount * sizeof(RawFace));

        if (hasGroupData) {
            outGroups.resize(rawCount);
            #pragma omp parallel for schedule(static) if(rawCount > 10000)
            for (size_t fi = 0; fi < rawCount; ++fi) {
                outGroups[fi] = (fi < rawGroups.size()) ? static_cast<uint32_t>(rawGroups[fi]) : 0;
            }
        }

        size_t localQuads = 0;
        size_t localTris = 0;

        #pragma omp parallel for schedule(static) if(rawCount > 10000) reduction(+:localQuads, localTris)
        for (size_t fi = 0; fi < rawCount; ++fi) {
            uint32_t& v3 = outFaces[fi * 4 + 3];
            if (v3 != NO_VERTEX && v3 < nbTotalVerts) {
                localQuads++;
            } else {
                v3 = TRI_INDEX;
                localTris++;
            }
        }
        nbQuads = localQuads;
        nbTris = localTris;
    }

    info.nbQuads = nbQuads;
    info.nbTriangles = nbTris;

    // UVs handling (if face corner UVs are available)
    if (hasUVData && rawUVs.size() == rawFaces.size() * 8) {
        uvAr.reserve(rawUVs.size());
        uvfAr.reserve(outFaces.size());

        for (size_t fi = 0; fi < rawFaces.size(); ++fi) {
            const auto& f = rawFaces[fi];
            bool quad = f.isQuad() && f.v[3] < nbTotalVerts;
            size_t baseUvIdx = uvAr.size() / 2;

            for (int c = 0; c < 4; ++c) {
                uvAr.push_back(rawUVs[fi * 8 + c * 2]);
                uvAr.push_back(rawUVs[fi * 8 + c * 2 + 1]);
            }

            if (triangulateQuads && quad) {
                // Tri 1: corners 0, 1, 2
                uvfAr.push_back(static_cast<uint32_t>(baseUvIdx + 0));
                uvfAr.push_back(static_cast<uint32_t>(baseUvIdx + 1));
                uvfAr.push_back(static_cast<uint32_t>(baseUvIdx + 2));
                uvfAr.push_back(TRI_INDEX);

                // Tri 2: corners 0, 2, 3
                uvfAr.push_back(static_cast<uint32_t>(baseUvIdx + 0));
                uvfAr.push_back(static_cast<uint32_t>(baseUvIdx + 2));
                uvfAr.push_back(static_cast<uint32_t>(baseUvIdx + 3));
                uvfAr.push_back(TRI_INDEX);
            } else {
                uvfAr.push_back(static_cast<uint32_t>(baseUvIdx + 0));
                uvfAr.push_back(static_cast<uint32_t>(baseUvIdx + 1));
                uvfAr.push_back(static_cast<uint32_t>(baseUvIdx + 2));
                uvfAr.push_back(quad ? static_cast<uint32_t>(baseUvIdx + 3) : TRI_INDEX);
            }
        }
    }

    // Free raw faces and raw groups memory early to minimize peak RAM
    rawFaces.clear(); rawFaces.shrink_to_fit();
    rawGroups.clear(); rawGroups.shrink_to_fit();
    rawUVs.clear(); rawUVs.shrink_to_fit();

    // Create and initialize Mesh
    Mesh* mesh = new Mesh();
    mesh->verts = std::move(verts);
    mesh->faces = std::move(outFaces);
    mesh->nbVerts = static_cast<int>(mesh->verts.size() / 3);
    mesh->nbFaces = static_cast<int>(mesh->faces.size() / 4);

    // Face groups
    if (hasGroupData && outGroups.size() == static_cast<size_t>(mesh->nbFaces)) {
        mesh->faceGroups = std::move(outGroups);
        mesh->isFaceGroupDirty = true;
    } else {
        mesh->initFaceGroups();
    }

    // Polypaint
    if (hasPaintData && rawPaint.size() == mesh->verts.size()) {
        mesh->colors = std::move(rawPaint);
    } else {
        mesh->colors.assign(mesh->nbVerts * 3, 1.0f);
    }

    // Materials and mask
    mesh->materials.resize(mesh->nbVerts * 3);
    #pragma omp parallel for schedule(static) if(mesh->nbVerts > 10000)
    for (int i = 0; i < mesh->nbVerts; ++i) {
        mesh->materials[i * 3 + 0] = 0.5f; // roughness
        mesh->materials[i * 3 + 1] = 0.0f; // metalness
        mesh->materials[i * 3 + 2] = (hasMaskData && static_cast<size_t>(i) < rawMask.size())
                                     ? rawMask[i] : 1.0f; // 1.0f = unmasked
    }
    rawMask.clear(); rawMask.shrink_to_fit();

    // UVs
    if (!uvAr.empty() && uvfAr.size() == mesh->faces.size()) {
        mesh->initTexCoordsDataFromOBJData(uvAr, uvfAr);
    }

    // Fast postInit for viewport display:
    // Skips heavy computeTopology and octree build, computing vertex normals directly.
    // Topology and octree will be built lazily on demand if sculpting tools are activated.
    mesh->postInit(false, false);

    if (outInfo) {
        *outInfo = info;
    }

    auto tEnd = std::chrono::high_resolution_clock::now();
    double msTotal = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

    sculpt_log("[GoZ Import] Successfully imported '%s': %d verts, %d faces (%zu quads, %zu tris), paint=%d, groups=%d, mask=%d in %.2f ms\n",
               info.name.empty() ? "GoZMesh" : info.name.c_str(),
               mesh->nbVerts, mesh->nbFaces, nbQuads, nbTris,
               hasPaintData ? 1 : 0, hasGroupData ? 1 : 0, hasMaskData ? 1 : 0, msTotal);

    return {mesh};
}

} // namespace ImportGoZ
