#ifndef VIDEODISPLAYWIDGET_HPP
#define VIDEODISPLAYWIDGET_HPP

#include "../../playback/sdlrenderer.hpp"

#include <QWidget>
#include <QBoxLayout>

class VideoDisplayWidget final : public QWidget
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(VideoDisplayWidget);

    SDLRenderer* renderer = nullptr;
    QBoxLayout* lout = nullptr;
    QWidget* wrapperW = nullptr;

    void resizeEvent(QResizeEvent* evt) override;
    void paintEvent(QPaintEvent* evt) override;
public:
    VideoDisplayWidget(QWidget* parent);
    ~VideoDisplayWidget();

    Q_INVOKABLE void destroySDLRenderer();
    Q_INVOKABLE SDLRenderer* createSDLRenderer();
};

#endif // VIDEODISPLAYWIDGET_HPP
