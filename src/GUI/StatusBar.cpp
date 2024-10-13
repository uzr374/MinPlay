#include "StatusBar.hpp"

StatusBar::StatusBar(QWidget* parent) : QStatusBar(parent) {
    time_label = new QLabel("00:00:00/00:00:00", this);
    addPermanentWidget(time_label);
}
