#include "scene/Scene.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <utility>
#include <glm/gtc/matrix_transform.hpp>
#include "files/MeshUtils.h"
#include "common/Constants.h"

Scene::Scene() {
    LightSource mainLight;
    mainLight.name = "Main Light";
    mainLight.type = LightType::DIRECTIONAL;
    mainLight.direction = glm::normalize(glm::vec3(-0.5f, -0.8f, -1.0f));
    mainLight.position = glm::vec3(0.0f, 10.0f, 10.0f);
    mainLight.color = glm::vec3(1.0f, 1.0f, 1.0f);
    mainLight.intensity = 1.0f;
    mainLight.castShadow = true;
    mainLight.enabled = true;
    m_lights.push_back(mainLight);
}

void Scene::addLight(LightType type) {
    LightSource L;
    L.type = type;
    L.name = "Light " + std::to_string(m_lights.size() + 1);
    if (type == LightType::DIRECTIONAL) {
        L.direction = glm::normalize(glm::vec3(-0.3f, -1.0f, -0.5f));
    } else {
        L.position = glm::vec3(0.0f, 5.0f, 5.0f);
    }
    m_lights.push_back(L);
}

void Scene::addLight(const LightSource& light) {
    m_lights.push_back(light);
}

void Scene::removeLight(size_t index) {
    if (index < m_lights.size()) {
        m_lights.erase(m_lights.begin() + index);
    }
}

Scene::~Scene() {
    clear();
}

void Scene::clear() {
    for (auto* m : m_meshes) {
        delete m;
    }
    m_meshes.clear();
    m_selectedMeshes.clear();
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
    for (auto* selMesh : m_selectedMeshes) {
        auto it = std::find(m_meshes.begin(), m_meshes.end(), selMesh);
        if (it != m_meshes.end()) {
            hs.selectedMeshIndices.push_back((int)std::distance(m_meshes.begin(), it));
        }
    }
    for (auto* m : m_meshes) {
        MeshState ms;
        ms.verts = m->verts;
        ms.colors = m->colors;
        ms.materials = m->materials;
        ms.faces = m->faces;
        ms.faceGroups = m->faceGroups;
        ms.vrfStartCount = m->vrfStartCount;
        ms.vertRingFace = m->vertRingFace;
        ms.vrvStartCount = m->vrvStartCount;
        ms.vertRingVert = m->vertRingVert;
        ms.vertOnEdge = m->vertOnEdge;
        ms.vertVisible = m->vertVisible;
        ms.nbVerts = m->nbVerts;
        ms.nbFaces = m->nbFaces;
        
        ms.matrix = m->matrix;
        
        ms.id = m->m_id;
        ms.outlinerName = m->outlinerName;
        ms.visibleV1 = m->visibleV1;
        ms.visibleV2 = m->visibleV2;
        
        hs.meshes.push_back(ms);
    }
    return hs;
}

void Scene::restoreState(const HistoryState& hs) {
    for (auto* m : m_meshes) {
        delete m;
    }
    m_meshes.clear();
    m_selectedMeshes.clear();

    for (const auto& ms : hs.meshes) {
        Mesh* m = new Mesh();
        m->verts = ms.verts;
        m->colors = ms.colors;
        m->materials = ms.materials;
        m->faces = ms.faces;
        m->faceGroups = ms.faceGroups;
        m->isFaceGroupDirty = true;
        m->vrfStartCount = ms.vrfStartCount;
        m->vertRingFace = ms.vertRingFace;
        m->vrvStartCount = ms.vrvStartCount;
        m->vertRingVert = ms.vertRingVert;
        m->vertOnEdge = ms.vertOnEdge;
        m->vertVisible = ms.vertVisible;
        m->nbVerts = ms.nbVerts;
        m->nbFaces = ms.nbFaces;

        m->matrix = ms.matrix;

        m->m_id = ms.id;
        m->outlinerName = ms.outlinerName;
        m->visibleV1 = ms.visibleV1;
        m->visibleV2 = ms.visibleV2;

        m->postInit();
        m_meshes.push_back(m);
    }
    m_selectedIdx = hs.selectedMeshIdx;
    for (int idx : hs.selectedMeshIndices) {
        if (idx >= 0 && idx < (int)m_meshes.size()) {
            m_selectedMeshes.push_back(m_meshes[idx]);
        }
    }
    if (m_selectedMeshes.empty() && m_selectedIdx >= 0 && m_selectedIdx < (int)m_meshes.size()) {
        m_selectedMeshes.push_back(m_meshes[m_selectedIdx]);
    }
}

