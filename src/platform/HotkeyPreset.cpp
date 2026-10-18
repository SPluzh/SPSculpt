#include "platform/HotkeyPreset.h"
#include <algorithm>
#include <iostream>

static const std::vector<HKActionInfo> s_actionInfoList = {
    // Tools
    { HKAction::ToolPaint,        "ToolPaint",        "Paint Brush",          "PNT", "Tools", false },
    { HKAction::ToolBrush,        "ToolBrush",        "Standard Brush",       "BRU", "Tools", false },
    { HKAction::ToolInflate,      "ToolInflate",      "Inflate",              "INF", "Tools", false },
    { HKAction::ToolTwist,        "ToolTwist",        "Twist",                "TWI", "Tools", false },
    { HKAction::ToolVTool,        "ToolVTool",        "V-Tool",               "VTL", "Tools", false },
    { HKAction::ToolTransform,    "ToolTransform",    "Transform Gizmo",      "TRN", "Tools", false },
    { HKAction::ToolSmooth,       "ToolSmooth",       "Smooth",               "SMO", "Tools", false },
    { HKAction::ToolRelax,        "ToolRelax",        "Relax",                "RLX", "Tools", false },
    { HKAction::ToolFlatten,      "ToolFlatten",      "Flatten",              "FLT", "Tools", false },
    { HKAction::ToolPinch,        "ToolPinch",        "Pinch",                "PIN", "Tools", false },
    { HKAction::ToolCrease,       "ToolCrease",       "Crease",               "CRE", "Tools", false },
    { HKAction::ToolDrag,         "ToolDrag",         "Drag",                 "DRG", "Tools", false },
    { HKAction::ToolMove,         "ToolMove",         "Move / Grab",          "MOV", "Tools", false },
    { HKAction::ToolClayBuildup,  "ToolClayBuildup",  "Clay Buildup",         "CLY", "Tools", false },
    { HKAction::ToolDamStandard,  "ToolDamStandard",  "Dam Standard",         "DAM", "Tools", false },
    { HKAction::ToolTopology,     "ToolTopology",     "Topology Tool",        "TPL", "Tools", false },
    { HKAction::ToolMask,         "ToolMask",         "Mask Brush",           "MSK", "Tools", false },
    { HKAction::ToolVisibility,   "ToolVisibility",   "Visibility Brush",     "VIS", "Tools", false },
    { HKAction::ToolPose,         "ToolPose",         "Pose Tool (Transpose)","POS", "Tools", false },
    { HKAction::ToolSnakeHook,    "ToolSnakeHook",    "SnakeHook",            "SNK", "Tools", false },
    { HKAction::ToolTube,         "ToolTube",         "Tube / Curve Tool",    "TUB", "Tools", false },
    { HKAction::ToolQuadDraw,     "ToolQuadDraw",     "Quad Draw (Retopo)",   "QDR", "Tools", false },

    // Sculpting Parameters
    { HKAction::BrushIntensity,   "BrushIntensity",   "Brush Intensity",      "INT", "Parameters", true },
    { HKAction::BrushRadius,      "BrushRadius",      "Brush Radius",         "RAD", "Parameters", true },
    { HKAction::BrushFocalShift,  "BrushFocalShift",  "Brush Focal Shift",    "FOC", "Parameters", true },
    { HKAction::BrushNegative,    "BrushNegative",    "Toggle Sub / Invert",  "NEG", "Parameters", false },
    { HKAction::BrushPicker,      "BrushPicker",      "Color / Depth Picker", "PCK", "Parameters", false },
    { HKAction::ToggleSymmetry,   "ToggleSymmetry",   "Toggle Symmetry",      "SYM", "Parameters", false },
    { HKAction::RemeshResolution, "RemeshResolution", "Remesh Resolution",   "REM", "Parameters", true },
    { HKAction::TopologyDetail,   "TopologyDetail",   "Topology Detail",      "DET", "Parameters", true },
    { HKAction::DeleteSelected,   "DeleteSelected",   "Delete Selected Mesh", "DEL", "Parameters", false },

    // Camera / Viewport
    { HKAction::CameraFov,        "CameraFov",        "Camera Field of View", "FOV", "Camera", true },
    { HKAction::CameraProjection, "CameraProjection", "Toggle Ortho / Persp", "PRJ", "Camera", false },
    { HKAction::CameraFrame,      "CameraFrame",      "Frame / Focus Mesh",   "FRM", "Camera", false },
    { HKAction::CameraLeft,       "CameraLeft",       "Snap Camera Left",     "LFT", "Camera", false },
    { HKAction::CameraReset,      "CameraReset",      "Reset Camera View",    "RST", "Camera", false },
    { HKAction::CameraUndo,       "CameraUndo",       "Camera Undo",          "CUN", "Camera", false },
    { HKAction::CameraRedo,       "CameraRedo",       "Camera Redo",          "CRD", "Camera", false },

    // Camera Bookmarks
    { HKAction::ApplyBookmark1,   "ApplyBookmark1",   "Bookmark 1",           "BM1", "Bookmarks", false },
    { HKAction::ApplyBookmark2,   "ApplyBookmark2",   "Bookmark 2",           "BM2", "Bookmarks", false },
    { HKAction::ApplyBookmark3,   "ApplyBookmark3",   "Bookmark 3",           "BM3", "Bookmarks", false },
    { HKAction::ApplyBookmark4,   "ApplyBookmark4",   "Bookmark 4",           "BM4", "Bookmarks", false },
    { HKAction::ApplyBookmark5,   "ApplyBookmark5",   "Bookmark 5",           "BM5", "Bookmarks", false },
    { HKAction::ApplyBookmark6,   "ApplyBookmark6",   "Bookmark 6",           "BM6", "Bookmarks", false },
    { HKAction::ApplyBookmark7,   "ApplyBookmark7",   "Bookmark 7",           "BM7", "Bookmarks", false },
    { HKAction::ApplyBookmark8,   "ApplyBookmark8",   "Bookmark 8",           "BM8", "Bookmarks", false },
    { HKAction::ApplyBookmark9,   "ApplyBookmark9",   "Bookmark 9",           "BM9", "Bookmarks", false },

    // Strife Camera
    { HKAction::StrifeLeft,       "StrifeLeft",       "Nudge Camera Left",    "NLF", "Camera", false },
    { HKAction::StrifeRight,      "StrifeRight",      "Nudge Camera Right",   "NRT", "Camera", false },
    { HKAction::StrifeUp,         "StrifeUp",         "Nudge Camera Up",      "NUP", "Camera", false },
    { HKAction::StrifeDown,       "StrifeDown",       "Nudge Camera Down",    "NDN", "Camera", false },

    // Scene / Files
    { HKAction::DuplicateSelection, "DuplicateSelection", "Duplicate Selection", "DUP", "Files", false },
    { HKAction::ClearScene,       "ClearScene",       "Clear Scene (New)",    "NEW", "Files", false },
    { HKAction::Undo,             "Undo",             "Undo",                 "UND", "Files", false },
    { HKAction::Redo,             "Redo",             "Redo",                 "RDO", "Files", false },
    { HKAction::OpenFile,         "OpenFile",         "Open File",            "OPN", "Files", false },
    { HKAction::OpenRecentFile,   "OpenRecentFile",   "Open Recent File",     "REC", "Files", false },
    { HKAction::SaveFile,         "SaveFile",         "Save File",            "SAV", "Files", false },
    { HKAction::SaveFileAs,       "SaveFileAs",       "Save File As",         "SVA", "Files", false },
    { HKAction::ExportOBJ,        "ExportOBJ",        "Export OBJ",           "EXP", "Files", false },
    { HKAction::ToggleDyntopo,    "ToggleDyntopo",    "Toggle Dynamic Topo",  "DYN", "Files", false },
    { HKAction::RunRemesh,        "RunRemesh",        "Run Voxel Remesh",     "RMS", "Files", false },

    // Misc
    { HKAction::ToggleOutliner,   "ToggleOutliner",   "Toggle Outliner",      "OUT", "Misc", false },
    { HKAction::ToggleCameraBookmarks, "ToggleCameraBookmarks", "Toggle Bookmarks", "BKM", "Misc", false },
    { HKAction::SoloSelected,     "SoloSelected",     "Solo Selected Mesh",   "SOL", "Misc", false },
    { HKAction::ToggleRefImagesVisible, "ToggleRefImagesVisible", "Toggle Reference Visibility", "RFV", "Misc", false },
    { HKAction::ToggleRefEditMode,"ToggleRefEditMode","Toggle Reference Edit", "RFE", "Misc", false },
};

