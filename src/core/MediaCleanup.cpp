#include "core/MediaCleanup.h"

#include "core/FavoritesStore.h"
#include "core/LoopStore.h"
#include "core/PathUtils.h"
#include "core/StoreBackup.h"

namespace
{
// How many clips that are about to be deleted carry this tag.
int staleTagUsage(const LoopStore& store, const QSet<QString>& staleKeys,
                  const QString& tag)
{
    int count = 0;
    for (const LoopClip& clip : store.clips())
    {
        if (!staleKeys.contains(PathUtils::normalizeMediaPath(clip.mediaPath)))
            continue;
        if (clip.tags.contains(tag, Qt::CaseInsensitive))
            ++count;
    }
    return count;
}
}

MediaCleanup::MediaCleanup(FavoritesStore* favorites, LoopStore* loops)
    : m_favorites(favorites)
    , m_loops(loops)
{
}

MediaCleanupPlan MediaCleanup::plan(const QSet<QString>& staleKeys) const
{
    MediaCleanupPlan result;
    result.staleKeys = staleKeys;
    if (staleKeys.isEmpty())
        return result;

    if (m_favorites)
    {
        result.favoriteMembers = m_favorites->countForPaths(staleKeys);
        for (const FavoriteFolder& folder : m_favorites->folders())
        {
            for (const FavoriteMember& member : folder.members)
            {
                if (staleKeys.contains(FavoritesStore::normalizePath(member.path)))
                {
                    ++result.favoriteFolders;
                    break;
                }
            }
        }
    }

    if (m_loops)
    {
        result.loopClips = m_loops->countForMediaPaths(staleKeys);
        for (const QString& tag : m_loops->tagsOfMediaPaths(staleKeys))
        {
            // Only tags that the deletion leaves completely unused are counted;
            // a tag shared with another video stays.
            if (m_loops->tagUsage(tag) - staleTagUsage(*m_loops, staleKeys, tag) <= 0)
                ++result.tags;
        }
    }

    return result;
}

MediaCleanupPlan MediaCleanup::apply(const QSet<QString>& staleKeys)
{
    MediaCleanupPlan result;
    result.staleKeys = staleKeys;
    if (staleKeys.isEmpty())
        return result;

    if (m_favorites)
        result.favoriteMembers = m_favorites->prunePaths(staleKeys);

    if (m_loops)
    {
        // Collect first: after the clips are gone there is no way to tell which
        // tags they used to carry.
        const QSet<QString> affected = m_loops->tagsOfMediaPaths(staleKeys);
        result.loopClips = m_loops->pruneMediaPaths(staleKeys);
        for (const QString& tag : affected)
        {
            if (m_loops->tagUsage(tag) == 0 && m_loops->removeTag(tag))
                ++result.tags;
        }
    }

    return result;
}

QStringList MediaCleanup::backup() const
{
    QStringList files;
    if (m_favorites && !m_favorites->filePath().isEmpty())
        files.append(m_favorites->filePath());
    if (m_loops && !m_loops->filePath().isEmpty())
        files.append(m_loops->filePath());
    return StoreBackup::backupFiles(files);
}

int MediaCleanup::relink(const QString& oldPath, const QString& newPath)
{
    int moved = 0;
    if (m_favorites)
        moved += m_favorites->relinkMediaPath(oldPath, newPath);
    if (m_loops)
        moved += m_loops->relinkMediaPath(oldPath, newPath);
    return moved;
}

int MediaCleanup::relinkAll(const QHash<QString, QString>& mapping)
{
    int moved = 0;
    for (auto it = mapping.constBegin(); it != mapping.constEnd(); ++it)
        moved += relink(it.key(), it.value());
    return moved;
}
