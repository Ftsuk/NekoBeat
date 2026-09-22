#include "core/Loc.h"
#include "ui/MediaCleanupPrompt.h"

#include "core/FavoritesStore.h"
#include "core/LoopStore.h"
#include "core/MediaCleanup.h"

#include <QDir>
#include <QMessageBox>

bool MediaCleanupPrompt::run(QWidget* parent, FavoritesStore* favorites,
                             LoopStore* loops, const QSet<QString>& staleKeys)
{
    if (staleKeys.isEmpty())
    {
        QMessageBox::information(parent, LT("清理失效数据"),
                                 LT("没有找到失效的条目。"));
        return false;
    }

    MediaCleanup cleanup(favorites, loops);
    const MediaCleanupPlan plan = cleanup.plan(staleKeys);
    if (plan.isEmpty())
    {
        QMessageBox::information(parent, LT("清理失效数据"),
                                 LT("没有找到失效的条目。"));
        return false;
    }

    QStringList lines;
    if (plan.favoriteMembers > 0)
    {
        lines.append(LT("· 收藏夹记录 %1 条（涉及 %2 个收藏夹）")
                         .arg(plan.favoriteMembers)
                         .arg(plan.favoriteFolders));
    }
    if (plan.loopClips > 0)
        lines.append(LT("· 循环片段 %1 个").arg(plan.loopClips));
    if (plan.tags > 0)
    {
        lines.append(LT("· 标签 %1 个（已没有其他片段使用）").arg(plan.tags));
    }

    const QMessageBox::StandardButton answer = QMessageBox::question(
        parent, LT("清理失效数据"),
        LT("以下记录将被删除，视频文件本身不会被删除：\n\n%1\n\n清理前会自动备份数据文件。继续？")
            .arg(lines.join(QLatin1Char('\n'))),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return false;

    const QStringList backups = cleanup.backup();
    const MediaCleanupPlan applied = cleanup.apply(staleKeys);

    if (favorites)
        favorites->save();
    if (loops)
        loops->save();

    QString message = LT("已清理 %1 条收藏记录、%2 个循环片段、%3 个标签。")
                          .arg(applied.favoriteMembers)
                          .arg(applied.loopClips)
                          .arg(applied.tags);
    if (!backups.isEmpty())
    {
        message += LT("\n\n备份已保存到：\n%1")
                       .arg(QDir::toNativeSeparators(backups.join(QStringLiteral("\n"))));
    }
    QMessageBox::information(parent, LT("清理完成"), message);
    return true;
}

void MediaCleanupPrompt::warnUnreachable(QWidget* parent, const QStringList& roots)
{
    QMessageBox::warning(
        parent, LT("暂时无法清理"),
        LT("这些位置当前打不开，无法判断里面的视频是真的被删除，还是只是暂时不在：\n\n%1\n\n请先让这些位置可用，再执行清理。")
            .arg(roots.join(QStringLiteral("\n"))));
}
