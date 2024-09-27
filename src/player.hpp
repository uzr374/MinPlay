#ifndef PLAYER_HPP
#define PLAYER_HPP

#include <string>

bool open_url(std::string url);
void stop_playback();
void pause();
void resume();
void postquit_cleanup();

#endif // PLAYER_HPP
