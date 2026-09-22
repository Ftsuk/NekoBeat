#include "core/Loc.h"
#include "sync/MotionScheduler.h"

#include "core/AppLogger.h"
#include "device/ITCodeTransport.h"
#include "sync/IMediaClock.h"

#include <QThread>
#include <algorithm>

namespace
{
// Index of the last action at or before `timeMs`, or -1 when the timeline only
// starts later. Actions are sorted by time (the loader guarantees it). This
// replaces a linear scan that ran on every seek - which is every heat map drag
// step - and on every realignment of a tick.
int lastActionIndexAtOrBefore(const QList<Action>& actions, qint64 timeMs)
{
    const auto it = std::upper_bound(actions.cbegin(), actions.cend(), timeMs,
                                     [](qint64 value, const Action& action) {
                                         return value < action.atMs;
                                     });
    return static_cast<int>(it - actions.cbegin()) - 1;
}
} // namespace

void SchedulerCore::setBundle(const ScriptBundle& bundle)
{
    m_bundle = bundle;
    QMap<int, TrackState> nextStates;
    for (auto it = m_bundle.tracks.constBegin(); it != m_bundle.tracks.constEnd(); ++it)
    {
        TrackState state;
        if (m_states.contains(it.key()))
            state = m_states.value(it.key());
        state.active = it.value().config.enabled;
        nextStates.insert(it.key(), state);
    }
    m_states = nextStates;
}

void SchedulerCore::start(qint64 mediaTimeMs, qint64 leadMs, bool useSafetyRamp)
{
    for (auto it = m_bundle.tracks.constBegin(); it != m_bundle.tracks.constEnd(); ++it)
    {
        const int key = it.key();
        if (!m_states.contains(key))
            continue;
        TrackState state = m_states.value(key);
        state.active = it.value().config.enabled;
        state.resyncing = true;
        state.segmentIndex = 0;
        state.leadMs = leadMs;
        state.holdUntilMs = 0;
        state.useSafetyRamp = useSafetyRamp;
        // lastCommandedPos is deliberately kept. After a pause, a seek or a
        // video switch the device is still sitting where the last command left
        // it, so resetting it to 50 would make the realignment ramp compute its
        // distance - and therefore its speed - from a position the device was
        // never at. A brand new axis starts at 50 through TrackState's default.
        m_states.insert(key, state);
    }
    seek(mediaTimeMs, leadMs, useSafetyRamp);
}

void SchedulerCore::pause()
{
    for (auto it = m_states.begin(); it != m_states.end(); ++it)
        it.value().active = false;
}

void SchedulerCore::seek(qint64 mediaTimeMs, qint64 leadMs, bool useSafetyRamp)
{
    for (auto it = m_bundle.tracks.constBegin(); it != m_bundle.tracks.constEnd(); ++it)
    {
        const int key = it.key();
        if (!m_states.contains(key))
            continue;
        TrackState state = m_states.value(key);
        state.leadMs = leadMs;
        state.resyncing = true;
        state.active = it.value().config.enabled;
        state.useSafetyRamp = useSafetyRamp;

        const QList<Action>& actions = it.value().actions;
        if (actions.isEmpty())
        {
            state.active = false;
            m_states.insert(key, state);
            continue;
        }

        const int index = lastActionIndexAtOrBefore(actions, mediaTimeMs);
        state.segmentIndex = std::clamp(index, 0, std::max(0, static_cast<int>(actions.size()) - 2));
        m_states.insert(key, state);
    }
}

