#ifndef VIDEODOCK_H
#define VIDEODOCK_H

#include "CDockWidget.hpp"

class VideoDock final : public CDockWidget{
    Q_OBJECT;

private:
    QWindow* embedded_window = nullptr;
    QWidget* wrapper_widget = nullptr;

public:
    explicit VideoDock(QWidget* parent);

    Q_INVOKABLE void embedVideoWindow(WId handle);
    Q_INVOKABLE void disembedVideoWindow();
    Q_INVOKABLE void postQuitCleanup();
};

void embedSDLWindow(WId handle);
void disembedSDLWindow();
void signalPostQuitCleanup();

#endif // VIDEODOCK_H
