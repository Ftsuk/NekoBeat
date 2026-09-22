#include "TestLanguage.h"
#include "core/Types.h"
#include "device/DeviceLink.h"
#include "device/ITCodeTransport.h"
#include "sync/IMediaClock.h"

#include <QMutex>
#include <QMutexLocker>
#include <QSignalSpy>
#include <QThread>
#include <QtTest>

#include <atomic>

namespace
{
// A clock that runs on real time, so "the worker kept ticking while the GUI was
// blocked" can be observed without a video file.
class RealtimeClock : public IMediaClock
{
public:
    RealtimeClock() { m_clock.start(); }
    qint64 positionMs() const override { return m_clock.elapsed(); }
    qint64 durationMs() const override { return 600000; }
    bool isPlaying() const override { return true; }

private:
    QElapsedTimer m_clock;
};

// Open port that records everything it is asked to send. writeLine runs on the
// worker thread while the test reads from the GUI thread, hence the mutex.
class RecordingTransport : public ITCodeTransport
{
public:
    bool open(const QString&, int) override
    {
        m_open = true;
        return true;
    }
    void close() override { m_open = false; }
    bool isOpen() const override { return true; }
    bool writeLine(const QString& line) override
    {
        QMutexLocker locker(&m_mutex);
        m_lines.append(line);
        return true;
    }
    void discardPendingWrites() override {}

    int lineCount() const
    {
        QMutexLocker locker(&m_mutex);
        return m_lines.size();
    }
    QStringList lines() const
    {
        QMutexLocker locker(&m_mutex);
        return m_lines;
    }

private:
    mutable QMutex m_mutex;
    QStringList m_lines;
    bool m_open = true;
};

// open() always fails, exactly like a port another program still holds.
class FailingOpenTransport : public ITCodeTransport
{
public:
    bool open(const QString&, int) override
    {
        emit connectionChanged(false, QStringLiteral("端口已被占用"));
        return false;
    }
    void close() override {}
    bool isOpen() const override { return false; }
    bool writeLine(const QString&) override { return false; }
};

// A transport that can be made to report a lost connection, like an unplugged
// USB adapter. An injected transport counts as already open.
class ControllableTransport : public ITCodeTransport
{
public:
    bool open(const QString&, int) override
    {
        m_open.store(true);
        emit connectionChanged(true, QStringLiteral("已连接"));
        return true;
    }
    void close() override { m_open.store(false); }
    bool isOpen() const override { return m_open.load(); }
    bool writeLine(const QString& line) override
    {
        if (!m_open.load())
            return false;
        QMutexLocker locker(&m_mutex);
        m_lines.append(line);
        return true;
    }
    void discardPendingWrites() override {}

    void loseConnection()
    {
        m_open.store(false);
        emit connectionChanged(false, QStringLiteral("设备连接已丢失"));
    }

    int lineCount() const
    {
        QMutexLocker locker(&m_mutex);
        return m_lines.size();
    }

private:
    mutable QMutex m_mutex;
    QStringList m_lines;
    std::atomic<bool> m_open{true};
};

ScriptBundle denseBundle(int actions, qint64 spacingMs)
{
    ScriptBundle bundle;
    Timeline timeline;
    timeline.track = Track::Stroke;
    timeline.actions.reserve(actions);
    for (int i = 0; i < actions; ++i)
        timeline.actions.append({static_cast<qint64>(i) * spacingMs, (i % 2) ? 100 : 0});
    bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);
    return bundle;
}
} // namespace

// The device link owns the worker thread the scheduler and the serial port live
// in. These cases pin the two properties the architecture change exists for:
// the interface can never block the device, and the device keeps running while
// the interface is busy.
class DeviceLinkTest : public QObject
{
    Q_OBJECT

private slots:
    void startsDisconnected()
    {
        RealtimeClock clock;
        DeviceLink link(&clock);

        QVERIFY(!link.isConnected());
        QVERIFY(!link.isRunning());
        QVERIFY(!link.capabilities().probeComplete);
    }

    // A failed open() must reach the caller: the window's retry counter and its
    // "connecting" flag are both driven by this signal.
    void failedOpenIsReportedToTheCaller()
    {
        RealtimeClock clock;
        FailingOpenTransport transport;
        DeviceLink link(&clock, &transport);

        QSignalSpy spy(&link, &DeviceLink::connectionChanged);
        link.connectDevice(QStringLiteral("COM_TEST"), 115200);

        QTRY_VERIFY_WITH_TIMEOUT(spy.count() > 0, 3000);
        QCOMPARE(spy.first().at(0).toBool(), false);
        QVERIFY(!link.isConnected());
    }

