#pragma once

#include <QDateTime>
#include <QString>

enum class ThumbnailState
{
    Pending,
    Ready,
    Failed
};

// One video entry of the media library. The struct stays plain data so it can
// be produced by the scanner, cached in the JSON index and consumed by unit
// tests without any UI or mpv dependency.
struct MediaItem
{
    QString path;
    QString displayName;
    QString cacheKey;
    qint64 fileSize = 0;
    QDateTime modified;
    qint64 durationMs = 0;
    int width = 0;
    int height = 0;
    QString container;
    QString videoCodec;
    QString audioCodec;
    int scriptAxes = 0;
    // Script file the user picked for this video (empty = automatic).
    QString preferredScript;
    QString thumbnailPath;
    ThumbnailState thumbnailState = ThumbnailState::Pending;
    bool metadataReady = false;
    // The referenced file is gone. The entry is kept so the grid can show a
    // grey card instead of silently dropping the user's favourite, tag and
    // loop records. Cleared automatically once the file is back.
    bool missing = false;

    bool hasScript() const { return scriptAxes > 0; }
    bool hasMetadata() const { return metadataReady; }
};
