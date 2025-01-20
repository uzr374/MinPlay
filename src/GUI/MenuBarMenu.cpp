#include "MenuBarMenu.hpp"

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

    sstreams_menu = m_playbackMenu->addMenu("Sub streams");
    astreams_menu = m_playbackMenu->addMenu("Audio streams");
    vstreams_menu = m_playbackMenu->addMenu("Video streams");

    connect(fopen_act, &QAction::triggered, this, &MenuBarMenu::getURLs);
    connect(pause_act, &QAction::triggered, this, &MenuBarMenu::pausePlayback);
    connect(resume_act, &QAction::triggered, this, &MenuBarMenu::resumePlayback);
    connect(stop_act, &QAction::triggered, this, &MenuBarMenu::stopPlayback);
    connect(sstreams_menu, &QMenu::triggered, this, &MenuBarMenu::streamSwitchRequested);
    connect(astreams_menu, &QMenu::triggered, this, &MenuBarMenu::streamSwitchRequested);
    connect(vstreams_menu, &QMenu::triggered, this, &MenuBarMenu::streamSwitchRequested);
}

MenuBarMenu::~MenuBarMenu(){}

QVector<QMenu*> MenuBarMenu::getTopLevelMenus() const {
    return QVector<QMenu*>{m_fileMenu, m_playbackMenu};
}

void MenuBarMenu::getURLs() {
    const auto urls = QFileDialog::getOpenFileNames(dynamic_cast<QWidget*>(this->parent()), "Choose files to open");
    if(!urls.isEmpty()){
        emit submitURLs(urls);
    }
}

void MenuBarMenu::updateStreams(std::vector<CAVStream> streams){
    for(auto act : video_streams){
        vstreams_menu->removeAction(act);
        delete act;
    }
    for(auto act : audio_streams){
        astreams_menu->removeAction(act);
        delete act;
    }
    for(auto act : sub_streams){
        sstreams_menu->removeAction(act);
        delete act;
    }

    video_streams = audio_streams = sub_streams = {};
    for(const auto& st : streams){
        if(st.isAudio()){
            auto act = astreams_menu->addAction(st.titleStr().c_str());
            act->setData(st.idx());
            audio_streams.push_back(act);
        } else if(st.isVideo()){
            auto act = vstreams_menu->addAction(st.titleStr().c_str());
            act->setData(st.idx());
            video_streams.push_back(act);
        } else if(st.isSub()){
            auto act = sstreams_menu->addAction(st.titleStr().c_str());
            act->setData(st.idx());
            sub_streams.push_back(act);
        }
    }
}

void MenuBarMenu::streamSwitchRequested(QAction* triggered){

}