const std::vector<HKActionInfo>& getAllHKActions() {
    return s_actionInfoList;
}

const HKActionInfo* getHKActionInfo(HKAction action) {
    for (const auto& info : s_actionInfoList) {
        if (info.action == action) return &info;
    }
    return nullptr;
}

HKAction stringToHKAction(const std::string& str) {
    for (const auto& info : s_actionInfoList) {
        if (str == info.id) return info.action;
    }
    return HKAction::None;
}

std::string hkActionToString(HKAction action) {
    const auto* info = getHKActionInfo(action);
    return info ? info->id : "None";
}

std::string KeyBinding::toString() const {
    if (!isValid()) return "";
    std::string res;
    if (ctrl) res += "Ctrl+";
    if (alt) res += "Alt+";
    if (shift) res += "Shift+";
    const char* keyName = SDL_GetKeyName(key);
    if (keyName && keyName[0] != '\0') {
        res += keyName;
    } else {
        res += "Key" + std::to_string((int)key);
    }
    return res;
}

KeyBinding KeyBinding::fromString(const std::string& str) {
    KeyBinding kb;
    if (str.empty()) return kb;

    std::string s = str;
    bool hasCtrl = false;
    bool hasAlt = false;
    bool hasShift = false;

    while (true) {
        if (s.rfind("Ctrl+", 0) == 0) {
            hasCtrl = true;
            s = s.substr(5);
        } else if (s.rfind("Alt+", 0) == 0) {
            hasAlt = true;
            s = s.substr(4);
        } else if (s.rfind("Shift+", 0) == 0) {
            hasShift = true;
            s = s.substr(6);
        } else {
            break;
        }
    }

    kb.ctrl = hasCtrl;
    kb.alt = hasAlt;
    kb.shift = hasShift;

    if (!s.empty()) {
        SDL_Keycode k = SDL_GetKeyFromName(s.c_str());
        if (k != SDLK_UNKNOWN) {
            kb.key = k;
        } else if (s.size() == 1) {
            char c = s[0];
            if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
            kb.key = static_cast<SDL_Keycode>(c);
        }
    }
    return kb;
}

