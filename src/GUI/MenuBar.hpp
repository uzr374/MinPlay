#ifndef MENUBAR_HPP
#define MENUBAR_HPP

#include <QMenuBar>

class MenuBar final : public QMenuBar{
    Q_OBJECT;

private:

public:
    explicit MenuBar(QWidget* parent, QVector<QMenu*> menus);

};

#endif // MENUBAR_HPP
