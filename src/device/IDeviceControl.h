#pragma once

#include "core/Track.h"
#include "core/Types.h"
#include "device/DeviceSession.h"

#include <QObject>
#include <QString>

// The GUI-side view of the device link. Everything reachable through this
// interface is either a cached value or a posted request: an implementation must
// never block the calling (GUI) thread on the serial port, and it must never
// make the caller wait for the device to answer.
//
// The concrete implementation is DeviceLink, which owns the worker thread the
// scheduler and the serial port live in. Tests use a lightweight fake.
class IDeviceControl : public QObject
{
    Q_OBJECT

public:
    explicit IDeviceControl(QObject* parent = nullptr) : QObject(parent) {}
    ~IDeviceControl() override = default;

    virtual bool isConnected() const = 0;
    virtual DeviceCapabilities capabilities() const = 0;
    // Sends one raw TCode line (reset, limit preview, calibration test). The
    // request is queued; the caller does not wait for the write.
    virtual void sendRawLine(const QString& line) = 0;

signals:
    void connectionChanged(bool connected, const QString& message);
};
