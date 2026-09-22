#include "core/Loc.h"
#include "device/DeviceSession.h"

#include "core/AppLogger.h"
#include "device/ITCodeTransport.h"

#include <QRegularExpression>

DeviceSession::DeviceSession(ITCodeTransport* transport, QObject* parent)
    : QObject(parent)
    , m_transport(transport)
{
    connect(m_transport, &ITCodeTransport::lineReceived, this, &DeviceSession::onLineReceived);
    connect(m_transport, &ITCodeTransport::errorOccurred, this, &DeviceSession::errorOccurred);
    // The transport's own connectionChanged has no other receiver. Without this
    // forwarding a failed open() (port busy / driver error) and an unplugged
    // cable never reach the window: it would not retry, would keep its
    // "connecting" flag set and would still believe the device is connected.
    // Only the disconnect direction is forwarded; connectToPort() announces a
    // successful open itself.
    connect(m_transport, &ITCodeTransport::connectionChanged, this,
            [this](bool connected, const QString& message) {
                if (!connected)
                    emit connectionChanged(false, message);
            });
    m_probeTimer.setSingleShot(true);
    connect(&m_probeTimer, &QTimer::timeout, this, &DeviceSession::onProbeTimeout);
}

DeviceSession::~DeviceSession()
{
    // Keep the safety behaviour (DSTOP + close) but stay silent: the owner may
    // already be partially destructed, and announcing a disconnect from inside
    // the teardown would call back into a half-dead window.
    disconnect(this, nullptr, nullptr, nullptr);
    m_probeTimer.stop();
    if (m_transport && m_transport->isOpen())
    {
        m_transport->writeLine(QStringLiteral("DSTOP"));
        m_transport->close();
    }
    m_capabilities = {};
}

bool DeviceSession::connectToPort(const QString& port, int baudRate)
{
    if (!m_transport->open(port, baudRate))
        return false;

    m_capabilities = {};
    m_capabilities.port = port;
    AppLogger::log(QStringLiteral("serial"), QStringLiteral("connect: %1").arg(port));
    emit connectionChanged(true, LT("%1 已连接").arg(port));
    // CH340 and many ESP32 boards reset when the port opens. Give the firmware
    // time to boot before sending D0/D1/D2, otherwise the first probe is lost.
    QTimer::singleShot(1000, this, [this] {
        if (m_transport && m_transport->isOpen())
            probe();
    });
    return true;
}

void DeviceSession::disconnectFromDevice()
{
    m_probeTimer.stop();
    AppLogger::log(QStringLiteral("serial"), QStringLiteral("disconnect"));
    if (m_transport->isOpen())
    {
        m_transport->writeLine(QStringLiteral("DSTOP"));
        m_transport->close();
    }
    m_capabilities = {};
    emit capabilitiesChanged(m_capabilities);
    emit connectionChanged(false, LT("设备已断开"));
}

bool DeviceSession::isConnected() const
{
    return m_transport && m_transport->isOpen();
}

bool DeviceSession::sendLine(const QString& line)
{
    return m_transport && m_transport->writeLine(line);
}

void DeviceSession::probe()
{
    if (!m_transport->isOpen())
        return;

    m_capabilities = {};
    m_capabilities.port = m_transport->property("portName").toString();
    AppLogger::log(QStringLiteral("serial"), QStringLiteral("probe D0/D1/D2"));
    m_transport->writeLine(QStringLiteral("D0"));
    m_transport->writeLine(QStringLiteral("D1"));
    m_transport->writeLine(QStringLiteral("D2"));
    m_probeTimer.start(1600);
}

void DeviceSession::onLineReceived(const QString& line)
{
    const QString trimmed = line.trimmed();
    AppLogger::log(QStringLiteral("serial"), QStringLiteral("rx: %1").arg(trimmed));
    if (trimmed.startsWith(QStringLiteral("TCode"), Qt::CaseInsensitive))
    {
        m_capabilities.tcodeVersion = trimmed;
        m_capabilities.valid = true;
        emit capabilitiesChanged(m_capabilities);
        return;
    }

    // A D2 answer carries either one axis per line ("L0") or the whole list on
    // a single line ("L0 L1 L2 R0 R1 R2"). Only matching the first token of the
    // line used to leave the other axes marked as unsupported, and they were
    // then filtered out of every script - the "only the main axis moves"
    // symptom. Scan every standalone token on the line instead.
    static const QRegularExpression axisPattern(
        QStringLiteral("(?:^|\\s)([LRVA])([0-9])(?=\\s|$)"));
    bool tracksChanged = false;
    auto axisMatches = axisPattern.globalMatch(trimmed);
    while (axisMatches.hasNext())
    {
        const QRegularExpressionMatch match = axisMatches.next();
        const QString identifier = match.captured(1) + match.captured(2);
        const auto track = TrackInfo::fromIdentifier(identifier);
        if (!track)
            continue;
        if (!m_capabilities.supportedTracks.contains(static_cast<int>(*track)))
        {
            m_capabilities.supportedTracks.insert(static_cast<int>(*track));
            tracksChanged = true;
        }
    }
    if (tracksChanged)
    {
        emit capabilitiesChanged(m_capabilities);
        return;
    }

    if (trimmed.contains(QStringLiteral("SR6"), Qt::CaseInsensitive)
        || trimmed.contains(QStringLiteral("OSR"), Qt::CaseInsensitive))
    {
        m_capabilities.firmware = trimmed;
        emit capabilitiesChanged(m_capabilities);
    }
}

void DeviceSession::onProbeTimeout()
{
    m_capabilities.probeComplete = true;
    if (m_capabilities.valid && m_capabilities.supportedTracks.isEmpty())
    {
        for (Track track : TrackInfo::sr6Tracks())
            m_capabilities.supportedTracks.insert(static_cast<int>(track));
        AppLogger::log(QStringLiteral("serial"),
                       QStringLiteral("D2 未返回有效轴列表，回退为 SR6 六轴预设"));
    }
    AppLogger::log(QStringLiteral("serial"),
                   QStringLiteral("probe complete, tcode=%1, tracks=%2")
                       .arg(m_capabilities.tcodeVersion)
                       .arg(m_capabilities.supportedTracks.size()));
    if (!m_capabilities.valid)
    {
        m_capabilities.valid = false;
        emit errorOccurred(LT("未收到有效的 TCode 握手响应"));
    }
    emit capabilitiesChanged(m_capabilities);
}