const char* navActionToString(NavAction action) {
    switch (action) {
        case NavAction::Orbit: return "Orbit";
        case NavAction::Pan: return "Pan";
        case NavAction::Zoom: return "Zoom";
        case NavAction::Roll: return "Roll";
        case NavAction::RotateLight: return "RotateLight";
        case NavAction::Pan2D: return "Pan2D";
        case NavAction::Zoom2D: return "Zoom2D";
        case NavAction::ResetView2D: return "ResetView2D";
        case NavAction::ResetView: return "ResetView";
        case NavAction::SubToolSelect: return "SubToolSelect";
        case NavAction::InvertSculpt: return "InvertSculpt";
        default: return "None";
    }
}

NavAction stringToNavAction(const std::string& str) {
    if (str == "Orbit") return NavAction::Orbit;
    if (str == "Pan") return NavAction::Pan;
    if (str == "Zoom") return NavAction::Zoom;
    if (str == "Roll") return NavAction::Roll;
    if (str == "RotateLight") return NavAction::RotateLight;
    if (str == "Pan2D") return NavAction::Pan2D;
    if (str == "Zoom2D") return NavAction::Zoom2D;
    if (str == "ResetView2D") return NavAction::ResetView2D;
    if (str == "ResetView") return NavAction::ResetView;
    if (str == "SubToolSelect") return NavAction::SubToolSelect;
    if (str == "InvertSculpt") return NavAction::InvertSculpt;
    return NavAction::None;
}

