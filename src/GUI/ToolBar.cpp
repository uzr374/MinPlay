#include "ToolBar.hpp"

#include <QHBoxLayout>

ToolBar::ToolBar(QWidget* parent) : QToolBar(parent){
    setObjectName("toolBar");
    playback_slider = new Slider(this, 10);
    vol_slider = new Slider(this, 2);

    playback_slider->setEnabled(false);

    addAction("Stop");
    addSeparator();
    addAction("Pause");
    addAction("Step");
    addSeparator();
    addAction("<<");
    addAction(">>");
    addSeparator();
    addWidget(playback_slider);
    addSeparator();
    addWidget(vol_slider);

    connect(playback_slider, &Slider::valueChanged, this, [&](int val){
        if(!playback_slider->falseUpdate())
            emit sigSeek(double(val)/playback_slider->maximum());});
}

void ToolBar::updatePlaybackPos(double pos, double dur){
    if(!std::isnan(dur) && dur > 0){
        const auto pcent = pos/dur;
        playback_slider->setPos(pcent);
    }
}

void ToolBar::setActive(bool active){
    playback_slider->setPos(0);
    playback_slider->setEnabled(active);
}
