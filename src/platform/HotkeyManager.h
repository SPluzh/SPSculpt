#pragma once
#include <SDL2/SDL.h>
#include <vector>
#include <string>
#include <memory>
#include "platform/HotkeyPreset.h"

class HotkeyManager {
public:
    struct KeyConflict {
        HKAction actionA;
        HKAction actionB;
        std::string keyString;
    };

    HotkeyManager();
    ~HotkeyManager() = default;

    void init();

    // Active preset
    HotkeyPreset& getActivePreset();
    const HotkeyPreset& getActivePreset() const;
    int getActivePresetIndex() const { return m_activeIndex; }
    void setActivePresetIndex(int idx);
    void setActivePreset(const std::string& name);

    // List of presets
    std::vector<HotkeyPreset>& getPresets() { return m_presets; }
    const std::vector<HotkeyPreset>& getPresets() const { return m_presets; }

    // CRUD presets
    void addPreset(const HotkeyPreset& preset);
    bool removePreset(const std::string& name); // Only user presets
    bool renamePreset(const std::string& oldName, const std::string& newName);
    HotkeyPreset duplicatePreset(const std::string& name, const std::string& newName);
    void resetPresetToDefault(const std::string& name); // Reset to factory defaults

    // Lookups
    HKAction lookupKeyAction(SDL_Keycode key, bool ctrl, bool alt, bool shift) const;
    NavAction lookupMouseNavAction(uint8_t button, bool ctrl, bool alt, bool shift, bool doubleClick, bool isRef2D) const;
    bool isNavigationEvent(const SDL_Event& e, bool isRef2D) const;

    // Modifiers behavior
    const ModifierBehavior& getModifierBehavior() const;

    // Validation
    std::vector<KeyConflict> findConflicts() const;

    // Persistence
    void saveAll();
    void loadAll();
    void saveActivePresetName(IniFile& ini) const;
    void loadActivePresetName(const IniFile& ini);

private:
    std::vector<HotkeyPreset> m_presets;
    int m_activeIndex = 0;
    std::string m_presetsDir = "presets";

    void ensureBuiltinPresets();
};
