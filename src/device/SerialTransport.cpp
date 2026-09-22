#include "core/Loc.h"
#include "device/SerialTransport.h"

#include <QVariant>

namespace
{
// The device is not draining the port. Writing more would only add latency and
// memory while the motion commands miss their moment; 16 KB is already about
// 1.4 s of traffic at 115200 baud, far beyond any healthy backlog.
constexpr qint64 kMaxPendingBytes = 16 * 1024;
// A wrong baud rate or a wedged firmware can send data without ever emitting a
// newline. Cap the reassembly buffer so it cannot grow without bound.
constexpr int kMaxInputBufferBytes = 64 * 1024;
}

SerialTransport::SerialTransport(QObject* parent)
    : ITCodeTransport(parent)
{
    connect(&m_serial, &QSerialPort::readyRead, this, &SerialTransport::onReadyRead);
    connect(&m_serial, &QSerialPort::errorOccurred, this, &SerialTransport::onErrorOccurred);
}

SerialTransport::~SerialTransport()
{
    close();
}

bool SerialTransport::open(const QString& port, int baudRate)
{
    close();
    m_serial.setPortName(port);
    m_serial.setBaudRate(baudRate);
    m_serial.setDataBits(QSerialPort::Data8);
    m_serial.setParity(QSerialPort::NoParity);
    m_serial.setStopBits(QSerialPort::OneStop);
    m_serial.setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serial.open(QIODevice::ReadWrite))
    {
        emit errorOccurred(LT("无法打开串口 %1：%2").arg(port, m_serial.errorString()));
        emit connectionChanged(false, m_serial.errorString());
        return false;
    }

    m_serial.clear();
    // Drop any half-received line from an earlier session, otherwise it would
    // be glued to the first bytes after reconnecting and parsed as a response.
    m_buffer.clear();
    m_backpressureWarned = false;
    setProperty("portName", QVariant(port));
    emit connectionChanged(true, LT("%1 已连接").arg(port));
    return true;
}

void SerialTransport::close()
{
    if (m_serial.isOpen())
        m_serial.close();
    m_buffer.clear();
    m_backpressureWarned = false;
}

bool SerialTransport::isOpen() const
{
    return m_serial.isOpen();
}

bool SerialTransport::writeLine(const QString& line)
{
    if (!m_serial.isOpen())
        return false;

    // Backpressure guard: report the port as unusable instead of queueing more
    // commands into a stalled device, so the owner can stop the link safely.
    if (m_serial.bytesToWrite() > kMaxPendingBytes)
    {
        if (!m_backpressureWarned)
        {
            m_backpressureWarned = true;
            emit errorOccurred(
                LT("串口发送积压超过 %1 KB，设备可能已停止响应")
                    .arg(kMaxPendingBytes / 1024));
        }
        return false;
    }

    m_backpressureWarned = false;
    const QByteArray payload = line.toUtf8() + QByteArrayLiteral("\n");
    return m_serial.write(payload) == payload.size();
}

void SerialTransport::discardPendingWrites()
{
    // Writes are buffered and drained asynchronously. Without this a DSTOP
    // issued on pause/stop would queue up behind every motion command that is
    // still in the buffer, so the device would keep moving for a while after
    // the user asked it to stop.
    if (m_serial.isOpen())
        m_serial.clear(QSerialPort::Output);
}

void SerialTransport::onReadyRead()
{
    m_buffer.append(m_serial.readAll());

    if (m_buffer.size() > kMaxInputBufferBytes)
    {
        m_buffer.clear();
        emit errorOccurred(LT("串口接收缓存溢出，已丢弃残留数据"));
        return;
    }

    int index = -1;
    while ((index = m_buffer.indexOf('\n')) >= 0)
    {
        QByteArray line = m_buffer.left(index);
        m_buffer.remove(0, index + 1);
        if (line.endsWith('\r'))
            line.chop(1);
        if (!line.isEmpty())
            emit lineReceived(QString::fromUtf8(line));
    }
}

void SerialTransport::onErrorOccurred(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError)
        return;

    const QString message = m_serial.errorString();

    // The adapter was unplugged or became unusable. Close the handle so
    // isOpen() reports the truth, and tell the owner the link is gone instead
    // of letting the scheduler keep writing into a dead port.
    const bool fatal = (error == QSerialPort::ResourceError
                        || error == QSerialPort::DeviceNotFoundError
                        || error == QSerialPort::PermissionError);

    emit errorOccurred(message);

    if (fatal && m_serial.isOpen())
    {
        m_serial.close();
        m_buffer.clear();
        m_backpressureWarned = false;
        emit connectionChanged(false, LT("设备连接已丢失：%1").arg(message));
    }
}
