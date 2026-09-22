#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

// How reachable one referenced media file currently is.
//
// The decision deliberately ignores "what the media library scanned": an empty
// scan proves nothing, because a root folder may have been removed from the
// library or a drive may simply be unplugged. Only Missing may ever be cleaned
// up. Unreachable means "cannot tell right now" and must keep its records, or
// the user finds an emptied favourite list the next time the drive is plugged
// back in.
enum class MediaAvailability
{
    Available,
    Missing,
    Unreachable
};

// Collects every media path referenced by the favourite folders and the loop
// list and sorts them into available / missing / unreachable.
//
// availablePaths() is what the stores use to decide whether a record is stale;
// it contains both Available and Unreachable entries, so nothing is ever
// removed just because a drive is offline. hasUnreachable() is the guard the
// clean-up entry points have to respect.
class MediaReferenceIndex
{
public:
    // A path the media library has already scanned; trusted without a stat.
    void addKnownAvailable(const QString& mediaPath);
    // A path referenced by a favourite member or a loop clip; needs probing.
    void addReference(const QString& mediaPath);

    // Probes every reference that is not known to be available yet. The
    // existence of a directory is cached, so many files in one folder cost one
    // probe.
    void resolve();

    MediaAvailability availability(const QString& mediaPath) const;

    // Available plus unreachable; never report these as stale.
    const QSet<QString>& availablePaths() const { return m_available; }
    // Confirmed gone: the file is absent and its location is reachable.
    const QSet<QString>& missingPaths() const { return m_missing; }
    int missingCount() const { return m_missing.size(); }

    bool hasUnreachable() const { return !m_unreachableRoots.isEmpty(); }
    // Volume roots that could not be reached, e.g. "F:/", for user hints.
    QStringList unreachableRoots() const;

    static QString normalize(const QString& mediaPath);
    static MediaAvailability probe(const QString& mediaPath);

private:
    QSet<QString> m_available;
    QSet<QString> m_missing;
    // Subset of m_available: reachable files whose location could not be
    // opened. Kept separately so availability() can still report Unreachable
    // while availablePaths() hands both groups to the stores.
    QSet<QString> m_unreachablePaths;
    QSet<QString> m_references;
    QSet<QString> m_unreachableRoots;
};