QList<AxisTarget> SchedulerCore::tick(qint64 mediaTimeMs)
{
    QList<AxisTarget> targets;
    m_catchUpLagMs = -1;
    m_maxPhaseLagMs = 0;

    for (auto it = m_bundle.tracks.constBegin(); it != m_bundle.tracks.constEnd(); ++it)
    {
        const int key = it.key();
        const Timeline& timeline = it.value();
        const QList<Action>& actions = timeline.actions;
        if (actions.isEmpty() || !m_states.contains(key))
            continue;

        TrackState state = m_states.value(key);
        if (!state.active)
            continue;

        const qint64 offset = timeline.config.offsetMs;
        const qint64 mediaWithOffset = mediaTimeMs + offset;

        if (state.resyncing)
        {
            const int index = lastActionIndexAtOrBefore(actions, mediaWithOffset);

            int targetPos = actions.first().pos;
            qint64 remaining = 0;
            if (index < 0)
            {
                // Nothing before the first action: the first point is the
                // target and the move is timed like every other realignment.
                targetPos = actions.first().pos;
                remaining = std::max<qint64>(0, actions.first().atMs - mediaWithOffset);
                state.segmentIndex = 0;
            }
            else if (index >= actions.size() - 1)
            {
                // At or past the last action. This used to be a hard-coded
                // 100 ms, so a jump to the end of the timeline ignored both the
                // distance and the safety ramp.
                targetPos = actions.last().pos;
                remaining = 0;
                state.segmentIndex = actions.size() - 1;
            }
            else
            {
                const Action& next = actions[index + 1];
                targetPos = next.pos;
                remaining = std::max<qint64>(0, next.atMs - mediaWithOffset);
                state.segmentIndex = index + 1;
            }

            // The ramp only stretches the move when the device has a long way to
            // go. With easing off the move falls back to the script's own timing
            // - the script is not slowed down by the easing feature.
            qint64 ramp = 0;
            if (state.useSafetyRamp)
            {
                const int distance = std::abs(targetPos - state.lastCommandedPos);
                ramp = std::clamp<qint64>(distance * m_seekRampMsPerPercent, 150, 4000);
            }
            const qint64 interval = std::clamp<qint64>(
                std::max<qint64>(remaining + state.leadMs, ramp), 100, 1500);

            targets.append({timeline.track, targetPos, interval});
            state.lastCommandedPos = targetPos;
            state.holdUntilMs = mediaWithOffset + std::min<qint64>(interval, 250);
            state.resyncing = false;

            m_states.insert(key, state);
            continue;
        }

        if (mediaWithOffset < state.holdUntilMs)
            continue;

        // Lag guard. The GUI thread also owns the scheduler, so a media library
        // scan, a modal dialog or a slow log flush can delay ticks. If the next
        // segment was due well beyond the catch-up tolerance, the device is
        // behind: replaying the backlog keeps it behind forever, because each
        // segment keeps its original duration. Jump to the current segment and
        // realign with the usual safety ramp instead.
        if (state.segmentIndex + 1 < actions.size())
        {
            const qint64 nextDueAt = actions[state.segmentIndex + 1].atMs - state.leadMs;
            const qint64 behindMs = mediaWithOffset - nextDueAt;
            if (behindMs > kMaxCatchUpBehindMs)
            {
                m_catchUpLagMs = std::max(m_catchUpLagMs, behindMs);
                state.resyncing = true;
                m_states.insert(key, state);
                continue;
            }
        }

        int emittedThisTick = 0;
        while (state.segmentIndex + 1 < actions.size())
        {
            const Action& from = actions[state.segmentIndex];
            const Action& to = actions[state.segmentIndex + 1];
            const qint64 dueAt = from.atMs - state.leadMs;
            if (mediaWithOffset < dueAt)
                break;

            // The ideal moment for this segment is `dueAt`. Anything later is
            // the stall showing up as an offset that never heals on its own,
            // because the segment keeps its own duration.
            m_maxPhaseLagMs = std::max(m_maxPhaseLagMs, mediaWithOffset - dueAt);

            // Never let a single tick turn into an unbounded burst on one
            // command line; realign on the next tick instead.
            if (emittedThisTick >= kMaxCatchUpSegmentsPerTick)
            {
                state.resyncing = true;
                break;
            }

            const qint64 interval = std::max<qint64>(10, to.atMs - from.atMs + state.leadMs);
            targets.append({timeline.track, to.pos, interval});
            state.lastCommandedPos = to.pos;
            ++state.segmentIndex;
            ++emittedThisTick;
        }
        m_states.insert(key, state);
    }

    return targets;
}

