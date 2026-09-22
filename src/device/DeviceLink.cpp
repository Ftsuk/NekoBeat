#include "device/DeviceLink.h"

#include "core/AppLogger.h"
#include "device/DeviceSession.h"
#include "device/SerialTransport.h"
#include "sync/IMediaClock.h"
#include "sync/MotionScheduler.h"

#include <QMetaObject>
#include <QThread>

namespace
{
// Queued connections copy their arguments, which requires the types to be known
// to the meta-object system. Without this a cross-thread setBundle() or
// capabilitiesChanged() fails at runtime ("Cannot queue arguments of type ...").
void registerLinkMetaTypes()
{
    static const bool registered = [] {
        qRegisterMetaType<Track>("Track");
        qRegisterMetaType<AxisConfig>("AxisConfig");
        qRegisterMetaType<ScriptBundle>("ScriptBundle");
        qRegisterMetaType<DeviceCapabilities>("DeviceCapabilities");
        return true;
    }();
    Q_UNUSED(registered);
}
} // namespace

DeviceLink::DeviceLink(IMediaClock* clock, QObject* parent)
    : DeviceLink(clock, nullptr, parent)
{
}

DeviceLink::DeviceLink(IMediaClock* clock, ITCodeTransport* injectedTransport, QObject* parent)
    : IDeviceControl(parent)
    , m_clock(clock)
    , m_injectedTransport(injectedTransport)
{
    registerLinkMetaTypes();
    startWorker();
}

DeviceLink::~DeviceLink()
{
    shutdown();
}

void DeviceLink::startWorker()
{
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("NekoBeat-device"));

    // A context object living in the worker thread: invokeMethod() with
    // BlockingQueuedConnection then really runs on it, which is what makes the
    // worker objects (and the QSerialPort inside SerialTransport) be created
    // there instead of on the GUI thread.
    m_workerContext = new QObject;
    m_workerContext->moveToThread(m_thread);

    // An injected transport was created by the caller (test) in this thread, so
    // its affinity has to change here - moveToThread() may only be called from
    // the thread that currently owns the object.
    if (m_injectedTransport)
        m_injectedTransport->moveToThread(m_thread);

    m_thread->start();
    QMetaObject::invokeMethod(m_workerContext, [this] { buildWorkerObjects(); },
                              Qt::BlockingQueuedConnection);
}

void DeviceLink::buildWorkerObjects()
{
    // Runs in the worker thread.
    if (m_injectedTransport)
    {
        m_transport = m_injectedTransport;
    }
    else
    {
        m_transport = new SerialTransport();
    }

    m_session = new DeviceSession(m_transport);
    m_scheduler = new MotionScheduler(m_transport, m_clock);

    // Requests from the GUI. The context object of each connection lives in the
    // worker thread, so these are queued automatically.
    connect(this, &DeviceLink::connectRequested, m_session,
            [this](const QString& port, int baudRate) { m_session->connectToPort(port, baudRate); });
    connect(this, &DeviceLink::disconnectRequested, m_session,
            [this] { m_session->disconnectFromDevice(); });
    connect(this, &DeviceLink::releaseRequested, m_session, [this] {
        if (m_transport && m_transport->isOpen())
        {
            m_transport->writeLine(TCodeEncoder::stopCommand());
            m_transport->close();
        }
    });
    connect(this, &DeviceLink::rawLineRequested, m_session,
            [this](const QString& line) { m_session->sendLine(line); });

    connect(this, &DeviceLink::startRequested, m_scheduler,
            [this](bool deviceMayBeFar) { m_scheduler->start(deviceMayBeFar); });
    connect(this, &DeviceLink::pauseRequested, m_scheduler, [this] { m_scheduler->pause(); });
    connect(this, &DeviceLink::pauseEasedRequested, m_scheduler,
            [this] { m_scheduler->pauseEased(); });
    connect(this, &DeviceLink::seekRequested, m_scheduler,
            [this](qint64 mediaTimeMs) { m_scheduler->seek(mediaTimeMs); });
    connect(this, &DeviceLink::stopRequested, m_scheduler,
            [this](bool home) { m_scheduler->stop(home); });
    connect(this, &DeviceLink::panicRequested, m_scheduler, [this] { m_scheduler->panic(); });
    connect(this, &DeviceLink::rearmRequested, m_scheduler, [this] { m_scheduler->rearm(); });
    connect(this, &DeviceLink::bundleRequested, m_scheduler,
            [this](const ScriptBundle& bundle) { m_scheduler->setBundle(bundle); });
    connect(this, &DeviceLink::seekRampRequested, m_scheduler,
            [this](int value) { m_scheduler->setSeekRampMsPerPercent(value); });
    connect(this, &DeviceLink::seekSafetyRequested, m_scheduler,
            [this](bool enabled) { m_scheduler->setSeekSafetyEnabled(enabled); });
    connect(this, &DeviceLink::playPauseEasingRequested, m_scheduler,
            [this](bool enabled) { m_scheduler->setPlayPauseEasingEnabled(enabled); });
    connect(this, &DeviceLink::axesConfirmedRequested, m_scheduler,
            [this](bool confirmed) { m_scheduler->setDeviceAxesConfirmed(confirmed); });

    // Answers back to the GUI (queued, because this object lives in the GUI
    // thread while the emitters live here).
    connect(m_session, &DeviceSession::connectionChanged, this,
            &DeviceLink::onConnectionChanged);
    connect(m_session, &DeviceSession::capabilitiesChanged, this,
            &DeviceLink::onCapabilitiesChanged);
    connect(m_session, &DeviceSession::errorOccurred, this, &DeviceLink::errorOccurred);
    connect(m_scheduler, &MotionScheduler::axisPositionChanged, this,
            &DeviceLink::axisPositionChanged);
    connect(m_scheduler, &MotionScheduler::stopped, this, &DeviceLink::stopped);
    connect(m_scheduler, &MotionScheduler::armedChanged, this, [this](bool armed) {
        m_armed = armed;
        emit armedChanged(armed);
    });
    connect(m_scheduler, &MotionScheduler::runningChanged, this, [this](bool running) {
        m_running = running;
        emit runningChanged(running);
    });
    connect(m_scheduler, &MotionScheduler::transportStalled, this,
            &DeviceLink::transportStalled);
    connect(m_scheduler, &MotionScheduler::linkStats, this, &DeviceLink::linkStats);

    // The startup defaults have to reach the worker even before anyone calls
    // the setters, so the cached GUI values are pushed once here.
    m_scheduler->setSeekRampMsPerPercent(m_seekRamp);
    m_scheduler->setSeekSafetyEnabled(m_seekSafety);
    m_scheduler->setPlayPauseEasingEnabled(m_playPauseEasing);
    m_scheduler->setDeviceAxesConfirmed(m_axesConfirmed);
}

