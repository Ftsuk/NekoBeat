#include "core/PathUtils.h"

#include <QDir>
#include <QFileInfo>

QString PathUtils::normalizeMediaPath(const QString& path)
{
    if (path.trimmed().isEmpty())
        return QString();
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower();
}
