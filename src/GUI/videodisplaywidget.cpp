#include "videodisplaywidget.hpp"

#include <QPainter>

VideoDisplayWidget::VideoDisplayWidget(QWidget* parent) : QWidget(parent) {
    lout = new QHBoxLayout(this);
    lout->setContentsMargins(0, 0, 0, 0);
    try{
        renderer = new SDLRenderer(this);
        wrapperW = renderer->toWidget(this);
        lout->addWidget(wrapperW);
    } catch(...){
        delete renderer;
        renderer = nullptr;
    }
}

VideoDisplayWidget::~VideoDisplayWidget(){
delete renderer;
}

void VideoDisplayWidget::resizeEvent(QResizeEvent* evt){
    QWidget::resizeEvent(evt);
}

void VideoDisplayWidget::paintEvent(QPaintEvent* evt){
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    QWidget::paintEvent(evt);
}

SDLRenderer* VideoDisplayWidget::getSDLRenderer(){
    return renderer;
}



