#include "mesh/Mesh.h"
#include <cstring>
#include <algorithm>
#include "mesh/NormalCalc.h"

void Mesh::allocate(int nbV, int nbF, int nbRF, int nbRV) {
    nbVerts = nbV;
    nbFaces = nbF;

    verts.resize(nbVerts * 3);
    faces.resize(nbFaces * 4);
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