const char* navActionGetDisplayName(NavAction action) {
    switch (action) {
        case NavAction::Orbit: return "Orbit Camera";
        case NavAction::Pan: return "Pan Camera";
        case NavAction::Zoom: return "Zoom Camera";
        case NavAction::Roll: return "Roll Camera";
        case NavAction::RotateLight: return "Rotate Light";
        case NavAction::Pan2D: return "Pan View (2D Ref)";
        case NavAction::Zoom2D: return "Zoom View (2D Ref)";
        case NavAction::ResetView2D: return "Reset View (2D Ref)";
        case NavAction::ResetView: return "Reset View (3D)";
        case NavAction::SubToolSelect: return "Select SubTool";
        case NavAction::InvertSculpt: return "Invert Sculpt";
        default: return "None";
    }
}

std::string MouseNavBinding::toString() const {
    std::string res;
    if (doubleClick) res += "2x";
    if (ctrl) res += "Ctrl+";
    if (alt) res += "Alt+";
    if (shift) res += "Shift+";

    if (mouseButton == SDL_BUTTON_LEFT) res += "LMB";
    else if (mouseButton == SDL_BUTTON_MIDDLE) res += "MMB";
    else if (mouseButton == SDL_BUTTON_RIGHT) res += "RMB";
    else res += "Mouse" + std::to_string((int)mouseButton);

    if (ref2D) res += "(2D)";
    res += ":";
    res += navActionToString(action);
    return res;
}

MouseNavBinding MouseNavBinding::fromString(const std::string& str) {
    MouseNavBinding b;
    size_t colon = str.find(':');
    if (colon == std::string::npos) return b;

    std::string trig = str.substr(0, colon);
    std::string actStr = str.substr(colon + 1);
    b.action = stringToNavAction(actStr);

    if (trig.rfind("2x", 0) == 0) {
        b.doubleClick = true;
        trig = trig.substr(2);
    }
    size_t p2d = trig.find("(2D)");
    if (p2d != std::string::npos) {
        b.ref2D = true;
        trig.erase(p2d, 4);
    }
    while (true) {
        if (trig.rfind("Ctrl+", 0) == 0) {
            b.ctrl = true;
            trig = trig.substr(5);
        } else if (trig.rfind("Alt+", 0) == 0) {
            b.alt = true;
            trig = trig.substr(4);
        } else if (trig.rfind("Shift+", 0) == 0) {
            b.shift = true;
            trig = trig.substr(6);
        } else {
            break;
        }
    }

    if (trig == "LMB") b.mouseButton = SDL_BUTTON_LEFT;
    else if (trig == "MMB") b.mouseButton = SDL_BUTTON_MIDDLE;
    else if (trig == "RMB") b.mouseButton = SDL_BUTTON_RIGHT;
    else if (trig.rfind("Mouse", 0) == 0) {
        try {
            b.mouseButton = static_cast<uint8_t>(std::stoi(trig.substr(5)));
        } catch (...) {
            b.mouseButton = 0;
        }
    }
    return b;
}

