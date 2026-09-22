#include "core/MediaLibraryStore.h"

#include "core/DurationFormat.h"
#include "core/FavoritesStore.h"
#include "core/ScriptLoader.h"
#include "core/Track.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

namespace
{
constexpr int kIndexVersion = 1;
constexpr auto kSettingsRootsKey = "library/roots";

QString normalizedPath(const QString& path)
{
    const QFileInfo info(path);
    return QDir::cleanPath(info.absoluteFilePath()).toLower();
}

QString thumbStateName(ThumbnailState state)
{
    switch (state)
    {
    case ThumbnailState::Ready:
        return QStringLiteral("ready");
    case ThumbnailState::Failed:
        return QStringLiteral("failed");
    case ThumbnailState::Pending:
        break;
    }
    return QStringLiteral("pending");
}

ThumbnailState thumbStateFromName(const QString& name)
{
    if (name == QStringLiteral("ready"))
        return ThumbnailState::Ready;
    if (name == QStringLiteral("failed"))
        return ThumbnailState::Failed;
    return ThumbnailState::Pending;
}

// Moves a thumbnail that was cached under the old path based key onto the new
// content based one. Without this an upgrade would regenerate every cover once.
void relocateThumbnailFile(const QString& oldPath, const QString& newPath)
{
    if (oldPath.isEmpty() || newPath.isEmpty() || oldPath == newPath)
        return;
    if (QFileInfo::exists(newPath) || !QFileInfo::exists(oldPath))
        return;
    QDir().mkpath(QFileInfo(newPath).absolutePath());
    QFile::rename(oldPath, newPath);
}

// A script the user picked explicitly travels with the video. Keep the old
// absolute path while it still resolves, otherwise reattach the same file name
// inside the video's new folder.
QString relocatedScriptPath(const QString& script, const QString& mediaPath)
{
    if (script.isEmpty())
        return QString();
    if (QFileInfo::exists(script))
        return script;

    const QString candidate = QDir(QFileInfo(mediaPath).absolutePath())
                                  .absoluteFilePath(QFileInfo(script).fileName());
    if (QFileInfo::exists(candidate))
        return candidate;

    // Keep the selection as it is. The script may live on a drive that is not
    // plugged in right now, and silently dropping the user's choice would be
    // worse than remembering a path that does not resolve yet.
    return script;
}
}

MediaLibraryStore::MediaLibraryStore(const QString& cacheRoot)
{
    if (!cacheRoot.isEmpty())
    {
        m_cacheRoot = QDir::cleanPath(cacheRoot);
        return;
    }

    // AppLocalDataLocation would repeat the organisation and application name
    // ("NekoBeat/NekoBeat"), so build the cache below the local data
    // root instead.
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    m_cacheRoot = QDir(base).absoluteFilePath(QStringLiteral("NekoBeat/library"));
}

QString MediaLibraryStore::thumbnailsDir() const
{
    return QDir(m_cacheRoot).absoluteFilePath(QStringLiteral("thumbs"));
}

QString MediaLibraryStore::temporaryDir() const
{
    return QDir(m_cacheRoot).absoluteFilePath(QStringLiteral("tmp"));
}

QString MediaLibraryStore::indexPath() const
{
    return QDir(m_cacheRoot).absoluteFilePath(QStringLiteral("index.json"));
}

void MediaLibraryStore::ensureDirectories() const
{
    QDir().mkpath(thumbnailsDir());
    QDir().mkpath(temporaryDir());
}

