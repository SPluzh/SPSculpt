#include "platform/HotkeyManager.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>

namespace fs = std::filesystem;

static bool isCameraAction(NavAction act) {
    return act == NavAction::Orbit ||
           act == NavAction::Pan ||
           act == NavAction::Zoom ||
           act == NavAction::Roll ||
           act == NavAction::RotateLight ||
           act == NavAction::Pan2D ||
           act == NavAction::Zoom2D ||
           act == NavAction::ResetView2D ||
           act == NavAction::ResetView;
}

HotkeyManager::HotkeyManager() {
    ensureBuiltinPresets();
}

void HotkeyManager::ensureBuiltinPresets() {
    auto findPreset = [this](const std::string& name) -> int {
        for (size_t i = 0; i < m_presets.size(); ++i) {
            if (m_presets[i].name == name) return (int)i;
        }
        return -1;
    };

    if (findPreset("ZBrush") == -1) {
        m_presets.push_back(HotkeyPreset::createZBrush());
    }
    if (findPreset("Maya") == -1) {
        m_presets.push_back(HotkeyPreset::createMaya());
    }
    if (findPreset("Blender") == -1) {
        m_presets.push_back(HotkeyPreset::createBlender());
    }
}

void HotkeyManager::init() {
    ensureBuiltinPresets();
    loadAll();
}

HotkeyPreset& HotkeyManager::getActivePreset() {
    if (m_presets.empty()) {
        ensureBuiltinPresets();
    }
    if (m_activeIndex < 0 || m_activeIndex >= (int)m_presets.size()) {
        m_activeIndex = 0;
    }
    return m_presets[m_activeIndex];
}

const HotkeyPreset& HotkeyManager::getActivePreset() const {
    if (m_presets.empty()) {
        static HotkeyPreset fallback = HotkeyPreset::createZBrush();
        return fallback;
    }
    if (m_activeIndex < 0 || m_activeIndex >= (int)m_presets.size()) {
        return m_presets[0];
    }
    return m_presets[m_activeIndex];
}

void HotkeyManager::setActivePresetIndex(int idx) {
    if (idx >= 0 && idx < (int)m_presets.size()) {
        m_activeIndex = idx;
    }
}

void HotkeyManager::setActivePreset(const std::string& name) {
    for (size_t i = 0; i < m_presets.size(); ++i) {
        if (m_presets[i].name == name) {
            m_activeIndex = (int)i;
            return;
        }
    }
}

void HotkeyManager::addPreset(const HotkeyPreset& preset) {
    for (size_t i = 0; i < m_presets.size(); ++i) {
        if (m_presets[i].name == preset.name) {
            m_presets[i] = preset;
            return;
        }
    }
    m_presets.push_back(preset);
}

bool HotkeyManager::removePreset(const std::string& name) {
    for (auto it = m_presets.begin(); it != m_presets.end(); ++it) {
        if (it->name == name) {
            if (it->isBuiltin) {
                return false; // Cannot remove built-in presets
            }
            // Remove file if exists
            std::string filePath = m_presetsDir + "/" + name + ".preset";
            std::error_code ec;
            fs::remove(filePath, ec);

            int removedIdx = (int)std::distance(m_presets.begin(), it);
            m_presets.erase(it);

            if (m_activeIndex >= (int)m_presets.size()) {
                m_activeIndex = std::max(0, (int)m_presets.size() - 1);
            }
            return true;
        }
    }
    return false;
}

bool HotkeyManager::renamePreset(const std::string& oldName, const std::string& newName) {
    if (newName.empty() || oldName == newName) return false;

    // Check if new name already taken
    for (const auto& p : m_presets) {
        if (p.name == newName) return false;
    }

    for (auto& p : m_presets) {
        if (p.name == oldName) {
            if (p.isBuiltin) return false; // Cannot rename built-ins

            std::string oldPath = m_presetsDir + "/" + oldName + ".preset";
            std::string newPath = m_presetsDir + "/" + newName + ".preset";
            std::error_code ec;
            fs::remove(oldPath, ec);

            p.name = newName;
            p.saveToFile(newPath);
            return true;
        }
    }
    return false;
}

HotkeyPreset HotkeyManager::duplicatePreset(const std::string& name, const std::string& newName) {
    for (const auto& p : m_presets) {
        if (p.name == name) {
            HotkeyPreset copy = p;
            copy.name = newName;
            copy.isBuiltin = false;
            m_presets.push_back(copy);
            m_activeIndex = (int)m_presets.size() - 1;

            std::error_code ec;
            fs::create_directories(m_presetsDir, ec);
            copy.saveToFile(m_presetsDir + "/" + newName + ".preset");
            return copy;
        }
    }
    return HotkeyPreset::createZBrush();
}

