#include "MpvEngine.h"

#include "core/AppLogger.h"

#include <mpv/client.h>

#include <QByteArray>
#include <QFileInfo>
#include <QtGlobal>

#include <algorithm>

namespace
{
// The scheduler extrapolates between mpv polls (every 50 ms) so that it can
// tick at 2 ms. That extrapolation has to stay bounded: while the decoder is
// stalled mpv still reports "not paused", and an unbounded clock would run
// ahead of the picture and drag the device with it.
constexpr qint64 kMaxExtrapolationMs = 120;
// No fresh position for this long means the GUI thread or the decoder is
// stalled. Hold the last known position instead of guessing forward; the next
// real sample then shows the true jump, which the scheduler realigns on.
constexpr qint64 kStalePositionMs = 500;
// After a seek mpv may keep reporting the pre-seek position for a few frames.
// Treat a sample as "still settling" while it is further than this from the
// requested target, and give up waiting after the timeout so a failed seek
// cannot freeze the clock.
constexpr qint64 kSeekSettleToleranceMs = 150;
constexpr qint64 kSeekSettleTimeoutMs = 400;

qint64 steadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}

MpvEngine::MpvEngine(QObject* parent)
    : IMediaClock(parent)
{
    m_pollTimer.setInterval(50);
    connect(&m_pollTimer, &QTimer::timeout, this, &MpvEngine::poll);
}

MpvEngine::~MpvEngine()
{
    if (m_mpv)
    {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
}

const char* MpvEngine::hardwareDecodingName(HardwareDecoding mode)
{
    switch (mode)
    {
    case HardwareDecoding::PreferHardware:
        return "auto";
    case HardwareDecoding::Software:
        return "no";
    case HardwareDecoding::Auto:
        break;
    }
    return "auto-safe";
}

void MpvEngine::setHardwareDecoding(HardwareDecoding mode)
{
    m_hwdecMode = mode;
    if (!m_mpv)
        return; // initialize() applies it before mpv comes up

    // mpv accepts hwdec changes at runtime; the decoder is re-created for the
    // next file, so the current playback is not interrupted.
    mpv_set_property_string(m_mpv, "hwdec", hardwareDecodingName(mode));
    AppLogger::log(QStringLiteral("mpv"),
                   QStringLiteral("解码方式改为 %1（下次打开视频时生效）")
                       .arg(QString::fromLatin1(hardwareDecodingName(mode))));
}

bool MpvEngine::initialize()
{
    m_mpv = mpv_create();
    if (!m_mpv)
        return false;

    mpv_set_option_string(m_mpv, "vo", "libmpv");
    // Hardware decoding keeps the CPU free and 4K/HEVC comfortable, but every
    // interop flavour available here renders one uninitialised frame while the
    // window changes size (measured with auto-safe, d3d11va-copy and dxva2-copy;
    // software decoding does not). The user-facing choice lives in the settings
    // menu; NEKOBEAT_HWDEC still overrides it for diagnostics.
    const QByteArray hwdecOverride = qgetenv("NEKOBEAT_HWDEC");
    const QByteArray hwdec = hwdecOverride.isEmpty()
                                 ? QByteArray(hardwareDecodingName(m_hwdecMode))
                                 : hwdecOverride;
    mpv_set_option_string(m_mpv, "hwdec", hwdec.constData());
    mpv_set_option_string(m_mpv, "idle", "yes");
    // Keep the file open at EOF so "eof-reached" stays readable; without this
    // the property is reset when mpv goes idle and the end of playback is
    // never reported (auto-next, device re-centering).
    mpv_set_option_string(m_mpv, "keep-open", "yes");
    mpv_set_option_string(m_mpv, "background-color", "#000000");
    mpv_set_option_string(m_mpv, "hr-seek", "yes");
    // Render the frame when it is actually due instead of rendering it early and
    // then sleeping until its target time. The sleeping is what used to block the
    // GUI thread for ~30 ms per frame; with the offset at 0 there is nothing to
    // wait for, so the call returns immediately *and* mpv keeps its own
    // presentation scheduling (see MpvVideoWidget::paintGL - 0.6.4 additionally
    // passed BLOCK_FOR_TARGET_TIME=0, which tells mpv the caller drives the render
    // loop and stops it announcing new frames).
    mpv_set_option_string(m_mpv, "video-timing-offset", "0.000");
    mpv_set_option_string(m_mpv, "osc", "no");
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "input-vo-keyboard", "no");
    mpv_set_option_string(m_mpv, "terminal", "no");
    mpv_set_option_string(m_mpv, "msg-level", "all=warn");
    // Pin the scalers instead of letting mpv pick them per scaling ratio. With
    // the default ("auto") mpv swaps between scalers when the ratio crosses a
    // threshold, and every swap is a shader recompile inside the render call -
    // which happens on the GUI thread, exactly when the window goes full screen
    // or a divider is dragged. That recompile is what made the picture (and the
    // device driven from the same thread) hitch. Fixed scalers never change, so
    // resizing only updates uniforms. 1080p content on a 1080p screen is 1:1
    // anyway, so the visible quality is unchanged there.
    const QList<QPair<QString, QString>> scalerOptions = {
        {QStringLiteral("scale"), QStringLiteral("bilinear")},
        {QStringLiteral("cscale"), QStringLiteral("bilinear")},
        {QStringLiteral("dscale"), QStringLiteral("mitchell")},
        {QStringLiteral("tscale"), QStringLiteral("oversample")},
    };
    for (const auto& option : scalerOptions)
    {
        if (mpv_set_option_string(m_mpv, option.first.toUtf8().constData(),
                                  option.second.toUtf8().constData())
            < 0)
        {
            AppLogger::log(QStringLiteral("mpv"),
                           QStringLiteral("缩放器选项被 mpv 拒绝: %1=%2（回退到默认）")
                               .arg(option.first, option.second));
        }
    }

    if (mpv_initialize(m_mpv) < 0)
        return false;

    // Log messages only reach the event queue once they are requested; poll()
    // forwards them into the application log.
    mpv_request_log_messages(m_mpv, "warn");

    // State is taken from mpv's own notifications instead of polling it with
    // mpv_get_property(). A property read is a round trip into mpv's playback
    // core and has to wait for the core lock, which the core can hold for a long
    // time while it re-configures its video chain - measured at 228 ms right
    // after a window resize, i.e. exactly when the picture goes full screen or
    // comes back. That wait runs on the GUI thread (which also drives the
    // device), so it showed up as "the picture and the machine both hitch".
    // Notifications are delivered through the event queue, which poll() drains
    // without touching the core lock.
    struct ObservedProperty
    {
        int id;
        const char* name;
        mpv_format format;
    };
    const ObservedProperty observed[] = {
        {kPropertyPosition, "time-pos", MPV_FORMAT_DOUBLE},
        {kPropertyDuration, "duration", MPV_FORMAT_DOUBLE},
        {kPropertyPaused, "pause", MPV_FORMAT_FLAG},
        {kPropertyEofReached, "eof-reached", MPV_FORMAT_FLAG},
    };
    for (const ObservedProperty& entry : observed)
    {
        if (mpv_observe_property(m_mpv, static_cast<uint64_t>(entry.id),
                                 entry.name, entry.format) < 0)
        {
            AppLogger::log(QStringLiteral("mpv"),
                           QStringLiteral("无法订阅属性 %1，状态可能不会更新")
                               .arg(QString::fromLatin1(entry.name)));
        }
    }

    m_pollTimer.start();
    return true;
}

