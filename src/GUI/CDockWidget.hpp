#ifndef CDOCKWIDGET_HPP
#define CDOCKWIDGET_HPP

#include <QDockWidget>

class CDockWidget : public QDockWidget{
    Q_OBJECT
private:

public:
    explicit CDockWidget(QWidget* parent);
};

#endif // CDOCKWIDGET_HPP
