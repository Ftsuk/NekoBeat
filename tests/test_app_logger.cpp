#include "TestLanguage.h"
#include "core/AppLogger.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

// The per-command [tcode] lines are the biggest source of log volume and can be
// switched off; everything else must keep being written.
class AppLoggerTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("NEKOBEAT_LOG_DIR", m_dir.path().toLocal8Bit());
    }

    void detailOffKeepsStateLinesOnly()
    {
        AppLogger::setTcodeDetailEnabled(false);
        AppLogger::log(QStringLiteral("tcode"), QStringLiteral("L05000I100 L15000I100"));
        AppLogger::log(QStringLiteral("sync"), QStringLiteral("联动统计：1 条命令 / 5000 ms"));
        AppLogger::shutdown();

        const QString text = readLog();
        QVERIFY2(!text.contains(QStringLiteral("L05000I100")), "关闭后不得写入逐条 TCode 命令");
        QVERIFY2(text.contains(QStringLiteral("联动统计")), "状态类日志必须仍然写入");
    }

    void detailOnWritesEveryCommand()
    {
        AppLogger::setTcodeDetailEnabled(true);
        AppLogger::log(QStringLiteral("tcode"), QStringLiteral("L05000I200"));
        AppLogger::shutdown();

        QVERIFY(readLog().contains(QStringLiteral("L05000I200")));
    }

    void switchBackOffStopsWritingAgain()
    {
        AppLogger::setTcodeDetailEnabled(false);
        AppLogger::log(QStringLiteral("tcode"), QStringLiteral("R00000I300"));
        AppLogger::shutdown();

        QVERIFY(!readLog().contains(QStringLiteral("R00000I300")));
        QVERIFY(AppLogger::tcodeDetailEnabled() == false);
    }

private:
    QString readLog() const
    {
        QFile file(QDir(m_dir.path()).absoluteFilePath(QStringLiteral("NekoBeat.log")));
        if (!file.open(QIODevice::ReadOnly))
            return QString();
        return QString::fromUtf8(file.readAll());
    }

    QTemporaryDir m_dir;
};

QTEST_GUILESS_MAIN(AppLoggerTest)

#include "test_app_logger.moc"
