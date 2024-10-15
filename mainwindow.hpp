#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "src/GUI/MenuBarMenu.hpp"
#include "playback/playercore.hpp"
#include "src/utils.hpp"

#include <QMainWindow>

class MainWindow : public QMainWindow
{
    Q_OBJECT

    const QString settingsUrl = Utils::getApplicationDir() + "/settings/mainwindow.state";
private:
    QWidget* priv_centralWidget = nullptr;
    MenuBarMenu* m_menus = nullptr;
    PlayerCore* core = nullptr;

    void closeEvent(QCloseEvent* evt) override;
private:
    QRect getDefaultWindowGeometry(QScreen* container);

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
};
#endif // MAINWINDOW_H