QString MediaLibraryStore::cacheKeyFor(const QString& mediaPath, qint64 fileSize,
                                       const QDateTime& modified) const
{
    // Deliberately free of the folder: reorganising the library must not throw
    // away a single thumbnail. Name + size + modification time still tells two
    // different videos with the same file name apart, which is exactly what a
    // pure file name key (as in XTPlayer) fails to do.
    const QString name = QFileInfo(mediaPath).fileName().toLower();
    const QString seed = name + QLatin1Char('|')
                         + QString::number(fileSize) + QLatin1Char('|')
                         + QString::number(modified.toMSecsSinceEpoch());
    return QString::fromLatin1(
        QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString MediaLibraryStore::legacyCacheKeyFor(const QString& mediaPath, qint64 fileSize,
                                             const QDateTime& modified)
{
    const QString seed = normalizedPath(mediaPath) + QLatin1Char('|')
                         + QString::number(fileSize) + QLatin1Char('|')
                         + QString::number(modified.toMSecsSinceEpoch());
    return QString::fromLatin1(
        QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString MediaLibraryStore::thumbnailPathFor(const QString& cacheKey) const
{
    return QDir(thumbnailsDir()).absoluteFilePath(cacheKey + QStringLiteral(".jpg"));
}

QHash<QString, MediaItem> MediaLibraryStore::loadIndex() const
{
    QHash<QString, MediaItem> items;
    QFile file(indexPath());
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return items;

    // Same guard as the script loader: never read a damaged index without a
    // bound on how much memory it can take.
    constexpr qint64 kMaxIndexBytes = 64 * 1024 * 1024;
    if (file.size() > kMaxIndexBytes)
        return items;

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return items;

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != kIndexVersion)
        return items;

    for (const QJsonValue& value : root.value(QStringLiteral("items")).toArray())
    {
        const QJsonObject object = value.toObject();
        const QString path = object.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            continue;

        MediaItem item;
        item.path = path;
        item.displayName = object.value(QStringLiteral("displayName")).toString();
        item.cacheKey = object.value(QStringLiteral("cacheKey")).toString();
        item.fileSize = static_cast<qint64>(
            object.value(QStringLiteral("fileSize")).toDouble());
        item.modified = QDateTime::fromString(
            object.value(QStringLiteral("modified")).toString(), Qt::ISODateWithMs);
        item.durationMs = static_cast<qint64>(
            object.value(QStringLiteral("durationMs")).toDouble());
        item.width = object.value(QStringLiteral("width")).toInt();
        item.height = object.value(QStringLiteral("height")).toInt();
        item.container = object.value(QStringLiteral("container")).toString();
        item.videoCodec = object.value(QStringLiteral("videoCodec")).toString();
        item.audioCodec = object.value(QStringLiteral("audioCodec")).toString();
        item.scriptAxes = object.value(QStringLiteral("scriptAxes")).toInt();
        item.preferredScript = object.value(QStringLiteral("preferredScript")).toString();
        item.thumbnailState = thumbStateFromName(
            object.value(QStringLiteral("thumbnailState")).toString());
        item.metadataReady = object.value(QStringLiteral("metadataReady")).toBool();
        items.insert(normalizedPath(path), item);
    }
    return items;
}

void MediaLibraryStore::saveIndex(const QList<MediaItem>& items) const
{
    ensureDirectories();

    QJsonArray array;
    for (const MediaItem& item : items)
    {
        QJsonObject object;
        object.insert(QStringLiteral("path"), item.path);
        object.insert(QStringLiteral("displayName"), item.displayName);
        object.insert(QStringLiteral("cacheKey"), item.cacheKey);
        object.insert(QStringLiteral("fileSize"), static_cast<double>(item.fileSize));
        object.insert(QStringLiteral("modified"),
                      item.modified.toString(Qt::ISODateWithMs));
        object.insert(QStringLiteral("durationMs"), static_cast<double>(item.durationMs));
        object.insert(QStringLiteral("width"), item.width);
        object.insert(QStringLiteral("height"), item.height);
        object.insert(QStringLiteral("container"), item.container);
        object.insert(QStringLiteral("videoCodec"), item.videoCodec);
        object.insert(QStringLiteral("audioCodec"), item.audioCodec);
        object.insert(QStringLiteral("scriptAxes"), item.scriptAxes);
        object.insert(QStringLiteral("preferredScript"), item.preferredScript);
        object.insert(QStringLiteral("thumbnailState"), thumbStateName(item.thumbnailState));
        object.insert(QStringLiteral("metadataReady"), item.metadataReady);
        array.append(object);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), kIndexVersion);
    root.insert(QStringLiteral("items"), array);

    // Atomic write. A plain truncate would leave a broken index.json behind on
    // a crash or a full disk, and rebuilding loses every remembered script
    // choice (preferredScript) along with the metadata cache.
    QSaveFile file(indexPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.commit();
}

void MediaLibraryStore::clearCache() const
{
    const QString thumbs = thumbnailsDir();
    const QString temp = temporaryDir();
    const QString root = QDir(m_cacheRoot).absolutePath();

    // Guard: only ever delete inside our own cache root.
    const bool thumbsInside = QDir(root).absoluteFilePath(QStringLiteral("thumbs"))
                              == QDir(thumbs).absolutePath();
    const bool tempInside = QDir(root).absoluteFilePath(QStringLiteral("tmp"))
                            == QDir(temp).absolutePath();
    if (!thumbsInside || !tempInside || root.isEmpty())
        return;

    QDir(thumbs).removeRecursively();
    QDir(temp).removeRecursively();
    QFile::remove(indexPath());
}

QList<MediaItem> MediaLibraryStore::scanRoots(const QStringList& roots) const
{
    const QHash<QString, MediaItem> cached = loadIndex();
    // The index is keyed by path, so a reorganised library would miss every
    // entry. This second index is keyed by the content based cache key, which
    // survives a move, and is what keeps thumbnails and probe results alive.
    QHash<QString, MediaItem> cachedByContent;
    for (auto it = cached.constBegin(); it != cached.constEnd(); ++it)
    {
        const MediaItem& item = it.value();
        if (item.path.isEmpty())
            continue;

        const QString contentKey = cacheKeyFor(item.path, item.fileSize, item.modified);
        if (!item.cacheKey.isEmpty() && item.cacheKey != contentKey)
        {
            relocateThumbnailFile(thumbnailPathFor(item.cacheKey),
                                  thumbnailPathFor(contentKey));
        }
        cachedByContent.insert(contentKey, item);
    }

    QHash<QString, MediaItem> merged;

    for (const QString& root : normalizeRoots(roots))
    {
        for (MediaItem item : scanRoot(root, cached, cachedByContent))
            merged.insert(normalizedPath(item.path), item);
    }

    return merged.values();
}

QList<MediaItem> MediaLibraryStore::scanRoot(
    const QString& root, const QHash<QString, MediaItem>& cached,
    const QHash<QString, MediaItem>& cachedByContent) const
{
    QList<MediaItem> items;
    const QDir rootDir(root);
    if (!rootDir.exists())
        return items;

    QDirIterator iterator(rootDir.absolutePath(), QDir::Files | QDir::Readable,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext())
    {
        const QString path = iterator.next();
        if (!isVideoFile(path))
            continue;

        const QFileInfo info(path);
        MediaItem item;
        item.path = info.absoluteFilePath();
        item.displayName = info.fileName();
        item.fileSize = info.size();
        item.modified = info.lastModified();
        item.cacheKey = cacheKeyFor(item.path, item.fileSize, item.modified);
        item.thumbnailPath = thumbnailPathFor(item.cacheKey);
        item.scriptAxes = scriptAxesFor(item.path);

        // Prefer the same path; fall back to the same content so a file that
        // moved between folders keeps its duration, codecs, script choice and
        // thumbnail instead of being probed again from scratch.
        const MediaItem* previous = nullptr;
        const auto samePath = cached.constFind(normalizedPath(item.path));
        if (samePath != cached.constEnd())
        {
            previous = &samePath.value();
        }
        else
        {
            const auto sameContent = cachedByContent.constFind(item.cacheKey);
            if (sameContent != cachedByContent.constEnd())
                previous = &sameContent.value();
        }

        if (previous)
        {
            // The script the user picked is remembered independently of the
            // metadata probe: otherwise every rescan (start, refresh, cache
            // clear) would silently drop the selection.
            item.preferredScript =
                relocatedScriptPath(previous->preferredScript, item.path);
            if (previous->fileSize == item.fileSize
                && previous->modified == item.modified
                && previous->metadataReady)
            {
                item.durationMs = previous->durationMs;
                item.width = previous->width;
                item.height = previous->height;
                item.container = previous->container;
                item.videoCodec = previous->videoCodec;
                item.audioCodec = previous->audioCodec;
                item.metadataReady = true;

                if (previous->thumbnailState == ThumbnailState::Ready
                    && QFileInfo::exists(item.thumbnailPath))
                {
                    item.thumbnailState = ThumbnailState::Ready;
                }
                else if (previous->thumbnailState == ThumbnailState::Failed)
                {
                    // Failed thumbnails are retried on the next session so a
                    // transient mpv error never sticks forever.
                    item.thumbnailState = ThumbnailState::Pending;
                }
            }
        }

        // The cover file itself is the other half of the evidence. The cache key
        // is built from name + size + modification time, so a cover that sits
        // under this key belongs to exactly this version of this file - no index
        // entry needed. Re-importing a folder the user removed earlier (which
        // drops its index entries) therefore picks the covers up again instead of
        // rendering all of them a second time.
        if (item.thumbnailState != ThumbnailState::Ready
            && QFileInfo::exists(item.thumbnailPath))
        {
            item.thumbnailState = ThumbnailState::Ready;
        }

        items.append(item);
    }
    return items;
}

QStringList MediaLibraryStore::loadRootFolders()
{
    QSettings settings;
    return normalizeRoots(settings.value(QString::fromLatin1(kSettingsRootsKey)).toStringList());
}

void MediaLibraryStore::saveRootFolders(const QStringList& roots)
{
    QSettings settings;
    settings.setValue(QString::fromLatin1(kSettingsRootsKey), normalizeRoots(roots));
}

QStringList MediaLibraryStore::normalizeRoots(const QStringList& roots)
{
    QStringList result;
    for (const QString& root : roots)
    {
        if (root.trimmed().isEmpty())
            continue;
        const QString clean = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
        if (!result.contains(clean, Qt::CaseInsensitive))
            result.append(clean);
    }
    return result;
}

bool MediaLibraryStore::isVideoFile(const QString& path)
{
    static const QStringList extensions = {
        QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("mov"),
        QStringLiteral("avi"), QStringLiteral("wmv"), QStringLiteral("webm"),
        QStringLiteral("m4v"), QStringLiteral("ts"), QStringLiteral("m2ts")
    };
    return extensions.contains(QFileInfo(path).suffix().toLower());
}

namespace
{
// 合并多轴脚本一定要用这三个顶层键之一。这里只做子串判断，命中或读不出文件
// 时都返回 true，交给 ScriptLoader 去给出准确答案。
bool scriptDeclaresMergedAxes(const QString& scriptPath)
{
    QFile file(scriptPath);
    if (!file.open(QIODevice::ReadOnly))
        return true;
    const QByteArray body = file.readAll();
    return body.contains("\"axes\"") || body.contains("\"channels\"")
           || body.contains("\"tracks\"");
}
} // namespace

int MediaLibraryStore::scriptAxesFor(const QString& mediaPath)
{
    // The axis count has to come from the script's contents, not from how many
    // .funscript files sit next to the video: a merged multi-axis script is a
    // single file that carries six axes, and counting files labelled it "single
    // axis" while playback happily drove all six. Loading the group is the one
    // answer that can never disagree with what actually plays.
    //
    // Parsing every script during a scan is expensive, so the two shapes that
    // cannot be multi-axis take a shortcut below. The shortcut only decides when
    // *not* to parse, so a wrong guess costs time, never correctness.
    const QFileInfo media(mediaPath);
    const QString base =
        media.absolutePath() + QLatin1Char('/') + media.completeBaseName();
    const QString mainScript = base + QStringLiteral(".funscript");

    // What the main script contributes: one axis when it is a plain single-axis
    // file, and "ask the parser" when it carries merged axes of its own.
    int axes = 0;
    if (QFileInfo::exists(mainScript))
        axes = scriptDeclaresMergedAxes(mainScript) ? -1 : 1;

    // One sibling file per remaining axis. A split file only ever contributes the
    // axis its name stands for - ScriptLoader::loadSplitTracks() takes that single
    // track out of it - so counting names here cannot disagree with playback.
    // This only holds while the main script is a plain single-axis file; merged
    // axes in the main script make the sibling rule ambiguous, hence the -1 above.
    if (axes >= 0)
    {
        for (Track track : TrackInfo::sr6Tracks())
        {
            if (track == Track::Stroke)
                continue;
            const QString canonicalName = TrackInfo::canonicalName(track);
            const QString tcodeName = TrackInfo::tcodeId(track).toLower();
            const QString canonicalFile =
                base + QLatin1Char('.') + canonicalName + QStringLiteral(".funscript");
            const QString tcodeFile =
                base + QLatin1Char('.') + tcodeName + QStringLiteral(".funscript");
            if (QFileInfo::exists(canonicalFile) || QFileInfo::exists(tcodeFile))
                ++axes;
        }
        return axes;
    }

    return ScriptLoader::load(mediaPath).tracks.size();
}

QString MediaLibraryStore::formatDuration(qint64 durationMs)
{
    if (durationMs <= 0)
        return QStringLiteral("--:--");

    return DurationFormat::clock(durationMs);
}

int MediaLibraryStore::naturalCompare(const QString& left, const QString& right)
{
    const int leftLength = left.size();
    const int rightLength = right.size();
    int i = 0;
    int j = 0;

    while (i < leftLength && j < rightLength)
    {
        const QChar leftChar = left.at(i);
        const QChar rightChar = right.at(j);
        const bool leftDigit = leftChar.isDigit();
        const bool rightDigit = rightChar.isDigit();

        if (leftDigit && rightDigit)
        {
            const int leftStart = i;
            const int rightStart = j;
            while (i < leftLength && left.at(i).isDigit())
                ++i;
            while (j < rightLength && right.at(j).isDigit())
                ++j;

            const QString leftNumber = left.mid(leftStart, i - leftStart);
            const QString rightNumber = right.mid(rightStart, j - rightStart);

            const qlonglong leftValue = leftNumber.toLongLong();
            const qlonglong rightValue = rightNumber.toLongLong();
            if (leftValue != rightValue)
                return leftValue < rightValue ? -1 : 1;
            if (leftNumber.size() != rightNumber.size())
                return leftNumber.size() < rightNumber.size() ? -1 : 1;
            continue;
        }

        const int folded = QString::compare(QString(leftChar), QString(rightChar),
                                            Qt::CaseInsensitive);
        if (folded != 0)
            return folded;
        ++i;
        ++j;
    }

    if (i < leftLength)
        return 1;
    if (j < rightLength)
        return -1;
    return 0;
}

QList<MediaItem> MediaLibraryStore::filterAndSort(const QList<MediaItem>& items,
                                                 const QString& needle,
                                                 bool onlyWithScript,
                                                 SortMode sortMode,
                                                 bool descending,
                                                 FavoriteFilter favoriteFilter,
                                                 const QSet<QString>& favoritePaths)
{
    QList<MediaItem> result;
    result.reserve(items.size());
    for (const MediaItem& item : items)
    {
        if (onlyWithScript && !item.hasScript())
            continue;
        if (!needle.isEmpty()
            && !item.displayName.contains(needle, Qt::CaseInsensitive))
        {
            continue;
        }
        if (favoriteFilter != FavoriteFilter::All)
        {
            const bool favorite =
                favoritePaths.contains(normalizedMediaPath(item.path));
            if (favoriteFilter == FavoriteFilter::OnlyFavorites && !favorite)
                continue;
            if (favoriteFilter == FavoriteFilter::NotFavorites && favorite)
                continue;
        }
        result.append(item);
    }

    std::stable_sort(result.begin(), result.end(), [sortMode, descending](
                                                       const MediaItem& left,
                                                       const MediaItem& right) {
        int order = 0;
        switch (sortMode)
        {
        case SortMode::Duration:
            if (left.durationMs != right.durationMs)
                order = left.durationMs < right.durationMs ? -1 : 1;
            break;
        case SortMode::RecentlyModified:
            if (left.modified != right.modified)
                order = left.modified < right.modified ? -1 : 1;
            break;
        case SortMode::Name:
            break;
        }
        if (order == 0)
            order = naturalCompare(left.displayName, right.displayName);

        return descending ? order > 0 : order < 0;
    });
    return result;
}

QString MediaLibraryStore::normalizedMediaPath(const QString& path)
{
    return FavoritesStore::normalizePath(path);
}
