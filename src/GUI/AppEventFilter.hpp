#ifndef APPEVENTFILTER_HPP
#define APPEVENTFILTER_HPP

#include <QObject>

class AppEventFilter : public QObject
{
    Q_OBJECT

    class PlayerCore& core;

    bool eventFilter(QObject* sender, QEvent* evt) override;
    void handleKeyEvent(class QKeyEvent* evt);
public:
    explicit AppEventFilter(PlayerCore& core, QObject *parent = nullptr);

signals:
};

#endif // APPEVENTFILTER_HPP
