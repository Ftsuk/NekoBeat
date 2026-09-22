#include "TestLanguage.h"
#include "media/MediaProbe.h"
#include "media/MediaProbeQueue.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

namespace
{
QString fixturePath(const QString& relative)
{
    const QString root = QString::fromLocal8Bit(qgetenv("NEKOBEAT_TEST_FIXTURES"));
    return root.isEmpty() ? QString() : QDir(root).absoluteFilePath(relative);
}
}

class MediaProbeTest : public QObject
{
    Q_OBJECT

private slots:
    void composeThumbnailLetterboxes()
    {
        QImage source(320, 240, QImage::Format_RGB32);
        source.fill(QColor(0xC0, 0x20, 0x20));

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString output = dir.filePath(QStringLiteral("thumb.jpg"));
        QVERIFY(MediaProbe::composeThumbnail(source, output));

        const QImage result(output);
        QCOMPARE(result.size(), QSize(400, 225));
        QVERIFY(MediaProbe::looksBlack(result) == false);
        QVERIFY(QColor(result.pixel(200, 112)).red() > 150);
    }

    void findLocalCoverPrefersVideoName()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString video = dir.filePath(QStringLiteral("clip.mp4"));
        const QString sidecar = dir.filePath(QStringLiteral("clip.jpg"));

        QFile videoFile(video);
        QVERIFY(videoFile.open(QIODevice::WriteOnly));
        videoFile.write("not a real video");
        videoFile.close();

        QImage image(64, 64, QImage::Format_RGB32);
        image.fill(Qt::blue);
        QVERIFY(image.save(sidecar, "JPG"));

        QCOMPARE(MediaProbe::findLocalCover(video), sidecar);
    }

    void probeReadsMetadataAndRendersThumbnail()
    {
        const QString media = fixturePath(
            QStringLiteral("[DeliriumBear] Marin - NTR 2/[DeliriumBear] Marin - NTR 2.mp4"));
        if (media.isEmpty() || !QFileInfo::exists(media))
            QSKIP("测试视频不存在");

        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        MediaProbeOptions options;
        options.thumbnailPath = dir.filePath(QStringLiteral("thumb.jpg"));
        options.temporaryRoot = dir.path();
        options.timeoutMs = 20000;

        const MediaProbeResult result = MediaProbe::probe(media, options);
        QVERIFY2(result.metadataOk, qPrintable(result.error));
        QVERIFY(result.durationMs > 0);
        QVERIFY(result.width > 0);
        QVERIFY(result.height > 0);
        QVERIFY(!result.videoCodec.isEmpty());
        QVERIFY(!result.container.isEmpty());
        QVERIFY(result.thumbnailOk);

        const QImage thumbnail(options.thumbnailPath);
        QCOMPARE(thumbnail.size(), QSize(400, 225));
    }

    void probeMetadataOnlyDoesNotRequireFrame()
    {
        const QString media = fixturePath(
            QStringLiteral("[MMD] Loli mettyamottya-Hirotakerご指名解禁腰振りダンス/"
                           "[MMD] Loli mettyamottya-Hirotakerご指名解禁腰振りダンス.mp4"));
        if (media.isEmpty() || !QFileInfo::exists(media))
            QSKIP("测试视频不存在");

        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        MediaProbeOptions options;
        options.temporaryRoot = dir.path();
        options.timeoutMs = 20000;

        const MediaProbeResult result = MediaProbe::probe(media, options);
        QVERIFY2(result.metadataOk, qPrintable(result.error));
        QVERIFY(result.durationMs > 0);
        QVERIFY(result.width > 0);
    }

    void differentSeekRatiosProduceDifferentFrames()
    {
        const QString media = fixturePath(
            QStringLiteral("[DeliriumBear] Marin - NTR 2/[DeliriumBear] Marin - NTR 2.mp4"));
        if (media.isEmpty() || !QFileInfo::exists(media))
            QSKIP("测试视频不存在");

        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        MediaProbeOptions first;
        first.thumbnailPath = dir.filePath(QStringLiteral("first.jpg"));
        first.temporaryRoot = dir.path();
        first.timeoutMs = 20000;
        first.seekRatio = 0.05;

        MediaProbeOptions second = first;
        second.thumbnailPath = dir.filePath(QStringLiteral("second.jpg"));
        second.seekRatio = 0.8;

        const MediaProbeResult firstResult = MediaProbe::probe(media, first);
        const MediaProbeResult secondResult = MediaProbe::probe(media, second);
        QVERIFY2(firstResult.thumbnailOk, qPrintable(firstResult.error));
        QVERIFY2(secondResult.thumbnailOk, qPrintable(secondResult.error));

        const QImage firstImage(first.thumbnailPath);
        const QImage secondImage(second.thumbnailPath);
        QVERIFY(!firstImage.isNull());
        QVERIFY(!secondImage.isNull());
        QVERIFY2(firstImage != secondImage,
                 "不同取帧位置应该得到不同的缩略图");
    }

    void queueAcceptsReenqueueAfterPendingCleared()
    {
        const QString media = fixturePath(
            QStringLiteral("[DeliriumBear] Marin - NTR 2/[DeliriumBear] Marin - NTR 2.mp4"));
        if (media.isEmpty() || !QFileInfo::exists(media))
            QSKIP("测试视频不存在");

        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        MediaProbeQueue queue;
        queue.setConcurrency(1);
        queue.setTemporaryRoot(dir.path());

        // The real probe occupies the single worker; the missing file stays in
        // the pending queue until we drop it.
        const QString pendingPath = media + QStringLiteral(".pending");
        queue.enqueue(media, dir.filePath(QStringLiteral("keep.jpg")));
        queue.enqueue(pendingPath, dir.filePath(QStringLiteral("drop.jpg")));
        queue.clearPending();
        QCOMPARE(queue.pendingCount(), 0);

        // Dropping the pending job must release its "already queued" marker,
        // otherwise this enqueue is ignored and the thumbnail never appears.
        queue.enqueue(pendingPath, dir.filePath(QStringLiteral("again.jpg")));
        QCOMPARE(queue.totalCount(), 1);

        QSignalSpy idleSpy(&queue, &MediaProbeQueue::idle);
        queue.clearPending();
        QVERIFY(idleSpy.wait(60000));
    }
};

QTEST_MAIN(MediaProbeTest)
#include "test_media_probe.moc"
