#pragma once

#include "core/MediaItem.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

// Storage and pure helpers behind the media library: cache locations, the JSON
// metadata index, folder scanning, filtering and sorting. No UI types here so
// the behaviour is directly unit tested.
class MediaLibraryStore
{
public:
    enum class SortMode
    {
        Name = 0,
        Duration = 1,
        RecentlyModified = 2
    };

    // Extra filter applied on top of the search box and the script filter.
    enum class FavoriteFilter
    {
        All = 0,
        OnlyFavorites = 1,
        NotFavorites = 2
    };

    explicit MediaLibraryStore(const QString& cacheRoot = QString());

    QString cacheRoot() const { return m_cacheRoot; }
    QString thumbnailsDir() const;
    QString temporaryDir() const;
    QString indexPath() const;
    void ensureDirectories() const;

    QString cacheKeyFor(const QString& mediaPath, qint64 fileSize,
                        const QDateTime& modified) const;
    // Key used before the folder moved out of the hash. Only needed to relocate
    // thumbnails that were cached by an older build.
    static QString legacyCacheKeyFor(const QString& mediaPath, qint64 fileSize,
                                     const QDateTime& modified);
    QString thumbnailPathFor(const QString& cacheKey) const;

    QHash<QString, MediaItem> loadIndex() const;
    void saveIndex(const QList<MediaItem>& items) const;
    void clearCache() const;

    QList<MediaItem> scanRoots(const QStringList& roots) const;

    static QStringList loadRootFolders();
    static void saveRootFolders(const QStringList& roots);
    static QStringList normalizeRoots(const QStringList& roots);
    static bool isVideoFile(const QString& path);
    static int scriptAxesFor(const QString& mediaPath);
    static QString formatDuration(qint64 durationMs);
    static int naturalCompare(const QString& left, const QString& right);

    static QList<MediaItem> filterAndSort(const QList<MediaItem>& items,
                                          const QString& needle,
                                          bool onlyWithScript,
                                          SortMode sortMode,
                                          bool descending = false,
                                          FavoriteFilter favoriteFilter = FavoriteFilter::All,
                                          const QSet<QString>& favoritePaths = {});

    // Normalised comparison key for media paths (lower case, clean separators).
    static QString normalizedMediaPath(const QString& path);

private:
    QList<MediaItem> scanRoot(const QString& root,
                              const QHash<QString, MediaItem>& cached,
                              const QHash<QString, MediaItem>& cachedByContent) const;

    QString m_cacheRoot;
};
