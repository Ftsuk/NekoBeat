#include "TestLanguage.h"
#include "media/MpvEngine.h"

#include <mpv/client.h>

#include <QDir>
#include <QFileInfo>
#include <QtTest>

namespace
{
QString fixturePath(const QString& relative)
{
    const QString root = QString::fromLocal8Bit(qgetenv("NEKOBEAT_TEST_FIXTURES"));
    return root.isEmpty() ? QString() : QDir(root).absoluteFilePath(relative);
}
}

class MediaPlaybackTest : public QObject
{
    Q_OBJECT

private slots:
    void sequentialPlaybackAndSeek()
    {
        const QStringList mediaFiles = {
            fixturePath(QStringLiteral("[Chr] ホタル 2 Collab with ナクル/"
                                       "[Chr] ホタル 2 Collab with ナクル.mp4")),
            fixturePath(QStringLiteral("[DeliriumBear] Marin - NTR 2/"
                                       "[DeliriumBear] Marin - NTR 2.mp4")),
            fixturePath(QStringLiteral("[MMD] Loli mettyamottya-Hirotakerご指名解禁腰振りダンス/"
                                       "[MMD] Loli mettyamottya-Hirotakerご指名解禁腰振りダンス.mp4"))
        };

        for (const QString& path : mediaFiles)
        {
            if (!QFileInfo::exists(path))
                QSKIP("测试视频不存在");
        }

        MpvEngine engine;
        QVERIFY(engine.initialize());
        mpv_set_property_string(engine.handle(), "vo", "null");
        mpv_set_property_string(engine.handle(), "ao", "null");
        mpv_set_property_string(engine.handle(), "speed", "4.0");

        for (const QString& path : mediaFiles)
        {
            engine.loadFile(path);
            engine.play();
            QTRY_VERIFY_WITH_TIMEOUT(engine.durationMs() > 0, 10000);
            QTRY_VERIFY_WITH_TIMEOUT(engine.isPlaying(), 10000);

            const qint64 target = engine.durationMs() / 2;
            engine.seek(target);
            QTest::qWait(800);
            QVERIFY(engine.isPlaying());
            QVERIFY(engine.positionMs() > target - 5000);

            // Playback state comes from mpv's property notifications rather than
            // from polling it (a poll blocks this thread on the playback core
            // lock), so the pause/resume path is part of the contract.
            engine.pause();
            QTRY_VERIFY_WITH_TIMEOUT(!engine.isPlaying(), 5000);
            engine.play();
            QTRY_VERIFY_WITH_TIMEOUT(engine.isPlaying(), 5000);
            engine.pause();
            QTRY_VERIFY_WITH_TIMEOUT(!engine.isPlaying(), 5000);
        }

        engine.stopPlayback();
        QCOMPARE(engine.positionMs(), 0);
        QVERIFY(!engine.isPlaying());
    }

    void endOfFileIsReported()
    {
        const QString path = fixturePath(QStringLiteral("[DeliriumBear] Marin - NTR 2/"
                                                       "[DeliriumBear] Marin - NTR 2.mp4"));
        if (!QFileInfo::exists(path))
            QSKIP("测试视频不存在");

        MpvEngine engine;
        QVERIFY(engine.initialize());
        mpv_set_property_string(engine.handle(), "vo", "null");
        mpv_set_property_string(engine.handle(), "ao", "null");
        mpv_set_property_string(engine.handle(), "speed", "8.0");

        QSignalSpy ended(&engine, &MpvEngine::mediaEnded);
        engine.loadFile(path);
        engine.play();
        QTRY_VERIFY_WITH_TIMEOUT(engine.durationMs() > 0, 10000);

        engine.seek(engine.durationMs() - 1000);
        QTRY_VERIFY_WITH_TIMEOUT(ended.count() > 0, 20000);
        QVERIFY(!engine.isPlaying());
    }
};

QTEST_MAIN(MediaPlaybackTest)
#include "test_media_playback.moc"
