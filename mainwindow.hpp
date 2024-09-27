#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "src/GUI/MenuBarMenu.hpp"

#include <QMainWindow>

class MainWindow : public QMainWindow
{
    Q_OBJECT

private:
    QWidget* priv_centralWidget = nullptr;
    MenuBarMenu* m_menus = nullptr;

    void closeEvent(QCloseEvent* evt) override;
private:
    QRect getDefaultWindowGeometry(QScreen* container);

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
};
#endif // MAINWINDOW_H
