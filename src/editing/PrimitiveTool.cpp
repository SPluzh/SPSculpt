#include "editing/PrimitiveTool.h"
#include "editing/SculptManager.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "mesh/Mesh.h"
#include "mesh/Topology.h"
#include "sculpt/PrimitiveGenerator.h"
#include "common/Logger.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>

PrimitiveTool::PrimitiveTool(SculptManager& sm)
    : m_sculpt(sm) {
}

PrimitiveTool::~PrimitiveTool() = default;

void PrimitiveTool::onActivate(Scene& scene) {
    Mesh* sel = scene.getSelected();
    if (sel && sel->isPrimitive && sel->primitiveData) {
        m_activeMesh = sel;
    }
}

void PrimitiveTool::onDeactivate() {
    m_isDraggingHandle = false;
    m_activeHandleIdx = -1;
    m_hoveredHandleIdx = -1;
}

Mesh* PrimitiveTool::spawnPrimitive(Scene& scene, PrimitiveType type, const glm::vec3& position, float scale) {
    Mesh* mesh = new Mesh();
    mesh->isPrimitive = true;
    mesh->primitiveData = std::make_unique<PrimitiveData>();
    mesh->primitiveData->type = type;

    // Set custom initial parameters per type
    switch (type) {
        case PrimitiveType::Sphere:
            mesh->primitiveData->radius = 35.0f;
            mesh->primitiveData->heightSegments = 32;
            mesh->primitiveData->radialSegments = 64;
            break;
        case PrimitiveType::Geosphere:
            mesh->primitiveData->radius = 35.0f;
            mesh->primitiveData->subdivisionLevel = 4;
            break;
        case PrimitiveType::Cube:
            mesh->primitiveData->size = glm::vec3(60.0f);
            mesh->primitiveData->segmentsX = 32;
            mesh->primitiveData->segmentsY = 32;
            mesh->primitiveData->segmentsZ = 32;
            mesh->primitiveData->cornerRadius = 0.0f;
            break;
        case PrimitiveType::Cylinder:
            mesh->primitiveData->height = 70.0f;
            mesh->primitiveData->radiusTop = 25.0f;
            mesh->primitiveData->radiusBottom = 25.0f;
            mesh->primitiveData->radialSegments = 64;
            mesh->primitiveData->heightSegments = 32;
            break;
        case PrimitiveType::Cone:
            mesh->primitiveData->height = 70.0f;
            mesh->primitiveData->radiusBottom = 35.0f;
            mesh->primitiveData->radiusTop = 0.0f;
            mesh->primitiveData->radialSegments = 64;
            mesh->primitiveData->heightSegments = 32;
            break;
        case PrimitiveType::Torus:
            mesh->primitiveData->torusMajorRadius = 35.0f;
            mesh->primitiveData->torusTubeRadius = 10.0f;
            mesh->primitiveData->arcDegrees = 360.0f;
            mesh->primitiveData->radialSegments = 32;
            mesh->primitiveData->heightSegments = 64;
            break;
        case PrimitiveType::Plane:
            mesh->primitiveData->size = glm::vec3(80.0f, 0.0f, 80.0f);
            mesh->primitiveData->segmentsX = 32;
            mesh->primitiveData->segmentsZ = 32;
            break;
        case PrimitiveType::Capsule:
            mesh->primitiveData->radius = 18.0f;
            mesh->primitiveData->height = 40.0f;
            mesh->primitiveData->radialSegments = 32;
            mesh->primitiveData->heightSegments = 20;
            mesh->primitiveData->capRings = 16;
            break;
    }

    mesh->outlinerName = std::string(getPrimitiveTypeName(type)) + " " + std::to_string(mesh->getID());

    updateMeshGeometry(mesh);

    if (position != glm::vec3(0.0f) || scale != 1.0f) {
        mesh->matrix = glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), glm::vec3(scale));
    }

    scene.pushHistoryState();
    scene.addMesh(mesh);
    scene.selectMesh(mesh);
    m_activeMesh = mesh;

    return mesh;
}

