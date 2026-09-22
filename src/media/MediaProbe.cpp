#include "core/Loc.h"
#include "MediaProbe.h"

#include <mpv/client.h>

#include <QDir>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPainter>
#include <QTemporaryDir>
#include <QThread>

namespace
{
constexpr int kThumbnailWidth = 400;
constexpr int kThumbnailHeight = 225;
constexpr int kThumbnailQuality = 85;
constexpr int kBlackLumaThreshold = 16;

const QStringList& coverBaseNames()
{
    static const QStringList names = {
        QStringLiteral("poster"), QStringLiteral("cover"), QStringLiteral("folder"),
        QStringLiteral("fanart"), QStringLiteral("thumb")
    };
    return names;
}

const QStringList& coverExtensions()
{
    static const QStringList extensions = {
        QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
        QStringLiteral("webp"), QStringLiteral("bmp")
    };
    return extensions;
}

QString stringProperty(mpv_handle* mpv, const char* name)
{
    char* value = mpv_get_property_string(mpv, name);
    if (!value)
        return QString();
    const QString result = QString::fromUtf8(value);
    mpv_free(value);
    return result;
}

qint64 intProperty(mpv_handle* mpv, const char* name, qint64 fallback = 0)
{
    qint64 value = 0;
    if (mpv_get_property(mpv, name, MPV_FORMAT_INT64, &value) < 0)
        return fallback;
    return value;
}

double doubleProperty(mpv_handle* mpv, const char* name, double fallback = 0.0)
{
    double value = 0.0;
    if (mpv_get_property(mpv, name, MPV_FORMAT_DOUBLE, &value) < 0)
        return fallback;
    return value;
}

bool cancelled(const MediaProbeOptions& options)
{
    return options.cancel && options.cancel->load();
}

bool waitForLoaded(mpv_handle* mpv, int timeoutMs, const MediaProbeOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs)
    {
        if (cancelled(options))
            return false;
        mpv_event* event = mpv_wait_event(mpv, 0.05);
        if (event->event_id == MPV_EVENT_FILE_LOADED)
            return true;
        if (event->event_id == MPV_EVENT_END_FILE)
            return false;
    }
    return false;
}

QString findFrameFile(const QString& directory)
{
    const QStringList files = QDir(directory).entryList(
        {QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.png")},
        QDir::Files, QDir::Name);
    return files.isEmpty() ? QString()
                           : QDir(directory).absoluteFilePath(files.first());
}

QString waitForFrameFile(const QString& directory, int timeoutMs,
                         const MediaProbeOptions& options)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs)
    {
        if (cancelled(options))
            return QString();
        const QString file = findFrameFile(directory);
        if (!file.isEmpty())
            return file;
        QThread::msleep(30);
    }
    return QString();
}

double seekSecondsFor(qint64 durationMs, double ratio)
{
    const double durationSeconds = durationMs / 1000.0;
    if (durationSeconds <= 0.0)
        return 0.0;

    // Keep the sampled position proportional on short clips too: clamping
    // every ratio to the same "middle of a short clip" made regenerated
    // thumbnails identical.
    double seconds = durationSeconds * ratio;
    const double minimum = std::min(5.0, durationSeconds * 0.5);
    if (seconds < minimum)
        seconds = minimum;
    if (seconds > durationSeconds - 0.5)
        seconds = std::max(0.0, durationSeconds - 0.5);
    return seconds;
}

