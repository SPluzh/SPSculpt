#pragma once

#include <imgui.h>
#include <array>
#include <functional>
#include <string>
#include <vector>

class GuiManager;
class SculptManager;
class Scene;
class AngleRenderer;
class IniFile;

enum class HudIslandId : int {
    TopLeft  = 0,
    LeftVert = 1,
    TopRight = 2,
    Count    = 3
};

struct HudRenderCtx {
    float       scale        = 1.0f;
    float       fontScale    = 1.0f;
    float       squareSize   = 32.0f;
    bool        isVertical   = false;
    HudIslandId islandId     = HudIslandId::TopRight;
    bool        customizeMode = false;
};

struct HudItemDef {
    std::string id;
    std::string label;
    std::string category;
    std::string iconText;      // Lucide icon or short text for catalog preview
    std::string iconTextureKey; // Optional PNG icon key for catalog preview
    bool allowHorizontal = true;
    bool allowVertical   = true;
    bool allowMultiple   = false;
    std::function<void(GuiManager&, SculptManager&, Scene&, AngleRenderer&, const HudRenderCtx&)> render;
};

struct PendingHudOp {
    enum class Type {
        Move,
        Insert,
        Remove
    };
    Type        type      = Type::Move;
    HudIslandId srcIsland = HudIslandId::TopRight;
    int         srcIndex  = -1;
    HudIslandId dstIsland = HudIslandId::TopRight;
    int         dstIndex  = -1;
    std::string itemId;
};

struct HudDragPayload {
    int  srcIsland = -1; // -1 when dragged from the Catalog palette
    int  srcIndex  = -1;
    char itemId[64] = {0};
};

class HudRegistry {
public:
    void clear();
    void registerItem(HudItemDef def);
    const HudItemDef* find(const std::string& id) const;
    const std::vector<HudItemDef>& getAll() const { return m_items; }

private:
    std::vector<HudItemDef> m_items;
};

class HudLayoutConfig {
public:
    static constexpr int ISLAND_COUNT = static_cast<int>(HudIslandId::Count);

    HudLayoutConfig();

    void resetToDefault();
    void save(IniFile& ini) const;
    void load(const IniFile& ini, const HudRegistry& registry);

    std::vector<std::string>& items(HudIslandId id) {
        return m_islands[static_cast<int>(id)];
    }
    const std::vector<std::string>& items(HudIslandId id) const {
        return m_islands[static_cast<int>(id)];
    }

    bool containsItem(const std::string& itemId) const;
    bool islandContains(HudIslandId id, const std::string& itemId) const;
    int  findItemIndex(HudIslandId id, const std::string& itemId) const;
    void applyOp(const PendingHudOp& op, const HudRegistry& registry);

    static const char* islandIniKey(HudIslandId id);
    static const char* islandDisplayName(HudIslandId id);
    static bool isIslandVertical(HudIslandId id);

private:
    std::array<std::vector<std::string>, ISLAND_COUNT> m_islands;
};

