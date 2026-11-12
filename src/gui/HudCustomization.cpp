#include <GLES3/gl3.h>
#include "gui/HudCustomization.h"
#include "gui/GuiManager.h"
#include "gui/IconsLucide.h"
#include "common/IniFile.h"
#include "common/StringUtils.h"
#include "editing/SculptManager.h"
#include "editing/PoseTool.h"
#include "editing/TubeTool.h"
#include "editing/QuadDrawTool.h"
#include "editing/MaskExtractTool.h"
#include "editing/ComponentSelectTool.h"
#include "editing/EdgeCreaseTool.h"
#include "editing/CurveFoldsTool.h"
#include "editing/undo/UndoManager.h"
#include "platform/FileDialog.h"
#include "platform/HotkeyManager.h"
#include "render/AngleRenderer.h"
#include "scene/Scene.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>

// ----------------------------------------------------------------------------
// HudRegistry
// ----------------------------------------------------------------------------

void HudRegistry::clear() {
    m_items.clear();
}

void HudRegistry::registerItem(HudItemDef def) {
    for (auto& existing : m_items) {
        if (existing.id == def.id) {
            existing = std::move(def);
            return;
        }
    }
    m_items.push_back(std::move(def));
}

const HudItemDef* HudRegistry::find(const std::string& id) const {
    for (const auto& item : m_items) {
        if (item.id == id) return &item;
    }
    return nullptr;
}

// ----------------------------------------------------------------------------
// HudLayoutConfig
// ----------------------------------------------------------------------------

HudLayoutConfig::HudLayoutConfig() {
    resetToDefault();
}

const char* HudLayoutConfig::islandIniKey(HudIslandId id) {
    switch (id) {
        case HudIslandId::TopLeft:  return "top_left";
        case HudIslandId::LeftVert: return "left_vert";
        case HudIslandId::TopRight: return "top_right";
        default:                    return "unknown";
    }
}

const char* HudLayoutConfig::islandDisplayName(HudIslandId id) {
    switch (id) {
        case HudIslandId::TopLeft:  return "Top-Left Tool Island";
        case HudIslandId::LeftVert: return "Left Vertical Island";
        case HudIslandId::TopRight: return "Top-Right Viewport Island";
        default:                    return "HUD Island";
    }
}

bool HudLayoutConfig::isIslandVertical(HudIslandId id) {
    return id == HudIslandId::LeftVert;
}

void HudLayoutConfig::resetToDefault() {
    m_islands[static_cast<int>(HudIslandId::TopLeft)] = {
        "active_tool",
        "alt_mode",
        "brush_settings"
    };

    m_islands[static_cast<int>(HudIslandId::LeftVert)] = {
        "app_menu",
        "outliner",
        "separator",
        "brush_sliders",
        "brush_sliders_hover",
        "separator",
        "voxel_remesh",
        "separator",
        "divider_tool",
        "measure_tool",
        "pose_tool",
        "tube_tool",
        "quad_draw_tool",
        "extract_tool",
        "curve_folds_tool",
        "comp_select_tool",
        "edge_crease_tool"
    };

    m_islands[static_cast<int>(HudIslandId::TopRight)] = {
        "symmetry",
        "symmetry_axes",
        "separator",
        "grid",
        "matcap",
        "pbr",
        "flat_shading",
        "wireframe",
        "dyntopo",
        "polygroups",
        "creases",
        "silhouette",
        "safe_frames",
        "xray",
        "perspective",
        "pan_zoom_2d",
        "split_view",
        "solo",
        "snapshot"
    };
}

void HudLayoutConfig::save(IniFile& ini) const {
    const std::string sec = "HudLayout";
    ini.setInt(sec, "version", 2);
    for (int i = 0; i < ISLAND_COUNT; ++i) {
        HudIslandId id = static_cast<HudIslandId>(i);
        const auto& vec = m_islands[i];
        std::string joined;
        for (size_t k = 0; k < vec.size(); ++k) {
            if (k > 0) joined += ',';
            joined += vec[k];
        }
        ini.set(sec, islandIniKey(id), joined);
    }
}

void HudLayoutConfig::load(const IniFile& ini, const HudRegistry& registry) {
    const std::string sec = "HudLayout";
    if (!ini.hasSection(sec)) {
        resetToDefault();
        return;
    }
    int version = ini.getInt(sec, "version", 1);

    for (int i = 0; i < ISLAND_COUNT; ++i) {
        HudIslandId id = static_cast<HudIslandId>(i);
        const char* key = islandIniKey(id);
        if (!ini.hasKey(sec, key)) continue;

        std::string raw = ini.get(sec, key, "");
        std::vector<std::string> parsed;
        std::stringstream ss(raw);
        std::string token;
        while (std::getline(ss, token, ',')) {
            size_t b = token.find_first_not_of(" \t\r\n");
            size_t e = token.find_last_not_of(" \t\r\n");
            if (b == std::string::npos) continue;
            token = token.substr(b, e - b + 1);
            if (registry.find(token) != nullptr) {
                parsed.push_back(token);
            }
        }
        m_islands[i] = std::move(parsed);
    }

    // Upgrade v1 layouts: insert the new hover brush-sliders button directly below brush_sliders
    if (version < 2 && !containsItem("brush_sliders_hover") && registry.find("brush_sliders_hover") != nullptr) {
        int slidersIdx = findItemIndex(HudIslandId::LeftVert, "brush_sliders");
        if (slidersIdx >= 0) {
            auto& leftVec = m_islands[static_cast<int>(HudIslandId::LeftVert)];
            leftVec.insert(leftVec.begin() + slidersIdx + 1, "brush_sliders_hover");
        }
    }
}

bool HudLayoutConfig::containsItem(const std::string& itemId) const {
    for (const auto& vec : m_islands) {
        for (const auto& id : vec) {
            if (id == itemId) return true;
        }
    }
    return false;
}

bool HudLayoutConfig::islandContains(HudIslandId id, const std::string& itemId) const {
    return findItemIndex(id, itemId) >= 0;
}

int HudLayoutConfig::findItemIndex(HudIslandId id, const std::string& itemId) const {
    int idx = static_cast<int>(id);
    if (idx < 0 || idx >= ISLAND_COUNT) return -1;
    const auto& vec = m_islands[idx];
    for (int i = 0; i < static_cast<int>(vec.size()); ++i) {
        if (vec[i] == itemId) return i;
    }
    return -1;
}

void HudLayoutConfig::applyOp(const PendingHudOp& op, const HudRegistry& registry) {
    auto validIsland = [](HudIslandId id) {
        int idx = static_cast<int>(id);
        return idx >= 0 && idx < ISLAND_COUNT;
    };

    if (op.type == PendingHudOp::Type::Remove) {
        if (!validIsland(op.srcIsland)) return;
        auto& vec = m_islands[static_cast<int>(op.srcIsland)];
        if (op.srcIndex >= 0 && op.srcIndex < static_cast<int>(vec.size())) {
            vec.erase(vec.begin() + op.srcIndex);
        } else if (!op.itemId.empty()) {
            int foundIdx = findItemIndex(op.srcIsland, op.itemId);
            if (foundIdx >= 0) vec.erase(vec.begin() + foundIdx);
        }
        return;
    }

    const HudItemDef* def = registry.find(op.itemId);
    if (!def || !validIsland(op.dstIsland)) return;

    bool dstVert = isIslandVertical(op.dstIsland);
    if (dstVert && !def->allowVertical) return;
    if (!dstVert && !def->allowHorizontal) return;

    if (op.type == PendingHudOp::Type::Insert) {
        auto& dstVec = m_islands[static_cast<int>(op.dstIsland)];
        if (!def->allowMultiple && islandContains(op.dstIsland, op.itemId)) {
            return;
        }
        int insertIdx = std::clamp(op.dstIndex, 0, static_cast<int>(dstVec.size()));
        dstVec.insert(dstVec.begin() + insertIdx, op.itemId);
        return;
    }

    if (op.type == PendingHudOp::Type::Move) {
        if (!validIsland(op.srcIsland)) return;
        auto& srcVec = m_islands[static_cast<int>(op.srcIsland)];
        auto& dstVec = m_islands[static_cast<int>(op.dstIsland)];
        if (op.srcIndex < 0 || op.srcIndex >= static_cast<int>(srcVec.size())) return;

        std::string movingId = srcVec[op.srcIndex];
        int targetIdx = std::clamp(op.dstIndex, 0, static_cast<int>(dstVec.size()));

        if (op.srcIsland == op.dstIsland) {
            if (targetIdx == op.srcIndex || targetIdx == op.srcIndex + 1) {
                return;
            }
            srcVec.erase(srcVec.begin() + op.srcIndex);
            if (targetIdx > op.srcIndex) targetIdx--;
            targetIdx = std::clamp(targetIdx, 0, static_cast<int>(dstVec.size()));
            dstVec.insert(dstVec.begin() + targetIdx, movingId);
        } else {
            srcVec.erase(srcVec.begin() + op.srcIndex);
            if (!def->allowMultiple && islandContains(op.dstIsland, movingId)) {
                return;
            }
            dstVec.insert(dstVec.begin() + targetIdx, movingId);
        }
    }
}

// ----------------------------------------------------------------------------
// Internal Shared HUD Button Rendering Helpers
// ----------------------------------------------------------------------------

namespace {

enum class HudAccent {
    None,
    Teal,
    Orange,
    Purple,
    Pink
};

void pushAccentColors(HudAccent accent) {
    switch (accent) {
        case HudAccent::Teal:
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.00f, 0.39f, 0.30f, 1.0f));
            break;
        case HudAccent::Orange:
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.45f, 0.05f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.55f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.35f, 0.00f, 1.0f));
            break;
        case HudAccent::Purple:
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.66f, 0.33f, 0.97f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.45f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.55f, 0.25f, 0.85f, 1.0f));
            break;
        case HudAccent::Pink:
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.15f, 0.55f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.25f, 0.65f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.70f, 0.10f, 0.45f, 1.0f));
            break;
        case HudAccent::None:
            break;
    }
}

void popAccentColors(HudAccent accent) {
    if (accent != HudAccent::None) {
        ImGui::PopStyleColor(3);
    }
}

bool drawHudIconButton(
    GLuint texId,
    const char* fallbackLabelWithId,
    const char* texBtnId,
    const ImVec2& btnSize,
    float scale,
    bool active,
    HudAccent activeAccent = HudAccent::Teal,
    float texPad = 1.0f)
{
    HudAccent accent = active ? activeAccent : HudAccent::None;
    pushAccentColors(accent);

    bool clicked = false;
    if (texId != 0) {
        clicked = ImGui::Button(texBtnId, btnSize);
        ImVec2 pMin = ImGui::GetItemRectMin();
        ImVec2 pMax = ImGui::GetItemRectMax();
        float pad = texPad * scale;
        ImVec2 imgMin(pMin.x + pad, pMin.y + pad);
        ImVec2 imgMax(pMax.x - pad, pMax.y - pad);
        ImU32 tintCol = active ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 240);
        ImGui::GetWindowDrawList()->AddImage(
            (ImTextureID)(uintptr_t)texId,
            imgMin, imgMax,
            ImVec2(0, 1), ImVec2(1, 0),
            tintCol);
    } else {
        clicked = ImGui::Button(fallbackLabelWithId, btnSize);
    }

    popAccentColors(accent);
    return clicked;
}

struct SplitBtnResult {
    bool mainClicked  = false;
    bool mainRightClicked = false;
    bool arrowClicked = false;
    ImVec2 rectMin{0.0f, 0.0f};
    ImVec2 rectMax{0.0f, 0.0f};
};

SplitBtnResult drawHudSplitButton(
    GLuint texId,
    const char* fallbackLabelWithId,
    const char* texBtnId,
    const char* arrowBtnId,
    const char* mainTooltip,
    const char* arrowTooltip,
    const HudRenderCtx& ctx,
    bool active,
    HudAccent activeAccent = HudAccent::Teal)
{
    SplitBtnResult res;
    HudAccent accent = active ? activeAccent : HudAccent::None;
    pushAccentColors(accent);

    ImVec2 squareBtn(ctx.squareSize, ctx.squareSize);

    if (ctx.isVertical) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    }

    if (texId != 0) {
        res.mainClicked = ImGui::Button(texBtnId, squareBtn);
        ImVec2 pMin = ImGui::GetItemRectMin();
        ImVec2 pMax = ImGui::GetItemRectMax();
        float pad = 1.0f * ctx.scale;
        ImGui::GetWindowDrawList()->AddImage(
            (ImTextureID)(uintptr_t)texId,
            ImVec2(pMin.x + pad, pMin.y + pad),
            ImVec2(pMax.x - pad, pMax.y - pad),
            ImVec2(0, 1), ImVec2(1, 0),
            active ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 240));
    } else {
        res.mainClicked = ImGui::Button(fallbackLabelWithId, squareBtn);
    }

    res.mainRightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    if (!ctx.customizeMode && mainTooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", mainTooltip);
    }
    res.rectMin = ImGui::GetItemRectMin();

    if (ctx.isVertical) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        ImGui::SetWindowFontScale(ctx.fontScale * 0.65f);
        res.arrowClicked = ImGui::Button(arrowBtnId, ImVec2(ctx.squareSize, ctx.squareSize * 0.45f));
        ImGui::SetWindowFontScale(ctx.fontScale);
        ImGui::PopStyleVar();
    } else {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
        ImGui::SetWindowFontScale(ctx.fontScale * 0.65f);
        res.arrowClicked = ImGui::Button(arrowBtnId, ImVec2(ctx.squareSize * 0.5f, ctx.squareSize));
        ImGui::SetWindowFontScale(ctx.fontScale);
        ImGui::PopStyleVar();
    }

    if (!ctx.customizeMode && arrowTooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", arrowTooltip);
    }
    res.rectMax = ImGui::GetItemRectMax();

    if (ctx.isVertical) {
        ImGui::PopStyleVar(); // ItemSpacing
    }
    popAccentColors(accent);
    return res;
}

