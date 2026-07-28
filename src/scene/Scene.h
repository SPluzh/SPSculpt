#pragma once
#include <vector>
#include <memory>
#include "mesh/Mesh.h"
#include "scene/Camera.h"
#include "render/ReferenceImage.h"

#include "scene/LightSource.h"

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
    int nbVerts = 0;
    int nbFaces = 0;

    glm::mat4 matrix = glm::mat4(1.0f);

    uint32_t id = 0;
    std::string outlinerName;
    bool visibleV1 = true;
    bool visibleV2 = true;
};

struct HistoryState {
    int selectedMeshIdx = -1;
    std::vector<int> selectedMeshIndices;
    std::vector<MeshState> meshes;
};

class Scene {
private:
    std::vector<Mesh*> m_meshes;
    std::vector<Mesh*> m_selectedMeshes;
    int m_selectedIdx = -1;
    Camera m_camera;

    friend class UndoManager;
    HistoryState saveCurrentState() const;
    void restoreState(const HistoryState& state);

public:
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
    void addTorus();
    void addPrimitiveAtMask(const std::string& type, bool useSym, bool symX, bool symY, bool symZ);
    void duplicateSelection();
    void mergeSelection();
    void clearScene();

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
    void removeReferenceImage(size_t index);

    // Light Sources
    std::vector<LightSource>& getLights() { return m_lights; }
    const std::vector<LightSource>& getLights() const { return m_lights; }
    void addLight(LightType type = LightType::DIRECTIONAL);
    void addLight(const LightSource& light);
    void removeLight(size_t index);

    bool getVoxelPreview() const { return m_voxelPreview; }
    float getVoxelStep() const { return m_voxelStep; }
    const std::vector<Mesh*>& getVoxelMeshes() const { return m_voxelMeshes; }
    void updateVoxelPreview(float step, const std::vector<Mesh*>& meshes);

    // Solo Mode
    bool isSoloActive() const { return m_soloMeshId != 0; }
    uint32_t getSoloMeshId() const { return m_soloMeshId; }
    void toggleSolo(Mesh* mesh);
    bool isMeshRenderVisible(const Mesh* mesh, int viewport = 0) const;

private:
    std::vector<LightSource> m_lights;
    std::vector<ReferenceImage> m_refImages;
    SplitMode m_splitMode = SplitMode::OFF;
    int m_activeViewport = 0;
    bool m_splitShowInactiveCursor = false;
    std::shared_ptr<Camera> m_cameraRight;

    bool m_voxelPreview = false;
    float m_voxelStep = 0.0f;
    std::vector<Mesh*> m_voxelMeshes;

    uint32_t m_soloMeshId = 0;
};
