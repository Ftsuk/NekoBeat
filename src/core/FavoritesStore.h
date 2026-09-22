#pragma once

#include <QDateTime>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

// One media file inside a favourite folder. The added timestamp lets the UI
// show when the entry was collected even though the default grid order stays
// the global sort the user picked in the media library.
struct FavoriteMember
{
    QString path;
    QDateTime added;
};

struct FavoriteFolder
{
    QString id;
    QString name;
    QDateTime created;
    QList<FavoriteMember> members;
};

// User owned favourite folders. The data lives in its own JSON file (roaming
// configuration, not the thumbnail cache) so clearing the media library cache
// never throws the collection away. No UI or mpv types are used here so the
// behaviour is unit tested directly.
class FavoritesStore
{
public:
    explicit FavoritesStore(const QString& filePath = QString());

    QString filePath() const { return m_filePath; }

    // Missing file => empty list and true. Corrupted/unsupported file => file
    // renamed to "<file>.bad", empty list and false.
    bool load();
    bool save() const;

    const QList<FavoriteFolder>& folders() const { return m_folders; }
    int count() const { return m_folders.size(); }
    bool contains(const QString& folderId) const;
    int indexOf(const QString& folderId) const;
    FavoriteFolder folder(const QString& folderId) const;
    QString folderName(const QString& folderId) const;

    // Mutations only touch memory; the caller decides when to save(). An empty
    // return id / false with error text means the request was rejected.
    QString createFolder(const QString& name, QString* error = nullptr);
    bool renameFolder(const QString& folderId, const QString& name,
                      QString* error = nullptr);
    bool removeFolder(const QString& folderId, QString* error = nullptr);
    bool moveFolder(const QString& folderId, int delta, QString* error = nullptr);

    bool addMember(const QString& folderId, const QString& mediaPath,
                   QString* error = nullptr);
    bool removeMember(const QString& folderId, const QString& mediaPath,
                      QString* error = nullptr);
    bool setMember(const QString& folderId, const QString& mediaPath, bool member,
                   QString* error = nullptr);
    bool isMember(const QString& folderId, const QString& mediaPath) const;

    QStringList folderIdsFor(const QString& mediaPath) const;
    QStringList folderNamesFor(const QString& mediaPath) const;
    bool isFavorite(const QString& mediaPath) const;

    int memberCount(const QString& folderId) const;
    QSet<QString> memberPaths(const QString& folderId) const;
    QSet<QString> allMemberPaths() const;

    int missingCount(const QString& folderId, const QSet<QString>& availablePaths) const;
    int missingCount(const QSet<QString>& availablePaths) const;
    int pruneMissing(const QSet<QString>& availablePaths);
    int pruneMissing(const QString& folderId, const QSet<QString>& availablePaths);

    // Explicit stale set. Only the keys listed are removed, so an empty or
    // wrongly built set can never wipe the collection - unlike pruneMissing(),
    // which removes everything outside the set it is handed.
    int countForPaths(const QSet<QString>& staleKeys) const;
    int prunePaths(const QSet<QString>& staleKeys);
    // Drops one media path from every folder; used by "清理此条".
    int removeMediaPath(const QString& mediaPath);
    // Moves every record pointing at oldPath over to newPath, e.g. after the
    // user reorganised folders. Returns the number of records touched.
    int relinkMediaPath(const QString& oldPath, const QString& newPath);

    void clear();

    static QString normalizePath(const QString& path);
    // Returns the trimmed name or an empty string while filling error text.
    // existingNames is matched case-insensitively.
    static QString validateName(const QString& name, const QStringList& existingNames,
                                QString* error = nullptr);
    static int maxNameLength();
    static int maxFolderCount();

private:
    bool markFileCorrupted();

    QList<FavoriteFolder> m_folders;
    QString m_filePath;
};
