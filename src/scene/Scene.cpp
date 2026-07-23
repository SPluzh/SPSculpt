#include "scene/Scene.h"
#include <algorithm>

#include <cstring>

Scene::Scene() {}

Scene::~Scene() {
    clear();
}

void Scene::clear() {
    for (auto* m : m_meshes) {
        delete m;
    }
    m_meshes.clear();
    m_selectedIdx = -1;
    clearHistory();

    for (auto& img : m_refImages) {
        if (img.texId != 0) {
            glDeleteTextures(1, &img.texId);
        }
    }
    m_refImages.clear();
}

void Scene::addReferenceImage(const std::string& path) {
    GLuint texId = loadTextureFromFile(path);
    if (texId != 0) {
        ReferenceImage img;
        img.path = path;
        img.texId = texId;
        img.opacity = 0.5f;
        img.scale = 1.0f;
        img.offsetX = 0.0f;
        img.offsetY = 0.0f;
        img.visible = true;
        img.pinned2D = true;
        m_refImages.push_back(img);
    }
}

void Scene::removeReferenceImage(size_t index) {
    if (index < m_refImages.size()) {
        if (m_refImages[index].texId != 0) {
            glDeleteTextures(1, &m_refImages[index].texId);
        }
        m_refImages.erase(m_refImages.begin() + index);
    }
}

HistoryState Scene::saveCurrentState() const {
    HistoryState hs;
    hs.selectedMeshIdx = m_selectedIdx;
    for (auto* m : m_meshes) {
        MeshState ms;
        ms.verts = m->verts;
        ms.colors = m->colors;
        ms.faces = m->faces;
        ms.vrfStartCount = m->vrfStartCount;
        ms.vertRingFace = m->vertRingFace;
        ms.vrvStartCount = m->vrvStartCount;
        ms.vertRingVert = m->vertRingVert;
        ms.vertOnEdge = m->vertOnEdge;
        ms.vertVisible = m->vertVisible;
        ms.nbVerts = m->nbVerts;
        ms.nbFaces = m->nbFaces;
        
        ms.shaderType = m->shaderType;
        ms.matcapIdx = m->matcapIdx;
        std::memcpy(ms.albedo, m->albedo, 3 * sizeof(float));
        ms.roughness = m->roughness;
        ms.metallic = m->metallic;
        ms.alpha = m->alpha;
        ms.showWireframe = m->showWireframe;
        ms.flatShading = m->flatShading;
        ms.matrix = m->matrix;
        
        hs.meshes.push_back(ms);
    }
    return hs;
}

void Scene::restoreState(const HistoryState& hs) {
    for (auto* m : m_meshes) {
        delete m;
    }
    m_meshes.clear();

    for (const auto& ms : hs.meshes) {
        Mesh* m = new Mesh();
        m->verts = ms.verts;
        m->colors = ms.colors;
        m->faces = ms.faces;
        m->vrfStartCount = ms.vrfStartCount;
        m->vertRingFace = ms.vertRingFace;
        m->vrvStartCount = ms.vrvStartCount;
        m->vertRingVert = ms.vertRingVert;
        m->vertOnEdge = ms.vertOnEdge;
        m->vertVisible = ms.vertVisible;
        m->nbVerts = ms.nbVerts;
        m->nbFaces = ms.nbFaces;

        m->shaderType = ms.shaderType;
        m->matcapIdx = ms.matcapIdx;
        std::memcpy(m->albedo, ms.albedo, 3 * sizeof(float));
        m->roughness = ms.roughness;
        m->metallic = ms.metallic;
        m->alpha = ms.alpha;
        m->showWireframe = ms.showWireframe;
        m->flatShading = ms.flatShading;
        m->matrix = ms.matrix;

        m->postInit();
        m_meshes.push_back(m);
    }
    m_selectedIdx = hs.selectedMeshIdx;
}

void Scene::pushHistoryState() {
    m_redoStack.clear();
    m_undoStack.push_back(saveCurrentState());
    if (m_undoStack.size() > m_maxHistoryStates) {
        m_undoStack.erase(m_undoStack.begin());
    }
}

void Scene::undo() {
    if (m_undoStack.empty()) return;
    m_redoStack.push_back(saveCurrentState());
    
    HistoryState prevState = m_undoStack.back();
    m_undoStack.pop_back();
    restoreState(prevState);
}

void Scene::redo() {
    if (m_redoStack.empty()) return;
    m_undoStack.push_back(saveCurrentState());
    
    HistoryState nextState = m_redoStack.back();
    m_redoStack.pop_back();
    restoreState(nextState);
}

void Scene::clearHistory() {
    m_undoStack.clear();
    m_redoStack.clear();
}

void Scene::addMesh(Mesh* m) {
    if (std::find(m_meshes.begin(), m_meshes.end(), m) == m_meshes.end()) {
        m_meshes.push_back(m);
    }
}

void Scene::removeMesh(Mesh* m) {
    auto it = std::find(m_meshes.begin(), m_meshes.end(), m);
    if (it != m_meshes.end()) {
        int idx = std::distance(m_meshes.begin(), it);
        m_meshes.erase(it);
        if (m_selectedIdx == idx) {
            m_selectedIdx = m_meshes.empty() ? -1 : 0;
        } else if (m_selectedIdx > idx) {
            m_selectedIdx--;
        }
    }
}

