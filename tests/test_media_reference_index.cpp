#include <QtTest>

#include "core/MediaReferenceIndex.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace
{
// A drive letter that is not mounted, so the volume root itself is missing.
// Which letters exist differs per machine, so probe instead of assuming.
QString unmountedVolumeRoot()
{
    const QStringList candidates = {QStringLiteral("Q:/"), QStringLiteral("W:/"),
                                    QStringLiteral("X:/"), QStringLiteral("Y:/"),
                                    QStringLiteral("Z:/"), QStringLiteral("A:/"),
                                    QStringLiteral("B:/")};
    for (const QString& root : candidates)
    {
        if (!QDir(root).exists())
            return root;
    }
    return QString();
}

bool writeFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write("x");
    return true;
}
}

class MediaReferenceIndexTest : public QObject
{
    Q_OBJECT

private slots:
    void existingFileIsAvailable()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("clip.mp4"));
        QVERIFY(writeFile(file));

        QCOMPARE(MediaReferenceIndex::probe(file), MediaAvailability::Available);
    }

    void deletedFileNextToReachableFolderIsMissing()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("gone.mp4"));

        // The folder exists, the file does not: the user really removed it.
        QCOMPARE(MediaReferenceIndex::probe(file), MediaAvailability::Missing);
    }

    void deletedFolderUnderMountedVolumeIsMissing()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file =
            QDir(dir.path()).absoluteFilePath(QStringLiteral("sub/deeper/gone.mp4"));

        // Neither "sub" nor "deeper" exists, but the volume does, so the file
        // is genuinely gone rather than sitting on an offline drive.
        QCOMPARE(MediaReferenceIndex::probe(file), MediaAvailability::Missing);
    }

    void unmountedVolumeIsUnreachable()
    {
        const QString root = unmountedVolumeRoot();
        if (root.isEmpty())
            QSKIP("Every candidate drive letter is mounted on this machine");

        QCOMPARE(MediaReferenceIndex::probe(root + QStringLiteral("media/clip.mp4")),
                 MediaAvailability::Unreachable);
    }

    void emptyPathIsUnreachable()
    {
        QCOMPARE(MediaReferenceIndex::probe(QString()), MediaAvailability::Unreachable);
        QCOMPARE(MediaReferenceIndex::probe(QStringLiteral("   ")),
                 MediaAvailability::Unreachable);
    }

    void resolveSplitsReferencesIntoThreeGroups()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString existing = dir.filePath(QStringLiteral("keep.mp4"));
        const QString deleted = dir.filePath(QStringLiteral("gone.mp4"));
        QVERIFY(writeFile(existing));

        const QString offlineRoot = unmountedVolumeRoot();

        MediaReferenceIndex index;
        index.addReference(existing);
        index.addReference(deleted);
        if (!offlineRoot.isEmpty())
            index.addReference(offlineRoot + QStringLiteral("media/offline.mp4"));
        index.resolve();

        const QString existingKey = MediaReferenceIndex::normalize(existing);
        const QString deletedKey = MediaReferenceIndex::normalize(deleted);

        QCOMPARE(index.availability(existing), MediaAvailability::Available);
        QCOMPARE(index.availability(deleted), MediaAvailability::Missing);
        QVERIFY(index.availablePaths().contains(existingKey));
        QVERIFY(!index.availablePaths().contains(deletedKey));
        QVERIFY(index.missingPaths().contains(deletedKey));
        QCOMPARE(index.missingCount(), 1);

        if (!offlineRoot.isEmpty())
        {
            const QString offlineKey =
                MediaReferenceIndex::normalize(offlineRoot
                                               + QStringLiteral("media/offline.mp4"));
            QCOMPARE(index.availability(offlineKey), MediaAvailability::Unreachable);
            // An offline drive must never be reported as stale, and its paths
            // stay in the available set so the stores keep their records.
            QVERIFY(!index.missingPaths().contains(offlineKey));
            QVERIFY(index.availablePaths().contains(offlineKey));
            QVERIFY(index.hasUnreachable());
            QVERIFY(!index.unreachableRoots().isEmpty());
        }
    }

    void knownAvailableSkipsProbing()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // Never written to disk, yet reported as scanned by the library.
        const QString path = dir.filePath(QStringLiteral("scanned.mp4"));

        MediaReferenceIndex index;
        index.addKnownAvailable(path);
        index.resolve();

        QCOMPARE(index.availability(path), MediaAvailability::Available);
        QCOMPARE(index.missingCount(), 0);
        QVERIFY(!index.hasUnreachable());
    }

    void unreachableReferencesKeepStoresFromPruning()
    {
        const QString root = unmountedVolumeRoot();
        if (root.isEmpty())
            QSKIP("Every candidate drive letter is mounted on this machine");

        MediaReferenceIndex index;
        index.addReference(root + QStringLiteral("favourites/clip.mp4"));
        index.resolve();

        // The guard the dialogs rely on: nothing may be cleaned up.
        QVERIFY(index.hasUnreachable());
        QCOMPARE(index.missingCount(), 0);
    }
};

QTEST_MAIN(MediaReferenceIndexTest)
#include "test_media_reference_index.moc"