void HotkeyManager::resetPresetToDefault(const std::string& name) {
    for (size_t i = 0; i < m_presets.size(); ++i) {
        if (m_presets[i].name == name) {
            if (name == "ZBrush") {
                m_presets[i] = HotkeyPreset::createZBrush();
            } else if (name == "Maya") {
                m_presets[i] = HotkeyPreset::createMaya();
            } else if (name == "Blender") {
                m_presets[i] = HotkeyPreset::createBlender();
            }
            std::string path = m_presetsDir + "/" + name + ".preset";
            std::error_code ec;
            fs::remove(path, ec); // Remove file override to revert to pure built-in code
            return;
        }
    }
}

HKAction HotkeyManager::lookupKeyAction(SDL_Keycode key, bool ctrl, bool alt, bool shift) const {
    const auto& preset = getActivePreset();
    for (const auto& [action, binding] : preset.keyBindings) {
        if (binding.matches(key, ctrl, alt, shift)) {
            return action;
        }
    }
    if (!ctrl && !alt && !shift && (key == SDLK_r || key == SDLK_7)) {
        return HKAction::ToolPinch;
    }
    return HKAction::None;
}

NavAction HotkeyManager::lookupMouseNavAction(uint8_t button, bool ctrl, bool alt, bool shift, bool doubleClick, bool isRef2D) const {
    const auto& preset = getActivePreset();

    // Check exact matches (including doubleClick flag)
    for (const auto& b : preset.mouseNavBindings) {
        if (b.matches(button, ctrl, alt, shift, doubleClick, isRef2D)) {
            return b.action;
        }
    }

    // If double-click did not find a double-click specific action, check if single-click applies
    if (doubleClick) {
        for (const auto& b : preset.mouseNavBindings) {
            if (b.matches(button, ctrl, alt, shift, false, isRef2D)) {
                return b.action;
            }
        }
    }

    return NavAction::None;
}

bool HotkeyManager::isNavigationEvent(const SDL_Event& e, bool isRef2D) const {
    if (e.type == SDL_MOUSEWHEEL) {
        return true;
    }

    if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
        bool ctrl = (SDL_GetModState() & KMOD_CTRL) != 0;
        bool alt = (SDL_GetModState() & KMOD_ALT) != 0;
        bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
        bool doubleClick = (e.button.clicks >= 2);

        NavAction act = lookupMouseNavAction(e.button.button, ctrl, alt, shift, doubleClick, isRef2D);
        if (isCameraAction(act)) {
            return true;
        }
    }

    return false;
}

const ModifierBehavior& HotkeyManager::getModifierBehavior() const {
    return getActivePreset().modifierBehavior;
}

std::vector<HotkeyManager::KeyConflict> HotkeyManager::findConflicts() const {
    std::vector<KeyConflict> conflicts;
    const auto& preset = getActivePreset();

    std::vector<std::pair<HKAction, KeyBinding>> list(preset.keyBindings.begin(), preset.keyBindings.end());
    for (size_t i = 0; i < list.size(); ++i) {
        for (size_t j = i + 1; j < list.size(); ++j) {
            if (list[i].second.isValid() && list[i].second == list[j].second) {
                conflicts.push_back({ list[i].first, list[j].first, list[i].second.toString() });
            }
        }
    }

    return conflicts;
}

void HotkeyManager::saveAll() {
    std::error_code ec;
    fs::create_directories(m_presetsDir, ec);

    for (const auto& p : m_presets) {
        // Save custom presets or modified built-ins
        std::string path = m_presetsDir + "/" + p.name + ".preset";
        p.saveToFile(path);
    }

    // Save active preset name
    std::string activePath = m_presetsDir + "/_active.txt";
    std::ofstream out(activePath);
    if (out.is_open()) {
        out << getActivePreset().name << "\n";
    }
}

void HotkeyManager::loadAll() {
    std::error_code ec;
    if (!fs::exists(m_presetsDir, ec)) {
        return;
    }

    for (const auto& entry : fs::directory_iterator(m_presetsDir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".preset") {
            HotkeyPreset p;
            if (p.loadFromFile(entry.path().string())) {
                addPreset(p);
            }
        }
    }

    // Load active preset name
    std::string activePath = m_presetsDir + "/_active.txt";
    std::ifstream in(activePath);
    if (in.is_open()) {
        std::string activeName;
        std::getline(in, activeName);
        if (!activeName.empty()) {
            setActivePreset(activeName);
        }
    }
}

void HotkeyManager::saveActivePresetName(IniFile& ini) const {
    ini.set("Hotkeys", "activePreset", getActivePreset().name);
}

void HotkeyManager::loadActivePresetName(const IniFile& ini) {
    if (ini.hasKey("Hotkeys", "activePreset")) {
        std::string name = ini.get("Hotkeys", "activePreset");
        setActivePreset(name);
    }
}
