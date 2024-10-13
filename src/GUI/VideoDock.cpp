#include "VideoDock.hpp"
#include "videodisplaywidget.hpp"

#include <QWindow>

VideoDock::VideoDock(QWidget* parent) : CDockWidget(parent){
    setWindowTitle("Video dock");

    auto dW = new VideoDisplayWidget(this);
    setWidget(dW);
}

