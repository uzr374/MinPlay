#ifndef APPEVENTFILTER_HPP
#define APPEVENTFILTER_HPP

#include <QObject>

class AppEventFilter : public QObject
{
    Q_OBJECT

    bool eventFilter(QObject* sender, QEvent* evt) override;
    void handleKeyEvent(class QKeyEvent* evt);
public:
    explicit AppEventFilter(QObject *parent = nullptr);

signals:
};

#endif // APPEVENTFILTER_HPP
