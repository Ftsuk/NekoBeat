#include "core/StoreBackup.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace
{
QString backupPathFor(const QString& filePath, const QDateTime& stamp)
{
    const QFileInfo info(filePath);
    return QDir(info.absolutePath())
        .absoluteFilePath(QStringLiteral("%1.%2.bak")
                              .arg(info.completeBaseName(),
                                   stamp.toString(QStringLiteral("yyyyMMdd-HHmmss"))));
}

// Newest last: the timestamp sorts lexicographically.
QStringList backupsOf(const QString& filePath)
{
    const QFileInfo info(filePath);
    const QDir dir(info.absolutePath());
    const QString pattern = info.completeBaseName() + QStringLiteral(".*.bak");

    QStringList result;
    for (const QFileInfo& entry : dir.entryInfoList({pattern}, QDir::Files, QDir::Name))
        result.append(entry.absoluteFilePath());
    return result;
}
}

QStringList StoreBackup::backupFiles(const QStringList& filePaths, int keepCopies)
{
    QStringList created;
    const QDateTime stamp = QDateTime::currentDateTime();

    for (const QString& path : filePaths)
    {
        if (path.isEmpty() || !QFileInfo::exists(path))
            continue;

        QString target = backupPathFor(path, stamp);
        // Two backups inside the same second must not overwrite each other.
        for (int attempt = 1; QFileInfo::exists(target) && attempt < 100; ++attempt)
        {
            const QFileInfo info(path);
            target = QDir(info.absolutePath())
                         .absoluteFilePath(QStringLiteral("%1.%2-%3.bak")
                                               .arg(info.completeBaseName(),
                                                    stamp.toString(
                                                        QStringLiteral("yyyyMMdd-HHmmss")))
                                               .arg(attempt));
        }

        if (QFile::copy(path, target))
            created.append(target);

        pruneBackups(path, qMax(1, keepCopies));
    }
    return created;
}

int StoreBackup::pruneBackups(const QString& filePath, int keepCopies)
{
    if (filePath.isEmpty() || keepCopies < 0)
        return 0;

    QStringList backups = backupsOf(filePath);
    int removed = 0;
    while (backups.size() > keepCopies)
    {
        QFile::remove(backups.takeFirst());
        ++removed;
    }
    return removed;
}
