#ifndef SDLRENDERER_HPP
#define SDLRENDERER_HPP

#include <SDL3/SDL.h>

#include <QWidget>
#include <QTimer>
#include <vector>

#include "avframeview.hpp"


class SDLRenderer final : public QObject
{
    Q_OBJECT;
    Q_DISABLE_COPY_MOVE(SDLRenderer);

    SDL_Window* wnd = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* sub_texture = nullptr, *vid_texture = nullptr;
    std::vector<SDL_PixelFormat> supported_sdl_pix_fmts;
    std::vector<AVPixelFormat> supported_avpix_fmts;

    int window_width = 0, window_height = 0;
    int last_frame_width = 0, last_frame_height = 0;
    AVPixelFormat last_frame_format = AV_PIX_FMT_NONE;
    AVRational last_sar = {};
    bool flip_v = false;

    QTimer event_timer;

    uintptr_t getWindowHandle();
    Q_SLOT void handleSDLEvents();
    void processSDLEvent(const SDL_Event& evt);

public:
    SDLRenderer(QObject* parent = nullptr);
    ~SDLRenderer();

    QWidget* toWidget(QWidget* parent);

    std::vector<AVPixelFormat> supportedFormats() const;

    bool updateVideoTexture(AVFrameView frame);
    void refreshDisplay();
    void clearDisplay();
};

#endif // SDLRENDERER_HPP
