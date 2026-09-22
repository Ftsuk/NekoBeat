#include "core/Loc.h"
#include "core/LoopStore.h"

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

#include <algorithm>

namespace
{
constexpr int kLoopStoreVersion = 1;
constexpr int kMaxNameLength = 30;
constexpr int kMaxFolderCount = 200;
constexpr int kMaxTagLength = 20;
constexpr int kMaxTagCount = 500;

QString defaultFilePath()
{
#ifdef Q_OS_WIN
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
        .absoluteFilePath(QStringLiteral("loops.json"));
}

QStringList normalizedTags(const QStringList& tags)
{
    QStringList result;
    for (const QString& tag : tags)
    {
        const QString trimmed = tag.trimmed();
        if (trimmed.isEmpty())
            continue;
        const bool duplicate =
            std::any_of(result.cbegin(), result.cend(), [&trimmed](const QString& existing) {
                return existing.compare(trimmed, Qt::CaseInsensitive) == 0;
            });
        if (!duplicate)
            result.append(trimmed);
    }
    return result;
}
}

LoopStore::LoopStore(const QString& filePath)
    : m_filePath(filePath.isEmpty() ? defaultFilePath() : QDir::cleanPath(filePath))
{
}

bool LoopStore::load()
{
    m_clips.clear();
    m_folders.clear();
    m_tags.clear();

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
    if (root.value(QStringLiteral("version")).toInt() != kLoopStoreVersion)
        return markFileCorrupted();

    QSet<QString> seenFolderIds;
    for (const QJsonValue& value : root.value(QStringLiteral("folders")).toArray())
    {
        const QJsonObject object = value.toObject();
        LoopFolder folder;
        folder.id = object.value(QStringLiteral("id")).toString().trimmed();
        folder.name = object.value(QStringLiteral("name")).toString().trimmed();
        folder.created = QDateTime::fromString(
            object.value(QStringLiteral("created")).toString(), Qt::ISODateWithMs);
        if (folder.id.isEmpty() || folder.name.isEmpty() || seenFolderIds.contains(folder.id))
            continue;
        seenFolderIds.insert(folder.id);
        m_folders.append(folder);
    }

    QSet<QString> seenClipIds;
    for (const QJsonValue& value : root.value(QStringLiteral("clips")).toArray())
    {
        const QJsonObject object = value.toObject();
        LoopClip clip;
        clip.id = object.value(QStringLiteral("id")).toString().trimmed();
        clip.mediaPath = object.value(QStringLiteral("mediaPath")).toString().trimmed();
        clip.startMs = static_cast<qint64>(object.value(QStringLiteral("startMs")).toDouble());
        clip.endMs = static_cast<qint64>(object.value(QStringLiteral("endMs")).toDouble());
        clip.title = object.value(QStringLiteral("title")).toString().trimmed();
        clip.note = object.value(QStringLiteral("note")).toString();
        clip.thumbKey = object.value(QStringLiteral("thumbKey")).toString();
        clip.created = QDateTime::fromString(
            object.value(QStringLiteral("created")).toString(), Qt::ISODateWithMs);
        clip.updated = QDateTime::fromString(
            object.value(QStringLiteral("updated")).toString(), Qt::ISODateWithMs);

        if (clip.id.isEmpty() || clip.mediaPath.isEmpty() || seenClipIds.contains(clip.id))
            continue;
        if (clip.endMs <= clip.startMs)
            continue;

        for (const QJsonValue& tagValue : object.value(QStringLiteral("tags")).toArray())
            clip.tags.append(tagValue.toString());
        clip.tags = normalizedTags(clip.tags);

        for (const QJsonValue& folderValue : object.value(QStringLiteral("folderIds")).toArray())
        {
            const QString folderId = folderValue.toString().trimmed();
            if (!folderId.isEmpty() && seenFolderIds.contains(folderId)
                && !clip.folderIds.contains(folderId))
            {
                clip.folderIds.append(folderId);
            }
        }

        if (clip.title.isEmpty())
            clip.title = LoopClipRules::defaultTitle(clip.mediaPath, clip.startMs, clip.endMs);
        if (clip.thumbKey.isEmpty())
            clip.thumbKey = thumbnailKeyFor(clip.mediaPath, clip.startMs);

        seenClipIds.insert(clip.id);
        m_clips.append(clip);
    }

    for (const QJsonValue& tagValue : root.value(QStringLiteral("tags")).toArray())
        registerTag(tagValue.toString());
    // Clips authored by an older build may carry tags missing from the pool.
    registerClipTags();

    // Drop folder ids that no longer resolve, e.g. after a manual edit.
    for (LoopClip& clip : m_clips)
    {
        for (int i = clip.folderIds.size() - 1; i >= 0; --i)
        {
            if (!seenFolderIds.contains(clip.folderIds.at(i)))
                clip.folderIds.removeAt(i);
        }
    }
    return true;
}

