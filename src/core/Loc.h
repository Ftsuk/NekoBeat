#pragma once

#include <QString>

// 界面语言的运行时词表。
//
// 源码里面向用户的文本一律写成中文原文，再用 LT(...) 取词：中文界面下
// LT("关闭") 返回原文，英文界面下返回词表里的英文。这样做的好处是同一处
// 代码只有一份文本、语言切换不影响逻辑字符串（文件格式、JSON 键、日志）。
namespace Loc
{

enum class Language
{
    English = 0,
    Chinese = 1,
};

// 当前语言。默认英文：没有存过设置时按英文启动。
Language language();
void setLanguage(Language language);

// 语言的持久化代号（"en" / "zh"），同时作为 QSettings 的取值。
QString languageCode(Language language);
Language languageFromCode(const QString& code, Language fallback = Language::English);

// 程序启动时读一次；设置界面改动后写回。
void loadFromSettings();
void saveToSettings();

// 中文原文 → 当前语言的文本。英文词表里没有的条目原样返回中文，所以漏译
// 只会退回中文，不会变成空白或乱码。
QString t(const QString& source);

// 词表实现（由 LocTable.cpp 提供）。
const QString& lookupEnglish(const QString& source);

} // namespace Loc

// 标记一段面向用户的文本。
#define LT(text) Loc::t(QStringLiteral(text))
