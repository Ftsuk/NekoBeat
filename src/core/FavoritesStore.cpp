#include "core/Loc.h"
#include "core/FavoritesStore.h"

#include "core/PathUtils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace
{
constexpr int kFavoritesVersion = 1;
constexpr int kMaxNameLength = 30;
constexpr int kMaxFolderCount = 200;

QString defaultFilePath()
{
#ifdef Q_OS_WIN
    // AppDataLocation would append organization/application and end up in
    // "Roaming/NekoBeat/NekoBeat"; the plain roaming folder keeps the
    // path short and matches where the user expects the collection to live.
    QString base = qEnvironmentVariable("APPDATA");
#else
    QString base;
#endif
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty())
        base = QDir::currentPath();
    return QDir(QDir(base).absoluteFilePath(QStringLiteral("NekoBeat")))
        .absoluteFilePath(QStringLiteral("favorites.json"));
}

}

FavoritesStore::FavoritesStore(const QString& filePath)
    : m_filePath(filePath.isEmpty() ? defaultFilePath() : QDir::cleanPath(filePath))
{
}

bool FavoritesStore::load()
{
    m_folders.clear();

    QFile file(m_filePath);
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return markFileCorrupted();

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != kFavoritesVersion)
        return markFileCorrupted();

    QSet<QString> seenIds;
    for (const QJsonValue& value : root.value(QStringLiteral("folders")).toArray())
    {
        const QJsonObject object = value.toObject();
        FavoriteFolder folder;
        folder.id = object.value(QStringLiteral("id")).toString().trimmed();
        folder.name = object.value(QStringLiteral("name")).toString().trimmed();
        folder.created = QDateTime::fromString(
            object.value(QStringLiteral("created")).toString(), Qt::ISODateWithMs);
        if (folder.id.isEmpty() || folder.name.isEmpty()
            || seenIds.contains(folder.id))
        {
            continue;
        }

        QSet<QString> seenMembers;
        for (const QJsonValue& memberValue : object.value(QStringLiteral("members")).toArray())
        {
            FavoriteMember member;
            if (memberValue.isString())
            {
                member.path = memberValue.toString();
            }
            else
            {
                const QJsonObject memberObject = memberValue.toObject();
                member.path = memberObject.value(QStringLiteral("path")).toString();
                member.added = QDateTime::fromString(
                    memberObject.value(QStringLiteral("added")).toString(),
                    Qt::ISODateWithMs);
            }
            if (member.path.isEmpty())
                continue;
            const QString key = normalizePath(member.path);
            if (key.isEmpty() || seenMembers.contains(key))
                continue;
            seenMembers.insert(key);
            folder.members.append(member);
        }

        seenIds.insert(folder.id);
        m_folders.append(folder);
    }
    return true;
}

bool FavoritesStore::save() const
{
    const QFileInfo info(m_filePath);
    if (!info.absoluteDir().exists() && !QDir().mkpath(info.absolutePath()))
        return false;

    QJsonArray folderArray;
    for (const FavoriteFolder& folder : m_folders)
    {
        QJsonArray memberArray;
        for (const FavoriteMember& member : folder.members)
        {
            QJsonObject memberObject;
            memberObject.insert(QStringLiteral("path"), member.path);
            memberObject.insert(QStringLiteral("added"),
                                member.added.toString(Qt::ISODateWithMs));
            memberArray.append(memberObject);
        }

        QJsonObject object;
        object.insert(QStringLiteral("id"), folder.id);
        object.insert(QStringLiteral("name"), folder.name);
        object.insert(QStringLiteral("created"), folder.created.toString(Qt::ISODateWithMs));
        object.insert(QStringLiteral("members"), memberArray);
        folderArray.append(object);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), kFavoritesVersion);
    root.insert(QStringLiteral("folders"), folderArray);

    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}

bool FavoritesStore::contains(const QString& folderId) const
{
    return indexOf(folderId) >= 0;
}

