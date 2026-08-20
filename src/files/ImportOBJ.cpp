#include "files/ImportOBJ.h"
#include "common/Constants.h"
#include "mesh/Topology.h"
#include "common/Logger.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace ImportOBJ {

inline const char* skipWhitespace(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) {
        ++p;
    }
    return p;
}

inline float fast_parse_float(const char* p, const char** next) {
    while (*p == ' ' || *p == '\t') ++p;

    const char* start = p;
    bool neg = false;
    if (*p == '-') {
        neg = true;
        ++p;
    } else if (*p == '+') {
        ++p;
    }

    double val = 0.0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10.0 + (*p - '0');
        ++p;
    }

    if (*p == '.') {
        ++p;
        double weight = 0.1;
        while (*p >= '0' && *p <= '9') {
            val += (*p - '0') * weight;
            weight *= 0.1;
            ++p;
        }
    }

    if (*p == 'e' || *p == 'E') {
        ++p;
        bool expNeg = false;
        if (*p == '-') {
            expNeg = true;
            ++p;
        } else if (*p == '+') {
            ++p;
        }
        int expVal = 0;
        while (*p >= '0' && *p <= '9') {
            expVal = expVal * 10 + (*p - '0');
            ++p;
        }
        if (expNeg) {
            val /= std::pow(10.0, expVal);
        } else {
            val *= std::pow(10.0, expVal);
        }
    }

    if (p == start || (p == start + 1 && (start[0] == '-' || start[0] == '+'))) {
        *next = start;
        return 0.0f;
    }

    *next = p;
    return neg ? static_cast<float>(-val) : static_cast<float>(val);
}

inline int fast_parse_int(const char* p, const char** next) {
    while (*p == ' ' || *p == '\t') ++p;

    const char* start = p;
    bool neg = false;
    if (*p == '-') {
        neg = true;
        ++p;
    } else if (*p == '+') {
        ++p;
    }

    int val = 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        ++p;
    }

    if (p == start || (p == start + 1 && (start[0] == '-' || start[0] == '+'))) {
        *next = start;
        return 0;
    }

    *next = p;
    return neg ? -val : val;
}

struct ObjCorner {
    int v = 0;
    int vt = 0;
    bool hasVt = false;
};

inline ObjCorner parseObjCorner(const char* p, const char** next) {
    ObjCorner c;
    c.v = fast_parse_int(p, next);
    if (*next == p) return c;
    p = *next;

    if (*p == '/') {
        ++p;
        if (*p != '/') {
            const char* vtNext = nullptr;
            c.vt = fast_parse_int(p, &vtNext);
            if (vtNext != p) {
                c.hasVt = true;
                p = vtNext;
            }
        }
        if (*p == '/') {
            ++p;
            const char* vnNext = nullptr;
            fast_parse_int(p, &vnNext);
            p = vnNext;
        }
    }
    *next = p;
    return c;
}

