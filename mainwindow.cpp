#include "mainwindow.hpp"

#include "src/GUI/MenuBar.hpp"
#include "src/GUI/VideoDock.hpp"

#include <QScreen>
#include <QDebug>
#include <QCloseEvent>

#include "src/player.hpp"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    //Use a placeholder in the place of the central widget
    setCentralWidget(new QWidget(this));
    centralWidget()->hide();

    //Set up the menu bar
    m_menus = new MenuBarMenu(this);//Menus and actions for the menu bar
    auto mBar = new MenuBar(this, m_menus->getTopLevelMenus());
    setMenuBar(mBar);

    //
    auto vDock = new VideoDock(this);
    addDockWidget(Qt::LeftDockWidgetArea, vDock);

    setGeometry(getDefaultWindowGeometry(this->screen()));
}

MainWindow::~MainWindow() {}

QRect MainWindow::getDefaultWindowGeometry(QScreen* container){
    const auto screen_size = container->size();
    const double screen_width = screen_size.width(), screen_height = screen_size.height();
    constexpr auto window_fill = 0.4;
    const int window_w = screen_width * window_fill, window_h = screen_height * window_fill;
    const QPoint winpos((screen_width - window_w)/2, (screen_height - window_h)/2);
    //qDebug() << "Winsize: [" << window_w << ", " << window_h <<"]";
    return QRect(winpos, QSize(window_w, window_h));
}

void MainWindow::closeEvent(QCloseEvent* evt){
    stop_playback();        //TODO: fix the crash here
    QMainWindow::closeEvent(evt);
}
