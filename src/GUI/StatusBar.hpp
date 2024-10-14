#ifndef STATUSBAR_HPP
#define STATUSBAR_HPP

#include <QStatusBar>
#include <QLabel>

class StatusBar final : public QStatusBar
{
    Q_OBJECT

    QLabel* time_label = nullptr;

public:
    explicit StatusBar(QWidget* parent = nullptr);

    Q_SLOT void updatePlaybackPos(double pos, double dur);
    Q_SLOT void setActive(bool active);
};

#endif // STATUSBAR_HPP