static void initMeshOBJ(Mesh* mesh,
                        std::vector<float>& vAr,
                        std::vector<uint32_t>& fAr,
                        std::vector<uint32_t>& fGroupAr,
                        std::vector<float>& cAr,
                        std::vector<float>& mAr,
                        std::vector<float>& texAr,
                        std::vector<uint32_t>& uvfAr,
                        std::vector<float>& cArMrgb,
                        std::vector<float>& mArMat) {
    auto tStart = std::chrono::high_resolution_clock::now();

    int nbVerts = vAr.size() / 3;
    int nbFaces = fAr.size() / 4;

    mesh->verts = vAr;
    mesh->faces = fAr;
    mesh->nbVerts = nbVerts;
    mesh->nbFaces = nbFaces;

    if (fGroupAr.size() == (size_t)nbFaces) {
        mesh->faceGroups = fGroupAr;
        mesh->isFaceGroupDirty = true;
    } else {
        mesh->initFaceGroups();
    }

    if (cArMrgb.size() == vAr.size()) {
        mesh->colors = cArMrgb;
    } else if (cAr.size() == vAr.size()) {
        mesh->colors = cAr;
    } else {
        mesh->colors.assign(nbVerts * 3, 1.0f);
    }

    if (mArMat.size() == vAr.size()) {
        mesh->materials = mArMat;
    } else if (mAr.size() == vAr.size()) {
        mesh->materials = mAr;
    } else {
        mesh->materials.resize(nbVerts * 3);
        for (int i = 0; i < nbVerts; ++i) {
            mesh->materials[i * 3]     = 0.5f; // roughness
            mesh->materials[i * 3 + 1] = 0.0f; // metalness
            mesh->materials[i * 3 + 2] = 1.0f; // mask
        }
    }

    auto tDataCopyEnd = std::chrono::high_resolution_clock::now();

    double msUVs = 0.0;
    if (!texAr.empty() && uvfAr.size() == fAr.size()) {
        auto tUVStart = std::chrono::high_resolution_clock::now();
        mesh->initTexCoordsDataFromOBJData(texAr, uvfAr);
        auto tUVEnd = std::chrono::high_resolution_clock::now();
        msUVs = std::chrono::duration<double, std::milli>(tUVEnd - tUVStart).count();
    }

    // Now compute topology and build octree/normals
    std::vector<uint32_t> vrvStartCount;
    std::vector<uint32_t> vertRingVert;
    std::vector<uint32_t> vrfStartCount;
    std::vector<uint32_t> vertRingFace;
    std::vector<uint8_t> vertOnEdge;
    
    auto tTopStart = std::chrono::high_resolution_clock::now();
    computeTopology(
        mesh->nbVerts, mesh->faces.data(), mesh->nbFaces,
        vrfStartCount, vertRingFace, vrvStartCount, vertRingVert, vertOnEdge
    );
    auto tTopEnd = std::chrono::high_resolution_clock::now();

    mesh->vrfStartCount = vrfStartCount;
    mesh->vertRingFace = vertRingFace;
    mesh->vrvStartCount = vrvStartCount;
    mesh->vertRingVert = vertRingVert;
    mesh->vertOnEdge = vertOnEdge;
    
    auto tPostInitStart = std::chrono::high_resolution_clock::now();
    mesh->postInit();
    auto tEnd = std::chrono::high_resolution_clock::now();

    double msDataCopy = std::chrono::duration<double, std::milli>(tDataCopyEnd - tStart).count();
    double msTopology = std::chrono::duration<double, std::milli>(tTopEnd - tTopStart).count();
    double msPostInit = std::chrono::duration<double, std::milli>(tEnd - tPostInitStart).count();
    double msTotal = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

    sculpt_log("[OBJ Import] Mesh init (Verts: %d, Faces: %d): Total %.2f ms (DataCopy: %.2fms, UVs: %.2fms, Topology: %.2fms, postInit: %.2fms)\n",
              nbVerts, nbFaces, msTotal, msDataCopy, msUVs, msTopology, msPostInit);

    vAr.clear();
    fAr.clear();
    fGroupAr.clear();
    cAr.clear();
    mAr.clear();
    texAr.clear();
    uvfAr.clear();
    cArMrgb.clear();
    mArMat.clear();
}

