#ifndef MENUBARMENU_H
#define MENUBARMENU_H

#include <QMenu>

class MenuBarMenu final: public QObject{
    Q_OBJECT

private:
    QMenu* m_fileMenu = nullptr, *m_playbackMenu = nullptr;

public:
    explicit MenuBarMenu(QWidget* parent);
    ~MenuBarMenu();

    QVector<QMenu*> getTopLevelMenus() const;

    Q_SLOT void openURL();

signals:
    void stopPlayback();
    void pausePlayback();
    void resumePlayback();
};

#endif // MENUBARMENU_H
