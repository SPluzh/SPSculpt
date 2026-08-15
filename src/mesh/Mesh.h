#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "mesh/Octree.h"
#include "mesh/Layer.h"

#include "sculpt/ArmatureGraph.h"

#include "common/Enums.h"
#include "common/Constants.h"

class Camera;

class Mesh {
public:
    // Armature support
    bool isArmature = false;
    std::unique_ptr<ArmatureGraph> armatureGraph;

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
    std::vector<uint32_t> faceGroups;      // nbFaces * 1 (GroupID per face)
    std::vector<uint32_t> vrfStartCount;   // nbVerts * 2
    std::vector<uint32_t> vertRingFace;
    std::vector<uint32_t> vrvStartCount;   // nbVerts * 2
    std::vector<uint32_t> vertRingVert;
    std::vector<uint8_t>  vertOnEdge;      // nbVerts
    std::vector<uint8_t>  vertVisible;     // nbVerts
    std::vector<uint8_t>  faceVisible;     // nbFaces
    std::vector<uint32_t> edges;           // nbEdges (valence per edge)
    std::vector<uint32_t> faceEdges;       // nbFaces * 4 (edge index per face edge)
    std::vector<uint32_t> vertTagFlags;    // nbVerts (tag flags for algorithms)

    // Dynamic Topology
    bool isDynamic = false;
    bool hasQuads = false;
    float subdivisionFactor = 1.0f;
    float decimationFactor = 1.0f;
    std::vector<std::vector<uint32_t>> dynVRV; // vertex ring vert
    std::vector<std::vector<uint32_t>> dynVRF; // vertex ring face
    std::vector<int32_t>  facesStateFlags;
    std::vector<uint32_t> vertSculptFlags;
    std::vector<int32_t>  vertStateFlags;

    // Octree
    Octree octree;

    // Layers
    LayerStack layerStack;
    bool hasLayers() const { return layerStack.hasBase(); }
    bool isLayerActive() const { return layerStack.hasBase() && layerStack.getActiveIdx() >= 0; }

    int nbVerts = 0;
    int nbFaces = 0;
    int nbEdges = 0;
    bool isDirty = true;
    bool isVertexDirty = false;
    bool isColorDirty = false;
    bool isMaterialDirty = false;
    bool isFaceGroupDirty = false;
    bool isTopologyDirty = false;
    uint32_t dirtyVertMin = 0;
    uint32_t dirtyVertMax = 0;

    void initFaceGroups();
    uint32_t getNextFreeGroupID() const;
    void setFaceGroup(uint32_t faceIdx, uint32_t gid);
    void initEdges();
    void initTopology();
    void initDynamicMode(bool force = false);
    void updateDynamicCSR();
    void convertToStatic();
    std::vector<uint32_t> triangulateQuadsInRegion(const std::vector<uint32_t>& iFaces);
    void reAllocateArrays(int nbAdd);
    void computeRingVertices(uint32_t iVert);
    uint32_t addNbVert(int count = 1);
    uint32_t addNbFace(int count = 1);
    std::vector<uint32_t> getVerticesFromFaces(const std::vector<uint32_t>& iFaces) const;
    std::vector<uint32_t> getFacesFromVertices(const std::vector<uint32_t>& iVerts) const;
    std::vector<uint32_t> expandsFaces(const std::vector<uint32_t>& iFaces, int ringDepth = 1) const;
    std::vector<uint32_t> expandsVertices(const std::vector<uint32_t>& iVerts, int ringDepth = 1) const;

    uint32_t getTagFlag() const {
        static uint32_t g_tagFlag = 0;
        return ++g_tagFlag;
    }
    bool hasOnlyTriangles() const;
    int getNbEdges() const { return nbEdges; }
    int getNbQuads() const;
    int getNbTriangles() const;

    // SGL/OBJ migration properties
    bool visibleV1 = true;
    bool visibleV2 = true;
    glm::vec3 center{0.0f};
    float scale = 1.0f;
    float symmetryOffset = 0.0f;
    std::vector<float>    texCoords;     // nbTexCoords * 2
    std::vector<uint32_t> facesTexCoord;  // nbFaces * 4
    bool hasUV = false;

    float getSymmetryOffset() const { return symmetryOffset; }
    void setSymmetryOffset(float val) { symmetryOffset = val; }

    float computeLocalRadius() const;
    void invalidateLocalRadius() const { m_localRadiusDirty = true; }
    glm::vec3 getSymmetryOriginForAxis(int axisIndex, SymmetryMode mode) const;
    glm::vec3 getSymmetryNormalForAxis(int axisIndex, SymmetryMode mode) const;
    virtual void flip(int axisIndex);
    virtual void mirror(int axisIndex, bool positiveToNegative, SymmetryMode mode);

private:
    mutable float m_cachedLocalRadius = -1.0f;
    mutable bool m_localRadiusDirty = true;

public:

    uint32_t m_id = 0;
    std::string outlinerName = "";
    uint32_t getID() const { return m_id; }
    void setID(uint32_t id) { m_id = id; }

    Mesh() {
        static uint32_t s_idCounter = 0;
        m_id = ++s_idCounter;
        outlinerName = "Mesh " + std::to_string(m_id);
    }

    Mesh(const Mesh& other) {
        isArmature = false;
        armatureGraph = nullptr;
        verts = other.verts;
        normals = other.normals;
        colors = other.colors;
        materials = other.materials;
        faceNormals = other.faceNormals;
        faceBoxes = other.faceBoxes;
        faceCenters = other.faceCenters;
        vertProxy = other.vertProxy;
        faces = other.faces;
        faceGroups = other.faceGroups;
        vrfStartCount = other.vrfStartCount;
        vertRingFace = other.vertRingFace;
        vrvStartCount = other.vrvStartCount;
        vertRingVert = other.vertRingVert;
        vertOnEdge = other.vertOnEdge;
        vertVisible = other.vertVisible;
        edges = other.edges;
        faceEdges = other.faceEdges;
        vertTagFlags = other.vertTagFlags;
        nbVerts = other.nbVerts;
        nbFaces = other.nbFaces;
        nbEdges = other.nbEdges;
        isDirty = other.isDirty;
        isVertexDirty = other.isVertexDirty;
        isColorDirty = other.isColorDirty;
        isMaterialDirty = other.isMaterialDirty;
        isFaceGroupDirty = other.isFaceGroupDirty;
        isTopologyDirty = other.isTopologyDirty;
        dirtyVertMin = other.dirtyVertMin;
        dirtyVertMax = other.dirtyVertMax;
        visibleV1 = other.visibleV1;
        visibleV2 = other.visibleV2;
        center = other.center;
        scale = other.scale;
        texCoords = other.texCoords;
        facesTexCoord = other.facesTexCoord;
        hasUV = other.hasUV;
        m_id = other.m_id;
        outlinerName = other.outlinerName;
        matrix = other.matrix;
        editMatrix = other.editMatrix;
        layerStack = other.layerStack;
    }

    virtual ~Mesh() = default;

    // Initialization from JS-managed arrays

    void allocate(int nbV, int nbF, int nbRF, int nbRV);
    void postInit();
    void updateAfterLayerBake();

    // Pointer getters for WASM / bindings


    Octree* getOctree() { return &octree; }

    void setDirty(bool dirty) { isDirty = dirty; }
    bool getDirty() const { return isDirty; }


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
    float computeWorldStep(int resolution) const;
    void bakeScale();
};

