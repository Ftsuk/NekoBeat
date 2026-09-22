#pragma once

#include <QString>
#include <QStringList>

// Copies user data aside before a destructive operation. A favourite list lost
// to one mis-click is not recoverable; a timestamped copy next to the original
// is.
namespace StoreBackup
{
// Copies every existing file to "<name>.<yyyyMMdd-HHmmss>.bak" beside it and
// drops the oldest copies so at most keepCopies remain. Returns what was made.
QStringList backupFiles(const QStringList& filePaths, int keepCopies = 5);

// Deletes the oldest timestamped backups of one file, keeping the newest
// keepCopies. Returns how many were removed.
int pruneBackups(const QString& filePath, int keepCopies);
}
