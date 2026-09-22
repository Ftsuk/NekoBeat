#include "core/Loc.h"

#include <QSettings>

#include <atomic>

namespace Loc
{
namespace
{

constexpr const char* kSettingsKey = "ui/language";

// 设备链路跑在自己的线程里，也会取词（串口错误、连接状态）；读的写的是同一个
// 变量，所以用原子量而不是普通枚举，免得切换语言时留下数据竞争。
std::atomic<Language> g_language{Language::English};

} // namespace

Language language()
{
    return g_language.load(std::memory_order_relaxed);
}

void setLanguage(Language languageIn)
{
    g_language.store(languageIn, std::memory_order_relaxed);
}

QString languageCode(Language languageIn)
{
    return languageIn == Language::Chinese ? QStringLiteral("zh") : QStringLiteral("en");
}

Language languageFromCode(const QString& code, Language fallback)
{
    const QString normalized = code.trimmed().toLower();
    if (normalized.startsWith(QStringLiteral("zh")))
        return Language::Chinese;
    if (normalized.startsWith(QStringLiteral("en")))
        return Language::English;
    return fallback;
}

void loadFromSettings()
{
    QSettings settings;
    // 默认英文：没有存过设置时（首次启动）按英文界面启动。
    g_language = languageFromCode(settings.value(QString::fromLatin1(kSettingsKey),
                                                 QStringLiteral("en"))
                                      .toString(),
                                  Language::English);
}

void saveToSettings()
{
    QSettings settings;
    settings.setValue(QString::fromLatin1(kSettingsKey), languageCode(g_language));
    // 切换语言后要立刻重启，新进程必须马上读到新值，不能等 QSettings 自己
    // 找个时机再落盘。
    settings.sync();
}

QString t(const QString& source)
{
    if (g_language == Language::Chinese)
        return source;
    const QString& translated = lookupEnglish(source);
    return translated.isNull() ? source : translated;
}

} // namespace Loc
