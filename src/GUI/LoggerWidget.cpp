#include "LoggerWidget.h"

#include <QHBoxLayout>
#include <QFileDialog>
#include <QFile>

LoggerWidget::LoggerWidget(QWidget *parent)
    : QWidget{parent}, text_edit(new QTextEdit())
{
    text_edit->setReadOnly(true);
    auto lout = new QHBoxLayout(this);
    lout->setContentsMargins(0, 0, 0, 0);
    lout->addWidget(text_edit);
}

void LoggerWidget::logMessage(QString msg){
    text_edit->append(msg);
}

void LoggerWidget::clearText(){
    text_edit->clear();
}

void LoggerWidget::saveToFile(){
    const auto path = QFileDialog::getSaveFileName(this, "Save the logger output as...", QString(), "Text files (*.txt, *.log)");
    if(!path.isEmpty()){
        QFile logText(path);
        if(!logText.open(QFile::WriteOnly | QFile::Text)){
            logMessage("Failed to open log file!");
        } else{
            QTextStream out(&logText);
            out << text_edit->toPlainText();
        }
    }
}
