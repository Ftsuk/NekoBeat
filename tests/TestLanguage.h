#pragma once

#include "core/Loc.h"

// 测试里的断言写的是中文文案，而程序默认语言是英文。这里在 main 之前把
// 词表固定成中文，断言就不会随默认语言或用户设置漂移。
namespace nekobeat_test
{

inline const struct ChineseLanguageGuard
{
    ChineseLanguageGuard() { Loc::setLanguage(Loc::Language::Chinese); }
} g_chineseLanguageGuard;

} // namespace nekobeat_test
