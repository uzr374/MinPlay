#ifndef LOGGERWIDGET_H
#define LOGGERWIDGET_H

#include <QTextEdit>

class LoggerWidget final : public QWidget
{
    Q_OBJECT

    QTextEdit* text_edit = nullptr;
public:
    explicit LoggerWidget(QWidget *parent = nullptr);

    Q_INVOKABLE void logMessage(QString msg);
    Q_SLOT void clearText();
    Q_SLOT void saveToFile();
};

#endif // LOGGERWIDGET_H
