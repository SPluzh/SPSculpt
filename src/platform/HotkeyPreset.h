#pragma once
#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "common/IniFile.h"

// All hotkey actions in the application
enum class HKAction {
    // Tools
    ToolPaint = 0,
    ToolBrush,
    ToolInflate,
    ToolTwist,
    ToolVTool,
    ToolTransform,
    ToolSmooth,
    ToolRelax,
    ToolFlatten,
    ToolPinch,
    ToolCrease,
    ToolDrag,
    ToolMove,
    ToolClayBuildup,
    ToolDamStandard,
    ToolTopology,
    ToolMask,
    ToolVisibility,
    ToolPose,
    ToolSnakeHook,
    
    // Sculpting Parameters
    BrushIntensity,        // A (modal)
    BrushRadius,           // S (modal)
    BrushFocalShift,       // D (modal)
    BrushNegative,         // N (toggle)
    BrushPicker,           // I (toggle)
    ToggleSymmetry,        // Alt+X (toggle)
    RemeshResolution,      // X (modal)
    TopologyDetail,        // Z (modal)
    DeleteSelected,        // Del
    
    // Camera / Viewport
    CameraFov,             // G (modal)
    CameraProjection,      // P (toggle Perspective/Ortho)
    CameraFrame,           // F (focus on mesh)
    CameraLeft,            // L
    CameraReset,           // Space
    CameraUndo,            // Alt+Z
    CameraRedo,            // Alt+Shift+Z
    
    // Camera Bookmarks
    ApplyBookmark1,
    ApplyBookmark2,
    ApplyBookmark3,
    ApplyBookmark4,
    ApplyBookmark5,
    ApplyBookmark6,
    ApplyBookmark7,
    ApplyBookmark8,
    ApplyBookmark9,
    
    // Strife Camera (Arrow keys)
    StrifeLeft,            // Left
    StrifeRight,           // Right
    StrifeUp,              // Up
    StrifeDown,            // Down
    
    // Scene / Files
    DuplicateSelection,    // Ctrl+D
    ClearScene,            // Ctrl+Alt+N
    Undo,                  // Ctrl+Z
    Redo,                  // Ctrl+Y or Ctrl+Shift+Z
    OpenFile,              // Ctrl+O
    OpenRecentFile,        // Ctrl+Shift+O
    SaveFile,              // Ctrl+S
    SaveFileAs,            // Ctrl+Shift+S
    ExportOBJ,             // Ctrl+E
    ToggleDyntopo,         // Ctrl+T
    RunRemesh,             // Ctrl+X
    
    // Misc
    ToggleOutliner,        // F1
    ToggleCameraBookmarks, // F2
    OpenContextPopup,
    SoloSelected,          // C
    ToggleRefImagesVisible,// Shift+Z
    ToggleRefEditMode,     // Z
    None
};

// Information helper for HKAction
struct HKActionInfo {
    HKAction action;
    const char* id;          // INI serialization ID (e.g. "ToolBrush")
    const char* displayName; // Human readable name (e.g. "Standard Brush")
    const char* shortName;   // 3-4 chars for virtual keyboard (e.g. "BRU")
    const char* category;    // Category: "Tools", "Parameters", "Camera", "Bookmarks", "Files", "Misc"
    bool isModal = false;    // Whether this action is a hold/drag modal
};

const std::vector<HKActionInfo>& getAllHKActions();
const HKActionInfo* getHKActionInfo(HKAction action);
HKAction stringToHKAction(const std::string& str);
std::string hkActionToString(HKAction action);

// Keyboard binding description
struct KeyBinding {
    SDL_Keycode key = SDLK_UNKNOWN;
    bool ctrl = false;
    bool alt = false;
    bool shift = false;

    bool isValid() const { return key != SDLK_UNKNOWN; }
    bool matches(SDL_Keycode k, bool c, bool a, bool s) const {
        return isValid() && key == k && ctrl == c && alt == a && shift == s;
    }

    std::string toString() const;
    static KeyBinding fromString(const std::string& str);

    bool operator==(const KeyBinding& other) const {
        return key == other.key && ctrl == other.ctrl && alt == other.alt && shift == other.shift;
    }
    bool operator!=(const KeyBinding& other) const {
        return !(*this == other);
    }
};

// Mouse Navigation Action
enum class NavAction {
    None = 0,
    Orbit,
    Pan,
    Zoom,
    Roll,
    RotateLight,
    Pan2D,
    Zoom2D,
    ResetView2D,
    ResetView,
    SubToolSelect,
    InvertSculpt
};

const char* navActionToString(NavAction action);
NavAction stringToNavAction(const std::string& str);
const char* navActionGetDisplayName(NavAction action);

// Mouse Navigation Binding
struct MouseNavBinding {
    uint8_t mouseButton = 0; // SDL_BUTTON_LEFT, SDL_BUTTON_MIDDLE, SDL_BUTTON_RIGHT
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool doubleClick = false;
    bool ref2D = false;
    NavAction action = NavAction::None;

    bool matches(uint8_t btn, bool c, bool a, bool s, bool dClick, bool isRef2D) const {
        if (mouseButton != btn) return false;
        if (ref2D != isRef2D) return false;
        if (doubleClick != dClick) return false;
        return ctrl == c && alt == a && shift == s;
    }

    std::string toString() const;
    static MouseNavBinding fromString(const std::string& str);
};

// Behavior of modifier keys (Shift/Ctrl/Alt)
struct ModifierBehavior {
    HKAction shiftHoldAction = HKAction::ToolSmooth;
    HKAction ctrlHoldAction = HKAction::ToolMask;
    HKAction ctrlShiftHoldAction = HKAction::ToolVisibility;
    bool altInvertsBrush = true;        // Alt alone inverts brush (ZBrush/Blender style)
    bool ctrlAltInvertsBrush = false;   // Ctrl+Alt inverts brush (Maya style)
    bool ctrlAltSubToolSelect = false;  // Ctrl+Alt+LMB selects subtool (Maya style)
};

// Full Hotkey Preset
struct HotkeyPreset {
    std::string name;
    bool isBuiltin = false;

    // Map HKAction -> primary key binding
    std::unordered_map<HKAction, KeyBinding> keyBindings;

    // List of mouse navigation bindings (order = check priority)
    std::vector<MouseNavBinding> mouseNavBindings;

    // Modifier key behaviors
    ModifierBehavior modifierBehavior;

    // Serialization
    void saveToIni(IniFile& ini) const;
    bool loadFromIni(const IniFile& ini);
    bool saveToFile(const std::string& filepath) const;
    bool loadFromFile(const std::string& filepath);

    // Factory methods for built-in presets
    static HotkeyPreset createZBrush();
    static HotkeyPreset createMaya();
    static HotkeyPreset createBlender();
};
