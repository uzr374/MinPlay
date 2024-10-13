#ifndef DISPLAYWIDGET_H
#define DISPLAYWIDGET_H

#include <QWidget>
#include <QTimer>

class DisplayWidget final : public QWidget{
    Q_OBJECT

private:
    QTimer m_mouseTimer;

private:
    void setCursorVisibility(bool visible);
    bool event(QEvent* evt) override;
    void paintEvent(QPaintEvent* evt) override;

public:
    explicit DisplayWidget(QWidget* parent);
    ~DisplayWidget();
};

#endif // DISPLAYWIDGET_H