int FavoritesStore::indexOf(const QString& folderId) const
{
    for (int i = 0; i < m_folders.size(); ++i)
    {
        if (m_folders.at(i).id == folderId)
            return i;
    }
    return -1;
}

FavoriteFolder FavoritesStore::folder(const QString& folderId) const
{
    const int index = indexOf(folderId);
    return index >= 0 ? m_folders.at(index) : FavoriteFolder();
}

QString FavoritesStore::folderName(const QString& folderId) const
{
    const int index = indexOf(folderId);
    return index >= 0 ? m_folders.at(index).name : QString();
}

QString FavoritesStore::createFolder(const QString& name, QString* error)
{
    if (m_folders.size() >= kMaxFolderCount)
    {
        if (error)
            *error = LT("收藏夹数量已达上限（%1 个）").arg(kMaxFolderCount);
        return QString();
    }

    QStringList existing;
    existing.reserve(m_folders.size());
    for (const FavoriteFolder& folder : m_folders)
        existing.append(folder.name);

    const QString validated = validateName(name, existing, error);
    if (validated.isEmpty())
        return QString();

    FavoriteFolder folder;
    folder.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    folder.name = validated;
    folder.created = QDateTime::currentDateTime();
    m_folders.append(folder);
    return folder.id;
}

bool FavoritesStore::renameFolder(const QString& folderId, const QString& name, QString* error)
{
    const int index = indexOf(folderId);
    if (index < 0)
    {
        if (error)
            *error = LT("收藏夹不存在");
        return false;
    }

    QStringList existing;
    existing.reserve(m_folders.size() - 1);
    for (int i = 0; i < m_folders.size(); ++i)
    {
        if (i != index)
            existing.append(m_folders.at(i).name);
    }

    const QString validated = validateName(name, existing, error);
    if (validated.isEmpty())
        return false;

    m_folders[index].name = validated;
    return true;
}

bool FavoritesStore::removeFolder(const QString& folderId, QString* error)
{
    const int index = indexOf(folderId);
    if (index < 0)
    {
        if (error)
            *error = LT("收藏夹不存在");
        return false;
    }
    m_folders.removeAt(index);
    return true;
}

bool FavoritesStore::moveFolder(const QString& folderId, int delta, QString* error)
{
    const int index = indexOf(folderId);
    if (index < 0)
    {
        if (error)
            *error = LT("收藏夹不存在");
        return false;
    }

    const int target = index + delta;
    if (target < 0 || target >= m_folders.size())
    {
        if (error)
            *error = LT("收藏夹已经在%1").arg(delta < 0 ? LT("最前")
                                                                    : LT("最后"));
        return false;
    }

    m_folders.move(index, target);
    return true;
}

bool FavoritesStore::addMember(const QString& folderId, const QString& mediaPath,
                               QString* error)
{
    const int index = indexOf(folderId);
    if (index < 0)
    {
        if (error)
            *error = LT("收藏夹不存在");
        return false;
    }

    const QString key = normalizePath(mediaPath);
    if (key.isEmpty())
    {
        if (error)
            *error = LT("无效的媒体路径");
        return false;
    }

    for (const FavoriteMember& member : m_folders.at(index).members)
    {
        if (normalizePath(member.path) == key)
            return true;
    }

    FavoriteMember member;
    member.path = mediaPath;
    member.added = QDateTime::currentDateTime();
    m_folders[index].members.append(member);
    return true;
}

bool FavoritesStore::removeMember(const QString& folderId, const QString& mediaPath,
                                  QString* error)
{
    const int index = indexOf(folderId);
    if (index < 0)
    {
        if (error)
            *error = LT("收藏夹不存在");
        return false;
    }

    const QString key = normalizePath(mediaPath);
    QList<FavoriteMember>& members = m_folders[index].members;
    for (int i = 0; i < members.size(); ++i)
    {
        if (normalizePath(members.at(i).path) == key)
        {
            members.removeAt(i);
            return true;
        }
    }
    return false;
}

