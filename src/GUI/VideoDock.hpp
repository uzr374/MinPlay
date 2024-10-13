#ifndef VIDEODOCK_H
#define VIDEODOCK_H

#include "CDockWidget.hpp"

class VideoDock final : public CDockWidget{
    Q_OBJECT;

public:
    explicit VideoDock(QWidget* parent);
};

#endif // VIDEODOCK_H