qint64 SchedulerCore::takeCatchUpLagMs()
{
    const qint64 lag = m_catchUpLagMs;
    m_catchUpLagMs = -1;
    return lag;
}

qint64 SchedulerCore::takeMaxPhaseLagMs()
{
    const qint64 lag = m_maxPhaseLagMs;
    m_maxPhaseLagMs = 0;
    return lag;
}

QList<AxisTarget> SchedulerCore::easeToStop(qint64 mediaTimeMs) const
{
    QList<AxisTarget> targets;

    for (auto it = m_bundle.tracks.constBegin(); it != m_bundle.tracks.constEnd(); ++it)
    {
        const int key = it.key();
        if (!m_states.contains(key))
            continue;

        const TrackState state = m_states.value(key);
        // Only an axis that is actually travelling can glide; a realignment in
        // flight is left to the immediate stop.
        if (!state.active || state.resyncing)
            continue;

        const QList<Action>& actions = it.value().actions;
        const int targetIndex = state.segmentIndex;
        if (targetIndex <= 0 || targetIndex >= actions.size())
            continue;

        const Action& from = actions.at(targetIndex - 1);
        const Action& to = actions.at(targetIndex);
        const qint64 mediaWithOffset = mediaTimeMs + it.value().config.offsetMs;
        const qint64 remaining = std::max<qint64>(0, to.atMs - mediaWithOffset);
        if (remaining > kMaxEaseStopRemainingMs)
            continue;

        // Estimate where the device is: it interpolates linearly between the two
        // actions of the segment, so the distance left to travel follows from
        // the media time.
        const qint64 span = std::max<qint64>(1, to.atMs - from.atMs);
        const qint64 elapsed = std::clamp<qint64>(mediaWithOffset - from.atMs, 0, span);
        const int estimatedPos =
            from.pos + qRound((to.pos - from.pos) * (static_cast<double>(elapsed) / span));
        const int distance = std::abs(to.pos - estimatedPos);

        // Same ramp as the jump safety easing (configured in ms per 1 % of
        // travel), only capped so a pause always settles quickly.
        const qint64 interval = std::clamp<qint64>(
            std::max<qint64>(remaining * 2, distance * m_seekRampMsPerPercent),
            kMinEaseStopMs, kMaxEaseStopMs);
        targets.append({it.value().track, to.pos, interval});
    }

    return targets;
}

MotionScheduler::MotionScheduler(ITCodeTransport* transport, IMediaClock* clock, QObject* parent)
    : QObject(parent)
    , m_transport(transport)
    , m_clock(clock)
{
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(2);
    connect(&m_timer, &QTimer::timeout, this, &MotionScheduler::onTick);
    m_stallLogClock.start();
}

MotionScheduler::~MotionScheduler()
{
    m_timer.stop();
}

void MotionScheduler::setBundle(const ScriptBundle& bundle)
{
    m_bundle = bundle;
    m_core.setBundle(bundle);
}

void MotionScheduler::setSeekRampMsPerPercent(int value)
{
    m_seekRampMsPerPercent = std::clamp(value, 5, 120);
    m_core.setSeekRampMsPerPercent(m_seekRampMsPerPercent);
}

void MotionScheduler::setSeekSafetyEnabled(bool enabled)
{
    m_seekSafetyEnabled = enabled;
    AppLogger::log(QStringLiteral("sync"),
                   QStringLiteral("跳转安全缓动: %1")
                       .arg(enabled ? LT("启用") : LT("关闭")));
}

void MotionScheduler::setPlayPauseEasingEnabled(bool enabled)
{
    m_playPauseEasing = enabled;
    AppLogger::log(QStringLiteral("sync"),
                   QStringLiteral("播放/暂停缓动: %1")
                       .arg(enabled ? LT("启用") : LT("关闭")));
}

