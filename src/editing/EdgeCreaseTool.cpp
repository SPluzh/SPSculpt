#include "editing/EdgeCreaseTool.h"
#include "editing/SculptManager.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "mesh/Mesh.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>

EdgeCreaseTool::EdgeCreaseTool(SculptManager& sm)
    : m_sculpt(sm)
{
}

SelectionManager& EdgeCreaseTool::getSelectionManager() {
    return m_sculpt.getSelectionManager();
}

const SelectionManager& EdgeCreaseTool::getSelectionManager() const {
    return m_sculpt.getSelectionManager();
}

void EdgeCreaseTool::onActivate(Scene& scene) {
    Mesh* mesh = scene.getSelected();
    if (mesh) {
        getSelectionManager().ensureEdgeTopology(*mesh);
    }
    m_isDraggingLevel = false;
}

void EdgeCreaseTool::onDeactivate() {
    m_isDraggingLevel = false;
}

void EdgeCreaseTool::applyCreaseToSelected(Mesh* mesh, bool enable) {
    if (!mesh || mesh->nbEdges == 0) return;
    auto edges = getSelectionManager().getEffectiveEdgeIndices(*mesh);
    if (edges.empty()) return;

    for (uint32_t e : edges) {
        mesh->creaseEdge(e, enable);
    }
    mesh->invalidateDynamicSubdiv();
    mesh->isDirty = true;
}

void EdgeCreaseTool::toggleCreaseOnSelected(Mesh* mesh) {
    if (!mesh || mesh->nbEdges == 0) return;
    auto edges = getSelectionManager().getEffectiveEdgeIndices(*mesh);
    if (edges.empty()) return;

    for (uint32_t e : edges) {
        mesh->creaseEdge(e, !mesh->isEdgeCreased(e));
    }
    mesh->invalidateDynamicSubdiv();
    mesh->isDirty = true;
}

bool EdgeCreaseTool::handleMouseDown(
    Scene& scene, const Camera& camera,
    float mouseX, float mouseY,
    bool ctrl, bool shift, int button
) {
    (void)camera; (void)ctrl; (void)shift;
    if (button != 1) return false;

    Mesh* mesh = scene.getSelected();
    if (!mesh) return false;

    m_isDraggingLevel = true;
    m_dragStartX = mouseX;
    m_dragStartLevel = mesh->dynamicSettings.creaseLevel;
    m_currentMousePos = glm::vec2(mouseX, mouseY);
    return true;
}

bool EdgeCreaseTool::handleMouseMove(
    Scene& scene, const Camera& camera,
    float mouseX, float mouseY,
    bool ctrl, bool shift
) {
    (void)camera; (void)ctrl; (void)shift;
    if (!m_isDraggingLevel) return false;

    Mesh* mesh = scene.getSelected();
    if (!mesh) return false;

    m_currentMousePos = glm::vec2(mouseX, mouseY);
    float dx = mouseX - m_dragStartX;
    float maxCrease = std::max(1.0f, (float)mesh->dynamicSettings.smoothLevel);
    float deltaLevel = (dx / 180.0f) * maxCrease;
    float newLevel = std::clamp(m_dragStartLevel + deltaLevel, 0.0f, maxCrease);

    if (std::abs(newLevel - mesh->dynamicSettings.creaseLevel) > 0.005f) {
        mesh->dynamicSettings.creaseLevel = newLevel;
        mesh->invalidateDynamicSubdiv();
        mesh->isDirty = true;
    }
    return true;
}

bool EdgeCreaseTool::handleMouseUp(Scene& scene, const Camera& camera, int button) {
    (void)camera;
    if (button != 1) return false;

    if (m_isDraggingLevel) {
        m_isDraggingLevel = false;
        scene.pushHistoryState();
        return true;
    }
    return false;
}

