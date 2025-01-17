#include "mainwindow.hpp"

#include <SDL3/SDL.h>

#include <QApplication>
#include <QLocale>
#include <QTranslator>
#include <QPalette>

static QPalette getDarkPalette(){
    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, QColor(45, 45, 45));
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Base, QColor(30, 30, 30));
    darkPalette.setColor(QPalette::AlternateBase, QColor(45, 45, 45));
    darkPalette.setColor(QPalette::ToolTipBase, Qt::white);
    darkPalette.setColor(QPalette::ToolTipText, Qt::white);
    darkPalette.setColor(QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Button, QColor(45, 45, 45));
    darkPalette.setColor(QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::BrightText, Qt::red);

    darkPalette.setColor(QPalette::Highlight, QColor(100, 100, 150));
    darkPalette.setColor(QPalette::HighlightedText, Qt::black);

    return darkPalette;
}

int main(int argc, char *argv[])
{
    bool sdl_initialized = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    if(!sdl_initialized) sdl_initialized = SDL_Init(SDL_INIT_VIDEO);
    if(!sdl_initialized) return -1;

    QApplication a(argc, argv);
    a.setPalette(getDarkPalette());

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
