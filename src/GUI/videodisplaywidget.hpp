#ifndef VIDEODISPLAYWIDGET_HPP
#define VIDEODISPLAYWIDGET_HPP

#include "../../playback/sdlrenderer.hpp"

#include <QWidget>
#include <QHBoxLayout>

class VideoDisplayWidget final : public QWidget
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(VideoDisplayWidget);

    SDLRenderer* renderer = nullptr;
    QHBoxLayout* lout = nullptr;
    QWidget* wrapperW = nullptr;

    void resizeEvent(QResizeEvent* evt) override;
    void paintEvent(QPaintEvent* evt) override;
public:
    VideoDisplayWidget(QWidget* parent);
    ~VideoDisplayWidget();

    SDLRenderer* getSDLRenderer();
};

#endif // VIDEODISPLAYWIDGET_HPP
