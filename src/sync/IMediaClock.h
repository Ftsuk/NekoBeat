#pragma once

#include <QObject>

class IMediaClock : public QObject
{
    Q_OBJECT

public:
    explicit IMediaClock(QObject* parent = nullptr) : QObject(parent) {}
    ~IMediaClock() override = default;

    virtual qint64 positionMs() const = 0;
    virtual qint64 durationMs() const = 0;
    virtual bool isPlaying() const = 0;

signals:
    void positionChanged(qint64 positionMs);
    void durationChanged(qint64 durationMs);
    void playingChanged(bool playing);
    void mediaEnded();
};