void HotkeyPreset::saveToIni(IniFile& ini) const {
    ini.set("Meta", "name", name);
    ini.setBool("Meta", "builtin", isBuiltin);

    // Keys
    for (const auto& info : getAllHKActions()) {
        auto it = keyBindings.find(info.action);
        if (it != keyBindings.end() && it->second.isValid()) {
            ini.set("Keys", info.id, it->second.toString());
        }
    }

    // MouseNav
    for (size_t i = 0; i < mouseNavBindings.size(); ++i) {
        ini.set("MouseNav", std::to_string(i), mouseNavBindings[i].toString());
    }

    // Modifiers
    ini.set("Modifiers", "shiftHoldAction", hkActionToString(modifierBehavior.shiftHoldAction));
    ini.set("Modifiers", "ctrlHoldAction", hkActionToString(modifierBehavior.ctrlHoldAction));
    ini.set("Modifiers", "ctrlShiftHoldAction", hkActionToString(modifierBehavior.ctrlShiftHoldAction));
    ini.setBool("Modifiers", "altInvertsBrush", modifierBehavior.altInvertsBrush);
    ini.setBool("Modifiers", "ctrlAltInvertsBrush", modifierBehavior.ctrlAltInvertsBrush);
    ini.setBool("Modifiers", "ctrlAltSubToolSelect", modifierBehavior.ctrlAltSubToolSelect);
}

bool HotkeyPreset::loadFromIni(const IniFile& ini) {
    if (!ini.hasSection("Meta")) return false;

    name = ini.get("Meta", "name", name);
    isBuiltin = ini.getBool("Meta", "builtin", isBuiltin);

    // Keys
    keyBindings.clear();
    for (const auto& info : getAllHKActions()) {
        if (ini.hasKey("Keys", info.id)) {
            std::string str = ini.get("Keys", info.id);
            KeyBinding kb = KeyBinding::fromString(str);
            if (kb.isValid()) {
                keyBindings[info.action] = kb;
            }
        }
    }

    // MouseNav
    mouseNavBindings.clear();
    if (ini.hasSection("MouseNav")) {
        const auto& keys = ini.getKeys("MouseNav");
        for (const auto& k : keys) {
            std::string entry = ini.get("MouseNav", k);
            MouseNavBinding b = MouseNavBinding::fromString(entry);
            if (b.action != NavAction::None) {
                mouseNavBindings.push_back(b);
            }
        }
    }

    // Modifiers
    if (ini.hasSection("Modifiers")) {
        modifierBehavior.shiftHoldAction = stringToHKAction(ini.get("Modifiers", "shiftHoldAction", "ToolSmooth"));
        modifierBehavior.ctrlHoldAction = stringToHKAction(ini.get("Modifiers", "ctrlHoldAction", "ToolMask"));
        modifierBehavior.ctrlShiftHoldAction = stringToHKAction(ini.get("Modifiers", "ctrlShiftHoldAction", "ToolVisibility"));
        modifierBehavior.altInvertsBrush = ini.getBool("Modifiers", "altInvertsBrush", modifierBehavior.altInvertsBrush);
        modifierBehavior.ctrlAltInvertsBrush = ini.getBool("Modifiers", "ctrlAltInvertsBrush", modifierBehavior.ctrlAltInvertsBrush);
        modifierBehavior.ctrlAltSubToolSelect = ini.getBool("Modifiers", "ctrlAltSubToolSelect", modifierBehavior.ctrlAltSubToolSelect);
    }

    return true;
}

bool HotkeyPreset::saveToFile(const std::string& filepath) const {
    IniFile ini;
    saveToIni(ini);
    return ini.save(filepath);
}

bool HotkeyPreset::loadFromFile(const std::string& filepath) {
    IniFile ini;
    if (!ini.load(filepath)) return false;
    return loadFromIni(ini);
}

