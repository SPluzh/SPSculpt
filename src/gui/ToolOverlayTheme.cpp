#include "gui/ToolOverlayTheme.h"
#include "common/IniFile.h"
#include <cstdio>
#include <algorithm>

namespace gui {

namespace {

void writeColor(IniFile& ini, const std::string& sec, const std::string& key, const ImVec4& c) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f %.4f %.4f %.4f", c.x, c.y, c.z, c.w);
    ini.set(sec, key, buf);
}

ImVec4 readColor(const IniFile& ini, const std::string& sec, const std::string& key, const ImVec4& def) {
    if (!ini.hasKey(sec, key)) return def;
    std::string val = ini.get(sec, key);
    float r = def.x, g = def.y, b = def.z, a = def.w;
    if (std::sscanf(val.c_str(), "%f %f %f %f", &r, &g, &b, &a) >= 3) {
        return ImVec4(r, g, b, a);
    }
    return def;
}

} // anonymous namespace

ToolOverlayTheme& ToolOverlayTheme::get() {
    static ToolOverlayTheme s_instance;
    return s_instance;
}

ToolOverlayTheme::ToolOverlayTheme() {
    resetToDefaults();
}

void ToolOverlayTheme::resetToDefaults() {
    pointRadius = 4.0f;
    lineWidth = 1.5f;

    weldTargetFill = ImVec4(1.0f, 0.12f, 0.12f, 0.94f);
    weldTargetRing = ImVec4(1.0f, 1.0f, 1.0f, 0.86f);
    vertexNormal = ImVec4(0.94f, 0.94f, 0.94f, 0.86f);
    vertexBorder = ImVec4(0.16f, 0.16f, 0.16f, 0.70f);
    vertexHover = ImVec4(1.0f, 0.90f, 0.16f, 1.0f);
    vertexCorner = ImVec4(0.20f, 1.0f, 0.39f, 1.0f);
    vertexDelete = ImVec4(1.0f, 0.16f, 0.16f, 1.0f);
    vertexPinned = ImVec4(0.12f, 0.86f, 1.0f, 0.94f);
    vertexMoving = ImVec4(0.20f, 0.86f, 1.0f, 1.0f);

    edgeWireframe = ImVec4(0.20f, 0.90f, 0.35f, 0.90f);
    edgeSelected = ImVec4(1.0f, 0.59f, 0.0f, 1.0f);
    edgeSelectedHover = ImVec4(1.0f, 1.0f, 0.39f, 1.0f);
    edgeHover = ImVec4(1.0f, 0.90f, 0.16f, 1.0f);
    edgeDelete = ImVec4(1.0f, 0.16f, 0.16f, 1.0f);
    edgePreview = ImVec4(0.20f, 1.0f, 0.47f, 1.0f);

    faceHoverFill = ImVec4(0.16f, 0.75f, 1.0f, 0.30f);
    faceHoverOutline = ImVec4(0.27f, 0.90f, 1.0f, 0.86f);
    faceDeleteFill = ImVec4(1.0f, 0.16f, 0.16f, 0.35f);
    faceDeleteOutline = ImVec4(1.0f, 0.24f, 0.24f, 0.86f);
    facePreviewFill = ImVec4(0.20f, 1.0f, 0.47f, 0.40f);
}

void ToolOverlayTheme::save(IniFile& ini, const std::string& section) const {
    ini.setFloat(section, "pointRadius", pointRadius);
    ini.setFloat(section, "lineWidth", lineWidth);

    writeColor(ini, section, "weldTargetFill", weldTargetFill);
    writeColor(ini, section, "weldTargetRing", weldTargetRing);
    writeColor(ini, section, "vertexNormal", vertexNormal);
    writeColor(ini, section, "vertexBorder", vertexBorder);
    writeColor(ini, section, "vertexHover", vertexHover);
    writeColor(ini, section, "vertexCorner", vertexCorner);
    writeColor(ini, section, "vertexDelete", vertexDelete);
    writeColor(ini, section, "vertexPinned", vertexPinned);
    writeColor(ini, section, "vertexMoving", vertexMoving);

    writeColor(ini, section, "edgeWireframe", edgeWireframe);
    writeColor(ini, section, "edgeSelected", edgeSelected);
    writeColor(ini, section, "edgeSelectedHover", edgeSelectedHover);
    writeColor(ini, section, "edgeHover", edgeHover);
    writeColor(ini, section, "edgeDelete", edgeDelete);
    writeColor(ini, section, "edgePreview", edgePreview);

    writeColor(ini, section, "faceHoverFill", faceHoverFill);
    writeColor(ini, section, "faceHoverOutline", faceHoverOutline);
    writeColor(ini, section, "faceDeleteFill", faceDeleteFill);
    writeColor(ini, section, "faceDeleteOutline", faceDeleteOutline);
    writeColor(ini, section, "facePreviewFill", facePreviewFill);
}