void EdgeCreaseTool::renderOverlay(Scene& scene, const Camera& camera, float vW, float vH) {
    (void)vW; (void)vH;
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    if (!drawList) return;

    Mesh* mesh = scene.getSelected();
    if (!mesh || mesh->nbVerts == 0) return;

    auto& selMgr = getSelectionManager();
    auto edges = selMgr.getEffectiveEdgeIndices(*mesh);
    selMgr.ensureEdgeTopology(*mesh);
    const auto& edgeTop = selMgr.getEdgeTopology(*mesh);

    // 1. Draw selected edges with crease indicator
    for (uint32_t e : edges) {
        if (e >= edgeTop.size()) continue;
        uint32_t va = edgeTop[e].v0;
        uint32_t vb = edgeTop[e].v1;
        if (va >= (uint32_t)mesh->nbVerts || vb >= (uint32_t)mesh->nbVerts) continue;

        glm::vec3 v0(mesh->verts[va * 3], mesh->verts[va * 3 + 1], mesh->verts[va * 3 + 2]);
        glm::vec3 v1(mesh->verts[vb * 3], mesh->verts[vb * 3 + 1], mesh->verts[vb * 3 + 2]);
        glm::vec3 s0 = camera.project(glm::vec3(mesh->matrix * glm::vec4(v0, 1.0f)));
        glm::vec3 s1 = camera.project(glm::vec3(mesh->matrix * glm::vec4(v1, 1.0f)));

        if (s0.z > 0.0f && s1.z > 0.0f) {
            bool isCreased = mesh->isEdgeCreased(e);
            ImU32 col = isCreased ? IM_COL32(255, 60, 180, 255) : IM_COL32(255, 170, 0, 230);
            float thickness = isCreased ? 3.5f : 2.0f;
            drawList->AddLine(ImVec2(s0.x, s0.y), ImVec2(s1.x, s1.y), col, thickness);
        }
    }

    // 2. Draw on-screen badge when dragging Crease Level
    if (m_isDraggingLevel) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Crease Level: %.2f", mesh->dynamicSettings.creaseLevel);
        ImVec2 textSz = ImGui::CalcTextSize(buf);
        ImVec2 pCenter(m_currentMousePos.x, m_currentMousePos.y - 30.0f);
        float pad = 8.0f;
        ImVec2 rMin(pCenter.x - textSz.x * 0.5f - pad, pCenter.y - textSz.y * 0.5f - pad);
        ImVec2 rMax(pCenter.x + textSz.x * 0.5f + pad, pCenter.y + textSz.y * 0.5f + pad);

        drawList->AddRectFilled(rMin, rMax, IM_COL32(25, 25, 25, 220), 4.0f);
        drawList->AddRect(rMin, rMax, IM_COL32(255, 60, 180, 255), 4.0f, 0, 1.5f);
        drawList->AddText(ImVec2(pCenter.x - textSz.x * 0.5f, pCenter.y - textSz.y * 0.5f), IM_COL32(255, 255, 255, 255), buf);
    }
}

void EdgeCreaseTool::renderGui(Scene& scene) {
    Mesh* mesh = scene.getSelected();
    auto& selMgr = getSelectionManager();
    auto edges = mesh ? selMgr.getEffectiveEdgeIndices(*mesh) : std::vector<uint32_t>{};

    ImGui::TextDisabled("EDGE CREASE TOOL");
    ImGui::Spacing();

    size_t count = edges.size();
    size_t creasedCount = 0;
    if (mesh) {
        for (uint32_t e : edges) {
            if (mesh->isEdgeCreased(e)) creasedCount++;
        }
    }

    ImGui::Text("Selected Edges: %zu (Creased: %zu)", count, creasedCount);
    ImGui::Spacing();

    float availW = ImGui::GetContentRegionAvail().x;
    float halfW = (availW - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

    bool hasEdges = (count > 0 && mesh != nullptr);

    if (!hasEdges) ImGui::BeginDisabled();
    if (ImGui::Button("Crease On", ImVec2(halfW, 28.0f))) {
        scene.pushHistoryState();
        applyCreaseToSelected(mesh, true);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Mark all selected edges as creased");
    }

    ImGui::SameLine();
    if (ImGui::Button("Crease Off", ImVec2(halfW, 28.0f))) {
        scene.pushHistoryState();
        applyCreaseToSelected(mesh, false);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Remove crease from all selected edges");
    }

    if (ImGui::Button("Toggle Crease", ImVec2(-1, 26.0f))) {
        scene.pushHistoryState();
        toggleCreaseOnSelected(mesh);
    }
    if (!hasEdges) ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Crease Level Slider
    if (mesh) {
        float maxCrease = std::max(1.0f, (float)mesh->dynamicSettings.smoothLevel);
        if (mesh->dynamicSettings.creaseLevel > maxCrease) {
            mesh->dynamicSettings.creaseLevel = maxCrease;
        }

        ImGui::Text("Sharpness Settings:");
        if (ImGui::SliderFloat("Crease Level", &mesh->dynamicSettings.creaseLevel, 0.0f, maxCrease, "%.2f")) {
            mesh->invalidateDynamicSubdiv();
            mesh->isDirty = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Continuous crease sharpness level in Dynamic Subdiv.\nTip: You can also drag with Left Mouse Button directly in the viewport!");
        }

        if (ImGui::Button("Uncrease Entire Mesh", ImVec2(-1, 24.0f))) {
            scene.pushHistoryState();
            mesh->uncreaseAll();
            mesh->invalidateDynamicSubdiv();
            mesh->isDirty = true;
        }
    } else {
        ImGui::TextDisabled("No mesh selected");
    }
}
