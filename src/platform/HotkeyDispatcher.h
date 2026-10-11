#pragma once
#include <SDL2/SDL.h>
#include "editing/SculptManager.h"
#include "scene/Scene.h"
#include "gui/GuiManager.h"

#include "platform/HotkeyPreset.h"

class HotkeyManager;


class AngleRenderer;

class HotkeyDispatcher {
public:
    HotkeyDispatcher();
    ~HotkeyDispatcher() = default;

    // Process SDL event. Returns true if the event was fully handled/consumed by the dispatcher.
    bool processEvent(const SDL_Event& event, SculptManager& sculpt, Scene& scene, GuiManager& gui, AngleRenderer* renderer = nullptr);

    // Update the modifier key stack (Shift/Ctrl/Alt)
    void updateModifiers(const SDL_Event& event, SculptManager& sculpt, Scene& scene, AngleRenderer* renderer = nullptr);

    // Reset all modifier states and restore the original brush if needed
    void resetModifiers(SculptManager& sculpt, AngleRenderer* renderer = nullptr);

    ModalMode getModalMode() const { return m_modalMode; }

    void setHotkeyManager(HotkeyManager* manager) { m_manager = manager; }
    HotkeyManager* getHotkeyManager() const { return m_manager; }

private:
    HKAction mapKeyToAction(const SDL_Keysym& keysym, bool ctrlPressed, bool altPressed, bool shiftPressed);
    bool executeAction(HKAction action, bool isDown, SculptManager& sculpt, Scene& scene, GuiManager& gui);

    HotkeyManager* m_manager = nullptr;
    ModalMode m_modalMode = ModalMode::NONE;
    BrushType m_prevBrush = BRUSH_FLATTEN;
    bool m_shiftActive = false;
    bool m_ctrlActive = false;
    bool m_altActive = false;
    bool m_polyGroupsTemporarilyEnabled = false;
    bool m_prevPolyGroupsState = false;
};
