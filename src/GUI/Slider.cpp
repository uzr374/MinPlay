#include "Slider.hpp"

#include <QMouseEvent>

Slider::Slider(QWidget* parent, int stretch) : QSlider(Qt::Horizontal, parent) {
    static constexpr auto MIN_VAL = 1, MAX_VAL = 1000;
    auto sPolicy = sizePolicy();
    sPolicy.setHorizontalStretch(stretch);
    setSizePolicy(sPolicy);
    setTracking(true);
    setMouseTracking(true);
    setRange(MIN_VAL, MAX_VAL);
    setSingleStep(MAX_VAL/10);
    setPageStep(MAX_VAL/50);
}

void Slider::handleMouseEvt(QMouseEvent* evt){
    const auto x = evt->pos().x();
    const double pcent = double(x)/width();
    setValue(pcent*(maximum() - minimum()));
}

void Slider::mousePressEvent(QMouseEvent* evt){
    handleMouseEvt(evt);
    return QSlider::mousePressEvent(evt);
}

void Slider::mouseReleaseEvent(QMouseEvent* evt){
    handleMouseEvt(evt);
    return QSlider::mouseReleaseEvent(evt);
}

void Slider::setPos(double percent){
    is_being_updated = true;
    setValue(percent * (maximum() - minimum()));
    is_being_updated = false;
}
