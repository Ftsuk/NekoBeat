#include <QtTest>

#include "core/FavoritesStore.h"
#include "core/LoopStore.h"
#include "core/MediaCleanup.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace
{
QString mediaPath(const QString& name)
{
    return QStringLiteral("C:/media/") + name;
}

LoopClip makeClip(const QString& mediaPathValue, qint64 startMs, const QStringList& tags)
{
    LoopClip clip;
    clip.id = QStringLiteral("clip-") + QString::number(startMs);
    clip.mediaPath = mediaPathValue;
    clip.startMs = startMs;
    clip.endMs = startMs + 5000;
    clip.title = QStringLiteral("T");
    clip.tags = tags;
    return clip;
}
}

class MediaCleanupTest : public QObject
{
    Q_OBJECT

private slots:
    void planCountsEverythingThatWouldGo()
    {
        FavoritesStore favorites(QStringLiteral("C:/tmp/favorites.json"));
        LoopStore loops(QStringLiteral("C:/tmp/loops.json"));
        QString error;

        const QString folder = favorites.createFolder(QStringLiteral("A"), &error);
        const QString other = favorites.createFolder(QStringLiteral("B"), &error);
        QVERIFY(favorites.addMember(folder, mediaPath("gone.mp4"), &error));
        QVERIFY(favorites.addMember(other, mediaPath("gone.mp4"), &error));
        QVERIFY(favorites.addMember(folder, mediaPath("keep.mp4"), &error));

        loops.addClip(makeClip(mediaPath("gone.mp4"), 1000,
                               {QStringLiteral("solo")}),
                      &error);
        loops.addClip(makeClip(mediaPath("gone.mp4"), 9000, {}), &error);
        // "shared" also hangs on a video that stays.
        loops.addClip(makeClip(mediaPath("keep.mp4"), 1000,
                               {QStringLiteral("shared")}),
                      &error);
        loops.addClip(makeClip(mediaPath("gone.mp4"), 5000,
                               {QStringLiteral("shared")}),
                      &error);

        MediaCleanup cleanup(&favorites, &loops);
        const MediaCleanupPlan plan =
            cleanup.plan({FavoritesStore::normalizePath(mediaPath("gone.mp4"))});

        QCOMPARE(plan.favoriteMembers, 2);
        QCOMPARE(plan.favoriteFolders, 2);
        QCOMPARE(plan.loopClips, 3);
        // Only "solo" loses its last user; "shared" is still used by keep.mp4.
        QCOMPARE(plan.tags, 1);
        QCOMPARE(plan.total(), 6);
    }

    void emptySetTouchesNothing()
    {
        FavoritesStore favorites(QStringLiteral("C:/tmp/favorites.json"));
        LoopStore loops(QStringLiteral("C:/tmp/loops.json"));
        QString error;

        const QString folder = favorites.createFolder(QStringLiteral("A"), &error);
        QVERIFY(favorites.addMember(folder, mediaPath("a.mp4"), &error));
        loops.addClip(makeClip(mediaPath("a.mp4"), 1000, {}), &error);

        MediaCleanup cleanup(&favorites, &loops);
        QCOMPARE(cleanup.plan({}).total(), 0);
        QCOMPARE(cleanup.apply({}).total(), 0);
        // The old pruneMissing() would have wiped everything here.
        QCOMPARE(favorites.memberCount(folder), 1);
        QCOMPARE(loops.clipCount(), 1);
    }