std::vector<Mesh*> importOBJ(const std::string& data) {
    auto tImportStart = std::chrono::high_resolution_clock::now();
    std::vector<Mesh*> meshes;

    std::vector<float> vAr;
    std::vector<float> cAr;
    std::vector<float> cArMrgb;
    std::vector<float> mAr;
    std::vector<float> mArMat;
    std::vector<float> texAr;
    std::vector<uint32_t> fAr;
    std::vector<uint32_t> fGroupAr;
    std::vector<uint32_t> uvfAr;

    size_t dataLen = data.size();
    if (dataLen > 10000) {
        vAr.reserve(dataLen / 30 * 3);
        fAr.reserve(dataLen / 30 * 4);
        fGroupAr.reserve(dataLen / 30);
    }

    uint32_t currentGroupId = 0;
    int offsetVertices = 0;
    int offsetTexCoords = 0;
    int nbVertices = 0;
    int nbTexCoords = 0;
    int lineCount = 0;
    int faceCount = 0;

    float inv255 = 1.0f / 255.0f;

    auto tParseStart = std::chrono::high_resolution_clock::now();

    const char* p = data.c_str();
    const char* end = p + dataLen;

    ObjCorner corners[64];

    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) ++p;
        if (p >= end) break;
        if (*p == '\n') {
            ++p;
            continue;
        }

        const char* lineStart = p;
        const char* lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n' && *lineEnd != '\r') {
            ++lineEnd;
        }
        p = lineEnd;

        const char* lineTrailing = lineEnd;
        while (lineTrailing > lineStart && (lineTrailing[-1] == ' ' || lineTrailing[-1] == '\t' || lineTrailing[-1] == '\r')) {
            --lineTrailing;
        }

        if (lineStart >= lineTrailing) continue;

        lineCount++;
        char firstChar = lineStart[0];

        if (firstChar == 'v') {
            if (lineStart + 1 < lineTrailing && (lineStart[1] == ' ' || lineStart[1] == '\t')) {
                const char* cur = lineStart + 2;
                const char* next = nullptr;

                float x = fast_parse_float(cur, &next);
                if (next != cur) {
                    cur = next;
                    float y = fast_parse_float(cur, &next);
                    if (next != cur) {
                        cur = next;
                        float z = fast_parse_float(cur, &next);
                        if (next != cur) {
                            cur = next;
                            vAr.push_back(x);
                            vAr.push_back(y);
                            vAr.push_back(z);
                            nbVertices++;

                            float r = fast_parse_float(cur, &next);
                            if (next != cur) {
                                cur = next;
                                float g = fast_parse_float(cur, &next);
                                if (next != cur) {
                                    cur = next;
                                    float b = fast_parse_float(cur, &next);
                                    if (next != cur) {
                                        cAr.push_back(r);
                                        cAr.push_back(g);
                                        cAr.push_back(b);
                                    }
                                }
                            }
                        }
                    }
                }
            } else if (lineStart + 1 < lineTrailing && lineStart[1] == 't') {
                if (lineStart + 2 < lineTrailing && (lineStart[2] == ' ' || lineStart[2] == '\t')) {
                    const char* cur = lineStart + 3;
                    const char* next = nullptr;
                    float u = fast_parse_float(cur, &next);
                    if (next != cur) {
                        cur = next;
                        float v = fast_parse_float(cur, &next);
                        if (next != cur) {
                            texAr.push_back(u);
                            texAr.push_back(v);
                            nbTexCoords++;
                        }
                    }
                }
            }
        } else if (firstChar == 'f') {
            if (lineStart + 1 < lineTrailing && (lineStart[1] == ' ' || lineStart[1] == '\t')) {
                faceCount++;
                int nbVerts = 0;
                const char* cur = lineStart + 1;
                while (cur < lineTrailing && nbVerts < 64) {
                    const char* next = nullptr;
                    ObjCorner c = parseObjCorner(cur, &next);
                    if (next == cur) {
                        ++cur;
                        while (cur < lineTrailing && (*cur == ' ' || *cur == '\t')) ++cur;
                    } else {
                        corners[nbVerts++] = c;
                        cur = next;
                    }
                }

                if (nbVerts >= 3) {
                    int nbPrim = (nbVerts + 1) / 2 - 1;
                    for (int j = 0; j < nbPrim; ++j) {
                        int id1 = j + 1;
                        int id2 = j + 2;
                        int id3 = nbVerts - id1;
                        int id4 = nbVerts - j;
                        if (id3 == id2) {
                            id3 = id4;
                            id4 = -1;
                        }

                        const ObjCorner& c1 = corners[id1 - 1];
                        const ObjCorner& c2 = corners[id2 - 1];
                        const ObjCorner& c3 = corners[id3 - 1];
                        bool isQuad = (id4 != -1);
                        ObjCorner c4;
                        if (isQuad) {
                            c4 = corners[id4 - 1];
                        }

                        int iv1 = c1.v;
                        int iv2 = c2.v;
                        int iv3 = c3.v;
                        int iv4 = isQuad ? c4.v : 0;

                        if (isQuad && (iv4 == iv1 || iv4 == iv2 || iv4 == iv3)) continue;
                        if (iv1 == iv2 || iv1 == iv3 || iv2 == iv3) continue;

                        iv1 = (iv1 < 0 ? iv1 + nbVertices : iv1 - 1) - offsetVertices;
                        iv2 = (iv2 < 0 ? iv2 + nbVertices : iv2 - 1) - offsetVertices;
                        iv3 = (iv3 < 0 ? iv3 + nbVertices : iv3 - 1) - offsetVertices;
                        if (isQuad) {
                            iv4 = (iv4 < 0 ? iv4 + nbVertices : iv4 - 1) - offsetVertices;
                        }

                        fAr.push_back(iv1);
                        fAr.push_back(iv2);
                        fAr.push_back(iv3);
                        fAr.push_back(isQuad ? iv4 : TRI_INDEX);
                        fGroupAr.push_back(currentGroupId);

                        if (c1.hasVt) {
                            int uv1 = c1.vt;
                            int uv2 = c2.vt;
                            int uv3 = c3.vt;
                            int uv4 = isQuad ? c4.vt : 0;

                            uv1 = (uv1 < 0 ? uv1 + nbTexCoords : uv1 - 1) - offsetTexCoords;
                            uv2 = (uv2 < 0 ? uv2 + nbTexCoords : uv2 - 1) - offsetTexCoords;
                            uv3 = (uv3 < 0 ? uv3 + nbTexCoords : uv3 - 1) - offsetTexCoords;
                            if (isQuad) {
                                uv4 = (uv4 < 0 ? uv4 + nbTexCoords : uv4 - 1) - offsetTexCoords;
                            }

                            uvfAr.push_back(uv1);
                            uvfAr.push_back(uv2);
                            uvfAr.push_back(uv3);
                            uvfAr.push_back(isQuad ? uv4 : TRI_INDEX);
                        } else if (!uvfAr.empty()) {
                            uvfAr.push_back(iv1);
                            uvfAr.push_back(iv2);
                            uvfAr.push_back(iv3);
                            uvfAr.push_back(isQuad ? iv4 : TRI_INDEX);
                        }
                    }
                }
            }
        } else if (firstChar == 'g') {
            if (lineStart + 1 < lineTrailing && (lineStart[1] == ' ' || lineStart[1] == '\t')) {
                const char* cur = lineStart + 2;
                while (cur < lineTrailing && (*cur == ' ' || *cur == '\t')) ++cur;
                const char* gNameStart = cur;
                while (cur < lineTrailing && *cur != ' ' && *cur != '\t') ++cur;

                if (cur > gNameStart) {
                    size_t gLen = cur - gNameStart;
                    if (gLen > 10 && std::strncmp(gNameStart, "polygroup_", 10) == 0) {
                        const char* numP = gNameStart + 10;
                        char* endP = nullptr;
                        currentGroupId = static_cast<uint32_t>(std::strtoul(numP, &endP, 10));
                    } else if (gLen > 6 && std::strncmp(gNameStart, "group_", 6) == 0) {
                        const char* numP = gNameStart + 6;
                        char* endP = nullptr;
                        currentGroupId = static_cast<uint32_t>(std::strtoul(numP, &endP, 10));
                    } else {
                        char* endP = nullptr;
                        unsigned long val = std::strtoul(gNameStart, &endP, 10);
                        if (endP == cur) {
                            currentGroupId = static_cast<uint32_t>(val);
                        } else {
                            uint32_t h = 0;
                            for (const char* pChar = gNameStart; pChar < cur; ++pChar) {
                                h = h * 31 + static_cast<uint8_t>(*pChar);
                            }
                            currentGroupId = (h % 1000 + 1);
                        }
                    }
                }
            }
        } else if (firstChar == '#') {
            if (lineStart + 1 < lineTrailing && lineStart[1] == 'M') {
                size_t lineLen = lineTrailing - lineStart;
                if (lineLen >= 6 && std::strncmp(lineStart, "#MRGB ", 6) == 0) {
                    const char* cur = lineStart + 6;
                    while (cur < lineTrailing && (*cur == ' ' || *cur == '\t')) ++cur;
                    const char* hexStart = cur;
                    while (cur < lineTrailing && *cur != ' ' && *cur != '\t') ++cur;
                    size_t blockLen = cur - hexStart;
                    if (blockLen >= 8) {
                        for (size_t m = 2; m + 6 <= blockLen; m += 8) {
                            char hexBuf[7] = {0};
                            std::memcpy(hexBuf, hexStart + m, 6);
                            unsigned int hexVal = static_cast<unsigned int>(std::strtoul(hexBuf, nullptr, 16));
                            cArMrgb.push_back(((hexVal >> 16) & 0xFF) * inv255);
                            cArMrgb.push_back(((hexVal >> 8) & 0xFF) * inv255);
                            cArMrgb.push_back((hexVal & 0xFF) * inv255);
                        }
                    }
                } else if (lineLen >= 5 && std::strncmp(lineStart, "#MAT ", 5) == 0) {
                    const char* cur = lineStart + 5;
                    while (cur < lineTrailing && (*cur == ' ' || *cur == '\t')) ++cur;
                    const char* hexStart = cur;
                    while (cur < lineTrailing && *cur != ' ' && *cur != '\t') ++cur;
                    size_t blockLen = cur - hexStart;
                    for (size_t n = 0; n + 6 <= blockLen; n += 6) {
                        char hexBuf[7] = {0};
                        std::memcpy(hexBuf, hexStart + n, 6);
                        unsigned int hexVal = static_cast<unsigned int>(std::strtoul(hexBuf, nullptr, 16));
                        mArMat.push_back(((hexVal >> 16) & 0xFF) * inv255);
                        mArMat.push_back(((hexVal >> 8) & 0xFF) * inv255);
                        mArMat.push_back((hexVal & 0xFF) * inv255);
                    }
                }
            }
        } else if (firstChar == 'o') {
            if (lineStart + 1 < lineTrailing && (lineStart[1] == ' ' || lineStart[1] == '\t')) {
                if (!meshes.empty()) {
                    initMeshOBJ(meshes.back(), vAr, fAr, fGroupAr, cAr, mAr, texAr, uvfAr, cArMrgb, mArMat);
                    offsetVertices = nbVertices;
                    offsetTexCoords = nbTexCoords;
                }
                meshes.push_back(new Mesh());
            }
        }
    }

    if (meshes.empty()) {
        meshes.push_back(new Mesh());
    }

    auto tParseEnd = std::chrono::high_resolution_clock::now();
    double msParse = std::chrono::duration<double, std::milli>(tParseEnd - tParseStart).count();

    sculpt_log("[OBJ Import] Stage 1 (Text Line Parsing): %.2f ms for %d lines (%d verts, %d faces, %d texCoords)\n",
              msParse, lineCount, nbVertices, faceCount, nbTexCoords);

    auto tBuildStart = std::chrono::high_resolution_clock::now();
    initMeshOBJ(meshes.back(), vAr, fAr, fGroupAr, cAr, mAr, texAr, uvfAr, cArMrgb, mArMat);
    auto tBuildEnd = std::chrono::high_resolution_clock::now();

    double msBuildTotal = std::chrono::duration<double, std::milli>(tBuildEnd - tBuildStart).count();
    double msImportTotal = std::chrono::duration<double, std::milli>(tBuildEnd - tImportStart).count();

    sculpt_log("[OBJ Import] Total ImportOBJ pipeline: %.2f ms (Text Parse: %.2f ms, Mesh Build: %.2f ms, Objects: %zu)\n",
              msImportTotal, msParse, msBuildTotal, meshes.size());

    return meshes;
}

} // namespace ImportOBJ


