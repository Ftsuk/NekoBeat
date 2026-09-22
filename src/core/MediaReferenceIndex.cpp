#include "core/MediaReferenceIndex.h"

#include "core/PathUtils.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>

#include <utility>

namespace
{
// QDir::cdUp() refuses to walk into a directory that does not exist, which is
// exactly the case we care about, so climb the cleaned path textually instead.
// Returns an empty string once there is nothing left above.
QString parentDirectory(const QString& path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0)
        return QString();

    QString parent = path.left(slash);
    // "C:" is not the root, "C:/" is.
    if (parent.endsWith(QLatin1Char(':')))
        parent += QLatin1Char('/');
    return parent;
}

// Walks up from the deepest directory until one actually exists. An empty
// result means not even the volume root could be opened - the drive is gone.
QString firstExistingAncestor(const QString& directory)
{
    QString current = QDir::cleanPath(directory);
    for (int guard = 0; guard < 64; ++guard)
    {
        if (QDir(current).exists())
            return current;

        const QString parent = parentDirectory(current);
        if (parent.isEmpty() || parent == current)
            return QString();
        current = parent;
    }
    return QString();
}

// "F:/a/b" -> "F:/" so the hint names the volume instead of one folder.
QString volumeRootOf(const QString& directory)
{
    QString root = QDir::cleanPath(directory);
    for (int guard = 0; guard < 64; ++guard)
    {
        const QString parent = parentDirectory(root);
        if (parent.isEmpty() || parent == root)
            break;
        root = parent;
    }
    return root;
}
}

QString MediaReferenceIndex::normalize(const QString& mediaPath)
{
    return PathUtils::normalizeMediaPath(mediaPath);
}

MediaAvailability MediaReferenceIndex::probe(const QString& mediaPath)
{
    if (normalize(mediaPath).isEmpty())
        return MediaAvailability::Unreachable;

    const QFileInfo info(mediaPath);
    if (info.exists())
        return MediaAvailability::Available;

    // The file is gone. Whether that counts as "deleted" or "cannot tell"
    // depends on reaching its location: with F:/a still openable the file was
    // really removed, with F:/ itself unopenable the drive is simply offline.
    return firstExistingAncestor(info.absolutePath()).isEmpty()
               ? MediaAvailability::Unreachable
               : MediaAvailability::Missing;
}

void MediaReferenceIndex::addKnownAvailable(const QString& mediaPath)
{
    const QString key = normalize(mediaPath);
    if (!key.isEmpty())
        m_available.insert(key);
}

void MediaReferenceIndex::addReference(const QString& mediaPath)
{
    const QString key = normalize(mediaPath);
    if (!key.isEmpty())
        m_references.insert(key);
}

void MediaReferenceIndex::resolve()
{
    m_missing.clear();
    m_unreachablePaths.clear();
    m_unreachableRoots.clear();

    // Many references usually share one folder, so probe folders instead of
    // single files.
    QHash<QString, QString> ancestorCache;

    for (const QString& key : std::as_const(m_references))
    {
        if (m_available.contains(key))
            continue;

        const QFileInfo info(key);
        if (info.exists())
        {
            m_available.insert(key);
            continue;
        }

        const QString directory = QDir::cleanPath(info.absolutePath());
        auto cached = ancestorCache.constFind(directory);
        if (cached == ancestorCache.constEnd())
            cached = ancestorCache.insert(directory, firstExistingAncestor(directory));

        if (cached.value().isEmpty())
        {
            m_unreachableRoots.insert(volumeRootOf(directory));
            m_unreachablePaths.insert(key);
            // Offline is not the same as gone: keep the record alive and let
            // the caller decide, but never let it be cleaned up.
            m_available.insert(key);
        }
        else
        {
            m_missing.insert(key);
        }
    }
}

MediaAvailability MediaReferenceIndex::availability(const QString& mediaPath) const
{
    const QString key = normalize(mediaPath);
    if (key.isEmpty())
        return MediaAvailability::Unreachable;
    if (m_missing.contains(key))
        return MediaAvailability::Missing;
    if (m_unreachablePaths.contains(key))
        return MediaAvailability::Unreachable;
    if (m_available.contains(key))
        return MediaAvailability::Available;
    // Never resolved: stay conservative so an unknown path is never cleaned.
    return MediaAvailability::Unreachable;
}

QStringList MediaReferenceIndex::unreachableRoots() const
{
    QStringList roots = m_unreachableRoots.values();
    roots.sort();
    return roots;
}