#include "editing/undo/UndoManager.h"

Mesh* Scene::getMeshById(uint32_t id) const {
    for (auto* m : m_meshes) {
        if (m && m->m_id == id) return m;
    }
    return nullptr;
}

void Scene::pushHistoryState() {
    g_undoManager.pushLegacyState(*this);
}

void Scene::undo() {
    g_undoManager.undo(*this);
}

void Scene::redo() {
    g_undoManager.redo(*this);
}

void Scene::clearHistory() {
    g_undoManager.clear();
}

bool Scene::canUndo() const {
    return g_undoManager.canUndo();
}

bool Scene::canRedo() const {
    return g_undoManager.canRedo();
}

void Scene::addMesh(Mesh* m) {
    if (std::find(m_meshes.begin(), m_meshes.end(), m) == m_meshes.end()) {
        m_meshes.push_back(m);
    }
}

void Scene::removeMesh(Mesh* m) {
    auto it = std::find(m_meshes.begin(), m_meshes.end(), m);
    if (it != m_meshes.end()) {
        int idx = (int)std::distance(m_meshes.begin(), it);
        m_meshes.erase(it);
        
        auto selIt = std::find(m_selectedMeshes.begin(), m_selectedMeshes.end(), m);
        if (selIt != m_selectedMeshes.end()) {
            m_selectedMeshes.erase(selIt);
        }
        
        delete m;
        
        if (m_selectedIdx == idx) {
            if (!m_selectedMeshes.empty()) {
                selectMesh(m_selectedMeshes.back());
            } else {
                m_selectedIdx = m_meshes.empty() ? -1 : 0;
                if (m_selectedIdx != -1) {
                    m_selectedMeshes.push_back(m_meshes[m_selectedIdx]);
                }
            }
        } else if (m_selectedIdx > idx) {
            m_selectedIdx--;
        }
    }
}

const std::vector<Mesh*>& Scene::getMeshes() const {
    return m_meshes;
}

const std::vector<Mesh*>& Scene::getSelectedMeshes() const {
    return m_selectedMeshes;
}

void Scene::setOrUnsetMesh(Mesh* target, bool multiSelect) {
    if (!target) {
        m_selectedMeshes.clear();
        m_selectedIdx = -1;
        return;
    }

    if (!multiSelect) {
        m_selectedMeshes.clear();
        m_selectedMeshes.push_back(target);
        selectMesh(target);
    } else {
        auto it = std::find(m_selectedMeshes.begin(), m_selectedMeshes.end(), target);
        if (it != m_selectedMeshes.end()) {
            m_selectedMeshes.erase(it);
            if (getSelected() == target) {
                if (!m_selectedMeshes.empty()) {
                    selectMesh(m_selectedMeshes.back());
                } else {
                    m_selectedIdx = -1;
                }
            }
        } else {
            m_selectedMeshes.push_back(target);
            selectMesh(target);
        }
    }
}

bool Scene::isMeshSelected(Mesh* target) const {
    return std::find(m_selectedMeshes.begin(), m_selectedMeshes.end(), target) != m_selectedMeshes.end();
}

void Scene::selectMesh(Mesh* m) {
    auto it = std::find(m_meshes.begin(), m_meshes.end(), m);
    if (it != m_meshes.end()) {
        m_selectedIdx = (int)std::distance(m_meshes.begin(), it);
        if (std::find(m_selectedMeshes.begin(), m_selectedMeshes.end(), m) == m_selectedMeshes.end()) {
            m_selectedMeshes.clear();
            m_selectedMeshes.push_back(m);
        }
    } else {
        m_selectedIdx = -1;
        m_selectedMeshes.clear();
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
        m_selectedMeshes.clear();
        if (idx != -1) {
            m_selectedMeshes.push_back(m_meshes[idx]);
        }
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
    if (idx == 1 && m_splitMode == SplitMode::INDEPENDENT && m_cameraRight) {
        return m_cameraRight.get();
    }
    return &m_camera;
}

const Camera* Scene::getCameraByIndex(int idx) const {
    if (idx == 1 && m_splitMode == SplitMode::INDEPENDENT && m_cameraRight) {
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

            colors.push_back(1.0f);
            colors.push_back(1.0f);
            colors.push_back(1.0f);

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
            faces.push_back(v3);
            faces.push_back(v2);
            faces.push_back(v1);
        }
    }
}

