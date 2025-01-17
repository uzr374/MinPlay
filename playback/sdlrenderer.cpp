#include "sdlrenderer.hpp"
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
                           int new_width, int new_height, SDL_BlendMode blendmode, AVFrameView frame)
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
    }
    return true;
}

static SDL_FRect calculate_display_rect(int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar)
{
    AVRational aspect_ratio = pic_sar;

    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
        aspect_ratio = av_make_q(1, 1);

    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    int64_t height = scr_height;
    int64_t width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
    }
    const auto x = (scr_width - width) / 2;
    const auto y = (scr_height - height) / 2;

    SDL_FRect rect{};
    rect.x = static_cast<float>(scr_xleft + x);
    rect.y = static_cast<float>(scr_ytop  + y);
    rect.w = static_cast<float>(std::max(width,  int64_t(1)));
    rect.h = static_cast<float>(std::max(height, int64_t(1)));

    return rect;
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
        const auto xwindow = SDL_GetNumberProperty(SDL_GetWindowProperties(sdl_wnd), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        retrieved_handle = (uintptr_t)xwindow;
    } else if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        struct wl_display *display = (struct wl_display *)SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_wnd), SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
        struct wl_surface *surface = (struct wl_surface *)SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_wnd), SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
        if (display && surface) {
            retrieved_handle = (uintptr_t)surface;
        }
    } else{
        qDebug() << "Failed to recognize the windowing system!";
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
    };

    if(!SDL_CreateWindowAndRenderer("SDLRenderer", 640, 480, SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS, &wnd, &renderer)){
        cleanup();
        throw std::runtime_error(SDL_GetError());
    }
    const auto props = SDL_GetRendererProperties(renderer);
    auto tex_fmts = (const SDL_PixelFormat*)SDL_GetPointerProperty(props, SDL_PROP_RENDERER_TEXTURE_FORMATS_POINTER, nullptr);
    if(!tex_fmts || tex_fmts[0] == SDL_PIXELFORMAT_UNKNOWN){
        cleanup();
        throw std::runtime_error("Failed to create a valid SDL renderer!");
    } else{
        auto sdl_to_av_pixfmt = [](SDL_PixelFormat sdl_fmt){
            for(const auto& [av_pfmt, sdl_pfmt] : sdl_texture_format_map){
                if(sdl_fmt == sdl_pfmt) return av_pfmt;
            }
            return AV_PIX_FMT_NONE;
        };
        auto tex_fmt = tex_fmts[0];
        int i = 0;
        while(tex_fmt != SDL_PIXELFORMAT_UNKNOWN){
            supported_sdl_pix_fmts.push_back(tex_fmt);
            const auto avpixfmt = sdl_to_av_pixfmt(tex_fmt);
            if(avpixfmt != AV_PIX_FMT_NONE)
                supported_avpix_fmts.push_back(avpixfmt);
            ++i;
            tex_fmt = tex_fmts[i];
        }
    }
    SDL_ShowWindow(wnd);
    SDL_SyncWindow(wnd);

    event_timer.setInterval(30);
    event_timer.setTimerType(Qt::CoarseTimer);
    connect(&event_timer, &QTimer::timeout, this, &SDLRenderer::handleSDLEvents);
    event_timer.start();
}

SDLRenderer::~SDLRenderer(){
    clearDisplay();
    if(renderer)
        SDL_DestroyRenderer(renderer);
    if(wnd)
        SDL_DestroyWindow(wnd);
}

QWidget* SDLRenderer::toWidget(QWidget* parent){
    const auto win_id = getSDLWindowHandle(wnd);
    const auto qwnd = QWindow::fromWinId(win_id);
    return QWidget::createWindowContainer(qwnd, parent);
}

void SDLRenderer::handleSDLEvents() {
    constexpr auto SDL_EVT_COUNT = 3;
    std::array<SDL_Event, SDL_EVT_COUNT> evts{};
    SDL_PumpEvents();
    const int evts_received = SDL_PeepEvents(evts.data(), evts.size(), SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST);
    for(int i = 0; i < evts_received; ++i)
        processSDLEvent(evts[i]);
}

