#pragma once

#include <QString>

class AppLogger
{
public:
    static void init();
    static void shutdown();
    static void log(const QString& category, const QString& message);
    // Per-command diagnostics ([tcode] lines) are the single biggest source of
    // log volume - over half of a typical session - and are only needed while
    // diagnosing the link. While this is off, dense summaries still report the
    // command rate and the worst tick gap.
    static void setTcodeDetailEnabled(bool enabled);
    static bool tcodeDetailEnabled();
    static QString logFilePath();
    static QString logDirectory();
};