void MotionScheduler::setDeviceAxesConfirmed(bool confirmed)
{
    if (m_axesConfirmed == confirmed)
        return;
    m_axesConfirmed = confirmed;
    AppLogger::log(QStringLiteral("sync"),
                   QStringLiteral("设备轴能力%1")
                       .arg(confirmed ? QStringLiteral("已确认")
                                      : QStringLiteral("尚未确认，联动保持挂起")));
}

void MotionScheduler::start(bool deviceMayBeFar)
{
    if (!m_transport || !m_transport->isOpen() || !m_clock)
        return;
    if (m_running)
        return;
    if (!m_axesConfirmed)
    {
        // Waiting for the probe result: driving an unknown axis set is exactly
        // what the capability filter exists to prevent.
        AppLogger::log(QStringLiteral("sync"),
                       QStringLiteral("轴能力尚未确认，暂不开始联动"));
        return;
    }

    // The two easing switches are independent: a clip switch or a fresh video
    // follows the "jump safely" setting, resuming follows the play/pause one.
    bool useSafetyRamp = deviceMayBeFar ? m_seekSafetyEnabled : m_playPauseEasing;
    if (m_needsConservativeResume)
    {
        // The device was emergency-stopped and is sitting somewhere unknown, so
        // the remembered position - and the distance the ramp is based on -
        // cannot be trusted. Resume with the ramp regardless of the switches.
        useSafetyRamp = true;
        m_needsConservativeResume = false;
    }

    // start() is only reached from a real playback resume, so an emergency
    // stop is released here. seek() deliberately does not re-arm, which keeps
    // heat map drags and loop rewinds from restarting the device behind a stop.
    if (!m_armed)
        rearm();

    m_running = true;
    m_core.start(m_clock->positionMs(), 8, useSafetyRamp);
    m_timer.start();
    m_tickClock.restart();
    m_statsClock.restart();
    m_tickGapMaxMs = 0;
    m_phaseLagMaxMs = 0;
    m_commandsSinceStats = 0;
    AppLogger::log(QStringLiteral("sync"),
                   QStringLiteral("start at %1 ms (ramp=%2)")
                       .arg(m_clock->positionMs())
                       .arg(useSafetyRamp ? QStringLiteral("on") : QStringLiteral("off")));
    emit runningChanged(true);
    onTick();
}

void MotionScheduler::pause()
{
    stopMotion(false);
}

void MotionScheduler::pauseEased()
{
    stopMotion(true);
}

void MotionScheduler::stopMotion(bool eased)
{
    const qint64 mediaTimeMs = m_clock ? m_clock->positionMs() : 0;
    const bool wasRunning = m_running;

    m_running = false;
    m_timer.stop();
    // Close out the statistics window so a short session still reports what the
    // link was doing instead of dropping the numbers.
    reportLinkStats();
    emit runningChanged(false);

    // Eased pause: while the device is close to the end of the action segment it
    // is travelling, it glides into that segment's target instead of being cut
    // off mid-stroke. The motion stays monotonic and bounded by the segment it
    // was already executing, so this is a softer stop, not a longer one.
    QList<AxisTarget> stopTargets;
    if (eased && wasRunning && m_playPauseEasing && m_armed && m_transport && m_transport->isOpen())
        stopTargets = m_core.easeToStop(mediaTimeMs);

    m_core.pause();
    AppLogger::log(QStringLiteral("sync"), QStringLiteral("pause"));

    // Always send the stop, even when the timer was not running: the device can
    // still be travelling towards the target of a command that was sent a
    // moment ago. Queued writes are dropped first, otherwise the DSTOP would
    // only be transmitted after the stale motion commands ahead of it.
    if (!m_transport || !m_transport->isOpen())
        return;

    m_transport->discardPendingWrites();
    if (!stopTargets.isEmpty())
    {
        sendTargets(stopTargets);
        AppLogger::log(QStringLiteral("sync"), QStringLiteral("缓动暂停：设备滑行到当前动作段终点"));
        return;
    }
    m_transport->writeLine(TCodeEncoder::stopCommand());
}