qint64 MpvEngine::positionMs() const
{
    const qint64 lastPosition = m_positionMs.load();
    if (!m_playing.load())
        return lastPosition;

    const qint64 lastUpdate = m_lastUpdateNs.load();
    if (lastUpdate <= 0)
        return lastPosition;

    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const qint64 nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    const qint64 realElapsedMs = (nowNs - lastUpdate) / 1000000;
    if (realElapsedMs > kStalePositionMs)
        return lastPosition;

    const qint64 mediaElapsedMs = static_cast<qint64>(realElapsedMs * m_speed.load());
    return lastPosition + std::clamp<qint64>(mediaElapsedMs, 0, kMaxExtrapolationMs);
}

qint64 MpvEngine::durationMs() const
{
    return m_durationMs.load();
}

bool MpvEngine::isPlaying() const
{
    return m_playing.load();
}

QString MpvEngine::hwdecCurrent() const
{
    if (!m_mpv)
        return QString();

    char* value = mpv_get_property_string(m_mpv, "hwdec-current");
    if (!value)
        return QString();
    const QString result = QString::fromUtf8(value);
    mpv_free(value);
    return result;
}

bool MpvEngine::hasVideoOutput() const
{
    if (!m_mpv)
        return false;
    int configured = 0;
    if (mpv_get_property(m_mpv, "vo-configured", MPV_FORMAT_FLAG, &configured) < 0)
        return false;
    return configured != 0;
}