bool applyCommonOptions(mpv_handle* mpv, const QString& imageDir, bool renderFrame,
                        double startSeconds)
{
    const auto setOption = [mpv](const char* name, const char* value) {
        return mpv_set_option_string(mpv, name, value) >= 0;
    };

    setOption("idle", "yes");
    setOption("keep-open", "yes");
    setOption("pause", "yes");
    setOption("hr-seek", "yes");
    setOption("ao", "null");
    setOption("terminal", "no");
    setOption("msg-level", "all=no");
    setOption("osc", "no");
    setOption("input-default-bindings", "no");
    setOption("input-vo-keyboard", "no");

    if (renderFrame)
    {
        // Decoding has to actually run for --frames/--start to take effect.
        setOption("pause", "no");

        if (!setOption("vo", "image"))
        {
            qWarning() << "mpv: image video output is unavailable";
            return false;
        }

        // VO sub-options are registered under their full names in libmpv; the
        // short forms exist on the mpv command line only.
        const QByteArray directory = imageDir.toUtf8();
        if (!setOption("vo-image-outdir", directory.constData())
            && !setOption("image-outdir", directory.constData()))
        {
            qWarning() << "mpv: cannot configure the image output directory";
            return false;
        }
        if (!setOption("vo-image-format", "jpg"))
            setOption("image-format", "jpg");

        qint64 frames = 1;
        const int frameResult = mpv_set_option(mpv, "frames", MPV_FORMAT_INT64, &frames);
        if (frameResult < 0)
            qWarning() << "mpv option failed: frames" << mpv_error_string(frameResult);

        // --start decides where the *first* decoded frame comes from. Seeking
        // after load is too late: with frames=1 mpv already wrote frame 0.
        if (startSeconds > 0.0)
        {
            const QByteArray start = QString::number(startSeconds, 'f', 3).toUtf8();
            setOption("start", start.constData());
        }
    }
    else
    {
        setOption("vo", "null");
    }
    return true;
}

MediaProbeResult readMetadata(mpv_handle* mpv)
{
    MediaProbeResult result;
    const double duration = doubleProperty(mpv, "duration");
    result.durationMs = static_cast<qint64>(duration * 1000.0);

    qint64 width = intProperty(mpv, "video-params/w");
    qint64 height = intProperty(mpv, "video-params/h");
    if (width <= 0 || height <= 0)
    {
        width = intProperty(mpv, "width");
        height = intProperty(mpv, "height");
    }
    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);

    result.container = stringProperty(mpv, "file-format");
    result.videoCodec = stringProperty(mpv, "video-codec");
    if (result.videoCodec.isEmpty())
        result.videoCodec = stringProperty(mpv, "video-codec-name");
    result.audioCodec = stringProperty(mpv, "audio-codec");
    if (result.audioCodec.isEmpty())
        result.audioCodec = stringProperty(mpv, "audio-codec-name");

    result.metadataOk = result.durationMs > 0 || result.width > 0;
    return result;
}

// Loads the file and optionally seeks to a ratio of the duration and renders a
// single frame into imageDir.
MediaProbeResult runProbe(const QString& mediaPath, const QString& imageDir,
                          bool renderFrame, double startSeconds,
                          const MediaProbeOptions& options)
{
    MediaProbeResult result;
    mpv_handle* mpv = mpv_create();
    if (!mpv)
    {
        result.error = LT("无法创建 mpv 实例");
        return result;
    }

    if (!applyCommonOptions(mpv, imageDir, renderFrame, startSeconds))
    {
        mpv_terminate_destroy(mpv);
        result.error = LT("无法配置 mpv 输出");
        return result;
    }
    if (mpv_initialize(mpv) < 0)
    {
        mpv_terminate_destroy(mpv);
        result.error = LT("mpv 初始化失败");
        return result;
    }

    const QByteArray path = mediaPath.toUtf8();
    const char* loadCommand[] = {"loadfile", path.constData(), nullptr};
    mpv_command(mpv, loadCommand);

    if (!waitForLoaded(mpv, options.timeoutMs, options))
    {
        mpv_terminate_destroy(mpv);
        result.error = LT("加载媒体超时");
        return result;
    }

    result = readMetadata(mpv);

    if (renderFrame)
    {
        const QString framePath = waitForFrameFile(imageDir, options.timeoutMs, options);
        if (framePath.isEmpty())
            result.error = LT("未生成缩略图");

        // Frame size is most reliable once a frame has been decoded.
        const MediaProbeResult afterFrame = readMetadata(mpv);
        if (afterFrame.width > 0 && afterFrame.height > 0)
        {
            result.width = afterFrame.width;
            result.height = afterFrame.height;
        }
        if (result.durationMs <= 0)
            result.durationMs = afterFrame.durationMs;
        if (result.videoCodec.isEmpty())
            result.videoCodec = afterFrame.videoCodec;
        result.metadataOk = result.metadataOk || afterFrame.metadataOk;
    }

    mpv_terminate_destroy(mpv);
    return result;
}
}

