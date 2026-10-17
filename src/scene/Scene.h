#pragma once
#include <vector>
#include <memory>
#include "mesh/Mesh.h"
#include "scene/Camera.h"
#include "render/ReferenceImage.h"

#include "scene/CameraBookmark.h"
#include "scene/LightSource.h"
#include "editing/MeasurementTypes.h"

class SculptManager;


struct MeshState {
    std::vector<float> verts;
    std::vector<float> colors;
    std::vector<float> materials;
    std::vector<uint32_t> faces;
    std::vector<uint32_t> faceGroups;
    std::vector<uint32_t> vrfStartCount;
    std::vector<uint32_t> vertRingFace;
    std::vector<uint32_t> vrvStartCount;
    std::vector<uint32_t> vertRingVert;
    std::vector<uint8_t> vertOnEdge;
    std::vector<uint8_t> vertVisible;
    std::vector<uint8_t> faceVisible;
    uint64_t vertsGeneration = 0;
    int nbVerts = 0;
    int nbFaces = 0;

    glm::mat4 matrix = glm::mat4(1.0f);

    LayerStack layerStack;

    uint32_t id = 0;
    std::string outlinerName;
    bool visibleV1 = true;
    bool visibleV2 = true;
    bool isArmature = false;
    bool isTube = false;
    std::unique_ptr<TubeData> tubeData;
    bool isPrimitive = false;
    std::unique_ptr<PrimitiveData> primitiveData;
    bool symEnabled = false;
    bool symX = true;
    bool symY = false;
    bool symZ = false;
    int symMode = 0;

    MeshState() = default;
    MeshState(const MeshState& o) {
        verts = o.verts;
        colors = o.colors;
        materials = o.materials;
        faces = o.faces;
        faceGroups = o.faceGroups;
        vrfStartCount = o.vrfStartCount;
        vertRingFace = o.vertRingFace;
        vrvStartCount = o.vrvStartCount;
        vertRingVert = o.vertRingVert;
        vertOnEdge = o.vertOnEdge;
        vertVisible = o.vertVisible;
        faceVisible = o.faceVisible;
        vertsGeneration = o.vertsGeneration;
        nbVerts = o.nbVerts;
        nbFaces = o.nbFaces;
        matrix = o.matrix;
        layerStack = o.layerStack;
        id = o.id;
        outlinerName = o.outlinerName;
        visibleV1 = o.visibleV1;
        visibleV2 = o.visibleV2;
        isArmature = o.isArmature;
        isTube = o.isTube;
        if (o.tubeData) {
            tubeData = std::make_unique<TubeData>(*o.tubeData);
        }
        isPrimitive = o.isPrimitive;
        if (o.primitiveData) {
            primitiveData = std::make_unique<PrimitiveData>(*o.primitiveData);
        }
        symEnabled = o.symEnabled;
        symX = o.symX;
        symY = o.symY;
        symZ = o.symZ;
        symMode = o.symMode;
    }
    MeshState(MeshState&&) noexcept = default;
    MeshState& operator=(const MeshState& o) {
        if (this != &o) {
            verts = o.verts;
            colors = o.colors;
            materials = o.materials;
            faces = o.faces;
            faceGroups = o.faceGroups;
            vrfStartCount = o.vrfStartCount;
            vertRingFace = o.vertRingFace;
            vrvStartCount = o.vrvStartCount;
            vertRingVert = o.vertRingVert;
            vertOnEdge = o.vertOnEdge;
            vertVisible = o.vertVisible;
            faceVisible = o.faceVisible;
            vertsGeneration = o.vertsGeneration;
            nbVerts = o.nbVerts;
            nbFaces = o.nbFaces;
            matrix = o.matrix;
            layerStack = o.layerStack;
            id = o.id;
            outlinerName = o.outlinerName;
            visibleV1 = o.visibleV1;
            visibleV2 = o.visibleV2;
            isArmature = o.isArmature;
            isTube = o.isTube;
            if (o.tubeData) {
                tubeData = std::make_unique<TubeData>(*o.tubeData);
            } else {
                tubeData = nullptr;
            }
            isPrimitive = o.isPrimitive;
            if (o.primitiveData) {
                primitiveData = std::make_unique<PrimitiveData>(*o.primitiveData);
            } else {
                primitiveData = nullptr;
            }
            symEnabled = o.symEnabled;
            symX = o.symX;
            symY = o.symY;
            symZ = o.symZ;
            symMode = o.symMode;
        }
        return *this;
    }
    MeshState& operator=(MeshState&&) noexcept = default;
};

struct HistoryState {
    int selectedMeshIdx = -1;
    std::vector<int> selectedMeshIndices;
    std::vector<MeshState> meshes;
    std::vector<MeasurementSegment> measureSegments;
    std::vector<MeasurementSegment> dividerSegments;
};

class Scene {
private:
    std::vector<Mesh*> m_meshes;
    std::vector<Mesh*> m_selectedMeshes;
    int m_selectedIdx = -1;
    Camera m_camera;
    SculptManager* m_sculptManager = nullptr;

    friend class UndoManager;
    friend class TimelapsePlayer;
    friend class GuiManager;

public:
    void setSculptManager(SculptManager* sm) { m_sculptManager = sm; }
    SculptManager* getSculptManager() const { return m_sculptManager; }