bool FavoritesStore::setMember(const QString& folderId, const QString& mediaPath,
                               bool member, QString* error)
{
    return member ? addMember(folderId, mediaPath, error)
                  : removeMember(folderId, mediaPath, error);
}

bool FavoritesStore::isMember(const QString& folderId, const QString& mediaPath) const
{
    const int index = indexOf(folderId);
    if (index < 0)
        return false;

    const QString key = normalizePath(mediaPath);
    for (const FavoriteMember& member : m_folders.at(index).members)
    {
        if (normalizePath(member.path) == key)
            return true;
    }
    return false;
}

QStringList FavoritesStore::folderIdsFor(const QString& mediaPath) const
{
    QStringList ids;
    for (const FavoriteFolder& folder : m_folders)
    {
        if (isMember(folder.id, mediaPath))
            ids.append(folder.id);
    }
    return ids;
}

QStringList FavoritesStore::folderNamesFor(const QString& mediaPath) const
{
    QStringList names;
    for (const FavoriteFolder& folder : m_folders)
    {
        if (isMember(folder.id, mediaPath))
            names.append(folder.name);
    }
    return names;
}

bool FavoritesStore::isFavorite(const QString& mediaPath) const
{
    const QString key = normalizePath(mediaPath);
    if (key.isEmpty())
        return false;
    for (const FavoriteFolder& folder : m_folders)
    {
        for (const FavoriteMember& member : folder.members)
        {
            if (normalizePath(member.path) == key)
                return true;
        }
    }
    return false;
}

int FavoritesStore::memberCount(const QString& folderId) const
{
    const int index = indexOf(folderId);
    return index >= 0 ? m_folders.at(index).members.size() : 0;
}

QSet<QString> FavoritesStore::memberPaths(const QString& folderId) const
{
    QSet<QString> paths;
    const int index = indexOf(folderId);
    if (index < 0)
        return paths;

    for (const FavoriteMember& member : m_folders.at(index).members)
    {
        const QString key = normalizePath(member.path);
        if (!key.isEmpty())
            paths.insert(key);
    }
    return paths;
}

QSet<QString> FavoritesStore::allMemberPaths() const
{
    QSet<QString> paths;
    for (const FavoriteFolder& folder : m_folders)
    {
        for (const FavoriteMember& member : folder.members)
        {
            const QString key = normalizePath(member.path);
            if (!key.isEmpty())
                paths.insert(key);
        }
    }
    return paths;
}

int FavoritesStore::missingCount(const QString& folderId,
                                 const QSet<QString>& availablePaths) const
{
    const int index = indexOf(folderId);
    if (index < 0)
        return 0;

    int missing = 0;
    for (const FavoriteMember& member : m_folders.at(index).members)
    {
        const QString key = normalizePath(member.path);
        if (key.isEmpty() || !availablePaths.contains(key))
            ++missing;
    }
    return missing;
}

int FavoritesStore::missingCount(const QSet<QString>& availablePaths) const
{
    int missing = 0;
    for (const FavoriteFolder& folder : m_folders)
        missing += missingCount(folder.id, availablePaths);
    return missing;
}

int FavoritesStore::pruneMissing(const QSet<QString>& availablePaths)
{
    int removed = 0;
    for (FavoriteFolder& folder : m_folders)
    {
        for (int i = folder.members.size() - 1; i >= 0; --i)
        {
            const QString key = normalizePath(folder.members.at(i).path);
            if (key.isEmpty() || !availablePaths.contains(key))
            {
                folder.members.removeAt(i);
                ++removed;
            }
        }
    }
    return removed;
}

int FavoritesStore::pruneMissing(const QString& folderId,
                                 const QSet<QString>& availablePaths)
{
    const int index = indexOf(folderId);
    if (index < 0)
        return 0;

    int removed = 0;
    QList<FavoriteMember>& members = m_folders[index].members;
    for (int i = members.size() - 1; i >= 0; --i)
    {
        const QString key = normalizePath(members.at(i).path);
        if (key.isEmpty() || !availablePaths.contains(key))
        {
            members.removeAt(i);
            ++removed;
        }
    }
    return removed;
}

