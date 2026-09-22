#include "core/Loc.h"
#include "core/LoopClip.h"

#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace
{
// mm:ss.mmm keeps the A/B fields short while still allowing frame level
// corrections inside the edit dialog.
QString formatTimecodeInternal(qint64 ms)
{
    const qint64 clamped = std::max<qint64>(0, ms);
    const qint64 minutes = clamped / 60000;
    const qint64 seconds = (clamped % 60000) / 1000;
    const qint64 millis = clamped % 1000;
    return QStringLiteral("%1:%2.%3")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(millis, 3, 10, QLatin1Char('0'));
}
}

void LoopClipRules::normalizeRange(qint64* startMs, qint64* endMs)
{
    if (!startMs || !endMs)
        return;
    if (*endMs < *startMs)
        std::swap(*startMs, *endMs);
}

QString LoopClipRules::validateRange(qint64 startMs, qint64 endMs, qint64 mediaDurationMs)
{
    if (startMs < 0 || endMs < 0)
        return LT("时间点不能为负数");
    if (endMs <= startMs)
        return LT("B 点必须晚于 A 点");
    if (endMs - startMs < kMinClipDurationMs)
        return LT("循环片段太短（至少 %1 毫秒）").arg(kMinClipDurationMs);
    if (mediaDurationMs > 0 && endMs > mediaDurationMs)
        return LT("结束时间超出视频长度");
    return QString();
}

QString LoopClipRules::validateClip(const LoopClip& clip, qint64 mediaDurationMs)
{
    if (clip.mediaPath.trimmed().isEmpty())
        return LT("片段没有关联视频");
    return validateRange(clip.startMs, clip.endMs, mediaDurationMs);
}

bool LoopClipRules::shouldRewind(qint64 positionMs, qint64 startMs, qint64 endMs)
{
    if (endMs <= startMs)
        return false;
    return positionMs >= endMs;
}

bool LoopClipRules::isOutsideRange(qint64 positionMs, qint64 startMs, qint64 endMs)
{
    if (endMs <= startMs)
        return true;
    return positionMs < startMs - kExitToleranceMs
           || positionMs > endMs + kExitToleranceMs;
}

QString LoopClipRules::formatTimecode(qint64 ms)
{
    return formatTimecodeInternal(ms);
}

bool LoopClipRules::parseTimecode(const QString& text, qint64* ms)
{
    if (!ms)
        return false;

    static const QRegularExpression pattern(
        QStringLiteral("^\\s*(?:(\\d+):)?(\\d{1,2})(?:[.,](\\d{1,3}))?\\s*$"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch())
        return false;

    bool ok = false;
    const qint64 minutes = match.captured(1).isEmpty()
                               ? 0
                               : match.captured(1).toLongLong(&ok);
    if (!ok && !match.captured(1).isEmpty())
        return false;
    const qint64 seconds = match.captured(2).toLongLong(&ok);
    if (!ok)
        return false;

    QString millisText = match.captured(3);
    while (millisText.size() < 3)
        millisText.append(QLatin1Char('0'));
    const qint64 millis = millisText.isEmpty() ? 0 : millisText.toLongLong(&ok);
    if (!ok && !millisText.isEmpty())
        return false;

    const qint64 total = minutes * 60000 + seconds * 1000 + millis;
    if (total < 0)
        return false;
    *ms = total;
    return true;
}

QString LoopClipRules::defaultTitle(const QString& mediaPath, qint64 startMs, qint64 endMs)
{
    const QString base = QFileInfo(mediaPath).completeBaseName().trimmed();
    const QString name = base.isEmpty() ? LT("视频片段") : base;
    return QStringLiteral("%1 · %2–%3")
        .arg(name, formatTimecodeInternal(startMs), formatTimecodeInternal(endMs));
}
