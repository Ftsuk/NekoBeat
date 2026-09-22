#include "TestLanguage.h"
#include "core/LoopStore.h"

#include "core/PathUtils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

namespace
{
LoopClip makeClip(const QString& path, qint64 startMs, qint64 endMs)
{
    LoopClip clip;
    clip.mediaPath = path;
    clip.startMs = startMs;
    clip.endMs = endMs;
    return clip;
}
}

class LoopStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void folderCrudAndReorder()
    {
        LoopStore store(QStringLiteral("C:/tmp/loops.json"));
        QVERIFY(store.folders().isEmpty());

        QString error;
        const QString first = store.createFolder(QStringLiteral("动作"), &error);
        QVERIFY2(!first.isEmpty(), qPrintable(error));
        const QString second = store.createFolder(QStringLiteral("剧情"), &error);
        QVERIFY(!second.isEmpty());
        QCOMPARE(store.folderCount(), 2);

        QVERIFY(store.renameFolder(first, QStringLiteral("  精选  "), &error));
        QCOMPARE(store.folderName(first), QStringLiteral("精选"));

        QVERIFY(store.moveFolder(second, -1, &error));
        QCOMPARE(store.folders().first().id, second);
        QVERIFY(!store.moveFolder(second, -1, &error));
        QVERIFY(error.contains(QStringLiteral("最前")));

        QVERIFY(store.removeFolder(first, &error));
        QCOMPARE(store.folderCount(), 1);
        QVERIFY(!store.removeFolder(first, &error));
        QCOMPARE(error, QStringLiteral("收藏夹不存在"));
    }

    void nameValidationRejectsBadInput()
    {
        LoopStore store(QStringLiteral("C:/tmp/loops.json"));
        QString error;

        QVERIFY(store.createFolder(QStringLiteral("   "), &error).isEmpty());
        QCOMPARE(error, QStringLiteral("收藏夹名称不能为空"));

        QVERIFY(store.createFolder(QString(LoopStore::maxNameLength() + 1, QLatin1Char('a')),
                                   &error)
                    .isEmpty());
        QVERIFY(error.contains(QStringLiteral("不能超过")));

        QVERIFY(!store.createFolder(QStringLiteral("Rescue"), &error).isEmpty());
        QVERIFY(store.createFolder(QStringLiteral("rescue"), &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("同名")));
    }

    void clipsSupportMultipleFoldersAndValidation()
    {
        LoopStore store(QStringLiteral("C:/tmp/loops.json"));
        QString error;
        const QString action = store.createFolder(QStringLiteral("动作"), &error);
        const QString favourite = store.createFolder(QStringLiteral("最爱"), &error);

        LoopClip clip = makeClip(QStringLiteral("C:/media/a.mp4"), 1000, 5000);
        clip.title = QStringLiteral("第一段");
        clip.tags = QStringList({QStringLiteral("慢"), QStringLiteral("热身")});
        clip.folderIds = QStringList({action, favourite, QStringLiteral("missing-folder")});
        const QString id = store.addClip(clip, &error);
        QVERIFY2(!id.isEmpty(), qPrintable(error));
        QCOMPARE(store.clipCount(), 1);

        const LoopClip stored = store.clip(id);
        QCOMPARE(stored.title, QStringLiteral("第一段"));
        QCOMPARE(stored.folderIds.size(), 2);
        QCOMPARE(stored.thumbKey, LoopStore::thumbnailKeyFor(stored.mediaPath, 1000));
        QCOMPARE(store.folderClipCount(action), 1);
        QCOMPARE(store.folderClipCount(favourite), 1);
        QVERIFY(stored.created.isValid());

        // Too short / reversed ranges are rejected.
        QVERIFY(store.addClip(makeClip(QStringLiteral("C:/media/a.mp4"), 0, 100), &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("太短")));
        QVERIFY(store.addClip(makeClip(QStringLiteral("C:/media/a.mp4"), 5000, 4000), &error)
                    .isEmpty());

        // A media file can hold many clips.
        LoopClip second = makeClip(QStringLiteral("C:/Media/A.MP4"), 9000, 12000);
        const QString secondId = store.addClip(second, &error);
        QVERIFY2(!secondId.isEmpty(), qPrintable(error));
        QCOMPARE(store.clipsForMedia(QStringLiteral("c:/media/a.mp4")).size(), 2);
        QCOMPARE(store.mediaPaths().size(), 1);

        // Editing keeps the creation timestamp but refreshes the title.
        LoopClip edited = store.clip(secondId);
        edited.title = QStringLiteral("第二段");
        edited.startMs = 8000;
        edited.tags = QStringList({QStringLiteral("快")});
        QVERIFY(store.updateClip(edited, &error));
        QCOMPARE(store.clip(secondId).title, QStringLiteral("第二段"));
        QCOMPARE(store.clip(secondId).thumbKey,
                 LoopStore::thumbnailKeyFor(edited.mediaPath, 8000));

        QVERIFY(!store.updateClip(makeClip(QString(), 0, 1000), &error));
        QVERIFY(store.removeClip(secondId, &error));
        QCOMPARE(store.clipCount(), 1);
        QVERIFY(!store.removeClip(secondId, &error));

        // Removing a folder also drops it from every clip.
        QVERIFY(store.removeFolder(favourite, &error));
        QCOMPARE(store.clip(id).folderIds, QStringList({action}));
    }

    void tagsRenameMergeAndPrune()
    {
        LoopStore store(QStringLiteral("C:/tmp/loops.json"));
        QString error;

        LoopClip first = makeClip(QStringLiteral("C:/media/a.mp4"), 0, 1000);
        first.tags = QStringList({QStringLiteral("慢"), QStringLiteral("热身")});
        const QString firstId = store.addClip(first, &error);
        LoopClip second = makeClip(QStringLiteral("C:/media/a.mp4"), 2000, 3000);
        second.tags = QStringList({QStringLiteral("慢"), QStringLiteral("快")});
        const QString secondId = store.addClip(second, &error);
        QVERIFY(!firstId.isEmpty());
        QVERIFY(!secondId.isEmpty());

        QCOMPARE(store.tagUsage(QStringLiteral("慢")), 2);
        QCOMPARE(store.tagUsage(QStringLiteral("快")), 1);
        QCOMPARE(store.tagsByUsage().first(), QStringLiteral("慢"));

        // Merging two tags keeps one entry and rewrites the clips.
        QVERIFY(store.renameTag(QStringLiteral("快"), QStringLiteral("慢"), &error));
        QCOMPARE(store.tagUsage(QStringLiteral("快")), 0);
        QCOMPARE(store.tagUsage(QStringLiteral("慢")), 2);
        QCOMPARE(store.clip(secondId).tags.size(), 1);
        QCOMPARE(store.clip(secondId).tags.first(), QStringLiteral("慢"));
        QVERIFY(!store.tags().contains(QStringLiteral("快")));

        // Renaming to a brand new tag keeps the spelling.
        QVERIFY(store.renameTag(QStringLiteral("热身"), QStringLiteral(" 暖场 "), &error));
        QCOMPARE(store.clip(firstId).tags, QStringList({QStringLiteral("慢"),
                                                        QStringLiteral("暖场")}));

        QVERIFY(store.removeTag(QStringLiteral("暖场"), &error));
        QVERIFY(!store.clip(firstId).tags.contains(QStringLiteral("暖场")));
        QVERIFY(!store.removeTag(QStringLiteral("暖场"), &error));

        // Every remaining tag is still used by a clip, so nothing is pruned.
        QCOMPARE(store.pruneUnusedTags(), 0);
    }

    void jsonRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("loops.json"));

        {
            LoopStore store(file);
            QString error;
            const QString folderId = store.createFolder(QStringLiteral("精选"), &error);
            LoopClip clip = makeClip(QStringLiteral("C:/media/a.mp4"), 1500, 9500);
            clip.title = QStringLiteral("片段甲");
            clip.note = QStringLiteral("备注内容");
            clip.tags = QStringList({QStringLiteral("慢"), QStringLiteral("慢")});
            clip.folderIds = QStringList({folderId});
            QVERIFY(!store.addClip(clip, &error).isEmpty());
            QVERIFY(store.save());
        }

        {
            LoopStore store(file);
            QVERIFY(store.load());
            QCOMPARE(store.folderCount(), 1);
            QCOMPARE(store.clipCount(), 1);
            const LoopClip clip = store.clips().first();
            QCOMPARE(clip.title, QStringLiteral("片段甲"));
            QCOMPARE(clip.note, QStringLiteral("备注内容"));
            QCOMPARE(clip.startMs, 1500LL);
            QCOMPARE(clip.endMs, 9500LL);
            QCOMPARE(clip.tags.size(), 1);
            QCOMPARE(clip.folderIds.size(), 1);
            QVERIFY(clip.created.isValid());
            QVERIFY(store.tags().contains(QStringLiteral("慢")));
        }
    }

    void corruptedFileIsBackedUp()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("loops.json"));

        QFile broken(file);
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("{ not json");
        broken.close();

        LoopStore store(file);
        QVERIFY(!store.load());
        QVERIFY(store.clips().isEmpty());
        QVERIFY(QFileInfo::exists(file + QStringLiteral(".bad")));

        // A version we do not understand is treated the same way.
        const QString other = dir.filePath(QStringLiteral("future.json"));
        QFile future(other);
        QVERIFY(future.open(QIODevice::WriteOnly));
        future.write("{\"version\": 99, \"clips\": []}");
        future.close();

        LoopStore futureStore(other);
        QVERIFY(!futureStore.load());
        QVERIFY(QFileInfo::exists(other + QStringLiteral(".bad")));

        // A missing file is simply empty.
        LoopStore missing(dir.filePath(QStringLiteral("absent.json")));
        QVERIFY(missing.load());
        QVERIFY(missing.clips().isEmpty());
    }

    void loadSkipsUnusableClips()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString file = dir.filePath(QStringLiteral("loops.json"));
        QFile handle(file);
        QVERIFY(handle.open(QIODevice::WriteOnly));
        handle.write(R"({
            "version": 1,
            "folders": [],
            "tags": ["孤儿标签"],
            "clips": [
                {"id": "ok", "mediaPath": "C:/media/a.mp4", "startMs": 1000, "endMs": 4000},
                {"id": "no-media", "mediaPath": "", "startMs": 0, "endMs": 100},
                {"id": "reversed", "mediaPath": "C:/media/b.mp4", "startMs": 5000, "endMs": 1000}
            ]
        })");
        handle.close();

        LoopStore store(file);
        QVERIFY(store.load());
        QCOMPARE(store.clipCount(), 1);
        QCOMPARE(store.clips().first().id, QStringLiteral("ok"));
        // The title falls back to the generated one.
        QCOMPARE(store.clips().first().title,
                 LoopClipRules::defaultTitle(QStringLiteral("C:/media/a.mp4"), 1000, 4000));
        QCOMPARE(store.pruneUnusedTags(), 1);
    }

    void missingClipsAreCountedAndPruned()
    {
        LoopStore store(QStringLiteral("C:/tmp/loops.json"));
        QString error;
        const QString first = store.addClip(makeClip(QStringLiteral("C:/media/a.mp4"), 0, 1000),
                                            &error);
        const QString second = store.addClip(makeClip(QStringLiteral("C:/media/b.mp4"), 0, 1000),
                                             &error);
        QVERIFY(!first.isEmpty());
        QVERIFY(!second.isEmpty());

        QSet<QString> available;
        available.insert(PathUtils::normalizeMediaPath(QStringLiteral("C:/media/a.mp4")));
        QCOMPARE(store.missingClipCount(available), 1);
        QCOMPARE(store.pruneMissingClips(available), 1);
        QCOMPARE(store.clipCount(), 1);
        QCOMPARE(store.clips().first().id, first);
    }

    void thumbnailKeyIsStable()
    {
        const QString key = LoopStore::thumbnailKeyFor(QStringLiteral("C:/media/a.mp4"), 1234);
        QCOMPARE(key, LoopStore::thumbnailKeyFor(QStringLiteral("c:/media/A.mp4"), 1234));
        QVERIFY(key.endsWith(QStringLiteral("@1234")));
        // Folder-free: moving the video must not throw the grabbed frame away.
        QCOMPARE(key, LoopStore::thumbnailKeyFor(
                          QStringLiteral("D:/sorted/2026/a.mp4"), 1234));
        QVERIFY(key != LoopStore::thumbnailKeyFor(QStringLiteral("C:/media/b.mp4"), 1234));
        QVERIFY(key != LoopStore::thumbnailKeyFor(QStringLiteral("C:/media/a.mp4"), 4321));
        QVERIFY(LoopStore::thumbnailKeyFor(QString(), 0).isEmpty());
    }
};

QTEST_MAIN(LoopStoreTest)
#include "test_loop_store.moc"