// Helper to set common keyboard bindings
static void populateCommonKeyBindings(HotkeyPreset& p) {
    p.keyBindings[HKAction::ToolPaint]         = { SDLK_0 };
    p.keyBindings[HKAction::ToolInflate]       = { SDLK_1 };
    p.keyBindings[HKAction::ToolFlatten]       = { SDLK_2 };
    p.keyBindings[HKAction::ToolVTool]         = { SDLK_3 };
    p.keyBindings[HKAction::ToolTransform]     = { SDLK_4 };
    p.keyBindings[HKAction::ToolSmooth]        = { SDLK_5 };
    p.keyBindings[HKAction::ToolBrush]         = { SDLK_6 };
    p.keyBindings[HKAction::ToolPinch]         = { SDLK_7 };
    p.keyBindings[HKAction::ToolCrease]        = { SDLK_8 };
    p.keyBindings[HKAction::ToolDrag]          = { SDLK_9 };

    p.keyBindings[HKAction::ToolMove]          = { SDLK_q };
    p.keyBindings[HKAction::ToolClayBuildup]    = { SDLK_w };
    p.keyBindings[HKAction::ToolDamStandard]   = { SDLK_e };
    p.keyBindings[HKAction::ToolTube]          = { SDLK_t };

    p.keyBindings[HKAction::BrushIntensity]    = { SDLK_a };
    p.keyBindings[HKAction::BrushRadius]       = { SDLK_s };
    p.keyBindings[HKAction::BrushFocalShift]   = { SDLK_d };
    p.keyBindings[HKAction::BrushNegative]     = { SDLK_n };
    p.keyBindings[HKAction::BrushPicker]       = { SDLK_i };
    p.keyBindings[HKAction::ToggleSymmetry]    = { SDLK_x, false, true, false }; // Alt+X
    p.keyBindings[HKAction::RemeshResolution]  = { SDLK_x };
    p.keyBindings[HKAction::ToggleRefEditMode] = { SDLK_z };
    p.keyBindings[HKAction::ToggleRefImagesVisible] = { SDLK_z, false, false, true }; // Shift+Z
    p.keyBindings[HKAction::DeleteSelected]    = { SDLK_DELETE };

    p.keyBindings[HKAction::CameraFov]         = { SDLK_g };
    p.keyBindings[HKAction::CameraProjection]  = { SDLK_p };
    p.keyBindings[HKAction::CameraFrame]       = { SDLK_f };
    p.keyBindings[HKAction::CameraLeft]        = { SDLK_l };
    p.keyBindings[HKAction::CameraReset]       = { SDLK_SPACE };
    p.keyBindings[HKAction::CameraUndo]        = { SDLK_z, false, true, false }; // Alt+Z
    p.keyBindings[HKAction::CameraRedo]        = { SDLK_z, false, true, true };  // Alt+Shift+Z

    p.keyBindings[HKAction::ApplyBookmark1]    = { SDLK_1, true, false, false }; // Ctrl+1
    p.keyBindings[HKAction::ApplyBookmark2]    = { SDLK_2, true, false, false };
    p.keyBindings[HKAction::ApplyBookmark3]    = { SDLK_3, true, false, false };
    p.keyBindings[HKAction::ApplyBookmark4]    = { SDLK_4, true, false, false };
    p.keyBindings[HKAction::ApplyBookmark5]    = { SDLK_5, true, false, false };
    p.keyBindings[HKAction::ApplyBookmark6]    = { SDLK_6, true, false, false };
    p.keyBindings[HKAction::ApplyBookmark7]    = { SDLK_7, true, false, false };
    p.keyBindings[HKAction::ApplyBookmark8]    = { SDLK_8, true, false, false };
    p.keyBindings[HKAction::ApplyBookmark9]    = { SDLK_9, true, false, false };

    p.keyBindings[HKAction::StrifeLeft]        = { SDLK_LEFT };
    p.keyBindings[HKAction::StrifeRight]       = { SDLK_RIGHT };
    p.keyBindings[HKAction::StrifeUp]          = { SDLK_UP };
    p.keyBindings[HKAction::StrifeDown]        = { SDLK_DOWN };

    p.keyBindings[HKAction::DuplicateSelection]= { SDLK_d, true, false, false }; // Ctrl+D
    p.keyBindings[HKAction::ClearScene]        = { SDLK_n, true, true, false };  // Ctrl+Alt+N
    p.keyBindings[HKAction::Undo]              = { SDLK_z, true, false, false }; // Ctrl+Z
    p.keyBindings[HKAction::Redo]              = { SDLK_y, true, false, false }; // Ctrl+Y
    p.keyBindings[HKAction::OpenFile]          = { SDLK_o, true, false, false }; // Ctrl+O
    p.keyBindings[HKAction::OpenRecentFile]    = { SDLK_o, true, false, true };  // Ctrl+Shift+O
    p.keyBindings[HKAction::SaveFile]          = { SDLK_s, true, false, false }; // Ctrl+S
    p.keyBindings[HKAction::SaveFileAs]        = { SDLK_s, true, false, true };  // Ctrl+Shift+S
    p.keyBindings[HKAction::ExportOBJ]         = { SDLK_e, true, false, false }; // Ctrl+E
    p.keyBindings[HKAction::ToggleDyntopo]     = { SDLK_t, true, false, false }; // Ctrl+T
    p.keyBindings[HKAction::RunRemesh]         = { SDLK_x, true, false, false }; // Ctrl+X

    p.keyBindings[HKAction::ToggleOutliner]    = { SDLK_F1 };
    p.keyBindings[HKAction::ToggleCameraBookmarks] = { SDLK_F2 };
    p.keyBindings[HKAction::SoloSelected]      = { SDLK_c };
}

