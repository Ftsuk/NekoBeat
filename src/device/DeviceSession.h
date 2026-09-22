#pragma once

#include "core/Types.h"

#include <QObject>
#include <QSet>
#include <QTimer>

class ITCodeTransport;

struct DeviceCapabilities
{
    QString port;
    QString firmware;
    QString tcodeVersion;
    QSet<int> supportedTracks;
    bool valid = false;
    bool probeComplete = false;
};

// Carried across the worker/GUI boundary by queued signals.
Q_DECLARE_METATYPE(DeviceCapabilities)

class DeviceSession : public QObject
{
    Q_OBJECT

public:
    explicit DeviceSession(ITCodeTransport* transport, QObject* parent = nullptr);
    ~DeviceSession() override;

    bool connectToPort(const QString& port, int baudRate = 115200);
    void disconnectFromDevice();
    bool isConnected() const;
    DeviceCapabilities capabilities() const { return m_capabilities; }

    bool sendLine(const QString& line);
    void probe();

signals:
    void capabilitiesChanged(const DeviceCapabilities& capabilities);
    void errorOccurred(const QString& message);
    void connectionChanged(bool connected, const QString& message);

private slots:
    void onLineReceived(const QString& line);
    void onProbeTimeout();

private:
    ITCodeTransport* m_transport = nullptr;
    DeviceCapabilities m_capabilities;
    QTimer m_probeTimer;
};
