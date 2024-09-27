#include "VideoDock.hpp"
#include "DisplayWidget.hpp"

#include <QWindow>

#include "../player.hpp"

static VideoDock* vdock_inst = nullptr;

VideoDock::VideoDock(QWidget* parent) : CDockWidget(parent){
    setWindowTitle("Video dock");

    auto dW = new DisplayWidget(this);
    setWidget(dW);

    vdock_inst = this;
}

void VideoDock::embedVideoWindow(WId native_handle){
    embedded_window = QWindow::fromWinId(native_handle);
    wrapper_widget = QWidget::createWindowContainer(embedded_window);
    setWidget(wrapper_widget);
    qDebug() << "Window with ID " << native_handle << " was successfully embedded";
}

//This must be invoked before destroying the native window in the render thread
void VideoDock::disembedVideoWindow(){
    qDebug() << "Disembedding the window";
    setWidget(nullptr);
    wrapper_widget->setParent(nullptr);
    embedded_window->setParent(nullptr);
    wrapper_widget->deleteLater();
    wrapper_widget = nullptr;
    embedded_window = nullptr;
}

void VideoDock::postQuitCleanup(){
    postquit_cleanup();
}

void embedSDLWindow(WId handle){
    QMetaObject::invokeMethod(vdock_inst, "embedVideoWindow", Qt::BlockingQueuedConnection, handle);
}

void disembedSDLWindow(){
    QMetaObject::invokeMethod(vdock_inst, "disembedVideoWindow", Qt::BlockingQueuedConnection);
}

void signalPostQuitCleanup(){
    QMetaObject::invokeMethod(vdock_inst, "postQuitCleanup", Qt::QueuedConnection);
}