void SDLRenderer::processSDLEvent(const SDL_Event& evt){
    switch(evt.type){
    case SDL_EVENT_KEY_DOWN:
    {
        const auto qtKey = toQtKey(evt.key.scancode);
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
        QTimer::singleShot(900, this, []{SDL_HideCursor();});
    }
        break;
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_RESIZED:
        if(evt.type == SDL_EVENT_WINDOW_RESIZED){
            window_width = evt.window.data1;
            window_height = evt.window.data2;
            qDebug() << "Window wxh: " << window_width << "x" <<window_height;
        }
        refreshDisplay();
        break;
    }
}

void SDLRenderer::refreshDisplay(){
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    if(!vid_texture){ SDL_RenderPresent(renderer); return; }

    const auto rect = calculate_display_rect(0, 0, window_width, window_height, last_frame_width, last_frame_height, last_sar);

    const auto res = SDL_RenderTextureRotated(renderer, vid_texture, NULL, &rect, 0, NULL, flip_v ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
    SDL_RenderPresent(renderer);
}

void SDLRenderer::clearDisplay(){
    if(sub_texture)
        SDL_DestroyTexture(sub_texture);
    if(vid_texture)
        SDL_DestroyTexture(vid_texture);
    sub_texture = vid_texture = nullptr;
    last_frame_width = last_frame_height = 0;
    last_frame_format = AV_PIX_FMT_NONE;
}

static std::pair<SDL_PixelFormat, SDL_BlendMode> get_sdl_pix_fmt_and_blendmode(AVPixelFormat format)
{
    auto sdl_blendmode = SDL_BLENDMODE_NONE;
    auto sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
    if (format == AV_PIX_FMT_RGB32   ||
        format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32   ||
        format == AV_PIX_FMT_BGR32_1)
        sdl_blendmode = SDL_BLENDMODE_BLEND;
    for (int i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map); i++) {
        if (format == sdl_texture_format_map[i].format) {
            sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
            break;
        }
    }

    return {sdl_pix_fmt, sdl_blendmode};
}

bool SDLRenderer::updateVideoTexture(AVFrameView img){
    if(last_frame_width != img.width() || last_frame_height != img.height() || last_frame_format != img.pixFmt()){
        const auto [sdl_pix_fmt, sdl_blendmode] = get_sdl_pix_fmt_and_blendmode(img.pixFmt());
        if(!realloc_texture(renderer, &vid_texture, sdl_pix_fmt,
                             img.width(), img.height(), sdl_blendmode, img)){
            qDebug() << "Failed to create a video texture!";
            return false;
        }
        last_frame_height = img.height();
        last_frame_width = img.width();
        last_frame_format = img.pixFmt();
        last_sar = img.sampleAR();
    }

    flip_v = img.flipV();

    bool ret = false;
    switch (img.pixFmt()) {
    case AV_PIX_FMT_YUV420P:
        if (img.linesize(0) > 0 && img.linesize(1) > 0 && img.linesize(2) > 0) {
            ret = SDL_UpdateYUVTexture(vid_texture, NULL, img.constDataPlane(0), img.linesize(0),
                                       img.constDataPlane(1), img.linesize(1),
                                       img.constDataPlane(2), img.linesize(2));
        } else if (img.linesize(0) < 0 && img.linesize(1) < 0 && img.linesize(2) < 0) {
            ret = SDL_UpdateYUVTexture(vid_texture, NULL, img.constDataPlane(0) + img.linesize(0) * (img.height() - 1), -img.linesize(0),
                                       img.constDataPlane(1) + img.linesize(1) * (AV_CEIL_RSHIFT(img.height(), 1) - 1), -img.linesize(1),
                                       img.constDataPlane(2) + img.linesize(2) * (AV_CEIL_RSHIFT(img.height(), 1) - 1), -img.linesize(2));
        } else {
            av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
            ret = false;
        }
        break;
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_NV21:
        ret = SDL_UpdateNVTexture(vid_texture, nullptr, img.constDataPlane(0), img.linesize(0), img.constDataPlane(1), img.linesize(1));
        break;
    default:
        if (img.linesize(0) < 0) {
            ret = SDL_UpdateTexture(vid_texture, nullptr, img.constDataPlane(0) + img.linesize(0) * (img.height() - 1), -img.linesize(0));
        } else {
            ret = SDL_UpdateTexture(vid_texture, nullptr, img.constDataPlane(0), img.linesize(0));
        }
        break;
    }

    return ret;
}

std::vector<AVPixelFormat> SDLRenderer::supportedFormats() const{
    return supported_avpix_fmts;
}