void MpvEngine::loadFile(const QString& path)
{
    if (!m_mpv)
        return;

    m_positionMs.store(0);
    m_durationMs.store(0);
    m_lastUpdateNs.store(0);
    m_seekTargetMs.store(-1);
    m_seekGuardUntilNs.store(0);
    m_playing.store(false);
    m_paused = false;
    m_eofEmitted = false;
    m_autoPlayPending = true;
    m_hwdecLogged = false;
    AppLogger::log(QStringLiteral("mpv"), QStringLiteral("loadFile: %1").arg(path));
    emit playingChanged(false);

    setProperty("pause", QStringLiteral("no"));
    const QByteArray utf8 = path.toUtf8();
    const char* command[] = {"loadfile", utf8.constData(), nullptr};
    mpv_command(m_mpv, command);
}

void MpvEngine::logHardwareDecoderOnce()
{
    if (m_hwdecLogged)
        return;
    m_hwdecLogged = true;

    // Which decoder ends up being used decides how a window resize behaves.
    // Worth a line so a "the picture flashed while going full screen" report
    // can be checked against the real mode instead of the requested one.
    const QString decoder = hwdecCurrent();
    AppLogger::log(QStringLiteral("mpv"),
                   QStringLiteral("硬件解码: %1")
                       .arg(decoder.isEmpty() ? QStringLiteral("(未知)") : decoder));
}

void MpvEngine::play()
{
    if (!m_mpv)
        return;

    m_autoPlayPending = false;
    AppLogger::log(QStringLiteral("mpv"), QStringLiteral("play"));
    int eofReached = 0;
    if (mpv_get_property(m_mpv, "eof-reached", MPV_FORMAT_FLAG, &eofReached) >= 0
        && eofReached)
    {
        seek(0);
    }
    setProperty("pause", QStringLiteral("no"));
}

void MpvEngine::pause()
{
    if (!m_mpv)
        return;
    AppLogger::log(QStringLiteral("mpv"), QStringLiteral("pause"));
    setProperty("pause", QStringLiteral("yes"));
}

void MpvEngine::seek(qint64 positionMs)
{
    if (!m_mpv)
        return;
    const QString value = QString::number(positionMs / 1000.0, 'f', 3);
    const QByteArray utf8 = value.toUtf8();
    const char* command[] = {"seek", utf8.constData(), "absolute", nullptr};
    mpv_command(m_mpv, command);
    m_positionMs.store(positionMs);
    m_seekTargetMs.store(positionMs);
    m_seekGuardUntilNs.store(steadyNowNs() + kSeekSettleTimeoutMs * 1000000LL);
    AppLogger::log(QStringLiteral("mpv"), QStringLiteral("seek: %1 ms").arg(positionMs));
    m_lastUpdateNs.store(steadyNowNs());
}

void MpvEngine::setVolume(int volume)
{
    if (!m_mpv)
        return;
    setProperty("volume", static_cast<double>(std::clamp(volume, 0, 100)));
}

void MpvEngine::setSpeed(double speed)
{
    if (!m_mpv)
        return;
    const double clamped = std::clamp(speed, 0.25, 4.0);
    m_speed.store(clamped);
    setProperty("speed", clamped);
}

void MpvEngine::stopPlayback()
{
    if (!m_mpv)
        return;
    const char* command[] = {"stop", nullptr};
    mpv_command(m_mpv, command);

    m_positionMs.store(0);
    m_durationMs.store(0);
    m_lastUpdateNs.store(0);
    m_seekTargetMs.store(-1);
    m_seekGuardUntilNs.store(0);
    m_playing.store(false);
    m_paused = false;
    m_eofEmitted = false;
    m_autoPlayPending = false;
    AppLogger::log(QStringLiteral("mpv"), QStringLiteral("stopPlayback"));
    emit positionChanged(0);
    emit durationChanged(0);
    emit playingChanged(false);
}