static void generateGeosphere(
    float radius, int subdivision,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
) {
    int sub = subdivision * 8;
    if (sub < 1) sub = 1;

    struct VecCompare {
        bool operator()(const glm::vec3& a, const glm::vec3& b) const {
            float eps = 1e-4f;
            if (std::abs(a.x - b.x) > eps) return a.x < b.x;
            if (std::abs(a.y - b.y) > eps) return a.y < b.y;
            if (std::abs(a.z - b.z) > eps) return a.z < b.z;
            return false;
        }
    };

    std::map<glm::vec3, uint32_t, VecCompare> vertexMap;
    std::vector<glm::vec3> uniqueVertices;

    auto getUniqueVertex = [&](const glm::vec3& pos) {
        glm::vec3 spherePos = glm::normalize(pos) * radius;
        auto it = vertexMap.find(spherePos);
        if (it != vertexMap.end()) {
            return it->second;
        }
        uint32_t index = uniqueVertices.size();
        uniqueVertices.push_back(spherePos);
        vertexMap[spherePos] = index;
        return index;
    };

    float side = 2.0f;
    float half = 1.0f;

    int startIdx = vertices.size() / 3;

    auto generateFace = [&](glm::vec3 origin, glm::vec3 right, glm::vec3 up) {
        std::vector<std::vector<uint32_t>> grid(sub + 1, std::vector<uint32_t>(sub + 1));
        for (int y = 0; y <= sub; ++y) {
            float v = (float)y / sub;
            for (int x = 0; x <= sub; ++x) {
                float u = (float)x / sub;
                glm::vec3 pos = origin + (u - 0.5f) * side * right + (v - 0.5f) * side * up;
                grid[y][x] = getUniqueVertex(pos);
            }
        }

        for (int y = 0; y < sub; ++y) {
            for (int x = 0; x < sub; ++x) {
                uint32_t v0 = grid[y][x];
                uint32_t v1 = grid[y][x + 1];
                uint32_t v2 = grid[y + 1][x + 1];
                uint32_t v3 = grid[y + 1][x];

                faces.push_back(startIdx + v0);
                faces.push_back(startIdx + v1);
                faces.push_back(startIdx + v2);
                faces.push_back(startIdx + v3);
            }
        }
    };

    generateFace(glm::vec3(0, 0, half), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0));
    generateFace(glm::vec3(0, 0, -half), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0));
    generateFace(glm::vec3(half, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
    generateFace(glm::vec3(-half, 0, 0), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0));
    generateFace(glm::vec3(0, half, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, -1));
    generateFace(glm::vec3(0, -half, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1));

    for (auto const& v : uniqueVertices) {
        vertices.push_back(v.x);
        vertices.push_back(v.y);
        vertices.push_back(v.z);

        colors.push_back(1.0f);
        colors.push_back(1.0f);
        colors.push_back(1.0f);

        glm::vec3 n = glm::normalize(v);
        normals.push_back(n.x);
        normals.push_back(n.y);
        normals.push_back(n.z);
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

static void generateSubdividedCube(
    float side, int sub,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
) {
    float half = side * 0.5f;

    auto generateFace = [&](glm::vec3 origin, glm::vec3 right, glm::vec3 up, glm::vec3 normal) {
        int startIdx = vertices.size() / 3;
        for (int y = 0; y <= sub; ++y) {
            float v = (float)y / sub;
            for (int x = 0; x <= sub; ++x) {
                float u = (float)x / sub;
                glm::vec3 pos = origin + (u - 0.5f) * side * right + (v - 0.5f) * side * up;
                vertices.push_back(pos.x);
                vertices.push_back(pos.y);
                vertices.push_back(pos.z);

                colors.push_back(1.0f);
                colors.push_back(1.0f);
                colors.push_back(1.0f);

                normals.push_back(normal.x);
                normals.push_back(normal.y);
                normals.push_back(normal.z);
            }
        }
        for (int y = 0; y < sub; ++y) {
            for (int x = 0; x < sub; ++x) {
                uint32_t v0 = startIdx + y * (sub + 1) + x;
                uint32_t v1 = startIdx + y * (sub + 1) + (x + 1);
                uint32_t v2 = startIdx + (y + 1) * (sub + 1) + (x + 1);
                uint32_t v3 = startIdx + (y + 1) * (sub + 1) + x;

                faces.push_back(v0);
                faces.push_back(v1);
                faces.push_back(v2);
                faces.push_back(v3);
            }
        }
    };

    generateFace(glm::vec3(0, 0, half), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1));
    generateFace(glm::vec3(0, 0, -half), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, -1));
    generateFace(glm::vec3(half, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0));
    generateFace(glm::vec3(-half, 0, 0), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0), glm::vec3(-1, 0, 0));
    generateFace(glm::vec3(0, half, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
    generateFace(glm::vec3(0, -half, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1), glm::vec3(0, -1, 0));
}

static void generateCylinder(
    float radiusTop, float radiusBottom, float height,
    int radSegments, int heightSegments, bool topCap, bool lowCap,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
) {
    float heightHalf = height * 0.5f;
    int startIdx = vertices.size() / 3;

    for (int i = 0; i <= heightSegments; i++) {
        float v = (float)i / heightSegments;
        float radius = v * (radiusBottom - radiusTop) + radiusTop;
        float y = -v * height + heightHalf;
        for (int j = 0; j < radSegments; j++) {
            float u = (float)M_PI * 2.0f * j / radSegments;
            float x = radius * std::sin(u);
            float z = radius * std::cos(u);

            vertices.push_back(x);
            vertices.push_back(y);
            vertices.push_back(z);

            colors.push_back(1.0f);
            colors.push_back(1.0f);
            colors.push_back(1.0f);

            float nx = std::sin(u);
            float nz = std::cos(u);
            normals.push_back(nx);
            normals.push_back(0.0f);
            normals.push_back(nz);
        }
    }

    for (int i = 0; i < heightSegments; i++) {
        for (int j = 0; j < radSegments; j++) {
            int off = (j == radSegments - 1) ? 0 : j + 1;
            uint32_t v0 = startIdx + radSegments * i + j;
            uint32_t v1 = startIdx + radSegments * (i + 1) + j;
            uint32_t v2 = startIdx + radSegments * (i + 1) + off;
            uint32_t v3 = startIdx + radSegments * i + off;

            faces.push_back(v0);
            faces.push_back(v1);
            faces.push_back(v2);
            faces.push_back(v3);
        }
    }

    if (topCap) {
        uint32_t last = startIdx + (vertices.size() / 3 - startIdx);
        vertices.push_back(0.0f);
        vertices.push_back(heightHalf);
        vertices.push_back(0.0f);

        colors.push_back(1.0f);
        colors.push_back(1.0f);
        colors.push_back(1.0f);

        normals.push_back(0.0f);
        normals.push_back(1.0f);
        normals.push_back(0.0f);

        for (int j = 0; j < radSegments; j++) {
            int next = (j == radSegments - 1) ? 0 : j + 1;
            faces.push_back(startIdx + j);
            faces.push_back(startIdx + next);
            faces.push_back(last);
            faces.push_back(TRI_INDEX);
        }
    }

    if (lowCap) {
        uint32_t last = startIdx + (vertices.size() / 3 - startIdx);
        vertices.push_back(0.0f);
        vertices.push_back(-heightHalf);
        vertices.push_back(0.0f);

        colors.push_back(1.0f);
        colors.push_back(1.0f);
        colors.push_back(1.0f);

        normals.push_back(0.0f);
        normals.push_back(-1.0f);
        normals.push_back(0.0f);

        int end = radSegments * heightSegments;
        for (int j = 0; j < radSegments; j++) {
            int next = (j == radSegments - 1) ? end : end + j + 1;
            faces.push_back(startIdx + next);
            faces.push_back(startIdx + end + j);
            faces.push_back(last);
            faces.push_back(TRI_INDEX);
        }
    }
}

static void generateTorus(
    float radiusOut, float radiusWidth, float arc,
    int nbRadial, int nbTubular,
    std::vector<float>& vertices,
    std::vector<uint32_t>& faces,
    std::vector<float>& colors,
    std::vector<float>& normals
) {
    bool isFull = std::abs(2.0f * (float)M_PI - arc) < 1e-2f;
    int endTubular = isFull ? nbTubular : nbTubular - 1;
    int startIdx = vertices.size() / 3;

    for (int i = 0; i < nbTubular; ++i) {
        for (int j = 0; j < nbRadial; ++j) {
            float u = (float)i / endTubular * arc;
            float v = (float)j / nbRadial * 2.0f * (float)M_PI;

            float x = (radiusOut + radiusWidth * std::cos(v)) * std::cos(u);
            float y = radiusWidth * std::sin(v);
            float z = (radiusOut + radiusWidth * std::cos(v)) * std::sin(u);

            vertices.push_back(x);
            vertices.push_back(y);
            vertices.push_back(z);

            colors.push_back(1.0f);
            colors.push_back(1.0f);
            colors.push_back(1.0f);

            float cx = radiusOut * std::cos(u);
            float cz = radiusOut * std::sin(u);
            float nx = x - cx;
            float ny = y;
            float nz = z - cz;
            float len = std::sqrt(nx*nx + ny*ny + nz*nz);
            if (len > 0.0f) {
                normals.push_back(nx / len);
                normals.push_back(ny / len);
                normals.push_back(nz / len);
            } else {
                normals.push_back(0.0f);
                normals.push_back(1.0f);
                normals.push_back(0.0f);
            }
        }
    }

    for (int i = 0; i < endTubular; ++i) {
        int offi = (i == nbTubular - 1) ? 0 : i + 1;
        for (int j = 0; j < nbRadial; ++j) {
            int offj = (j == nbRadial - 1) ? 0 : j + 1;
            uint32_t v0 = startIdx + nbRadial * i + j;
            uint32_t v1 = startIdx + nbRadial * i + offj;
            uint32_t v2 = startIdx + nbRadial * offi + offj;
            uint32_t v3 = startIdx + nbRadial * offi + j;

            faces.push_back(v0);
            faces.push_back(v1);
            faces.push_back(v2);
            faces.push_back(v3);
        }
    }

    if (!isFull) {
        uint32_t last = startIdx + (vertices.size() / 3 - startIdx);
        vertices.push_back(radiusOut);
        vertices.push_back(0.0f);
        vertices.push_back(0.0f);

        colors.push_back(1.0f);
        colors.push_back(1.0f);
        colors.push_back(1.0f);

        normals.push_back(-1.0f);
        normals.push_back(0.0f);
        normals.push_back(0.0f);

        for (int j = 0; j < nbRadial; j++) {
            int next = (j == nbRadial - 1) ? 0 : j + 1;
            faces.push_back(last);
            faces.push_back(startIdx + next);
            faces.push_back(startIdx + j);
            faces.push_back(TRI_INDEX);
        }

        last = startIdx + (vertices.size() / 3 - startIdx);
        vertices.push_back(radiusOut * std::cos(arc));
        vertices.push_back(0.0f);
        vertices.push_back(radiusOut * std::sin(arc));

        colors.push_back(1.0f);
        colors.push_back(1.0f);
        colors.push_back(1.0f);

        normals.push_back(std::cos(arc));
        normals.push_back(0.0f);
        normals.push_back(std::sin(arc));

        int end = nbRadial * (nbTubular - 1);
        for (int j = 0; j < nbRadial; j++) {
            int next = (j == nbRadial - 1) ? end : end + j + 1;
            faces.push_back(last);
            faces.push_back(startIdx + end + j);
            faces.push_back(startIdx + next);
            faces.push_back(TRI_INDEX);
        }
    }
}

void Scene::addSphere() {
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
    mesh->outlinerName = "Sphere " + std::to_string(mesh->m_id);
    mesh->postInit();

    pushHistoryState();
    addMesh(mesh);
    selectMesh(mesh);
}

void Scene::addGeosphere() {
    std::vector<float> vertices;
    std::vector<uint32_t> faces;
    std::vector<float> colors;
    std::vector<float> normals;

    generateGeosphere(50.0f, 4, vertices, faces, colors, normals);
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
    mesh->outlinerName = "Geosphere " + std::to_string(mesh->m_id);
    mesh->postInit();

    pushHistoryState();
    addMesh(mesh);
    selectMesh(mesh);
}

void Scene::addCube() {
    std::vector<float> vertices;
    std::vector<uint32_t> faces;
    std::vector<float> colors;
    std::vector<float> normals;

    generateSubdividedCube(70.0f, 50, vertices, faces, colors, normals);
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
    mesh->outlinerName = "Cube " + std::to_string(mesh->m_id);
    mesh->postInit();

    pushHistoryState();
    addMesh(mesh);
    selectMesh(mesh);
}

void Scene::addCylinder() {
    std::vector<float> vertices;
    std::vector<uint32_t> faces;
    std::vector<float> colors;
    std::vector<float> normals;

    generateCylinder(25.0f, 25.0f, 70.0f, 80, 80, true, true, vertices, faces, colors, normals);
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
    mesh->outlinerName = "Cylinder " + std::to_string(mesh->m_id);
    mesh->postInit();

    pushHistoryState();
    addMesh(mesh);
    selectMesh(mesh);
}

void Scene::addTorus() {
    std::vector<float> vertices;
    std::vector<uint32_t> faces;
    std::vector<float> colors;
    std::vector<float> normals;

    generateTorus(35.0f, 10.0f, 2.0f * (float)M_PI, 32, 128, vertices, faces, colors, normals);
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
    mesh->outlinerName = "Torus " + std::to_string(mesh->m_id);
    mesh->postInit();

    pushHistoryState();
    addMesh(mesh);
    selectMesh(mesh);
}

void Scene::addPrimitiveAtMask(const std::string& type, bool useSym, int symAxis) {
    Mesh* activeMesh = getSelected();
    if (!activeMesh) return;

    glm::vec3 minBound(INFINITY);
    glm::vec3 maxBound(-INFINITY);
    int count = 0;

    for (int i = 0; i < activeMesh->nbVerts; ++i) {
        if (activeMesh->materials[i * 3 + 2] < 1.0f) {
            glm::vec3 v(activeMesh->verts[i * 3], activeMesh->verts[i * 3 + 1], activeMesh->verts[i * 3 + 2]);
            minBound = glm::min(minBound, v);
            maxBound = glm::max(maxBound, v);
            count++;
        }
    }

    if (count == 0) {
        if (type == "sphere") addSphere();
        else if (type == "geosphere") addGeosphere();
        else if (type == "cube") addCube();
        else if (type == "cylinder") addCylinder();
        else if (type == "torus") addTorus();
        return;
    }

    pushHistoryState();

    glm::vec3 localCenter = (minBound + maxBound) * 0.5f;
    glm::vec3 localSize = maxBound - minBound;

    auto spawnPrimitive = [&](glm::vec3 locCenter, std::string suffix) {
        std::vector<float> vertices;
        std::vector<uint32_t> faces;
        std::vector<float> colors;
        std::vector<float> normals;

        Mesh* mesh = nullptr;
        if (type == "sphere") {
            generateUVSphere(50.0f, 100, 100, vertices, faces, colors, normals);
            mesh = new Mesh();
            mesh->outlinerName = "Sphere " + suffix;
        } else if (type == "geosphere") {
            generateGeosphere(50.0f, 4, vertices, faces, colors, normals);
            mesh = new Mesh();
            mesh->outlinerName = "Geosphere " + suffix;
        } else if (type == "cube") {
            generateSubdividedCube(70.0f, 50, vertices, faces, colors, normals);
            mesh = new Mesh();
            mesh->outlinerName = "Cube " + suffix;
        } else if (type == "cylinder") {
            generateCylinder(25.0f, 25.0f, 70.0f, 80, 80, true, true, vertices, faces, colors, normals);
            mesh = new Mesh();
            mesh->outlinerName = "Cylinder " + suffix;
        } else if (type == "torus") {
            generateTorus(35.0f, 10.0f, 2.0f * (float)M_PI, 32, 128, vertices, faces, colors, normals);
            mesh = new Mesh();
            mesh->outlinerName = "Torus " + suffix;
        }

        if (!mesh) return (Mesh*)nullptr;

        int nbVerts = vertices.size() / 3;
        int nbFaces = faces.size() / 4;

        std::vector<uint32_t> vrvStartCount;
        std::vector<uint32_t> vertRingVert;
        std::vector<uint32_t> vrfStartCount;
        std::vector<uint32_t> vertRingFace;
        std::vector<uint8_t> vertOnEdge;
        computeTopology(nbVerts, faces.data(), nbFaces, vrfStartCount, vertRingFace, vrvStartCount, vertRingVert, vertOnEdge);

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

        glm::vec3 worldCenter = glm::vec3(activeMesh->matrix * glm::vec4(locCenter, 1.0f));
        float maxDim = glm::max(localSize.x, glm::max(localSize.y, localSize.z));
        float sourceMeshScale = glm::length(glm::vec3(activeMesh->matrix[0]));
        float s = (maxDim / 100.0f) * sourceMeshScale;
        s = glm::max(s, 0.01f);

        mesh->matrix = glm::translate(glm::mat4(1.0f), worldCenter) * glm::scale(glm::mat4(1.0f), glm::vec3(s));
        addMesh(mesh);
        return mesh;
    };

    Mesh* prim = spawnPrimitive(localCenter, std::to_string(m_meshes.size() + 1));
    if (prim) {
        selectMesh(prim);
    }

    if (useSym) {
        glm::vec3 mirroredCenter = localCenter;
        if (symAxis == 0) mirroredCenter.x = -localCenter.x;
        else if (symAxis == 1) mirroredCenter.y = -localCenter.y;
        else if (symAxis == 2) mirroredCenter.z = -localCenter.z;

        if (glm::distance(mirroredCenter, localCenter) > 0.001f) {
            spawnPrimitive(mirroredCenter, std::to_string(m_meshes.size() + 1));
        }
    }
}

void Scene::duplicateSelection() {
    if (m_selectedMeshes.empty()) return;

    pushHistoryState();

    std::vector<Mesh*> newCopies;
    for (Mesh* src : m_selectedMeshes) {
        Mesh* copy = new Mesh();
        copy->verts = src->verts;
        copy->colors = src->colors;
        copy->faces = src->faces;
        copy->vrfStartCount = src->vrfStartCount;
        copy->vertRingFace = src->vertRingFace;
        copy->vrvStartCount = src->vrvStartCount;
        copy->vertRingVert = src->vertRingVert;
        copy->vertOnEdge = src->vertOnEdge;
        copy->vertVisible = src->vertVisible;
        copy->nbVerts = src->nbVerts;
        copy->nbFaces = src->nbFaces;

        copy->matrix = src->matrix;
        copy->visibleV1 = src->visibleV1;
        copy->visibleV2 = src->visibleV2;

        copy->outlinerName = src->outlinerName + " Copy";
        copy->postInit();
        addMesh(copy);
        newCopies.push_back(copy);
    }

    m_selectedMeshes = newCopies;
    if (!newCopies.empty()) {
        selectMesh(newCopies.back());
    }
}

void Scene::mergeSelection() {
    if (m_selectedMeshes.size() < 2) return;

    pushHistoryState();

    MergedMesh mm = MeshUtils::mergeMeshes(m_selectedMeshes);
    std::vector<Mesh*> toRemove = m_selectedMeshes;
    m_selectedMeshes.clear();
    
    for (Mesh* m : toRemove) {
        auto it = std::find(m_meshes.begin(), m_meshes.end(), m);
        if (it != m_meshes.end()) {
            m_meshes.erase(it);
            delete m;
        }
    }

    std::vector<uint32_t> vrvStartCount;
    std::vector<uint32_t> vertRingVert;
    std::vector<uint32_t> vrfStartCount;
    std::vector<uint32_t> vertRingFace;
    std::vector<uint8_t> vertOnEdge;
    computeTopology(mm.nbVerts, mm.faces.data(), mm.nbFaces, vrfStartCount, vertRingFace, vrvStartCount, vertRingVert, vertOnEdge);

    Mesh* mergedMesh = new Mesh();
    mergedMesh->verts = mm.verts;
    mergedMesh->faces = mm.faces;
    mergedMesh->colors = mm.colors;
    mergedMesh->materials = mm.materials;
    mergedMesh->nbVerts = mm.nbVerts;
    mergedMesh->nbFaces = mm.nbFaces;
    mergedMesh->vrfStartCount = vrfStartCount;
    mergedMesh->vertRingFace = vertRingFace;
    mergedMesh->vrvStartCount = vrvStartCount;
    mergedMesh->vertRingVert = vertRingVert;
    mergedMesh->vertOnEdge = vertOnEdge;
    mergedMesh->matrix = glm::mat4(1.0f);
    mergedMesh->outlinerName = "Merged Mesh";
    mergedMesh->postInit();

    addMesh(mergedMesh);
    selectMesh(mergedMesh);
}

void Scene::clearScene() {
    pushHistoryState();
    clear();
}

void Scene::updateVoxelPreview(float step, const std::vector<Mesh*>& meshes) {
    if (step <= 0.0f || meshes.empty()) {
        m_voxelPreview = false;
        m_voxelStep = 0.0f;
        m_voxelMeshes.clear();
    } else {
        m_voxelPreview = true;
        m_voxelStep = step;
        m_voxelMeshes = meshes;
    }
}
