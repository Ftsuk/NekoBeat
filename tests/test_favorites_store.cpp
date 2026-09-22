#include "TestLanguage.h"
#include "core/FavoritesStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

namespace
{
QString mediaPath(const QString& name)
{
    return QStringLiteral("C:/media/") + name;
}
}

class FavoritesStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void createRenameRemoveAndReorder()
    {
        FavoritesStore store(QStringLiteral("C:/tmp/favorites.json"));
        QVERIFY(store.folders().isEmpty());

        QString error;
        const QString first = store.createFolder(QStringLiteral("动作"), &error);
        QVERIFY2(!first.isEmpty(), qPrintable(error));
        const QString second = store.createFolder(QStringLiteral("剧情"), &error);
        QVERIFY2(!second.isEmpty(), qPrintable(error));
        QCOMPARE(store.count(), 2);
        QCOMPARE(store.folderName(first), QStringLiteral("动作"));

        QVERIFY(store.renameFolder(first, QStringLiteral("  精选  "), &error));
        QCOMPARE(store.folderName(first), QStringLiteral("精选"));

        QVERIFY(store.moveFolder(second, -1, &error));
        QCOMPARE(store.folders().first().id, second);
        QCOMPARE(store.folders().last().id, first);

        QVERIFY(store.removeFolder(first, &error));
        QCOMPARE(store.count(), 1);
        QVERIFY(!store.removeFolder(first, &error));
        QCOMPARE(error, QStringLiteral("收藏夹不存在"));
    }

    void nameValidationRejectsBadInput()
    {
        FavoritesStore store(QStringLiteral("C:/tmp/favorites.json"));
        QString error;

        QVERIFY(store.createFolder(QStringLiteral("   "), &error).isEmpty());
        QCOMPARE(error, QStringLiteral("收藏夹名称不能为空"));

        QVERIFY(store.createFolder(QString(FavoritesStore::maxNameLength() + 1,
                                           QLatin1Char('a')),
                                   &error)
                    .isEmpty());
        QVERIFY(error.contains(QStringLiteral("不能超过")));

        QVERIFY(!store.createFolder(QStringLiteral("Rescue"), &error).isEmpty());
        QVERIFY(store.createFolder(QStringLiteral("rescue"), &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("同名")));

        // Renaming a folder to its own name is allowed.
        const QString id = store.folders().first().id;
        QVERIFY(store.renameFolder(id, QStringLiteral("RESCUE"), &error));
    }

    void membershipSupportsMultipleFolders()
    {
        FavoritesStore store(QStringLiteral("C:/tmp/favorites.json"));
        QString error;
        const QString a = store.createFolder(QStringLiteral("A"), &error);
        const QString b = store.createFolder(QStringLiteral("B"), &error);

        QVERIFY(store.addMember(a, mediaPath("clip.mp4"), &error));
        QVERIFY(store.addMember(b, mediaPath("clip.mp4"), &error));
        // Adding twice must not duplicate the entry.
        QVERIFY(store.addMember(a, mediaPath("clip.mp4"), &error));
        QCOMPARE(store.memberCount(a), 1);
        QCOMPARE(store.memberCount(b), 1);
        QVERIFY(store.isFavorite(mediaPath("clip.mp4")));
        QCOMPARE(store.folderIdsFor(mediaPath("clip.mp4")).size(), 2);
        QCOMPARE(store.folderNamesFor(mediaPath("clip.mp4")),
                 QStringList({QStringLiteral("A"), QStringLiteral("B")}));

        QVERIFY(store.removeMember(a, mediaPath("clip.mp4"), &error));
        QCOMPARE(store.memberCount(a), 0);
        QCOMPARE(store.memberCount(b), 1);
        QVERIFY(store.isFavorite(mediaPath("clip.mp4")));

        QVERIFY(store.setMember(b, mediaPath("clip.mp4"), false, &error));
        QVERIFY(!store.isFavorite(mediaPath("clip.mp4")));
    }

    void membershipIgnoresPathCaseAndSeparators()
    {
        FavoritesStore store(QStringLiteral("C:/tmp/favorites.json"));
        QString error;
        const QString id = store.createFolder(QStringLiteral("A"), &error);

        QVERIFY(store.addMember(id, QStringLiteral("C:/Media/Clip.mp4"), &error));
        QVERIFY(store.isMember(id, QStringLiteral("c:/media/clip.MP4")));
        QVERIFY(store.isMember(id, QStringLiteral("C:\\Media\\Clip.mp4")));
        QVERIFY(store.isFavorite(QStringLiteral("c:\\MEDIA\\clip.mp4")));
        QVERIFY(!store.isFavorite(QStringLiteral("C:/media/other.mp4")));

        QVERIFY(store.addMember(id, QStringLiteral("c:/MEDIA/clip.mp4"), &error));
        QCOMPARE(store.memberCount(id), 1);

        const QSet<QString> memberPaths = store.memberPaths(id);
        QCOMPARE(memberPaths.size(), 1);
        QVERIFY(memberPaths.contains(FavoritesStore::normalizePath(mediaPath("clip.mp4"))));
        QCOMPARE(store.allMemberPaths(), memberPaths);
    }

    void jsonRoundTripKeepsEverything()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("favorites.json"));

        FavoritesStore store(path);
        QString error;
        const QString first = store.createFolder(QStringLiteral("动作"), &error);
        const QString second = store.createFolder(QStringLiteral("剧情"), &error);
        QVERIFY(store.addMember(second, mediaPath("a.mp4"), &error));
        QVERIFY(store.addMember(second, mediaPath("b.mp4"), &error));
        QVERIFY(store.addMember(first, mediaPath("b.mp4"), &error));
        QVERIFY(store.save());
        QVERIFY(QFileInfo::exists(path));

        FavoritesStore loaded(path);
        QVERIFY(loaded.load());
        QCOMPARE(loaded.count(), 2);
        QCOMPARE(loaded.folderName(first), QStringLiteral("动作"));
        QCOMPARE(loaded.folderName(second), QStringLiteral("剧情"));
        QCOMPARE(loaded.memberCount(second), 2);
        QCOMPARE(loaded.folderIdsFor(mediaPath("b.mp4")).size(), 2);
        QVERIFY(!loaded.folders().first().created.isNull());
        QVERIFY(!loaded.folder(second).members.first().added.isNull());
    }

    void saveCreatesMissingDirectory()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path =
            QDir(dir.path()).absoluteFilePath(QStringLiteral("nested/deeper/favorites.json"));

        FavoritesStore store(path);
        QVERIFY(store.createFolder(QStringLiteral("A")).size() > 0);
        QVERIFY(store.save());
        QVERIFY(QFileInfo::exists(path));

        FavoritesStore loaded(path);
        QVERIFY(loaded.load());
        QCOMPARE(loaded.count(), 1);
    }

    void corruptFileIsBackedUp()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("favorites.json"));

        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("{ this is not json");
        file.close();

        FavoritesStore store(path);
        QVERIFY(!store.load());
        QVERIFY(store.folders().isEmpty());
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(QFileInfo::exists(path + QStringLiteral(".bad")));

        // The store stays usable after the reset.
        QVERIFY(!store.createFolder(QStringLiteral("A")).isEmpty());
        QVERIFY(store.save());
        QVERIFY(QFileInfo::exists(path));
    }

    void unsupportedVersionIsRejected()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("favorites.json"));

        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("{\"version\": 99, \"folders\": []}");
        file.close();

        FavoritesStore store(path);
        QVERIFY(!store.load());
        QVERIFY(store.folders().isEmpty());
    }

    void pruneMissingRemovesOnlyStaleEntries()
    {
        FavoritesStore store(QStringLiteral("C:/tmp/favorites.json"));
        QString error;
        const QString a = store.createFolder(QStringLiteral("A"), &error);
        const QString b = store.createFolder(QStringLiteral("B"), &error);
        QVERIFY(store.addMember(a, mediaPath("keep.mp4"), &error));
        QVERIFY(store.addMember(a, mediaPath("gone.mp4"), &error));
        QVERIFY(store.addMember(b, mediaPath("gone.mp4"), &error));
        QVERIFY(store.addMember(b, mediaPath("keep.mp4"), &error));

        const QSet<QString> available = {
            FavoritesStore::normalizePath(mediaPath("keep.mp4"))
        };
        QCOMPARE(store.missingCount(a, available), 1);
        QCOMPARE(store.missingCount(available), 2);

        QCOMPARE(store.pruneMissing(available), 2);
        QCOMPARE(store.memberCount(a), 1);
        QCOMPARE(store.memberCount(b), 1);
        QCOMPARE(store.missingCount(available), 0);
        QVERIFY(store.isMember(a, mediaPath("keep.mp4")));
        QVERIFY(store.isMember(b, mediaPath("keep.mp4")));
    }
};

QTEST_MAIN(FavoritesStoreTest)
#include "test_favorites_store.moc"
