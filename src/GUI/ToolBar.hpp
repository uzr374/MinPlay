#ifndef TOOLBAR_HPP
#define TOOLBAR_HPP

#include "Slider.hpp"

#include <QToolBar>

class ToolBar final: public QToolBar{
    Q_OBJECT

private:
    Slider* vol_slider = nullptr, *playback_slider = nullptr;

public:
    explicit ToolBar(QWidget* parent);

    Q_SIGNAL void sigSeek(double percent);
};

#endif // TOOLBAR_HPP
