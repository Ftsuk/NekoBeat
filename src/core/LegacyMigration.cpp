#include "core/LegacyMigration.h"

#include "core/Loc.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QString>

namespace LegacyMigration
{
namespace
{

// 与 FavoritesStore / LoopStore 取漫游目录的方式保持一致，否则算出来的路径
// 和真实数据所在的位置对不上，迁移就会静默失败。
QString roamingBase()
{
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty())
        base = QDir::currentPath();
    return base;
}

// 与 MediaLibraryStore 取本地缓存根的方式保持一致。
QString localBase()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return base;
}

// 把 from 里 to 还没有的文件补过去，返回是否全部成功。
bool copyMissingInto(const QString& from, const QString& to, int* copied)
{
    const QDir source(from);
    if (!source.exists())
        return true;
    if (!QDir().mkpath(to))
        return false;

    bool ok = true;
    for (const QFileInfo& info :
         source.entryInfoList(QDir::Files | QDir::Hidden | QDir::System))
    {
        const QString target = to + QLatin1Char('/') + info.fileName();
        if (QFileInfo::exists(target))
            continue; // 目标里已经有同名文件：保新，不覆盖
        if (QFile::copy(info.absoluteFilePath(), target))
            ++*copied;
        else
            ok = false;
    }
    for (const QFileInfo& info :
         source.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden))
        ok = copyMissingInto(info.absoluteFilePath(),
                             to + QLatin1Char('/') + info.fileName(), copied) && ok;
    return ok;
}

// 把旧目录并进新目录。目标不存在时整体改名（同盘几乎瞬时）；目标已经存在
// （上次迁移只做了一半）或改名失败时补齐缺的文件。只有补全成功才删掉旧目录，
// 数据没搬完之前绝不删。
bool absorbDirectory(const QString& from, const QString& to, int* copied)
{
    if (from.isEmpty() || to.isEmpty() || from == to)
        return false;
    if (!QFileInfo::exists(from))
        return false;

    if (!QFileInfo::exists(to) && QDir().rename(from, to))
        return true;

    if (!copyMissingInto(from, to, copied))
        return false;
    return QDir(from).removeRecursively();
}

// 旧位置的所有设置整体复制过来（包含子组）。只在新位置完全没有设置时做，
// 否则会把用户在新版本里改过的值覆盖掉。
bool migrateSettings()
{
    if (QCoreApplication::organizationName() == legacyName()
        && QCoreApplication::applicationName() == legacyName())
    {
        return false;
    }

    QSettings fresh;
    if (!fresh.allKeys().isEmpty())
        return false;

    QSettings legacy(legacyName(), legacyName());
    const QStringList keys = legacy.allKeys();
    if (keys.isEmpty())
        return false;

    for (const QString& key : keys)
        fresh.setValue(key, legacy.value(key));
    fresh.sync();
    return true;
}

} // namespace

QString legacyName()
{
    return QStringLiteral("ScriptMotion");
}

QStringList run()
{
    QStringList notes;

    if (migrateSettings())
        notes.append(LT("已把旧的界面设置、轴限制与媒体库配置迁移到新名字下"));

    const QString name = QCoreApplication::applicationName();
    const struct
    {
        QString base;
        QString label;
    } locations[] = {
        {roamingBase(), LT("收藏夹与循环片段")},
        {localBase(), LT("缩略图缓存与索引")},
    };

    for (const auto& location : locations)
    {
        if (location.base.isEmpty())
            continue;
        const QString from = QDir(location.base).absoluteFilePath(legacyName());
        const QString to = QDir(location.base).absoluteFilePath(name);
        int copied = 0;
        if (absorbDirectory(from, to, &copied))
        {
            notes.append(copied > 0
                             ? LT("已迁移%1（补入 %2 个文件）")
                                   .arg(location.label)
                                   .arg(copied)
                             : LT("已迁移%1").arg(location.label));
        }
        else if (QFileInfo::exists(from))
        {
            // 搬不动就留着旧目录，只报告，不删任何东西。
            notes.append(LT("旧数据仍在 %1（迁移未完成）").arg(from));
        }
    }

    return notes;
}

} // namespace LegacyMigration
