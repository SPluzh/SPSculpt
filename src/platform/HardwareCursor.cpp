#include "platform/HardwareCursor.h"
#include <imgui.h>
#include <cmath>
#include <algorithm>

HardwareCursorManager::HardwareCursorManager() = default;

HardwareCursorManager::~HardwareCursorManager() {
    shutdown();
}

SDL_Cursor* HardwareCursorManager::createCrosshairCursor(Uint8 r, Uint8 g, Uint8 b, float scale) {
    const int w = 48;
    const int h = 48;
    const float cx = 24.0f;
    const float cy = 24.0f;

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return nullptr;

    SDL_LockSurface(surface);
    Uint32* pixels = static_cast<Uint32*>(surface->pixels);
    int pitch32 = surface->pitch / 4;

    const float s = std::max(1.0f, scale);
    const float gap = 3.5f * s;
    const float armEnd = 10.0f * s;
    const float halfThick = 0.75f * s;
    const float fringe = 0.75f;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float dx = std::abs(static_cast<float>(x) - cx);
            float dy = std::abs(static_cast<float>(y) - cy);

            // Horizontal segment distance
            float distEndH = 0.0f;
            if (dx < gap) distEndH = gap - dx;
            else if (dx > armEnd) distEndH = dx - armEnd;
            float distH = std::hypot(distEndH, dy);

            // Vertical segment distance
            float distEndV = 0.0f;
            if (dy < gap) distEndV = gap - dy;
            else if (dy > armEnd) distEndV = dy - armEnd;
            float distV = std::hypot(distEndV, dx);

            float d = std::min(distH, distV);

            float alpha = 0.0f;
            if (d <= halfThick) {
                alpha = 1.0f;
            } else if (d <= halfThick + fringe) {
                alpha = 1.0f - (d - halfThick) / fringe;
            }

            if (alpha > 0.05f) {
                Uint8 a = static_cast<Uint8>(std::clamp(alpha * 255.0f, 0.0f, 255.0f));
                // Premultiply RGB for Windows ARGB cursor transparency
                Uint8 pr = static_cast<Uint8>((static_cast<int>(r) * a) / 255);
                Uint8 pg = static_cast<Uint8>((static_cast<int>(g) * a) / 255);
                Uint8 pb = static_cast<Uint8>((static_cast<int>(b) * a) / 255);
                pixels[y * pitch32 + x] = SDL_MapRGBA(surface->format, pr, pg, pb, a);
            } else {
                pixels[y * pitch32 + x] = 0;
            }
        }
    }
    SDL_UnlockSurface(surface);

    SDL_Cursor* cursor = SDL_CreateColorCursor(surface, 24, 24);
    SDL_FreeSurface(surface);
    return cursor;
}

SDL_Cursor* HardwareCursorManager::createDotCursor(Uint8 r, Uint8 g, Uint8 b, float scale) {
    const int w = 48;
    const int h = 48;
    const float cx = 24.0f;
    const float cy = 24.0f;

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return nullptr;

    SDL_LockSurface(surface);
    Uint32* pixels = static_cast<Uint32*>(surface->pixels);
    int pitch32 = surface->pitch / 4;

    const float s = std::max(1.0f, scale);
    const float dotRadius = 3.5f * s;
    const float fringe = 0.8f;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float dist = std::hypot(static_cast<float>(x) - cx, static_cast<float>(y) - cy);

            float alpha = 0.0f;
            if (dist <= dotRadius - fringe * 0.5f) {
                alpha = 1.0f;
            } else if (dist <= dotRadius + fringe * 0.5f) {
                alpha = (dotRadius + fringe * 0.5f - dist) / fringe;
            }

            if (alpha > 0.05f) {
                Uint8 a = static_cast<Uint8>(std::clamp(alpha * 255.0f, 0.0f, 255.0f));
                Uint8 pr = static_cast<Uint8>((static_cast<int>(r) * a) / 255);
                Uint8 pg = static_cast<Uint8>((static_cast<int>(g) * a) / 255);
                Uint8 pb = static_cast<Uint8>((static_cast<int>(b) * a) / 255);
                pixels[y * pitch32 + x] = SDL_MapRGBA(surface->format, pr, pg, pb, a);
            } else {
                pixels[y * pitch32 + x] = 0;
            }
        }
    }
    SDL_UnlockSurface(surface);

    SDL_Cursor* cursor = SDL_CreateColorCursor(surface, 24, 24);
    SDL_FreeSurface(surface);
    return cursor;
}

