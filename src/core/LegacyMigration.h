#pragma once

#include <QStringList>

// 把旧产品名（ScriptMotion）留下的设置与数据搬到 NekoBeat 下。
//
// 改名不能顺手把用户的东西清空：轴限制、媒体库文件夹、收藏夹、循环片段、
// 已探到的元数据与缩略图缓存，全都存在以产品名命名的位置。这里在程序启动、
// 任何存储被创建之前跑一次，搬完就不再碰；新位置已经有数据时一律不动。
namespace LegacyMigration
{

// 必须在 QCoreApplication 的 organizationName / applicationName 设置好之后、
// 任何 QSettings 或 AppLogger 初始化之前调用。
// 返回这次实际做了什么，供调用方写进日志（迁移发生在日志可用之前）。
QStringList run();

// 旧产品名。只在本模块里出现，其余代码一律用新名。
QString legacyName();

} // namespace LegacyMigration