void setPopupPosForSplit(const SplitBtnResult& res, const HudRenderCtx& ctx) {
    if (ctx.isVertical) {
        ImGui::SetNextWindowPos(
            ImVec2(res.rectMax.x + 6.0f * ctx.scale, res.rectMin.y),
            ImGuiCond_Appearing,
            ImVec2(0.0f, 0.0f));
    } else {
        float cx = (res.rectMin.x + res.rectMax.x) * 0.5f;
        float by = res.rectMax.y + 4.0f * ctx.scale;
        ImGui::SetNextWindowPos(ImVec2(cx, by), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
    }
}

void activateBrushSideEffects(GuiManager& gui, SculptManager& sculpt, Scene& scene, BrushType bType) {
    if (bType == BRUSH_TUBE) {
        gui.setShowSculptingPanel(true);
        if (auto* tube = sculpt.getTubeTool()) tube->onActivate(scene);
    } else if (bType == BRUSH_QUAD_DRAW) {
        gui.setShowSculptingPanel(true);
        if (auto* qd = sculpt.getQuadDrawTool()) qd->onActivate(scene);
    } else if (bType == BRUSH_EXTRACT) {
        gui.setShowSculptingPanel(true);
        if (auto* ext = sculpt.getMaskExtractTool()) {
            Mesh* sel = scene.getSelected();
            if (sel && !ext->isPreviewActive()) {
                ext->startPreview(scene, sel);
            }
        }
    } else if (bType == BRUSH_CURVE_FOLDS) {
        gui.setShowSculptingPanel(true);
        if (auto* cf = sculpt.getCurveFoldsTool()) cf->onActivate(scene);
    } else if (bType == BRUSH_COMPONENT_SELECT) {
        gui.setShowComponentPanel(true);
        if (auto* selTool = sculpt.getComponentSelectTool()) selTool->onActivate(scene);
    } else if (bType == BRUSH_EDGE_CREASE) {
        gui.setShowComponentPanel(true);
        if (auto* crTool = sculpt.getEdgeCreaseTool()) crTool->onActivate(scene);
    }
}

std::string makeBrushStableId(BrushType b) {
    switch (b) {
        case BRUSH_FLATTEN:            return "brush:flatten";
        case BRUSH_SMOOTH:             return "brush:smooth";
        case BRUSH_INFLATE:            return "brush:inflate";
        case BRUSH_PINCH:              return "brush:pinch";
        case BRUSH_CREASE:             return "brush:crease";
        case BRUSH_VTOOL:              return "brush:vtool";
        case BRUSH_MOVE:               return "brush:move";
        case BRUSH_DRAG:               return "brush:drag";
        case BRUSH_ELASTIC:            return "brush:elastic";
        case BRUSH_MASK:               return "brush:mask";
        case BRUSH_PAINT:              return "brush:paint";
        case BRUSH_TWIST:              return "brush:twist";
        case BRUSH_LOCALSCALE:         return "brush:localscale";
        case BRUSH_CLAY:               return "brush:clay";
        case BRUSH_CLAYBUILDUP:        return "brush:claybuildup";
        case BRUSH_DAMSTANDARD:        return "brush:damstandard";
        case BRUSH_SQUAREBRUSH:        return "brush:squarebrush";
        case BRUSH_VISIBILITY:         return "brush:visibility";
        case BRUSH_MASK_GRADIENT_BLUR: return "brush:mask_gradient_blur";
        case BRUSH_MEASURE:            return "measure_tool";
        case BRUSH_DIVIDER:            return "divider_tool";
        case BRUSH_TRANSFORM:          return "brush:transform";
        case BRUSH_ARMATURE_SPHERES:   return "brush:armature_spheres";
        case BRUSH_BRUSH:              return "brush:standard";
        case BRUSH_POLYGROUP:          return "brush:polygroup";
        case BRUSH_CLIP_CURVE:         return "brush:clip_curve";
        case BRUSH_TRIM:               return "brush:trim";
        case BRUSH_SPLIT:              return "brush:split";
        case BRUSH_DELETE_LAYER:       return "brush:delete_layer";
        case BRUSH_RELAX:              return "brush:relax";
        case BRUSH_POSE:               return "pose_tool";
        case BRUSH_SNAKEHOOK:          return "brush:snakehook";
        case BRUSH_TUBE:               return "tube_tool";
        case BRUSH_QUAD_DRAW:          return "quad_draw_tool";
        case BRUSH_EXTRACT:            return "extract_tool";
        case BRUSH_COMPONENT_SELECT:   return "comp_select_tool";
        case BRUSH_EDGE_CREASE:        return "edge_crease_tool";
        case BRUSH_CURVE_FOLDS:        return "curve_folds_tool";
        case BRUSH_RAKE:               return "brush:rake";
        case BRUSH_DYNAMIC_TOPOLOGY:   return "brush:dyntopo_tool";
        default:                       return "brush:unknown";
    }
}

std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

enum class BrushSliderKind {
    Radius = 0,
    Intensity,
    FocalShift
};

const char* brushSliderIcon(BrushSliderKind kind) {
    switch (kind) {
        case BrushSliderKind::Intensity:  return ICON_LC_ZAP;
        case BrushSliderKind::Radius:     return ICON_LC_CIRCLE;
        case BrushSliderKind::FocalShift: return ICON_LC_CIRCLE_DOT;
        default:                          return ICON_LC_SLIDERS_HORIZONTAL;
    }
}

void setBrushSliderTooltip(BrushSliderKind kind, const SculptManager& sculpt) {
    switch (kind) {
        case BrushSliderKind::Radius:
            ImGui::SetTooltip("Size: %.0f px", sculpt.getBrushRadius());
            break;
        case BrushSliderKind::Intensity:
            ImGui::SetTooltip("Intensity: %.0f%% (0%% to 1000%%)", sculpt.getBrushIntensity() * 100.0f);
            break;
        case BrushSliderKind::FocalShift:
            ImGui::SetTooltip("Focal Shift: %.0f%%", sculpt.getFocalShift() * 100.0f);
            break;
    }
}

struct BrushSliderResult {
    bool hovered = false;
    bool active  = false;
    ImVec2 rectMin{0.0f, 0.0f};
    ImVec2 rectMax{0.0f, 0.0f};
};

BrushSliderResult drawVerticalBrushSlider(
    SculptManager& sculpt,
    BrushSliderKind kind,
    const char* widgetId,
    const ImVec2& sliderSize,
    bool customizeMode)
{
    BrushSliderResult res;
    switch (kind) {
        case BrushSliderKind::Radius: {
            float radius = sculpt.getBrushRadius();
            if (ImGui::VSliderFloat(widgetId, sliderSize, &radius, 1.0f, 1000.0f, "")) {
                sculpt.setBrushRadius(radius);
            }
            res.hovered = ImGui::IsItemHovered();
            res.active = ImGui::IsItemActive();
            res.rectMin = ImGui::GetItemRectMin();
            res.rectMax = ImGui::GetItemRectMax();
            if (!customizeMode && res.hovered) {
                setBrushSliderTooltip(kind, sculpt);
                float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f) {
                    float step = (radius < 50.0f) ? 2.0f : 10.0f;
                    sculpt.setBrushRadius(std::clamp(radius + wheel * step, 1.0f, 1000.0f));
                }
            }
            break;
        }
        case BrushSliderKind::Intensity: {
            float intensityPct = sculpt.getBrushIntensity() * 100.0f;
            float maxHudPct = (intensityPct > 100.0f) ? std::min(1000.0f, std::max(100.0f, intensityPct)) : 100.0f;
            if (ImGui::VSliderFloat(widgetId, sliderSize, &intensityPct, 0.0f, maxHudPct, "")) {
                sculpt.setBrushIntensity(std::clamp(intensityPct / 100.0f, 0.0f, 10.0f));
            }
            res.hovered = ImGui::IsItemHovered();
            res.active = ImGui::IsItemActive();
            res.rectMin = ImGui::GetItemRectMin();
            res.rectMax = ImGui::GetItemRectMax();
            if (!customizeMode && res.active && ImGui::GetIO().MouseDelta.y < 0.0f && intensityPct >= maxHudPct - 0.1f) {
                intensityPct = std::min(1000.0f, intensityPct + 10.0f);
                sculpt.setBrushIntensity(intensityPct / 100.0f);
            }
            if (!customizeMode && res.hovered) {
                setBrushSliderTooltip(kind, sculpt);
                float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f) {
                    float stepPct = (intensityPct < 100.0f) ? 5.0f : 25.0f;
                    intensityPct = std::clamp(intensityPct + wheel * stepPct, 0.0f, 1000.0f);
                    sculpt.setBrushIntensity(intensityPct / 100.0f);
                }
            }
            break;
        }
        case BrushSliderKind::FocalShift: {
            float focalShift = sculpt.getFocalShift();
            if (ImGui::VSliderFloat(widgetId, sliderSize, &focalShift, -1.0f, 1.0f, "")) {
                sculpt.setFocalShift(focalShift);
            }
            res.hovered = ImGui::IsItemHovered();
            res.active = ImGui::IsItemActive();
            res.rectMin = ImGui::GetItemRectMin();
            res.rectMax = ImGui::GetItemRectMax();
            if (!customizeMode && res.hovered) {
                setBrushSliderTooltip(kind, sculpt);
                float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0f) {
                    sculpt.setFocalShift(std::clamp(focalShift + wheel * 0.05f, -1.0f, 1.0f));
                }
            }
            break;
        }
    }
    return res;
}

} // namespace

// ----------------------------------------------------------------------------
// GuiManager HUD Registry & Customization Methods
// ----------------------------------------------------------------------------

void GuiManager::enterHudCustomizeMode() {
    m_customizeHudMode = true;
    m_showHudCatalog = true;
    m_isOverBrushSliders = false;
}

void GuiManager::exitHudCustomizeMode() {
    m_customizeHudMode = false;
    m_showHudCatalog = false;
    m_hudHasLastActiveDrag = false;
    m_hudWasDragging = false;
}

