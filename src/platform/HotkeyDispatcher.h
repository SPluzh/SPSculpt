#pragma once
#include <SDL2/SDL.h>
#include "editing/SculptManager.h"
#include "scene/Scene.h"
#include "gui/GuiManager.h"

enum class HKAction {
    // Tools
    ToolPaint = 0,
    ToolBrush,
    ToolInflate,
    ToolTwist,
    ToolTransform,
    ToolSmooth,
    ToolFlatten,
    ToolPinch,
    ToolCrease,
    ToolDrag,
    ToolMove,
    ToolClayBuildup,
    ToolDamStandard,
    ToolTopology,
    
    // Sculpting Parameters
    BrushIntensity,        // A (modal)
    BrushRadius,           // S (modal)
    BrushFocalShift,       // D (modal)
    BrushNegative,         // N (toggle)
    BrushPicker,           // I (toggle)
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
    OpenFile,              // Ctrl+O / Ctrl+I
    ExportOBJ,             // Ctrl+E
    ToggleDyntopo,         // Ctrl+T
    RunRemesh,             // Ctrl+X
    
    // Misc
    OpenContextPopup,      // F1
    None
};


class HotkeyDispatcher {
public:
    HotkeyDispatcher();
    ~HotkeyDispatcher() = default;

    // Process SDL event. Returns true if the event was fully handled/consumed by the dispatcher.
    bool processEvent(const SDL_Event& event, SculptManager& sculpt, Scene& scene, GuiManager& gui);

    // Update the modifier key stack (Shift/Ctrl/Alt)
    void updateModifiers(const SDL_Event& event, SculptManager& sculpt);

private:
    HKAction mapKeyToAction(const SDL_Keysym& keysym, bool ctrlPressed, bool altPressed, bool shiftPressed);
    bool executeAction(HKAction action, bool isDown, SculptManager& sculpt, Scene& scene, GuiManager& gui);

    ModalMode m_modalMode = ModalMode::NONE;
    BrushType m_prevBrush = BRUSH_FLATTEN;
    bool m_shiftActive = false;
    bool m_ctrlActive = false;
};
