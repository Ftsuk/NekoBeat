#include "ui/MediaLibrary.h"

#include <QDir>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

// The folder scan used to run on the GUI thread - thousands of file-system calls
// during which the picture and (worse) the device were frozen. It now runs in a
// worker, which is observable: right after a rescan is requested the model is
// still empty, and it only fills once the event loop delivers the result.
class MediaLibraryScanTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(m_settingsDir.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("NekoBeatTest"));
        QCoreApplication::setApplicationName(QStringLiteral("NekoBeatScanTest"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());

        QVERIFY(m_mediaDir.isValid());
        QVERIFY(m_cacheDir.isValid());
        // Empty files are enough: the scan only looks at names, sizes and the
        // neighbouring .funscript files.
        for (int i = 0; i < 5; ++i)
        {
            const QString base = QDir(m_mediaDir.path())
                                     .absoluteFilePath(QStringLiteral("clip-%1").arg(i));
            QFile video(base + QStringLiteral(".mp4"));
            QVERIFY(video.open(QIODevice::WriteOnly));
            video.write("x");
            video.close();
        }
        QFile script(QDir(m_mediaDir.path()).absoluteFilePath(QStringLiteral("clip-0.funscript")));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("{\"actions\":[{\"at\":0,\"pos\":0},{\"at\":1000,\"pos\":100}]}");
        script.close();
    }

    void scanRunsOffTheGuiThread()
    {
        MediaLibrary library(nullptr, m_cacheDir.path());

        // addRootFolder() triggers a rescan. If that scan were synchronous the
        // model would already be filled here - without a single event-loop turn.
        library.addRootFolder(m_mediaDir.path());
        QCOMPARE(library.itemCount(), 0);

        QTRY_VERIFY_WITH_TIMEOUT(library.itemCount() >= 5, 15000);
    }

private:
    QTemporaryDir m_settingsDir;
    QTemporaryDir m_mediaDir;
    QTemporaryDir m_cacheDir;
};

QTEST_MAIN(MediaLibraryScanTest)

#include "test_media_library_scan.moc"
