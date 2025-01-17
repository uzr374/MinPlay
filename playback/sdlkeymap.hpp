#ifndef SDLKEYMAP_HPP
#define SDLKEYMAP_HPP

#include <QKeyEvent>
#include <SDL3/SDL_scancode.h>

Qt::Key toQtKey(SDL_Scancode sdl_key);

#endif // SDLKEYMAP_HPP