bool LoopStore::save() const
{
    const QFileInfo info(m_filePath);
    if (!info.absoluteDir().exists() && !QDir().mkpath(info.absolutePath()))
        return false;

    QJsonArray clipArray;
    for (const LoopClip& clip : m_clips)
    {
        QJsonArray tagArray;
        for (const QString& tag : clip.tags)
            tagArray.append(tag);
        QJsonArray folderArray;
        for (const QString& folderId : clip.folderIds)
            folderArray.append(folderId);

        QJsonObject object;
        object.insert(QStringLiteral("id"), clip.id);
        object.insert(QStringLiteral("mediaPath"), clip.mediaPath);
        object.insert(QStringLiteral("startMs"), static_cast<double>(clip.startMs));
        object.insert(QStringLiteral("endMs"), static_cast<double>(clip.endMs));
        object.insert(QStringLiteral("title"), clip.title);
        object.insert(QStringLiteral("note"), clip.note);
        object.insert(QStringLiteral("thumbKey"), clip.thumbKey);
        object.insert(QStringLiteral("tags"), tagArray);
        object.insert(QStringLiteral("folderIds"), folderArray);
        object.insert(QStringLiteral("created"), clip.created.toString(Qt::ISODateWithMs));
        object.insert(QStringLiteral("updated"), clip.updated.toString(Qt::ISODateWithMs));
        clipArray.append(object);
    }

    QJsonArray folderArray;
    for (const LoopFolder& folder : m_folders)
    {
        QJsonObject object;
        object.insert(QStringLiteral("id"), folder.id);
        object.insert(QStringLiteral("name"), folder.name);
        object.insert(QStringLiteral("created"), folder.created.toString(Qt::ISODateWithMs));
        folderArray.append(object);
    }

    QJsonArray tagArray;
    for (const QString& tag : m_tags)
        tagArray.append(tag);

    QJsonObject root;
    root.insert(QStringLiteral("version"), kLoopStoreVersion);
    root.insert(QStringLiteral("folders"), folderArray);
    root.insert(QStringLiteral("tags"), tagArray);
    root.insert(QStringLiteral("clips"), clipArray);

    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}

bool LoopStore::containsClip(const QString& clipId) const
{
    return indexOfClip(clipId) >= 0;
}

int LoopStore::indexOfClip(const QString& clipId) const
{
    for (int i = 0; i < m_clips.size(); ++i)
    {
        if (m_clips.at(i).id == clipId)
            return i;
    }
    return -1;
}

LoopClip LoopStore::clip(const QString& clipId) const
{
    const int index = indexOfClip(clipId);
    return index >= 0 ? m_clips.at(index) : LoopClip();
}

QList<LoopClip> LoopStore::clipsForMedia(const QString& mediaPath) const
{
    const QString key = PathUtils::normalizeMediaPath(mediaPath);
    QList<LoopClip> result;
    if (key.isEmpty())
        return result;
    for (const LoopClip& item : m_clips)
    {
        if (PathUtils::normalizeMediaPath(item.mediaPath) == key)
            result.append(item);
    }
    return result;
}