void PrimitiveTool::updateMeshGeometry(Mesh* mesh) {
    if (!mesh || !mesh->isPrimitive || !mesh->primitiveData) return;

    if (mesh->primitiveData->autoDensity) {
        mesh->primitiveData->recomputeAutoDensity();
    }

    std::vector<float> vertices, colors, normals;
    std::vector<uint32_t> faces;

    PrimitiveGenerator::generate(*mesh->primitiveData, vertices, faces, colors, normals);

    int nbVerts = static_cast<int>(vertices.size() / 3);
    int nbFaces = static_cast<int>(faces.size() / 4);

    std::vector<uint32_t> vrvStartCount, vertRingVert, vrfStartCount, vertRingFace;
    std::vector<uint8_t> vertOnEdge;
    computeTopology(nbVerts, faces.data(), nbFaces, vrfStartCount, vertRingFace, vrvStartCount, vertRingVert, vertOnEdge);

    mesh->verts = std::move(vertices);
    mesh->faces = std::move(faces);
    mesh->colors = std::move(colors);
    mesh->normals = std::move(normals);
    mesh->nbVerts = nbVerts;
    mesh->nbFaces = nbFaces;
    mesh->vrfStartCount = std::move(vrfStartCount);
    mesh->vertRingFace = std::move(vertRingFace);
    mesh->vrvStartCount = std::move(vrvStartCount);
    mesh->vertRingVert = std::move(vertRingVert);
    mesh->vertOnEdge = std::move(vertOnEdge);

    // Initialize visibility and material attributes so wireframe shading is fully visible
    mesh->vertVisible.assign(nbVerts, 1);
    mesh->faceVisible.assign(nbFaces, 1);
    mesh->materials.assign(nbVerts * 3, 0.0f);
    for (int i = 0; i < nbVerts; ++i) {
        mesh->materials[i * 3 + 0] = 0.5f; // roughness
        mesh->materials[i * 3 + 1] = 0.0f; // metalness
        mesh->materials[i * 3 + 2] = 0.0f; // mask = 0 (unmasked)
    }

    mesh->hasCustomNormals = false;
    mesh->initFaceGroups(mesh->getNextFreeGroupID());
    mesh->postInit();
    mesh->setDirty(true);
    mesh->isTopologyDirty = true;
    mesh->isFaceGroupDirty = true;
}


void PrimitiveTool::updateGeometryIfNeeded(Scene& scene) {
    if (m_geometryDirty) {
        if (m_activeMesh && m_activeMesh->isPrimitive && m_activeMesh->primitiveData) {
            updateMeshGeometry(m_activeMesh);
        }
        m_geometryDirty = false;
        scene.setModified(true);
    }
}

Mesh* PrimitiveTool::bakeToMesh(Scene& scene, Mesh* mesh) {
    if (!mesh || !mesh->isPrimitive) return mesh;

    scene.pushHistoryState();
    mesh->isPrimitive = false;
    mesh->primitiveData.reset();
    scene.setModified(true);

    if (m_activeMesh == mesh) {
        m_activeMesh = nullptr;
    }
    return mesh;
}

void PrimitiveTool::bakeAllPrimitives(Scene& scene) {
    bool bakedAny = false;
    for (Mesh* m : scene.getMeshes()) {
        if (m && m->isPrimitive) {
            if (!bakedAny) {
                scene.pushHistoryState();
                bakedAny = true;
            }
            m->isPrimitive = false;
            m->primitiveData.reset();
        }
    }
    if (bakedAny) {
        scene.setModified(true);
        m_activeMesh = nullptr;
    }
}