void MotionScheduler::seek(qint64 mediaTimeMs)
{
    m_core.seek(mediaTimeMs, 8, m_seekSafetyEnabled);
    AppLogger::log(QStringLiteral("sync"), QStringLiteral("seek: %1 ms").arg(mediaTimeMs));

    if (!m_armed || !m_axesConfirmed || !m_transport || !m_transport->isOpen())
        return;

    if (!m_running)
    {
        // Paused: the video does not advance, but the device still has to move
        // to the point the user jumped to - that is the whole point of jumping
        // while paused. Send one realignment command and leave the timer off, so
        // this cannot restart playback on its own; resuming does its own
        // realignment anyway.
        const QList<AxisTarget> targets = m_core.tick(mediaTimeMs);
        logSafetyEvents(mediaTimeMs);
        if (!targets.isEmpty())
            sendTargets(targets);
        return;
    }

    const QList<AxisTarget> targets = m_core.tick(mediaTimeMs);
    logSafetyEvents(mediaTimeMs);
    if (!targets.isEmpty())
        sendTargets(targets);
}

void MotionScheduler::stop(bool home)
{
    m_running = false;
    m_timer.stop();
    reportLinkStats();
    emit runningChanged(false);
    m_core.pause();
    AppLogger::log(QStringLiteral("sync"), QStringLiteral("stop, home=%1").arg(home));
    if (m_transport && m_transport->isOpen())
    {
        m_transport->discardPendingWrites();
        m_transport->writeLine(TCodeEncoder::stopCommand());
        if (home)
            sendHome();
    }
    emit stopped(true);
}

void MotionScheduler::panic()
{
    m_running = false;
    m_timer.stop();
    reportLinkStats();
    emit runningChanged(false);
    m_core.pause();
    AppLogger::log(QStringLiteral("sync"), QStringLiteral("panic"));
    if (m_transport && m_transport->isOpen())
    {
        m_transport->discardPendingWrites();
        m_transport->writeLine(TCodeEncoder::stopCommand());
    }
    m_armed = false;
    m_needsConservativeResume = true;
    emit armedChanged(false);
}

void MotionScheduler::rearm()
{
    m_armed = true;
    AppLogger::log(QStringLiteral("sync"), QStringLiteral("rearm（解除急停）"));
    emit armedChanged(true);
}

void MotionScheduler::onTick()
{
    if (!m_running || !m_armed || !m_transport || !m_transport->isOpen() || !m_clock)
        return;

    // The scheduler and the window share the GUI thread, so a late tick is
    // exactly the moment the device has to hold still. Logging the gap keeps a
    // hitch attributable instead of guesswork.
    qint64 gapMs = 0;
    if (m_tickClock.isValid())
        gapMs = m_tickClock.restart();
    else
        m_tickClock.start();
    m_tickGapMaxMs = std::max(m_tickGapMaxMs, gapMs);
    reportTickGap(gapMs);

    const QList<AxisTarget> targets = m_core.tick(m_clock->positionMs());
    logSafetyEvents(m_clock->positionMs());
    m_phaseLagMaxMs = std::max(m_phaseLagMaxMs, m_core.takeMaxPhaseLagMs());
    if (!targets.isEmpty())
        sendTargets(targets);

    // One dense line every five seconds replaces thousands of per-command lines
    // when the detail switch is off, and still shows whether the device was
    // served evenly.
    if (m_statsClock.isValid() && m_statsClock.elapsed() >= 5000)
        reportLinkStats();
}

void MotionScheduler::reportLinkStats()
{
    if (m_commandsSinceStats <= 0)
        return;

    const qint64 windowMs = m_statsClock.isValid() ? m_statsClock.elapsed() : 0;
    AppLogger::log(QStringLiteral("sync"),
                   QStringLiteral("联动统计：%1 条命令 / %2 ms，最大 tick 间隔 %3 ms，"
                                  "最大相位滞后 %4 ms")
                       .arg(m_commandsSinceStats)
                       .arg(windowMs)
                       .arg(m_tickGapMaxMs)
                       .arg(m_phaseLagMaxMs));
    emit linkStats(static_cast<int>(windowMs), m_commandsSinceStats,
                   static_cast<int>(m_tickGapMaxMs), static_cast<int>(m_phaseLagMaxMs));
    m_commandsSinceStats = 0;
    m_tickGapMaxMs = 0;
    m_phaseLagMaxMs = 0;
    m_statsClock.restart();
}