QString LoopStore::addClip(const LoopClip& clip, QString* error)
{
    const QString problem = LoopClipRules::validateClip(clip);
    if (!problem.isEmpty())
    {
        if (error)
            *error = problem;
        return QString();
    }

    QStringList existingTitles;
    existingTitles.reserve(m_clips.size());
    for (const LoopClip& item : m_clips)
        existingTitles.append(item.title);

    LoopClip stored = clip;
    stored.mediaPath = QDir::cleanPath(QFileInfo(clip.mediaPath).absoluteFilePath());
    stored.tags = normalizedTags(clip.tags);
    for (const QString& tag : stored.tags)
    {
        if (tag.size() > kMaxTagLength)
        {
            if (error)
                *error = LT("标签「%1」不能超过 %2 个字符")
                             .arg(tag)
                             .arg(kMaxTagLength);
            return QString();
        }
    }
    if (m_tags.size() + stored.tags.size() > kMaxTagCount)
    {
        if (error)
            *error = LT("标签数量已达上限（%1 个）").arg(kMaxTagCount);
        return QString();
    }

    stored.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    stored.title = stored.title.trimmed();
    if (stored.title.isEmpty())
        stored.title = LoopClipRules::defaultTitle(stored.mediaPath, stored.startMs, stored.endMs);
    stored.thumbKey = thumbnailKeyFor(stored.mediaPath, stored.startMs);
    stored.created = QDateTime::currentDateTime();
    stored.updated = stored.created;

    // Only keep folder ids that really exist.
    for (int i = stored.folderIds.size() - 1; i >= 0; --i)
    {
        if (!containsFolder(stored.folderIds.at(i)))
            stored.folderIds.removeAt(i);
    }
    stored.folderIds.removeDuplicates();

    m_clips.append(stored);
    for (const QString& tag : stored.tags)
        registerTag(tag);
    return stored.id;
}

bool LoopStore::updateClip(const LoopClip& clip, QString* error)
{
    const int index = indexOfClip(clip.id);
    if (index < 0)
    {
        if (error)
            *error = LT("循环片段不存在");
        return false;
    }

    LoopClip updated = clip;
    updated.tags = normalizedTags(clip.tags);
    const QString problem = LoopClipRules::validateClip(updated);
    if (!problem.isEmpty())
    {
        if (error)
            *error = problem;
        return false;
    }

    updated.mediaPath = QDir::cleanPath(QFileInfo(clip.mediaPath).absoluteFilePath());
    updated.title = updated.title.trimmed();
    if (updated.title.isEmpty())
        updated.title = LoopClipRules::defaultTitle(updated.mediaPath, updated.startMs,
                                                    updated.endMs);
    updated.thumbKey = thumbnailKeyFor(updated.mediaPath, updated.startMs);
    updated.created = m_clips.at(index).created;
    updated.updated = QDateTime::currentDateTime();

    for (int i = updated.folderIds.size() - 1; i >= 0; --i)
    {
        if (!containsFolder(updated.folderIds.at(i)))
            updated.folderIds.removeAt(i);
    }
    updated.folderIds.removeDuplicates();

    m_clips[index] = updated;
    for (const QString& tag : updated.tags)
        registerTag(tag);
    return true;
}

bool LoopStore::removeClip(const QString& clipId, QString* error)
{
    const int index = indexOfClip(clipId);
    if (index < 0)
    {
        if (error)
            *error = LT("循环片段不存在");
        return false;
    }
    m_clips.removeAt(index);
    return true;
}

bool LoopStore::setClipFolder(const QString& clipId, const QString& folderId, bool member,
                              QString* error)
{
    const int index = indexOfClip(clipId);
    if (index < 0)
    {
        if (error)
            *error = LT("循环片段不存在");
        return false;
    }
    if (member && !containsFolder(folderId))
    {
        if (error)
            *error = LT("收藏夹不存在");
        return false;
    }

    QStringList& folderIds = m_clips[index].folderIds;
    const bool contains = folderIds.contains(folderId);
    if (member == contains)
        return true;
    if (member)
        folderIds.append(folderId);
    else
        folderIds.removeAll(folderId);
    return true;
}

bool LoopStore::containsFolder(const QString& folderId) const
{
    return indexOfFolder(folderId) >= 0;
}

int LoopStore::indexOfFolder(const QString& folderId) const
{
    for (int i = 0; i < m_folders.size(); ++i)
    {
        if (m_folders.at(i).id == folderId)
            return i;
    }
    return -1;
}

