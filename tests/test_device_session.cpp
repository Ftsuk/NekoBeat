#include "core/Track.h"
#include "device/DeviceSession.h"
#include "device/ITCodeTransport.h"

#include <QtTest>

namespace
{
// Minimal transport: the session only needs somebody to emit lineReceived, and
// recording the writes keeps the probe assertions possible.
class FakeTransport : public ITCodeTransport
{
public:
    bool open(const QString& port, int) override
    {
        m_port = port;
        m_open = true;
        return true;
    }
    void close() override { m_open = false; }
    bool isOpen() const override { return m_open; }
    bool writeLine(const QString& line) override
    {
        writes.append(line);
        return true;
    }
    void discardPendingWrites() override {}

    void feed(const QString& line) { emit lineReceived(line); }

    QStringList writes;
    QString m_port;
    bool m_open = false;
};
} // namespace

class DeviceSessionTest : public QObject
{
    Q_OBJECT

private slots:
    // The whole axis list on one line: every axis has to be picked up, not just
    // the first token.
    void singleLineAxisListIsFullyParsed()
    {
        FakeTransport transport;
        DeviceSession session(&transport);

        transport.feed(QStringLiteral("L0 L1 L2 R0 R1 R2"));

        const DeviceCapabilities capabilities = session.capabilities();
        QCOMPARE(capabilities.supportedTracks.size(), 6);
        for (Track track : TrackInfo::sr6Tracks())
            QVERIFY(capabilities.supportedTracks.contains(static_cast<int>(track)));
    }

    void multiLineAxisListIsFullyParsed()
    {
        FakeTransport transport;
        DeviceSession session(&transport);

        transport.feed(QStringLiteral("L0"));
        transport.feed(QStringLiteral("L1"));
        transport.feed(QStringLiteral("L2"));
        transport.feed(QStringLiteral("R0"));
        transport.feed(QStringLiteral("R1"));
        transport.feed(QStringLiteral("R2"));

        QCOMPARE(session.capabilities().supportedTracks.size(), 6);
    }

    void axesOnADecoratedLineAreFound()
    {
        FakeTransport transport;
        DeviceSession session(&transport);

        transport.feed(QStringLiteral("Supported axes: L0 R1 V0 A2"));

        const DeviceCapabilities capabilities = session.capabilities();
        QCOMPARE(capabilities.supportedTracks.size(), 4);
        QVERIFY(capabilities.supportedTracks.contains(static_cast<int>(Track::Stroke)));
        QVERIFY(capabilities.supportedTracks.contains(static_cast<int>(Track::Roll)));
        QVERIFY(capabilities.supportedTracks.contains(static_cast<int>(Track::Vib)));
        QVERIFY(capabilities.supportedTracks.contains(static_cast<int>(Track::Lube)));
    }

    // Text that merely contains axis-like runs must not be mistaken for a list:
    // "SR6" is firmware, and "L0I100" is a command echo without a separator.
    void unrelatedTextDoesNotAddAxes()
    {
        FakeTransport transport;
        DeviceSession session(&transport);

        transport.feed(QStringLiteral("SR6 v3.0"));
        QCOMPARE(session.capabilities().supportedTracks.size(), 0);

        transport.feed(QStringLiteral("L09999I100"));
        QCOMPARE(session.capabilities().supportedTracks.size(), 0);
    }

    void handshakeLineMarksTheDeviceValid()
    {
        FakeTransport transport;
        DeviceSession session(&transport);

        QVERIFY(!session.capabilities().valid);
        transport.feed(QStringLiteral("TCode v0.3"));
        QVERIFY(session.capabilities().valid);
    }

    void probeWritesHandshakeCommands()
    {
        FakeTransport transport;
        DeviceSession session(&transport);
        QVERIFY(session.connectToPort(QStringLiteral("COM_TEST")));

        session.probe();

        QCOMPARE(transport.writes, QStringList({QStringLiteral("D0"), QStringLiteral("D1"),
                                                QStringLiteral("D2")}));
    }
};

QTEST_GUILESS_MAIN(DeviceSessionTest)

#include "test_device_session.moc"