void PrimitiveTool::buildHandlesForMesh(Mesh* mesh, const Camera& camera) {
    m_currentHandles.clear();
    if (!mesh || !mesh->isPrimitive || !mesh->primitiveData) return;

    const PrimitiveData& p = *mesh->primitiveData;
    glm::mat4 model = mesh->matrix;

    auto addHandle = [&](PrimitiveHandleType type, const glm::vec3& localPos, const glm::vec3& localAxis,
                         const std::string& label, float startVal, float minV, float maxV) {
        PrimitiveHandle h;
        h.type = type;
        h.localPos = localPos;
        h.localAxis = localAxis;
        h.worldPos = glm::vec3(model * glm::vec4(localPos, 1.0f));
        glm::vec3 proj = camera.project(h.worldPos);
        h.screenPos = glm::vec2(proj.x, proj.y);
        h.label = label;
        h.startVal = startVal;
        h.minVal = minV;
        h.maxVal = maxV;
        if (proj.z >= 0.0f && proj.z <= 1.0f) {
            m_currentHandles.push_back(h);
        }
    };

    switch (p.type) {
        case PrimitiveType::Sphere:
            addHandle(PrimitiveHandleType::SphereRadius, glm::vec3(p.radius, 0.0f, 0.0f), glm::vec3(1, 0, 0),
                      "Radius", p.radius, 0.5f, 500.0f);
            addHandle(PrimitiveHandleType::SphereRadius, glm::vec3(0.0f, p.radius, 0.0f), glm::vec3(0, 1, 0),
                      "Radius", p.radius, 0.5f, 500.0f);
            break;
        case PrimitiveType::Geosphere:
            addHandle(PrimitiveHandleType::GeosphereRadius, glm::vec3(p.radius, 0.0f, 0.0f), glm::vec3(1, 0, 0),
                      "Radius", p.radius, 0.5f, 500.0f);
            addHandle(PrimitiveHandleType::GeosphereRadius, glm::vec3(0.0f, p.radius, 0.0f), glm::vec3(0, 1, 0),
                      "Radius", p.radius, 0.5f, 500.0f);
            break;
        case PrimitiveType::Cube: {
            glm::vec3 half = p.size * 0.5f;
            addHandle(PrimitiveHandleType::CubeSizeX, glm::vec3(half.x, 0, 0), glm::vec3(1, 0, 0),
                      "Width (X)", p.size.x, 0.5f, 500.0f);
            addHandle(PrimitiveHandleType::CubeSizeY, glm::vec3(0, half.y, 0), glm::vec3(0, 1, 0),
                      "Height (Y)", p.size.y, 0.5f, 500.0f);
            addHandle(PrimitiveHandleType::CubeSizeZ, glm::vec3(0, 0, half.z), glm::vec3(0, 0, 1),
                      "Depth (Z)", p.size.z, 0.5f, 500.0f);
            float maxCr = 0.5f * std::min({p.size.x, p.size.y, p.size.z});
            addHandle(PrimitiveHandleType::CubeCornerRadius, glm::vec3(half.x - p.cornerRadius, half.y - p.cornerRadius, half.z), glm::vec3(1, 1, 0),
                      "Corner Fillet", p.cornerRadius, 0.0f, maxCr);
            break;
        }
        case PrimitiveType::Cylinder: {
            float halfH = p.height * 0.5f;
            addHandle(PrimitiveHandleType::CylinderHeight, glm::vec3(0, halfH, 0), glm::vec3(0, 1, 0),
                      "Height", p.height, 0.5f, 500.0f);
            addHandle(PrimitiveHandleType::CylinderRadiusTop, glm::vec3(p.radiusTop, halfH, 0), glm::vec3(1, 0, 0),
                      "R Top", p.radiusTop, 0.0f, 500.0f);
            addHandle(PrimitiveHandleType::CylinderRadiusBottom, glm::vec3(p.radiusBottom, -halfH, 0), glm::vec3(1, 0, 0),
                      "R Bottom", p.radiusBottom, 0.1f, 500.0f);
            if (p.innerRadius > 0.0f) {
                addHandle(PrimitiveHandleType::CylinderInnerRadius, glm::vec3(p.innerRadius, halfH, 0), glm::vec3(1, 0, 0),
                          "Inner R", p.innerRadius, 0.0f, std::min(p.radiusTop, p.radiusBottom) - 0.1f);
            }
            break;
        }
        case PrimitiveType::Cone: {
            float halfH = p.height * 0.5f;
            addHandle(PrimitiveHandleType::ConeHeight, glm::vec3(0, halfH, 0), glm::vec3(0, 1, 0),
                      "Height", p.height, 0.5f, 500.0f);
            addHandle(PrimitiveHandleType::ConeRadiusBottom, glm::vec3(p.radiusBottom, -halfH, 0), glm::vec3(1, 0, 0),
                      "Base R", p.radiusBottom, 0.1f, 500.0f);
            if (p.radiusTop > 0.0f) {
                addHandle(PrimitiveHandleType::ConeRadiusTop, glm::vec3(p.radiusTop, halfH, 0), glm::vec3(1, 0, 0),
                          "Top R", p.radiusTop, 0.0f, 500.0f);
            }
            break;
        }
        case PrimitiveType::Torus: {
            addHandle(PrimitiveHandleType::TorusMajorRadius, glm::vec3(p.torusMajorRadius + p.torusTubeRadius, 0, 0), glm::vec3(1, 0, 0),
                      "Major R", p.torusMajorRadius, 1.0f, 500.0f);
            addHandle(PrimitiveHandleType::TorusTubeRadius, glm::vec3(p.torusMajorRadius, p.torusTubeRadius, 0), glm::vec3(0, 1, 0),
                      "Tube R", p.torusTubeRadius, 0.1f, 200.0f);
            break;
        }
        case PrimitiveType::Plane: {
            addHandle(PrimitiveHandleType::PlaneSizeX, glm::vec3(p.size.x * 0.5f, 0, 0), glm::vec3(1, 0, 0),
                      "Width (X)", p.size.x, 0.5f, 500.0f);
            addHandle(PrimitiveHandleType::PlaneSizeZ, glm::vec3(0, 0, p.size.z * 0.5f), glm::vec3(0, 0, 1),
                      "Length (Z)", p.size.z, 0.5f, 500.0f);
            break;
        }
        case PrimitiveType::Capsule: {
            float halfH = p.height * 0.5f;
            addHandle(PrimitiveHandleType::CapsuleHeight, glm::vec3(0, halfH + p.radius, 0), glm::vec3(0, 1, 0),
                      "Height", p.height, 0.0f, 500.0f);
            addHandle(PrimitiveHandleType::CapsuleRadius, glm::vec3(p.radius, 0, 0), glm::vec3(1, 0, 0),
                      "Radius", p.radius, 0.1f, 300.0f);
            break;
        }
    }
}

