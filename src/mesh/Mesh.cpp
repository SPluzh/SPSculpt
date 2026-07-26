#include "mesh/Mesh.h"
#include <cstring>
#include <algorithm>
#include "mesh/NormalCalc.h"

void Mesh::initFaceGroups() {
    faceGroups.assign(nbFaces, 0);
    isFaceGroupDirty = true;
}

uint32_t Mesh::getNextFreeGroupID() const {
    uint32_t maxId = 0;
    for (uint32_t gid : faceGroups) {
        if (gid > maxId) maxId = gid;
    }
    return maxId + 1;
}

void Mesh::setFaceGroup(uint32_t faceIdx, uint32_t gid) {
    if (faceIdx < (uint32_t)nbFaces) {
        if (faceGroups.size() != (size_t)nbFaces) {
            faceGroups.resize(nbFaces, 0);
        }
        faceGroups[faceIdx] = gid;
        isFaceGroupDirty = true;
    }
}

void Mesh::allocate(int nbV, int nbF, int nbRF, int nbRV) {
    nbVerts = nbV;
    nbFaces = nbF;

    verts.resize(nbVerts * 3);
    faces.resize(nbFaces * 4);
    faceGroups.assign(nbFaces, 0);
    isFaceGroupDirty = true;
    vrfStartCount.resize(nbVerts * 2);
    vertRingFace.resize(nbRF);
    vrvStartCount.resize(nbVerts * 2);
    vertRingVert.resize(nbRV);
    vertOnEdge.resize(nbVerts);

    normals.resize(nbVerts * 3, 0.0f);
    colors.resize(nbVerts * 3, 1.0f);
    materials.resize(nbVerts * 3);
    for (int i = 0; i < nbVerts; ++i) {
        materials[i * 3]     = 0.5f; // roughness
        materials[i * 3 + 1] = 0.0f; // metalness
        materials[i * 3 + 2] = 1.0f; // mask
    }
    vertProxy.resize(nbVerts * 3);
    vertVisible.assign(nbVerts, 1);

    faceNormals.resize(nbFaces * 3, 0.0f);
    faceBoxes.resize(nbFaces * 6, 0.0f);
    faceCenters.resize(nbFaces * 3, 0.0f);
}

void Mesh::postInit() {
    vertProxy = verts;

    if (faceGroups.size() != (size_t)nbFaces) {
        initFaceGroups();
    } else {
        isFaceGroupDirty = true;
    }


    if (normals.size() != nbVerts * 3) {
        normals.resize(nbVerts * 3, 0.0f);
    }
    if (colors.size() != nbVerts * 3) {
        colors.assign(nbVerts * 3, 1.0f);
    }
    if (materials.size() != nbVerts * 3) {
        materials.resize(nbVerts * 3);
        for (int i = 0; i < nbVerts; ++i) {
            materials[i * 3]     = 0.5f; // roughness
            materials[i * 3 + 1] = 0.0f; // metalness
            materials[i * 3 + 2] = 1.0f; // mask
        }
    }

    if (vertVisible.size() != (size_t)nbVerts) {
        vertVisible.assign(nbVerts, 1);
    }

    faceNormals.resize(nbFaces * 3, 0.0f);
    faceBoxes.resize(nbFaces * 6, 0.0f);
    faceCenters.resize(nbFaces * 3, 0.0f);

    updateFaceNormalsAndBoxes(
        verts.data(), nbVerts,
        faces.data(), nbFaces,
        nullptr, -1,
        faceNormals.data(),
        faceBoxes.data(),
        faceCenters.data()
    );

    updateVertexNormals(
        nullptr, -1, nbVerts,
        vrfStartCount.data(),
        vertRingFace.data(),
        faceNormals.data(),
        normals.data()
    );

    octree.build(
        nbVerts, nbFaces,
        faceCenters.data(),
        faceBoxes.data(),
        verts.data(),
        faces.data()
    );
}

#include "scene/Camera.h"
void Mesh::updateMatrices(const Camera& camera) {
    enMatrix = glm::transpose(glm::inverse(glm::mat3(editMatrix)));
    mvMatrix = camera.getViewMatrix() * matrix;
    nMatrix = glm::transpose(glm::inverse(glm::mat3(mvMatrix)));
    mvpMatrix = camera.getProjMatrix() * mvMatrix;
}

void Mesh::computeBbox(float* outBbox) const {
    if (nbVerts == 0) {
        std::memset(outBbox, 0, 6 * sizeof(float));
        return;
    }
    float minX = verts[0], maxX = verts[0];
    float minY = verts[1], maxY = verts[1];
    float minZ = verts[2], maxZ = verts[2];
    for (int i = 1; i < nbVerts; ++i) {
        float x = verts[i * 3];
        float y = verts[i * 3 + 1];
        float z = verts[i * 3 + 2];
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
        if (z < minZ) minZ = z;
        if (z > maxZ) maxZ = z;
    }
    outBbox[0] = minX; outBbox[1] = minY; outBbox[2] = minZ;
    outBbox[3] = maxX; outBbox[4] = maxY; outBbox[5] = maxZ;
}