HotkeyPreset HotkeyPreset::createZBrush() {
    HotkeyPreset p;
    p.name = "ZBrush";
    p.isBuiltin = true;
    populateCommonKeyBindings(p);

    // ZBrush Navigation
    // 2D Ref Mode
    p.mouseNavBindings.push_back({ SDL_BUTTON_RIGHT, false, false, false, true, true, NavAction::ResetView2D });
    p.mouseNavBindings.push_back({ SDL_BUTTON_RIGHT, true, false, false, false, true, NavAction::Zoom2D });
    p.mouseNavBindings.push_back({ SDL_BUTTON_RIGHT, false, true, false, false, true, NavAction::Pan2D });

    // 3D Mode
    p.mouseNavBindings.push_back({ SDL_BUTTON_MIDDLE, false, false, false, true, false, NavAction::ResetView });
    p.mouseNavBindings.push_back({ SDL_BUTTON_MIDDLE, false, false, true, false, false, NavAction::RotateLight });
    p.mouseNavBindings.push_back({ SDL_BUTTON_MIDDLE, false, false, false, false, false, NavAction::Pan });

    p.mouseNavBindings.push_back({ SDL_BUTTON_RIGHT, true, false, false, false, false, NavAction::Zoom });
    p.mouseNavBindings.push_back({ SDL_BUTTON_RIGHT, false, true, true, false, false, NavAction::Roll });
    p.mouseNavBindings.push_back({ SDL_BUTTON_RIGHT, false, true, false, false, false, NavAction::Pan });
    p.mouseNavBindings.push_back({ SDL_BUTTON_RIGHT, false, false, false, false, false, NavAction::Orbit });

    p.mouseNavBindings.push_back({ SDL_BUTTON_LEFT, false, true, false, false, false, NavAction::SubToolSelect });

    // Modifiers
    p.modifierBehavior.shiftHoldAction = HKAction::ToolSmooth;
    p.modifierBehavior.ctrlHoldAction = HKAction::ToolMask;
    p.modifierBehavior.ctrlShiftHoldAction = HKAction::ToolVisibility;
    p.modifierBehavior.altInvertsBrush = true;
    p.modifierBehavior.ctrlAltInvertsBrush = false;
    p.modifierBehavior.ctrlAltSubToolSelect = false;

    return p;
}

