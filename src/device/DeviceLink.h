#pragma once

#include "core/Types.h"
#include "device/IDeviceControl.h"

#include <QThread>

class ITCodeTransport;
class IMediaClock;
class DeviceSession;
class MotionScheduler;

// Facade in front of the device worker thread.
//
// Everything device related - the serial port, the handshake, the action timer
// and the TCode encoding - runs in one worker thread, because those steps used
// to share the GUI thread with video rendering, window layout and library scans:
// any of those could delay a command and make the machine stutter. The GUI now
// only posts requests and reads cached state, so a busy interface can no longer
// stop the device.
//
// GUI thread friends:
//   - every public method returns immediately (requests are queued);
//   - isConnected()/capabilities()/isRunning() read cached copies;
//   - seek() coalesces: only the newest target of the current event loop turn
//     is forwarded, so a burst can never queue up in the worker;
//   - shutdown() is the one synchronous call: it stops the machine (DSTOP) and
//     closes the port before the thread goes away.
class DeviceLink : public IDeviceControl
{
    Q_OBJECT

public:
    explicit DeviceLink(IMediaClock* clock, QObject* parent = nullptr);
    // Test seam: drive the link through an injected transport (already open)
    // instead of a real serial port.
    DeviceLink(IMediaClock* clock, ITCodeTransport* injectedTransport,
               QObject* parent = nullptr);
    ~DeviceLink() override;

    bool isConnected() const override { return m_connected; }
    DeviceCapabilities capabilities() const override { return m_capabilities; }

    bool isRunning() const { return m_running; }
    bool isArmed() const { return m_armed; }
    bool axesConfirmed() const { return m_axesConfirmed; }
    int seekRampMsPerPercent() const { return m_seekRamp; }
    bool seekSafetyEnabled() const { return m_seekSafety; }
    bool playPauseEasingEnabled() const { return m_playPauseEasing; }

    // Requests (all queued to the worker thread).
    void connectDevice(const QString& port, int baudRate = 115200);
    void disconnectDevice();
    void releasePort();
    void sendRawLine(const QString& line) override;
    void start(bool deviceMayBeFar = false);
    void pause();
    void pauseEased();
    void seek(qint64 mediaTimeMs);
    void stop(bool home);
    void panic();
    void rearm();
    void setBundle(const ScriptBundle& bundle);
    void setSeekRampMsPerPercent(int value);
    void setSeekSafetyEnabled(bool enabled);
    void setPlayPauseEasingEnabled(bool enabled);
    // While the freshly opened port has not been probed yet the axis list is
    // unknown; the scheduler stays idle until this is set to true.
    void setDeviceAxesConfirmed(bool confirmed);

    // Synchronous teardown: DSTOP, close the port, stop the worker thread. Only
    // called when the application is going away (or by the destructor).
    void shutdown();

signals:
    // Posted to the worker.
    void connectRequested(const QString& port, int baudRate);
    void disconnectRequested();
    void releaseRequested();
    void rawLineRequested(const QString& line);
    void startRequested(bool deviceMayBeFar);
    void pauseRequested();
    void pauseEasedRequested();
    void stopRequested(bool home);
    void panicRequested();
    void rearmRequested();
    void bundleRequested(const ScriptBundle& bundle);
    void seekRequested(qint64 mediaTimeMs);
    void seekRampRequested(int value);
    void seekSafetyRequested(bool enabled);
    void playPauseEasingRequested(bool enabled);
    void axesConfirmedRequested(bool confirmed);
    void shutdownRequested();

    // Forwarded back to the GUI (queued).
    void capabilitiesChanged(const DeviceCapabilities& capabilities);
    void errorOccurred(const QString& message);
    void axisPositionChanged(int track, int posPercent);
    void stopped(bool normalEnd);
    void armedChanged(bool armed);
    void runningChanged(bool running);
    void transportStalled();
    // One line per statistics window: command count, worst tick gap and worst
    // phase lag (how late a command was emitted relative to its ideal moment).
    void linkStats(int windowMs, int commands, int maxTickGapMs, int maxPhaseLagMs);

private:
    void startWorker();
    // Runs inside the worker thread: creates the transport/session/scheduler
    // there, so no QObject and no QSerialPort is ever born on the GUI thread.
    void buildWorkerObjects();
    void teardownWorkerObjects();

    void onConnectionChanged(bool connected, const QString& message);
    void onCapabilitiesChanged(const DeviceCapabilities& capabilities);

    IMediaClock* m_clock = nullptr;
    ITCodeTransport* m_injectedTransport = nullptr;
    QThread* m_thread = nullptr;
    QObject* m_workerContext = nullptr;
    ITCodeTransport* m_transport = nullptr;      // lives in the worker thread
    DeviceSession* m_session = nullptr;          // lives in the worker thread
    MotionScheduler* m_scheduler = nullptr;      // lives in the worker thread

    // Cached GUI-side state.
    bool m_connected = false;
    bool m_running = false;
    bool m_armed = true;
    bool m_axesConfirmed = false;
    DeviceCapabilities m_capabilities;
    int m_seekRamp = 30;
    bool m_seekSafety = true;
    bool m_playPauseEasing = true;

    // Latest-wins seek coalescing.
    qint64 m_latestSeekMs = 0;
    bool m_seekFlushPending = false;
};
