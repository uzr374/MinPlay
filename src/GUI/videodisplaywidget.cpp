#include "videodisplaywidget.hpp"

#include <QPainter>

VideoDisplayWidget::VideoDisplayWidget(QWidget* parent) : QWidget(parent) {
    lout = new QBoxLayout(QBoxLayout::TopToBottom, this);
    lout->setContentsMargins(0, 0, 0, 0);
}

VideoDisplayWidget::~VideoDisplayWidget(){
    destroySDLRenderer();
}

void VideoDisplayWidget::resizeEvent(QResizeEvent* evt){
    QWidget::resizeEvent(evt);
}

void VideoDisplayWidget::paintEvent(QPaintEvent* evt){
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    QWidget::paintEvent(evt);
}

SDLRenderer* VideoDisplayWidget::createSDLRenderer(){
    if(renderer){
        destroySDLRenderer();
    }
    try{
        renderer = new SDLRenderer(this);
        wrapperW = renderer->toWidget(this);
        lout->addWidget(wrapperW);
    } catch(...){
        delete renderer;
        renderer = nullptr;
    }

    return renderer;
}

void VideoDisplayWidget::destroySDLRenderer(){
    if(renderer){
        renderer->finishRendering();
        lout->removeWidget(wrapperW);
        delete wrapperW;//Deletes renderer's wrapper widget
        delete renderer;//Deletes the SDL window and renderer
        wrapperW = nullptr;
        renderer = nullptr;
    }
}