void HardwareCursorManager::rebuildCursors(float uiScale) {
    m_uiScale = std::max(1.0f, uiScale);

    // Free existing colored cursors
    for (int i = 0; i < static_cast<int>(CursorColorVariant::COUNT); ++i) {
        if (m_dotCursors[i]) {
            SDL_FreeCursor(m_dotCursors[i]);
            m_dotCursors[i] = nullptr;
        }
        if (m_crosshairCursors[i]) {
            SDL_FreeCursor(m_crosshairCursors[i]);
            m_crosshairCursors[i] = nullptr;
        }
    }

    struct ColorDef {
        Uint8 r, g, b;
    };
    ColorDef colors[static_cast<int>(CursorColorVariant::COUNT)] = {
        { 204, 102, 0 },   // ORANGE (default sculpt)
        { 204, 0, 0 },     // RED (hover mesh)
        { 0, 153, 255 },   // BLUE (smooth / shift)
        { 255, 255, 0 },   // YELLOW (mask / ctrl)
        { 0, 230, 217 }    // CYAN (relax)
    };

    for (int i = 0; i < static_cast<int>(CursorColorVariant::COUNT); ++i) {
        m_crosshairCursors[i] = createCrosshairCursor(colors[i].r, colors[i].g, colors[i].b, m_uiScale);
        m_dotCursors[i] = createDotCursor(colors[i].r, colors[i].g, colors[i].b, m_uiScale);
    }
}

void HardwareCursorManager::init(float uiScale) {
    if (!m_defaultCursor) {
        m_defaultCursor = SDL_GetDefaultCursor();
    }
    rebuildCursors(uiScale);

    if (!m_sysCrosshairCursor) {
        m_sysCrosshairCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
    }
}

void HardwareCursorManager::updateScale(float uiScale) {
    if (std::abs(m_uiScale - uiScale) > 0.05f) {
        rebuildCursors(uiScale);
    }
}

void HardwareCursorManager::shutdown() {
    for (int i = 0; i < static_cast<int>(CursorColorVariant::COUNT); ++i) {
        if (m_dotCursors[i]) {
            SDL_FreeCursor(m_dotCursors[i]);
            m_dotCursors[i] = nullptr;
        }
        if (m_crosshairCursors[i]) {
            SDL_FreeCursor(m_crosshairCursors[i]);
            m_crosshairCursors[i] = nullptr;
        }
    }
    if (m_sysCrosshairCursor) {
        SDL_FreeCursor(m_sysCrosshairCursor);
        m_sysCrosshairCursor = nullptr;
    }
}

void HardwareCursorManager::setMode(HardwareCursorMode mode) {
    m_mode = mode;
}

CursorColorVariant HardwareCursorManager::resolveColorVariant(const glm::vec3& color) const {
    if (color.b > 0.8f && color.r < 0.2f) return CursorColorVariant::BLUE;
    if (color.r > 0.8f && color.g > 0.8f) return CursorColorVariant::YELLOW;
    if (color.g > 0.8f && color.b > 0.7f) return CursorColorVariant::CYAN;
    if (color.r > 0.7f && color.g < 0.2f) return CursorColorVariant::RED;
    return CursorColorVariant::ORANGE;
}

SDL_Cursor* HardwareCursorManager::getActiveHardwareCursor(CursorColorVariant variant) const {
    int idx = static_cast<int>(variant);
    switch (m_mode) {
        case HardwareCursorMode::DOT:
            return m_dotCursors[idx];
        case HardwareCursorMode::CROSSHAIR:
            return m_crosshairCursors[idx];
        case HardwareCursorMode::SYSTEM_CROSSHAIR:
            return m_sysCrosshairCursor;
        default:
            return nullptr;
    }
}

void HardwareCursorManager::update(bool isSculpting, bool isOverMesh, bool isMouseOverUi, bool hasFocus, const glm::vec3& cursorColor) {
    ImGuiIO& io = ImGui::GetIO();

    if (!hasFocus || isMouseOverUi) {
        m_isHardwareActive = false;
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        SDL_ShowCursor(SDL_ENABLE);
        if (m_defaultCursor) {
            SDL_SetCursor(m_defaultCursor);
        }
        return;
    }

    // Legacy/default software behavior:
    if (m_mode == HardwareCursorMode::OFF) {
        m_isHardwareActive = false;
        if (isSculpting) {
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            SDL_ShowCursor(SDL_DISABLE);
        } else {
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
            SDL_ShowCursor(SDL_ENABLE);
        }
        return;
    }

    // Hardware cursor mode: activate ONLY when over the mesh or actively sculpting
    bool shouldShowHardware = (isSculpting || isOverMesh);
    if (shouldShowHardware) {
        CursorColorVariant variant = resolveColorVariant(cursorColor);
        SDL_Cursor* targetCursor = getActiveHardwareCursor(variant);
        if (targetCursor) {
            m_isHardwareActive = true;
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            SDL_ShowCursor(SDL_ENABLE);
            SDL_SetCursor(targetCursor);
            return;
        }
    }

    // In all other cases (e.g. hovering over empty viewport background):
    // Show standard OS arrow cursor!
    m_isHardwareActive = false;
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    SDL_ShowCursor(SDL_ENABLE);
    if (m_defaultCursor) {
        SDL_SetCursor(m_defaultCursor);
    }
}
