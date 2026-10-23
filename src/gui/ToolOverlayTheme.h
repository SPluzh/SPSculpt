#pragma once

#include <imgui.h>
#include <string>

class IniFile;

namespace gui {

enum class OverlayVertexState {
    Normal = 0,
    Hover,
    ExtrudableCorner,
    DeleteHover,
    Pinned,
    MovingDrag,
    WeldTarget
};

enum class OverlayEdgeState {
    Normal = 0,
    Hover,
    Selected,
    SelectedHover,
    DeleteHover,
    PreviewLoopCut,
    PreviewMultiCut
};

enum class OverlayFaceState {
    Hover = 0,
    Selected,
    DeleteHover,
    PreviewCreate
};

class ToolOverlayTheme {
public:
    static ToolOverlayTheme& get();

    // Reset all parameters to factory defaults
    void resetToDefaults();

    // Save and load via IniFile
    void save(IniFile& ini, const std::string& section = "ToolOverlay") const;
    void load(const IniFile& ini, const std::string& section = "ToolOverlay");

    // Unified UI drawing method for Preferences
    void drawSettingsUI();

    // High-level drawing helpers for viewport tool overlays
    void drawWeldMarker(ImDrawList* dl, const ImVec2& pos, float radius = -1.0f) const;
    void drawVertex(ImDrawList* dl, const ImVec2& pos, OverlayVertexState state, float radius = -1.0f) const;
    void drawEdge(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, OverlayEdgeState state, float width = -1.0f) const;
    void drawFace(ImDrawList* dl, const ImVec2* pts, int count, OverlayFaceState state) const;

    // Helper conversion to ImU32
    static ImU32 toU32(const ImVec4& col) {
        return ImGui::ColorConvertFloat4ToU32(col);
    }

public:
    // Sizing metrics
    float pointRadius{4.0f};
    float lineWidth{1.5f};

    // Vertex colors
    ImVec4 weldTargetFill{1.0f, 0.12f, 0.12f, 0.94f};       // Pulsing Red (weld & snap target)
    ImVec4 weldTargetRing{1.0f, 1.0f, 1.0f, 0.86f};          // Crisp White Outer Ring
    ImVec4 vertexNormal{0.94f, 0.94f, 0.94f, 0.86f};         // White / Light Grey
    ImVec4 vertexBorder{0.16f, 0.16f, 0.16f, 0.70f};         // Dark Outline
    ImVec4 vertexHover{1.0f, 0.90f, 0.16f, 1.0f};            // Bright Yellow
    ImVec4 vertexCorner{0.20f, 1.0f, 0.39f, 1.0f};           // Bright Green (Extrudable corner)
    ImVec4 vertexDelete{1.0f, 0.16f, 0.16f, 1.0f};           // Red (Delete hover)
    ImVec4 vertexPinned{0.12f, 0.86f, 1.0f, 0.94f};          // Cyan (Pinned / Frozen)
    ImVec4 vertexMoving{0.20f, 0.86f, 1.0f, 1.0f};           // Bright Cyan (Drag tweak)

    // Edge colors
    ImVec4 edgeWireframe{0.20f, 0.90f, 0.35f, 0.90f};        // Green Wireframe
    ImVec4 edgeSelected{1.0f, 0.59f, 0.0f, 1.0f};            // Orange Selection
    ImVec4 edgeSelectedHover{1.0f, 1.0f, 0.39f, 1.0f};       // Bright Yellow-Orange Hover
    ImVec4 edgeHover{1.0f, 0.90f, 0.16f, 1.0f};               // Yellow Hover
    ImVec4 edgeDelete{1.0f, 0.16f, 0.16f, 1.0f};              // Red Delete Hover
    ImVec4 edgePreview{0.20f, 1.0f, 0.47f, 1.0f};             // Emerald Green (Loop cut / Multi-cut)

    // Face highlight colors
    ImVec4 faceHoverFill{0.16f, 0.75f, 1.0f, 0.30f};         // Translucent Cyan
    ImVec4 faceHoverOutline{0.27f, 0.90f, 1.0f, 0.86f};      // Cyan Outline
    ImVec4 faceDeleteFill{1.0f, 0.16f, 0.16f, 0.35f};        // Translucent Red
    ImVec4 faceDeleteOutline{1.0f, 0.24f, 0.24f, 0.86f};     // Red Outline
    ImVec4 facePreviewFill{0.20f, 1.0f, 0.47f, 0.40f};       // Translucent Green (Quad create)

private:
    ToolOverlayTheme();
};

} // namespace gui
