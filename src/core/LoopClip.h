#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

// One A-B segment the user collected from a video. Plain data only: the store,
// the playback logic and the unit tests all depend on this header without
// pulling in any UI or mpv type.
struct LoopClip
{
    QString id;
    QString mediaPath;
    qint64 startMs = 0;
    qint64 endMs = 0;
    QString title;
    QStringList tags;
    QStringList folderIds;
    QString note;
    // Cache key of the frame grabbed at the A point; recomputable, but stored
    // so a future relocation of the media file can invalidate it explicitly.
    QString thumbKey;
    QDateTime created;
    QDateTime updated;

    qint64 durationMs() const { return endMs - startMs; }
};

// A folder of loop clips. A clip may belong to several folders at once, just
// like a media file may sit in several favourite folders.
struct LoopFolder
{
    QString id;
    QString name;
    QDateTime created;
};

// Pure helpers around the A-B range. They live next to the data so both the
// UI and the tests use exactly the same rules.
namespace LoopClipRules
{
constexpr qint64 kMinClipDurationMs = 200;
// Manual seeks inside this margin keep the loop alive; a seek outside ends it.
constexpr qint64 kExitToleranceMs = 300;

// Swaps A/B when the user marked the two points the other way round.
void normalizeRange(qint64* startMs, qint64* endMs);

// Empty string means accepted, otherwise the reason shown to the user.
// mediaDurationMs <= 0 skips the "inside the video" check.
QString validateRange(qint64 startMs, qint64 endMs, qint64 mediaDurationMs = 0);
QString validateClip(const LoopClip& clip, qint64 mediaDurationMs = 0);

// True when playback has reached (or passed) the B point and the loop has to
// jump back to A. mpv polls every 50 ms, so B is allowed to be overshot.
bool shouldRewind(qint64 positionMs, qint64 startMs, qint64 endMs);

// True when a user initiated seek left the A-B range plus its tolerance.
bool isOutsideRange(qint64 positionMs, qint64 startMs, qint64 endMs);

QString formatTimecode(qint64 ms);
bool parseTimecode(const QString& text, qint64* ms);

QString defaultTitle(const QString& mediaPath, qint64 startMs, qint64 endMs);
}
