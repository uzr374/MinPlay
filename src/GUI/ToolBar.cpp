#include "ToolBar.hpp"

#include <QHBoxLayout>

ToolBar::ToolBar(QWidget* parent) : QToolBar(parent){
    setObjectName("toolBar");
    playback_slider = new Slider(this, 10);
    vol_slider = new Slider(this, 2);

    addWidget(playback_slider);
    addWidget(vol_slider);

    connect(playback_slider, &Slider::valueChanged, this, [&](int val){emit sigSeek(double(val)/playback_slider->maximum());});
}
