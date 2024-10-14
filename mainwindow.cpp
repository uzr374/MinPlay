#include "mainwindow.hpp"

#include "src/GUI/MenuBar.hpp"
#include "src/GUI/VideoDock.hpp"
#include "src/GUI/videodisplaywidget.hpp"
#include "src/GUI/ToolBar.hpp"
#include "src/GUI/StatusBar.hpp"

#include <QScreen>
#include <QDebug>
#include <QCloseEvent>

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

    auto tBar = new ToolBar(this);
    addToolBar(Qt::BottomToolBarArea, tBar);

    auto sBar = new StatusBar(this);
    setStatusBar(sBar);

    auto vDock = new VideoDock(this);
    auto vWidget = static_cast<VideoDisplayWidget*>(vDock->widget());
    addDockWidget(Qt::LeftDockWidgetArea, vDock);

    core = new PlayerCore(this, vWidget);

    setGeometry(getDefaultWindowGeometry(this->screen()));

    connect(m_menus, &MenuBarMenu::stopPlayback, core, &PlayerCore::stopPlayback);
    connect(m_menus, &MenuBarMenu::pausePlayback, core, &PlayerCore::pausePlayback);
    connect(m_menus, &MenuBarMenu::resumePlayback, core, &PlayerCore::resumePlayback);
    connect(tBar, &ToolBar::sigSeek, core, &PlayerCore::requestSeekPercent);
    connect(core, &PlayerCore::updatePlaybackPos, tBar, &ToolBar::updatePlaybackPos);
    connect(core, &PlayerCore::updatePlaybackPos, sBar, &StatusBar::updatePlaybackPos);
    connect(core, &PlayerCore::setControlsActive, tBar, &ToolBar::setActive);
    connect(core, &PlayerCore::setControlsActive, sBar, &StatusBar::setActive);
}

MainWindow::~MainWindow() {
    core->stopPlayback();
}

QRect MainWindow::getDefaultWindowGeometry(QScreen* container){
    const auto screen_size = container->size();
    const double screen_width = screen_size.width(), screen_height = screen_size.height();
    constexpr auto window_fill = 0.4;
    const int window_w = screen_width * window_fill, window_h = screen_height * window_fill;
    const QPoint winpos((screen_width - window_w)/2, (screen_height - window_h)/2);
    return QRect(winpos, QSize(window_w, window_h));
}

void MainWindow::closeEvent(QCloseEvent* evt){
    core->stopPlayback();
    QMainWindow::closeEvent(evt);
}