void GuiManager::initHudRegistry() {
    if (m_hudRegistryInitialized) return;
    m_hudRegistryInitialized = true;
    m_hudRegistry.clear();

    // 1. Layout helpers (Separator & Customize HUD toggle)
    m_hudRegistry.registerItem({
        "separator", "Separator (Divider)", "Layout", "|", "",
        true, true, true,
        [](GuiManager&, SculptManager&, Scene&, AngleRenderer&, const HudRenderCtx& ctx) {
            if (ctx.isVertical) {
                ImGui::Separator();
            } else {
                ImGui::TextDisabled("|");
            }
        }
    });

    m_hudRegistry.registerItem({
        "customize_hud", "Customize HUD Buttons", "Layout", ICON_LC_LAYOUT_GRID, "",
        true, true, false,
        [](GuiManager& gui, SculptManager&, Scene&, AngleRenderer&, const HudRenderCtx& ctx) {
            bool active = gui.isHudCustomizeMode();
            if (drawHudIconButton(0, ICON_LC_LAYOUT_GRID "##hudCustomizeToggleBtn", "", ImVec2(ctx.squareSize, ctx.squareSize), ctx.scale, active, HudAccent::Orange)) {
                gui.toggleHudCustomizeMode();
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Customize HUD Buttons (Drag, Add, Remove)");
            }
        }
    });

    // 2. Top-Left Active Tool & Controls
    m_hudRegistry.registerItem({
        "active_tool", "Active Tool Selector (2x)", "Tools", ICON_LC_BRUSH, "",
        true, true, false,
        [](GuiManager& gui, SculptManager& sculpt, Scene& scene, AngleRenderer&, const HudRenderCtx& ctx) {
            float bigBtnSize = ctx.squareSize * 2.0f;
            BrushType activeBrush = sculpt.getBrush();
            const char* activeIconKey = getBrushIconKeyStr(activeBrush);
            GLuint activeIconTex = activeIconKey ? gui.getIconTexture(activeIconKey) : 0;
            const char* activeBrushName = getBrushName(activeBrush);

            bool clickedActive = ImGui::Button("##hudActiveToolBtnTop", ImVec2(bigBtnSize, bigBtnSize));
            if (activeIconTex != 0) {
                ImVec2 pMin = ImGui::GetItemRectMin();
                ImVec2 pMax = ImGui::GetItemRectMax();
                float pad = 3.0f * ctx.scale;
                ImGui::GetWindowDrawList()->AddImage(
                    (ImTextureID)(uintptr_t)activeIconTex,
                    ImVec2(pMin.x + pad, pMin.y + pad),
                    ImVec2(pMax.x - pad, pMax.y - pad),
                    ImVec2(0, 1), ImVec2(1, 0), IM_COL32(255, 255, 255, 255));
            } else {
                ImVec2 pMin = ImGui::GetItemRectMin();
                const char* label = (activeBrush == BRUSH_TUBE) ? ICON_LC_CYLINDER "\nTube" :
                                    (activeBrush == BRUSH_QUAD_DRAW) ? ICON_LC_SQUARE "\nQuad" :
                                    (activeBrush == BRUSH_EXTRACT) ? ICON_LC_LAYERS "\nExtract" :
                                    (activeBrush == BRUSH_COMPONENT_SELECT) ? ICON_LC_SQUARE_DASHED_MOUSE_POINTER "\nSelect" :
                                    (activeBrush == BRUSH_EDGE_CREASE) ? ICON_LC_SQUARE_SPLIT_VERTICAL "\nCrease" :
                                    (activeBrush == BRUSH_CURVE_FOLDS) ? ICON_LC_SPLINE "\nFolds" :
                                    (activeBrush == BRUSH_DYNAMIC_TOPOLOGY) ? ICON_LC_GRID "\nTopology" : activeBrushName;
                ImVec2 textSz = ImGui::CalcTextSize(label);
                ImU32 textCol = (activeBrush == BRUSH_DYNAMIC_TOPOLOGY) ? IM_COL32(168, 85, 247, 255) : IM_COL32(255, 255, 255, 240);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(pMin.x + (bigBtnSize - textSz.x) * 0.5f, pMin.y + (bigBtnSize - textSz.y) * 0.5f),
                    textCol, label);
            }

            if (clickedActive) {
                ImGui::OpenPopup("##hudActiveToolPopupTop");
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) {
                const char* hkStr = getBrushHotkeyStr(activeBrush);
                if (hkStr) {
                    ImGui::SetTooltip("Active Tool: %s [%s] (Click to switch)", activeBrushName, hkStr);
                } else {
                    ImGui::SetTooltip("Active Tool: %s (Click to switch)", activeBrushName);
                }
            }

            if (ImGui::BeginPopup("##hudActiveToolPopupTop")) {
                ImGui::SetWindowFontScale(ctx.fontScale);
                ImGui::TextUnformatted("Select Tool");
                ImGui::Separator();
                for (int i = 0; i < BRUSH_COUNT; i++) {
                    BrushType bType = static_cast<BrushType>(i);
                    bool isSelected = (activeBrush == bType);
                    const char* iconKey = getBrushIconKeyStr(bType);
                    GLuint iconTex = iconKey ? gui.getIconTexture(iconKey) : 0;
                    if (iconTex != 0) {
                        float icH = ImGui::GetFrameHeight();
                        ImGui::Image((ImTextureID)(uintptr_t)iconTex, ImVec2(icH, icH), ImVec2(0, 1), ImVec2(1, 0));
                        ImGui::SameLine();
                    }
                    const char* hk = getBrushHotkeyStr(bType);
                    std::string itemLabel = hk ? (std::string(getBrushName(bType)) + " [" + hk + "]") : getBrushName(bType);
                    if (ImGui::Selectable(itemLabel.c_str(), isSelected)) {
                        sculpt.setBrush(bType);
                        activateBrushSideEffects(gui, sculpt, scene, bType);
                    }
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndPopup();
            }
        }
    });

    m_hudRegistry.registerItem({
        "alt_mode", "Alt (Invert Brush)", "Tools", "Alt", "",
        true, true, false,
        [](GuiManager& gui, SculptManager& sculpt, Scene& scene, AngleRenderer&, const HudRenderCtx& ctx) {
            bool isAltPhysicallyPressed = ImGui::GetIO().KeyAlt || ((SDL_GetModState() & KMOD_ALT) != 0);
            bool isCtrlPhysicallyPressed = ImGui::GetIO().KeyCtrl || ((SDL_GetModState() & KMOD_CTRL) != 0);
            bool isMayaNav = gui.m_hotkeyManager ? gui.m_hotkeyManager->getModifierBehavior().ctrlAltInvertsBrush
                                                 : (scene.getCamera().getNavigationStyle() == CameraEnums::NavigationStyle::MAYA);
            bool isAltModifierActive = isMayaNav ? (isCtrlPhysicallyPressed && isAltPhysicallyPressed) : isAltPhysicallyPressed;
            bool isAltActive = isAltModifierActive || sculpt.getNegative();

            if (drawHudIconButton(0, "Alt##hudAltBtnTop", "", ImVec2(ctx.squareSize, ctx.squareSize), ctx.scale, isAltActive)) {
                sculpt.toggleNegative();
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) {
                ImGui::SetTooltip(isMayaNav ? "Invert Brush Mode (Ctrl+Alt / N)" : "Invert Brush Mode (Alt / N)");
            }
        }
    });

    m_hudRegistry.registerItem({
        "brush_settings", "Brush & Sculpting Settings", "Panels", ICON_LC_SLIDERS, "",
        true, true, false,
        [](GuiManager& gui, SculptManager&, Scene&, AngleRenderer&, const HudRenderCtx& ctx) {
            if (drawHudIconButton(0, ICON_LC_SLIDERS "##hudBrushSettingsBtnTop", "", ImVec2(ctx.squareSize, ctx.squareSize), ctx.scale, gui.m_showSculptingPanel)) {
                gui.m_showSculptingPanel = !gui.m_showSculptingPanel;
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Brush & Sculpting Settings Panel");
            }
        }
    });

    // 3. Left Vertical Island Core Items
    m_hudRegistry.registerItem({
        "app_menu", "Main Menu", "Panels", ICON_LC_MENU, "",
        true, true, false,
        [](GuiManager& gui, SculptManager& sculpt, Scene& scene, AngleRenderer& renderer, const HudRenderCtx& ctx) {
            if (ImGui::Button(ICON_LC_MENU "##hudVertAppMenu", ImVec2(ctx.squareSize, ctx.squareSize))) {
                ImGui::OpenPopup("##hudVertAppMenuPopup");
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Main Menu (File, Panels, Options)");
            }
            if (ImGui::BeginPopup("##hudVertAppMenuPopup")) {
                ImGui::SetWindowFontScale(ctx.fontScale);
                gui.drawAppMenuItems(sculpt, scene, renderer);
                ImGui::Separator();
                if (ImGui::MenuItem(gui.isHudCustomizeMode() ? ICON_LC_CHECK " Finish Customizing HUD"
                                                             : ICON_LC_LAYOUT_GRID " Customize HUD Buttons...",
                                    nullptr, gui.isHudCustomizeMode())) {
                    gui.toggleHudCustomizeMode();
                }
                ImGui::EndPopup();
            }
        }
    });

    m_hudRegistry.registerItem({
        "outliner", "Scene Outliner", "Panels", ICON_LC_LIST, "",
        true, true, false,
        [](GuiManager& gui, SculptManager&, Scene&, AngleRenderer&, const HudRenderCtx& ctx) {
            if (drawHudIconButton(0, ICON_LC_LIST "##hudVertOutlinerBtn", "", ImVec2(ctx.squareSize, ctx.squareSize), ctx.scale, gui.m_showScenePanel)) {
                gui.m_showScenePanel = !gui.m_showScenePanel;
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Scene Outliner");
            }
        }
    });

    m_hudRegistry.registerItem({
        "brush_sliders", "Brush Sliders (Size / Intensity / Focal)", "Tools", ICON_LC_SLIDERS_HORIZONTAL, "",
        false, true, false,
        [](GuiManager& gui, SculptManager& sculpt, Scene&, AngleRenderer&, const HudRenderCtx& ctx) {
            float vSliderH = 80.0f * ctx.scale;
            ImVec2 sSize(ctx.squareSize, vSliderH);

            BrushSliderResult rad = drawVerticalBrushSlider(sculpt, BrushSliderKind::Radius, "##hudVertRadius", sSize, ctx.customizeMode);
            ImGui::Spacing();
            BrushSliderResult inten = drawVerticalBrushSlider(sculpt, BrushSliderKind::Intensity, "##hudVertIntensity", sSize, ctx.customizeMode);
            ImGui::Spacing();
            BrushSliderResult focal = drawVerticalBrushSlider(sculpt, BrushSliderKind::FocalShift, "##hudVertFocalShift", sSize, ctx.customizeMode);

            gui.m_sliderRadiusMin = rad.rectMin;
            gui.m_sliderRadiusMax = rad.rectMax;
            gui.m_sliderIntensityMin = inten.rectMin;
            gui.m_sliderIntensityMax = inten.rectMax;
            gui.m_sliderFocalShiftMin = focal.rectMin;
            gui.m_sliderFocalShiftMax = focal.rectMax;
            gui.m_slidersValid = !ctx.customizeMode;
            if (!ctx.customizeMode &&
                (rad.hovered || rad.active || inten.hovered || inten.active || focal.hovered || focal.active)) {
                gui.m_isOverBrushSliders = true;
            }
        }
    });

    m_hudRegistry.registerItem({
        "brush_sliders_hover", "Brush Sliders (Hover Button)", "Tools", ICON_LC_SLIDERS_HORIZONTAL, "",
        true, true, false,
        [](GuiManager& gui, SculptManager&, Scene&, AngleRenderer&, const HudRenderCtx& ctx) {
            bool popOpen = !ctx.customizeMode && gui.m_brushHoverPopoverOpen;
            drawHudIconButton(0, ICON_LC_SLIDERS_HORIZONTAL "##hudBrushSlidersHoverBtn", "",
                              ImVec2(ctx.squareSize, ctx.squareSize), ctx.scale, popOpen);
            if (!ctx.customizeMode) {
                gui.m_brushHoverBtnDrawnThisFrame = true;
                gui.m_brushHoverBtnMin = ImGui::GetItemRectMin();
                gui.m_brushHoverBtnMax = ImGui::GetItemRectMax();
                gui.m_brushHoverIsland = ctx.islandId;
                gui.m_brushHoverBtnHovered = ImGui::IsItemHovered();
            }
        }
    });

    m_hudRegistry.registerItem({
        "voxel_remesh", "Voxel Remesh", "Tools", ICON_LC_BOXES, "voxelremesh",
        true, true, false,
        [](GuiManager& gui, SculptManager&, Scene& scene, AngleRenderer&, const HudRenderCtx& ctx) {
            bool runningRemesh = gui.isRemeshRunning();
            GLuint vrTex = gui.getIconTexture("voxelremesh");
            char tip[64];
            snprintf(tip, sizeof(tip), "Voxel Remesh (Resolution: %d)", gui.m_remeshResolution);

            SplitBtnResult res = drawHudSplitButton(
                vrTex,
                ICON_LC_BOXES "##hudVertVoxelRemesh",
                "##hudVertVoxelRemesh",
                ICON_LC_CHEVRON_DOWN "##hudVertRemeshArrow",
                tip, "Remesh Settings", ctx, runningRemesh, HudAccent::Orange);

            if (res.mainClicked) {
                gui.performRemesh(scene);
            }
            if (res.arrowClicked || res.mainRightClicked) {
                ImGui::OpenPopup("##hudVertRemeshPopup");
            }
            setPopupPosForSplit(res, ctx);
            if (ImGui::BeginPopup("##hudVertRemeshPopup")) {
                ImGui::SetWindowFontScale(ctx.fontScale);
                ImGui::TextUnformatted("Voxel Remesh");
                ImGui::Separator();
                ImGui::Text("Resolution: %d", gui.m_remeshResolution);
                ImGui::PushItemWidth(140.0f * ctx.scale);
                ImGui::SliderInt("##hudRemeshResSlider", &gui.m_remeshResolution, 10, 3000);
                if (ImGui::IsItemActive()) {
                    Mesh* selectedMesh = scene.getSelected();
                    if (selectedMesh) {
                        float step = selectedMesh->computeWorldStep(gui.m_remeshResolution);
                        scene.updateVoxelPreview(step, {selectedMesh});
                    }
                } else if (ImGui::IsItemDeactivated()) {
                    scene.updateVoxelPreview(0.0f, {});
                }
                ImGui::PopItemWidth();
                renderRemeshEstimationUI(scene.getSelected(), gui.m_remeshResolution, gui.m_remeshKeepSharpEdges);
                ImGui::Checkbox("Keep PolyGroups", &gui.m_remeshKeepPolyGroups);
                ImGui::Checkbox("Keep Masked", &gui.m_remeshKeepMasked);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Preserves the masked geometry untouched with original vertices and topology,\n"
                                      "while remeshing only the unmasked regions.");
                }
                ImGui::Checkbox("Align Symmetry Axes", &gui.m_remeshAlignSymmetry);

                bool regQuadsDisabled = gui.m_remeshKeepSharpEdges;
                if (regQuadsDisabled) ImGui::BeginDisabled();
                ImGui::Checkbox("Regularize Quads", &gui.m_remeshRegularizeQuads);
                if (regQuadsDisabled) {
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip("Unavailable when 'Keep sharp edges' is enabled.\n"
                                          "PolyCube 6-Chart regularization would round off and destroy sharp features.\n"
                                          "Surface Nets with QEF feature snapping is used instead.");
                    }
                } else if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Eliminates diamond/rhombus polygon artifacts using QEF Hermite positioning and constrained tangential relaxation.");
                }

                ImGui::Checkbox("Keep sharp edges", &gui.m_remeshKeepSharpEdges);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("This option is mostly useful for simple primitive boolean operations.\n"
                                      "It can introduce distortion in some areas because points are snapped to the edges.\n"
                                      "Note: Disables 'Regularize Quads' (PolyCube 6-Chart) to prevent rounding sharp creases.");
                }
                ImGui::Separator();
                if (ImGui::Button("Remesh Now", ImVec2(-1.0f, 0.0f))) {
                    scene.updateVoxelPreview(0.0f, {});
                    gui.performRemesh(scene);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
    });

    m_hudRegistry.registerItem({
        "divider_tool", "Divider Tool", "Tools", ICON_LC_RULER, "dividertool",
        true, true, false,
        [](GuiManager& gui, SculptManager& sculpt, Scene&, AngleRenderer&, const HudRenderCtx& ctx) {
            bool isDividerActive = (sculpt.getBrush() == BRUSH_DIVIDER);
            GLuint dividerTex = gui.getIconTexture("dividertool");
            char tip[64];
            snprintf(tip, sizeof(tip), "Divider Tool (Divisions: %d)", sculpt.getDividerDivisions());

            SplitBtnResult res = drawHudSplitButton(
                dividerTex,
                ICON_LC_RULER "##hudVertDividerTool",
                "##hudVertDividerTool",
                ICON_LC_CHEVRON_DOWN "##hudVertDividerArrow",
                tip, "Divider Settings", ctx, isDividerActive);

            if (res.mainClicked) {
                sculpt.setBrush(BRUSH_DIVIDER);
            }
            if (res.arrowClicked || res.mainRightClicked) {
                ImGui::OpenPopup("##hudVertDividerPopup");
            }
            setPopupPosForSplit(res, ctx);
            if (ImGui::BeginPopup("##hudVertDividerPopup")) {
                ImGui::SetWindowFontScale(ctx.fontScale);
                ImGui::TextUnformatted("Divider Settings");
                ImGui::Separator();

                int divs = sculpt.getDividerDivisions();
                ImGui::Text("Divisions / Segments: %d", divs);
                ImGui::PushItemWidth(140.0f * ctx.scale);
                if (ImGui::SliderInt("##hudDividerDivsSlider", &divs, 2, 10)) {
                    sculpt.setDividerDivisions(divs);
                }
                ImGui::PopItemWidth();

                bool useDist = sculpt.getMeasureUseDistanceThickness();
                if (ImGui::Checkbox("Perspective Thickness", &useDist)) {
                    sculpt.setMeasureUseDistanceThickness(useDist);
                }

                ImGui::Separator();
                if (ImGui::Button("Clear Dividers", ImVec2(-1.0f, 0.0f))) {
                    sculpt.clearMeasurements();
                }
                ImGui::EndPopup();
            }
        }
    });

    auto registerSimpleBrushTool = [this](const std::string& id, const std::string& label, BrushType bType,
                                          const char* iconKey, const char* fallbackIcon, const char* btnId, const char* tooltip) {
        std::string texBtn = std::string("##") + btnId;
        std::string fallbackBtn = std::string(fallbackIcon) + "##" + btnId;
        m_hudRegistry.registerItem({
            id, label, "Tools", fallbackIcon, iconKey ? iconKey : "",
            true, true, false,
            [bType, iconKey, texBtn, fallbackBtn, tooltip](GuiManager& gui, SculptManager& sculpt, Scene& scene, AngleRenderer&, const HudRenderCtx& ctx) {
                bool active = (sculpt.getBrush() == bType);
                GLuint tex = iconKey ? gui.getIconTexture(iconKey) : 0;
                if (drawHudIconButton(tex, fallbackBtn.c_str(), texBtn.c_str(), ImVec2(ctx.squareSize, ctx.squareSize), ctx.scale, active)) {
                    sculpt.setBrush(bType);
                    activateBrushSideEffects(gui, sculpt, scene, bType);
                }
                if (!ctx.customizeMode && ImGui::IsItemHovered() && tooltip) {
                    ImGui::SetTooltip("%s", tooltip);
                }
            }
        });
    };

    registerSimpleBrushTool("measure_tool", "Measure Tool", BRUSH_MEASURE, "measuretool", ICON_LC_RULER, "hudVertMeasureTool", "Measure Tool");
    registerSimpleBrushTool("pose_tool", "Pose Tool (Transpose)", BRUSH_POSE, "posetool", ICON_LC_MOVE_3D, "hudVertPoseTool", "Pose Tool (Transpose)");
    registerSimpleBrushTool("tube_tool", "Tube / Curve Tool", BRUSH_TUBE, "tubetool", ICON_LC_CYLINDER, "hudVertTubeTool",
                            "Tube / Curve Tool\nDraw parametric cylinders and curves along surfaces or in air");
    registerSimpleBrushTool("quad_draw_tool", "Quad Draw Retopology", BRUSH_QUAD_DRAW, "quaddraw", ICON_LC_SQUARE, "hudVertQuadDrawTool",
                            "Quad Draw Retopology Tool\nInteractive surface retopology tool with snapping, extrude, knife, and relax");
    registerSimpleBrushTool("extract_tool", "Mask Extract Tool", BRUSH_EXTRACT, "extract_tool", ICON_LC_LAYERS, "hudVertExtractTool",
                            "Mask Extract Tool\nExtract clothing, armor, and panels from painted masks with adjustable thickness and live 3D manipulator");
    registerSimpleBrushTool("curve_folds_tool", "Curve Folds Tool", BRUSH_CURVE_FOLDS, nullptr, ICON_LC_SPLINE, "hudVertCurveFoldsTool",
                            "Curve Folds Tool\nDraw and sculpt cloth folds, creases, and ridges along 3D splines with live preview");
    registerSimpleBrushTool("comp_select_tool", "Component Selection", BRUSH_COMPONENT_SELECT, nullptr, ICON_LC_SQUARE_DASHED_MOUSE_POINTER, "hudVertCompSelectTool",
                            "Component Selection Tool\nSelect vertices, edges, loops, and polygons for editing or transformation");
    registerSimpleBrushTool("edge_crease_tool", "Edge Crease Tool", BRUSH_EDGE_CREASE, nullptr, ICON_LC_SQUARE_SPLIT_VERTICAL, "hudVertEdgeCreaseTool",
                            "Edge Crease Tool\nTurn on/off creases on selected edges and interactively adjust Crease Level for Dynamic Subdivision");

    // 4. Top-Right Viewport & Symmetry Items
    m_hudRegistry.registerItem({
        "symmetry", "Symmetry Toggle & Mirror", "Symmetry", ICON_LC_SPLIT, "symmetry",
        true, true, false,
        [](GuiManager& gui, SculptManager& sculpt, Scene& scene, AngleRenderer& renderer, const HudRenderCtx& ctx) {
            bool useSym = sculpt.getUseSym();
            GLuint symTex = gui.getIconTexture("symmetry");
            SplitBtnResult res = drawHudSplitButton(
                symTex,
                ICON_LC_SPLIT "##hudSym",
                "##hudSym",
                ICON_LC_CHEVRON_DOWN "##hudSymArrow",
                "Toggle Symmetry (Alt+X)", "Symmetry Settings, Flip & Mirror", ctx, useSym);

            if (res.mainClicked) {
                sculpt.setUseSym(!useSym);
            }
            if (res.arrowClicked || res.mainRightClicked) {
                ImGui::OpenPopup("##hudSymPopup");
            }
            setPopupPosForSplit(res, ctx);
            if (ImGui::BeginPopup("##hudSymPopup")) {
                ImGui::SetWindowFontScale(ctx.fontScale);

                bool symEnabled = sculpt.getUseSym();
                if (ImGui::Checkbox("Enable Symmetry", &symEnabled)) {
                    sculpt.setUseSym(symEnabled);
                }

                ImGui::Separator();
                ImGui::TextUnformatted("Symmetry Space:");
                SymmetryMode symModePopup = sculpt.getSymmetryMode();
                int modeIdx = (symModePopup == SymmetryMode::World) ? 1 : 0;
                if (ImGui::RadioButton("Local Space", &modeIdx, 0)) sculpt.setSymmetryMode(SymmetryMode::Local);
                ImGui::SameLine();
                if (ImGui::RadioButton("World Space", &modeIdx, 1)) sculpt.setSymmetryMode(SymmetryMode::World);

                ImGui::Separator();
                ImGui::TextUnformatted("Active Axes:");
                bool symXPop = sculpt.getSymX();
                bool symYPop = sculpt.getSymY();
                bool symZPop = sculpt.getSymZ();
                if (ImGui::Checkbox("X", &symXPop)) {
                    sculpt.setSymX(symXPop);
                    sculpt.saveSymmetryToMesh(scene.getSelected());
                }
                ImGui::SameLine();
                if (ImGui::Checkbox("Y", &symYPop)) {
                    sculpt.setSymY(symYPop);
                    sculpt.saveSymmetryToMesh(scene.getSelected());
                }
                ImGui::SameLine();
                if (ImGui::Checkbox("Z", &symZPop)) {
                    sculpt.setSymZ(symZPop);
                    sculpt.saveSymmetryToMesh(scene.getSelected());
                }

                ImGui::Separator();
                ImGui::TextUnformatted("Radial Symmetry:");
                bool radEnabledPop = sculpt.getSymRadialEnabled();
                if (ImGui::Checkbox("Enable Radial", &radEnabledPop)) {
                    sculpt.setSymRadialEnabled(radEnabledPop);
                    if (radEnabledPop && !sculpt.getUseSym()) {
                        sculpt.setUseSym(true);
                    }
                    sculpt.saveSymmetryToMesh(scene.getSelected());
                }
                int radAxisPop = sculpt.getSymRadialAxis();
                ImGui::TextUnformatted("Axis:");
                ImGui::SameLine();
                if (ImGui::RadioButton("X##hudRadAxis", &radAxisPop, 0)) {
                    sculpt.setSymRadialAxis(0);
                    sculpt.saveSymmetryToMesh(scene.getSelected());
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Y##hudRadAxis", &radAxisPop, 1)) {
                    sculpt.setSymRadialAxis(1);
                    sculpt.saveSymmetryToMesh(scene.getSelected());
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Z##hudRadAxis", &radAxisPop, 2)) {
                    sculpt.setSymRadialAxis(2);
                    sculpt.saveSymmetryToMesh(scene.getSelected());
                }
                int radCountPop = sculpt.getSymRadialCount();
                ImGui::PushItemWidth(160.0f * ctx.scale);
                if (ImGui::SliderInt("Radial Count", &radCountPop, 2, 64)) {
                    sculpt.setSymRadialCount(radCountPop);
                    sculpt.saveSymmetryToMesh(scene.getSelected());
                }
                ImGui::PopItemWidth();
                const int kRadPresets[] = { 4, 6, 8, 12, 16 };
                for (int pIdx = 0; pIdx < 5; ++pIdx) {
                    if (pIdx > 0) ImGui::SameLine();
                    char pBuf[16];
                    snprintf(pBuf, sizeof(pBuf), "%d##radP", kRadPresets[pIdx]);
                    if (ImGui::SmallButton(pBuf)) {
                        sculpt.setSymRadialCount(kRadPresets[pIdx]);
                        if (!sculpt.getSymRadialEnabled()) sculpt.setSymRadialEnabled(true);
                        if (!sculpt.getUseSym()) sculpt.setUseSym(true);
                        sculpt.saveSymmetryToMesh(scene.getSelected());
                    }
                }

                ImGui::Separator();
                bool showSymLine = renderer.getShowSymmetryLine();
                if (ImGui::Checkbox("Show Symmetry Line", &showSymLine)) {
                    renderer.setShowSymmetryLine(showSymLine);
                }
                if (showSymLine) {
                    ImGui::Indent();
                    float symLineWidth = renderer.getSymmetryLineWidth();
                    ImGui::PushItemWidth(140.0f * ctx.scale);
                    if (ImGui::SliderFloat("Line Width", &symLineWidth, 0.01f, 0.5f, "%.3f")) {
                        renderer.setSymmetryLineWidth(symLineWidth);
                    }
                    ImGui::PopItemWidth();
                    ImGui::Unindent();
                }

                Mesh* activeMesh = scene.getSelected();
                if (activeMesh) {
                    float symOffset = activeMesh->getSymmetryOffset();
                    ImGui::PushItemWidth(160.0f * ctx.scale);
                    if (ImGui::SliderFloat("Symmetry Offset", &symOffset, -1.0f, 1.0f, "%.3f")) {
                        activeMesh->setSymmetryOffset(symOffset);
                    }
                    ImGui::PopItemWidth();
                }

                ImGui::Separator();
                ImGui::TextUnformatted("Flip Object:");
                float btnWidth = 65.0f * ctx.scale;
                for (int axis = 0; axis < 3; ++axis) {
                    if (axis > 0) ImGui::SameLine();
                    const char* lbl = (axis == 0) ? "Flip X" : (axis == 1) ? "Flip Y" : "Flip Z";
                    if (ImGui::Button(lbl, ImVec2(btnWidth, 24.0f * ctx.scale))) {
                        std::vector<Mesh*> selected = scene.getSelectedMeshes();
                        if (selected.empty() && scene.getSelected()) selected.push_back(scene.getSelected());
                        if (!selected.empty()) {
                            scene.pushHistoryState();
                            for (Mesh* m : selected) if (m) m->flip(axis);
                        }
                    }
                }

                ImGui::Separator();
                ImGui::TextUnformatted("Mirror Object:");
                ImGui::RadioButton("X##hudMirror", &gui.m_mirrorAxis, 0); ImGui::SameLine();
                ImGui::RadioButton("Y##hudMirror", &gui.m_mirrorAxis, 1); ImGui::SameLine();
                ImGui::RadioButton("Z##hudMirror", &gui.m_mirrorAxis, 2);

                int dirIdx = gui.m_mirrorPositiveToNegative ? 0 : 1;
                if (ImGui::RadioButton("+ to -", &dirIdx, 0)) gui.m_mirrorPositiveToNegative = true;
                ImGui::SameLine();
                if (ImGui::RadioButton("- to +", &dirIdx, 1)) gui.m_mirrorPositiveToNegative = false;

                if (ImGui::Button("Mirror Object", ImVec2(-1.0f, 26.0f * ctx.scale))) {
                    std::vector<Mesh*> selected = scene.getSelectedMeshes();
                    if (selected.empty() && scene.getSelected()) selected.push_back(scene.getSelected());
                    if (!selected.empty()) {
                        scene.pushHistoryState();
                        for (Mesh* m : selected) {
                            if (m) m->mirror(gui.m_mirrorAxis, gui.m_mirrorPositiveToNegative, sculpt.getSymmetryMode());
                        }
                    }
                }
                ImGui::EndPopup();
            }
        }
    });

    m_hudRegistry.registerItem({
        "symmetry_axes", "Symmetry Axes (W/L, X, Y, Z, R)", "Symmetry", "XYZ", "",
        true, true, false,
        [](GuiManager&, SculptManager& sculpt, Scene& scene, AngleRenderer&, const HudRenderCtx& ctx) {
            ImVec2 halfSquareBtn(ctx.squareSize * 0.5f, ctx.squareSize);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));

            bool useSym = sculpt.getUseSym();
            bool isWorldSym = (sculpt.getSymmetryMode() == SymmetryMode::World);
            if (drawHudIconButton(0, isWorldSym ? "W##hudSymMode" : "L##hudSymMode", "", halfSquareBtn, ctx.scale, useSym)) {
                sculpt.setSymmetryMode(isWorldSym ? SymmetryMode::Local : SymmetryMode::World);
                sculpt.saveSymmetryToMesh(scene.getSelected());
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Symmetry Space: %s (Click to toggle)", isWorldSym ? "World" : "Local");
            }

            if (!ctx.isVertical) ImGui::SameLine();
            bool symX = sculpt.getSymX();
            if (drawHudIconButton(0, "X##hudSymX", "", halfSquareBtn, ctx.scale, symX)) {
                sculpt.setSymX(!symX);
                sculpt.saveSymmetryToMesh(scene.getSelected());
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle X Symmetry");

            if (!ctx.isVertical) ImGui::SameLine();
            bool symY = sculpt.getSymY();
            if (drawHudIconButton(0, "Y##hudSymY", "", halfSquareBtn, ctx.scale, symY)) {
                sculpt.setSymY(!symY);
                sculpt.saveSymmetryToMesh(scene.getSelected());
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Y Symmetry");

            if (!ctx.isVertical) ImGui::SameLine();
            bool symZ = sculpt.getSymZ();
            if (drawHudIconButton(0, "Z##hudSymZ", "", halfSquareBtn, ctx.scale, symZ)) {
                sculpt.setSymZ(!symZ);
                sculpt.saveSymmetryToMesh(scene.getSelected());
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Z Symmetry");

            if (!ctx.isVertical) ImGui::SameLine();
            bool radOn = sculpt.getSymRadialEnabled();
            const char* radAxisNames[] = { "X", "Y", "Z" };
            int rAx = std::clamp(sculpt.getSymRadialAxis(), 0, 2);
            char radLbl[32];
            if (radOn) {
                snprintf(radLbl, sizeof(radLbl), "R%d%s##hudSymRad", sculpt.getSymRadialCount(), radAxisNames[rAx]);
            } else {
                snprintf(radLbl, sizeof(radLbl), "R##hudSymRad");
            }
            ImVec2 radBtnSz(radOn ? ctx.squareSize * 1.15f : halfSquareBtn.x, ctx.squareSize);
            if (drawHudIconButton(0, radLbl, "", radBtnSz, ctx.scale, radOn)) {
                sculpt.setSymRadialEnabled(!radOn);
                if (!radOn && !sculpt.getUseSym()) {
                    sculpt.setUseSym(true);
                }
                sculpt.saveSymmetryToMesh(scene.getSelected());
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Radial Symmetry: %s (Count %d, Axis %s, Alt+R)\nRight-click Symmetry button for settings",
                                  radOn ? "On" : "Off", sculpt.getSymRadialCount(), radAxisNames[rAx]);
            }

            ImGui::PopStyleVar(1);
        }
    });

    m_hudRegistry.registerItem({
        "grid", "Floor Grid", "Viewport", ICON_LC_GRID, "grid",
        true, true, false,
        [](GuiManager& gui, SculptManager&, Scene&, AngleRenderer& renderer, const HudRenderCtx& ctx) {
            bool showGrid = renderer.getShowGrid();
            GLuint gridTex = gui.getIconTexture("grid");
            SplitBtnResult res = drawHudSplitButton(
                gridTex,
                ICON_LC_GRID "##hudGrid",
                "##hudGrid",
                ICON_LC_CHEVRON_DOWN "##hudGridArrow",
                "Toggle Grid Display (Right-click for settings)", "Grid Presets & Settings", ctx, showGrid);

            if (res.mainClicked) {
                renderer.setShowGrid(!showGrid);
            }
            if (res.arrowClicked || res.mainRightClicked) {
                ImGui::OpenPopup("##hudGridPopup");
            }
            setPopupPosForSplit(res, ctx);
            if (ImGui::BeginPopup("##hudGridPopup")) {
                ImGui::SetWindowFontScale(ctx.fontScale);
                ImGui::TextUnformatted("Floor Grid Settings");
                ImGui::Separator();

                GridSettings& gs = renderer.getGridSettings();
                bool enable = gs.showGrid;
                if (ImGui::Checkbox("Enable Grid##hud", &enable)) renderer.setShowGrid(enable);

                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Geometry & Extent");

                const char* modeItems[] = { "Bounded Podium", "Infinite Floor" };
                int modeIdx = gs.infiniteGrid ? 1 : 0;
                ImGui::TextUnformatted("Extent:    ");
                ImGui::SameLine();
                ImGui::PushItemWidth(140.0f * ctx.scale);
                if (ImGui::Combo("##hudGridExtent", &modeIdx, modeItems, IM_ARRAYSIZE(modeItems))) {
                    gs.infiniteGrid = (modeIdx == 1);
                }
                ImGui::PopItemWidth();

                if (!gs.infiniteGrid) {
                    float r = gs.gridRadius;
                    ImGui::TextUnformatted("Radius:    "); ImGui::SameLine();
                    ImGui::PushItemWidth(140.0f * ctx.scale);
                    if (ImGui::SliderFloat("##hudGridRadius", &r, 20.0f, 5000.0f, "%.0f")) gs.gridRadius = r;
                    ImGui::PopItemWidth();

                    float feather = gs.edgeFeather;
                    ImGui::TextUnformatted("Feather:   "); ImGui::SameLine();
                    ImGui::PushItemWidth(140.0f * ctx.scale);
                    if (ImGui::SliderFloat("##hudGridFeather", &feather, 0.0f, 1.0f, "%.2f")) gs.edgeFeather = feather;
                    ImGui::PopItemWidth();

                    bool centerAt0 = gs.centerAtOrigin;
                    if (ImGui::Checkbox("Center at World (0,0)##hud", &centerAt0)) gs.centerAtOrigin = centerAt0;
                } else {
                    float fadeDist = gs.fadeDistance;
                    ImGui::TextUnformatted("Horizon:   "); ImGui::SameLine();
                    ImGui::PushItemWidth(140.0f * ctx.scale);
                    if (ImGui::SliderFloat("##hudGridFade", &fadeDist, 0.2f, 5.0f, "%.2fx")) gs.fadeDistance = fadeDist;
                    ImGui::PopItemWidth();
                }

                float op = gs.opacity;
                ImGui::TextUnformatted("Opacity:   "); ImGui::SameLine();
                ImGui::PushItemWidth(140.0f * ctx.scale);
                if (ImGui::SliderFloat("##hudGridOpacity", &op, 0.05f, 1.0f, "%.2f")) gs.opacity = op;
                ImGui::PopItemWidth();

                float sp = gs.spacing;
                ImGui::TextUnformatted("Spacing:   "); ImGui::SameLine();
                ImGui::PushItemWidth(140.0f * ctx.scale);
                if (ImGui::SliderFloat("##hudGridSpacing", &sp, 5.0f, 200.0f, "%.1f")) gs.spacing = sp;
                ImGui::PopItemWidth();

                ImGui::TextUnformatted("Quick Step:"); ImGui::SameLine();
                float spacingOptions[] = { 10.0f, 25.0f, 50.0f, 100.0f };
                for (int sIdx = 0; sIdx < 4; ++sIdx) {
                    if (sIdx > 0) ImGui::SameLine();
                    char sLabel[16];
                    snprintf(sLabel, sizeof(sLabel), "%d##hudSp", (int)spacingOptions[sIdx]);
                    bool isSelected = (std::abs(gs.spacing - spacingOptions[sIdx]) < 0.1f);
                    if (isSelected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.8f));
                    if (ImGui::Button(sLabel)) gs.spacing = spacingOptions[sIdx];
                    if (isSelected) ImGui::PopStyleColor();
                }

                int subdivs = gs.subdivisions;
                ImGui::TextUnformatted("Subdivs:   "); ImGui::SameLine();
                ImGui::PushItemWidth(140.0f * ctx.scale);
                if (ImGui::SliderInt("##hudGridSubdivs", &subdivs, 2, 10)) gs.subdivisions = subdivs;
                ImGui::PopItemWidth();

                float lw = gs.lineWidth;
                ImGui::TextUnformatted("Line Width:"); ImGui::SameLine();
                ImGui::PushItemWidth(140.0f * ctx.scale);
                if (ImGui::SliderFloat("##hudGridLineWidth", &lw, 0.5f, 3.0f, "%.1f px")) gs.lineWidth = lw;
                ImGui::PopItemWidth();

                bool axes = gs.showAxes;
                if (ImGui::Checkbox("Colored Axes (X/Z)##hud", &axes)) gs.showAxes = axes;
                ImGui::SameLine();
                bool subdivsToggle = gs.showSubdivisions;
                if (ImGui::Checkbox("Subdivisions##hud", &subdivsToggle)) gs.showSubdivisions = subdivsToggle;

                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "Colors");

                glm::vec4 majorCol = glm::vec4(gs.majorColor, gs.majorAlpha);
                if (ImGui::ColorEdit4("##hudMajorCol", &majorCol.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs)) {
                    gs.majorColor = glm::vec3(majorCol);
                    gs.majorAlpha = majorCol.a;
                }
                ImGui::SameLine(); ImGui::TextUnformatted("Major Lines");

                glm::vec4 minorCol = glm::vec4(gs.minorColor, gs.minorAlpha);
                if (ImGui::ColorEdit4("##hudMinorCol", &minorCol.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs)) {
                    gs.minorColor = glm::vec3(minorCol);
                    gs.minorAlpha = minorCol.a;
                }
                ImGui::SameLine(); ImGui::TextUnformatted("Minor Lines");

                glm::vec3 xCol = gs.xAxisColor;
                if (ImGui::ColorEdit3("##hudXCol", &xCol.r, ImGuiColorEditFlags_NoInputs)) gs.xAxisColor = xCol;
                ImGui::SameLine(); ImGui::TextUnformatted("X Axis (Red)");

                glm::vec3 zCol = gs.zAxisColor;
                if (ImGui::ColorEdit3("##hudZCol", &zCol.r, ImGuiColorEditFlags_NoInputs)) gs.zAxisColor = zCol;
                ImGui::SameLine(); ImGui::TextUnformatted("Z Axis (Blue)");

                ImGui::Separator();
                if (ImGui::Button("Reset to Default##hudGrid")) {
                    renderer.resetGridToDefault();
                }
                ImGui::EndPopup();
            }
        }
    });

    m_hudRegistry.registerItem({
        "matcap", "Matcap Shading", "Viewport", "MC", "matcap",
        true, true, false,
        [](GuiManager& gui, SculptManager&, Scene&, AngleRenderer& renderer, const HudRenderCtx& ctx) {
            bool isMatcap = (renderer.getShaderType() == 1);
            GLuint mcTex = gui.getIconTexture("matcap");
            SplitBtnResult res = drawHudSplitButton(
                mcTex,
                "MC##hudMatcap",
                "##hudMatcap",
                ICON_LC_CHEVRON_DOWN "##hudMatcapArrow",
                "Matcap Shading (Click to switch, click again or right-click for presets)",
                "Matcap Presets & Settings", ctx, isMatcap);

            bool openMcPopup = res.arrowClicked || res.mainRightClicked;
            if (res.mainClicked) {
                if (isMatcap) {
                    openMcPopup = true;
                } else {
                    gui.m_previousShaderType = 1;
                    renderer.setShaderType(1);
                }
            }
            if (openMcPopup) ImGui::OpenPopup("##hudMatcapPopup");
            setPopupPosForSplit(res, ctx);
            if (ImGui::BeginPopup("##hudMatcapPopup")) {
                ImGui::SetWindowFontScale(ctx.fontScale);
                ImGui::TextUnformatted("Matcap Shading");
                ImGui::Separator();

                const auto& matcaps = renderer.getMatcaps();
                int curIdx = renderer.getMatcap();
                if (curIdx >= 0 && curIdx < static_cast<int>(matcaps.size())) {
                    ImGui::TextColored(ImVec4(0.01f, 0.75f, 0.65f, 1.0f), "Active: %s", matcaps[curIdx].name.c_str());
                }

                float thumbSize = 44.0f * ctx.scale;
                float pad = 4.0f * ctx.scale;
                int cols = 4;
                int totalMatcaps = static_cast<int>(matcaps.size());
                int numRows = (totalMatcaps + cols - 1) / cols;
                float scrollHeight = std::min(220.0f * ctx.scale, numRows * (thumbSize + pad) + 8.0f * ctx.scale);
                float scrollWidth = (thumbSize + pad) * cols + 20.0f * ctx.scale;

                if (ImGui::BeginChild("##hudMatcapGrid", ImVec2(scrollWidth, scrollHeight), true)) {
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    for (int i = 0; i < totalMatcaps; ++i) {
                        renderer.ensureMatcapLoaded(i);
                        GLuint texId = matcaps[i].textureId;
                        bool isSelected = (curIdx == i && renderer.getShaderType() == 1);

                        ImGui::PushID(i);
                        if (i % cols != 0) ImGui::SameLine(0.0f, pad);

                        ImVec2 p0 = ImGui::GetCursorScreenPos();
                        ImVec2 p1 = ImVec2(p0.x + thumbSize, p0.y + thumbSize);
                        bool clickedThumb = ImGui::InvisibleButton("##mcThumb", ImVec2(thumbSize, thumbSize));
                        bool isHovered = ImGui::IsItemHovered();

                        ImVec2 center = ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                        float radius = thumbSize * 0.5f - 2.0f * ctx.scale;

                        if (texId != 0) {
                            drawList->AddImageRounded((ImTextureID)(uintptr_t)texId,
                                ImVec2(center.x - radius, center.y - radius),
                                ImVec2(center.x + radius, center.y + radius),
                                ImVec2(0, 1), ImVec2(1, 0), IM_COL32(255, 255, 255, 255), radius);
                        } else {
                            drawList->AddCircleFilled(center, radius, IM_COL32(70, 70, 70, 255));
                        }

                        if (isSelected) {
                            drawList->AddCircle(center, radius + 1.5f * ctx.scale, IM_COL32(2, 200, 168, 255), 32, 2.5f * ctx.scale);
                        } else if (isHovered) {
                            drawList->AddCircle(center, radius + 1.0f * ctx.scale, IM_COL32(255, 255, 255, 220), 32, 1.5f * ctx.scale);
                        }

                        if (clickedThumb) {
                            renderer.setMatcap(i);
                            gui.m_previousShaderType = 1;
                            renderer.setShaderType(1);
                        }
                        if (isHovered) ImGui::SetTooltip("%s", matcaps[i].name.c_str());
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();

                ImGui::Separator();
                if (ImGui::Button(ICON_LC_FOLDER_OPEN " Import Custom Matcap...", ImVec2(-1.0f, 24.0f * ctx.scale))) {
                    static const std::vector<FileDialog::FilterSpec> imgFilters = {
                        {"Image Files (*.png, *.jpg, *.jpeg)", "*.png;*.jpg;*.jpeg"},
                        {"All Files (*.*)", "*.*"}
                    };
                    std::string picked = FileDialog::openFile(imgFilters, "Import Matcap Texture");
                    if (!picked.empty()) {
                        size_t lastSlash = picked.find_last_of("/\\");
                        std::string name = (lastSlash != std::string::npos) ? picked.substr(lastSlash + 1) : picked;
                        renderer.importMatcap(name, picked);
                        int newIdx = renderer.findMatcapIndexByName(name);
                        if (newIdx >= 0) {
                            renderer.setMatcap(newIdx);
                            renderer.setShaderType(1);
                            gui.m_previousShaderType = 1;
                        }
                    }
                }
                ImGui::EndPopup();
            }
        }
    });

    m_hudRegistry.registerItem({
        "pbr", "PBR Shader", "Viewport", "PBR", "pbr",
        true, true, false,
        [](GuiManager& gui, SculptManager&, Scene&, AngleRenderer& renderer, const HudRenderCtx& ctx) {
            bool isPbr = (renderer.getShaderType() == 0);
            GLuint pbrTex = gui.getIconTexture("pbr");
            SplitBtnResult res = drawHudSplitButton(
                pbrTex,
                "PBR##hudPbr",
                "##hudPbr",
                ICON_LC_CHEVRON_DOWN "##hudPbrArrow",
                "PBR Shader (Click to switch, click again or right-click for environments)",
                "PBR Environment & Lighting Settings", ctx, isPbr);

            bool openPbrPopup = res.arrowClicked || res.mainRightClicked;
            if (res.mainClicked) {
                if (isPbr) {
                    openPbrPopup = true;
                } else {
                    gui.m_previousShaderType = 0;
                    renderer.setShaderType(0);
                }
            }
            if (openPbrPopup) ImGui::OpenPopup("##hudPbrPopup");
            setPopupPosForSplit(res, ctx);
            if (ImGui::BeginPopup("##hudPbrPopup")) {
                ImGui::SetWindowFontScale(ctx.fontScale);
                ImGui::TextUnformatted("PBR Environment & Lighting");
                ImGui::Separator();

                const auto& envs = renderer.getEnvironments();
                int curEnvIdx = renderer.getCurrentEnvIdx();
                const char* currentEnvName = (curEnvIdx >= 0 && curEnvIdx < static_cast<int>(envs.size())) ? envs[curEnvIdx].name.c_str() : "None";

                ImGui::TextUnformatted("Environment Preset:");
                ImGui::PushItemWidth(180.0f * ctx.scale);
                if (ImGui::BeginCombo("##hudEnvCombo", currentEnvName)) {
                    for (int i = 0; i < static_cast<int>(envs.size()); ++i) {
                        bool selected = (curEnvIdx == i);
                        if (ImGui::Selectable(envs[i].name.c_str(), selected)) {
                            renderer.setEnvironmentPreset(i);
                            gui.m_previousShaderType = 0;
                            renderer.setShaderType(0);
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();

                ImGui::Separator();
                float envRotDeg = glm::degrees(renderer.getEnvRotation());
                while (envRotDeg < 0.0f) envRotDeg += 360.0f;
                while (envRotDeg >= 360.0f) envRotDeg -= 360.0f;
                ImGui::TextUnformatted("Rotation:"); ImGui::SameLine();
                ImGui::PushItemWidth(120.0f * ctx.scale);
                if (ImGui::SliderFloat("##hudEnvRot", &envRotDeg, 0.0f, 360.0f, "%.0f°")) {
                    renderer.setEnvRotation(glm::radians(envRotDeg));
                }
                ImGui::PopItemWidth();

                float curvatureVal = renderer.getCurvature() * 20.0f;
                ImGui::TextUnformatted("Curvature:"); ImGui::SameLine();
                ImGui::PushItemWidth(120.0f * ctx.scale);
                if (ImGui::SliderFloat("##hudPbrCurvature", &curvatureVal, 0.0f, 100.0f, "%.0f")) {
                    renderer.setCurvature(curvatureVal / 20.0f);
                }
                ImGui::PopItemWidth();

                ImGui::Separator();
                ImGui::TextDisabled("Material Properties:");

                float roughness = renderer.getRoughness();
                ImGui::TextUnformatted("Roughness:"); ImGui::SameLine();
                ImGui::PushItemWidth(110.0f * ctx.scale);
                if (ImGui::SliderFloat("##hudPbrRoughness", &roughness, 0.0f, 1.0f, "%.2f")) renderer.setRoughness(roughness);
                ImGui::PopItemWidth();

                float metallic = renderer.getMetallic();
                ImGui::TextUnformatted("Metallic: "); ImGui::SameLine();
                ImGui::PushItemWidth(110.0f * ctx.scale);
                if (ImGui::SliderFloat("##hudPbrMetallic", &metallic, 0.0f, 1.0f, "%.2f")) renderer.setMetallic(metallic);
                ImGui::PopItemWidth();

                ImGui::EndPopup();
            }
        }
    });

    m_hudRegistry.registerItem({
        "flat_shading", "Flat Shading", "Viewport", ICON_LC_CUBOID, "flatshading",
        true, true, false,
        [](GuiManager& gui, SculptManager&, Scene&, AngleRenderer& renderer, const HudRenderCtx& ctx) {
            bool flatShading = renderer.getFlatShading();
            GLuint fsTex = gui.getIconTexture("flatshading");
            if (drawHudIconButton(fsTex, ICON_LC_CUBOID "##hudFlatShading", "##hudFlatShading",
                                  ImVec2(ctx.squareSize, ctx.squareSize), ctx.scale, flatShading)) {
                renderer.setFlatShading(!flatShading);
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Flat Shading");
        }
    });

    m_hudRegistry.registerItem({
        "wireframe", "Wireframe Overlay", "Viewport", ICON_LC_TRIANGLE, "wireframe",
        true, true, false,
        [](GuiManager& gui, SculptManager&, Scene&, AngleRenderer& renderer, const HudRenderCtx& ctx) {
            bool showWireframe = renderer.getShowWireframe();
            GLuint wfTex = gui.getIconTexture("wireframe");
            SplitBtnResult res = drawHudSplitButton(
                wfTex,
                ICON_LC_TRIANGLE "##hudWireframe",
                "##hudWireframe",
                ICON_LC_CHEVRON_DOWN "##hudWireframeArrow",
                "Toggle Wireframe Shading", "Wireframe Settings", ctx, showWireframe);

            if (res.mainClicked) renderer.setShowWireframe(!showWireframe);
            if (res.arrowClicked || res.mainRightClicked) ImGui::OpenPopup("##hudWireframePopup");
            setPopupPosForSplit(res, ctx);
            if (ImGui::BeginPopup("##hudWireframePopup")) {
                ImGui::SetWindowFontScale(ctx.fontScale);
                ImGui::TextUnformatted("Wireframe Settings");
                ImGui::Separator();

                bool enable = renderer.getShowWireframe();
                if (ImGui::Checkbox("Enable Wireframe", &enable)) renderer.setShowWireframe(enable);

                ImGui::Separator();
                float wfOpacity = renderer.getWireframeOpacity();
                ImGui::TextUnformatted("Opacity:"); ImGui::SameLine();
                ImGui::PushItemWidth(120.0f * ctx.scale);
                if (ImGui::SliderFloat("##hudWfOpacity", &wfOpacity, 0.05f, 1.0f, "%.2f")) {
                    renderer.setWireframeOpacity(wfOpacity);
                    glm::vec4 col = renderer.getWireframeColor();
                    col.a = wfOpacity;
                    renderer.setWireframeColor(col);
                }
                ImGui::PopItemWidth();

                ImGui::Separator();
                ImGui::TextUnformatted("Wireframe Color:");
                glm::vec4 wfCol = renderer.getWireframeColor();
                wfCol.a = renderer.getWireframeOpacity();
                if (ImGui::ColorEdit4("##hudWireframeColor", &wfCol.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs)) {
                    renderer.setWireframeColor(wfCol);
                    renderer.setWireframeOpacity(wfCol.a);
                }
                ImGui::SameLine(); ImGui::TextUnformatted("Color & Alpha");

                ImGui::Separator();
                bool showCreasesPopup = renderer.getShowCreaseLines();
                if (ImGui::Checkbox("Show Crease Lines (Pink)", &showCreasesPopup)) {
                    renderer.setShowCreaseLines(showCreasesPopup);
                }
                ImGui::EndPopup();
            }
        }
    });

    m_hudRegistry.registerItem({
        "dyntopo", "Dynamic Topology (Dyn)", "Tools", "Dyn", "",
        true, true, false,
        [](GuiManager&, SculptManager& sculpt, Scene&, AngleRenderer&, const HudRenderCtx& ctx) {
            bool dyntopoEnabled = sculpt.isDyntopoEnabled();
            SplitBtnResult res = drawHudSplitButton(
                0,
                "Dyn##hudDyntopo",
                "",
                ICON_LC_CHEVRON_DOWN "##hudDyntopoArrow",
                "Toggle Dynamic Topology (Ctrl+D)", "Dynamic Topology Settings", ctx, dyntopoEnabled, HudAccent::Purple);

            if (res.mainClicked) sculpt.toggleDyntopo();
            if (res.arrowClicked || res.mainRightClicked) ImGui::OpenPopup("##hudDyntopoPopup");
            setPopupPosForSplit(res, ctx);
            if (ImGui::BeginPopup("##hudDyntopoPopup")) {
                ImGui::SetWindowFontScale(ctx.fontScale);
                ImGui::TextUnformatted("Dynamic Topology Settings");
                ImGui::Separator();

                auto& dyntopo = sculpt.getDyntopoSettings();
                bool dynEn = dyntopo.enabled;
                if (ImGui::Checkbox("Enable Dynamic Topology", &dynEn)) sculpt.setDyntopoEnabled(dynEn);

                ImGui::Separator();
                ImGui::TextUnformatted("Detail Mode:");
                int modeIdx = dyntopo.constantDetail ? 1 : 0;
                if (ImGui::RadioButton("Relative (Radius & Zoom)", &modeIdx, 0)) dyntopo.constantDetail = false;
                if (ImGui::RadioButton("Constant (World Space)", &modeIdx, 1)) dyntopo.constantDetail = true;

                ImGui::Separator();
                float detailPct = dyntopo.detail * 100.0f;
                if (ImGui::SliderFloat("Detail", &detailPct, 0.0f, 100.0f, "%.0f%%")) {
                    dyntopo.detail = std::clamp(detailPct / 100.0f, 0.0f, 1.0f);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Higher detail produces smaller triangles");

                bool picking = sculpt.isPickMeshSizeActive();
                if (picking) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.66f, 0.33f, 0.97f, 1.0f));
                if (ImGui::Button(ICON_LC_PIPETTE " Pick Detail from Surface")) {
                    sculpt.togglePickMeshSize();
                }
                if (picking) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click on a mesh polygon to match its edge resolution");

                ImGui::Separator();
                ImGui::SliderFloat("Smooth Remesh", &dyntopo.smoothRemesh, 0.0f, 1.0f, "%.2f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = no remesh on Shift (smooth only)\n1 = simplifies triangles 2x during Shift");

                ImGui::Checkbox("Auto Wireframe", &dyntopo.autoWireframe);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Temporarily shows wireframe while sculpting");

                BrushType curType = sculpt.getBrush();
                if (curType != BRUSH_DYNAMIC_TOPOLOGY && curType != BRUSH_QUAD_DRAW &&
                    curType != BRUSH_MEASURE && curType != BRUSH_DIVIDER &&
                    curType != BRUSH_TRANSFORM && curType != BRUSH_ARMATURE_SPHERES &&
                    curType != BRUSH_COMPONENT_SELECT && curType != BRUSH_EDGE_CREASE) {
                    ImGui::Separator();
                    BrushSettings& curBrush = sculpt.getCurrentSettings();
                    ImGui::Checkbox("Enable for Active Brush", &curBrush.useDynamicTopology);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Toggle Dynamic Topology for current brush (%s)", getBrushName(curType));
                    }
                }
                ImGui::EndPopup();
            }
        }
    });

    m_hudRegistry.registerItem({
        "polygroups", "PolyGroups Display", "Viewport", ICON_LC_LAYERS, "polygroup",
        true, true, false,
        [](GuiManager& gui, SculptManager& sculpt, Scene&, AngleRenderer& renderer, const HudRenderCtx& ctx) {
            bool showPolyGroups = renderer.getShowPolyGroups() || (sculpt.getBrush() == BRUSH_POLYGROUP);
            GLuint pgTex = gui.getIconTexture("polygroup");
            if (drawHudIconButton(pgTex, ICON_LC_LAYERS "##hudPolyGroups", "##hudPolyGroups",
                                  ImVec2(ctx.squareSize, ctx.squareSize), ctx.scale, showPolyGroups)) {
                renderer.setShowPolyGroups(!renderer.getShowPolyGroups());
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle PolyGroups Display");
        }
    });

    m_hudRegistry.registerItem({
        "creases", "Crease Lines Display", "Viewport", ICON_LC_SQUARE_SPLIT_VERTICAL, "",
        true, true, false,
        [](GuiManager&, SculptManager&, Scene&, AngleRenderer& renderer, const HudRenderCtx& ctx) {
            bool showCreases = renderer.getShowCreaseLines();
            if (drawHudIconButton(0, ICON_LC_SQUARE_SPLIT_VERTICAL "##hudCreases", "",
                                  ImVec2(ctx.squareSize, ctx.squareSize), ctx.scale, showCreases, HudAccent::Pink)) {
                renderer.setShowCreaseLines(!showCreases);
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) {
                ImGui::SetTooltip(showCreases ? "Hide Crease Lines (Pink Dashed Lines)" : "Show Crease Lines (Pink Dashed Lines)");
            }
        }
    });

    m_hudRegistry.registerItem({
        "silhouette", "Silhouette Mode", "Viewport", ICON_LC_CONTRAST, "silhouette",
        true, true, false,
        [](GuiManager& gui, SculptManager&, Scene&, AngleRenderer& renderer, const HudRenderCtx& ctx) {
            bool isSilhouette = (renderer.getShaderType() == 6);
            GLuint silTex = gui.getIconTexture("silhouette");
            if (drawHudIconButton(silTex, ICON_LC_CONTRAST "##hudSilhouette", "##hudSilhouette",
                                  ImVec2(ctx.squareSize, ctx.squareSize), ctx.scale, isSilhouette)) {
                if (isSilhouette) {
                    renderer.setShaderType(gui.m_previousShaderType);
                } else {
                    gui.m_previousShaderType = renderer.getShaderType();
                    renderer.setShaderType(6);
                }
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Silhouette Mode");
        }
    });

    m_hudRegistry.registerItem({
        "safe_frames", "Safe Frames Overlay", "Viewport", ICON_LC_FRAME, "frame",
        true, true, false,
        [](GuiManager& gui, SculptManager&, Scene&, AngleRenderer& renderer, const HudRenderCtx& ctx) {
            bool showSafeFrames = renderer.getShowSafeFrames();
            GLuint frameTex = gui.getIconTexture("frame");
            if (drawHudIconButton(frameTex, ICON_LC_FRAME "##hudSafeFrames", "##hudSafeFrames",
                                  ImVec2(ctx.squareSize, ctx.squareSize), ctx.scale, showSafeFrames)) {
                renderer.setShowSafeFrames(!showSafeFrames);
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Safe Frames Overlay");
        }
    });

    m_hudRegistry.registerItem({
        "xray", "X-Ray Mode", "Viewport", ICON_LC_SCAN_FACE, "",
        true, true, false,
        [](GuiManager& gui, SculptManager&, Scene&, AngleRenderer& renderer, const HudRenderCtx& ctx) {
            bool xrayEnabled = renderer.getXrayEnabled();
            GLuint xrayTex = gui.getXrayIconTexture();
            SplitBtnResult res = drawHudSplitButton(
                xrayTex,
                ICON_LC_SCAN_FACE "##hudXray",
                "##hudXray",
                ICON_LC_CHEVRON_DOWN "##hudXrayArrow",
                "Toggle X-Ray Mode", "X-Ray Settings", ctx, xrayEnabled);

            if (res.mainClicked) renderer.setXrayEnabled(!xrayEnabled);
            if (res.arrowClicked || res.mainRightClicked) ImGui::OpenPopup("##hudXrayPopup");
            setPopupPosForSplit(res, ctx);
            if (ImGui::BeginPopup("##hudXrayPopup")) {
                ImGui::SetWindowFontScale(ctx.fontScale);
                ImGui::TextUnformatted("X-Ray Settings");
                ImGui::Separator();

                bool enable = renderer.getXrayEnabled();
                if (ImGui::Checkbox("Enable X-Ray Mode", &enable)) renderer.setXrayEnabled(enable);

                ImGui::Separator();
                ImGui::TextUnformatted("Target:");
                int target = renderer.getXrayTarget();
                bool changedTarget = false;
                if (ImGui::RadioButton("Unselected (default)", &target, 0)) changedTarget = true;
                if (ImGui::RadioButton("Selected", &target, 1)) changedTarget = true;
                if (ImGui::RadioButton("All", &target, 2)) changedTarget = true;
                if (changedTarget) renderer.setXrayTarget(target);

                ImGui::Separator();
                ImGui::TextUnformatted("X-Ray Color:");
                glm::vec4 xrayCol = renderer.getXrayColor();
                if (ImGui::ColorEdit4("##hudXrayColor", &xrayCol.r, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs)) {
                    renderer.setXrayColor(xrayCol);
                }
                ImGui::SameLine(); ImGui::TextUnformatted("Color & Alpha");
                ImGui::EndPopup();
            }
        }
    });

    m_hudRegistry.registerItem({
        "perspective", "Perspective / FOV", "Viewport", ICON_LC_CAMERA, "persp",
        true, true, false,
        [](GuiManager& gui, SculptManager&, Scene& scene, AngleRenderer&, const HudRenderCtx& ctx) {
            bool isPerspective = (scene.getCamera().getProjectionType() == CameraEnums::Projection::PERSPECTIVE);
            GLuint fovTex = gui.getIconTexture("persp");
            SplitBtnResult res = drawHudSplitButton(
                fovTex,
                ICON_LC_CAMERA "##hudPerspective",
                "##hudPerspective",
                ICON_LC_CHEVRON_DOWN "##hudFovArrow",
                "Toggle Perspective Projection (P)", "FOV Settings", ctx, isPerspective);

            if (res.mainClicked) {
                scene.getCamera().setProjectionType(isPerspective ? CameraEnums::Projection::ORTHOGRAPHIC
                                                                  : CameraEnums::Projection::PERSPECTIVE);
            }
            if (res.arrowClicked || res.mainRightClicked) ImGui::OpenPopup("##hudFovPopup");
            setPopupPosForSplit(res, ctx);
            if (ImGui::BeginPopup("##hudFovPopup")) {
                ImGui::SetWindowFontScale(ctx.fontScale);
                float fov = scene.getCamera().getFov();
                ImGui::TextUnformatted("FOV:"); ImGui::SameLine();
                ImGui::PushItemWidth(120.0f * ctx.scale);
                if (ImGui::SliderFloat("##hudFovSlider", &fov, 10.0f, 120.0f, "%.0f mm")) {
                    scene.getCamera().setFov(fov);
                }
                ImGui::PopItemWidth();
                ImGui::EndPopup();
            }
        }
    });

    m_hudRegistry.registerItem({
        "pan_zoom_2d", "2D Pan & Zoom", "Viewport", "2D", "2dpanzoom",
        true, true, false,
        [](GuiManager& gui, SculptManager&, Scene& scene, AngleRenderer&, const HudRenderCtx& ctx) {
            Camera& camera = scene.getCamera();
            bool ref2D = camera.getRef2DMode();
            GLuint panZoomTex = gui.getIconTexture("2dpanzoom");
            SplitBtnResult res = drawHudSplitButton(
                panZoomTex,
                "2D##hud2DPanZoomBtn",
                "##hud2DPanZoomBtn",
                ICON_LC_CHEVRON_DOWN "##hud2DPanZoomArrow",
                "2D Pan & Zoom Mode (Toggle)", "2D View Options & Reset", ctx, ref2D);

            if (res.mainClicked) {
                camera.setRef2DMode(!ref2D);
                if (scene.getSplitMode() != Scene::SplitMode::OFF && scene.getCameraRight()) {
                    scene.getCameraRight()->setRef2DMode(!ref2D);
                }
            }
            if (res.arrowClicked || res.mainRightClicked) ImGui::OpenPopup("##hud2DPanZoomPopup");
            setPopupPosForSplit(res, ctx);
            if (ImGui::BeginPopup("##hud2DPanZoomPopup")) {
                ImGui::SetWindowFontScale(ctx.fontScale);
                ImGui::TextUnformatted("2D Pan / Zoom View");
                ImGui::Separator();

                bool active2D = camera.getRef2DMode();
                if (ImGui::Checkbox("2D Mode Enabled", &active2D)) {
                    camera.setRef2DMode(active2D);
                    if (scene.getSplitMode() != Scene::SplitMode::OFF && scene.getCameraRight()) {
                        scene.getCameraRight()->setRef2DMode(active2D);
                    }
                }
                ImGui::Separator();
                if (ImGui::Button("Reset 2D View", ImVec2(140.0f * ctx.scale, 24.0f * ctx.scale))) {
                    camera.resetView2D();
                    if (scene.getSplitMode() != Scene::SplitMode::OFF && scene.getCameraRight()) {
                        scene.getCameraRight()->resetView2D();
                    }
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", "Reset 2D pan offset to center and zoom scale to 100%");
                }
                ImGui::EndPopup();
            }
        }
    });

    m_hudRegistry.registerItem({
        "split_view", "Split Viewport", "Viewport", ICON_LC_COLUMNS, "splitviewport",
        true, true, false,
        [](GuiManager& gui, SculptManager&, Scene& scene, AngleRenderer& renderer, const HudRenderCtx& ctx) {
            bool isSplit = (scene.getSplitMode() != Scene::SplitMode::OFF);
            auto applySplitMode = [&](Scene::SplitMode newMode) {
                scene.setSplitMode(newMode);
                if (gui.m_window) {
                    int w, h;
                    SDL_GetWindowSize(gui.m_window, &w, &h);
                    renderer.resize(w, h);
                    if (scene.getSplitMode() != Scene::SplitMode::OFF) {
                        int halfW = w / 2;
                        scene.getCamera().onResize(halfW, h);
                        if (scene.getCameraRight()) scene.getCameraRight()->onResize(w - halfW, h);
                    } else {
                        scene.getCamera().onResize(w, h);
                    }
                }
            };

            GLuint splitTex = gui.getIconTexture("splitviewport");
            SplitBtnResult res = drawHudSplitButton(
                splitTex,
                ICON_LC_COLUMNS "##hudSplitView",
                "##hudSplitView",
                ICON_LC_CHEVRON_DOWN "##hudSplitViewArrow",
                "Toggle Split Viewport", "Split View Options", ctx, isSplit);

            if (res.mainClicked) {
                applySplitMode(isSplit ? Scene::SplitMode::OFF : Scene::SplitMode::MIRROR);
            }
            if (res.arrowClicked || res.mainRightClicked) ImGui::OpenPopup("##hudSplitViewPopup");
            setPopupPosForSplit(res, ctx);
            if (ImGui::BeginPopup("##hudSplitViewPopup")) {
                ImGui::SetWindowFontScale(ctx.fontScale);
                ImGui::TextUnformatted("Split View:");
                int curSplitMode = static_cast<int>(scene.getSplitMode());
                bool changed = false;
                if (ImGui::RadioButton("Off", &curSplitMode, 0)) changed = true;
                if (ImGui::RadioButton("Mirror", &curSplitMode, 1)) changed = true;
                if (ImGui::RadioButton("Independent", &curSplitMode, 2)) changed = true;
                if (changed) applySplitMode(static_cast<Scene::SplitMode>(curSplitMode));
                if (scene.getSplitMode() != Scene::SplitMode::OFF) {
                    ImGui::Separator();
                    bool showInactive = scene.getSplitShowInactiveCursor();
                    if (ImGui::Checkbox("Show Inactive Cursor", &showInactive)) {
                        scene.setSplitShowInactiveCursor(showInactive);
                    }
                }
                ImGui::EndPopup();
            }
        }
    });

    m_hudRegistry.registerItem({
        "solo", "Solo Mode", "Viewport", ICON_LC_EYE, "",
        true, true, false,
        [](GuiManager&, SculptManager&, Scene& scene, AngleRenderer&, const HudRenderCtx& ctx) {
            bool isSolo = scene.isSoloActive();
            if (drawHudIconButton(0, ICON_LC_EYE "##hudSolo", "", ImVec2(ctx.squareSize, ctx.squareSize), ctx.scale, isSolo, HudAccent::Orange)) {
                scene.toggleSolo(scene.getSelected());
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Solo Mode (C)");
        }
    });

    m_hudRegistry.registerItem({
        "snapshot", "Model Snapshot", "Viewport", ICON_LC_IMAGE, "",
        true, true, false,
        [](GuiManager&, SculptManager&, Scene& scene, AngleRenderer& renderer, const HudRenderCtx& ctx) {
            bool hasSnapshot = renderer.hasActiveSnapshot();
            if (drawHudIconButton(0, ICON_LC_IMAGE "##hudSnapshot", "", ImVec2(ctx.squareSize, ctx.squareSize), ctx.scale, hasSnapshot)) {
                renderer.toggleSnapshot(scene);
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Model Snapshot (Screen Reference)");
        }
    });

    // 5. Additional Panels & Quick Actions (available to add in Catalog!)
    m_hudRegistry.registerItem({
        "action_undo", "Undo", "Actions", ICON_LC_UNDO_2, "",
        true, true, false,
        [](GuiManager&, SculptManager&, Scene& scene, AngleRenderer&, const HudRenderCtx& ctx) {
            bool canUndo = g_undoManager.canUndo();
            if (!canUndo && !ctx.customizeMode) ImGui::BeginDisabled();
            if (ImGui::Button(ICON_LC_UNDO_2 "##hudActionUndo", ImVec2(ctx.squareSize, ctx.squareSize))) {
                g_undoManager.undo(scene);
            }
            if (!canUndo && !ctx.customizeMode) ImGui::EndDisabled();
            if (!ctx.customizeMode && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Undo (Ctrl+Z)");
            }
        }
    });

    m_hudRegistry.registerItem({
        "action_redo", "Redo", "Actions", ICON_LC_REDO_2, "",
        true, true, false,
        [](GuiManager&, SculptManager&, Scene& scene, AngleRenderer&, const HudRenderCtx& ctx) {
            bool canRedo = g_undoManager.canRedo();
            if (!canRedo && !ctx.customizeMode) ImGui::BeginDisabled();
            if (ImGui::Button(ICON_LC_REDO_2 "##hudActionRedo", ImVec2(ctx.squareSize, ctx.squareSize))) {
                g_undoManager.redo(scene);
            }
            if (!canRedo && !ctx.customizeMode) ImGui::EndDisabled();
            if (!ctx.customizeMode && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Redo (Ctrl+Shift+Z)");
            }
        }
    });

    m_hudRegistry.registerItem({
        "action_save", "Save Project", "Actions", ICON_LC_SAVE, "",
        true, true, false,
        [](GuiManager& gui, SculptManager& sculpt, Scene& scene, AngleRenderer&, const HudRenderCtx& ctx) {
            if (ImGui::Button(ICON_LC_SAVE "##hudActionSave", ImVec2(ctx.squareSize, ctx.squareSize))) {
                gui.saveScene(scene, &sculpt);
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) ImGui::SetTooltip("Save Project (Ctrl+S)");
        }
    });

    m_hudRegistry.registerItem({
        "action_clear_mask", "Clear Mask", "Actions", ICON_LC_ERASER, "",
        true, true, false,
        [](GuiManager&, SculptManager& sculpt, Scene& scene, AngleRenderer&, const HudRenderCtx& ctx) {
            if (ImGui::Button(ICON_LC_ERASER "##hudActionClearMask", ImVec2(ctx.squareSize, ctx.squareSize))) {
                if (Mesh* m = scene.getSelected()) sculpt.clearMask(m, &scene);
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) ImGui::SetTooltip("Clear Mask");
        }
    });

    m_hudRegistry.registerItem({
        "action_invert_mask", "Invert Mask", "Actions", ICON_LC_CONTRAST, "",
        true, true, false,
        [](GuiManager&, SculptManager& sculpt, Scene& scene, AngleRenderer&, const HudRenderCtx& ctx) {
            if (ImGui::Button(ICON_LC_CONTRAST "##hudActionInvertMask", ImVec2(ctx.squareSize, ctx.squareSize))) {
                if (Mesh* m = scene.getSelected()) sculpt.invertMask(m);
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) ImGui::SetTooltip("Invert Mask");
        }
    });

    m_hudRegistry.registerItem({
        "action_focus_camera", "Frame Selected Mesh", "Actions", ICON_LC_FOCUS, "",
        true, true, false,
        [](GuiManager& gui, SculptManager&, Scene& scene, AngleRenderer&, const HudRenderCtx& ctx) {
            if (ImGui::Button(ICON_LC_FOCUS "##hudActionFocus", ImVec2(ctx.squareSize, ctx.squareSize))) {
                std::vector<Mesh*> selectedMeshes = scene.getSelectedMeshes();
                if (selectedMeshes.empty() && scene.getSelected()) {
                    selectedMeshes.push_back(scene.getSelected());
                }
                if (selectedMeshes.empty()) {
                    for (Mesh* m : scene.getMeshes()) {
                        if (m && m->isVisible(scene.getActiveViewport())) {
                            selectedMeshes.push_back(m);
                        }
                    }
                }
                if (!selectedMeshes.empty()) {
                    scene.getCamera().resetViewToMeshes(selectedMeshes, gui.getAnimateBookmarks());
                } else {
                    scene.getCamera().resetView();
                }
            }
            if (!ctx.customizeMode && ImGui::IsItemHovered()) ImGui::SetTooltip("Frame Selected Mesh (F)");
        }
    });

    auto registerPanelToggle = [this](const std::string& id, const std::string& label, const char* icon,
                                      bool GuiManager::*memberPtr, const char* btnId, const char* tooltip) {
        std::string fullBtn = std::string(icon) + "##" + btnId;
        m_hudRegistry.registerItem({
            id, label, "Panels", icon, "",
            true, true, false,
            [memberPtr, fullBtn, tooltip](GuiManager& gui, SculptManager&, Scene&, AngleRenderer&, const HudRenderCtx& ctx) {
                bool active = gui.*memberPtr;
                if (drawHudIconButton(0, fullBtn.c_str(), "", ImVec2(ctx.squareSize, ctx.squareSize), ctx.scale, active)) {
                    gui.*memberPtr = !active;
                }
                if (!ctx.customizeMode && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
            }
        });
    };

    registerPanelToggle("panel_topology", "Topology & Remesh Panel", ICON_LC_CPU, &GuiManager::m_showTopologyPanel, "hudPanelTopo", "Toggle Topology & Remesh Panel");
    registerPanelToggle("panel_masking", "Masking Panel", ICON_LC_PAINTBRUSH, &GuiManager::m_showMaskingPanel, "hudPanelMask", "Toggle Masking Panel");
    registerPanelToggle("panel_multires", "Multiresolution Panel", ICON_LC_LAYERS_2, &GuiManager::m_showMultiresPanel, "hudPanelMultires", "Toggle Multiresolution & Subdiv Panel");
    registerPanelToggle("panel_layers", "Sculpt Layers Panel", ICON_LC_LAYERS, &GuiManager::m_showLayersPanel, "hudPanelLayers", "Toggle Sculpt Layers Panel");
    registerPanelToggle("panel_bookmarks", "Camera Bookmarks Panel", ICON_LC_BOOKMARK, &GuiManager::m_showCameraBookmarksPanel, "hudPanelBookmarks", "Toggle Camera Bookmarks Panel (F2)");
    registerPanelToggle("panel_timelapse", "Timelapse Panel", ICON_LC_FILM, &GuiManager::m_showTimelapsePanel, "hudPanelTimelapse", "Toggle Timelapse Panel");
    registerPanelToggle("panel_component", "Component Tools Panel", ICON_LC_SQUARE_SPLIT_VERTICAL, &GuiManager::m_showComponentPanel, "hudPanelComp", "Toggle Component Tools Panel");
    registerPanelToggle("panel_preferences", "Preferences Panel", ICON_LC_SETTINGS, &GuiManager::m_showPreferencesPanel, "hudPanelPrefs", "Toggle Preferences Panel (Ctrl+,)");

    // 6. Auto-register all Brushes from BRUSH_COUNT not already covered by dedicated items
    for (int i = 0; i < BRUSH_COUNT; ++i) {
        BrushType bType = static_cast<BrushType>(i);
        std::string stableId = makeBrushStableId(bType);
        if (m_hudRegistry.find(stableId) != nullptr) {
            continue;
        }

        const char* bName = getBrushName(bType);
        const char* iconKey = getBrushIconKeyStr(bType);
        const char* hk = getBrushHotkeyStr(bType);

        std::string label = std::string(bName) + " Brush";
        std::string shortLbl;
        if (bName && bName[0] != '\0') {
            shortLbl += bName[0];
            if (bName[1] != '\0') shortLbl += bName[1];
        } else {
            shortLbl = "Br";
        }
        std::string texBtn = "##hudBrush_" + stableId;
        std::string fallbackBtn = shortLbl + "##hudBrush_" + stableId;
        std::string tip = hk ? (std::string(bName) + " [" + hk + "]") : std::string(bName);

        m_hudRegistry.registerItem({
            stableId, label, "Brushes", ICON_LC_BRUSH, iconKey ? iconKey : "",
            true, true, false,
            [bType, iconKey, texBtn, fallbackBtn, tip](GuiManager& gui, SculptManager& sculpt, Scene& scene, AngleRenderer&, const HudRenderCtx& ctx) {
                bool active = (sculpt.getBrush() == bType);
                GLuint tex = iconKey ? gui.getIconTexture(iconKey) : 0;
                if (drawHudIconButton(tex, fallbackBtn.c_str(), texBtn.c_str(),
                                      ImVec2(ctx.squareSize, ctx.squareSize), ctx.scale, active)) {
                    sculpt.setBrush(bType);
                    activateBrushSideEffects(gui, sculpt, scene, bType);
                }
                if (!ctx.customizeMode && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", tip.c_str());
                }
            }
        });
    }
}

void GuiManager::applyPendingHudOps() {
    if (m_pendingHudOps.empty()) return;
    for (const auto& op : m_pendingHudOps) {
        m_hudLayout.applyOp(op, m_hudRegistry);
    }
    m_pendingHudOps.clear();
}

void GuiManager::drawHudQuickAddPopup(HudIslandId islandId, float fontScale) {
    char popupId[48];
    snprintf(popupId, sizeof(popupId), "##hudQuickAddPopup_%d", static_cast<int>(islandId));
    if (!ImGui::BeginPopup(popupId)) return;

    ImGui::SetWindowFontScale(fontScale);
    ImGui::Text("Add to %s", HudLayoutConfig::islandDisplayName(islandId));
    ImGui::Separator();

    static char quickFilter[64] = "";
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##hudQuickFilter", "Filter...", quickFilter, sizeof(quickFilter));
    std::string filterLower = toLowerAscii(quickFilter);

    bool isVert = HudLayoutConfig::isIslandVertical(islandId);

    if (ImGui::BeginChild("##hudQuickAddList", ImVec2(260.0f, 300.0f), true)) {
        for (const auto& item : m_hudRegistry.getAll()) {
            if (isVert && !item.allowVertical) continue;
            if (!isVert && !item.allowHorizontal) continue;
            if (!item.allowMultiple && m_hudLayout.islandContains(islandId, item.id)) continue;

            if (!filterLower.empty()) {
                std::string haystack = toLowerAscii(item.label + " " + item.category + " " + item.id);
                if (haystack.find(filterLower) == std::string::npos) continue;
            }

            std::string rowLbl = "[" + item.category + "] " + item.label + "##qa_" + item.id;
            if (ImGui::Selectable(rowLbl.c_str())) {
                PendingHudOp op;
                op.type = PendingHudOp::Type::Insert;
                op.dstIsland = islandId;
                op.dstIndex = static_cast<int>(m_hudLayout.items(islandId).size());
                op.itemId = item.id;
                m_pendingHudOps.push_back(std::move(op));
                ImGui::CloseCurrentPopup();
            }
        }
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

void GuiManager::drawHudIslandSlots(
    HudIslandId islandId,
    SculptManager& sculpt,
    Scene& scene,
    AngleRenderer& renderer,
    const HudRenderCtx& baseCtx,
    float maxRowWidth)
{
    HudRenderCtx ctx = baseCtx;
    ctx.islandId = islandId;
    ctx.isVertical = HudLayoutConfig::isIslandVertical(islandId);
    ctx.customizeMode = m_customizeHudMode;

    auto& itemIds = m_hudLayout.items(islandId);
    const int count = static_cast<int>(itemIds.size());
    float currentRowW = 0.0f;
    ImVec2 winStartPos = ImGui::GetCursorPos();

    struct SlotBox {
        ImVec2 min;
        ImVec2 max;
    };
    std::vector<SlotBox> slotBoxes(static_cast<size_t>(std::max(0, count)));

    auto handleDropOnSlot = [&](int targetSlotIdx, const ImVec2& pMin, const ImVec2& pMax, bool isEndButton) {
        if (!ImGui::BeginDragDropTarget()) return;

        if (const ImGuiPayload* peek = ImGui::AcceptDragDropPayload("HUD_ITEM",
                ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            if (peek->DataSize == sizeof(HudDragPayload)) {
                const auto* payload = static_cast<const HudDragPayload*>(peek->Data);
                const HudItemDef* def = m_hudRegistry.find(payload->itemId);
                bool compatible = def && (ctx.isVertical ? def->allowVertical : def->allowHorizontal);

                if (compatible) {
                    int insertIdx = targetSlotIdx;
                    if (!isEndButton) {
                        ImVec2 mp = ImGui::GetMousePos();
                        bool insertAfter = ctx.isVertical
                            ? (mp.y >= (pMin.y + pMax.y) * 0.5f)
                            : (mp.x >= (pMin.x + pMax.x) * 0.5f);
                        if (insertAfter) insertIdx++;

                        ImDrawList* fdl = ImGui::GetForegroundDrawList();
                        ImU32 lineCol = IM_COL32(20, 235, 195, 255);
                        float thick = std::max(2.0f, 2.5f * ctx.scale);
                        if (ctx.isVertical) {
                            float y = insertAfter ? pMax.y : pMin.y;
                            fdl->AddLine(ImVec2(pMin.x - 2.0f, y), ImVec2(pMax.x + 2.0f, y), lineCol, thick);
                        } else {
                            float x = insertAfter ? pMax.x : pMin.x;
                            fdl->AddLine(ImVec2(x, pMin.y - 2.0f), ImVec2(x, pMax.y + 2.0f), lineCol, thick);
                        }
                    } else {
                        ImDrawList* fdl = ImGui::GetForegroundDrawList();
                        fdl->AddRect(pMin, pMax, IM_COL32(20, 235, 195, 255), 3.0f * ctx.scale, 0, 2.0f * ctx.scale);
                    }

                    if (peek->IsDelivery()) {
                        PendingHudOp op;
                        op.type = (payload->srcIsland >= 0) ? PendingHudOp::Type::Move : PendingHudOp::Type::Insert;
                        op.srcIsland = (payload->srcIsland >= 0) ? static_cast<HudIslandId>(payload->srcIsland) : islandId;
                        op.srcIndex = payload->srcIndex;
                        op.dstIsland = islandId;
                        op.dstIndex = insertIdx;
                        op.itemId = payload->itemId;
                        m_pendingHudOps.push_back(std::move(op));
                        m_hudDropHandledThisFrame = true;
                    }
                } else {
                    ImGui::GetForegroundDrawList()->AddRect(
                        pMin, pMax, IM_COL32(235, 70, 70, 220), 3.0f * ctx.scale, 0, 2.0f * ctx.scale);
                }
            }
        }
        ImGui::EndDragDropTarget();
    };

    for (int idx = 0; idx < count; ++idx) {
        const std::string& itemId = itemIds[idx];
        const HudItemDef* def = m_hudRegistry.find(itemId);
        if (!def) continue;

        if (!ctx.isVertical && idx > 0) {
            if (maxRowWidth > 0.0f) {
                float approxW = (itemId == "active_tool") ? (ctx.squareSize * 2.0f) : ctx.squareSize;
                if (currentRowW + approxW <= maxRowWidth) {
                    ImGui::SameLine(0.0f, 0.0f);
                } else {
                    currentRowW = 0.0f;
                    ImGui::SetCursorPosX(winStartPos.x);
                }
            } else {
                ImGui::SameLine();
            }
        }

        ImGui::PushID(idx);

        if (!ctx.customizeMode) {
            def->render(*this, sculpt, scene, renderer, ctx);
            if (!ctx.isVertical && maxRowWidth > 0.0f) {
                float itemW = ImGui::GetItemRectSize().x;
                currentRowW += std::max(itemW, (itemId == "active_tool") ? ctx.squareSize * 2.0f : ctx.squareSize);
            }
        } else {
            ImGui::BeginGroup();
            // AllowOverlap ensures inner disabled buttons do not steal HoveredId from the overlay drag handle
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            ImGui::PushItemFlag(ImGuiItemFlags_AllowOverlap, true);
            def->render(*this, sculpt, scene, renderer, ctx);
            ImGui::PopItemFlag();
            ImGui::PopItemFlag();
            ImGui::EndGroup();

            ImVec2 pMin = ImGui::GetItemRectMin();
            ImVec2 pMax = ImGui::GetItemRectMax();
            float slotW = std::max(10.0f * ctx.scale, pMax.x - pMin.x);
            float slotH = std::max(10.0f * ctx.scale, pMax.y - pMin.y);
            ImVec2 slotMax(pMin.x + slotW, pMin.y + slotH);
            slotBoxes[static_cast<size_t>(idx)] = { pMin, slotMax };

            if (!ctx.isVertical && maxRowWidth > 0.0f) {
                currentRowW += slotW;
            }

            ImGuiWindow* curWin = ImGui::GetCurrentWindow();
            float savedPrevBaseOffset = curWin ? curWin->DC.PrevLineTextBaseOffset : 0.0f;

            // Overlay hit target for direct drag-and-drop, 'x' removal, and context menu
            ImGui::SetCursorScreenPos(pMin);
            bool slotClicked = ImGui::InvisibleButton("##hudSlotDrag", ImVec2(slotW, slotH));
            if (curWin) {
                curWin->DC.PrevLineTextBaseOffset = savedPrevBaseOffset;
            }
            bool slotHovered = ImGui::IsItemHovered();

            float badgeSize = std::clamp(10.0f * ctx.scale, 8.0f, std::min(slotW, slotH) * 0.42f);
            ImVec2 delMin(slotMax.x - badgeSize, pMin.y);
            ImVec2 delMax(slotMax.x, pMin.y + badgeSize);

            const ImGuiPayload* curPayload = ImGui::GetDragDropPayload();
            bool anyHudDragActive = (curPayload != nullptr && curPayload->IsDataType("HUD_ITEM"));
            bool isSelfBeingDragged = false;
            if (anyHudDragActive && curPayload->DataSize == sizeof(HudDragPayload)) {
                const auto* ap = static_cast<const HudDragPayload*>(curPayload->Data);
                isSelfBeingDragged = (ap->srcIsland == static_cast<int>(islandId) && ap->srcIndex == idx);
            }

            bool hoveringSlot = ImGui::IsMouseHoveringRect(pMin, slotMax);
            bool hoveringDel = !anyHudDragActive && hoveringSlot && ImGui::IsMouseHoveringRect(delMin, delMax);

            // Click 'x' badge (released without dragging) removes item directly
            if (slotClicked && hoveringDel && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left, 3.0f)) {
                PendingHudOp op;
                op.type = PendingHudOp::Type::Remove;
                op.srcIsland = islandId;
                op.srcIndex = idx;
                op.itemId = itemId;
                m_pendingHudOps.push_back(std::move(op));
            }

            // Always allow initiating direct drag from anywhere on the button
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                HudDragPayload payload{};
                payload.srcIsland = static_cast<int>(islandId);
                payload.srcIndex = idx;
                std::strncpy(payload.itemId, itemId.c_str(), sizeof(payload.itemId) - 1);
                ImGui::SetDragDropPayload("HUD_ITEM", &payload, sizeof(payload));
                ImGui::Text("%s %s", def->iconText.c_str(), def->label.c_str());
                ImGui::EndDragDropSource();
            }

            handleDropOnSlot(idx, pMin, slotMax, false);

            if (ImGui::BeginPopupContextItem("##hudSlotCtx")) {
                ImGui::SetWindowFontScale(ctx.fontScale);
                ImGui::TextDisabled("%s", def->label.c_str());
                ImGui::Separator();
                if (idx > 0 && ImGui::MenuItem(ctx.isVertical ? "Move Up" : "Move Left")) {
                    PendingHudOp op;
                    op.type = PendingHudOp::Type::Move;
                    op.srcIsland = islandId;
                    op.srcIndex = idx;
                    op.dstIsland = islandId;
                    op.dstIndex = idx - 1;
                    op.itemId = itemId;
                    m_pendingHudOps.push_back(std::move(op));
                }
                if (idx + 1 < count && ImGui::MenuItem(ctx.isVertical ? "Move Down" : "Move Right")) {
                    PendingHudOp op;
                    op.type = PendingHudOp::Type::Move;
                    op.srcIsland = islandId;
                    op.srcIndex = idx;
                    op.dstIsland = islandId;
                    op.dstIndex = idx + 2;
                    op.itemId = itemId;
                    m_pendingHudOps.push_back(std::move(op));
                }
                if (ImGui::MenuItem("Insert Separator Before")) {
                    PendingHudOp op;
                    op.type = PendingHudOp::Type::Insert;
                    op.dstIsland = islandId;
                    op.dstIndex = idx;
                    op.itemId = "separator";
                    m_pendingHudOps.push_back(std::move(op));
                }
                if (ImGui::MenuItem("Insert Separator After")) {
                    PendingHudOp op;
                    op.type = PendingHudOp::Type::Insert;
                    op.dstIsland = islandId;
                    op.dstIndex = idx + 1;
                    op.itemId = "separator";
                    m_pendingHudOps.push_back(std::move(op));
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Remove from HUD")) {
                    PendingHudOp op;
                    op.type = PendingHudOp::Type::Remove;
                    op.srcIsland = islandId;
                    op.srcIndex = idx;
                    op.itemId = itemId;
                    m_pendingHudOps.push_back(std::move(op));
                }
                ImGui::EndPopup();
            }

            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (isSelfBeingDragged) {
                dl->AddRectFilled(pMin, slotMax, IM_COL32(20, 22, 26, 150), 3.0f * ctx.scale);
                dl->AddRect(pMin, slotMax, IM_COL32(20, 235, 195, 190), 3.0f * ctx.scale, 0, 1.5f * ctx.scale);
            } else if ((slotHovered || hoveringSlot) && !anyHudDragActive) {
                dl->AddRect(pMin, slotMax, IM_COL32(255, 200, 70, 220), 3.0f * ctx.scale, 0, 1.5f * ctx.scale);
                dl->AddRectFilled(delMin, delMax, hoveringDel ? IM_COL32(220, 50, 50, 245) : IM_COL32(30, 30, 35, 220), 2.0f * ctx.scale);
                float pad = badgeSize * 0.25f;
                dl->AddLine(ImVec2(delMin.x + pad, delMin.y + pad), ImVec2(delMax.x - pad, delMax.y - pad), IM_COL32(255, 255, 255, 240), 1.5f);
                dl->AddLine(ImVec2(delMax.x - pad, delMin.y + pad), ImVec2(delMin.x + pad, delMax.y - pad), IM_COL32(255, 255, 255, 240), 1.5f);
                ImGui::SetTooltip("%s\nDrag to move • Click × or drag off to remove • Right-click for options", def->label.c_str());
            } else if (!anyHudDragActive) {
                dl->AddRect(pMin, slotMax, IM_COL32(255, 200, 70, 55), 3.0f * ctx.scale, 0, 1.0f * ctx.scale);
            }
        }

        ImGui::PopID();
    }

    // In Customize Mode, append a '+' button at the end of the island (also acts as end drop target)
    if (ctx.customizeMode) {
        if (!ctx.isVertical && count > 0) {
            if (maxRowWidth > 0.0f) {
                if (currentRowW + ctx.squareSize <= maxRowWidth) {
                    ImGui::SameLine(0.0f, 0.0f);
                } else {
                    ImGui::SetCursorPosX(winStartPos.x);
                }
            } else {
                ImGui::SameLine();
            }
        }

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.05f, 0.35f, 0.30f, 0.75f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.08f, 0.55f, 0.45f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.03f, 0.28f, 0.22f, 1.0f));
        char addId[32];
        snprintf(addId, sizeof(addId), ICON_LC_PLUS "##hudAddBtn_%d", static_cast<int>(islandId));
        if (ImGui::Button(addId, ImVec2(ctx.squareSize, ctx.squareSize))) {
            char popupId[48];
            snprintf(popupId, sizeof(popupId), "##hudQuickAddPopup_%d", static_cast<int>(islandId));
            ImGui::OpenPopup(popupId);
        }
        ImGui::PopStyleColor(3);

        if (ImGui::IsItemHovered() && !ImGui::GetDragDropPayload()) {
            ImGui::SetTooltip("Add button to this island (or drop button here)");
        }
        handleDropOnSlot(count, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true);
        drawHudQuickAddPopup(islandId, ctx.fontScale);

        // Island-window fallback drop target (handles gaps between buttons, padding, empty islands, and self-drops)
        if (ImGui::GetDragDropPayload() != nullptr) {
            ImGuiWindow* curWin = ImGui::GetCurrentWindow();
            if (curWin) {
                ImRect bgRect = curWin->Rect();
                bgRect.Expand(2.0f);
                if (ImGui::BeginDragDropTargetCustom(bgRect, curWin->GetID("##hudIslandBgDrop"))) {
                    if (const ImGuiPayload* peek = ImGui::AcceptDragDropPayload("HUD_ITEM",
                            ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                        if (peek->DataSize == sizeof(HudDragPayload)) {
                            const auto* payload = static_cast<const HudDragPayload*>(peek->Data);
                            const HudItemDef* draggedDef = m_hudRegistry.find(payload->itemId);
                            bool compatible = draggedDef && (ctx.isVertical ? draggedDef->allowVertical : draggedDef->allowHorizontal);

                            if (compatible) {
                                ImVec2 mp = ImGui::GetMousePos();
                                bool droppedOnSelf = false;
                                if (payload->srcIsland == static_cast<int>(islandId) &&
                                    payload->srcIndex >= 0 && payload->srcIndex < count) {
                                    const SlotBox& selfBox = slotBoxes[static_cast<size_t>(payload->srcIndex)];
                                    droppedOnSelf = (mp.x >= selfBox.min.x && mp.x <= selfBox.max.x &&
                                                     mp.y >= selfBox.min.y && mp.y <= selfBox.max.y);
                                }

                                if (!droppedOnSelf) {
                                    int insertIdx = count;
                                    if (count > 0) {
                                        if (!ctx.isVertical && maxRowWidth > 0.0f) {
                                            // 2D wrapped grid (e.g. TopLeft): pick nearest slot box
                                            int bestS = 0;
                                            float bestDistSq = 1e30f;
                                            for (int s = 0; s < count; ++s) {
                                                const SlotBox& sb = slotBoxes[static_cast<size_t>(s)];
                                                float cx = std::clamp(mp.x, sb.min.x, sb.max.x);
                                                float cy = std::clamp(mp.y, sb.min.y, sb.max.y);
                                                float dx = mp.x - cx;
                                                float dy = mp.y - cy;
                                                float dSq = dx * dx + dy * dy;
                                                if (dSq < bestDistSq) {
                                                    bestDistSq = dSq;
                                                    bestS = s;
                                                }
                                            }
                                            const SlotBox& bestBox = slotBoxes[static_cast<size_t>(bestS)];
                                            insertIdx = (mp.x >= 0.5f * (bestBox.min.x + bestBox.max.x)) ? (bestS + 1) : bestS;
                                        } else {
                                            for (int s = 0; s < count; ++s) {
                                                const SlotBox& sb = slotBoxes[static_cast<size_t>(s)];
                                                float center = ctx.isVertical ? 0.5f * (sb.min.y + sb.max.y)
                                                                              : 0.5f * (sb.min.x + sb.max.x);
                                                float coord = ctx.isVertical ? mp.y : mp.x;
                                                if (coord < center) {
                                                    insertIdx = s;
                                                    break;
                                                }
                                            }
                                        }
                                    }

                                    ImDrawList* fdl = ImGui::GetForegroundDrawList();
                                    ImU32 lineCol = IM_COL32(20, 235, 195, 255);
                                    float thick = std::max(2.0f, 2.5f * ctx.scale);
                                    if (count > 0) {
                                        int refSlot = std::min(insertIdx, count - 1);
                                        bool atEnd = (insertIdx >= count);
                                        const SlotBox& sb = slotBoxes[static_cast<size_t>(refSlot)];
                                        if (ctx.isVertical) {
                                            float y = atEnd ? sb.max.y : sb.min.y;
                                            fdl->AddLine(ImVec2(sb.min.x - 2.0f, y), ImVec2(sb.max.x + 2.0f, y), lineCol, thick);
                                        } else {
                                            float x = atEnd ? sb.max.x : sb.min.x;
                                            fdl->AddLine(ImVec2(x, sb.min.y - 2.0f), ImVec2(x, sb.max.y + 2.0f), lineCol, thick);
                                        }
                                    } else {
                                        fdl->AddRect(bgRect.Min, bgRect.Max, lineCol, 4.0f * ctx.scale, 0, thick);
                                    }

                                    if (peek->IsDelivery()) {
                                        PendingHudOp op;
                                        op.type = (payload->srcIsland >= 0) ? PendingHudOp::Type::Move : PendingHudOp::Type::Insert;
                                        op.srcIsland = (payload->srcIsland >= 0) ? static_cast<HudIslandId>(payload->srcIsland) : islandId;
                                        op.srcIndex = payload->srcIndex;
                                        op.dstIsland = islandId;
                                        op.dstIndex = insertIdx;
                                        op.itemId = payload->itemId;
                                        m_pendingHudOps.push_back(std::move(op));
                                        m_hudDropHandledThisFrame = true;
                                    }
                                } else if (peek->IsDelivery()) {
                                    // Dropped back onto itself: keep in place
                                    m_hudDropHandledThisFrame = true;
                                }
                            } else {
                                ImGui::GetForegroundDrawList()->AddRect(
                                    bgRect.Min, bgRect.Max, IM_COL32(235, 70, 70, 220), 4.0f * ctx.scale, 0, 2.0f * ctx.scale);
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
            }
        }
    }

    // Right-click on island background (or empty area) to toggle customize mode
    if (ImGui::BeginPopupContextWindow("##hudIslandContextMenu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        ImGui::SetWindowFontScale(ctx.fontScale);
        if (ImGui::MenuItem(m_customizeHudMode ? "Finish Customizing HUD" : "Customize HUD Buttons...")) {
            toggleHudCustomizeMode();
        }
        if (ImGui::MenuItem("Open HUD Button Catalog...", nullptr, &m_showHudCatalog)) {
            if (m_showHudCatalog) m_customizeHudMode = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset HUD Layout to Default")) {
            m_hudLayout.resetToDefault();
        }
        ImGui::EndPopup();
    }
}

void GuiManager::drawHudCustomizeBanner() {
    if (!m_customizeHudMode) return;

    float scale = getUiScale();
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y - 12.0f * scale),
        ImGuiCond_Appearing,
        ImVec2(0.5f, 1.0f));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.09f, 0.12f, 0.14f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.95f, 0.72f, 0.20f, 0.90f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * scale, 6.0f * scale));

    ImGuiWindowFlags bFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                              ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::Begin("##HudCustomizeBanner", nullptr, bFlags)) {
        ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.25f, 1.0f), ICON_LC_PENCIL " HUD Customize Mode");
        ImGui::SameLine();
        ImGui::TextDisabled("| Drag buttons to reorder/move, drag outside or click × to remove");
        ImGui::SameLine();
        if (ImGui::Button(m_showHudCatalog ? "Hide Catalog" : "Show Catalog")) {
            m_showHudCatalog = !m_showHudCatalog;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_LC_ROTATE_CCW " Reset Default")) {
            m_hudLayout.resetToDefault();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.01f, 0.52f, 0.45f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.02f, 0.65f, 0.54f, 1.0f));
        if (ImGui::Button("Done")) {
            exitHudCustomizeMode();
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void GuiManager::drawHudCustomizeCatalog(SculptManager& sculpt, Scene& scene) {
    (void)sculpt;
    (void)scene;
    if (!m_showHudCatalog) return;

    float scale = getUiScale();
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(500.0f * scale, 540.0f * scale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

    bool open = m_showHudCatalog;
    if (ImGui::Begin("HUD Button Catalog##HudCustomizeCatalog", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextWrapped("Drag any button onto any HUD island, or toggle which islands have it (+Top-L / +Left / +Top-R). The same button can live on multiple islands.");
        ImGui::Spacing();

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 110.0f * scale);
        ImGui::InputTextWithHint("##hudCatSearch", ICON_LC_SEARCH " Search tools, brushes, panels...", m_hudCatalogSearch, sizeof(m_hudCatalogSearch));
        ImGui::SameLine();
        if (ImGui::Button(ICON_LC_ROTATE_CCW " Reset All", ImVec2(-1.0f, 0.0f))) {
            m_hudLayout.resetToDefault();
        }

        static const char* kCategories[] = { "All", "Tools", "Brushes", "Symmetry", "Viewport", "Panels", "Actions", "Layout" };
        if (ImGui::BeginTabBar("##hudCatTabs")) {
            for (int c = 0; c < IM_ARRAYSIZE(kCategories); ++c) {
                if (ImGui::BeginTabItem(kCategories[c])) {
                    m_hudCatalogCategory = c;
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }

        std::string searchLower = toLowerAscii(m_hudCatalogSearch);
        const char* activeCat = kCategories[std::clamp(m_hudCatalogCategory, 0, (int)IM_ARRAYSIZE(kCategories) - 1)];

        if (ImGui::BeginChild("##hudCatList", ImVec2(0.0f, 0.0f), true)) {
            float rowH = ImGui::GetFrameHeight();
            const float chipW = std::max(54.0f * scale, ImGui::CalcTextSize(ICON_LC_CHECK " Top-R").x + 12.0f * scale);
            const float chipsTotalW = chipW * 3.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;

            struct IslandChipInfo {
                HudIslandId id;
                const char* shortLabel;
            };
            static constexpr IslandChipInfo kIslands[3] = {
                { HudIslandId::TopLeft,  "Top-L" },
                { HudIslandId::LeftVert, "Left"  },
                { HudIslandId::TopRight, "Top-R" },
            };

            int rowIdx = 0;
            for (const auto& item : m_hudRegistry.getAll()) {
                if (m_hudCatalogCategory > 0 && item.category != activeCat) continue;

                if (!searchLower.empty()) {
                    std::string hay = toLowerAscii(item.label + " " + item.category + " " + item.id);
                    if (hay.find(searchLower) == std::string::npos) continue;
                }

                ImGui::PushID(rowIdx++);

                // Icon preview
                GLuint tex = !item.iconTextureKey.empty() ? getIconTexture(item.iconTextureKey) : 0;
                if (tex != 0) {
                    ImGui::Image((ImTextureID)(uintptr_t)tex, ImVec2(rowH, rowH), ImVec2(0, 1), ImVec2(1, 0));
                } else {
                    ImGui::BeginDisabled();
                    ImGui::Button(item.iconText.c_str(), ImVec2(rowH, rowH));
                    ImGui::EndDisabled();
                }

                ImGui::SameLine();

                std::string selLabel = item.label;
                if (!item.allowHorizontal) selLabel += " (Vert)";

                ImVec2 avail = ImGui::GetContentRegionAvail();
                ImGui::Selectable(selLabel.c_str(), false, ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(std::max(40.0f, avail.x - chipsTotalW - 8.0f * scale), rowH));

                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    HudDragPayload payload{};
                    payload.srcIsland = -1; // From catalog
                    payload.srcIndex = -1;
                    std::strncpy(payload.itemId, item.id.c_str(), sizeof(payload.itemId) - 1);
                    ImGui::SetDragDropPayload("HUD_ITEM", &payload, sizeof(payload));
                    ImGui::Text("%s %s", item.iconText.c_str(), item.label.c_str());
                    ImGui::EndDragDropSource();
                }

                for (int k = 0; k < 3; ++k) {
                    ImGui::SameLine();
                    HudIslandId isl = kIslands[k].id;
                    bool dstVert = HudLayoutConfig::isIslandVertical(isl);
                    bool compatible = dstVert ? item.allowVertical : item.allowHorizontal;
                    int existingIdx = m_hudLayout.findItemIndex(isl, item.id);
                    bool onIsland = !item.allowMultiple && (existingIdx >= 0);

                    ImGui::PushID(k);
                    if (!compatible) ImGui::BeginDisabled();
                    if (onIsland) pushAccentColors(HudAccent::Teal);

                    char chipLabel[48];
                    snprintf(chipLabel, sizeof(chipLabel), "%s %s", onIsland ? ICON_LC_CHECK : ICON_LC_PLUS, kIslands[k].shortLabel);

                    if (ImGui::Button(chipLabel, ImVec2(chipW, rowH))) {
                        PendingHudOp op;
                        if (onIsland) {
                            op.type = PendingHudOp::Type::Remove;
                            op.srcIsland = isl;
                            op.srcIndex = existingIdx;
                            op.itemId = item.id;
                        } else {
                            op.type = PendingHudOp::Type::Insert;
                            op.dstIsland = isl;
                            op.dstIndex = static_cast<int>(m_hudLayout.items(isl).size());
                            op.itemId = item.id;
                        }
                        m_pendingHudOps.push_back(std::move(op));
                    }

                    if (onIsland) popAccentColors(HudAccent::Teal);
                    if (!compatible) {
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            ImGui::SetTooltip("Not supported on %s", HudLayoutConfig::islandDisplayName(isl));
                        }
                    } else if (ImGui::IsItemHovered()) {
                        if (onIsland) {
                            ImGui::SetTooltip("Active on %s (click to remove)", HudLayoutConfig::islandDisplayName(isl));
                        } else {
                            ImGui::SetTooltip("Add to %s", HudLayoutConfig::islandDisplayName(isl));
                        }
                    }
                    ImGui::PopID();
                }

                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        // Dropping an island button onto the Catalog window removes it from the HUD
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HUD_ITEM")) {
                if (p->DataSize == sizeof(HudDragPayload)) {
                    const auto* payload = static_cast<const HudDragPayload*>(p->Data);
                    if (payload->srcIsland >= 0) {
                        PendingHudOp op;
                        op.type = PendingHudOp::Type::Remove;
                        op.srcIsland = static_cast<HudIslandId>(payload->srcIsland);
                        op.srcIndex = payload->srcIndex;
                        op.itemId = payload->itemId;
                        m_pendingHudOps.push_back(std::move(op));
                        m_hudDropHandledThisFrame = true;
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
    }
    ImGui::End();

    if (!open) {
        m_showHudCatalog = false;
    }
}

void GuiManager::drawBrushHoverSlidersPopover(SculptManager& sculpt, const HudRenderCtx& baseCtx) {
    if (!m_brushHoverBtnDrawnThisFrame || m_customizeHudMode || ImGui::GetDragDropPayload() != nullptr) {
        m_brushHoverPopoverOpen = false;
        m_brushHoverCloseTimer = 0.0f;
        m_brushHoverPopoverHovered = false;
        m_brushHoverSliderActive = false;
        return;
    }

    ImVec2 mp = ImGui::GetMousePos();
    bool hasPrevPopRect = (m_brushHoverPopMax.x > m_brushHoverPopMin.x &&
                           m_brushHoverPopMax.y > m_brushHoverPopMin.y);

    auto inRect = [](const ImVec2& p, const ImVec2& rMin, const ImVec2& rMax) {
        return p.x >= rMin.x && p.x <= rMax.x && p.y >= rMin.y && p.y <= rMax.y;
    };

    // Keep hover alive while moving across the small gap between trigger button and popover
    bool inBridge = false;
    if (m_brushHoverPopoverOpen && hasPrevPopRect) {
        const ImVec2& bMin = m_brushHoverBtnMin;
        const ImVec2& bMax = m_brushHoverBtnMax;
        const ImVec2& pMin = m_brushHoverPopMin;
        const ImVec2& pMax = m_brushHoverPopMax;
        if (pMin.x >= bMax.x - 2.0f) {
            inBridge = inRect(mp, ImVec2(bMax.x - 2.0f, std::min(bMin.y, pMin.y)),
                                  ImVec2(pMin.x + 2.0f, std::max(bMax.y, pMax.y)));
        } else if (pMax.x <= bMin.x + 2.0f) {
            inBridge = inRect(mp, ImVec2(pMax.x - 2.0f, std::min(bMin.y, pMin.y)),
                                  ImVec2(bMin.x + 2.0f, std::max(bMax.y, pMax.y)));
        } else if (pMin.y >= bMax.y - 2.0f) {
            inBridge = inRect(mp, ImVec2(std::min(bMin.x, pMin.x), bMax.y - 2.0f),
                                  ImVec2(std::max(bMax.x, pMax.x), pMin.y + 2.0f));
        } else if (pMax.y <= bMin.y + 2.0f) {
            inBridge = inRect(mp, ImVec2(std::min(bMin.x, pMin.x), pMax.y - 2.0f),
                                  ImVec2(std::max(bMax.x, pMax.x), bMin.y + 2.0f));
        }
    }

    if (m_brushHoverBtnHovered) {
        m_brushHoverPopoverOpen = true;
        m_brushHoverCloseTimer = 0.18f;
    } else if (m_brushHoverPopoverOpen) {
        if (m_brushHoverSliderActive || m_brushHoverPopoverHovered || inBridge) {
            m_brushHoverCloseTimer = 0.18f;
        } else {
            m_brushHoverCloseTimer -= ImGui::GetIO().DeltaTime;
            if (m_brushHoverCloseTimer <= 0.0f) {
                m_brushHoverPopoverOpen = false;
            }
        }
    }

    if (!m_brushHoverPopoverOpen) {
        m_brushHoverPopoverHovered = false;
        m_brushHoverSliderActive = false;
        return;
    }

    float scale = baseCtx.scale;
    float gap = 4.0f * scale;
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    float colW = std::max(24.0f * scale, (m_brushHoverBtnMax.x - m_brushHoverBtnMin.x));
    float sliderH = 100.0f * scale;
    ImVec2 estSize(colW * 3.0f + 28.0f * scale, sliderH + 46.0f * scale);
    ImVec2 prevSize = hasPrevPopRect
        ? ImVec2(m_brushHoverPopMax.x - m_brushHoverPopMin.x, m_brushHoverPopMax.y - m_brushHoverPopMin.y)
        : estSize;

    ImVec2 winPos;
    if (m_brushHoverIsland == HudIslandId::TopRight) {
        winPos = ImVec2(m_brushHoverBtnMax.x - prevSize.x, m_brushHoverBtnMax.y + gap);
    } else if (m_brushHoverIsland == HudIslandId::TopLeft &&
               (m_brushHoverBtnMax.x + gap + prevSize.x > viewport->WorkPos.x + viewport->WorkSize.x)) {
        winPos = ImVec2(m_brushHoverBtnMin.x, m_brushHoverBtnMax.y + gap);
    } else {
        winPos = ImVec2(m_brushHoverBtnMax.x + gap, m_brushHoverBtnMin.y);
    }

    float minX = viewport->WorkPos.x + 4.0f;
    float minY = viewport->WorkPos.y + 4.0f;
    float maxX = std::max(minX, viewport->WorkPos.x + viewport->WorkSize.x - prevSize.x - 4.0f);
    float maxY = std::max(minY, viewport->WorkPos.y + viewport->WorkSize.y - prevSize.y - 4.0f);
    winPos.x = std::clamp(winPos.x, minX, maxX);
    winPos.y = std::clamp(winPos.y, minY, maxY);

    ImGui::SetNextWindowPos(winPos, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.09f, 0.10f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.01f, 0.52f, 0.45f, 0.70f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * scale, 6.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * scale, 4.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * scale);

    ImGuiWindowFlags popFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;

    bool popHovered = false;
    bool anyActive = false;
    bool anySliderHovered = false;

    if (ImGui::Begin("##hudBrushSlidersHoverWin", nullptr, popFlags)) {
        ImGui::SetWindowFontScale(baseCtx.fontScale);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

        struct ColSpec {
            BrushSliderKind kind;
            const char* widgetId;
        };
        static constexpr ColSpec kCols[3] = {
            { BrushSliderKind::Intensity,  "##hudPopIntensity" },
            { BrushSliderKind::Radius,     "##hudPopRadius" },
            { BrushSliderKind::FocalShift, "##hudPopFocalShift" },
        };

        for (int c = 0; c < 3; ++c) {
            if (c > 0) ImGui::SameLine();
            ImGui::BeginGroup();

            const char* icon = brushSliderIcon(kCols[c].kind);
            float iconW = ImGui::CalcTextSize(icon).x;
            float colX = ImGui::GetCursorPosX();
            ImGui::SetCursorPosX(colX + std::max(0.0f, (colW - iconW) * 0.5f));
            ImGui::TextUnformatted(icon);
            bool iconHovered = ImGui::IsItemHovered();

            ImGui::SetCursorPosX(colX);
            BrushSliderResult sr = drawVerticalBrushSlider(
                sculpt, kCols[c].kind, kCols[c].widgetId, ImVec2(colW, sliderH), false);

            if (iconHovered && !sr.hovered) {
                setBrushSliderTooltip(kCols[c].kind, sculpt);
            }

            if (kCols[c].kind == BrushSliderKind::Intensity) {
                m_sliderIntensityMin = sr.rectMin;
                m_sliderIntensityMax = sr.rectMax;
            } else if (kCols[c].kind == BrushSliderKind::Radius) {
                m_sliderRadiusMin = sr.rectMin;
                m_sliderRadiusMax = sr.rectMax;
            } else if (kCols[c].kind == BrushSliderKind::FocalShift) {
                m_sliderFocalShiftMin = sr.rectMin;
                m_sliderFocalShiftMax = sr.rectMax;
            }

            if (sr.active) anyActive = true;
            if (sr.hovered || iconHovered) anySliderHovered = true;

            ImGui::EndGroup();
        }

        m_slidersValid = true;
        if (anyActive || anySliderHovered) {
            m_isOverBrushSliders = true;
        }

        m_brushHoverPopMin = ImGui::GetWindowPos();
        ImVec2 wSz = ImGui::GetWindowSize();
        m_brushHoverPopMax = ImVec2(m_brushHoverPopMin.x + wSz.x, m_brushHoverPopMin.y + wSz.y);
        popHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_ChildWindows);
    }
    ImGui::End();

    ImGui::PopStyleVar(4);   // WindowRounding, WindowPadding, ItemSpacing, FrameRounding
    ImGui::PopStyleColor(2); // WindowBg, Border

    m_brushHoverPopoverHovered = popHovered;
    m_brushHoverSliderActive = anyActive;
}

void GuiManager::drawFloatingIslandHUD(SculptManager& sculpt, Scene& scene, AngleRenderer& renderer) {
    initHudRegistry();

    if (!m_showFloatingIsland) {
        m_slidersValid = false;
        m_isOverBrushSliders = false;
        m_brushHoverPopoverOpen = false;
        return;
    }

    m_hudDropHandledThisFrame = false;
    m_slidersValid = false;
    m_isOverBrushSliders = false;
    m_brushHoverBtnDrawnThisFrame = false;
    m_brushHoverBtnHovered = false;

    float scale = getFloatingIslandScale();
    float fontScale = m_floatingIslandScale / std::max(0.01f, m_uiScale);
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    HudRenderCtx baseCtx;
    baseCtx.scale = scale;
    baseCtx.fontScale = fontScale;
    baseCtx.customizeMode = m_customizeHudMode;

    // Common style colors & variables for HUD islands
    ImVec4 borderCol = m_customizeHudMode ? ImVec4(0.95f, 0.72f, 0.20f, 0.90f) : ImVec4(0.01f, 0.52f, 0.45f, 0.40f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.09f, 0.10f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_Border, borderCol);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f * scale, 3.0f * scale));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove;

    // ------------------------------------------------------------------------
    // 1. TOP ACTIVE TOOL & ALT ISLAND (Above Left Vertical HUD)
    // ------------------------------------------------------------------------
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y), ImGuiCond_Always, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * scale, 4.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * scale, 4.0f * scale));

    float topIslandHeight = 0.0f;
    if (ImGui::Begin("##FloatingIslandActiveToolTop", nullptr, flags)) {
        ImGui::SetWindowFontScale(fontScale);
        baseCtx.squareSize = ImGui::GetFrameHeight();
        drawHudIslandSlots(HudIslandId::TopLeft, sculpt, scene, renderer, baseCtx, baseCtx.squareSize * 2.0f + 2.0f);
        topIslandHeight = ImGui::GetWindowSize().y;
    }
    ImGui::End();
    ImGui::PopStyleVar(2); // WindowPadding, ItemSpacing

    // ------------------------------------------------------------------------
    // 2. VERTICAL BRUSH HUD (Left, below Top Active Tool Island)
    // ------------------------------------------------------------------------
    float topGap = (topIslandHeight > 0.0f) ? topIslandHeight : 56.0f * scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + topGap), ImGuiCond_Always, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * scale, 4.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * scale, 6.0f * scale));

    if (ImGui::Begin("##FloatingIslandBrushVert", nullptr, flags)) {
        ImGui::SetWindowFontScale(fontScale);
        baseCtx.squareSize = ImGui::GetFrameHeight();
        drawHudIslandSlots(HudIslandId::LeftVert, sculpt, scene, renderer, baseCtx);
    }
    ImGui::End();
    ImGui::PopStyleVar(2); // WindowPadding, ItemSpacing

    // ------------------------------------------------------------------------
    // 3. HORIZONTAL VIEWPORT & SYMMETRY HUD (Top-Right)
    // ------------------------------------------------------------------------
    ImVec2 posRight = ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y);
    ImGui::SetNextWindowPos(posRight, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, 4.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.0f * scale, 0.0f));

    if (ImGui::Begin("##FloatingIslandHUD_v2", nullptr, flags)) {
        ImGui::SetWindowFontScale(fontScale);
        baseCtx.squareSize = ImGui::GetFrameHeight();
        drawHudIslandSlots(HudIslandId::TopRight, sculpt, scene, renderer, baseCtx);
    }
    ImGui::End();
    ImGui::PopStyleVar(2); // WindowPadding, ItemSpacing

    ImGui::PopStyleVar(3);   // WindowRounding, FrameRounding, FramePadding
    ImGui::PopStyleColor(2); // WindowBg, Border

    // Brush Sliders Hover Popover (if placed on any island and hovered)
    drawBrushHoverSlidersPopover(sculpt, baseCtx);

    // Customize banner + catalog
    if (m_customizeHudMode || m_showHudCatalog) {
        drawHudCustomizeBanner();
        drawHudCustomizeCatalog(sculpt, scene);
    }

    // Track drag-off-island removal: if a HUD item from an island was being dragged and released outside all islands/targets
    const ImGuiPayload* curPayload = ImGui::GetDragDropPayload();
    bool isDraggingHudItem = (curPayload != nullptr && curPayload->IsDataType("HUD_ITEM") &&
                              curPayload->DataSize == sizeof(HudDragPayload));
    if (isDraggingHudItem) {
        std::memcpy(&m_hudLastActiveDrag, curPayload->Data, sizeof(HudDragPayload));
        m_hudHasLastActiveDrag = true;
        m_hudWasDragging = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            m_hudDragCancelledByEsc = true;
        }
        if (m_hudLastActiveDrag.srcIsland >= 0 && !ImGui::IsDragDropPayloadBeingAccepted()) {
            ImVec2 mp = ImGui::GetMousePos();
            ImDrawList* fdl = ImGui::GetForegroundDrawList();
            const char* hint = "Release outside HUD to remove";
            ImVec2 txtSz = ImGui::CalcTextSize(hint);
            ImVec2 boxMin(mp.x + 14.0f, mp.y + 18.0f);
            ImVec2 boxMax(boxMin.x + txtSz.x + 12.0f, boxMin.y + txtSz.y + 8.0f);
            fdl->AddRectFilled(boxMin, boxMax, IM_COL32(140, 30, 35, 225), 4.0f);
            fdl->AddRect(boxMin, boxMax, IM_COL32(240, 80, 80, 245), 4.0f);
            fdl->AddText(ImVec2(boxMin.x + 6.0f, boxMin.y + 4.0f), IM_COL32(255, 240, 240, 255), hint);
        }
        ImGui::GetIO().WantCaptureMouse = true;
    } else if (m_hudWasDragging) {
        if (!m_hudDropHandledThisFrame && !m_hudDragCancelledByEsc &&
            m_hudHasLastActiveDrag && m_hudLastActiveDrag.srcIsland >= 0) {
            PendingHudOp op;
            op.type = PendingHudOp::Type::Remove;
            op.srcIsland = static_cast<HudIslandId>(m_hudLastActiveDrag.srcIsland);
            op.srcIndex = m_hudLastActiveDrag.srcIndex;
            op.itemId = m_hudLastActiveDrag.itemId;
            m_pendingHudOps.push_back(std::move(op));
        }
        m_hudWasDragging = false;
        m_hudHasLastActiveDrag = false;
        m_hudDragCancelledByEsc = false;
    }

    // Apply all queued add/move/remove operations safely after rendering
    applyPendingHudOps();
}

