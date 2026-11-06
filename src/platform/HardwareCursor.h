#pragma once

#include <SDL.h>
#include <glm/glm.hpp>

enum class HardwareCursorMode : int {
    OFF = 0,             // Software only / default (hidden during stroke)
    DOT = 1,             // Hardware dot matching software styling (0 ms lag)
    CROSSHAIR = 2,       // Hardware open-center crosshair matching software styling (0 ms lag)
    SYSTEM_CROSSHAIR = 3 // OS system crosshair (0 ms lag)
};

enum class CursorColorVariant : int {
    ORANGE = 0, // default sculpt (0.8, 0.4, 0.0)
    RED = 1,    // hover mesh (0.8, 0.0, 0.0)
    BLUE = 2,   // smooth / shift (0.0, 0.6, 1.0)
    YELLOW = 3, // mask / ctrl (1.0, 1.0, 0.0)
    CYAN = 4,   // relax (0.0, 0.9, 0.85)
    COUNT = 5
};

class HardwareCursorManager {
public:
    HardwareCursorManager();
    ~HardwareCursorManager();

    void init(float uiScale = 1.0f);
    void updateScale(float uiScale);
    void shutdown();

    void setMode(HardwareCursorMode mode);
    HardwareCursorMode getMode() const { return m_mode; }

    bool isHardwareCursorActive() const { return m_isHardwareActive; }

    void update(bool isSculpting, bool isOverMesh, bool isMouseOverUi, bool hasFocus, const glm::vec3& cursorColor);

private:
    void rebuildCursors(float uiScale);
    SDL_Cursor* createDotCursor(Uint8 r, Uint8 g, Uint8 b, float scale);
    SDL_Cursor* createCrosshairCursor(Uint8 r, Uint8 g, Uint8 b, float scale);
    CursorColorVariant resolveColorVariant(const glm::vec3& color) const;
    SDL_Cursor* getActiveHardwareCursor(CursorColorVariant variant) const;

    HardwareCursorMode m_mode = HardwareCursorMode::OFF;
    bool m_isHardwareActive = false;
    float m_uiScale = 1.0f;
    CursorColorVariant m_currentColor = CursorColorVariant::ORANGE;

    SDL_Cursor* m_dotCursors[static_cast<int>(CursorColorVariant::COUNT)] = { nullptr };
    SDL_Cursor* m_crosshairCursors[static_cast<int>(CursorColorVariant::COUNT)] = { nullptr };
    SDL_Cursor* m_sysCrosshairCursor = nullptr;
    SDL_Cursor* m_defaultCursor = nullptr;
};
