#pragma once

#include "core/LoopClip.h"

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

// User owned loop clips. Like FavoritesStore the data lives in its own JSON file
// under the roaming configuration directory, so clearing the thumbnail cache
// never throws the collection away. No UI or mpv types are used here so the
// behaviour is unit tested directly.
class LoopStore
{
public:
    explicit LoopStore(const QString& filePath = QString());

    QString filePath() const { return m_filePath; }

    // Missing file => empty data and true. Corrupted/unsupported file => file
    // renamed to "<file>.bad", empty data and false.
    bool load();
    bool save() const;

    // ---- clips ---------------------------------------------------------
    const QList<LoopClip>& clips() const { return m_clips; }
    int clipCount() const { return m_clips.size(); }
    bool containsClip(const QString& clipId) const;
    int indexOfClip(const QString& clipId) const;
    LoopClip clip(const QString& clipId) const;
    QList<LoopClip> clipsForMedia(const QString& mediaPath) const;

    // Returns the new id, or an empty string with error text filled in.
    QString addClip(const LoopClip& clip, QString* error = nullptr);
    bool updateClip(const LoopClip& clip, QString* error = nullptr);
    bool removeClip(const QString& clipId, QString* error = nullptr);
    bool setClipFolder(const QString& clipId, const QString& folderId, bool member,
                       QString* error = nullptr);

    // ---- folders -------------------------------------------------------
    const QList<LoopFolder>& folders() const { return m_folders; }
    int folderCount() const { return m_folders.size(); }
    bool containsFolder(const QString& folderId) const;
    int indexOfFolder(const QString& folderId) const;
    LoopFolder folder(const QString& folderId) const;
    QString folderName(const QString& folderId) const;
    QString createFolder(const QString& name, QString* error = nullptr);
    bool renameFolder(const QString& folderId, const QString& name,
                      QString* error = nullptr);
    bool removeFolder(const QString& folderId, QString* error = nullptr);
    bool moveFolder(const QString& folderId, int delta, QString* error = nullptr);
    int folderClipCount(const QString& folderId) const;

    // ---- tags ----------------------------------------------------------
    // Tags keep the spelling the user typed; comparison is case insensitive.
    const QStringList& tags() const { return m_tags; }
    // Sorted by usage (descending) and then by name.
    QStringList tagsByUsage() const;
    int tagUsage(const QString& tag) const;
    // Renames a tag everywhere; when the target already exists the two merge.
    bool renameTag(const QString& from, const QString& to, QString* error = nullptr);
    bool removeTag(const QString& tag, QString* error = nullptr);
    int pruneUnusedTags();

    // ---- missing media -------------------------------------------------
    int missingClipCount(const QSet<QString>& availablePaths) const;
    int pruneMissingClips(const QSet<QString>& availablePaths);
    QSet<QString> mediaPaths() const;

    // Explicit stale set: only the listed media paths are dropped, so an empty
    // set can never wipe the list. Used by the shared clean-up.
    int countForMediaPaths(const QSet<QString>& staleKeys) const;
    int pruneMediaPaths(const QSet<QString>& staleKeys);
    // Every clip of one video; used by "清理此条".
    int removeMediaPath(const QString& mediaPath);
    // Tags carried by the clips of these media paths, so the caller can find
    // the tag definitions that the deletion leaves without any user.
    QSet<QString> tagsOfMediaPaths(const QSet<QString>& keys) const;
    // Re-points every clip of oldPath at newPath after the user reorganised the
    // library. The stored thumbnail key is kept: it still resolves.
    int relinkMediaPath(const QString& oldPath, const QString& newPath);

    void clear();

    static QString thumbnailKeyFor(const QString& mediaPath, qint64 startMs);
    static QString validateName(const QString& name, const QStringList& existingNames,
                                QString* error = nullptr);
    static QString validateTag(const QString& tag, QString* error = nullptr);
    static int maxNameLength();
    static int maxFolderCount();
    static int maxTagLength();
    static int maxTagCount();

private:
    bool markFileCorrupted();
    void registerTag(const QString& tag);
    void registerClipTags();
    int indexOfTag(const QString& tag) const;

    QList<LoopClip> m_clips;
    QList<LoopFolder> m_folders;
    QStringList m_tags;
    QString m_filePath;
};
