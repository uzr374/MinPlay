#include "StatusBar.hpp"

#include "../utils.hpp"

StatusBar::StatusBar(QWidget* parent) : QStatusBar(parent) {
    time_label = new QLabel(QString(), this);
    addPermanentWidget(time_label);
    setActive(false);
}

void StatusBar::updatePlaybackPos(double pos, double dur){
    time_label->setText(Utils::posToHMS(pos, dur));
}

void StatusBar::setActive(bool active){
    updatePlaybackPos(0.0, 0.0);
}