void MpvEngine::poll()
{
    if (!m_mpv)
        return;

    // libmpv requires the host to drain its event queue. This engine reads state
    // through the events mpv pushes (see the mpv_observe_property calls in
    // initialize()): a property read would be a round trip into the playback core
    // and block this thread for as long as the core holds its lock - measured at
    // 228 ms while mpv re-configured its video chain right after a window resize.
    // Draining the queue also keeps mpv's own messages (and every warning it
    // emits) visible in the application log.
    while (mpv_event* event = mpv_wait_event(m_mpv, 0))
    {
        if (event->event_id == MPV_EVENT_NONE)
            break;
        switch (event->event_id)
        {
        case MPV_EVENT_LOG_MESSAGE:
        {
            const auto* message = static_cast<mpv_event_log_message*>(event->data);
            AppLogger::log(QStringLiteral("mpv"),
                           QStringLiteral("%1: %2")
                               .arg(QString::fromUtf8(message->prefix),
                                    QString::fromUtf8(message->text).trimmed()));
            break;
        }
        case MPV_EVENT_PROPERTY_CHANGE:
        {
            const auto* property = static_cast<mpv_event_property*>(event->data);
            applyPropertyChange(static_cast<int>(event->reply_userdata),
                                static_cast<int>(property->format), property->data);
            break;
        }
        case MPV_EVENT_FILE_LOADED:
            // The decoder list is only populated once the file is open; asking
            // right after the load command always returned an empty string.
            logHardwareDecoderOnce();
            break;
        default:
            break;
        }
    }

    // The paused state and the playable duration are both inputs to "playing", so
    // a change in either one has to be re-evaluated - and so does a pending
    // auto-play, which waits for the file to report a duration.
    if (m_autoPlayPending && m_durationMs.load() > 0)
    {
        setProperty("pause", QStringLiteral("no"));
        m_autoPlayPending = false;
    }
    updatePlayingState();
}

void MpvEngine::applyPropertyChange(int propertyId, int format, void* data)
{
    if (!data)
    {
        // The property became unavailable (mpv went idle, or the file was
        // closed). Nothing to apply.
        return;
    }

    switch (propertyId)
    {
    case kPropertyPosition:
    {
        if (format != MPV_FORMAT_DOUBLE)
            return;
        const qint64 value = static_cast<qint64>(*static_cast<double*>(data) * 1000.0);
        const qint64 seekTarget = m_seekTargetMs.load();
        const qint64 nowNs = steadyNowNs();

        // For a few frames after a seek mpv can still report the old position.
        // Feeding that sample to the scheduler would drive the device back to
        // where it just left and then forward again, so it is ignored until the
        // reported position reaches the requested target. The timeout keeps a
        // seek that never lands from freezing the clock.
        const bool settling = seekTarget >= 0
                              && nowNs < m_seekGuardUntilNs.load()
                              && qAbs(value - seekTarget) > kSeekSettleToleranceMs;
        if (settling)
            return;

        if (seekTarget >= 0)
            m_seekTargetMs.store(-1);
        if (value != m_positionMs.load())
        {
            m_positionMs.store(value);
            m_lastUpdateNs.store(nowNs);
            emit positionChanged(value);
        }
        break;
    }
    case kPropertyDuration:
    {
        if (format != MPV_FORMAT_DOUBLE)
            return;
        const qint64 value = static_cast<qint64>(*static_cast<double*>(data) * 1000.0);
        if (value != m_durationMs.load())
        {
            m_durationMs.store(value);
            emit durationChanged(value);
        }
        break;
    }
    case kPropertyPaused:
    {
        if (format != MPV_FORMAT_FLAG)
            return;
        const bool paused = *static_cast<int*>(data) != 0;
        if (paused != m_paused)
        {
            m_paused = paused;
            updatePlayingState();
        }
        break;
    }
    case kPropertyEofReached:
    {
        if (format != MPV_FORMAT_FLAG)
            return;
        const bool eofReached = *static_cast<int*>(data) != 0;
        if (eofReached && !m_eofEmitted)
        {
            m_eofEmitted = true;
            AppLogger::log(QStringLiteral("mpv"), QStringLiteral("media ended"));
            updatePlayingState();
            emit mediaEnded();
        }
        else if (!eofReached)
        {
            m_eofEmitted = false;
        }
        break;
    }
    default:
        break;
    }
}

void MpvEngine::updatePlayingState()
{
    const bool playing = !m_paused && (m_durationMs.load() > 0) && !m_eofEmitted;
    if (playing == m_playing.load())
        return;

    m_playing.store(playing);
    AppLogger::log(QStringLiteral("mpv"),
                   QStringLiteral("playingChanged: %1")
                       .arg(playing ? QStringLiteral("true") : QStringLiteral("false")));
    emit playingChanged(playing);
}

void MpvEngine::setProperty(const char* name, const QString& value)
{
    mpv_set_property_string(m_mpv, name, value.toUtf8().constData());
}

void MpvEngine::setProperty(const char* name, double value)
{
    mpv_set_property(m_mpv, name, MPV_FORMAT_DOUBLE, &value);
}

void MpvEngine::setProperty(const char* name, qint64 value)
{
    mpv_set_property(m_mpv, name, MPV_FORMAT_INT64, &value);
}
