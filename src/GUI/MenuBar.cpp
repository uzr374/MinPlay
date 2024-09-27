#include "MenuBar.hpp"

MenuBar::MenuBar(QWidget* parent, QVector<QMenu*> menus){
    setWindowTitle("Menu bar");
    for(auto menu: menus){
        addMenu(menu);
    }
}
