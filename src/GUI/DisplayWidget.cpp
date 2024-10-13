#include "DisplayWidget.hpp"

#include <QEvent>
#include <QCursor>
#include <QPainter>
#include <QHBoxLayout>

DisplayWidget::DisplayWidget(QWidget* parent) : QWidget(parent), m_mouseTimer(this){
    m_mouseTimer.setSingleShot(true);
    m_mouseTimer.setInterval(900);
    m_mouseTimer.callOnTimeout([this]{setCursorVisibility(false);});

    setMouseTracking(true);//To receive mouse move events properly
}

DisplayWidget::~DisplayWidget(){

}

void DisplayWidget::setCursorVisibility(bool visible){
    setCursor(visible ? Qt::ArrowCursor : Qt::BlankCursor);
}

bool DisplayWidget::event(QEvent* evt){
    switch(evt->type()){
        //Fallthrough
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
        setCursorVisibility(true);
        if(m_mouseTimer.isActive()){
            m_mouseTimer.stop();
        }
        m_mouseTimer.start();
        break;
    }

    return QWidget::event(evt);
}

void DisplayWidget::paintEvent(QPaintEvent* evt){
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    return QWidget::paintEvent(evt);
}
