#include "TestLanguage.h"
#include "sync/MotionScheduler.h"

#include "core/ScriptLoader.h"
#include "device/ITCodeTransport.h"
#include "sync/IMediaClock.h"

#include <QDir>
#include <QFileInfo>
#include <QtTest>

namespace
{
QString fixturePath(const QString& relative)
{
    const QString root = QString::fromLocal8Bit(qgetenv("NEKOBEAT_TEST_FIXTURES"));
    return root.isEmpty() ? QString() : QDir(root).absoluteFilePath(relative);
}
}

class FakeClock : public IMediaClock
{
public:
    qint64 positionMs() const override { return m_position; }
    qint64 durationMs() const override { return 600000; }
    bool isPlaying() const override { return true; }
    void setPosition(qint64 value) { m_position = value; }

private:
    qint64 m_position = 0;
};

class FakeTransport : public ITCodeTransport
{
public:
    bool open(const QString&, int) override { m_open = true; return true; }
    void close() override { m_open = false; }
    bool isOpen() const override { return m_open; }
    bool writeLine(const QString& line) override
    {
        events.append(line);
        if (failWrites)
            return false;
        lines.append(line);
        return true;
    }
    void discardPendingWrites() override { events.append(QStringLiteral("discard")); }

    QStringList lines;
    // Full ordered trace of writes and buffer flushes, used to prove that a
    // stop cannot be queued behind stale motion commands.
    QStringList events;
    // Simulates a stalled or disconnected port that refuses every command.
    bool failWrites = false;

private:
    bool m_open = true;
};

class SchedulerTest : public QObject
{
    Q_OBJECT

private slots:
    void emitsSegments()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        timeline.actions = {{0, 0}, {1000, 100}, {2000, 0}};
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        SchedulerCore core;
        core.setBundle(bundle);
        core.start(0);

        const QList<AxisTarget> first = core.tick(0);
        QCOMPARE(first.size(), 1);
        QCOMPARE(first.first().track, Track::Stroke);

