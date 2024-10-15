#include "sdlrenderer.hpp"
#include "cavframe.h"
#include "sdlkeymap.hpp"

#include <QWindow>
#include <QKeyEvent>
#include <QApplication>
#include <QTimer>
#include <stdexcept>

static constexpr struct TextureFormatEntry {
    AVPixelFormat format;
    SDL_PixelFormat texture_fmt;
} sdl_texture_format_map[] = {
    { AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
    { AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_XRGB4444 },
    { AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_XRGB1555 },
    { AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_XBGR1555 },
    { AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
    { AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
    { AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
    { AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
    { AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_XRGB8888 },
    { AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_XBGR8888 },
    { AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
    { AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
    { AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
    { AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
    { AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
    { AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
    { AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
    { AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
    { AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
    { AV_PIX_FMT_NV12,           SDL_PIXELFORMAT_NV12 },
    { AV_PIX_FMT_NV21,           SDL_PIXELFORMAT_NV21 },
    { AV_PIX_FMT_P010,           SDL_PIXELFORMAT_P010 }
};

static bool realloc_texture(SDL_Renderer* renderer, SDL_Texture **texture, SDL_PixelFormat new_format,
                           int new_width, int new_height, SDL_BlendMode blendmode, bool init_texture, const AVFrame* frame)
{
    SDL_PixelFormat format;
    SDL_Colorspace colorspace;
    int access, w, h;

    auto must_realloc_texture = [&](SDL_Texture* tex){
        if(!tex) return true;
        const auto props = SDL_GetTextureProperties(tex);
        w = SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_WIDTH_NUMBER, 0);
        h = SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_HEIGHT_NUMBER, 0);
        access = SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_ACCESS_NUMBER, -1);
        auto integer_format = SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_FORMAT_NUMBER, SDL_PIXELFORMAT_UNKNOWN);
        auto integer_colorspace = SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_COLORSPACE_NUMBER, SDL_COLORSPACE_UNKNOWN);
        format = static_cast<SDL_PixelFormat>(integer_format);
        colorspace = static_cast<SDL_Colorspace>(integer_colorspace);
        return new_width != w || new_height != h || new_format != format;
    };

    if (must_realloc_texture(*texture)) {
        void *pixels;
        int pitch;
        if (*texture)
            SDL_DestroyTexture(*texture);
        //const auto texprops = SDL_CreateProperties();
        if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
            return false;
        if (!SDL_SetTextureBlendMode(*texture, blendmode))
            return false;
        if (init_texture) {
            if (!SDL_LockTexture(*texture, NULL, &pixels, &pitch))
                return false;
            std::memset(pixels, 0, pitch * new_height);
            SDL_UnlockTexture(*texture);
        }
    }
    return true;
}

static void calculate_display_rect(SDL_FRect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar)
{
    AVRational aspect_ratio = pic_sar;
    int64_t width, height, x, y;

    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
        aspect_ratio = av_make_q(1, 1);

    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    height = scr_height;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
    }
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    rect->x = scr_xleft + x;
    rect->y = scr_ytop  + y;
    rect->w = FFMAX((int)width,  1);
    rect->h = FFMAX((int)height, 1);
}

static void get_sdl_pix_fmt_and_blendmode(int format, SDL_PixelFormat *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode)
{
    *sdl_blendmode = SDL_BLENDMODE_NONE;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
    if (format == AV_PIX_FMT_RGB32   ||
        format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32   ||
        format == AV_PIX_FMT_BGR32_1)
        *sdl_blendmode = SDL_BLENDMODE_BLEND;
    for (int i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
        if (format == sdl_texture_format_map[i].format) {
            *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
            return;
        }
    }
}

static uintptr_t getSDLWindowHandle(SDL_Window* sdl_wnd){
    uintptr_t retrieved_handle = 0U;

#if defined(SDL_PLATFORM_WIN32)
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (hwnd) {
        retrieved_handle = hwnd;
    }
#elif defined(SDL_PLATFORM_MACOS)
    NSWindow *nswindow = (__bridge NSWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    if (nswindow) {
        retrieved_handle = nswindow;
    }
#elif defined(SDL_PLATFORM_LINUX)
    if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0) {
        auto xwindow = SDL_GetNumberProperty(SDL_GetWindowProperties(sdl_wnd), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        retrieved_handle = (uintptr_t)xwindow;
    } else if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        struct wl_display *display = (struct wl_display *)SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_wnd), SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
        struct wl_surface *surface = (struct wl_surface *)SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_wnd), SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
        if (display && surface) {
            retrieved_handle = (uintptr_t)surface;
        }
    }
#elif defined(SDL_PLATFORM_IOS)
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    UIWindow *uiwindow = (__bridge UIWindow *)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
    if (uiwindow) {
        retrieved_handle = uiwindow;
    }
#endif

    return retrieved_handle;
}

SDLRenderer::SDLRenderer(QObject* parent) : QObject(parent), event_timer(this) {
    auto cleanup = [this]{
        if(renderer)
            SDL_DestroyRenderer(renderer);
        if(wnd)
            SDL_DestroyWindow(wnd);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    };

    if(!SDL_InitSubSystem(SDL_INIT_VIDEO)){
        cleanup();
        throw std::runtime_error(SDL_GetError());
    }
    if(!SDL_CreateWindowAndRenderer("SDLRenderer", 1, 1, SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS, &wnd, &renderer)){
        cleanup();
        throw std::runtime_error(SDL_GetError());
    }
    const auto props = SDL_GetRendererProperties(renderer);
    auto tex_fmts = (const SDL_PixelFormat*)SDL_GetPointerProperty(props, SDL_PROP_RENDERER_TEXTURE_FORMATS_POINTER, nullptr);
    if(!tex_fmts || tex_fmts[0] == SDL_PIXELFORMAT_UNKNOWN){
        cleanup();
        throw std::runtime_error("Failed to create SDL renderer!");
    } else{
        auto sdl_to_av_pixfmt = [](SDL_PixelFormat sdl_fmt){
            for(int i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; ++i){
                auto [av_pfmt, sdl_pfmt] = sdl_texture_format_map[i];
                if(sdl_fmt == sdl_pfmt) return av_pfmt;
            }
            return AV_PIX_FMT_NONE;
        };
        auto tex_fmt = tex_fmts[0];
        int i = 0;
        while(tex_fmt != SDL_PIXELFORMAT_UNKNOWN){
            supported_sdl_pix_fmts.push_back(tex_fmt);
            supported_avpix_fmts.push_back(sdl_to_av_pixfmt(tex_fmt));
            ++i;
            tex_fmt = tex_fmts[i];
        }
    }
    SDL_ShowWindow(wnd);
    SDL_SyncWindow(wnd);

    UPDATE_EVT_ID = QEvent::registerEventType();

    event_timer.setInterval(30);
    event_timer.setTimerType(Qt::CoarseTimer);
    connect(&event_timer, &QTimer::timeout, this, &SDLRenderer::handleSDLEvents);
    event_timer.start();
}

SDLRenderer::~SDLRenderer(){
    if(sub_texture)
        SDL_DestroyTexture(sub_texture);
    if(vid_texture)
        SDL_DestroyTexture(vid_texture);
    if(renderer)
        SDL_DestroyRenderer(renderer);
    if(wnd)
        SDL_DestroyWindow(wnd);
    if(SDL_WasInit(SDL_INIT_VIDEO))
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

QWidget* SDLRenderer::toWidget(QWidget* parent){
    const auto win_id = getSDLWindowHandle(wnd);
    auto qwnd = QWindow::fromWinId(win_id);
    return QWidget::createWindowContainer(qwnd, parent);
}

void SDLRenderer::finishRendering(){
    if(wnd) {
        SDL_HideWindow(wnd);
        SDL_SyncWindow(wnd);
    }
}

void SDLRenderer::handleSDLEvents() {
    constexpr auto SDL_EVT_COUNT = 3;
    std::array<SDL_Event, SDL_EVT_COUNT> evts{};
    SDL_PumpEvents();
    const int evts_received = SDL_PeepEvents(evts.data(), SDL_EVT_COUNT, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST);
    if(evts_received > 0) {
        for(int i = 0; i < evts_received; ++i){
            const auto& evt = evts[i];
            processSDLEvent(evt);
        }
    }
}

void SDLRenderer::processSDLEvent(const SDL_Event& evt){
    switch(evt.type){
    case SDL_EVENT_KEY_DOWN:
    {
        const auto qtKey = toQtKey(evt.key.key);
        if(qtKey != Qt::Key_unknown){
            QApplication::postEvent(QApplication::instance(), new QKeyEvent(QEvent::KeyPress, qtKey, Qt::NoModifier));
        }
    }
        break;
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        SDL_ShowCursor();
        QTimer::singleShot(900, []{SDL_HideCursor();});
    }
        break;
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_RESIZED:
        if(evt.type == SDL_EVENT_WINDOW_RESIZED){
            window_width = evt.window.data1;
            window_height = evt.window.data2;
        }
        //qDebug() << "Renderer has been resized to " << window_width << "x"<<window_height;
        refreshDisplay();
        break;
    }
}

void SDLRenderer::requestUpdate(){
    QCoreApplication::postEvent(this, new QEvent(QEvent::Type(UPDATE_EVT_ID)));
}

bool SDLRenderer::event(QEvent* evt){
    if(evt->type() == UPDATE_EVT_ID){
        QMetaObject::invokeMethod(this, &SDLRenderer::refreshDisplay, Qt::QueuedConnection);
        //QTimer::singleShot(0, this, [this]{refreshDisplay();});
        return true;
    }

    return QObject::event(evt);
}

void SDLRenderer::refreshDisplay(){
    auto& img = video_buf.getReadable();
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    if(!img.videoFrameValid()){ SDL_RenderPresent(renderer); return; }

    if(!img.isUploaded()){
        if(last_frame_width != img.width() || last_frame_height != img.height() || last_frame_format != img.pixFmt()){
            SDL_PixelFormat sdl_pix_fmt;
            SDL_BlendMode sdl_blendmode;
            get_sdl_pix_fmt_and_blendmode(img.pixFmt(), &sdl_pix_fmt, &sdl_blendmode);
            if(!realloc_texture(renderer, &vid_texture, sdl_pix_fmt,
                                 img.width(), img.height(), sdl_blendmode, false, img.constAv())) return;
        }

        const AVFrame* frame = img.constAv();
        bool ret = false;
        switch (img.pixFmt()) {
        case AV_PIX_FMT_YUV420P:
            if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
                ret = SDL_UpdateYUVTexture(vid_texture, NULL, frame->data[0], frame->linesize[0],
                                           frame->data[1], frame->linesize[1],
                                           frame->data[2], frame->linesize[2]);
            } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
                ret = SDL_UpdateYUVTexture(vid_texture, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0],
                                           frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
                                           frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
            } else {
                av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
                ret = false;
            }
            break;
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV21:
            ret = SDL_UpdateNVTexture(vid_texture, nullptr, frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1]);
                break;
        default:
            if (frame->linesize[0] < 0) {
                ret = SDL_UpdateTexture(vid_texture, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
            } else {
                ret = SDL_UpdateTexture(vid_texture, NULL, frame->data[0], frame->linesize[0]);
            }
            break;
        }
        img.setUploaded(true);
    }

    SDL_FRect rect{};
    calculate_display_rect(&rect, 0, 0, window_width, window_height, img.width(), img.height(), img.sampleAR());
    //set_sdl_yuv_conversion_mode(vp->frame);

    SDL_RenderTextureRotated(renderer, vid_texture, NULL, &rect, 0, NULL, (img.linesize(0) < 0) ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
    //set_sdl_yuv_conversion_mode(NULL);
//     if (sp) {
// #if 1
//         SDL_RenderTexture(renderer, sub_texture, NULL, &rect);
// #else
//         int i;
//         double xratio = (double)rect.w / (double)sp->width;
//         double yratio = (double)rect.h / (double)sp->height;
//         for (i = 0; i < sp->sub.num_rects; i++) {
//             SDL_Rect *sub_rect = (SDL_Rect*)sp->sub.rects[i];
//             SDL_Rect target = {.x = rect.x + sub_rect->x * xratio,
//                                .y = rect.y + sub_rect->y * yratio,
//                                .w = sub_rect->w * xratio,
//                                .h = sub_rect->h * yratio};
//             SDL_RenderCopy(renderer, is->sub_texture, sub_rect, &target);
//         }
// #endif

    SDL_RenderPresent(renderer);
}

void SDLRenderer::clearDisplay(){
    video_buf.clear();
    requestUpdate();
}

bool SDLRenderer::pushFrame(CAVFrame&& frame){
    const bool res = video_buf.pushImage(std::move(frame));
    requestUpdate();
    return res;
}

std::vector<AVPixelFormat> SDLRenderer::supportedFormats() const{
    return supported_avpix_fmts;
}
