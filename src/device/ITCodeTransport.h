#pragma once

#include <QObject>
#include <QString>

class ITCodeTransport : public QObject
{
    Q_OBJECT

public:
    explicit ITCodeTransport(QObject* parent = nullptr) : QObject(parent) {}
    ~ITCodeTransport() override = default;

    virtual bool open(const QString& port, int baudRate) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual bool writeLine(const QString& line) = 0;
    // Drops commands that were queued but not yet put on the wire. Callers use
    // this before a safety command (DSTOP) so it cannot be delayed behind stale
    // motion commands still sitting in the driver's send buffer.
    virtual void discardPendingWrites() {}

signals:
    void lineReceived(const QString& line);
    void errorOccurred(const QString& message);
    void connectionChanged(bool connected, const QString& message);
};
