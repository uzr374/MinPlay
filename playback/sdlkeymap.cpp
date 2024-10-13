#include "sdlkeymap.hpp"

struct SDLKeyMapping final{
    SDL_Keycode sdl_key = SDLK_UNKNOWN;
    Qt::Key qt_key = Qt::Key_unknown;
};

static constexpr SDLKeyMapping sdl_to_qt_key_map[] = {
    {SDLK_SPACE,    Qt::Key_Space},
    {SDLK_UP,       Qt::Key_Up},
    {SDLK_DOWN,     Qt::Key_Down},
    {SDLK_LEFT,     Qt::Key_Left},
    {SDLK_RIGHT,    Qt::Key_Right},
    {SDLK_PAGEUP,   Qt::Key_PageUp},
    {SDLK_PAGEDOWN, Qt::Key_PageDown},
    {SDLK_M,        Qt::Key_M},
    {SDLK_W,        Qt::Key_W},
    {SDLK_A,        Qt::Key_A},
    {SDLK_V,        Qt::Key_V},
    {SDLK_S,        Qt::Key_S},
    {SDLK_F,        Qt::Key_F}
};

Qt::Key toQtKey(SDL_Keycode code) {
    for (const auto& mapping : sdl_to_qt_key_map) {
        if (mapping.sdl_key == code) {
            return mapping.qt_key;
        }
    }

    return Qt::Key_unknown;
}