void ToolOverlayTheme::load(const IniFile& ini, const std::string& section) {
    if (!ini.hasSection(section)) return;

    if (ini.hasKey(section, "pointRadius")) pointRadius = ini.getFloat(section, "pointRadius", pointRadius);
    if (ini.hasKey(section, "lineWidth")) lineWidth = ini.getFloat(section, "lineWidth", lineWidth);

    weldTargetFill = readColor(ini, section, "weldTargetFill", weldTargetFill);
    weldTargetRing = readColor(ini, section, "weldTargetRing", weldTargetRing);
    vertexNormal = readColor(ini, section, "vertexNormal", vertexNormal);
    vertexBorder = readColor(ini, section, "vertexBorder", vertexBorder);
    vertexHover = readColor(ini, section, "vertexHover", vertexHover);
    vertexCorner = readColor(ini, section, "vertexCorner", vertexCorner);
    vertexDelete = readColor(ini, section, "vertexDelete", vertexDelete);
    vertexPinned = readColor(ini, section, "vertexPinned", vertexPinned);
    vertexMoving = readColor(ini, section, "vertexMoving", vertexMoving);

    edgeWireframe = readColor(ini, section, "edgeWireframe", edgeWireframe);
    edgeSelected = readColor(ini, section, "edgeSelected", edgeSelected);
    edgeSelectedHover = readColor(ini, section, "edgeSelectedHover", edgeSelectedHover);
    edgeHover = readColor(ini, section, "edgeHover", edgeHover);
    edgeDelete = readColor(ini, section, "edgeDelete", edgeDelete);
    edgePreview = readColor(ini, section, "edgePreview", edgePreview);

    faceHoverFill = readColor(ini, section, "faceHoverFill", faceHoverFill);
    faceHoverOutline = readColor(ini, section, "faceHoverOutline", faceHoverOutline);
    faceDeleteFill = readColor(ini, section, "faceDeleteFill", faceDeleteFill);
    faceDeleteOutline = readColor(ini, section, "faceDeleteOutline", faceDeleteOutline);
    facePreviewFill = readColor(ini, section, "facePreviewFill", facePreviewFill);
}

void ToolOverlayTheme::drawWeldMarker(ImDrawList* dl, const ImVec2& pos, float radius) const {
    float r = (radius > 0.0f) ? radius : pointRadius;
    dl->AddCircleFilled(pos, r * 1.6f, toU32(weldTargetFill));
    dl->AddCircle(pos, r * 2.2f, toU32(weldTargetRing), 16, 2.0f);
}

void ToolOverlayTheme::drawVertex(ImDrawList* dl, const ImVec2& pos, OverlayVertexState state, float radius) const {
    float r = (radius > 0.0f) ? radius : pointRadius;
    switch (state) {
        case OverlayVertexState::WeldTarget:
            drawWeldMarker(dl, pos, r);
            break;
        case OverlayVertexState::Hover:
            dl->AddCircleFilled(pos, r * 1.4f, toU32(vertexHover));
            dl->AddCircle(pos, r * 1.8f, IM_COL32(0, 0, 0, 180), 16, 1.5f);
            break;
        case OverlayVertexState::ExtrudableCorner:
            dl->AddCircleFilled(pos, r * 1.5f, toU32(vertexCorner));
            dl->AddCircle(pos, r * 2.2f, toU32(weldTargetRing), 16, 2.0f);
            break;
        case OverlayVertexState::DeleteHover:
            dl->AddCircleFilled(pos, r * 1.5f, toU32(vertexDelete));
            dl->AddCircle(pos, r * 2.0f, toU32(weldTargetRing), 16, 1.5f);
            break;
        case OverlayVertexState::Pinned:
            dl->AddCircleFilled(pos, r * 1.2f, toU32(vertexPinned));
            dl->AddCircle(pos, r * 1.4f, IM_COL32(0, 0, 0, 180), 16, 1.0f);
            break;
        case OverlayVertexState::MovingDrag:
            dl->AddCircleFilled(pos, r * 1.3f, toU32(vertexMoving));
            dl->AddCircle(pos, r * 1.6f, IM_COL32(255, 255, 255, 200), 16, 1.5f);
            break;
        case OverlayVertexState::Normal:
        default:
            dl->AddCircleFilled(pos, r, toU32(vertexNormal));
            dl->AddCircle(pos, r, toU32(vertexBorder), 12, 1.0f);
            break;
    }
}

