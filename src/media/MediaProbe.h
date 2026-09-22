#pragma once

#include <QImage>
#include <QString>

#include <atomic>
#include <memory>

struct MediaProbeOptions
{
    // Empty means "metadata only, do not render a frame".
    QString thumbnailPath;
    // Directory used for temporary mpv frame output. System temp is used when
    // this is empty.
    QString temporaryRoot;
    int timeoutMs = 5000;
    // 0..1 position of the captured frame; negative means "pick 10%".
    double seekRatio = -1.0;
    // Regeneration ignores sidecar cover images so a real frame is captured.
    bool ignoreLocalCover = false;
    // Shared cancellation flag: the worker aborts quickly when it turns true.
    std::shared_ptr<std::atomic<bool>> cancel;
};

struct MediaProbeResult
{
    bool metadataOk = false;
    bool thumbnailOk = false;
    // False for a metadata-only probe (no thumbnail path was requested): the
    // caller must then leave an already cached cover exactly as it is.
    bool thumbnailRequested = true;
    qint64 durationMs = 0;
    int width = 0;
    int height = 0;
    QString container;
    QString videoCodec;
    QString audioCodec;
    QString error;
};

// Reads metadata and renders a single video frame through an isolated headless
// libmpv instance. The probe never touches the playback handle, so thumbnail
// generation can run while a video is playing.
namespace MediaProbe
{
MediaProbeResult probe(const QString& mediaPath, const MediaProbeOptions& options);

// Grabs the frame at an exact position. Unlike probe() this never falls back to
// a sidecar cover, never shifts the position to a "safe" 10 %, and never
// retries at 40 % - a loop thumbnail has to show the A point the user marked.
MediaProbeResult captureFrameAt(const QString& mediaPath, qint64 positionMs,
                                const QString& outputPath, const QString& temporaryRoot,
                                int timeoutMs = 8000,
                                std::shared_ptr<std::atomic<bool>> cancel = {});

// Looks for a sidecar cover image next to the video (video name, poster, cover,
// folder, fanart, thumb).
QString findLocalCover(const QString& mediaPath);

// Scales the frame into a 16:9 letterboxed JPEG so grid cards keep a uniform
// aspect ratio.
bool composeThumbnail(const QImage& source, const QString& outputPath);

bool looksBlack(const QImage& image);
}
