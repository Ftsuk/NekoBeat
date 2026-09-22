#pragma once

#include "device/ITCodeTransport.h"

#include <QSerialPort>

class SerialTransport : public ITCodeTransport
{
    Q_OBJECT

public:
    explicit SerialTransport(QObject* parent = nullptr);
    ~SerialTransport() override;

    bool open(const QString& port, int baudRate) override;
    void close() override;
    bool isOpen() const override;
    bool writeLine(const QString& line) override;
    void discardPendingWrites() override;

private slots:
    void onReadyRead();
    void onErrorOccurred(QSerialPort::SerialPortError error);

private:
    QSerialPort m_serial;
    QByteArray m_buffer;
    bool m_backpressureWarned = false;
};
