#pragma once
#include <vector>
#include "mesh/Mesh.h"
#include "scene/Camera.h"
#include "render/ReferenceImage.h"

struct MeshState {
    std::vector<float> verts;
    std::vector<float> colors;
    std::vector<uint32_t> faces;
    std::vector<uint32_t> vrfStartCount;
    std::vector<uint32_t> vertRingFace;
    std::vector<uint32_t> vrvStartCount;
    std::vector<uint32_t> vertRingVert;
    std::vector<uint8_t> vertOnEdge;
    std::vector<uint8_t> vertVisible;
    int nbVerts = 0;
    int nbFaces = 0;

    int shaderType = 0;
    int matcapIdx = 0;
    float albedo[3] = {0.72f, 0.52f, 0.45f};
    float roughness = 0.5f;
    float metallic = 0.0f;
    float alpha = 1.0f;
    bool showWireframe = false;
    bool flatShading = false;
};

struct HistoryState {
    int selectedMeshIdx = -1;
    std::vector<MeshState> meshes;
};

class Scene {
private:
    std::vector<Mesh*> m_meshes;
    int m_selectedIdx = -1;
    Camera m_camera;

    std::vector<HistoryState> m_undoStack;
    std::vector<HistoryState> m_redoStack;
    size_t m_maxHistoryStates = 30;

    HistoryState saveCurrentState() const;
    void restoreState(const HistoryState& state);

public:
    Scene();
    ~Scene();

    void addMesh(Mesh* m);
    void removeMesh(Mesh* m);
    void clear();
    const std::vector<Mesh*>& getMeshes() const;
    void selectMesh(Mesh* m);
    Mesh* getSelected() const;
    int getSelectedIdx() const;
    void setSelectedIdx(int idx);

    Camera& getCamera();
    const Camera& getCamera() const;
    Camera* getCameraPtr() { return &m_camera; }

    void loadDefaultSphere();

    void pushHistoryState();
    void undo();
    void redo();
    void clearHistory();

    // Reference Images
    std::vector<ReferenceImage>& getReferenceImages() { return m_refImages; }
    const std::vector<ReferenceImage>& getReferenceImages() const { return m_refImages; }
    void addReferenceImage(const std::string& path);
    void removeReferenceImage(size_t index);

private:
    std::vector<ReferenceImage> m_refImages;
};