        const QList<AxisTarget> second = core.tick(1000);
        QCOMPARE(second.size(), 1);
        QCOMPARE(second.first().posPercent, 0);
    }

    void emptyBundleIsSafe()
    {
        SchedulerCore core;
        core.setBundle({});
        core.start(0);
        QVERIFY(core.tick(0).isEmpty());
    }

    void seekUsesSafeRamp()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        timeline.actions = {{0, 0}, {20, 100}, {2000, 0}};
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        SchedulerCore core;
        core.setBundle(bundle);
        core.start(0);
        QVERIFY(!core.tick(0).isEmpty());

        core.seek(10);
        const QList<AxisTarget> resync = core.tick(10);
        QCOMPARE(resync.size(), 1);
        QVERIFY(resync.first().intervalMs >= 100);
        QVERIFY(resync.first().intervalMs <= 1500);
    }

    void seekInLongScriptStillEmits()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        timeline.actions = {{0, 0}, {1000, 100}, {2000, 0}, {3000, 100}};
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        SchedulerCore core;
        core.setBundle(bundle);
        core.start(1500);
        const QList<AxisTarget> resync = core.tick(1500);
        QCOMPARE(resync.size(), 1);
        QCOMPARE(resync.first().track, Track::Stroke);
    }

    void realSplitSampleStartsAllAxes()
    {
        const QString mediaPath = fixturePath(QStringLiteral(
            "[Chr] ホタル 2 Collab with ナクル/[Chr] ホタル 2 Collab with ナクル.mp4"));
        if (mediaPath.isEmpty() || !QFileInfo::exists(mediaPath))
            QSKIP("回归媒体样本不存在");
        const ScriptBundle bundle = ScriptLoader::load(mediaPath);
        QCOMPARE(bundle.tracks.size(), 6);

        SchedulerCore core;
        core.setBundle(bundle);
        core.start(0);

        QSet<int> emitted;
        for (qint64 time = 0; time <= 3000; time += 10)
        {
            const QList<AxisTarget> targets = core.tick(time);
            for (const AxisTarget& target : targets)
                emitted.insert(static_cast<int>(target.track));
        }
        QCOMPARE(emitted.size(), 6);
    }

    void realSplitSampleSeekStillEmitsAllAxes()
    {
        const QString mediaPath = fixturePath(QStringLiteral(
            "[Chr] ホタル 2 Collab with ナクル/[Chr] ホタル 2 Collab with ナクル.mp4"));
        if (mediaPath.isEmpty() || !QFileInfo::exists(mediaPath))
            QSKIP("回归媒体样本不存在");
        const ScriptBundle bundle = ScriptLoader::load(mediaPath);

        SchedulerCore core;
        core.setBundle(bundle);
        core.start(100000);
        const QList<AxisTarget> targets = core.tick(100000);

        QSet<int> emitted;
        for (const AxisTarget& target : targets)
            emitted.insert(static_cast<int>(target.track));
        QCOMPARE(emitted.size(), 6);
    }

    void schedulerKeepsWritingWhilePlaying()
    {
        const QString mediaPath = fixturePath(QStringLiteral(
            "[Chr] ホタル 2 Collab with ナクル/[Chr] ホタル 2 Collab with ナクル.mp4"));
        if (mediaPath.isEmpty() || !QFileInfo::exists(mediaPath))
            QSKIP("回归媒体样本不存在");
        const ScriptBundle bundle = ScriptLoader::load(mediaPath);

        FakeClock clock;
        FakeTransport transport;
        MotionScheduler scheduler(&transport, &clock);
        scheduler.setBundle(bundle);
        scheduler.start();

        for (int i = 0; i < 120; ++i)
        {
            clock.setPosition(i * 10);
            QTest::qWait(3);
        }
        scheduler.pause();

        QVERIFY(!transport.lines.isEmpty());
        QVERIFY(transport.lines.join(QLatin1Char(' ')).contains(QStringLiteral("L0")));
    }

    void schedulerSeekWritesImmediately()
    {
        const QString mediaPath = fixturePath(QStringLiteral(
            "[Chr] ホタル 2 Collab with ナクル/[Chr] ホタル 2 Collab with ナクル.mp4"));
        if (mediaPath.isEmpty() || !QFileInfo::exists(mediaPath))
            QSKIP("回归媒体样本不存在");
        const ScriptBundle bundle = ScriptLoader::load(mediaPath);

        FakeClock clock;
        FakeTransport transport;
        MotionScheduler scheduler(&transport, &clock);
        scheduler.setBundle(bundle);
        scheduler.start();
        QTest::qWait(20);
        transport.lines.clear();

        clock.setPosition(100000);
        scheduler.seek(100000);
        QVERIFY(!transport.lines.isEmpty());
        QVERIFY(transport.lines.last().contains(QStringLiteral("L0")));
    }

    // A stalled GUI thread (media library scan, modal dialog, slow log flush)
    // must not turn into a backlog replay: the device would keep its original
    // segment durations and stay behind the video forever.
    void stalledTickRealignsInsteadOfReplayingBacklog()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        for (int i = 0; i <= 200; ++i)
            timeline.actions.append({i * 20, (i % 2) ? 100 : 0});
        timeline.actions.append({20000, 50});
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        SchedulerCore core;
        core.setBundle(bundle);
        core.start(0);
        core.tick(0);

        // Two seconds of segments came due at once.
        const QList<AxisTarget> afterStall = core.tick(2000);
        QVERIFY2(afterStall.size() <= 1,
                 "落后的 tick 必须跳段，而不是补发积压内容");
        QVERIFY2(core.takeCatchUpLagMs() >= 1000,
                 "落后事件必须上报给上层用于日志");

        // The following tick performs the realignment with the safety ramp.
        const QList<AxisTarget> realign = core.tick(2000);
        QCOMPARE(realign.size(), 1);
        QVERIFY(realign.first().intervalMs >= 100);
        QVERIFY(realign.first().intervalMs <= 1500);
    }

    // Inside the lag tolerance a single tick still must not emit an unbounded
    // burst on one command line.
    void denseTickIsCappedPerTick()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        // A flat script keeps the realignment ramp - and therefore the hold
        // that follows it - short, so the lag threshold still has room and this
        // case really exercises the per-tick cap instead of the realign.
        for (int i = 0; i < 500; ++i)
            timeline.actions.append({i, 50});
        timeline.actions.append({5000, 50});
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        SchedulerCore core;
        core.setBundle(bundle);
        core.start(0);
        core.tick(0);

        // 200 ms is below the realign threshold, so this exercises the cap.
        const QList<AxisTarget> burst = core.tick(200);
        QVERIFY(!burst.isEmpty());
        QVERIFY2(burst.size() <= 16,
                 "单次 tick 必须限制补发段数，避免超长命令行");
    }

    // Pausing, seeking or switching videos leaves the device where the last
    // command put it. The realignment ramp has to measure its distance from
    // that position, otherwise it picks a speed the device never needed.
    void resumeKeepsKnownDevicePosition()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        timeline.actions = {{0, 0}, {100, 100}, {120, 100}, {10000, 0}};
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        SchedulerCore core;
        core.setBundle(bundle);
        core.start(0);
        core.tick(0); // resync: the device is now at 100 %

        core.pause();
        // Explicit safety ramp: this case is about the ramp measuring its
        // distance from the device's real position.
        core.start(110, 8, true);
        const QList<AxisTarget> resumed = core.tick(110);
        QCOMPARE(resumed.size(), 1);
        QVERIFY2(resumed.first().intervalMs <= 200,
                 "恢复播放时斜坡距离必须基于设备真实位置");
    }

    // Resuming playback must not stretch the realignment: the device is already
    // parked where it stopped, so an extra ramp only adds lag.
    void resumeDoesNotApplySafetyRamp()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        // The device is sent to 100 %, then a resync 4 ms later asks for 0 %.
        timeline.actions = {{0, 0}, {30, 100}, {35, 0}, {3000, 0}};
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        SchedulerCore core;
        core.setBundle(bundle);
        core.start(0);
        core.tick(0);

        core.pause();
        core.start(31); // default: no safety ramp (resume)
        const QList<AxisTarget> resumed = core.tick(31);
        QCOMPARE(resumed.size(), 1);
        QCOMPARE(resumed.first().intervalMs, qint64(100));
    }

    // The jump-safety switch: with easing on the same move is stretched out,
    // with it off the device goes as fast as the script timing allows.
    void seekSafetySwitchRemovesEasing()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        timeline.actions = {{0, 0}, {30, 100}, {35, 0}, {3000, 0}};
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        SchedulerCore core;
        core.setBundle(bundle);
        core.start(0);
        core.tick(0);

        core.seek(31);
        const QList<AxisTarget> eased = core.tick(31);
        QCOMPARE(eased.size(), 1);

        // The scheduler turns the ramp off for this seek when the switch is off;
        // passing the flag straight through is what it does in the app.
        core.seek(31, 8, false);
        const QList<AxisTarget> direct = core.tick(31);
        QCOMPARE(direct.size(), 1);

        QVERIFY2(eased.first().intervalMs > direct.first().intervalMs,
                 "关闭跳转安全后，重对齐不应再被拉长");
        QCOMPARE(direct.first().intervalMs, qint64(100));
    }

    // The end-of-timeline branch used to be a hard-coded 100 ms, so a jump to
    // the end of a video ignored both the distance and the safety switch.
    void jumpPastTheLastActionUsesTheRamp()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        timeline.actions = {{0, 0}, {1000, 100}, {2000, 50}};
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        SchedulerCore core;
        core.setBundle(bundle);

        core.start(500);
        const QList<AxisTarget> first = core.tick(500);
        QCOMPARE(first.size(), 1);
        QCOMPARE(first.first().posPercent, 100);

        core.seek(5000);
        const QList<AxisTarget> late = core.tick(5000);
        QCOMPARE(late.size(), 1);
        QCOMPARE(late.first().posPercent, 50);
        QVERIFY2(late.first().intervalMs > 100,
                 "跳到时间轴末尾必须按距离给斜坡，不能再用固定 100 ms");
    }

    // After an emergency stop the device sits somewhere unknown, so the
    // remembered position cannot be trusted. Resuming must use the ramp even
    // when the user switched the play/pause easing off.
    void emergencyStopForcesTheRampOnResume()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        timeline.actions = {{0, 0}, {1000, 100}, {2000, 0}};
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        FakeClock clock;
        clock.setPosition(990);
        FakeTransport transport;
        MotionScheduler scheduler(&transport, &clock);
        scheduler.setBundle(bundle);
        scheduler.setPlayPauseEasingEnabled(false);

        const auto intervalOf = [](const QString& line) {
            const int index = line.indexOf(QLatin1Char('I'));
            return index < 0 ? qint64(-1) : line.mid(index + 1).toLongLong();
        };

        scheduler.start();
        scheduler.pause();
        transport.lines.clear();

        scheduler.start();
        QVERIFY(!transport.lines.isEmpty());
        QCOMPARE(intervalOf(transport.lines.last()), qint64(100));

        scheduler.pause();
        scheduler.panic();
        transport.lines.clear();

        scheduler.start();
        QVERIFY(!transport.lines.isEmpty());
        QVERIFY2(intervalOf(transport.lines.last()) > 100,
                 "急停后恢复必须使用安全斜坡");
    }

    // 播放/暂停缓动 has its own switch, independent of the jump safety one.
    // Resuming playback shares the ramp maths, so with the switch on the same
    // resume is stretched and with it off it goes straight to the script point.
    void playPauseEasingSwitchOwnsTheResumeRamp()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        timeline.actions = {{0, 0}, {30, 100}, {35, 0}, {3000, 0}};
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        FakeClock clock;
        FakeTransport transport;
        MotionScheduler scheduler(&transport, &clock);
        scheduler.setBundle(bundle);
        QVERIFY2(scheduler.playPauseEasingEnabled(), "播放/暂停缓动默认开启");

        scheduler.start();
        scheduler.pause();
        clock.setPosition(31);

        transport.lines.clear();
        scheduler.start();
        const QString eased = transport.lines.value(transport.lines.size() - 1);

        scheduler.pause();
        scheduler.setPlayPauseEasingEnabled(false);
        transport.lines.clear();
        scheduler.start();
        const QString direct = transport.lines.value(transport.lines.size() - 1);

        QVERIFY2(eased.endsWith(QStringLiteral("I1500")),
                 qPrintable(QStringLiteral("开启播放/暂停缓动时必须拉长斜坡: %1").arg(eased)));
        QVERIFY2(direct.endsWith(QStringLiteral("I100")),
                 qPrintable(QStringLiteral("关闭后恢复播放必须直接到位: %1").arg(direct)));
    }

    // Eased pause (the play/pause control): instead of cutting the stroke off,
    // the device glides into the end of the action segment it is travelling. The
    // glide is bounded, and with the switch off (or the segment far from its
    // end) the stop stays immediate. The plain stop never glides.
    void easedPauseGlidesToTheSegmentEnd()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        timeline.actions = {{0, 0}, {500, 100}, {1000, 0}};
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        FakeClock clock;
        FakeTransport transport;
        MotionScheduler scheduler(&transport, &clock);
        scheduler.setBundle(bundle);
        scheduler.start();

        clock.setPosition(430); // 70 ms left in the segment the axis is running
        scheduler.pauseEased();

        const QString glide = transport.lines.value(transport.lines.size() - 1);
        QVERIFY2(glide != QStringLiteral("DSTOP"),
                 "缓动暂停必须让设备滑行到当前动作段的终点，而不是硬切 DSTOP");
        const int interval = glide.mid(glide.indexOf(QLatin1Char('I')) + 1).toInt();
        QVERIFY2(interval >= 120 && interval <= 600,
                 qPrintable(QStringLiteral("滑行时长必须被限制: %1").arg(interval)));

        // Far from the end of the stroke, an eased pause stops right away.
        clock.setPosition(0);
        transport.lines.clear();
        MotionScheduler second(&transport, &clock);
        second.setBundle(bundle);
        second.start();
        second.pauseEased();
        QCOMPARE(transport.lines.value(transport.lines.size() - 1), QStringLiteral("DSTOP"));

        // And with the switch off every pause is the immediate stop again.
        transport.lines.clear();
        MotionScheduler third(&transport, &clock);
        third.setBundle(bundle);
        third.setPlayPauseEasingEnabled(false);
        third.start();
        clock.setPosition(430);
        third.pauseEased();
        QCOMPARE(transport.lines.value(transport.lines.size() - 1), QStringLiteral("DSTOP"));

        // The plain stop (video switch, device unplugged, closing the app) never
        // glides, whatever the switch says.
        transport.lines.clear();
        MotionScheduler fourth(&transport, &clock);
        fourth.setBundle(bundle);
        fourth.start();
        clock.setPosition(430);
        fourth.pause();
        QCOMPARE(transport.lines.value(transport.lines.size() - 1), QStringLiteral("DSTOP"));
    }

    // A port that keeps refusing commands means the device can no longer be
    // trusted to follow the video. The scheduler has to stop instead of quietly
    // losing sync.
    void stalledTransportStopsScheduling()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        for (int i = 0; i < 400; ++i)
            timeline.actions.append({i * 10, (i % 2) ? 100 : 0});
        timeline.actions.append({20000, 50});
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        FakeClock clock;
        FakeTransport transport;
        transport.failWrites = true;

        MotionScheduler scheduler(&transport, &clock);
        scheduler.setBundle(bundle);

        QSignalSpy stalledSpy(&scheduler, &MotionScheduler::transportStalled);
        scheduler.start();

        for (int i = 0; i < 400 && stalledSpy.isEmpty(); ++i)
        {
            clock.setPosition(i * 10);
            QTest::qWait(2);
        }

        QVERIFY2(!stalledSpy.isEmpty(), "持续写入失败必须上报 transportStalled");
        QVERIFY(!scheduler.isRunning());
        QVERIFY(transport.lines.isEmpty());
    }

    // The emergency stop has to hold until playback is explicitly resumed: a
    // heat map drag or a loop rewind must not restart the device behind it.
    void emergencyStopBlocksSeeksUntilPlaybackResumes()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        timeline.actions = {{0, 0}, {1000, 100}, {2000, 0}};
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        FakeClock clock;
        FakeTransport transport;
        MotionScheduler scheduler(&transport, &clock);
        scheduler.setBundle(bundle);
        scheduler.start();
        scheduler.pause();

        scheduler.panic();
        QVERIFY(!scheduler.isArmed());
        QVERIFY(!transport.lines.isEmpty());
        QCOMPARE(transport.lines.last(), QStringLiteral("DSTOP"));
        transport.lines.clear();

        // Seeking while stopped must not drive the device again.
        scheduler.seek(1500);
        QVERIFY2(transport.lines.isEmpty(),
                 "急停后 seek 不得重新驱动设备");

        // An explicit resume re-arms and keeps driving.
        scheduler.start();
        QVERIFY(scheduler.isArmed());
        QVERIFY(!transport.lines.isEmpty());
    }

    // A port that was just opened has no confirmed axis list: neither the
    // playback start nor a seek may drive the device until D0/D1/D2 came back,
    // otherwise a script can command axes this firmware does not have.
    void unconfirmedAxesHoldTheLinkUntilTheProbeFinishes()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        timeline.actions = {{0, 0}, {1000, 100}, {2000, 0}};
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        FakeClock clock;
        FakeTransport transport;
        MotionScheduler scheduler(&transport, &clock);
        scheduler.setBundle(bundle);
        scheduler.setDeviceAxesConfirmed(false);

        scheduler.start();
        QVERIFY(!scheduler.isRunning());
        QVERIFY2(transport.lines.isEmpty(), "轴能力未确认时不得发送动作命令");

        scheduler.seek(1500);
        QVERIFY2(transport.lines.isEmpty(), "轴能力未确认时 seek 不得驱动设备");

        // The probe answered: the link may start now.
        scheduler.setDeviceAxesConfirmed(true);
        scheduler.start();
        QVERIFY(scheduler.isRunning());
        QVERIFY(!transport.lines.isEmpty());
    }

    // The seek realignment used to walk the whole action list. It is now a
    // binary search, so this pins the result of every boundary case against the
    // previous linear rule - heat map drags run this on every mouse move.
    void seekTargetMatchesTheLinearReferenceOnLargeTimelines()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        QList<Action> actions;
        actions.reserve(20000);
        for (int i = 0; i < 20000; ++i)
            actions.append({static_cast<qint64>(i) * 10, (i * 7) % 101});
        timeline.actions = actions;
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        const auto referenceTarget = [&actions](qint64 timeMs) {
            int index = -1;
            for (int i = 0; i < actions.size(); ++i)
            {
                if (actions.at(i).atMs <= timeMs)
                    index = i;
                else
                    break;
            }
            if (index < 0)
                return actions.first().pos;
            if (index >= actions.size() - 1)
                return actions.last().pos;
            return actions.at(index + 1).pos;
        };

        const QList<qint64> probes = {-1,
                                      0,
                                      5,
                                      999,
                                      1000,
                                      1001,
                                      12345,
                                      199'990,
                                      200'000,
                                      999'999};
        for (qint64 timeMs : probes)
        {
            SchedulerCore core;
            core.setBundle(bundle);
            core.seek(timeMs, 8, false);

            const QList<AxisTarget> targets = core.tick(timeMs);
            QCOMPARE(targets.size(), 1);
            QCOMPARE(targets.first().posPercent, referenceTarget(timeMs));
        }
    }

    // Pausing (with the play/pause easing off), stopping and switching videos
    // all end with a DSTOP. That stop must not queue up behind motion commands
    // still sitting in the driver's send buffer, otherwise the device keeps
    // moving after the user stopped it.
    void stopFlushesQueuedCommandsFirst()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        timeline.actions = {{0, 0}, {500, 100}, {1000, 0}};
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        FakeClock clock;
        FakeTransport transport;
        MotionScheduler scheduler(&transport, &clock);
        scheduler.setBundle(bundle);
        scheduler.start();
        transport.events.clear();

        scheduler.pause();

        const int discardIndex = transport.events.indexOf(QStringLiteral("discard"));
        const int stopIndex = transport.events.indexOf(QStringLiteral("DSTOP"));
        QVERIFY2(discardIndex >= 0, "暂停前必须清空待发命令");
        QVERIFY2(stopIndex >= 0, "暂停必须发送 DSTOP");
        QVERIFY2(discardIndex < stopIndex, "必须先丢弃积压命令，再发送 DSTOP");
    }

    void panicFlushesQueuedCommandsFirst()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        timeline.actions = {{0, 0}, {500, 100}, {1000, 0}};
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        FakeClock clock;
        FakeTransport transport;
        MotionScheduler scheduler(&transport, &clock);
        scheduler.setBundle(bundle);
        scheduler.start();
        transport.events.clear();

        scheduler.panic();

        const int discardIndex = transport.events.indexOf(QStringLiteral("discard"));
        const int stopIndex = transport.events.indexOf(QStringLiteral("DSTOP"));
        QVERIFY2(discardIndex >= 0 && discardIndex < stopIndex,
                 "急停必须先丢弃积压命令，再发送 DSTOP");
    }

    // "Home" does not mean the same thing for every axis: a position axis
    // returns to its calibrated home, while an amplitude channel (vibration)
    // has to be switched off - half power would leave it running after the
    // video ended.
    void homeSendsPositionAxesHomeAndTurnsAmplitudeAxesOff()
    {
        ScriptBundle bundle;
        Timeline stroke;
        stroke.track = Track::Stroke;
        stroke.actions = {{0, 0}, {1000, 100}};
        bundle.tracks.insert(static_cast<int>(Track::Stroke), stroke);

        Timeline vibration;
        vibration.track = Track::Vib;
        vibration.actions = {{0, 0}, {1000, 100}};
        bundle.tracks.insert(static_cast<int>(Track::Vib), vibration);

        FakeClock clock;
        FakeTransport transport;
        MotionScheduler scheduler(&transport, &clock);
        scheduler.setBundle(bundle);

        scheduler.stop(true);

        QCOMPARE(transport.lines.size(), 2);
        QCOMPARE(transport.lines.first(), QStringLiteral("DSTOP"));
        QCOMPARE(transport.lines.last(), QStringLiteral("L05000I1000 V00000I1000"));
    }

    // A calibrated (off-centre) home is what the reset has to use.
    void homeFollowsACalibratedHomeValue()
    {
        ScriptBundle bundle;
        Timeline stroke;
        stroke.track = Track::Stroke;
        stroke.actions = {{0, 0}, {1000, 100}};
        stroke.config.home = 7500;
        bundle.tracks.insert(static_cast<int>(Track::Stroke), stroke);

        FakeClock clock;
        FakeTransport transport;
        MotionScheduler scheduler(&transport, &clock);
        scheduler.setBundle(bundle);

        scheduler.stop(true);

        QCOMPARE(transport.lines.last(), QStringLiteral("L07499I1000"));
    }

    // The device can still be travelling from a command sent a moment ago even
    // when the timer is not running, so a pause has to stop it unconditionally.
    void pauseStopsEvenWhenNotRunning()
    {
        FakeClock clock;
        FakeTransport transport;
        MotionScheduler scheduler(&transport, &clock);

        QVERIFY(!scheduler.isRunning());
        scheduler.pause();

        QCOMPARE(transport.events.last(), QStringLiteral("DSTOP"));
    }

    // Paused seeking must move the device to the jumped-to position (that is
    // how the user checks where a point is) without restarting playback.
    void pausedSeekMovesDeviceWithoutStartingPlayback()
    {
        ScriptBundle bundle;
        Timeline timeline;
        timeline.track = Track::Stroke;
        timeline.actions = {{0, 0}, {1000, 100}, {2000, 0}};
        bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);

        FakeClock clock;
        FakeTransport transport;
        MotionScheduler scheduler(&transport, &clock);
        scheduler.setBundle(bundle);
        scheduler.start();
        scheduler.pause();
        transport.events.clear();

        scheduler.seek(1200);

        QVERIFY2(!scheduler.isRunning(), "暂停中的 seek 不得重新启动调度器");
        QVERIFY2(!transport.events.isEmpty(), "暂停中的 seek 必须把设备移动到目标位置");
    }

};

// QTEST_APPLESS_MAIN creates no event loop, so QTimer never fired and the
// scheduler's timed behaviour was never actually exercised. QTEST_GUILESS_MAIN
// gives the test a QCoreApplication without needing a GUI platform plugin.
QTEST_GUILESS_MAIN(SchedulerTest)
#include "test_scheduler.moc"
