#include "mainwindow.hpp"

#include <SDL3/SDL.h>

#include <QApplication>
#include <QLocale>
#include <QTranslator>

int main(int argc, char *argv[])
{
    const bool sdl_initialized = SDL_Init(0);
    if(!sdl_initialized) return -1;
    QApplication a(argc, argv);

    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString &locale : uiLanguages) {
        const QString baseName = "MinPlay_" + QLocale(locale).name();
        if (translator.load(":/i18n/" + baseName)) {
            a.installTranslator(&translator);
            break;
        }
    }
    MainWindow w;
    w.show();
    const auto ret = a.exec();
    SDL_Quit();

    return ret;
}
