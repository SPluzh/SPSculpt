#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
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
    bool isVertexDirty = false;
    bool isTopologyDirty = false;
    uint32_t dirtyVertMin = 0;
    uint32_t dirtyVertMax = 0;

    // SGL/OBJ migration properties
    bool visibleV1 = true;
    bool visibleV2 = true;
    glm::vec3 center{0.0f};
    float scale = 1.0f;
    std::vector<float>    texCoords;     // nbTexCoords * 2
    std::vector<uint32_t> facesTexCoord;  // nbFaces * 4
    bool hasUV = false;

    uint32_t m_id = 0;
    std::string outlinerName = "";
    uint32_t getID() const { return m_id; }

    Mesh() {
        static uint32_t s_idCounter = 0;
        m_id = ++s_idCounter;
        outlinerName = "Mesh " + std::to_string(m_id);
    }
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
    int getShaderType() const { return shaderType; }
    void setMatcap(int idx) { matcapIdx = idx; }
    int getMatcap() const { return matcapIdx; }
    void setAlbedo(float r, float g, float b) { albedo[0] = r; albedo[1] = g; albedo[2] = b; }
    void setRoughness(float r) { roughness = r; }
    void setMetallic(float m) { metallic = m; }
    void setAlpha(float a) { alpha = a; }
    float getOpacity() const { return alpha; }
    void setShowWireframe(bool show) { showWireframe = show; }
    bool getShowWireframe() const { return showWireframe; }
    void setFlatShading(bool flat) { flatShading = flat; }
    bool getFlatShading() const { return flatShading; }
    float curvature = 0.0f;
    void setCurvature(float c) { curvature = c; }

    bool isVisible(int viewport) const { return viewport == 0 ? visibleV1 : visibleV2; }
    void setVisible(bool visible, int viewport) { if (viewport == 0) visibleV1 = visible; else visibleV2 = visible; }
    glm::vec3 getCenter() const { return center; }
    void setCenter(const glm::vec3& c) { center = c; }
    float getScale() const { return scale; }
    void setScale(float s) { scale = s; }
    bool getHasUV() const { return hasUV; }
    void setHasUV(bool h) { hasUV = h; }
    int getNbTexCoords() const { return texCoords.size() / 2; }
    int getNbVertices() const { return nbVerts; }
    int getNbFaces() const { return nbFaces; }

    const std::vector<float>& getVertices() const { return verts; }
    std::vector<float>& getVertices() { return verts; }
    const std::vector<float>& getColors() const { return colors; }
    std::vector<float>& getColors() { return colors; }
    const std::vector<float>& getMaterials() const { return materials; }
    std::vector<float>& getMaterials() { return materials; }
    const std::vector<uint32_t>& getFaces() const { return faces; }
    std::vector<uint32_t>& getFaces() { return faces; }
    const std::vector<float>& getTexCoords() const { return texCoords; }
    std::vector<float>& getTexCoords() { return texCoords; }
    const std::vector<uint32_t>& getFacesTexCoord() const { return facesTexCoord; }
    std::vector<uint32_t>& getFacesTexCoord() { return facesTexCoord; }

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
    void setMatrix(const glm::mat4& m) {
        matrix = m;
    }
    void setEditMatrix(const std::vector<float>& m) {
        if (m.size() == 16) {
            std::memcpy(&editMatrix, m.data(), 16 * sizeof(float));
        }
    }

    void initTexCoordsDataFromOBJData(const std::vector<float>& uvAr, const std::vector<uint32_t>& uvfArOrig);
    void updateMatrices(const Camera& camera);
    void computeBbox(float* outBbox) const;
};