    HistoryState saveCurrentState(bool includeTopology = false) const;
    void restoreState(const HistoryState& state);
    Mesh* getMeshById(uint32_t id) const;
    enum class SplitMode {
        OFF = 0,
        MIRROR = 1,
        INDEPENDENT = 2
    };

    Scene();
    ~Scene();

    void addMesh(Mesh* m);
    void removeMesh(Mesh* m);
    void deleteMesh(Mesh* m);
    void deleteSelection();
    void replaceMesh(Mesh* oldMesh, Mesh* newMesh);
    void clear();
    const std::vector<Mesh*>& getMeshes() const;
    void selectMesh(Mesh* m);
    Mesh* getSelected() const;
    int getSelectedIdx() const;
    void setSelectedIdx(int idx);

    const std::vector<Mesh*>& getSelectedMeshes() const;
    void setOrUnsetMesh(Mesh* target, bool multiSelect);
    bool isMeshSelected(Mesh* target) const;

    void addSphere();
    void addGeosphere();
    void addCube();
    void addCylinder();
    void addCone();
    void addTorus();
    void addPlane();
    void addCapsule();
    void addPrimitiveAtMask(const std::string& type, bool useSym, bool symX, bool symY, bool symZ);
    void duplicateSelection();
    void mergeSelection();
    void clearScene();

    uint32_t getNextFreePolyGroupID() const;

    Camera& getCamera();
    const Camera& getCamera() const;
    Camera* getCameraPtr();
    const Camera* getCameraPtr() const;
    Camera* getCameraByIndex(int idx);
    const Camera* getCameraByIndex(int idx) const;

    SplitMode getSplitMode() const { return m_splitMode; }
    void setSplitMode(SplitMode mode);
    
    int getActiveViewport() const { return m_activeViewport; }
    void setActiveViewport(int vp) { m_activeViewport = vp; }

    bool getSplitShowInactiveCursor() const { return m_splitShowInactiveCursor; }
    void setSplitShowInactiveCursor(bool val) { m_splitShowInactiveCursor = val; }
    
    std::shared_ptr<Camera> getCameraRight() { return m_splitMode == SplitMode::INDEPENDENT ? m_cameraRight : nullptr; }
    std::shared_ptr<const Camera> getCameraRight() const { return m_splitMode == SplitMode::INDEPENDENT ? m_cameraRight : nullptr; }

    void loadDefaultSphere();

    void pushHistoryState();
    void undo();
    void redo();
    void clearHistory();
    bool canUndo() const;
    bool canRedo() const;

    // Reference Images
    std::vector<ReferenceImage>& getReferenceImages() { return m_refImages; }
    const std::vector<ReferenceImage>& getReferenceImages() const { return m_refImages; }
    void addReferenceImage(const std::string& path);
    bool updateReferenceImagePath(size_t index, const std::string& newPath);
    bool reloadReferenceImage(size_t index);
    void removeReferenceImage(size_t index);
    void setAllReferenceImagesVisible(bool vis);
    bool areAnyReferenceImagesVisible() const;
    void toggleAllReferenceImagesVisible();

    // Light Sources
    std::vector<LightSource>& getLights() { return m_lights; }
    const std::vector<LightSource>& getLights() const { return m_lights; }
    void addLight(LightType type = LightType::DIRECTIONAL);
    void addLight(const LightSource& light);
    void removeLight(size_t index);
    void rotateAllLights(float deltaAngleRad);

    bool getVoxelPreview() const { return m_voxelPreview; }
    float getVoxelStep() const { return m_voxelStep; }
    const std::vector<Mesh*>& getVoxelMeshes() const { return m_voxelMeshes; }
    void updateVoxelPreview(float step, const std::vector<Mesh*>& meshes);

    // Solo Mode
    bool isSoloActive() const { return m_soloMeshId != 0; }
    uint32_t getSoloMeshId() const { return m_soloMeshId; }
    void toggleSolo(Mesh* mesh);
    bool isMeshRenderVisible(const Mesh* mesh, int viewport = 0) const;
    bool isRefImageRenderVisible(const ReferenceImage& img, int viewport = 0) const;

    // Camera Bookmarks
    std::vector<CameraBookmark>& getCameraBookmarks() { return m_cameraBookmarks; }
    const std::vector<CameraBookmark>& getCameraBookmarks() const { return m_cameraBookmarks; }
    int getActiveCameraBookmarkIdx() const { return m_activeCameraBookmarkIdx; }
    void setActiveCameraBookmarkIdx(int idx) { m_activeCameraBookmarkIdx = idx; }

    CameraBookmark captureCurrentAsBookmark(const std::string& name = "") const;
    void applyBookmark(int idx, bool animate = true);
    void addBookmark(const CameraBookmark& bm);
    void removeBookmark(int idx);

    // Modification tracking
    bool isModified() const { return m_isModified; }
    void setModified(bool mod = true) { m_isModified = mod; }

private:
    std::vector<LightSource> m_lights;
    std::vector<ReferenceImage> m_refImages;
    std::vector<CameraBookmark> m_cameraBookmarks;
    int m_activeCameraBookmarkIdx = -1;
    SplitMode m_splitMode = SplitMode::OFF;
    int m_activeViewport = 0;
    bool m_splitShowInactiveCursor = false;
    std::shared_ptr<Camera> m_cameraRight;

    bool m_voxelPreview = false;
    float m_voxelStep = 0.0f;
    std::vector<Mesh*> m_voxelMeshes;

    uint32_t m_soloMeshId = 0;
    bool m_isModified = false;
};