#include "common/Constants.h"
void Mesh::initTexCoordsDataFromOBJData(const std::vector<float>& uvAr, const std::vector<uint32_t>& uvfArOrig) {
    size_t len = faces.size();
    if (len != uvfArOrig.size()) return;
    
    size_t nbUV = uvAr.size() / 2;
    std::vector<uint32_t> uvfArOrigFixed = uvfArOrig;
    for (size_t i = 0; i < len; ++i) {
        if (uvfArOrigFixed[i] >= nbUV) {
            uvfArOrigFixed[i] = faces[i];
        }
    }
    
    std::vector<int32_t> tagV(nbVerts, 0);
    std::vector<float> tArTemp(nbVerts * 2, 0.0f);
    std::vector<std::vector<uint32_t>> dup;
    int32_t acc = 0;
    int32_t nbDuplicates = 0;
    
    for (size_t i = 0; i < len; ++i) {
        uint32_t iv = faces[i];
        if (iv == TRI_INDEX) continue;
        
        uint32_t uv = uvfArOrigFixed[i];
        int32_t tag = tagV[iv];
        if (tag == (int32_t)(uv + 1)) continue;
        
        if (tag == 0) {
            tagV[iv] = uv + 1;
            tArTemp[iv * 2] = uvAr[uv * 2];
            tArTemp[iv * 2 + 1] = uvAr[uv * 2 + 1];
            continue;
        }
        
        if (tag > 0) {
            tagV[iv] = --acc;
            dup.push_back({uv});
            nbDuplicates++;
            continue;
        }
        
        std::vector<uint32_t>& dupArray = dup[-tag - 1];
        size_t nbDup = dupArray.size();
        size_t j = 0;
        for (; j < nbDup; ++j) {
            if (dupArray[j] == uv) break;
        }
        if (j == nbDup) {
            nbDuplicates++;
            dupArray.push_back(uv);
        }
    }
    
    std::vector<float> tAr((nbVerts + nbDuplicates) * 2, 0.0f);
    std::copy(tArTemp.begin(), tArTemp.end(), tAr.begin());
    std::vector<uint32_t> startCount(nbVerts * 2, 0);
    acc = 0;
    for (int i = 0; i < nbVerts; ++i) {
        int32_t tag = tagV[i];
        if (tag >= 0) continue;
        
        const auto& dAr = dup[-tag - 1];
        size_t nbDu = dAr.size();
        uint32_t start = nbVerts + acc;
        startCount[i * 2] = start;
        startCount[i * 2 + 1] = nbDu;
        acc += nbDu;
        for (size_t j = 0; j < nbDu; ++j) {
            uint32_t idUv = dAr[j] * 2;
            uint32_t idUvCoord = (start + j) * 2;
            tAr[idUvCoord] = uvAr[idUv];
            tAr[idUvCoord + 1] = uvAr[idUv + 1];
        }
    }
    
    std::vector<uint32_t> uvfAr = faces;
    for (size_t i = 0; i < len; ++i) {
        uint32_t iv = uvfAr[i];
        if (iv == TRI_INDEX) continue;
        
        int32_t tag = tagV[iv];
        if (tag > 0) continue;
        
        uint32_t idtex = uvfArOrigFixed[i];
        const auto& dArray = dup[-tag - 1];
        size_t nbEl = dArray.size();
        for (size_t j = 0; j < nbEl; ++j) {
            if (idtex == dArray[j]) {
                uvfAr[i] = startCount[iv * 2] + (uint32_t)j;
                break;
            }
        }
    }
    
    if (nbDuplicates > 0) {
        verts.resize((nbVerts + nbDuplicates) * 3);
        normals.resize((nbVerts + nbDuplicates) * 3);
        colors.resize((nbVerts + nbDuplicates) * 3);
        materials.resize((nbVerts + nbDuplicates) * 3);
        vertVisible.resize(nbVerts + nbDuplicates, 1);
        
        for (int i = 0; i < nbVerts; ++i) {
            int32_t tag = tagV[i];
            if (tag >= 0) continue;
            
            uint32_t start = startCount[i * 2];
            uint32_t count = startCount[i * 2 + 1];
            
            float vx = verts[i * 3], vy = verts[i * 3 + 1], vz = verts[i * 3 + 2];
            float nx = normals[i * 3], ny = normals[i * 3 + 1], nz = normals[i * 3 + 2];
            float cx = colors[i * 3], cy = colors[i * 3 + 1], cz = colors[i * 3 + 2];
            float mx = materials[i * 3], my = materials[i * 3 + 1], mz = materials[i * 3 + 2];
            uint8_t vis = vertVisible[i];
            
            for (uint32_t j = 0; j < count; ++j) {
                uint32_t idx = start + j;
                verts[idx * 3] = vx; verts[idx * 3 + 1] = vy; verts[idx * 3 + 2] = vz;
                normals[idx * 3] = nx; normals[idx * 3 + 1] = ny; normals[idx * 3 + 2] = nz;
                colors[idx * 3] = cx; colors[idx * 3 + 1] = cy; colors[idx * 3 + 2] = cz;
                materials[idx * 3] = mx; materials[idx * 3 + 1] = my; materials[idx * 3 + 2] = mz;
                vertVisible[idx] = vis;
            }
        }
    }
    
    texCoords = tAr;
    facesTexCoord = uvfAr;
    hasUV = true;
    nbVerts = verts.size() / 3;
}
