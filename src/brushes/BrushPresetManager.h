#pragma once
#include "brushes/BrushPreset.h"
#include <vector>
#include <string>

class BrushPresetManager {
public:
    static BrushPresetManager& instance();

    // Loading
    void loadDefaults();                          // Load default list or ZBrushes
    bool loadFromFile(const std::string& path);   // single .json
    int  loadFromFolder(const std::string& dir);  // all of ZBrushes/

    // Access
    const std::vector<BrushPreset>& presets() const { return m_presets; }
    const BrushPreset* findByName(const std::string& name) const;
    const BrushPreset* findByUid(const std::string& uid) const;
    BrushPreset*       findByNameMut(const std::string& name);

    // Management
    void addPreset(BrushPreset p);
    void removePreset(const std::string& uid);
    bool savePreset(const BrushPreset& p, const std::string& path) const;

    // Active preset
    void            setActive(const std::string& uid);
    const BrushPreset* active() const;
    BrushPreset*       activeMut();

private:
    BrushPresetManager() = default;
    std::vector<BrushPreset> m_presets;
    std::string              m_activeUid;
};