int FavoritesStore::countForPaths(const QSet<QString>& staleKeys) const
{
    if (staleKeys.isEmpty())
        return 0;

    int count = 0;
    for (const FavoriteFolder& folder : m_folders)
    {
        for (const FavoriteMember& member : folder.members)
        {
            const QString key = normalizePath(member.path);
            if (!key.isEmpty() && staleKeys.contains(key))
                ++count;
        }
    }
    return count;
}

int FavoritesStore::prunePaths(const QSet<QString>& staleKeys)
{
    if (staleKeys.isEmpty())
        return 0;

    int removed = 0;
    for (FavoriteFolder& folder : m_folders)
    {
        for (int i = folder.members.size() - 1; i >= 0; --i)
        {
            const QString key = normalizePath(folder.members.at(i).path);
            if (!key.isEmpty() && staleKeys.contains(key))
            {
                folder.members.removeAt(i);
                ++removed;
            }
        }
    }
    return removed;
}

int FavoritesStore::removeMediaPath(const QString& mediaPath)
{
    const QString target = normalizePath(mediaPath);
    if (target.isEmpty())
        return 0;

    int removed = 0;
    for (FavoriteFolder& folder : m_folders)
    {
        for (int i = folder.members.size() - 1; i >= 0; --i)
        {
            if (normalizePath(folder.members.at(i).path) == target)
            {
                folder.members.removeAt(i);
                ++removed;
            }
        }
    }
    return removed;
}

int FavoritesStore::relinkMediaPath(const QString& oldPath, const QString& newPath)
{
    const QString oldKey = normalizePath(oldPath);
    const QString newKey = normalizePath(newPath);
    if (oldKey.isEmpty() || newKey.isEmpty() || oldKey == newKey)
        return 0;

    int changed = 0;
    for (FavoriteFolder& folder : m_folders)
    {
        // A folder may already hold the new path; in that case the stale entry
        // is simply dropped instead of creating a duplicate.
        bool hasTarget = false;
        for (const FavoriteMember& member : folder.members)
        {
            if (normalizePath(member.path) == newKey)
            {
                hasTarget = true;
                break;
            }
        }

        for (int i = folder.members.size() - 1; i >= 0; --i)
        {
            if (normalizePath(folder.members.at(i).path) != oldKey)
                continue;

            if (hasTarget)
            {
                folder.members.removeAt(i);
            }
            else
            {
                folder.members[i].path = newPath;
                hasTarget = true;
            }
            ++changed;
        }
    }
    return changed;
}

void FavoritesStore::clear()
{
    m_folders.clear();
}

QString FavoritesStore::normalizePath(const QString& path)
{
    // Kept as a thin wrapper so existing callers and tests keep working while
    // the implementation is shared with the loop store.
    return PathUtils::normalizeMediaPath(path);
}

QString FavoritesStore::validateName(const QString& name,
                                     const QStringList& existingNames, QString* error)
{
    const QString trimmed = name.trimmed();
    const auto reject = [error](const QString& text) {
        if (error)
            *error = text;
        return QString();
    };

    if (trimmed.isEmpty())
        return reject(LT("收藏夹名称不能为空"));
    if (trimmed.size() > kMaxNameLength)
    {
        return reject(LT("收藏夹名称不能超过 %1 个字符")
                          .arg(kMaxNameLength));
    }
    for (const QString& existing : existingNames)
    {
        if (existing.compare(trimmed, Qt::CaseInsensitive) == 0)
            return reject(LT("已存在同名收藏夹「%1」").arg(trimmed));
    }

    if (error)
        error->clear();
    return trimmed;
}

int FavoritesStore::maxNameLength()
{
    return kMaxNameLength;
}

int FavoritesStore::maxFolderCount()
{
    return kMaxFolderCount;
}

bool FavoritesStore::markFileCorrupted()
{
    const QString backup = m_filePath + QStringLiteral(".bad");
    QFile::remove(backup);
    QFile::rename(m_filePath, backup);
    return false;
}
