#ifndef SDLRENDERER_HPP
#define SDLRENDERER_HPP

#include <SDL3/SDL.h>

#include <QtGlobal>
#include <QWidget>
#include <QTimer>
#include <vector>
#include <mutex>

#include "cavframe.h"

template<typename Image>
class ImageDoubleBuffer final{
private:
    std::mutex buf_mtx;
    std::array<Image, 2> buf;
    int buf_read_idx = 0;
    bool image_pending = false;

public:
    Image& getReadable(){
        std::scoped_lock lck(buf_mtx);
        if(image_pending){
            buf_read_idx ^= 1;
            image_pending = false;
        }
        return buf[buf_read_idx];
    }

    /*returns whether the last image has been read*/
    bool pushImage(Image&& img){
        std::scoped_lock lck(buf_mtx);
        const bool was_read = !image_pending;
        image_pending = true;
        buf[buf_read_idx ^ 1] = std::move(img);
        return was_read;
    }

    void clear(){
        std::scoped_lock lck(buf_mtx);
        buf_read_idx = 0;
        image_pending = false;
        for(auto& img: buf) img.unref();
    }
};

class SDLRenderer final : public QObject
{
    Q_OBJECT;
    Q_DISABLE_COPY_MOVE(SDLRenderer);

    SDL_Window* wnd = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* sub_texture = nullptr, *vid_texture = nullptr;
    std::vector<SDL_PixelFormat> supported_sdl_pix_fmts;
    std::vector<AVPixelFormat> supported_avpix_fmts;
    QTimer event_timer;
    int window_width = 0, window_height = 0;
    int last_frame_width = 0, last_frame_height = 0;
    AVPixelFormat last_frame_format = AV_PIX_FMT_NONE;

    ImageDoubleBuffer<CAVFrame> video_buf;

    uintptr_t getWindowHandle();
    Q_SLOT void handleSDLEvents();
    void processSDLEvent(const SDL_Event& evt);
    bool event(QEvent* evt) override;
    Q_INVOKABLE void refreshDisplay();

    int UPDATE_EVT_ID = -1;

public:
    SDLRenderer(QObject* parent = nullptr);
    ~SDLRenderer();

    QWidget* toWidget(QWidget* parent);
    void finishRendering();

    /*The following functions are thread-safe*/
    std::vector<AVPixelFormat> supportedFormats() const;
    void requestUpdate();
    bool pushFrame(CAVFrame&&);
    void clearDisplay();
};

 std::vector<AVPixelFormat> SDL_supportedFormats();

#endif // SDLRENDERER_HPP