    // An unplugged adapter has to tell the window and stop the worker's
    // scheduler; otherwise the timer keeps running and the device would resume
    // on its own once a port is opened again.
    void lostConnectionIsReportedAndStopsTheScheduler()
    {
        RealtimeClock clock;
        ControllableTransport transport;
        DeviceLink link(&clock, &transport);
        link.setDeviceAxesConfirmed(true);
        link.setBundle(denseBundle(500, 20));
        link.start();
        QTRY_VERIFY_WITH_TIMEOUT(transport.lineCount() > 0, 3000);

        QSignalSpy lost(&link, &DeviceLink::connectionChanged);
        transport.loseConnection();

        QTRY_VERIFY_WITH_TIMEOUT(lost.count() > 0, 3000);
        QCOMPARE(lost.first().at(0).toBool(), false);
        QVERIFY(!link.isConnected());
        QTRY_VERIFY_WITH_TIMEOUT(!link.isRunning(), 3000);

        const int afterLoss = transport.lineCount();
        QTest::qWait(150);
        QCOMPARE(transport.lineCount(), afterLoss);
    }

    // The whole point of the batch: with the scheduler out of the GUI thread, a
    // 400 ms freeze of the interface no longer stops the machine.
    void workerKeepsDrivingWhileTheGuiThreadIsBlocked()
    {
        RealtimeClock clock;
        RecordingTransport transport;
        DeviceLink link(&clock, &transport);
        link.setDeviceAxesConfirmed(true);

        link.setBundle(denseBundle(500, 20));
        link.start();
        QTRY_VERIFY_WITH_TIMEOUT(transport.lineCount() > 0, 3000);

        const int before = transport.lineCount();
        // Reproduces a library scan, a modal dialog or a slow layout: the old
        // design stopped sending for exactly this long.
        QThread::msleep(400);
        const int after = transport.lineCount();

        link.stop(false);

        qInfo("GUI 阻塞 400 ms 期间设备线程发出 %d 条指令（阻塞前累计 %d）",
              after - before, before);
        QVERIFY2(after - before >= 5,
                 qPrintable(QStringLiteral("GUI 阻塞 400 ms 期间只发出 %1 条指令")
                                .arg(after - before)));
    }

    // Requests have to return immediately even with a large script: the GUI must
    // never wait for the worker.
    void requestsDoNotBlockTheCaller()
    {
        RealtimeClock clock;
        RecordingTransport transport;
        DeviceLink link(&clock, &transport);

        const ScriptBundle big = denseBundle(20000, 10);
        QElapsedTimer timer;
        timer.start();
        link.setBundle(big);
        link.start(true);
        const qint64 elapsed = timer.elapsed();

        QVERIFY2(elapsed < 50,
                 qPrintable(QStringLiteral("请求耗时 %1 ms，门面不应阻塞调用方").arg(elapsed)));
    }

    // Coalescing: a burst of seeks in one event-loop turn must reach the worker
    // as a single request, so a burst can never queue up stale targets.
    void seekBurstIsCoalesced()
    {
        RealtimeClock clock;
        RecordingTransport transport;
        DeviceLink link(&clock, &transport);
        link.setDeviceAxesConfirmed(true);
        link.setBundle(denseBundle(500, 20));

        QSignalSpy spy(&link, &DeviceLink::seekRequested);
        for (int i = 0; i < 100; ++i)
            link.seek(1000 + i * 10);

        // Nothing is posted during the loop itself; the flush happens once the
        // event loop runs again.
        QCOMPARE(spy.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 1000);
        QCOMPARE(spy.first().at(0).toLongLong(), 1990);
    }

    // Shutdown is the one synchronous step: it has to stop the machine and join
    // the thread, otherwise closing the window would leave the port open.
    void shutdownStopsTheMachineAndJoinsTheThread()
    {
        RealtimeClock clock;
        RecordingTransport transport;
        DeviceLink link(&clock, &transport);
        link.setDeviceAxesConfirmed(true);
        link.setBundle(denseBundle(500, 20));
        link.start();
        QTRY_VERIFY_WITH_TIMEOUT(transport.lineCount() > 0, 3000);

        link.shutdown();

        const QStringList lines = transport.lines();
        QVERIFY(!lines.isEmpty());
        QVERIFY2(lines.last() == QStringLiteral("DSTOP"),
                 qPrintable(QStringLiteral("关闭时最后一条必须是 DSTOP，实际是 %1")
                                .arg(lines.last())));
        QVERIFY(!link.isRunning());
    }

    // A device that was never probed must not be driven at all.
    void unconfirmedAxesKeepTheSchedulerIdle()
    {
        RealtimeClock clock;
        RecordingTransport transport;
        DeviceLink link(&clock, &transport);

        link.setDeviceAxesConfirmed(false);
        link.setBundle(denseBundle(100, 20));
        link.start();
        QTest::qWait(200);

        QCOMPARE(transport.lineCount(), 0);
        QVERIFY(!link.isRunning());
    }
};

QTEST_GUILESS_MAIN(DeviceLinkTest)

#include "test_device_link.moc"