void DeviceLink::teardownWorkerObjects()
{
    // Runs in the worker thread.
    if (m_scheduler)
    {
        // Stop the machine first: the session destructor sends DSTOP as well,
        // but the timer has to be off before anything else is torn down.
        m_scheduler->panic();
        delete m_scheduler;
        m_scheduler = nullptr;
    }
    if (m_session)
    {
        delete m_session;
        m_session = nullptr;
    }
    if (m_transport && !m_injectedTransport)
    {
        delete m_transport;
    }
    m_transport = nullptr;
}

void DeviceLink::shutdown()
{
    if (!m_thread || !m_thread->isRunning())
        return;

    // The one synchronous call in this class: the application is closing, so
    // waiting for DSTOP and the port close is exactly what we want.
    if (m_workerContext)
    {
        QMetaObject::invokeMethod(m_workerContext, [this] { teardownWorkerObjects(); },
                                  Qt::BlockingQueuedConnection);
    }

    m_thread->quit();
    if (!m_thread->wait(3000))
        AppLogger::log(QStringLiteral("device"),
                       QStringLiteral("设备线程未在 3 秒内退出（可能有阻塞的串口写入）"));

    // The worker context has no parent; without this every DeviceLink leaks one
    // QObject.
    delete m_workerContext;
    m_workerContext = nullptr;

    // Nothing can be running any more; say so instead of leaving the cached
    // state (and the interface built on it) believing the link is still live.
    m_running = false;
    m_connected = false;
    m_axesConfirmed = false;
    m_capabilities = DeviceCapabilities{};
}

void DeviceLink::connectDevice(const QString& port, int baudRate)
{
    emit connectRequested(port, baudRate);
}

void DeviceLink::disconnectDevice()
{
    emit disconnectRequested();
}

void DeviceLink::releasePort()
{
    emit releaseRequested();
}

void DeviceLink::sendRawLine(const QString& line)
{
    emit rawLineRequested(line);
}

void DeviceLink::start(bool deviceMayBeFar)
{
    emit startRequested(deviceMayBeFar);
}

void DeviceLink::pause()
{
    emit pauseRequested();
}

void DeviceLink::pauseEased()
{
    emit pauseEasedRequested();
}

void DeviceLink::seek(qint64 mediaTimeMs)
{
    m_latestSeekMs = mediaTimeMs;
    if (m_seekFlushPending)
        return;

    // Coalescing on the GUI side of the queue: a burst of seeks (a blocked
    // interface catching up, a drag that produced several events in one turn)
    // turns into a single request, so the worker never builds a backlog of
    // stale targets.
    m_seekFlushPending = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_seekFlushPending = false;
            emit seekRequested(m_latestSeekMs);
        },
        Qt::QueuedConnection);
}

void DeviceLink::stop(bool home)
{
    emit stopRequested(home);
}

void DeviceLink::panic()
{
    emit panicRequested();
}

void DeviceLink::rearm()
{
    emit rearmRequested();
}

void DeviceLink::setBundle(const ScriptBundle& bundle)
{
    emit bundleRequested(bundle);
}

void DeviceLink::setSeekRampMsPerPercent(int value)
{
    m_seekRamp = value;
    emit seekRampRequested(value);
}

void DeviceLink::setSeekSafetyEnabled(bool enabled)
{
    m_seekSafety = enabled;
    emit seekSafetyRequested(enabled);
}

void DeviceLink::setPlayPauseEasingEnabled(bool enabled)
{
    m_playPauseEasing = enabled;
    emit playPauseEasingRequested(enabled);
}

void DeviceLink::setDeviceAxesConfirmed(bool confirmed)
{
    m_axesConfirmed = confirmed;
    emit axesConfirmedRequested(confirmed);
}

void DeviceLink::onConnectionChanged(bool connected, const QString& message)
{
    m_connected = connected;
    if (!connected)
    {
        // The port is gone, so the worker's scheduler has to stop too - not just
        // this cache. Otherwise its timer keeps running and the device would
        // silently resume as soon as a port is opened again.
        emit pauseRequested();
        // A closed port cannot run anything; reflect that immediately instead of
        // waiting for the scheduler's own state update.
        m_running = false;
        m_axesConfirmed = false;
        m_capabilities = DeviceCapabilities{};
    }
    emit connectionChanged(connected, message);
}

void DeviceLink::onCapabilitiesChanged(const DeviceCapabilities& capabilities)
{
    m_capabilities = capabilities;
    if (capabilities.probeComplete)
    {
        // The scheduler owns the authoritative flag; this cache only mirrors it
        // for the interface.
        m_axesConfirmed = true;
    }
    emit capabilitiesChanged(capabilities);
}