    void applyRemovesExactlyTheStaleSet()
    {
        FavoritesStore favorites(QStringLiteral("C:/tmp/favorites.json"));
        LoopStore loops(QStringLiteral("C:/tmp/loops.json"));
        QString error;

        const QString folder = favorites.createFolder(QStringLiteral("A"), &error);
        QVERIFY(favorites.addMember(folder, mediaPath("gone.mp4"), &error));
        QVERIFY(favorites.addMember(folder, mediaPath("keep.mp4"), &error));
        loops.addClip(makeClip(mediaPath("gone.mp4"), 1000, {QStringLiteral("solo")}),
                      &error);
        loops.addClip(makeClip(mediaPath("keep.mp4"), 1000, {QStringLiteral("solo")}),
                      &error);

        MediaCleanup cleanup(&favorites, &loops);
        const QSet<QString> stale = {
            FavoritesStore::normalizePath(mediaPath("gone.mp4"))
        };
        const MediaCleanupPlan applied = cleanup.apply(stale);

        QCOMPARE(applied.favoriteMembers, 1);
        QCOMPARE(applied.loopClips, 1);
        // "solo" survives because the kept clip still carries it.
        QCOMPARE(applied.tags, 0);
        QCOMPARE(favorites.memberCount(folder), 1);
        QVERIFY(favorites.isMember(folder, mediaPath("keep.mp4")));
        QCOMPARE(loops.clipCount(), 1);
        QCOMPARE(loops.tags().size(), 1);
    }

    void relinkMovesFavouritesAndClips()
    {
        FavoritesStore favorites(QStringLiteral("C:/tmp/favorites.json"));
        LoopStore loops(QStringLiteral("C:/tmp/loops.json"));
        QString error;

        const QString folder = favorites.createFolder(QStringLiteral("A"), &error);
        const QString oldPath = mediaPath("clip.mp4");
        const QString newPath = QStringLiteral("D:/sorted/2026/clip.mp4");
        QVERIFY(favorites.addMember(folder, oldPath, &error));
        loops.addClip(makeClip(oldPath, 1000, {}), &error);

        QCOMPARE(favorites.relinkMediaPath(oldPath, newPath), 1);
        QCOMPARE(loops.relinkMediaPath(oldPath, newPath), 1);

        QVERIFY(favorites.isMember(folder, newPath));
        QVERIFY(!favorites.isMember(folder, oldPath));
        QCOMPARE(loops.clips().first().mediaPath, newPath);

        // Relinking again from the old path is a no-op, and no duplicate entry
        // appears in the folder.
        QCOMPARE(favorites.relinkMediaPath(oldPath, newPath), 0);
        QCOMPARE(favorites.memberCount(folder), 1);
    }

    void relinkDoesNotDuplicateAnExistingEntry()
    {
        FavoritesStore favorites(QStringLiteral("C:/tmp/favorites.json"));
        QString error;
        const QString folder = favorites.createFolder(QStringLiteral("A"), &error);

        const QString oldPath = mediaPath("clip.mp4");
        const QString newPath = QStringLiteral("D:/sorted/clip.mp4");
        QVERIFY(favorites.addMember(folder, oldPath, &error));
        QVERIFY(favorites.addMember(folder, newPath, &error));

        QCOMPARE(favorites.relinkMediaPath(oldPath, newPath), 1);
        QCOMPARE(favorites.memberCount(folder), 1);
        QVERIFY(favorites.isMember(folder, newPath));
    }

    void backupKeepsANumberedCopyAndPrunesOldOnes()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("favorites.json"));
        QFile handle(file);
        QVERIFY(handle.open(QIODevice::WriteOnly));
        handle.write("{}");
        handle.close();

        FavoritesStore favorites(file);
        LoopStore loops(dir.filePath(QStringLiteral("loops.json")));
        MediaCleanup cleanup(&favorites, &loops);

        const QStringList created = cleanup.backup();
        QCOMPARE(created.size(), 1);
        QVERIFY(QFileInfo::exists(created.first()));
        QVERIFY(created.first().endsWith(QStringLiteral(".bak")));

        // Only the first backup exists, the loops file was never written.
        QCOMPARE(cleanup.backup().size(), 1);
        for (const QString& path : created)
            QVERIFY(QFileInfo::exists(path));
    }
};

QTEST_MAIN(MediaCleanupTest)
#include "test_media_cleanup.moc"
