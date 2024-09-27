#ifndef TOOLBAR_HPP
#define TOOLBAR_HPP

#include <QToolBar>

class ToolBar final: public QToolBar{
    Q_OBJECT

private:

public:
    explicit ToolBar(QWidget* parent) : QToolBar(parent){}

};

#endif // TOOLBAR_HPP