LoopFolder LoopStore::folder(const QString& folderId) const
{
    const int index = indexOfFolder(folderId);
    return index >= 0 ? m_folders.at(index) : LoopFolder();
}

QString LoopStore::folderName(const QString& folderId) const
{
    const int index = indexOfFolder(folderId);
    return index >= 0 ? m_folders.at(index).name : QString();
}

QString LoopStore::createFolder(const QString& name, QString* error)
{
    if (m_folders.size() >= kMaxFolderCount)
    {
        if (error)
            *error = LT("收藏夹数量已达上限（%1 个）").arg(kMaxFolderCount);
        return QString();
    }

    QStringList existing;
    existing.reserve(m_folders.size());
    for (const LoopFolder& folder : m_folders)
        existing.append(folder.name);

    const QString validated = validateName(name, existing, error);
    if (validated.isEmpty())
        return QString();

    LoopFolder folder;
    folder.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    folder.name = validated;
    folder.created = QDateTime::currentDateTime();
    m_folders.append(folder);
    return folder.id;
}

bool LoopStore::renameFolder(const QString& folderId, const QString& name, QString* error)
{
    const int index = indexOfFolder(folderId);
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

bool LoopStore::removeFolder(const QString& folderId, QString* error)
{
    const int index = indexOfFolder(folderId);
    if (index < 0)
    {
        if (error)
            *error = LT("收藏夹不存在");
        return false;
    }
    m_folders.removeAt(index);
    for (LoopClip& clip : m_clips)
        clip.folderIds.removeAll(folderId);
    return true;
}

bool LoopStore::moveFolder(const QString& folderId, int delta, QString* error)
{
    const int index = indexOfFolder(folderId);
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
            *error = LT("收藏夹已经在%1").arg(
                delta < 0 ? LT("最前") : LT("最后"));
        return false;
    }

    m_folders.move(index, target);
    return true;
}

int LoopStore::folderClipCount(const QString& folderId) const
{
    int count = 0;
    for (const LoopClip& clip : m_clips)
    {
        if (clip.folderIds.contains(folderId))
            ++count;
    }
    return count;
}

int LoopStore::indexOfTag(const QString& tag) const
{
    const QString trimmed = tag.trimmed();
    for (int i = 0; i < m_tags.size(); ++i)
    {
        if (m_tags.at(i).compare(trimmed, Qt::CaseInsensitive) == 0)
            return i;
    }
    return -1;
}

void LoopStore::registerTag(const QString& tag)
{
    const QString trimmed = tag.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > kMaxTagLength)
        return;
    if (indexOfTag(trimmed) >= 0 || m_tags.size() >= kMaxTagCount)
        return;
    m_tags.append(trimmed);
}

void LoopStore::registerClipTags()
{
    for (const LoopClip& clip : m_clips)
    {
        for (const QString& tag : clip.tags)
            registerTag(tag);
    }
}

