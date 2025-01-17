#include "sdlkeymap.hpp"

struct SDLKeyMapping final{
    SDL_Scancode sdl_key = SDL_SCANCODE_UNKNOWN;
    Qt::Key qt_key = Qt::Key_unknown;
};

static constexpr SDLKeyMapping sdl_to_qt_key_map[] = {
    {SDL_SCANCODE_SPACE,    Qt::Key_Space},
    {SDL_SCANCODE_UP,       Qt::Key_Up},
    {SDL_SCANCODE_DOWN,     Qt::Key_Down},
    {SDL_SCANCODE_LEFT,     Qt::Key_Left},
    {SDL_SCANCODE_RIGHT,    Qt::Key_Right},
    {SDL_SCANCODE_PAGEUP,   Qt::Key_PageUp},
    {SDL_SCANCODE_PAGEDOWN, Qt::Key_PageDown},
    {SDL_SCANCODE_M,        Qt::Key_M},
    {SDL_SCANCODE_W,        Qt::Key_W},
    {SDL_SCANCODE_A,        Qt::Key_A},
    {SDL_SCANCODE_V,        Qt::Key_V},
    {SDL_SCANCODE_S,        Qt::Key_S},
    {SDL_SCANCODE_F,        Qt::Key_F}
};

Qt::Key toQtKey(SDL_Scancode code) {
    for (const auto& mapping : sdl_to_qt_key_map) {
        if (mapping.sdl_key == code) {
            return mapping.qt_key;
        }
    }

    return Qt::Key_unknown;
}

