#include "core/AppLogger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

#include <atomic>

namespace
{
QMutex g_mutex;
QString g_logPath;
QString g_logDirectory;
QFile* g_file = nullptr;
int g_pendingLines = 0;
std::atomic<bool> g_tcodeDetail{true};

// A session with a device attached wrote 4.6 MB in a couple of hours, most of
// it per-command lines, so the old 5 MB cap rotated the interesting part away
// within minutes. The cap is now high enough to keep a full session, and the
// per-command lines can be switched off entirely.
constexpr qint64 kMaxLogBytes = 32 * 1024 * 1024;
// Per-command lines are buffered and written out at this cadence instead of
// every N lines, so a burst cannot turn into a disk write on the GUI thread.
constexpr qint64 kFlushIntervalMs = 250;

QString findProjectRoot(const QString& startDirectory)
{
    QDir dir(startDirectory);
    for (int depth = 0; depth < 6; ++depth)
    {
        if (QFileInfo::exists(dir.filePath(QStringLiteral("CMakeLists.txt")))
            || QFileInfo::exists(dir.filePath(QStringLiteral(".nekobeat-root"))))
        {
            return dir.absolutePath();
        }
        if (!dir.cdUp())
            break;
    }
    return {};
}
}

void AppLogger::setTcodeDetailEnabled(bool enabled)
{
    g_tcodeDetail.store(enabled);
}

bool AppLogger::tcodeDetailEnabled()
{
    return g_tcodeDetail.load();
}

void AppLogger::init()
{
    QMutexLocker locker(&g_mutex);
    if (g_file)
        return;

    QString directory = QString::fromLocal8Bit(qgetenv("NEKOBEAT_LOG_DIR"));
    if (directory.isEmpty())
    {
        const QString projectRoot = findProjectRoot(QCoreApplication::applicationDirPath());
        if (!projectRoot.isEmpty())
            directory = projectRoot + QStringLiteral("/logs");
        else
            directory = QCoreApplication::applicationDirPath() + QStringLiteral("/logs");
    }

    QDir dir(directory);
    dir.mkpath(QStringLiteral("."));

    g_logDirectory = directory;
    g_logPath = directory + QStringLiteral("/NekoBeat.log");

    QFile existing(g_logPath);
    if (existing.exists() && existing.size() > kMaxLogBytes)
    {
        const QString rotated = g_logPath + QStringLiteral(".old");
        QFile::remove(rotated);
        QFile::rename(g_logPath, rotated);
    }

    g_file = new QFile(g_logPath);
    g_file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
}

void AppLogger::log(const QString& category, const QString& message)
{
    const bool isTcodeDetail = category == QStringLiteral("tcode");
    if (isTcodeDetail && !g_tcodeDetail.load())
        return;

    init();

    QMutexLocker locker(&g_mutex);
    if (!g_file || !g_file->isOpen())
        return;

    static QElapsedTimer flushClock;
    static bool flushClockRunning = false;
    if (!flushClockRunning)
    {
        flushClock.start();
        flushClockRunning = true;
    }

    const QByteArray line = (QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
                             + QStringLiteral(" [") + category + QStringLiteral("] ")
                             + message + QLatin1Char('\n'))
                                .toUtf8();
    g_file->write(line);
    ++g_pendingLines;

    // Anything that is not a per-command line is flushed right away, so
    // warnings, errors and state changes are always on disk.
    if (!isTcodeDetail || flushClock.elapsed() >= kFlushIntervalMs)
    {
        g_file->flush();
        g_pendingLines = 0;
        flushClock.restart();
    }

    if (g_file->size() > kMaxLogBytes)
    {
        const QString rotated = g_logPath + QStringLiteral(".old");
        g_file->close();
        delete g_file;
        g_file = nullptr;
        QFile::remove(rotated);
        QFile::rename(g_logPath, rotated);
        g_file = new QFile(g_logPath);
        g_file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    }
}

void AppLogger::shutdown()
{
    QMutexLocker locker(&g_mutex);
    if (!g_file)
        return;
    g_file->flush();
    g_file->close();
    delete g_file;
    g_file = nullptr;
}

QString AppLogger::logFilePath()
{
    init();
    QMutexLocker locker(&g_mutex);
    return g_logPath;
}

QString AppLogger::logDirectory()
{
    init();
    QMutexLocker locker(&g_mutex);
    return g_logDirectory;
}
