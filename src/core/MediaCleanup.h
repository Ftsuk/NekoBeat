#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

class FavoritesStore;
class LoopStore;

// Exactly what a clean-up is about to remove. It is built before anything is
// touched so the confirmation dialog can list real numbers instead of "some
// entries".
struct MediaCleanupPlan
{
    QSet<QString> staleKeys;
    int favoriteMembers = 0;
    int favoriteFolders = 0;
    int loopClips = 0;
    // Tag definitions that would be left without a single user.
    int tags = 0;

    int total() const { return favoriteMembers + loopClips + tags; }
    bool isEmpty() const { return total() == 0; }
};

// One clean-up pass over every store that remembers media by path. Adding a new
// store means adding it here, so no dialog can forget it and leave half the
// references behind.
class MediaCleanup
{
public:
    MediaCleanup(FavoritesStore* favorites, LoopStore* loops);

    // Dry run: only reads.
    MediaCleanupPlan plan(const QSet<QString>& staleKeys) const;
    // Removes precisely what plan() reported, and returns the real numbers.
    // Call backup() first.
    MediaCleanupPlan apply(const QSet<QString>& staleKeys);

    // Copies both data files aside and returns the copies that were made.
    QStringList backup() const;

    // Re-points every record of oldPath at newPath. Returns how many records
    // moved. The caller saves the stores afterwards.
    int relink(const QString& oldPath, const QString& newPath);
    int relinkAll(const QHash<QString, QString>& mapping);

private:
    FavoritesStore* m_favorites = nullptr;
    LoopStore* m_loops = nullptr;
};
