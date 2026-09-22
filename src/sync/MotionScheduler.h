#pragma once

#include "core/Types.h"
#include "device/TCodeEncoder.h"

#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <algorithm>

class IMediaClock;
class ITCodeTransport;

class SchedulerCore
{
public:
    void setBundle(const ScriptBundle& bundle);
    // useSafetyRamp: stretch the realignment so a large jump is travelled
    // smoothly. The caller decides: seeks and video switches pass the
    // "jump safely" switch, resuming playback passes the play/pause switch.
    void start(qint64 mediaTimeMs, qint64 leadMs = 8, bool useSafetyRamp = false);
    void pause();
    void seek(qint64 mediaTimeMs, qint64 leadMs = 8, bool useSafetyRamp = true);
    // Targets that let the device glide to the end of the action segment it is
    // travelling right now, used by the eased pause. Empty when there is
    // nothing close enough to glide into.
    QList<AxisTarget> easeToStop(qint64 mediaTimeMs) const;
    void setSeekRampMsPerPercent(int value) { m_seekRampMsPerPercent = std::clamp(value, 5, 120); }
    QList<AxisTarget> tick(qint64 mediaTimeMs);

    // Lag reported by the most recent tick() that had to realign, or -1 when
    // that tick was normal. Reading clears it. The core stays free of IO so it
    // remains directly unit testable; the owner turns this into a log line.
    qint64 takeCatchUpLagMs();
    // How late the commands emitted by the most recent tick() were sent,
    // relative to the moment the script needed them on the wire. This is the
    // part of a stall that ends up as a permanent phase offset between the
    // picture and the machine, so it is reported next to the command rate.
    // Reading clears it.
    qint64 takeMaxPhaseLagMs();

private:
    struct TrackState
    {
        bool active = false;
        bool resyncing = true;
        int segmentIndex = 0;
        int lastCommandedPos = 50;
        qint64 leadMs = 8;
        qint64 holdUntilMs = 0;
        // Carried from start()/seek() so tick() knows whether this realignment
        // is allowed to stretch for safety.
        bool useSafetyRamp = true;
    };

    // The scheduler shares the GUI thread. Scans, modal dialogs and log IO can
    // stall it for hundreds of milliseconds, which makes a batch of action
    // segments come due at once. Feeding them all to the device does not help:
    // every segment keeps its own duration, so the device replays them at
    // normal speed and never catches up with the video. Past this lag we jump
    // to the current segment and realign with the existing safety ramp instead.
    static constexpr qint64 kMaxCatchUpBehindMs = 250;
    // Second line of defence: even inside the tolerance, never emit more than
    // this many segments for one axis in a single tick, so one command line can
    // never grow into a multi-kilobyte burst.
    static constexpr int kMaxCatchUpSegmentsPerTick = 16;
    // An eased pause only glides into the current action segment while that
    // segment is about to end; past this the device is stopped right away,
    // because gliding would mean running after the stroke's target for far too
    // long.
    static constexpr qint64 kMaxEaseStopRemainingMs = 400;
    static constexpr qint64 kMinEaseStopMs = 120;
    static constexpr qint64 kMaxEaseStopMs = 600;
    ScriptBundle m_bundle;
    QMap<int, TrackState> m_states;
    int m_seekRampMsPerPercent = 30;
    qint64 m_catchUpLagMs = -1;
    qint64 m_maxPhaseLagMs = 0;
};

class MotionScheduler : public QObject
{
    Q_OBJECT

public:
    explicit MotionScheduler(ITCodeTransport* transport, IMediaClock* clock, QObject* parent = nullptr);
    ~MotionScheduler() override;

