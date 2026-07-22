#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "mesh/Octree.h"

class Camera;

class Mesh {
public:
    // Geometry
    std::vector<float>    verts;        // nbVerts * 3
    std::vector<float>    normals;      // nbVerts * 3
    std::vector<float>    colors;       // nbVerts * 3
    std::vector<float>    materials;    // nbVerts * 3
    std::vector<float>    faceNormals;  // nbFaces * 3
    std::vector<float>    faceBoxes;    // nbFaces * 6
    std::vector<float>    faceCenters;  // nbFaces * 3
    std::vector<float>    vertProxy;    // nbVerts * 3

    // Topology
    std::vector<uint32_t> faces;           // nbFaces * 4
    std::vector<uint32_t> vrfStartCount;   // nbVerts * 2
    std::vector<uint32_t> vertRingFace;
    std::vector<uint32_t> vrvStartCount;   // nbVerts * 2
    std::vector<uint32_t> vertRingVert;
    std::vector<uint8_t>  vertOnEdge;      // nbVerts
    std::vector<uint8_t>  vertVisible;     // nbVerts

    // Octree
    Octree octree;

    int nbVerts = 0;
    int nbFaces = 0;
    bool isDirty = true;

    Mesh() = default;
    ~Mesh() = default;

    // Initialization from JS-managed arrays

    void allocate(int nbV, int nbF, int nbRF, int nbRV);
    void postInit();

    // Pointer getters for WASM / bindings


    Octree* getOctree() { return &octree; }

    void setDirty(bool dirty) { isDirty = dirty; }
    bool getDirty() const { return isDirty; }

    int shaderType = 0; // 0 = PBR, 1 = MATCAP, 2 = FLAT
    int matcapIdx = 0;
    float albedo[3] = {0.72f, 0.52f, 0.45f};
    float roughness = 0.5f;
    float metallic = 0.0f;
    float alpha = 1.0f;
    bool showWireframe = false;
    bool flatShading = false;

    unsigned int textureId = 0;
    void setTextureId(unsigned int id) { textureId = id; }
    void setShaderType(int type) { shaderType = type; }
    void setMatcap(int idx) { matcapIdx = idx; }
    int getMatcap() const { return matcapIdx; }
    void setAlbedo(float r, float g, float b) { albedo[0] = r; albedo[1] = g; albedo[2] = b; }
    void setRoughness(float r) { roughness = r; }
    void setMetallic(float m) { metallic = m; }
    void setAlpha(float a) { alpha = a; }
    void setShowWireframe(bool show) { showWireframe = show; }
    void setFlatShading(bool flat) { flatShading = flat; }
    float curvature = 0.0f;
    void setCurvature(float c) { curvature = c; }

    glm::mat4 matrix = glm::mat4(1.0f);
    glm::mat4 editMatrix = glm::mat4(1.0f);
    glm::mat4 mvMatrix = glm::mat4(1.0f);
    glm::mat4 mvpMatrix = glm::mat4(1.0f);
    glm::mat3 nMatrix = glm::mat3(1.0f);
    glm::mat3 enMatrix = glm::mat3(1.0f);

    void setMatrix(const std::vector<float>& m) {
        if (m.size() == 16) {
            std::memcpy(&matrix, m.data(), 16 * sizeof(float));
        }
    }
    void setEditMatrix(const std::vector<float>& m) {
        if (m.size() == 16) {
            std::memcpy(&editMatrix, m.data(), 16 * sizeof(float));
        }
    }

    void updateMatrices(const Camera& camera);
    void computeBbox(float* outBbox) const;
};