void ToolOverlayTheme::drawEdge(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, OverlayEdgeState state, float width) const {
    float w = (width > 0.0f) ? width : lineWidth;
    ImU32 col = toU32(edgeWireframe);
    float lw = w;
    switch (state) {
        case OverlayEdgeState::Hover:
            col = toU32(edgeHover);
            lw = w * 2.8f;
            break;
        case OverlayEdgeState::Selected:
            col = toU32(edgeSelected);
            lw = w * 2.8f;
            break;
        case OverlayEdgeState::SelectedHover:
            col = toU32(edgeSelectedHover);
            lw = w * 3.6f;
            break;
        case OverlayEdgeState::DeleteHover:
            col = toU32(edgeDelete);
            lw = w * 3.8f;
            break;
        case OverlayEdgeState::PreviewLoopCut:
            col = toU32(edgePreview);
            lw = w * 2.4f;
            break;
        case OverlayEdgeState::PreviewMultiCut:
            col = toU32(vertexMoving);
            lw = w * 2.2f;
            break;
        case OverlayEdgeState::Normal:
        default:
            col = toU32(edgeWireframe);
            lw = w;
            break;
    }
    dl->AddLine(p0, p1, col, lw);
}

void ToolOverlayTheme::drawFace(ImDrawList* dl, const ImVec2* pts, int count, OverlayFaceState state) const {
    if (!dl || !pts || count < 3) return;
    ImU32 fillCol = toU32(faceHoverFill);
    ImU32 lineCol = toU32(faceHoverOutline);
    float lineW = lineWidth * 2.0f;

    switch (state) {
        case OverlayFaceState::Selected:
            fillCol = toU32(faceHoverFill);
            lineCol = toU32(faceHoverOutline);
            break;
        case OverlayFaceState::DeleteHover:
            fillCol = toU32(faceDeleteFill);
            lineCol = toU32(faceDeleteOutline);
            lineW = lineWidth * 2.5f;
            break;
        case OverlayFaceState::PreviewCreate:
            fillCol = toU32(facePreviewFill);
            lineCol = toU32(edgePreview);
            break;
        case OverlayFaceState::Hover:
        default:
            fillCol = toU32(faceHoverFill);
            lineCol = toU32(faceHoverOutline);
            break;
    }

    dl->AddConvexPolyFilled(pts, count, fillCol);
    dl->AddPolyline(pts, count, lineCol, ImDrawFlags_Closed, lineW);
}

void ToolOverlayTheme::drawSettingsUI() {
    ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Unified Tool Overlays & Snapping Colors");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::SliderFloat("Default Point Size", &pointRadius, 2.0f, 12.0f, "%.1f px");
    ImGui::SliderFloat("Default Line Width", &lineWidth, 0.5f, 6.0f, "%.1f px");

    ImGui::Spacing();
    const ImGuiColorEditFlags flags = ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf;

    if (ImGui::CollapsingHeader("Vertex & Snapping Colors", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::ColorEdit4("Weld & Snap Target", (float*)&weldTargetFill, flags);
        ImGui::ColorEdit4("Weld Target Outer Ring", (float*)&weldTargetRing, flags);
        ImGui::ColorEdit4("Standard Vertex", (float*)&vertexNormal, flags);
        ImGui::ColorEdit4("Vertex Border", (float*)&vertexBorder, flags);
        ImGui::ColorEdit4("Hovered Vertex", (float*)&vertexHover, flags);
        ImGui::ColorEdit4("Extrudable Corner Vertex", (float*)&vertexCorner, flags);
        ImGui::ColorEdit4("Pinned Vertex", (float*)&vertexPinned, flags);
        ImGui::ColorEdit4("Moving Drag Vertex", (float*)&vertexMoving, flags);
        ImGui::ColorEdit4("Delete Hover Vertex", (float*)&vertexDelete, flags);
    }

    if (ImGui::CollapsingHeader("Edge & Wireframe Colors", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::ColorEdit4("Retopo Wireframe", (float*)&edgeWireframe, flags);
        ImGui::ColorEdit4("Selected Edge", (float*)&edgeSelected, flags);
        ImGui::ColorEdit4("Selected Hover Edge", (float*)&edgeSelectedHover, flags);
        ImGui::ColorEdit4("Hovered Edge", (float*)&edgeHover, flags);
        ImGui::ColorEdit4("Cut Preview Edge", (float*)&edgePreview, flags);
        ImGui::ColorEdit4("Delete Hover Edge", (float*)&edgeDelete, flags);
    }

    if (ImGui::CollapsingHeader("Face Highlight Colors")) {
        ImGui::ColorEdit4("Hover Face Fill", (float*)&faceHoverFill, flags);
        ImGui::ColorEdit4("Hover Face Outline", (float*)&faceHoverOutline, flags);
        ImGui::ColorEdit4("Delete Face Fill", (float*)&faceDeleteFill, flags);
        ImGui::ColorEdit4("Delete Face Outline", (float*)&faceDeleteOutline, flags);
        ImGui::ColorEdit4("Preview Create Face", (float*)&facePreviewFill, flags);
    }

    ImGui::Spacing();
    if (ImGui::Button("Reset Overlays to Defaults")) {
        resetToDefaults();
    }
}

} // namespace gui
