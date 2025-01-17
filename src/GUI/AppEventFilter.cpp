#include "AppEventFilter.hpp"
#include "../../playback/playbackengine.hpp"

#include <QKeyEvent>

AppEventFilter::AppEventFilter(QObject *parent)
    : QObject{parent}
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
        playerCore.togglePause();
        break;
    case Qt::Key_Right:
        playerCore.requestSeekIncr(5);
        break;
    case Qt::Key_Left:
        playerCore.requestSeekIncr(-5);
        break;
    default:
        break;
    }
}