void PrimitiveTool::applyHandleDelta(PrimitiveHandleType type, float delta) {
    if (!m_activeMesh || !m_activeMesh->primitiveData) return;
    PrimitiveData& p = *m_activeMesh->primitiveData;
    const PrimitiveData& start = m_startPrimitiveData;

    switch (type) {
        case PrimitiveHandleType::SphereRadius:
            p.radius = std::clamp(start.radius + delta, 0.5f, 500.0f);
            break;
        case PrimitiveHandleType::GeosphereRadius:
            p.radius = std::clamp(start.radius + delta, 0.5f, 500.0f);
            break;
        case PrimitiveHandleType::CubeSizeX:
            p.size.x = std::clamp(start.size.x + delta * 2.0f, 0.5f, 500.0f);
            break;
        case PrimitiveHandleType::CubeSizeY:
            p.size.y = std::clamp(start.size.y + delta * 2.0f, 0.5f, 500.0f);
            break;
        case PrimitiveHandleType::CubeSizeZ:
            p.size.z = std::clamp(start.size.z + delta * 2.0f, 0.5f, 500.0f);
            break;
        case PrimitiveHandleType::CubeCornerRadius: {
            float maxCr = 0.5f * std::min({p.size.x, p.size.y, p.size.z});
            p.cornerRadius = std::clamp(start.cornerRadius + delta, 0.0f, maxCr);
            break;
        }
        case PrimitiveHandleType::CylinderHeight:
            p.height = std::clamp(start.height + delta * 2.0f, 0.5f, 500.0f);
            break;
        case PrimitiveHandleType::CylinderRadiusTop:
            p.radiusTop = std::clamp(start.radiusTop + delta, 0.0f, 500.0f);
            break;
        case PrimitiveHandleType::CylinderRadiusBottom:
            p.radiusBottom = std::clamp(start.radiusBottom + delta, 0.1f, 500.0f);
            break;
        case PrimitiveHandleType::CylinderInnerRadius: {
            float maxInner = std::min(p.radiusTop, p.radiusBottom) - 0.2f;
            p.innerRadius = std::clamp(start.innerRadius + delta, 0.0f, std::max(0.0f, maxInner));
            break;
        }
        case PrimitiveHandleType::ConeHeight:
            p.height = std::clamp(start.height + delta * 2.0f, 0.5f, 500.0f);
            break;
        case PrimitiveHandleType::ConeRadiusBottom:
            p.radiusBottom = std::clamp(start.radiusBottom + delta, 0.1f, 500.0f);
            break;
        case PrimitiveHandleType::ConeRadiusTop:
            p.radiusTop = std::clamp(start.radiusTop + delta, 0.0f, 500.0f);
            break;
        case PrimitiveHandleType::TorusMajorRadius:
            p.torusMajorRadius = std::clamp(start.torusMajorRadius + delta, 1.0f, 500.0f);
            break;
        case PrimitiveHandleType::TorusTubeRadius:
            p.torusTubeRadius = std::clamp(start.torusTubeRadius + delta, 0.1f, 200.0f);
            break;
        case PrimitiveHandleType::PlaneSizeX:
            p.size.x = std::clamp(start.size.x + delta * 2.0f, 0.5f, 500.0f);
            break;
        case PrimitiveHandleType::PlaneSizeZ:
            p.size.z = std::clamp(start.size.z + delta * 2.0f, 0.5f, 500.0f);
            break;
        case PrimitiveHandleType::CapsuleRadius:
            p.radius = std::clamp(start.radius + delta, 0.1f, 300.0f);
            break;
        case PrimitiveHandleType::CapsuleHeight:
            p.height = std::clamp(start.height + delta * 2.0f, 0.0f, 500.0f);
            break;
        default:
            break;
    }

    if (p.autoDensity) {
        p.recomputeAutoDensity();
    }
}