HotkeyPreset HotkeyPreset::createMaya() {
    HotkeyPreset p;
    p.name = "Maya";
    p.isBuiltin = true;
    populateCommonKeyBindings(p);

    // Maya Navigation
    // 2D Ref Mode
    p.mouseNavBindings.push_back({ SDL_BUTTON_MIDDLE, false, false, false, true, true, NavAction::ResetView2D });
    p.mouseNavBindings.push_back({ SDL_BUTTON_MIDDLE, false, true, false, false, true, NavAction::Pan2D });
    p.mouseNavBindings.push_back({ SDL_BUTTON_RIGHT, false, true, false, false, true, NavAction::Zoom2D });

    // 3D Mode
    p.mouseNavBindings.push_back({ SDL_BUTTON_MIDDLE, false, false, false, true, false, NavAction::ResetView });
    p.mouseNavBindings.push_back({ SDL_BUTTON_MIDDLE, true, false, true, false, false, NavAction::RotateLight });

    p.mouseNavBindings.push_back({ SDL_BUTTON_LEFT, false, true, false, false, false, NavAction::Orbit });
    p.mouseNavBindings.push_back({ SDL_BUTTON_MIDDLE, false, true, false, false, false, NavAction::Pan });
    p.mouseNavBindings.push_back({ SDL_BUTTON_RIGHT, false, true, false, false, false, NavAction::Zoom });
    p.mouseNavBindings.push_back({ SDL_BUTTON_MIDDLE, false, false, false, false, false, NavAction::Pan });

    p.mouseNavBindings.push_back({ SDL_BUTTON_LEFT, true, true, false, false, false, NavAction::SubToolSelect });

    // Modifiers
    p.modifierBehavior.shiftHoldAction = HKAction::ToolSmooth;
    p.modifierBehavior.ctrlHoldAction = HKAction::ToolMask;
    p.modifierBehavior.ctrlShiftHoldAction = HKAction::ToolVisibility;
    p.modifierBehavior.altInvertsBrush = false;
    p.modifierBehavior.ctrlAltInvertsBrush = true;
    p.modifierBehavior.ctrlAltSubToolSelect = true;

    return p;
}

HotkeyPreset HotkeyPreset::createBlender() {
    HotkeyPreset p;
    p.name = "Blender";
    p.isBuiltin = true;
    populateCommonKeyBindings(p);

    // Blender Navigation
    // 2D Ref Mode
    p.mouseNavBindings.push_back({ SDL_BUTTON_MIDDLE, false, false, false, true, true, NavAction::ResetView2D });
    p.mouseNavBindings.push_back({ SDL_BUTTON_MIDDLE, false, false, true, false, true, NavAction::Pan2D });
    p.mouseNavBindings.push_back({ SDL_BUTTON_MIDDLE, true, false, false, false, true, NavAction::Zoom2D });

    // 3D Mode
    p.mouseNavBindings.push_back({ SDL_BUTTON_MIDDLE, false, false, false, true, false, NavAction::ResetView });
    p.mouseNavBindings.push_back({ SDL_BUTTON_MIDDLE, true, false, true, false, false, NavAction::RotateLight });
    p.mouseNavBindings.push_back({ SDL_BUTTON_MIDDLE, false, false, true, false, false, NavAction::Pan });
    p.mouseNavBindings.push_back({ SDL_BUTTON_MIDDLE, true, false, false, false, false, NavAction::Zoom });
    p.mouseNavBindings.push_back({ SDL_BUTTON_MIDDLE, false, true, false, false, false, NavAction::Roll });
    p.mouseNavBindings.push_back({ SDL_BUTTON_MIDDLE, false, false, false, false, false, NavAction::Orbit });

    p.mouseNavBindings.push_back({ SDL_BUTTON_LEFT, false, true, false, false, false, NavAction::SubToolSelect });

    // Modifiers
    p.modifierBehavior.shiftHoldAction = HKAction::ToolSmooth;
    p.modifierBehavior.ctrlHoldAction = HKAction::ToolMask;
    p.modifierBehavior.ctrlShiftHoldAction = HKAction::ToolVisibility;
    p.modifierBehavior.altInvertsBrush = true;
    p.modifierBehavior.ctrlAltInvertsBrush = false;
    p.modifierBehavior.ctrlAltSubToolSelect = false;

    return p;
}
