#ifndef SLIDER_HPP
#define SLIDER_HPP

#include <QSlider>

class Slider : public QSlider
{
    Q_OBJECT;

    void mousePressEvent(QMouseEvent* evt) override;
    void mouseReleaseEvent(QMouseEvent* evt) override;
    void handleMouseEvt(QMouseEvent* evt);

    bool is_being_updated = false;
public:
    Slider(QWidget* parent = nullptr, int stretch = 0);
    void setPos(double percent);
    inline bool falseUpdate() const{return is_being_updated;}
};

#endif // SLIDER_HPP