const std::vector<Mesh*>& Scene::getMeshes() const {
    return m_meshes;
}

void Scene::selectMesh(Mesh* m) {
    auto it = std::find(m_meshes.begin(), m_meshes.end(), m);
    if (it != m_meshes.end()) {
        m_selectedIdx = std::distance(m_meshes.begin(), it);
    } else {
        m_selectedIdx = -1;
    }
}

Mesh* Scene::getSelected() const {
    if (m_selectedIdx >= 0 && m_selectedIdx < (int)m_meshes.size()) {
        return m_meshes[m_selectedIdx];
    }
    return nullptr;
}

int Scene::getSelectedIdx() const {
    return m_selectedIdx;
}

void Scene::setSelectedIdx(int idx) {
    if (idx >= -1 && idx < (int)m_meshes.size()) {
        m_selectedIdx = idx;
    }
}

Camera& Scene::getCamera() {
    if (m_splitMode == SplitMode::INDEPENDENT && m_activeViewport == 1 && m_cameraRight) {
        return *m_cameraRight;
    }
    return m_camera;
}

const Camera& Scene::getCamera() const {
    if (m_splitMode == SplitMode::INDEPENDENT && m_activeViewport == 1 && m_cameraRight) {
        return *m_cameraRight;
    }
    return m_camera;
}

Camera* Scene::getCameraPtr() {
    if (m_splitMode == SplitMode::INDEPENDENT && m_activeViewport == 1 && m_cameraRight) {
        return m_cameraRight.get();
    }
    return &m_camera;
}

const Camera* Scene::getCameraPtr() const {
    if (m_splitMode == SplitMode::INDEPENDENT && m_activeViewport == 1 && m_cameraRight) {
        return m_cameraRight.get();
    }
    return &m_camera;
}

Camera* Scene::getCameraByIndex(int idx) {
    if (idx == 1 && m_cameraRight) {
        return m_cameraRight.get();
    }
    return &m_camera;
}

const Camera* Scene::getCameraByIndex(int idx) const {
    if (idx == 1 && m_cameraRight) {
        return m_cameraRight.get();
    }
    return &m_camera;
}

void Scene::setSplitMode(SplitMode mode) {
    m_splitMode = mode;
    if (mode == SplitMode::INDEPENDENT && !m_cameraRight) {
        m_cameraRight = std::make_shared<Camera>(m_camera);
        m_cameraRight->toggleViewRight();
    }
    m_activeViewport = 0;
}


#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

#include "mesh/Topology.h"
#include <cmath>

static void generateUVSphere(
    float radius, int rings, int sectors,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
) {
    float const R = 1.0f / (float)(rings - 1);
    float const S = 1.0f / (float)(sectors - 1);

    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < sectors; ++s) {
            float const y = radius * std::sin(-M_PI_2 + M_PI * r * R);
            float const x = radius * std::cos(2 * M_PI * s * S) * std::sin(M_PI * r * R);
            float const z = radius * std::sin(2 * M_PI * s * S) * std::sin(M_PI * r * R);

            vertices.push_back(x);
            vertices.push_back(y);
            vertices.push_back(z);

            colors.push_back(0.72f);
            colors.push_back(0.52f);
            colors.push_back(0.45f);

            normals.push_back(x / radius);
            normals.push_back(y / radius);
            normals.push_back(z / radius);
        }
    }

    for (int r = 0; r < rings - 1; ++r) {
        for (int s = 0; s < sectors - 1; ++s) {
            uint32_t v0 = r * sectors + s;
            uint32_t v1 = r * sectors + (s + 1);
            uint32_t v2 = (r + 1) * sectors + (s + 1);
            uint32_t v3 = (r + 1) * sectors + s;

            faces.push_back(v0);
            faces.push_back(v1);
            faces.push_back(v2);
            faces.push_back(v3);
        }
    }
}

void Scene::loadDefaultSphere() {
    std::vector<float> vertices;
    std::vector<uint32_t> faces;
    std::vector<float> colors;
    std::vector<float> normals;

    generateUVSphere(50.0f, 100, 100, vertices, faces, colors, normals);
    int nbVerts = vertices.size() / 3;
    int nbFaces = faces.size() / 4;

    std::vector<uint32_t> vrvStartCount;
    std::vector<uint32_t> vertRingVert;
    std::vector<uint32_t> vrfStartCount;
    std::vector<uint32_t> vertRingFace;
    std::vector<uint8_t> vertOnEdge;
    computeTopology(nbVerts, faces.data(), nbFaces, vrfStartCount, vertRingFace, vrvStartCount, vertRingVert, vertOnEdge);

    Mesh* mesh = new Mesh();
    mesh->verts = vertices;
    mesh->faces = faces;
    mesh->colors = colors;
    mesh->normals = normals;
    mesh->nbVerts = nbVerts;
    mesh->nbFaces = nbFaces;
    mesh->vrfStartCount = vrfStartCount;
    mesh->vertRingFace = vertRingFace;
    mesh->vrvStartCount = vrvStartCount;
    mesh->vertRingVert = vertRingVert;
    mesh->vertOnEdge = vertOnEdge;
    mesh->postInit();

    addMesh(mesh);
    selectMesh(mesh);

    m_camera.resetView();
}
