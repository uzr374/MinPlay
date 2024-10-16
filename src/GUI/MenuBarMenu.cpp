#include "MenuBarMenu.hpp"

#include "../../playback/playercore.hpp"

#include <QFileDialog>


MenuBarMenu::MenuBarMenu(QWidget* parent) : QObject(parent){
    auto createMenu = [&](const QString& title){
        return new QMenu(title, parent);
    };
    m_fileMenu = createMenu("File");
    m_playbackMenu = createMenu("Playback");

    auto fopen_act = m_fileMenu->addAction(tr("Open"));

    auto addPlaybMnuAct = [&](QString title){
        return m_playbackMenu->addAction(title);
    };

    auto pause_act = addPlaybMnuAct("Pause");
    auto resume_act = addPlaybMnuAct("Resume");
    auto stop_act = addPlaybMnuAct("Stop");

    connect(fopen_act, &QAction::triggered, this, &MenuBarMenu::openURL);
    connect(pause_act, &QAction::triggered, this, &MenuBarMenu::pausePlayback);
    connect(resume_act, &QAction::triggered, this, &MenuBarMenu::resumePlayback);
    connect(stop_act, &QAction::triggered, this, &MenuBarMenu::stopPlayback);
}

MenuBarMenu::~MenuBarMenu(){}

QVector<QMenu*> MenuBarMenu::getTopLevelMenus() const {
    return QVector<QMenu*>{m_fileMenu, m_playbackMenu};
}

void MenuBarMenu::openURL(){
    const auto path = QFileDialog::getOpenFileName(dynamic_cast<QWidget*>(this->parent()), "Choose a file to open");
    if(!path.isEmpty()){
        playerCore.openURL(path);
    }
}
