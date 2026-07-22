#include "files/ImportOBJ.h"
#include "common/Constants.h"
#include "mesh/Topology.h"
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cmath>

namespace ImportOBJ {

static std::vector<std::string> splitString(const std::string& str, char delim) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(str);
    while (std::getline(tokenStream, token, delim)) {
        tokens.push_back(token);
    }
    return tokens;
}

static std::vector<std::string> splitByWhitespace(const std::string& str) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream iss(str);
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

static void initMeshOBJ(Mesh* mesh,
                        std::vector<float>& vAr,
                        std::vector<uint32_t>& fAr,
                        std::vector<float>& cAr,
                        std::vector<float>& mAr,
                        std::vector<float>& texAr,
                        std::vector<uint32_t>& uvfAr,
                        std::vector<float>& cArMrgb,
                        std::vector<float>& mArMat) {
    int nbVerts = vAr.size() / 3;
    int nbFaces = fAr.size() / 4;

    mesh->verts = vAr;
    mesh->faces = fAr;
    mesh->nbVerts = nbVerts;
    mesh->nbFaces = nbFaces;

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

    if (!texAr.empty() && uvfAr.size() == fAr.size()) {
        mesh->initTexCoordsDataFromOBJData(texAr, uvfAr);
    }

    // Now compute topology and build octree/normals
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

    vAr.clear();
    fAr.clear();
    cAr.clear();
    mAr.clear();
    texAr.clear();
    uvfAr.clear();
    cArMrgb.clear();
    mArMat.clear();
}

std::vector<Mesh*> importOBJ(const std::string& data) {
    std::vector<Mesh*> meshes;

    std::vector<float> vAr;
    std::vector<float> cAr;
    std::vector<float> cArMrgb;
    std::vector<float> mAr;
    std::vector<float> mArMat;
    std::vector<float> texAr;
    std::vector<uint32_t> fAr;
    std::vector<uint32_t> uvfAr;

    int offsetVertices = 0;
    int offsetTexCoords = 0;
    int nbVertices = 0;
    int nbTexCoords = 0;

    std::istringstream stream(data);
    std::string line;
    float inv255 = 1.0f / 255.0f;

    while (std::getline(stream, line)) {
        // Trim whitespace
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        size_t last = line.find_last_not_of(" \t\r\n");
        line = line.substr(first, (last - first + 1));

        if (line.empty()) continue;

        char firstChar = line[0];

        if (firstChar == 'v') {
            if (line.length() > 1 && line[1] == ' ') {
                auto split = splitByWhitespace(line);
                if (split.size() >= 4) {
                    vAr.push_back(std::stof(split[1]));
                    vAr.push_back(std::stof(split[2]));
                    vAr.push_back(std::stof(split[3]));
                    if (split.size() >= 7) {
                        cAr.push_back(std::stof(split[4]));
                        cAr.push_back(std::stof(split[5]));
                        cAr.push_back(std::stof(split[6]));
                    }
                    nbVertices++;
                }
            } else if (line.length() > 1 && line[1] == 't') {
                auto split = splitByWhitespace(line);
                if (split.size() >= 3) {
                    texAr.push_back(std::stof(split[1]));
                    texAr.push_back(std::stof(split[2]));
                    nbTexCoords++;
                }
            }
        } else if (firstChar == 'f') {
            auto split = splitByWhitespace(line);
            int nbVerts = (int)split.size() - 1;
            if (nbVerts < 3) continue;

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

                auto sp1 = splitString(split[id1], '/');
                auto sp2 = splitString(split[id2], '/');
                auto sp3 = splitString(split[id3], '/');
                bool isQuad = (id4 != -1);
                std::vector<std::string> sp4;
                if (isQuad) {
                    sp4 = splitString(split[id4], '/');
                }

                int iv1 = std::stoi(sp1[0]);
                int iv2 = std::stoi(sp2[0]);
                int iv3 = std::stoi(sp3[0]);
                int iv4 = isQuad ? std::stoi(sp4[0]) : 0;

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

                if (sp1.size() > 1 && !sp1[1].empty()) {
                    int uv1 = std::stoi(sp1[1]);
                    int uv2 = std::stoi(sp2[1]);
                    int uv3 = std::stoi(sp3[1]);
                    int uv4 = isQuad ? std::stoi(sp4[1]) : 0;

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
        } else if (firstChar == '#') {
            if (line.length() > 1 && line[1] == 'M') {
                if (line.rfind("#MRGB ", 0) == 0) {
                    auto split = splitByWhitespace(line);
                    if (split.size() >= 2) {
                        std::string blockMRGB = split[1];
                        for (size_t m = 2; m + 6 <= blockMRGB.length(); m += 8) {
                            unsigned int hexVal = std::stoul(blockMRGB.substr(m, 6), nullptr, 16);
                            cArMrgb.push_back(((hexVal >> 16) & 0xFF) * inv255);
                            cArMrgb.push_back(((hexVal >> 8) & 0xFF) * inv255);
                            cArMrgb.push_back((hexVal & 0xFF) * inv255);
                        }
                    }
                } else if (line.rfind("#MAT ", 0) == 0) {
                    auto split = splitByWhitespace(line);
                    if (split.size() >= 2) {
                        std::string blockMAT = split[1];
                        for (size_t n = 0; n + 6 <= blockMAT.length(); n += 6) {
                            unsigned int hexVal = std::stoul(blockMAT.substr(n, 6), nullptr, 16);
                            mArMat.push_back(((hexVal >> 16) & 0xFF) * inv255);
                            mArMat.push_back(((hexVal >> 8) & 0xFF) * inv255);
                            mArMat.push_back((hexVal & 0xFF) * inv255);
                        }
                    }
                }
            }
        } else if (line.rfind("o ", 0) == 0) {
            if (!meshes.empty()) {
                initMeshOBJ(meshes.back(), vAr, fAr, cAr, mAr, texAr, uvfAr, cArMrgb, mArMat);
                offsetVertices = nbVertices;
                offsetTexCoords = nbTexCoords;
            }
            meshes.push_back(new Mesh());
        }
    }

    if (meshes.empty()) {
        meshes.push_back(new Mesh());
    }
    initMeshOBJ(meshes.back(), vAr, fAr, cAr, mAr, texAr, uvfAr, cArMrgb, mArMat);

    return meshes;
}

} // namespace ImportOBJ
