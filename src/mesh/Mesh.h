#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include <memory>
#include <GLES3/gl3.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "mesh/Octree.h"
#include "mesh/Layer.h"
#include "mesh/SubdivCache.h"

#include "sculpt/ArmatureGraph.h"
#include "sculpt/TubeData.h"
#include "sculpt/PrimitiveData.h"

#include "common/Enums.h"

class Camera;

struct DynamicSubdivSettings {
    bool enabled = false;
    int  smoothLevel = 2;    // 1 to 6 (SmoothSubdiv)
    int  flatLevel = 0;      // 0 to 4 (FlatSubdiv)
    int  creaseLevel = 2;    // 1 to 15 (CreaseLvl in ZBrush)
    bool showCage = false;   // Render base cage wireframe over smooth mesh (Default: OFF)
    float cageAlpha = 0.6f;  // Cage wireframe opacity
};

class Mesh {
public:
    // Armature support
    bool isArmature = false;
    std::unique_ptr<ArmatureGraph> armatureGraph;

    // Procedural Tube support
    bool isTube = false;
    std::unique_ptr<TubeData> tubeData;

    // Procedural Primitive support
    bool isPrimitive = false;
    std::unique_ptr<PrimitiveData> primitiveData;

    // Per-mesh normal map (loaded from glTF, never a ReferenceImage)
    GLuint normalMapTexId = 0;

    // Geometry
    std::vector<float>    verts;        // nbVerts * 3
    std::vector<float>    normals;      // nbVerts * 3
    bool                  hasCustomNormals = false; // Preserved imported normals (e.g. from glTF)
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
    std::vector<uint8_t>  edgeCreases;     // nbEdges (1 = creased, 0 = smooth)

    // Dynamic Subdivision (ZBrush style)
    DynamicSubdivSettings dynamicSettings;
    std::unique_ptr<Mesh> dynamicCacheMesh;
    std::vector<std::unique_ptr<Mesh>> dynamicLevels;
    std::vector<Subdivision::SubdivLevelCache> dynamicCaches;
    bool isDynamicDirty = true;
    bool isDynamicTopologyBuilt = false;

    // Octree
    Octree octree;

    // Layers
    LayerStack layerStack;
    bool hasLayers() const { return layerStack.hasBase() && layerStack.count() > 0; }
    bool isLayerActive() const { return layerStack.hasBase() && layerStack.getActiveIdx() >= 0; }
    bool isLayerRecording() const { return layerStack.hasBase() && layerStack.count() > 0 && layerStack.isRecording(); }

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

    void initFaceGroups(uint32_t defaultGid = 0);
    uint32_t getNextFreeGroupID() const;
    void setFaceGroup(uint32_t faceIdx, uint32_t gid);
    void initEdges();
    void initTopology();
    uint32_t getTagFlag() const {
        static uint32_t g_tagFlag = 0;
        return ++g_tagFlag;
    }
    bool hasOnlyTriangles() const;
    int getNbEdges() const { return nbEdges; }
    int getNbQuads() const;
    int getNbTriangles() const;

    // Crease and Dynamic Subdivision methods
    void initCreases();
    void creaseEdge(uint32_t edgeIdx, bool creased);
    bool isEdgeCreased(uint32_t edgeIdx) const;
    void creaseByAngle(float angleThresholdDegrees);
    void creaseByPolyGroups();
    void uncreaseAll();
    bool hasCreases() const;
    int getNbCreasedEdges() const;

    void updateDynamicMesh();
    void updateDynamicGeometryOnly();
    void rebuildDynamicTopology();
    void applyDynamicSubdivision();
    void invalidateDynamicSubdiv() {
        isDynamicDirty = true;
        isDynamicTopologyBuilt = false;
        dynamicCaches.clear();
        isDirty = true;
    }
    Mesh* getRenderMesh() { return (dynamicSettings.enabled && dynamicCacheMesh) ? dynamicCacheMesh.get() : this; }
    const Mesh* getRenderMesh() const { return (dynamicSettings.enabled && dynamicCacheMesh) ? dynamicCacheMesh.get() : this; }

    // SGL/OBJ migration properties
    bool visibleV1 = true;
    bool visibleV2 = true;
    
    // Per-mesh symmetry state
    bool symEnabled = false;
    bool symX = true;
    bool symY = false;
    bool symZ = false;
    int symMode = 0; // 0 = Local, 1 = World

    glm::vec3 center{0.0f};
    float scale = 1.0f;
    float symmetryOffset = 0.0f;
    std::vector<float>    texCoords;     // nbTexCoords * 2
    std::vector<uint32_t> facesTexCoord;  // nbFaces * 4
    std::vector<float>    tangents;      // nbVerts * 4 (XYZW)
    std::vector<float>    uvFlat;        // nbVerts * 2 (vertex-indexed for GPU)
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

    void copyDataFrom(const Mesh& other) {
        isArmature = false;
        armatureGraph = nullptr;
        isTube = other.isTube;
        if (other.tubeData) {
            tubeData = std::make_unique<TubeData>(*other.tubeData);
        } else {
            tubeData = nullptr;
        }
        isPrimitive = other.isPrimitive;
        if (other.primitiveData) {
            primitiveData = std::make_unique<PrimitiveData>(*other.primitiveData);
        } else {
            primitiveData = nullptr;
        }
        normalMapTexId = other.normalMapTexId;
        verts = other.verts;
        normals = other.normals;
        hasCustomNormals = other.hasCustomNormals;
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
        faceVisible = other.faceVisible;
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
        symEnabled = other.symEnabled;
        symX = other.symX;
        symY = other.symY;
        symZ = other.symZ;
        symMode = other.symMode;
        center = other.center;
        scale = other.scale;
        texCoords = other.texCoords;
        facesTexCoord = other.facesTexCoord;
        tangents = other.tangents;
        uvFlat = other.uvFlat;
        hasUV = other.hasUV;
        m_id = other.m_id;
        outlinerName = other.outlinerName;
        matrix = other.matrix;
        editMatrix = other.editMatrix;
        layerStack = other.layerStack;
        edgeCreases = other.edgeCreases;
        dynamicSettings = other.dynamicSettings;
        isDynamicDirty = true;
        isDynamicTopologyBuilt = false;
    }

    Mesh(const Mesh& other) {
        copyDataFrom(other);
    }

    Mesh& operator=(const Mesh& other) {
        if (this != &other) {
            copyDataFrom(other);
        }
        return *this;
    }

    virtual ~Mesh() = default;

    // Initialization from JS-managed arrays

    void allocate(int nbV, int nbF, int nbRF, int nbRV);
    void postInit(bool buildOctree = true);
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

    uint64_t getVertsGeneration() const { return m_vertsGeneration; }
    void bumpVertsGeneration() { m_vertsGeneration++; }

    void initTexCoordsDataFromOBJData(const std::vector<float>& uvAr, const std::vector<uint32_t>& uvfArOrig);
    void updateMatrices(const Camera& camera);
    void computeBbox(float* outBbox) const;
    float computeWorldStep(int resolution) const;
    void bakeScale();
    void scaleFaceNormalsBoxesCentersAndOctree(float scaleX, float scaleY, float scaleZ);

private:
    uint64_t m_vertsGeneration = 0;
};


