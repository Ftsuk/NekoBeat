#pragma once

#include "sync/IMediaClock.h"

#include <QObject>
#include <QString>
#include <QTimer>
#include <atomic>
#include <chrono>

struct mpv_handle;

class MpvEngine : public IMediaClock
{
    Q_OBJECT

public:
    // How much mpv is allowed to lean on the GPU for decoding. The interop
    // flavours (auto-safe / auto) can flash a light block while the window
    // changes size on some drivers, so "software" is the escape hatch users are
    // offered in the settings menu.
    enum class HardwareDecoding
    {
        Auto = 0,          // mpv auto-safe: hardware when possible, software otherwise
        PreferHardware = 1, // mpv auto: tries more backends, still falls back
        Software = 2       // mpv no: CPU decoding only
    };

    explicit MpvEngine(QObject* parent = nullptr);
    ~MpvEngine() override;

    // Before initialize() this only remembers the choice; afterwards it is
    // applied to the running instance (mpv re-initialises the decoder the next
    // time a file is opened, so playback is never interrupted).
    void setHardwareDecoding(HardwareDecoding mode);
    HardwareDecoding hardwareDecoding() const { return m_hwdecMode; }
    static const char* hardwareDecodingName(HardwareDecoding mode);

    bool initialize();
    mpv_handle* handle() const { return m_mpv; }

    qint64 positionMs() const override;
    qint64 durationMs() const override;
    bool isPlaying() const override;
    QString hwdecCurrent() const;
    // Whether mpv currently has a configured video output. It stays false for
    // the whole session when a file was opened before the host had a render
    // context ("No render context set"): audio keeps playing, the picture never
    // updates.
    bool hasVideoOutput() const;

    void loadFile(const QString& path);
    void play();
    void pause();
    void seek(qint64 positionMs);
    void setVolume(int volume);
    void setSpeed(double speed);
    void stopPlayback();

private slots:
    void poll();
    // Applies one mpv property notification (see the mpv_observe_property calls
    // in initialize()).
    void applyPropertyChange(int propertyId, int format, void* data);

private:
    void setProperty(const char* name, const QString& value);
    void setProperty(const char* name, double value);
    void setProperty(const char* name, qint64 value);
    // Reports the decoder mpv actually ended up using. It is only known once
    // the file is open, so this runs on MPV_EVENT_FILE_LOADED rather than right
    // after the load command (where the property is still empty).
    void logHardwareDecoderOnce();
    // Recomputes and publishes the playing state from the observed paused
    // flag, the duration and the end-of-file flag.
    void updatePlayingState();
    static constexpr int kPropertyPosition = 1;
    static constexpr int kPropertyDuration = 2;
    static constexpr int kPropertyPaused = 3;
    static constexpr int kPropertyEofReached = 4;

    mpv_handle* m_mpv = nullptr;
    QTimer m_pollTimer;
    std::atomic<qint64> m_positionMs{0};
    std::atomic<qint64> m_durationMs{0};
    std::atomic<qint64> m_lastUpdateNs{0};
    // While a seek is settling mpv can still report the old position. These
    // guard against feeding that stale sample to the scheduler.
    std::atomic<qint64> m_seekTargetMs{-1};
    std::atomic<qint64> m_seekGuardUntilNs{0};
    std::atomic<bool> m_playing{false};
    // Playback rate; the position extrapolation between polls has to follow it,
    // otherwise the device would be driven along a 1x timeline at other speeds.
    std::atomic<double> m_speed{1.0};
    bool m_paused = false;
    bool m_eofEmitted = false;
    bool m_autoPlayPending = false;
    bool m_hwdecLogged = false;
    HardwareDecoding m_hwdecMode = HardwareDecoding::Auto;
};