QStringList LoopStore::tagsByUsage() const
{
    QStringList result = m_tags;
    std::stable_sort(result.begin(), result.end(), [this](const QString& left,
                                                          const QString& right) {
        const int leftUsage = tagUsage(left);
        const int rightUsage = tagUsage(right);
        if (leftUsage != rightUsage)
            return leftUsage > rightUsage;
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    return result;
}

int LoopStore::tagUsage(const QString& tag) const
{
    const QString trimmed = tag.trimmed();
    int count = 0;
    for (const LoopClip& clip : m_clips)
    {
        for (const QString& clipTag : clip.tags)
        {
            if (clipTag.compare(trimmed, Qt::CaseInsensitive) == 0)
            {
                ++count;
                break;
            }
        }
    }
    return count;
}

bool LoopStore::renameTag(const QString& from, const QString& to, QString* error)
{
    const QString source = from.trimmed();
    const QString target = to.trimmed();
    if (source.isEmpty())
    {
        if (error)
            *error = LT("标签不存在");
        return false;
    }
    if (target.isEmpty())
    {
        if (error)
            *error = LT("标签名称不能为空");
        return false;
    }
    if (target.size() > kMaxTagLength)
    {
        if (error)
            *error = LT("标签不能超过 %1 个字符").arg(kMaxTagLength);
        return false;
    }

    const int sourceIndex = indexOfTag(source);
    if (sourceIndex < 0)
    {
        if (error)
            *error = LT("标签不存在");
        return false;
    }
    const QString previous = m_tags.at(sourceIndex);

    if (source.compare(target, Qt::CaseInsensitive) == 0)
    {
        // Case-only rename keeps the new spelling.
        m_tags[sourceIndex] = target;
        for (LoopClip& clip : m_clips)
        {
            for (QString& tag : clip.tags)
            {
                if (tag.compare(previous, Qt::CaseInsensitive) == 0)
                    tag = target;
            }
        }
        return true;
    }

    const bool targetExists = indexOfTag(target) >= 0;
    if (targetExists)
    {
        // Merging into an existing tag simply drops the source entry.
        m_tags.removeAt(sourceIndex);
    }
    else
    {
        if (m_tags.size() >= kMaxTagCount)
        {
            if (error)
                *error = LT("标签数量已达上限（%1 个）").arg(kMaxTagCount);
            return false;
        }
        m_tags[sourceIndex] = target;
    }

    for (LoopClip& clip : m_clips)
    {
        QStringList renamed;
        for (const QString& tag : clip.tags)
        {
            const QString value = tag.compare(previous, Qt::CaseInsensitive) == 0 ? target : tag;
            const bool duplicate =
                std::any_of(renamed.cbegin(), renamed.cend(), [&value](const QString& item) {
                    return item.compare(value, Qt::CaseInsensitive) == 0;
                });
            if (!duplicate)
                renamed.append(value);
        }
        clip.tags = renamed;
    }
    return true;
}

bool LoopStore::removeTag(const QString& tag, QString* error)
{
    const QString trimmed = tag.trimmed();
    const int index = indexOfTag(trimmed);
    if (index < 0)
    {
        if (error)
            *error = LT("标签不存在");
        return false;
    }

    const QString removed = m_tags.at(index);
    m_tags.removeAt(index);
    for (LoopClip& clip : m_clips)
    {
        for (int i = clip.tags.size() - 1; i >= 0; --i)
        {
            if (clip.tags.at(i).compare(removed, Qt::CaseInsensitive) == 0)
                clip.tags.removeAt(i);
        }
    }
    return true;
}

int LoopStore::pruneUnusedTags()
{
    int removed = 0;
    for (int i = m_tags.size() - 1; i >= 0; --i)
    {
        if (tagUsage(m_tags.at(i)) == 0)
        {
            m_tags.removeAt(i);
            ++removed;
        }
    }
    return removed;
}

int LoopStore::missingClipCount(const QSet<QString>& availablePaths) const
{
    int missing = 0;
    for (const LoopClip& clip : m_clips)
    {
        const QString key = PathUtils::normalizeMediaPath(clip.mediaPath);
        if (key.isEmpty() || !availablePaths.contains(key))
            ++missing;
    }
    return missing;
}

int LoopStore::pruneMissingClips(const QSet<QString>& availablePaths)
{
    int removed = 0;
    for (int i = m_clips.size() - 1; i >= 0; --i)
    {
        const QString key = PathUtils::normalizeMediaPath(m_clips.at(i).mediaPath);
        if (key.isEmpty() || !availablePaths.contains(key))
        {
            m_clips.removeAt(i);
            ++removed;
        }
    }
    if (removed > 0)
        pruneUnusedTags();
    return removed;
}

QSet<QString> LoopStore::mediaPaths() const
{
    QSet<QString> paths;
    for (const LoopClip& clip : m_clips)
    {
        const QString key = PathUtils::normalizeMediaPath(clip.mediaPath);
        if (!key.isEmpty())
            paths.insert(key);
    }
    return paths;
}

int LoopStore::countForMediaPaths(const QSet<QString>& staleKeys) const
{
    if (staleKeys.isEmpty())
        return 0;

    int count = 0;
    for (const LoopClip& clip : m_clips)
    {
        const QString key = PathUtils::normalizeMediaPath(clip.mediaPath);
        if (!key.isEmpty() && staleKeys.contains(key))
            ++count;
    }
    return count;
}

int LoopStore::pruneMediaPaths(const QSet<QString>& staleKeys)
{
    if (staleKeys.isEmpty())
        return 0;

    int removed = 0;
    for (int i = m_clips.size() - 1; i >= 0; --i)
    {
        const QString key = PathUtils::normalizeMediaPath(m_clips.at(i).mediaPath);
        if (!key.isEmpty() && staleKeys.contains(key))
        {
            m_clips.removeAt(i);
            ++removed;
        }
    }
    return removed;
}

int LoopStore::removeMediaPath(const QString& mediaPath)
{
    const QString target = PathUtils::normalizeMediaPath(mediaPath);
    if (target.isEmpty())
        return 0;

    int removed = 0;
    for (int i = m_clips.size() - 1; i >= 0; --i)
    {
        if (PathUtils::normalizeMediaPath(m_clips.at(i).mediaPath) == target)
        {
            m_clips.removeAt(i);
            ++removed;
        }
    }
    return removed;
}

QSet<QString> LoopStore::tagsOfMediaPaths(const QSet<QString>& keys) const
{
    QSet<QString> tags;
    if (keys.isEmpty())
        return tags;

    for (const LoopClip& clip : m_clips)
    {
        if (!keys.contains(PathUtils::normalizeMediaPath(clip.mediaPath)))
            continue;
        for (const QString& tag : clip.tags)
            tags.insert(tag);
    }
    return tags;
}

int LoopStore::relinkMediaPath(const QString& oldPath, const QString& newPath)
{
    const QString oldKey = PathUtils::normalizeMediaPath(oldPath);
    const QString newKey = PathUtils::normalizeMediaPath(newPath);
    if (oldKey.isEmpty() || newKey.isEmpty() || oldKey == newKey)
        return 0;

    int changed = 0;
    for (LoopClip& clip : m_clips)
    {
        if (PathUtils::normalizeMediaPath(clip.mediaPath) != oldKey)
            continue;
        clip.mediaPath = newPath;
        clip.updated = QDateTime::currentDateTime();
        ++changed;
    }
    return changed;
}

void LoopStore::clear()
{
    m_clips.clear();
    m_folders.clear();
    m_tags.clear();
}

QString LoopStore::thumbnailKeyFor(const QString& mediaPath, qint64 startMs)
{
    // Folder-free on purpose: reorganising the library must not invalidate the
    // frame that was grabbed at the A point.
    const QString name = QFileInfo(mediaPath).fileName().toLower();
    if (name.isEmpty())
        return QString();
    return QStringLiteral("%1@%2").arg(name).arg(std::max<qint64>(0, startMs));
}

QString LoopStore::validateName(const QString& name, const QStringList& existingNames,
                                QString* error)
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
        return reject(LT("收藏夹名称不能超过 %1 个字符").arg(kMaxNameLength));
    for (const QString& existing : existingNames)
    {
        if (existing.compare(trimmed, Qt::CaseInsensitive) == 0)
            return reject(LT("已存在同名收藏夹「%1」").arg(trimmed));
    }

    if (error)
        error->clear();
    return trimmed;
}

QString LoopStore::validateTag(const QString& tag, QString* error)
{
    const QString trimmed = tag.trimmed();
    if (trimmed.isEmpty())
    {
        if (error)
            *error = LT("标签不能为空");
        return QString();
    }
    if (trimmed.size() > kMaxTagLength)
    {
        if (error)
            *error = LT("标签不能超过 %1 个字符").arg(kMaxTagLength);
        return QString();
    }
    if (error)
        error->clear();
    return trimmed;
}

int LoopStore::maxNameLength()
{
    return kMaxNameLength;
}

int LoopStore::maxFolderCount()
{
    return kMaxFolderCount;
}

int LoopStore::maxTagLength()
{
    return kMaxTagLength;
}

int LoopStore::maxTagCount()
{
    return kMaxTagCount;
}

bool LoopStore::markFileCorrupted()
{
    const QString backup = m_filePath + QStringLiteral(".bad");
    QFile::remove(backup);
    QFile::rename(m_filePath, backup);
    return false;
}