void MotionScheduler::reportTickGap(qint64 gapMs)
{
    if (gapMs < kStallLogMs)
        return;
    // Never flood the log: one line per second at most.
    if (m_stallLogClock.isValid() && m_stallLogClock.elapsed() < 1000)
        return;

    m_stallLogClock.restart();
    AppLogger::log(QStringLiteral("sync"),
                   QStringLiteral("主线程卡顿 %1 ms：定时器迟到，设备这段时间没有新命令").arg(gapMs));
}

void MotionScheduler::logSafetyEvents(qint64 mediaTimeMs)
{
    const qint64 lag = m_core.takeCatchUpLagMs();
    if (lag >= 0
        && (m_lastCatchUpLogMs < 0 || mediaTimeMs - m_lastCatchUpLogMs >= 1000))
    {
        m_lastCatchUpLogMs = mediaTimeMs;
        AppLogger::log(QStringLiteral("sync"),
                       QStringLiteral("落后 %1 ms，跳段重对齐（不再补发积压动作）").arg(lag));
    }
}

void MotionScheduler::sendTargets(const QList<AxisTarget>& targets)
{
    QMap<int, AxisConfig> configs;
    for (auto it = m_bundle.tracks.constBegin(); it != m_bundle.tracks.constEnd(); ++it)
        configs.insert(it.key(), it.value().config);

    const QString line = TCodeEncoder::encodeLine(targets, configs);
    if (line.isEmpty())
        return;

    if (!m_transport->writeLine(line))
    {
        handleWriteFailure();
        return;
    }
    m_writeFailures = 0;
    ++m_commandsSinceStats;

    ++m_commandCounter;
    AppLogger::log(QStringLiteral("tcode"),
                   QStringLiteral("t=%1ms #%2 | %3")
                       .arg(m_clock ? m_clock->positionMs() : -1)
                       .arg(m_commandCounter)
                       .arg(line));
    emit commandSent(line);
    for (const AxisTarget& target : targets)
        emit axisPositionChanged(static_cast<int>(target.track), target.posPercent);
}

void MotionScheduler::handleWriteFailure()
{
    ++m_writeFailures;

    if (m_writeFailures == 1 || m_writeFailures % 50 == 0)
    {
        AppLogger::log(QStringLiteral("sync"),
                       QStringLiteral("串口写入失败（连续 %1 次），本批命令已丢弃")
                           .arg(m_writeFailures));
    }

    // A few dropped commands can happen while the device is busy; a sustained
    // run means the link is unusable and the device can no longer be trusted to
    // follow the video. Stop driving it instead of silently losing sync.
    constexpr int kMaxWriteFailures = 25;
    if (m_writeFailures < kMaxWriteFailures)
        return;

    AppLogger::log(QStringLiteral("sync"),
                   QStringLiteral("串口持续写入失败，已停止联动"));
    m_running = false;
    m_timer.stop();
    m_core.pause();
    emit runningChanged(false);
    emit transportStalled();
}

void MotionScheduler::sendHome()
{
    QList<AxisTarget> targets;
    for (auto it = m_bundle.tracks.constBegin(); it != m_bundle.tracks.constEnd(); ++it)
    {
        const AxisConfig& config = it.value().config;
        if (!config.enabled)
            continue;
        // Position axes go to their configured home; an amplitude channel
        // (vibration / lube / pump) is switched off instead - "50 %" there
        // would leave the motor running at half power after the video ended.
        const int percent =
            TrackInfo::kind(it.value().track) == TrackInfo::AxisKind::Amplitude
                ? 0
                : TCodeEncoder::percentForHome(config);
        targets.append({it.value().track, percent, 1000});
    }
    sendTargets(targets);
}