bool PrimitiveTool::handleMouseDown(
    Scene& scene, const Camera& camera,
    float mouseX, float mouseY,
    bool isCtrl, bool isAlt, bool isShift
) {
    (void)isCtrl; (void)isAlt; (void)isShift;
    Mesh* sel = scene.getSelected();
    if (!sel || !sel->isPrimitive || !sel->primitiveData) {
        return false;
    }
    m_activeMesh = sel;

    buildHandlesForMesh(m_activeMesh, camera);

    glm::vec2 mousePos(mouseX, mouseY);
    int hitIdx = -1;
    float closestDist = 16.0f;

    for (size_t i = 0; i < m_currentHandles.size(); ++i) {
        float d = glm::distance(mousePos, m_currentHandles[i].screenPos);
        if (d < closestDist) {
            closestDist = d;
            hitIdx = static_cast<int>(i);
        }
    }

    if (hitIdx != -1) {
        m_isDraggingHandle = true;
        m_activeHandleIdx = hitIdx;
        m_dragStartMouse = mousePos;
        m_startPrimitiveData = *m_activeMesh->primitiveData;
        return true;
    }

    return false;
}

bool PrimitiveTool::handleMouseMove(Scene& scene, const Camera& camera, float mouseX, float mouseY) {
    Mesh* sel = scene.getSelected();
    if (!sel || !sel->isPrimitive || !sel->primitiveData) {
        m_hoveredHandleIdx = -1;
        return false;
    }
    m_activeMesh = sel;

    glm::vec2 mousePos(mouseX, mouseY);

    if (m_isDraggingHandle && m_activeHandleIdx >= 0 && m_activeHandleIdx < static_cast<int>(m_currentHandles.size())) {
        const auto& h = m_currentHandles[m_activeHandleIdx];
        glm::mat4 model = m_activeMesh->matrix;
        glm::vec3 worldAxis = glm::vec3(model * glm::vec4(h.localAxis, 0.0f));
        float localAxisLen = glm::length(worldAxis);
        if (localAxisLen < 1e-4f) return true;
        worldAxis /= localAxisLen;

        glm::vec3 p0 = h.worldPos;
        glm::vec3 p1 = h.worldPos + worldAxis;
        glm::vec3 s0 = camera.project(p0);
        glm::vec3 s1 = camera.project(p1);

        glm::vec2 screenDir = glm::vec2(s1.x - s0.x, s1.y - s0.y);
        float screenLen = glm::length(screenDir);
        if (screenLen > 1e-3f) {
            screenDir /= screenLen;
            float mouseProj = glm::dot(mousePos - m_dragStartMouse, screenDir);
            float worldDelta = mouseProj / screenLen;
            worldDelta /= localAxisLen;

            applyHandleDelta(h.type, worldDelta);
            updateMeshGeometry(m_activeMesh);
            scene.setModified(true);
        }
        return true;
    }

    // Hover detection when not dragging
    buildHandlesForMesh(m_activeMesh, camera);
    int newHover = -1;
    float closestDist = 16.0f;
    for (size_t i = 0; i < m_currentHandles.size(); ++i) {
        float d = glm::distance(mousePos, m_currentHandles[i].screenPos);
        if (d < closestDist) {
            closestDist = d;
            newHover = static_cast<int>(i);
        }
    }
    m_hoveredHandleIdx = newHover;
    return (m_hoveredHandleIdx != -1);
}

