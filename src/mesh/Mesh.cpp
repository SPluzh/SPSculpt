#include "mesh/Mesh.h"
#include <cstring>
#include <algorithm>
#include <map>
#include "mesh/NormalCalc.h"
#include "mesh/Topology.h"
#include "common/Constants.h"

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

bool Mesh::hasOnlyTriangles() const {
    for (int i = 0; i < nbFaces; ++i) {
        if (faces[i * 4 + 3] != TRI_INDEX) return false;
    }
    return true;
}

int Mesh::getNbQuads() const {
    int quads = 0;
    for (int i = 0; i < nbFaces; ++i) {
        if (faces[i * 4 + 3] != TRI_INDEX) quads++;
    }
    return quads;
}

int Mesh::getNbTriangles() const {
    int tris = 0;
    for (int i = 0; i < nbFaces; ++i) {
        if (faces[i * 4 + 3] != TRI_INDEX) tris += 2;
        else tris += 1;
    }
    return tris;
}

void Mesh::initTopology() {
    computeTopology(
        nbVerts, faces.data(), nbFaces,
        vrfStartCount, vertRingFace,
        vrvStartCount, vertRingVert,
        vertOnEdge
    );
    initEdges();
    vertTagFlags.assign(nbVerts, 0);
}

void Mesh::initEdges() {
    faceEdges.resize(nbFaces * 4, 0);
    nbEdges = 0;
    std::vector<uint32_t> vertEdgeTemp(nbVerts, 0);

    for (int i = 0; i < nbVerts; ++i) {
        uint32_t start = vrfStartCount[i * 2];
        uint32_t end = start + vrfStartCount[i * 2 + 1];
        uint32_t compTest = nbEdges;
        for (uint32_t j = start; j < end; ++j) {
            uint32_t id = vertRingFace[j] * 4;
            uint32_t iv1 = faces[id];
            uint32_t iv2 = faces[id + 1];
            uint32_t iv3 = faces[id + 2];
            uint32_t iv4 = faces[id + 3];
            uint32_t t = 0;
            uint32_t idEdge = 0;
            if (iv4 == TRI_INDEX) {
                if ((uint32_t)i > iv1) {
                    t = vertEdgeTemp[iv1];
                    idEdge = id + ((uint32_t)i == iv2 ? 0 : 2);
                    if (t <= compTest) {
                        faceEdges[idEdge] = nbEdges;
                        vertEdgeTemp[iv1] = ++nbEdges;
                    } else {
                        faceEdges[idEdge] = t - 1;
                    }
                }
                if ((uint32_t)i > iv2) {
                    t = vertEdgeTemp[iv2];
                    idEdge = id + ((uint32_t)i == iv1 ? 0 : 1);
                    if (t <= compTest) {
                        faceEdges[idEdge] = nbEdges;
                        vertEdgeTemp[iv2] = ++nbEdges;
                    } else {
                        faceEdges[idEdge] = t - 1;
                    }
                }
                if ((uint32_t)i > iv3) {
                    t = vertEdgeTemp[iv3];
                    idEdge = id + ((uint32_t)i == iv1 ? 2 : 1);
                    if (t <= compTest) {
                        faceEdges[idEdge] = nbEdges;
                        vertEdgeTemp[iv3] = ++nbEdges;
                    } else {
                        faceEdges[idEdge] = t - 1;
                    }
                }
                faceEdges[id + 3] = TRI_INDEX;
            } else {
                if ((uint32_t)i > iv1 && (uint32_t)i != iv3) {
                    t = vertEdgeTemp[iv1];
                    idEdge = id + ((uint32_t)i == iv2 ? 0 : 3);
                    if (t <= compTest) {
                        faceEdges[idEdge] = nbEdges;
                        vertEdgeTemp[iv1] = ++nbEdges;
                    } else {
                        faceEdges[idEdge] = t - 1;
                    }
                }
                if ((uint32_t)i > iv2 && (uint32_t)i != iv4) {
                    t = vertEdgeTemp[iv2];
                    idEdge = id + ((uint32_t)i == iv1 ? 0 : 1);
                    if (t <= compTest) {
                        faceEdges[idEdge] = nbEdges;
                        vertEdgeTemp[iv2] = ++nbEdges;
                    } else {
                        faceEdges[idEdge] = t - 1;
                    }
                }
                if ((uint32_t)i > iv3 && (uint32_t)i != iv1) {
                    t = vertEdgeTemp[iv3];
                    idEdge = id + ((uint32_t)i == iv2 ? 1 : 2);
                    if (t <= compTest) {
                        faceEdges[idEdge] = nbEdges;
                        vertEdgeTemp[iv3] = ++nbEdges;
                    } else {
                        faceEdges[idEdge] = t - 1;
                    }
                }
                if ((uint32_t)i > iv4 && (uint32_t)i != iv2) {
                    t = vertEdgeTemp[iv4];
                    idEdge = id + ((uint32_t)i == iv1 ? 3 : 2);
                    if (t <= compTest) {
                        faceEdges[idEdge] = nbEdges;
                        vertEdgeTemp[iv4] = ++nbEdges;
                    } else {
                        faceEdges[idEdge] = t - 1;
                    }
                }
            }
        }
    }
    edges.assign(nbEdges, 0);
    for (int k = 0; k < nbFaces; ++k) {
        uint32_t idf = k * 4;
        if (faceEdges[idf] < (uint32_t)nbEdges) edges[faceEdges[idf]]++;
        if (faceEdges[idf + 1] < (uint32_t)nbEdges) edges[faceEdges[idf + 1]]++;
        if (faceEdges[idf + 2] < (uint32_t)nbEdges) edges[faceEdges[idf + 2]]++;
        uint32_t i4 = faceEdges[idf + 3];
        if (i4 != TRI_INDEX && i4 < (uint32_t)nbEdges)
            edges[i4]++;
    }
}

