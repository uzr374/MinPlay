#include "AppEventFilter.hpp"
#include "../../playback/playbackengine.hpp"

#include <QKeyEvent>

AppEventFilter::AppEventFilter(PlayerCore& cor, QObject *parent) : core(cor),
     QObject{parent}
{

}

bool AppEventFilter::eventFilter(QObject* sender, QEvent* evt){
    switch (evt->type()) {
    case QEvent::KeyPress:
    {
        handleKeyEvent(static_cast<QKeyEvent*>(evt));
    }
        break;
    default:
        break;
    }

    return QObject::eventFilter(sender, evt);
}

void AppEventFilter::handleKeyEvent(QKeyEvent* evt){
    switch (evt->key()) {
    case Qt::Key_Space:
        core.togglePause();
        break;
    case Qt::Key_Right:
        core.requestSeekIncr(5);
        break;
    case Qt::Key_Left:
        core.requestSeekIncr(-5);
        break;
    case Qt::Key_Up:
        //core.requestSeekIncr(60);
        break;
    case Qt::Key_Down:
        //core.requestSeekIncr(-60);
        break;
    default:
        break;
    }
}