namespace MediaProbe
{
QString findLocalCover(const QString& mediaPath)
{
    const QFileInfo media(mediaPath);
    const QDir directory(media.absolutePath());

    QStringList bases;
    bases.append(media.completeBaseName());
    bases.append(coverBaseNames());

    for (const QString& base : bases)
    {
        for (const QString& extension : coverExtensions())
        {
            const QString candidate = directory.absoluteFilePath(
                base + QLatin1Char('.') + extension);
            if (QFileInfo::exists(candidate))
                return candidate;
        }
    }
    return QString();
}

bool composeThumbnail(const QImage& source, const QString& outputPath)
{
    if (source.isNull())
        return false;

    QImage canvas(kThumbnailWidth, kThumbnailHeight, QImage::Format_RGB32);
    canvas.fill(Qt::black);

    const QImage scaled = source.scaled(kThumbnailWidth, kThumbnailHeight,
                                        Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter painter(&canvas);
    painter.drawImage((kThumbnailWidth - scaled.width()) / 2,
                      (kThumbnailHeight - scaled.height()) / 2, scaled);
    painter.end();

    const QFileInfo output(outputPath);
    QDir().mkpath(output.absolutePath());
    return canvas.save(outputPath, "JPEG", kThumbnailQuality);
}

bool looksBlack(const QImage& image)
{
    if (image.isNull())
        return true;

    const QImage small = image.scaled(16, 9, Qt::IgnoreAspectRatio,
                                      Qt::FastTransformation)
                             .convertToFormat(QImage::Format_RGB32);
    qint64 sum = 0;
    for (int y = 0; y < small.height(); ++y)
    {
        const QRgb* line = reinterpret_cast<const QRgb*>(small.constScanLine(y));
        for (int x = 0; x < small.width(); ++x)
        {
            const QRgb pixel = line[x];
            sum += (qRed(pixel) * 299 + qGreen(pixel) * 587 + qBlue(pixel) * 114) / 1000;
        }
    }
    const int pixels = small.width() * small.height();
    return pixels > 0 && (sum / pixels) < kBlackLumaThreshold;
}

MediaProbeResult probe(const QString& mediaPath, const MediaProbeOptions& options)
{
    MediaProbeResult result;
    if (cancelled(options))
    {
        result.error = LT("已取消");
        return result;
    }
    if (!QFileInfo::exists(mediaPath))
    {
        result.error = LT("文件不存在");
        return result;
    }

    const bool wantsThumbnail = !options.thumbnailPath.isEmpty();
    // Reported back so a metadata-only probe (an already cached cover is kept)
    // cannot be mistaken for a failed thumbnail render.
    result.thumbnailRequested = wantsThumbnail;
    const QString localCover = (wantsThumbnail && !options.ignoreLocalCover)
                                   ? findLocalCover(mediaPath)
                                   : QString();
    const bool needsFrame = wantsThumbnail && localCover.isEmpty();

    const QString tempBase = options.temporaryRoot.isEmpty()
                                 ? QDir::tempPath()
                                 : QDir(options.temporaryRoot).absolutePath();
    QDir().mkpath(tempBase);

    QTemporaryDir scratch(QDir(tempBase).absoluteFilePath(QStringLiteral("probe-XXXXXX")));
    if (!scratch.isValid())
    {
        result.error = LT("无法创建临时目录");
        return result;
    }

    const double ratio = options.seekRatio >= 0.0 ? options.seekRatio : 0.1;

    // Pass 1: metadata only. Knowing the duration first lets the frame pass
    // position the decoder with --start.
    result = runProbe(mediaPath, scratch.path(), false, -1.0, options);

    if (needsFrame)
    {
        const double startSeconds = result.metadataOk
                                        ? seekSecondsFor(result.durationMs, ratio)
                                        : -1.0;

        QTemporaryDir frameDir(QDir(tempBase).absoluteFilePath(
            QStringLiteral("frame-XXXXXX")));
        if (!frameDir.isValid())
        {
            result.error = LT("无法创建临时目录");
            return result;
        }

        const MediaProbeResult frameResult =
            runProbe(mediaPath, frameDir.path(), true, startSeconds, options);
        if (frameResult.metadataOk)
        {
            if (frameResult.width > 0)
                result.width = frameResult.width;
            if (frameResult.height > 0)
                result.height = frameResult.height;
            if (result.durationMs <= 0)
                result.durationMs = frameResult.durationMs;
            if (result.videoCodec.isEmpty())
                result.videoCodec = frameResult.videoCodec;
            result.metadataOk = true;
        }

        QImage frame(findFrameFile(frameDir.path()));

        // The chosen position can be an intro card or a fade; try once more
        // deeper in the file before giving up.
        if (!frame.isNull() && looksBlack(frame) && result.durationMs > 0)
        {
            QTemporaryDir retryDir(QDir(tempBase).absoluteFilePath(
                QStringLiteral("frame-XXXXXX")));
            if (retryDir.isValid())
            {
                runProbe(mediaPath, retryDir.path(), true,
                         seekSecondsFor(result.durationMs, 0.4), options);
                const QImage retryFrame(findFrameFile(retryDir.path()));
                if (!retryFrame.isNull() && !looksBlack(retryFrame))
                    frame = retryFrame;
            }
        }

        if (!frame.isNull())
            composeThumbnail(frame, options.thumbnailPath);
    }

    if (!localCover.isEmpty())
    {
        const QImage cover(localCover);
        if (!cover.isNull() && composeThumbnail(cover, options.thumbnailPath))
            result.error.clear();
    }

    result.thumbnailOk = wantsThumbnail
                         && QFileInfo::exists(options.thumbnailPath)
                         && QFileInfo(options.thumbnailPath).size() > 0;
    return result;
}

MediaProbeResult captureFrameAt(const QString& mediaPath, qint64 positionMs,
                                const QString& outputPath, const QString& temporaryRoot,
                                int timeoutMs, std::shared_ptr<std::atomic<bool>> cancel)
{
    MediaProbeOptions options;
    options.thumbnailPath = outputPath;
    options.temporaryRoot = temporaryRoot;
    options.timeoutMs = timeoutMs;
    options.ignoreLocalCover = true;
    options.cancel = std::move(cancel);

    MediaProbeResult result;
    if (mediaPath.isEmpty() || outputPath.isEmpty())
    {
        result.error = LT("参数无效");
        return result;
    }
    if (!QFileInfo::exists(mediaPath))
    {
        result.error = LT("文件不存在");
        return result;
    }

    const QString tempBase = options.temporaryRoot.isEmpty()
                                 ? QDir::tempPath()
                                 : QDir(options.temporaryRoot).absolutePath();
    QDir().mkpath(tempBase);

    QTemporaryDir scratch(
        QDir(tempBase).absoluteFilePath(QStringLiteral("loop-probe-XXXXXX")));
    if (!scratch.isValid())
    {
        result.error = LT("无法创建临时目录");
        return result;
    }

    // Pass 1: metadata only, so the requested position can be clamped against
    // the real duration before the decoder is asked for a frame.
    result = runProbe(mediaPath, scratch.path(), false, -1.0, options);

    qint64 target = std::max<qint64>(0, positionMs);
    if (result.durationMs > 0)
        target = std::min(target, std::max<qint64>(0, result.durationMs - 200));
    const double startSeconds = target / 1000.0;

    QTemporaryDir frameDir(
        QDir(tempBase).absoluteFilePath(QStringLiteral("loop-frame-XXXXXX")));
    if (!frameDir.isValid())
    {
        result.error = LT("无法创建临时目录");
        return result;
    }

    const MediaProbeResult frameResult =
        runProbe(mediaPath, frameDir.path(), true, startSeconds, options);
    if (frameResult.metadataOk)
    {
        if (result.durationMs <= 0)
            result.durationMs = frameResult.durationMs;
        if (result.width <= 0)
            result.width = frameResult.width;
        if (result.height <= 0)
            result.height = frameResult.height;
        result.metadataOk = true;
    }

    const QImage frame(findFrameFile(frameDir.path()));
    if (!frame.isNull())
        composeThumbnail(frame, outputPath);

    const QFileInfo written(outputPath);
    result.thumbnailOk = written.exists() && written.size() > 0;
    if (!result.thumbnailOk && result.error.isEmpty())
        result.error = LT("未能截取画面");
    return result;
}
}
