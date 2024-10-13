#ifndef SDLKEYMAP_HPP
#define SDLKEYMAP_HPP

#include <QKeyEvent>
#include <SDL3/SDL_keycode.h>

Qt::Key toQtKey(SDL_Keycode sdl_key);

#endif // SDLKEYMAP_HPP