void Mesh::allocate(int nbV, int nbF, int nbRF, int nbRV) {
    m_localRadiusDirty = true;
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
    vertTagFlags.assign(nbVerts, 0);

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
    faceVisible.assign(nbFaces, 1);

    faceNormals.resize(nbFaces * 3, 0.0f);
    faceBoxes.resize(nbFaces * 6, 0.0f);
    faceCenters.resize(nbFaces * 3, 0.0f);
}

void Mesh::postInit() {
    m_localRadiusDirty = true;
    vertProxy = verts;
    vertTagFlags.assign(nbVerts, 0);

    float bbox[6];
    computeBbox(bbox);
    center = glm::vec3((bbox[0] + bbox[3]) * 0.5f, (bbox[1] + bbox[4]) * 0.5f, (bbox[2] + bbox[5]) * 0.5f);

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
    if (faceVisible.size() != (size_t)nbFaces) {
        faceVisible.assign(nbFaces, 1);
    }

    faceNormals.resize(nbFaces * 3, 0.0f);
    faceBoxes.resize(nbFaces * 6, 0.0f);
    faceCenters.resize(nbFaces * 3, 0.0f);

    if (vrfStartCount.size() != (size_t)(nbVerts * 2) || vertRingFace.empty()) {
        initTopology();
    } else {
        initEdges();
    }

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

#include <cmath>

float Mesh::computeLocalRadius() const {
    if (nbVerts == 0) return 1.0f;
    if (!m_localRadiusDirty && m_cachedLocalRadius > 0.0f) {
        return m_cachedLocalRadius;
    }
    float bbox[6];
    computeBbox(bbox);
    float dx = bbox[3] - bbox[0];
    float dy = bbox[4] - bbox[1];
    float dz = bbox[5] - bbox[2];
    float radius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;
    m_cachedLocalRadius = radius > 1e-6f ? radius : 1.0f;
    m_localRadiusDirty = false;
    return m_cachedLocalRadius;
}

glm::vec3 Mesh::getSymmetryOriginForAxis(int axisIndex, SymmetryMode mode) const {
    glm::vec3 normalVec(0.0f);
    if (axisIndex == 0) normalVec = glm::vec3(1.0f, 0.0f, 0.0f);
    else if (axisIndex == 1) normalVec = glm::vec3(0.0f, 1.0f, 0.0f);
    else if (axisIndex == 2) normalVec = glm::vec3(0.0f, 0.0f, 1.0f);

    if (mode == SymmetryMode::World) {
        float worldRadius = computeLocalRadius() * scale;
        glm::vec3 worldOrigin = normalVec * (symmetryOffset * worldRadius);
        glm::mat4 invM = glm::inverse(matrix);
        return glm::vec3(invM * glm::vec4(worldOrigin, 1.0f));
    } else {
        float localRadius = computeLocalRadius();
        return normalVec * (symmetryOffset * localRadius);
    }
}

glm::vec3 Mesh::getSymmetryNormalForAxis(int axisIndex, SymmetryMode mode) const {
    glm::vec3 normalVec(0.0f);
    if (axisIndex == 0) normalVec = glm::vec3(1.0f, 0.0f, 0.0f);
    else if (axisIndex == 1) normalVec = glm::vec3(0.0f, 1.0f, 0.0f);
    else if (axisIndex == 2) normalVec = glm::vec3(0.0f, 0.0f, 1.0f);

    if (mode == SymmetryMode::World) {
        glm::mat3 m3 = glm::transpose(glm::mat3(matrix));
        glm::vec3 localNormal = m3 * normalVec;
        float len = glm::length(localNormal);
        return len > 1e-5f ? localNormal / len : normalVec;
    } else {
        return normalVec;
    }
}

void Mesh::flip(int axisIndex) {
    if (nbVerts == 0 || nbFaces == 0) return;
    if (axisIndex < 0 || axisIndex > 2) return;

    float cVal = center[axisIndex];
    for (int i = 0; i < nbVerts; ++i) {
        int id = i * 3 + axisIndex;
        verts[id] = 2.0f * cVal - verts[id];
    }

    for (int i = 0; i < nbFaces; ++i) {
        int id = i * 4;
        uint32_t iv1 = faces[id];
        uint32_t iv2 = faces[id + 1];
        uint32_t iv3 = faces[id + 2];
        uint32_t iv4 = faces[id + 3];
        if (iv4 == TRI_INDEX) {
            faces[id + 1] = iv3;
            faces[id + 2] = iv2;
        } else {
            faces[id + 1] = iv4;
            faces[id + 3] = iv2;
        }
    }

    if (hasUV && facesTexCoord.size() == static_cast<size_t>(nbFaces * 4)) {
        for (int i = 0; i < nbFaces; ++i) {
            int id = i * 4;
            uint32_t uv1 = facesTexCoord[id];
            uint32_t uv2 = facesTexCoord[id + 1];
            uint32_t uv3 = facesTexCoord[id + 2];
            uint32_t uv4 = facesTexCoord[id + 3];
            if (uv4 == TRI_INDEX) {
                facesTexCoord[id + 1] = uv3;
                facesTexCoord[id + 2] = uv2;
            } else {
                facesTexCoord[id + 1] = uv4;
                facesTexCoord[id + 3] = uv2;
            }
        }
    }

    vrfStartCount.clear();
    vertRingFace.clear();
    vrvStartCount.clear();
    vertRingVert.clear();
    vertOnEdge.clear();

    postInit();
    isDirty = true;
    isTopologyDirty = true;
}

void Mesh::mirror(int axisIndex, bool positiveToNegative, SymmetryMode mode) {
    if (nbVerts == 0 || nbFaces == 0) return;
    if (axisIndex < 0 || axisIndex > 2) return;

    glm::vec3 origin = getSymmetryOriginForAxis(axisIndex, mode);
    glm::vec3 normal = getSymmetryNormalForAxis(axisIndex, mode);

    // 1. Calculate signed distance to the plane for each vertex
    std::vector<float> distances(nbVerts);
    for (int i = 0; i < nbVerts; ++i) {
        int idx = i * 3;
        float vx = verts[idx]     - origin.x;
        float vy = verts[idx + 1] - origin.y;
        float vz = verts[idx + 2] - origin.z;
        distances[i] = vx * normal.x + vy * normal.y + vz * normal.z;
    }

    // 2. Classify vertices: Keep, Discard, or OnPlane
    enum class Side { Keep, Discard, OnPlane };
    const float eps = 1e-6f;
    std::vector<Side> vertSide(nbVerts);
    for (int i = 0; i < nbVerts; ++i) {
        float d = distances[i];
        if (std::abs(d) <= eps) {
            vertSide[i] = Side::OnPlane;
            distances[i] = 0.0f;
        } else if (positiveToNegative ? (d > 0.0f) : (d < 0.0f)) {
            vertSide[i] = Side::Keep;
        } else {
            vertSide[i] = Side::Discard;
        }
    }

    // 3. Classify faces: Kept, Discarded, or Crossing
    enum class FaceType { Kept, Discarded, Crossing };
    std::vector<FaceType> faceTypes(nbFaces);
    for (int i = 0; i < nbFaces; ++i) {
        int idx = i * 4;
        uint32_t iv1 = faces[idx];
        uint32_t iv2 = faces[idx + 1];
        uint32_t iv3 = faces[idx + 2];
        uint32_t iv4 = faces[idx + 3];

        bool hasKeep = (vertSide[iv1] == Side::Keep || vertSide[iv2] == Side::Keep || vertSide[iv3] == Side::Keep || (iv4 != TRI_INDEX && vertSide[iv4] == Side::Keep));
        bool hasDiscard = (vertSide[iv1] == Side::Discard || vertSide[iv2] == Side::Discard || vertSide[iv3] == Side::Discard || (iv4 != TRI_INDEX && vertSide[iv4] == Side::Discard));

        if (hasKeep && !hasDiscard) {
            faceTypes[i] = FaceType::Kept;
        } else if (hasDiscard && !hasKeep) {
            faceTypes[i] = FaceType::Discarded;
        } else if (hasKeep && hasDiscard) {
            faceTypes[i] = FaceType::Crossing;
        } else { // All vertices are OnPlane
            faceTypes[i] = FaceType::Kept;
        }
    }

    // 4. Construct kept half-mesh geometry
    std::vector<float> tempVertices;
    std::vector<float> tempColors;
    std::vector<float> tempMaterials;
    std::vector<bool> boundaryFlags;
    std::vector<uint32_t> tempFaces;
    std::vector<uint32_t> tempFaceGroups;
    std::vector<int32_t> newVertsMap(nbVerts, -1);

    bool hasColors  = (colors.size() == static_cast<size_t>(nbVerts * 3));
    bool hasMats    = (materials.size() == static_cast<size_t>(nbVerts * 3));
    bool hasFaceGps = (faceGroups.size() == static_cast<size_t>(nbFaces));

    auto addKeptVertex = [&](uint32_t origIdx) -> uint32_t {
        if (newVertsMap[origIdx] != -1) {
            return static_cast<uint32_t>(newVertsMap[origIdx]);
        }

        uint32_t newIdx = static_cast<uint32_t>(tempVertices.size() / 3);
        newVertsMap[origIdx] = static_cast<int32_t>(newIdx);

        uint32_t o3 = origIdx * 3;
        float vx = verts[o3];
        float vy = verts[o3 + 1];
        float vz = verts[o3 + 2];

        bool onBoundary = (vertSide[origIdx] == Side::OnPlane);
        if (onBoundary) {
            float dist = distances[origIdx];
            vx -= dist * normal.x;
            vy -= dist * normal.y;
            vz -= dist * normal.z;
        }

        tempVertices.push_back(vx);
        tempVertices.push_back(vy);
        tempVertices.push_back(vz);

        if (hasColors) {
            tempColors.push_back(colors[o3]);
            tempColors.push_back(colors[o3 + 1]);
            tempColors.push_back(colors[o3 + 2]);
        }
        if (hasMats) {
            tempMaterials.push_back(materials[o3]);
            tempMaterials.push_back(materials[o3 + 1]);
            tempMaterials.push_back(materials[o3 + 2]);
        }

        boundaryFlags.push_back(onBoundary);
        return newIdx;
    };

    std::map<std::pair<uint32_t, uint32_t>, uint32_t> clipVertexCache;

    auto addClipVertex = [&](uint32_t idxA, uint32_t idxB) -> uint32_t {
        auto key = std::make_pair(std::min(idxA, idxB), std::max(idxA, idxB));
        auto it = clipVertexCache.find(key);
        if (it != clipVertexCache.end()) {
            return it->second;
        }

        float dA = distances[idxA];
        float dB = distances[idxB];

        float denom = dA - dB;
        float t = 0.5f;
        if (std::abs(denom) > 1e-12f) {
            t = dA / denom;
        }
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;

        uint32_t a3 = idxA * 3;
        uint32_t b3 = idxB * 3;

        float vx = verts[a3]   + t * (verts[b3]   - verts[a3]);
        float vy = verts[a3+1] + t * (verts[b3+1] - verts[a3+1]);
        float vz = verts[a3+2] + t * (verts[b3+2] - verts[a3+2]);

        // Snap position exactly onto plane
        float dx = vx - origin.x;
        float dy = vy - origin.y;
        float dz = vz - origin.z;
        float projDist = dx * normal.x + dy * normal.y + dz * normal.z;
        vx -= projDist * normal.x;
        vy -= projDist * normal.y;
        vz -= projDist * normal.z;

        uint32_t newIdx = static_cast<uint32_t>(tempVertices.size() / 3);
        tempVertices.push_back(vx);
        tempVertices.push_back(vy);
        tempVertices.push_back(vz);

        if (hasColors) {
            tempColors.push_back(colors[a3]   + t * (colors[b3]   - colors[a3]));
            tempColors.push_back(colors[a3+1] + t * (colors[b3+1] - colors[a3+1]));
            tempColors.push_back(colors[a3+2] + t * (colors[b3+2] - colors[a3+2]));
        }
        if (hasMats) {
            tempMaterials.push_back(materials[a3]   + t * (materials[b3]   - materials[a3]));
            tempMaterials.push_back(materials[a3+1] + t * (materials[b3+1] - materials[a3+1]));
            tempMaterials.push_back(materials[a3+2] + t * (materials[b3+2] - materials[a3+2]));
        }

        boundaryFlags.push_back(true);
        clipVertexCache[key] = newIdx;
        return newIdx;
    };

    for (int i = 0; i < nbFaces; ++i) {
        if (faceTypes[i] == FaceType::Discarded) {
            continue;
        }

        uint32_t gid = hasFaceGps ? faceGroups[i] : 0;
        int idx = i * 4;
        uint32_t iv1 = faces[idx];
        uint32_t iv2 = faces[idx + 1];
        uint32_t iv3 = faces[idx + 2];
        uint32_t iv4 = faces[idx + 3];

        if (faceTypes[i] == FaceType::Kept) {
            uint32_t nv1 = addKeptVertex(iv1);
            uint32_t nv2 = addKeptVertex(iv2);
            uint32_t nv3 = addKeptVertex(iv3);
            uint32_t nv4 = (iv4 != TRI_INDEX) ? addKeptVertex(iv4) : TRI_INDEX;

            tempFaces.push_back(nv1);
            tempFaces.push_back(nv2);
            tempFaces.push_back(nv3);
            tempFaces.push_back(nv4);
            tempFaceGroups.push_back(gid);
        } else if (faceTypes[i] == FaceType::Crossing) {
            std::vector<uint32_t> fVerts = {iv1, iv2, iv3};
            if (iv4 != TRI_INDEX) fVerts.push_back(iv4);

            size_t nCorner = fVerts.size();
            std::vector<uint32_t> outPolygon;

            for (size_t c = 0; c < nCorner; ++c) {
                uint32_t curr = fVerts[c];
                uint32_t next = fVerts[(c + 1) % nCorner];

                Side sideCurr = vertSide[curr];
                Side sideNext = vertSide[next];

                bool currInside = (sideCurr == Side::Keep || sideCurr == Side::OnPlane);
                bool nextInside = (sideNext == Side::Keep || sideNext == Side::OnPlane);

                if (currInside) {
                    uint32_t vCurrIdx = addKeptVertex(curr);
                    if (outPolygon.empty() || outPolygon.back() != vCurrIdx) {
                        outPolygon.push_back(vCurrIdx);
                    }

                    if (!nextInside) {
                        if (sideCurr != Side::OnPlane) {
                            uint32_t vClip = addClipVertex(curr, next);
                            if (outPolygon.empty() || outPolygon.back() != vClip) {
                                outPolygon.push_back(vClip);
                            }
                        }
                    }
                } else {
                    if (nextInside) {
                        if (sideNext != Side::OnPlane) {
                            uint32_t vClip = addClipVertex(curr, next);
                            if (outPolygon.empty() || outPolygon.back() != vClip) {
                                outPolygon.push_back(vClip);
                            }
                        }
                    }
                }
            }

            if (outPolygon.size() > 1 && outPolygon.front() == outPolygon.back()) {
                outPolygon.pop_back();
            }

            if (outPolygon.size() == 3) {
                tempFaces.push_back(outPolygon[0]);
                tempFaces.push_back(outPolygon[1]);
                tempFaces.push_back(outPolygon[2]);
                tempFaces.push_back(TRI_INDEX);
                tempFaceGroups.push_back(gid);
            } else if (outPolygon.size() == 4) {
                tempFaces.push_back(outPolygon[0]);
                tempFaces.push_back(outPolygon[1]);
                tempFaces.push_back(outPolygon[2]);
                tempFaces.push_back(outPolygon[3]);
                tempFaceGroups.push_back(gid);
            } else if (outPolygon.size() > 4) {
                for (size_t k = 1; k + 1 < outPolygon.size(); ++k) {
                    tempFaces.push_back(outPolygon[0]);
                    tempFaces.push_back(outPolygon[k]);
                    tempFaces.push_back(outPolygon[k + 1]);
                    tempFaces.push_back(TRI_INDEX);
                    tempFaceGroups.push_back(gid);
                }
            }
        }
    }

    if (tempFaces.empty()) return;

    // 5. Mirror strictly source-side vertices
    size_t numKeptVerts = tempVertices.size() / 3;
    std::vector<uint32_t> mirrorMap(numKeptVerts);

    for (size_t i = 0; i < numKeptVerts; ++i) {
        if (boundaryFlags[i]) {
            mirrorMap[i] = static_cast<uint32_t>(i);
        } else {
            uint32_t newIdx = static_cast<uint32_t>(tempVertices.size() / 3);
            mirrorMap[i] = newIdx;

            size_t i3 = i * 3;
            float vx = tempVertices[i3];
            float vy = tempVertices[i3 + 1];
            float vz = tempVertices[i3 + 2];

            float dx = vx - origin.x;
            float dy = vy - origin.y;
            float dz = vz - origin.z;
            float d = dx * normal.x + dy * normal.y + dz * normal.z;

            float mx = vx - 2.0f * d * normal.x;
            float my = vy - 2.0f * d * normal.y;
            float mz = vz - 2.0f * d * normal.z;

            tempVertices.push_back(mx);
            tempVertices.push_back(my);
            tempVertices.push_back(mz);

            if (hasColors) {
                tempColors.push_back(tempColors[i3]);
                tempColors.push_back(tempColors[i3 + 1]);
                tempColors.push_back(tempColors[i3 + 2]);
            }
            if (hasMats) {
                tempMaterials.push_back(tempMaterials[i3]);
                tempMaterials.push_back(tempMaterials[i3 + 1]);
                tempMaterials.push_back(tempMaterials[i3 + 2]);
            }
        }
    }

    // 6. Build final face list combining kept faces and mirrored faces with reversed winding
    std::vector<uint32_t> finalFaces;
    size_t numKeptFaceCount = tempFaces.size() / 4;
    finalFaces.reserve(tempFaces.size() * 2);

    std::vector<uint32_t> finalFaceGroups;
    finalFaceGroups.reserve(tempFaceGroups.size() * 2);

    for (size_t f = 0; f < tempFaces.size(); ++f) {
        finalFaces.push_back(tempFaces[f]);
    }
    for (size_t fg : tempFaceGroups) {
        finalFaceGroups.push_back(fg);
    }

    for (size_t i = 0; i < numKeptFaceCount; ++i) {
        size_t idx = i * 4;
        uint32_t nv1 = tempFaces[idx];
        uint32_t nv2 = tempFaces[idx + 1];
        uint32_t nv3 = tempFaces[idx + 2];
        uint32_t nv4 = tempFaces[idx + 3];

        uint32_t mv1 = mirrorMap[nv1];
        uint32_t mv2 = mirrorMap[nv2];
        uint32_t mv3 = mirrorMap[nv3];
        uint32_t mv4 = (nv4 != TRI_INDEX) ? mirrorMap[nv4] : TRI_INDEX;

        if (mv4 == TRI_INDEX) {
            finalFaces.push_back(mv1);
            finalFaces.push_back(mv3);
            finalFaces.push_back(mv2);
            finalFaces.push_back(TRI_INDEX);
        } else {
            finalFaces.push_back(mv1);
            finalFaces.push_back(mv4);
            finalFaces.push_back(mv3);
            finalFaces.push_back(mv2);
        }

        finalFaceGroups.push_back(tempFaceGroups[i]);
    }

    // 7. Update Mesh data
    nbVerts = static_cast<int>(tempVertices.size() / 3);
    nbFaces = static_cast<int>(finalFaces.size() / 4);

    verts = std::move(tempVertices);
    if (hasColors) colors = std::move(tempColors);
    else colors.assign(nbVerts * 3, 1.0f);

    if (hasMats) materials = std::move(tempMaterials);
    else {
        materials.resize(nbVerts * 3);
        for (int i = 0; i < nbVerts; ++i) {
            materials[i * 3]     = 0.5f;
            materials[i * 3 + 1] = 0.0f;
            materials[i * 3 + 2] = 1.0f;
        }
    }

    faces = std::move(finalFaces);
    faceGroups = std::move(finalFaceGroups);

    vrfStartCount.clear();
    vertRingFace.clear();
    vrvStartCount.clear();
    vertRingVert.clear();
    vertOnEdge.clear();

    postInit();
    isDirty = true;
    isTopologyDirty = true;
}