bool PrimitiveTool::handleMouseUp(Scene& scene) {
    if (m_isDraggingHandle) {
        m_isDraggingHandle = false;
        m_activeHandleIdx = -1;
        scene.pushHistoryState();
        return true;
    }
    return false;
}

void PrimitiveTool::renderOverlay(
    Scene& scene, const Camera& camera,
    float viewportW, float viewportH
) {
    (void)viewportW; (void)viewportH;
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList) return;

    Mesh* sel = scene.getSelected();
    if (!sel || !sel->isPrimitive || !sel->primitiveData) {
        return;
    }
    m_activeMesh = sel;

    buildHandlesForMesh(m_activeMesh, camera);
    if (m_currentHandles.empty()) return;

    glm::mat4 model = m_activeMesh->matrix;
    glm::vec3 centerWorld = glm::vec3(model * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    glm::vec3 centerProj = camera.project(centerWorld);
    ImVec2 centerScreen(centerProj.x, centerProj.y);

    // Draw handles & connector guide lines
    for (size_t i = 0; i < m_currentHandles.size(); ++i) {
        const auto& h = m_currentHandles[i];
        bool isHovered = (static_cast<int>(i) == m_hoveredHandleIdx);
        bool isActive = (m_isDraggingHandle && static_cast<int>(i) == m_activeHandleIdx);

        // Guide line from center (or anchor) to handle
        if (centerProj.z >= 0.0f && centerProj.z <= 1.0f) {
            ImU32 lineCol = (isHovered || isActive) ? IM_COL32(255, 230, 40, 220) : IM_COL32(0, 200, 240, 140);
            drawList->AddLine(centerScreen, ImVec2(h.screenPos.x, h.screenPos.y), IM_COL32(0, 0, 0, 160), 3.0f);
            drawList->AddLine(centerScreen, ImVec2(h.screenPos.x, h.screenPos.y), lineCol, (isHovered || isActive) ? 2.5f : 1.5f);
        }

        // Draw Handle
        float radius = (isHovered || isActive) ? 9.5f : 7.0f;
        ImVec2 hPos(h.screenPos.x, h.screenPos.y);

        // Shadow circle
        drawList->AddCircleFilled(ImVec2(hPos.x + 1.5f, hPos.y + 1.5f), radius + 1.0f, IM_COL32(0, 0, 0, 180), 16);

        // Outer rim
        ImU32 rimCol = (isHovered || isActive) ? IM_COL32(255, 240, 100, 255) : IM_COL32(20, 25, 30, 240);
        drawList->AddCircle(hPos, radius, rimCol, 16, 2.0f);

        // Inner fill
        ImU32 fillCol;
        if (isActive) {
            fillCol = IM_COL32(255, 150, 20, 255); // Vibrant Orange when dragging
        } else if (isHovered) {
            fillCol = IM_COL32(255, 225, 40, 255); // Glowing Yellow on hover
        } else {
            fillCol = IM_COL32(0, 215, 245, 240);  // Cyan default
        }
        drawList->AddCircleFilled(hPos, radius - 1.5f, fillCol, 16);

        // Tooltip badge showing parameter value
        if (isHovered || isActive) {
            char valStr[64];
            float curVal = 0.0f;
            const PrimitiveData& p = *m_activeMesh->primitiveData;
            switch (h.type) {
                case PrimitiveHandleType::SphereRadius:
                case PrimitiveHandleType::GeosphereRadius:
                case PrimitiveHandleType::CapsuleRadius:
                    curVal = p.radius; break;
                case PrimitiveHandleType::CubeSizeX:
                    curVal = p.size.x; break;
                case PrimitiveHandleType::CubeSizeY:
                    curVal = p.size.y; break;
                case PrimitiveHandleType::CubeSizeZ:
                    curVal = p.size.z; break;
                case PrimitiveHandleType::CubeCornerRadius:
                    curVal = p.cornerRadius; break;
                case PrimitiveHandleType::CylinderHeight:
                case PrimitiveHandleType::ConeHeight:
                case PrimitiveHandleType::CapsuleHeight:
                    curVal = p.height; break;
                case PrimitiveHandleType::CylinderRadiusTop:
                case PrimitiveHandleType::ConeRadiusTop:
                    curVal = p.radiusTop; break;
                case PrimitiveHandleType::CylinderRadiusBottom:
                case PrimitiveHandleType::ConeRadiusBottom:
                    curVal = p.radiusBottom; break;
                case PrimitiveHandleType::CylinderInnerRadius:
                    curVal = p.innerRadius; break;
                case PrimitiveHandleType::TorusMajorRadius:
                    curVal = p.torusMajorRadius; break;
                case PrimitiveHandleType::TorusTubeRadius:
                    curVal = p.torusTubeRadius; break;
                case PrimitiveHandleType::PlaneSizeX:
                    curVal = p.size.x; break;
                case PrimitiveHandleType::PlaneSizeZ:
                    curVal = p.size.z; break;
                default: break;
            }

            snprintf(valStr, sizeof(valStr), "%s: %.2f", h.label.c_str(), curVal);
            ImVec2 txtSize = ImGui::CalcTextSize(valStr);

            ImVec2 badgeMin(hPos.x + 14.0f, hPos.y - txtSize.y * 0.5f - 4.0f);
            ImVec2 badgeMax(badgeMin.x + txtSize.x + 12.0f, badgeMin.y + txtSize.y + 8.0f);

            // Badge Background
            drawList->AddRectFilled(badgeMin, badgeMax, IM_COL32(15, 18, 22, 230), 4.0f);
            drawList->AddRect(badgeMin, badgeMax, IM_COL32(255, 220, 50, 220), 4.0f, 0, 1.5f);

            // Text
            drawList->AddText(ImVec2(badgeMin.x + 6.0f, badgeMin.y + 4.0f), IM_COL32(255, 255, 255, 255), valStr);
        }
    }
}