    void setBundle(const ScriptBundle& bundle);
    // deviceMayBeFar=true is used when the device may be far from where the
    // video is (switching videos or clips): the "jump safely" switch then owns
    // the ramp. Plain resuming uses the play/pause easing switch instead, and
    // with both switches off the device reaches the target as fast as the script
    // timing allows.
    void start(bool deviceMayBeFar = false);
    // Immediate stop: drop the queued commands and send DSTOP. Used whenever
    // the video is switched, the device is unplugged, the app is closed or the
    // play/pause easing is switched off.
    void pause();
    // Stop from the play/pause control. With the play/pause easing switch on and
    // the device close to the end of the action segment it is travelling, it
    // glides into that segment's target (0.12-0.6 s) instead of being cut off
    // mid-stroke; otherwise this is the same immediate DSTOP as pause().
    void pauseEased();
    void seek(qint64 mediaTimeMs);
    void stop(bool home);
    void panic();
    void rearm();
    bool isArmed() const { return m_armed; }
    bool isRunning() const { return m_running; }
    void setSeekRampMsPerPercent(int value);
    int seekRampMsPerPercent() const { return m_seekRampMsPerPercent; }
    // Global switch for the "jump safely" easing. When off, seeks and video
    // switches reach the target as fast as the script timing allows.
    void setSeekSafetyEnabled(bool enabled);
    bool seekSafetyEnabled() const { return m_seekSafetyEnabled; }
    // Own switch for the play/pause transitions: resuming eases the device back
    // into the script, pausing lets it glide to the end of the current action
    // segment instead of being cut off mid-stroke.
    void setPlayPauseEasingEnabled(bool enabled);
    bool playPauseEasingEnabled() const { return m_playPauseEasing; }
    // The axis list of a freshly opened port is unknown until D0/D1/D2 come
    // back. Until the owner confirms it, the scheduler must not drive anything:
    // a script may otherwise command axes this firmware does not have. Defaults
    // to true so a scheduler without a device (unit tests) behaves normally.
    void setDeviceAxesConfirmed(bool confirmed);
    bool deviceAxesConfirmed() const { return m_axesConfirmed; }

signals:
    void axisPositionChanged(int track, int posPercent);
    void commandSent(const QString& command);
    void stopped(bool normalEnd);
    void armedChanged(bool armed);
    void runningChanged(bool running);
    // One line per statistics window: window length, commands sent, worst tick
    // gap and worst phase lag inside it.
    void linkStats(int windowMs, int commands, int maxTickGapMs, int maxPhaseLagMs);
    // The transport kept refusing commands (backpressure or a closed port) for
    // long enough that the device can no longer be considered in sync.
    void transportStalled();

private slots:
    void onTick();

private:
    // A tick that arrives this late means the GUI thread (which also owns the
    // scheduler) was blocked, and the device got no commands for that long.
    static constexpr qint64 kStallLogMs = 80;
    void sendTargets(const QList<AxisTarget>& targets);
    void sendHome();
    void logSafetyEvents(qint64 mediaTimeMs);
    void handleWriteFailure();
    // Shared body of pause() and pauseEased().
    void stopMotion(bool eased);
    // Logs a main-thread stall (a late tick) so the hitch the device feels can be
    // traced to whatever blocked the GUI thread.
    void reportTickGap(qint64 gapMs);
    // Periodic summary of the link quality. It is what makes the log useful
    // with per-command lines switched off: the command rate and the worst tick
    // gap are exactly the two numbers that describe how the device was served.
    void reportLinkStats();

    ITCodeTransport* m_transport = nullptr;
    IMediaClock* m_clock = nullptr;
    QTimer m_timer;
    SchedulerCore m_core;
    ScriptBundle m_bundle;
    bool m_armed = true;
    bool m_running = false;
    int m_commandCounter = 0;
    int m_seekRampMsPerPercent = 30;
    bool m_seekSafetyEnabled = true;
    bool m_playPauseEasing = true;
    bool m_axesConfirmed = true;
    // Set by panic(): the device was stopped somewhere unknown, so the next
    // resume must use the safety ramp even when the switches are off.
    bool m_needsConservativeResume = false;
    qint64 m_lastCatchUpLogMs = -1;
    int m_writeFailures = 0;
    QElapsedTimer m_tickClock;
    QElapsedTimer m_stallLogClock;
    QElapsedTimer m_statsClock;
    qint64 m_tickGapMaxMs = 0;
    qint64 m_phaseLagMaxMs = 0;
    int m_commandsSinceStats = 0;
};
