#include "files/ExportOBJ.h"
#include "common/Constants.h"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

namespace ExportOBJ {

static std::string floatToHexByte(float val) {
    int byteVal = (int)std::round(val * 255.0f);
    byteVal = std::clamp(byteVal, 0, 255);
    std::stringstream ss;
    ss << std::setw(2) << std::setfill('0') << std::hex << byteVal;
    return ss.str();
}

static void addMesh(const Mesh* mesh, std::stringstream& ss, int offsets[2], bool colorZbrush, bool colorAppend) {
    const auto& vAr = mesh->verts;
    const auto& cAr = mesh->colors;
    const auto& mAr = mesh->materials;
    const auto& fAr = mesh->faces;

    int nbVertices = mesh->nbVerts;
    int nbFaces = mesh->nbFaces;
    int nbTexCoords = mesh->texCoords.size() / 2;

    glm::mat4 matrix = mesh->matrix;

    ///////////
    // VERTICES
    ///////////
    for (int i = 0; i < nbVertices; ++i) {
        int j = i * 3;
        glm::vec4 p(vAr[j], vAr[j + 1], vAr[j + 2], 1.0f);
        glm::vec4 wp = matrix * p;

        ss << "v " << wp.x << " " << wp.y << " " << wp.z;
        if (colorAppend && cAr.size() >= (size_t)(j + 3)) {
            ss << " " << cAr[j] << " " << cAr[j + 1] << " " << cAr[j + 2];
        }
        ss << "\n";
    }

    ////////////////
    // COLORS-zbrush
    ////////////////
    if (colorZbrush && cAr.size() >= (size_t)(nbVertices * 3)) {
        // zbrush-like vertex color
        int nbChunk = (nbVertices + 63) / 64;
        for (int i = 0; i < nbChunk; ++i) {
            ss << "#MRGB ";
            int j = i * 64;
            int nbCol = (i == nbChunk - 1) ? nbVertices : (j + 64);
            for (; j < nbCol; ++j) {
                ss << "ff";
                int cId = j * 3;
                ss << floatToHexByte(cAr[cId]);
                ss << floatToHexByte(cAr[cId + 1]);
                ss << floatToHexByte(cAr[cId + 2]);
            }
            ss << "\n";
        }

        // zbrush-like vertex material
        if (mAr.size() >= (size_t)(nbVertices * 3)) {
            nbChunk = (nbVertices + 45) / 46;
            for (int i = 0; i < nbChunk; ++i) {
                ss << "#MAT ";
                int j = i * 46;
                int nbMat = (i == nbChunk - 1) ? nbVertices : (j + 46);
                for (; j < nbMat; ++j) {
                    int mId = j * 3;
                    ss << floatToHexByte(mAr[mId]);
                    ss << floatToHexByte(mAr[mId + 1]);
                    ss << floatToHexByte(mAr[mId + 2]);
                }
                ss << "\n";
            }
        }
    }

    /////
    // UV
    /////
    const auto& fArUV = mesh->facesTexCoord;
    const auto& uvAr = mesh->texCoords;
    bool saveUV = mesh->hasUV && !uvAr.empty() && !fArUV.empty();
    if (saveUV) {
        for (int i = 0; i < nbTexCoords; ++i) {
            int j = i * 2;
            ss << "vt " << uvAr[j] << " " << uvAr[j + 1] << "\n";
        }
    }

    ////////
    // FACES
    ////////
    int offV = offsets[0];
    int offTex = offsets[1];
    offsets[0] += nbVertices;
    offsets[1] += nbTexCoords;

    for (int i = 0; i < nbFaces; ++i) {
        int j = i * 4;
        uint32_t id = fAr[j + 3];
        if (saveUV) {
            ss << "f " << (offV + fAr[j]) << "/" << (offTex + fArUV[j]);
            ss << " " << (offV + fAr[j + 1]) << "/" << (offTex + fArUV[j + 1]);
            ss << " " << (offV + fAr[j + 2]) << "/" << (offTex + fArUV[j + 2]);
            if (id != TRI_INDEX) {
                ss << " " << (offV + id) << "/" << (offTex + fArUV[j + 3]) << "\n";
            } else {
                ss << "\n";
            }
        } else {
            ss << "f " << (offV + fAr[j]);
            ss << " " << (offV + fAr[j + 1]);
            ss << " " << (offV + fAr[j + 2]);
            if (id != TRI_INDEX) {
                ss << " " << (offV + id) << "\n";
            } else {
                ss << "\n";
            }
        }
    }
}

std::string exportOBJ(const std::vector<Mesh*>& meshes, bool colorZbrush, bool colorAppend) {
    std::stringstream ss;
    ss << "s 0\n";
    int offsets[2] = {1, 1};
    for (size_t i = 0; i < meshes.size(); ++i) {
        ss << "o mesh_" << i << "\n";
        addMesh(meshes[i], ss, offsets, colorZbrush, colorAppend);
    }
    return ss.str();
}

} // namespace ExportOBJ
