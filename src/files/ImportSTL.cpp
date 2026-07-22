#include "files/ImportSTL.h"
#include "common/Constants.h"
#include "mesh/Topology.h"
#include <unordered_map>
#include <string>
#include <sstream>
#include <iostream>
#include <cstring>
#include <algorithm>

namespace ImportSTL {

static uint32_t detectNewVertex(std::unordered_map<std::string, uint32_t>& mapVertices,
                                 const std::vector<float>& vb,
                                 const std::vector<float>& vbc,
                                 size_t start,
                                 std::vector<float>& outVerts,
                                 std::vector<float>& outColors,
                                 uint32_t& nbVertices) {
    float x = vb[start];
    float y = vb[start + 1];
    float z = vb[start + 2];
    
    // Hash key matches JS string interpolation
    std::string hash = std::to_string(x) + "+" + std::to_string(y) + "+" + std::to_string(z);
    
    auto it = mapVertices.find(hash);
    if (it == mapVertices.end()) {
        uint32_t idVertex = nbVertices;
        mapVertices[hash] = idVertex;
        
        outVerts.push_back(x);
        outVerts.push_back(y);
        outVerts.push_back(z);
        
        if (!vbc.empty()) {
            outColors.push_back(vbc[start]);
            outColors.push_back(vbc[start + 1]);
            outColors.push_back(vbc[start + 2]);
        }
        
        nbVertices++;
        return idVertex;
    }
    return it->second;
}

static std::vector<float> importAsciiSTL(const std::string& data) {
    std::vector<float> vb;
    std::istringstream stream(data);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    
    size_t nbLines = lines.size();
    for (size_t i = 0; i < nbLines; ++i) {
        // Trim line
        std::string trimmed = lines[i];
        size_t first = trimmed.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            size_t last = trimmed.find_last_not_of(" \t\r\n");
            trimmed = trimmed.substr(first, (last - first + 1));
        } else {
            continue;
        }
        
        if (trimmed.rfind("facet", 0) == 0) {
            if (i + 4 < nbLines) {
                for (int offset = 2; offset <= 4; ++offset) {
                    std::string vLine = lines[i + offset];
                    size_t vf = vLine.find_first_not_of(" \t\r\n");
                    if (vf != std::string::npos) {
                        size_t vl = vLine.find_last_not_of(" \t\r\n");
                        vLine = vLine.substr(vf, (vl - vf + 1));
                    }
                    std::istringstream iss(vLine);
                    std::string dummy;
                    float x = 0.0f, y = 0.0f, z = 0.0f;
                    if (iss >> dummy >> x >> y >> z) {
                        vb.push_back(x);
                        vb.push_back(y);
                        vb.push_back(z);
                    }
                }
            }
        }
    }
    return vb;
}

static std::pair<std::vector<float>, std::vector<float>> importBinarySTL(const std::vector<uint8_t>& buffer, uint32_t nbTriangles) {
    std::vector<float> vb(nbTriangles * 9, 0.0f);
    std::vector<float> vbc(nbTriangles * 9, 1.0f);
    
    // Header Magic Check for "COLOR="
    std::string header(reinterpret_cast<const char*>(buffer.data()), std::min<size_t>(80, buffer.size()));
    bool colorMagic = (header.find("COLOR=") != std::string::npos);
    
    size_t offset = 96; // 84 + 12 (first facet normal)
    size_t j = 0;
    std::vector<uint16_t> uc(nbTriangles);
    
    for (uint32_t i = 0; i < nbTriangles; ++i) {
        if (offset + 36 + 2 > buffer.size()) break;
        
        std::memcpy(&vb[j], buffer.data() + offset, 36);
        j += 9;
        offset += 36;
        
        uint16_t uVal;
        std::memcpy(&uVal, buffer.data() + offset, 2);
        uc[i] = uVal;
        offset += 2 + 12; // skip attributes and next normal
    }
    
    float inv = 1.0f / 31.0f;
    for (uint32_t i = 0; i < nbTriangles; ++i) {
        uint16_t u = uc[i];
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        
        bool bit15 = (u & 32768) != 0;
        if (colorMagic) {
            if (!bit15) {
                r = ((u & 31) & 31) * inv;
                g = ((u >> 5) & 31) * inv;
                b = ((u >> 10) & 31) * inv;
            }
        } else if (bit15) {
            r = ((u >> 10) & 31) * inv;
            g = ((u >> 5) & 31) * inv;
            b = ((u & 31) & 31) * inv;
        }
        
        size_t cIdx = i * 9;
        vbc[cIdx] = vbc[cIdx + 3] = vbc[cIdx + 6] = r;
        vbc[cIdx + 1] = vbc[cIdx + 4] = vbc[cIdx + 7] = g;
        vbc[cIdx + 2] = vbc[cIdx + 5] = vbc[cIdx + 8] = b;
    }
    
    return {vb, vbc};
}

std::vector<Mesh*> importSTL(const std::vector<uint8_t>& buffer) {
    uint32_t nbTriangles = 0;
    bool isBinary = false;
    if (buffer.size() >= 84) {
        std::memcpy(&nbTriangles, buffer.data() + 80, 4);
        isBinary = (84 + nbTriangles * 50 == buffer.size());
    }
    
    std::vector<float> vb;
    std::vector<float> vbc;
    
    if (isBinary) {
        auto res = importBinarySTL(buffer, nbTriangles);
        vb = res.first;
        vbc = res.second;
    } else {
        std::string asciiData(reinterpret_cast<const char*>(buffer.data()), buffer.size());
        vb = importAsciiSTL(asciiData);
    }
    
    nbTriangles = vb.size() / 9;
    std::unordered_map<std::string, uint32_t> mapVertices;
    uint32_t nbVertices = 0;
    
    std::vector<float> outVerts;
    std::vector<float> outColors;
    std::vector<uint32_t> outFaces;
    outFaces.reserve(nbTriangles * 4);
    
    for (uint32_t i = 0; i < nbTriangles; ++i) {
        size_t idv = i * 9;
        uint32_t iv1 = detectNewVertex(mapVertices, vb, vbc, idv, outVerts, outColors, nbVertices);
        uint32_t iv2 = detectNewVertex(mapVertices, vb, vbc, idv + 3, outVerts, outColors, nbVertices);
        uint32_t iv3 = detectNewVertex(mapVertices, vb, vbc, idv + 6, outVerts, outColors, nbVertices);
        
        outFaces.push_back(iv1);
        outFaces.push_back(iv2);
        outFaces.push_back(iv3);
        outFaces.push_back(TRI_INDEX);
    }
    
    Mesh* mesh = new Mesh();
    mesh->verts = outVerts;
    if (!outColors.empty()) {
        mesh->colors = outColors;
    } else {
        mesh->colors.assign(nbVertices * 3, 1.0f);
    }
    mesh->faces = outFaces;
    mesh->nbVerts = nbVertices;
    mesh->nbFaces = nbTriangles;
    
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
    
    return {mesh};
}

} // namespace ImportSTL
