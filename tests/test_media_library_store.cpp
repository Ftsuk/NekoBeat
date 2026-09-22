#include "TestLanguage.h"
#include "core/MediaLibraryStore.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

namespace
{
MediaItem makeItem(const QString& name, qint64 durationMs, int axes,
                   const QDateTime& modified)
{
    MediaItem item;
    item.path = QStringLiteral("C:/media/") + name;
    item.displayName = name;
    item.fileSize = 1024 * 1024;
    item.modified = modified;
    item.durationMs = durationMs;
    item.width = 1920;
    item.height = 1080;
    item.container = QStringLiteral("mov,mp4");
    item.videoCodec = QStringLiteral("h264");
    item.audioCodec = QStringLiteral("aac");
    item.scriptAxes = axes;
    item.metadataReady = true;
    item.thumbnailState = ThumbnailState::Ready;
    return item;
}
}

class MediaLibraryStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void cacheKeyTracksFileIdentity()
    {
        MediaLibraryStore store(QStringLiteral("C:/cache"));
        const QDateTime first = QDateTime::fromString(QStringLiteral("2026-09-19T10:00:00.000"),
                                                      Qt::ISODateWithMs);
        const QDateTime second = first.addSecs(60);

        const QString key = store.cacheKeyFor(QStringLiteral("C:/media/a.mp4"), 100, first);
        QCOMPARE(key.size(), 40);
        QCOMPARE(key, store.cacheKeyFor(QStringLiteral("c:/media/A.mp4"), 100, first));
        QVERIFY(key != store.cacheKeyFor(QStringLiteral("C:/media/a.mp4"), 101, first));
        QVERIFY(key != store.cacheKeyFor(QStringLiteral("C:/media/a.mp4"), 100, second));
        QVERIFY(key != store.cacheKeyFor(QStringLiteral("C:/media/b.mp4"), 100, first));
    }

    void cacheKeySurvivesReorganisingFolders()
    {
        MediaLibraryStore store(QStringLiteral("C:/cache"));
        const QDateTime modified = QDateTime::fromString(
            QStringLiteral("2026-09-19T10:00:00.000"), Qt::ISODateWithMs);

        // Moving a video into a new folder must reuse its thumbnail. Same file
        // name, size and timestamp therefore have to hash to the same key.
        QCOMPARE(store.cacheKeyFor(QStringLiteral("C:/inbox/clip.mp4"), 100, modified),
                 store.cacheKeyFor(QStringLiteral("D:/sorted/2026/clip.mp4"), 100,
                                   modified));

        // A different video that merely shares the name still stays separate.
        QVERIFY(store.cacheKeyFor(QStringLiteral("C:/inbox/clip.mp4"), 100, modified)
                != store.cacheKeyFor(QStringLiteral("D:/sorted/clip.mp4"), 101, modified));

        // The pre-reorganisation key did depend on the folder, which is why the
        // upgrade path has to relocate those thumbnails.
        QVERIFY(MediaLibraryStore::legacyCacheKeyFor(QStringLiteral("C:/inbox/clip.mp4"),
                                                     100, modified)
                != MediaLibraryStore::legacyCacheKeyFor(
                    QStringLiteral("D:/sorted/clip.mp4"), 100, modified));
    }

    void legacyThumbnailIsRelocatedOnUpgrade()
    {
        QTemporaryDir mediaDir;
        QTemporaryDir cacheDir;
        QVERIFY(mediaDir.isValid());
        QVERIFY(cacheDir.isValid());

        const QString video = mediaDir.filePath(QStringLiteral("clip.mp4"));
        QFile file(video);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("video");
        file.close();

        const QFileInfo info(video);
        MediaLibraryStore store(cacheDir.path());
        store.ensureDirectories();

        MediaItem item;
        item.path = info.absoluteFilePath();
        item.displayName = info.fileName();
        item.fileSize = info.size();
        item.modified = info.lastModified();
        // Written by an older build, when the folder was still part of the key.
        item.cacheKey = MediaLibraryStore::legacyCacheKeyFor(item.path, item.fileSize,
                                                             item.modified);
        item.thumbnailPath = store.thumbnailPathFor(item.cacheKey);
        item.durationMs = 42000;
        item.metadataReady = true;
        item.thumbnailState = ThumbnailState::Ready;
        store.saveIndex({item});

        QVERIFY(QFile(item.thumbnailPath).open(QIODevice::WriteOnly));
        QFile(item.thumbnailPath).close();

        const QString upgradedKey =
            store.cacheKeyFor(item.path, item.fileSize, item.modified);
        QVERIFY(upgradedKey != item.cacheKey);

        const QList<MediaItem> scanned = store.scanRoots({mediaDir.path()});
        QCOMPARE(scanned.size(), 1);
        QCOMPARE(scanned.first().cacheKey, upgradedKey);
        QCOMPARE(scanned.first().thumbnailState, ThumbnailState::Ready);
        QVERIFY(QFileInfo::exists(store.thumbnailPathFor(upgradedKey)));
    }

    void movedVideoKeepsThumbnailAndProbedMetadata()
    {
        QTemporaryDir mediaDir;
        QTemporaryDir cacheDir;
        QVERIFY(mediaDir.isValid());
        QVERIFY(cacheDir.isValid());

        const QString inbox = mediaDir.filePath(QStringLiteral("inbox"));
        const QString sorted = mediaDir.filePath(QStringLiteral("sorted/2026"));
        QVERIFY(QDir().mkpath(inbox));
        QVERIFY(QDir().mkpath(sorted));

        const QString video = QDir(inbox).absoluteFilePath(QStringLiteral("clip.mp4"));
        QFile file(video);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("video");
        file.close();

        const QFileInfo info(video);
        MediaLibraryStore store(cacheDir.path());
        store.ensureDirectories();

        MediaItem item;
        item.path = info.absoluteFilePath();
        item.displayName = info.fileName();
        item.fileSize = info.size();
        item.modified = info.lastModified();
        item.cacheKey = store.cacheKeyFor(item.path, item.fileSize, item.modified);
        item.thumbnailPath = store.thumbnailPathFor(item.cacheKey);
        item.durationMs = 90000;
        item.width = 3840;
        item.height = 2160;
        item.videoCodec = QStringLiteral("h265");
        item.metadataReady = true;
        item.thumbnailState = ThumbnailState::Ready;
        // The user picked a script sitting next to the video.
        const QString script = QDir(inbox).absoluteFilePath(QStringLiteral("clip.roll.funscript"));
        QFile scriptFile(script);
        QVERIFY(scriptFile.open(QIODevice::WriteOnly));
        scriptFile.close();
        item.preferredScript = script;
        store.saveIndex({item});

        QVERIFY(QFile(item.thumbnailPath).open(QIODevice::WriteOnly));
        QFile(item.thumbnailPath).close();

        // Reorganise: the whole folder moves, the file itself is untouched.
        const QString movedVideo = QDir(sorted).absoluteFilePath(QStringLiteral("clip.mp4"));
        QVERIFY(QFile::rename(video, movedVideo));
        QVERIFY(QFile::rename(script, QDir(sorted).absoluteFilePath(
                                          QStringLiteral("clip.roll.funscript"))));

        const QList<MediaItem> scanned = store.scanRoots({mediaDir.path()});
        QCOMPARE(scanned.size(), 1);
        const MediaItem& result = scanned.first();
        QCOMPARE(result.path, QDir::cleanPath(QFileInfo(movedVideo).absoluteFilePath()));
        // Nothing was probed again and the cover was not regenerated.
        QCOMPARE(result.durationMs, 90000);
        QCOMPARE(result.width, 3840);
        QVERIFY(result.metadataReady);
        QCOMPARE(result.thumbnailState, ThumbnailState::Ready);
        QVERIFY(QFileInfo::exists(result.thumbnailPath));
        // The script choice follows the video into its new folder.
        QCOMPARE(result.preferredScript,
                 QDir::cleanPath(QFileInfo(QDir(sorted).absoluteFilePath(
                     QStringLiteral("clip.roll.funscript"))).absoluteFilePath()));
    }

    void reimportedFolderReusesCachedCovers()
    {
        // 用户场景：媒体库文件夹被移除后又重新导入。索引里已经没有这些条目了
        // （移除文件夹、清理都是一次 rescan，索引也会跟着重写），但缩略图文件
        // 还在缓存目录里。缓存 key 只由 文件名+大小+修改时间 决定，所以只要这
        // 三者没变，封面就必须直接复用，不能再渲染一遍。
        QTemporaryDir mediaDir;
        QTemporaryDir cacheDir;
        QVERIFY(mediaDir.isValid());
        QVERIFY(cacheDir.isValid());

        const QString video = QDir(mediaDir.path()).absoluteFilePath(QStringLiteral("clip.mp4"));
        QFile file(video);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("video");
        file.close();

        const QFileInfo info(video);
        MediaLibraryStore store(cacheDir.path());
        store.ensureDirectories();

        const QString key = store.cacheKeyFor(info.absoluteFilePath(), info.size(),
                                             info.lastModified());
        const QString cover = store.thumbnailPathFor(key);
        QFile coverFile(cover);
        QVERIFY(coverFile.open(QIODevice::WriteOnly));
        coverFile.write("jpeg");
        coverFile.close();

        // 索引是空的：这正是"移除过文件夹后再导入"时的状态。
        QVERIFY(store.loadIndex().isEmpty());

        QList<MediaItem> scanned = store.scanRoots({mediaDir.path()});
        QCOMPARE(scanned.size(), 1);
        QCOMPARE(scanned.first().cacheKey, key);
        QCOMPARE(scanned.first().thumbnailState, ThumbnailState::Ready);
        // 元数据（时长/分辨率/编码）不在封面文件里，还是得补读一次。
        QVERIFY(!scanned.first().metadataReady);

        // 视频真的变了（大小/修改时间不同）时，旧封面不能冒充新的。
        QFile changed(video);
        QVERIFY(changed.open(QIODevice::Append));
        changed.write("more");
        changed.close();

        scanned = store.scanRoots({mediaDir.path()});
        QCOMPARE(scanned.size(), 1);
        QVERIFY(scanned.first().cacheKey != key);
        QCOMPARE(scanned.first().thumbnailState, ThumbnailState::Pending);
    }

    void indexRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MediaLibraryStore store(dir.path());

        const QDateTime modified = QDateTime::fromString(
            QStringLiteral("2026-09-19T10:00:00.000"), Qt::ISODateWithMs);
        const QList<MediaItem> items = {
            makeItem(QStringLiteral("a.mp4"), 65000, 6, modified),
            makeItem(QStringLiteral("b.mp4"), 12000, 0, modified.addDays(-1))
        };
        store.saveIndex(items);

        const QHash<QString, MediaItem> loaded = store.loadIndex();
        QCOMPARE(loaded.size(), 2);

        bool found = false;
        for (const MediaItem& item : loaded)
        {
            if (item.displayName != QStringLiteral("a.mp4"))
                continue;
            found = true;
            QCOMPARE(item.durationMs, 65000);
            QCOMPARE(item.width, 1920);
            QCOMPARE(item.height, 1080);
            QCOMPARE(item.container, QStringLiteral("mov,mp4"));
            QCOMPARE(item.videoCodec, QStringLiteral("h264"));
            QCOMPARE(item.audioCodec, QStringLiteral("aac"));
            QCOMPARE(item.scriptAxes, 6);
            QVERIFY(item.metadataReady);
            QCOMPARE(item.thumbnailState, ThumbnailState::Ready);
        }
        QVERIFY(found);
    }

    void corruptedIndexIsIgnored()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MediaLibraryStore store(dir.path());
        store.ensureDirectories();

        QFile file(store.indexPath());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("{ this is not json");
        file.close();

        QVERIFY(store.loadIndex().isEmpty());

        // A cache clear removes the index and thumbnail folders without
        // touching anything outside the cache root.
        store.clearCache();
        QVERIFY(!QFileInfo::exists(store.indexPath()));
    }

    void normalizeRootsDeduplicates()
    {
        const QStringList normalized = MediaLibraryStore::normalizeRoots(
            {QStringLiteral("C:/media"), QStringLiteral("C:/MEDIA"),
             QStringLiteral("C:/media/"), QStringLiteral("  ")});
        QCOMPARE(normalized.size(), 1);
        QCOMPARE(normalized.first(), QStringLiteral("C:/media"));
    }

    void scanRootsKeepsPreferredScript()
    {
        QTemporaryDir mediaDir;
        QTemporaryDir cacheDir;
        QVERIFY(mediaDir.isValid());
        QVERIFY(cacheDir.isValid());

        const QString video = mediaDir.filePath(QStringLiteral("clip.mp4"));
        QFile file(video);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("video");
        file.close();

        const QFileInfo info(video);
        MediaLibraryStore store(cacheDir.path());
        store.ensureDirectories();

        MediaItem item;
        item.path = info.absoluteFilePath();
        item.displayName = info.fileName();
        item.fileSize = info.size();
        item.modified = info.lastModified();
        item.cacheKey = store.cacheKeyFor(item.path, item.fileSize, item.modified);
        item.thumbnailPath = store.thumbnailPathFor(item.cacheKey);
        item.durationMs = 42000;
        item.width = 1920;
        item.height = 1080;
        item.metadataReady = true;
        item.thumbnailState = ThumbnailState::Ready;
        item.preferredScript = QStringLiteral("D:/scripts/clip.roll.funscript");
        store.saveIndex({item});

        // A rescan must keep both the probed metadata and the script the user
        // picked; dropping the selection here used to lose it on every refresh.
        const QList<MediaItem> scanned = store.scanRoots({mediaDir.path()});
        QCOMPARE(scanned.size(), 1);
        QCOMPARE(scanned.first().preferredScript, item.preferredScript);
        QCOMPARE(scanned.first().durationMs, 42000);
        QVERIFY(scanned.first().metadataReady);
    }

    void naturalSortOrdersNumbers()
    {
        QVERIFY(MediaLibraryStore::naturalCompare(QStringLiteral("a2.mp4"),
                                                  QStringLiteral("a10.mp4"))
                < 0);
        QVERIFY(MediaLibraryStore::naturalCompare(QStringLiteral("a10.mp4"),
                                                  QStringLiteral("a2.mp4"))
                > 0);
        QCOMPARE(MediaLibraryStore::naturalCompare(QStringLiteral("Same.mp4"),
                                                   QStringLiteral("same.mp4")),
                 0);
    }

    void filterAndSortRespectsScripts()
    {
        const QDateTime modified = QDateTime::fromString(
            QStringLiteral("2026-09-19T10:00:00.000"), Qt::ISODateWithMs);
        const QList<MediaItem> items = {
            makeItem(QStringLiteral("a2.mp4"), 30000, 0, modified),
            makeItem(QStringLiteral("a10.mp4"), 90000, 6, modified.addDays(-2)),
            makeItem(QStringLiteral("alpha.mp4"), 120000, 1, modified.addDays(-1))
        };

        const QList<MediaItem> byName = MediaLibraryStore::filterAndSort(
            items, QString(), false, MediaLibraryStore::SortMode::Name);
        QCOMPARE(byName.size(), 3);
        QCOMPARE(byName.at(0).displayName, QStringLiteral("a2.mp4"));
        QCOMPARE(byName.at(1).displayName, QStringLiteral("a10.mp4"));
        QCOMPARE(byName.at(2).displayName, QStringLiteral("alpha.mp4"));

        const QList<MediaItem> withScripts = MediaLibraryStore::filterAndSort(
            items, QString(), true, MediaLibraryStore::SortMode::Name);
        QCOMPARE(withScripts.size(), 2);

        const QList<MediaItem> searched = MediaLibraryStore::filterAndSort(
            items, QStringLiteral("ALPHA"), false, MediaLibraryStore::SortMode::Name);
        QCOMPARE(searched.size(), 1);

        const QList<MediaItem> byDuration = MediaLibraryStore::filterAndSort(
            items, QString(), false, MediaLibraryStore::SortMode::Duration);
        QCOMPARE(byDuration.first().displayName, QStringLiteral("a2.mp4"));
        QCOMPARE(byDuration.last().displayName, QStringLiteral("alpha.mp4"));

        const QList<MediaItem> byModified = MediaLibraryStore::filterAndSort(
            items, QString(), false, MediaLibraryStore::SortMode::RecentlyModified);
        QCOMPARE(byModified.first().displayName, QStringLiteral("a10.mp4"));
        QCOMPARE(byModified.last().displayName, QStringLiteral("a2.mp4"));
    }

    void sortDirectionIsConfigurable()
    {
        const QDateTime modified = QDateTime::fromString(
            QStringLiteral("2026-09-19T10:00:00.000"), Qt::ISODateWithMs);
        const QList<MediaItem> items = {
            makeItem(QStringLiteral("a2.mp4"), 30000, 0, modified),
            makeItem(QStringLiteral("a10.mp4"), 90000, 6, modified.addDays(-2)),
            makeItem(QStringLiteral("alpha.mp4"), 120000, 1, modified.addDays(-1))
        };

        const QList<MediaItem> durationAscending = MediaLibraryStore::filterAndSort(
            items, QString(), false, MediaLibraryStore::SortMode::Duration, false);
        QCOMPARE(durationAscending.first().displayName, QStringLiteral("a2.mp4"));
        QCOMPARE(durationAscending.last().displayName, QStringLiteral("alpha.mp4"));

        const QList<MediaItem> durationDescending = MediaLibraryStore::filterAndSort(
            items, QString(), false, MediaLibraryStore::SortMode::Duration, true);
        QCOMPARE(durationDescending.first().displayName, QStringLiteral("alpha.mp4"));
        QCOMPARE(durationDescending.last().displayName, QStringLiteral("a2.mp4"));

        const QList<MediaItem> nameDescending = MediaLibraryStore::filterAndSort(
            items, QString(), false, MediaLibraryStore::SortMode::Name, true);
        QCOMPARE(nameDescending.first().displayName, QStringLiteral("alpha.mp4"));
        QCOMPARE(nameDescending.last().displayName, QStringLiteral("a2.mp4"));

        const QList<MediaItem> modifiedDescending = MediaLibraryStore::filterAndSort(
            items, QString(), false, MediaLibraryStore::SortMode::RecentlyModified, true);
        QCOMPARE(modifiedDescending.first().displayName, QStringLiteral("a2.mp4"));
        QCOMPARE(modifiedDescending.last().displayName, QStringLiteral("a10.mp4"));
    }

    void favoriteFilterNarrowsResults()
    {
        const QDateTime modified = QDateTime::fromString(
            QStringLiteral("2026-09-19T10:00:00.000"), Qt::ISODateWithMs);
        const QList<MediaItem> items = {
            makeItem(QStringLiteral("a2.mp4"), 30000, 0, modified),
            makeItem(QStringLiteral("a10.mp4"), 90000, 6, modified.addDays(-2)),
            makeItem(QStringLiteral("alpha.mp4"), 120000, 1, modified.addDays(-1))
        };
        const QSet<QString> favorites = {
            MediaLibraryStore::normalizedMediaPath(QStringLiteral("C:/media/a2.mp4")),
            MediaLibraryStore::normalizedMediaPath(QStringLiteral("C:/media/alpha.mp4"))
        };

        const QList<MediaItem> onlyFavorites = MediaLibraryStore::filterAndSort(
            items, QString(), false, MediaLibraryStore::SortMode::Name, false,
            MediaLibraryStore::FavoriteFilter::OnlyFavorites, favorites);
        QCOMPARE(onlyFavorites.size(), 2);
        QCOMPARE(onlyFavorites.at(0).displayName, QStringLiteral("a2.mp4"));
        QCOMPARE(onlyFavorites.at(1).displayName, QStringLiteral("alpha.mp4"));

        const QList<MediaItem> notFavorites = MediaLibraryStore::filterAndSort(
            items, QString(), false, MediaLibraryStore::SortMode::Name, false,
            MediaLibraryStore::FavoriteFilter::NotFavorites, favorites);
        QCOMPARE(notFavorites.size(), 1);
        QCOMPARE(notFavorites.first().displayName, QStringLiteral("a10.mp4"));

        // The favourite filter stacks with the search box and script filter.
        const QList<MediaItem> searched = MediaLibraryStore::filterAndSort(
            items, QStringLiteral("alpha"), false, MediaLibraryStore::SortMode::Name,
            false, MediaLibraryStore::FavoriteFilter::OnlyFavorites, favorites);
        QCOMPARE(searched.size(), 1);
        QCOMPARE(searched.first().displayName, QStringLiteral("alpha.mp4"));

        const QList<MediaItem> withScript = MediaLibraryStore::filterAndSort(
            items, QString(), true, MediaLibraryStore::SortMode::Name, false,
            MediaLibraryStore::FavoriteFilter::OnlyFavorites, favorites);
        QCOMPARE(withScript.size(), 1);
        QCOMPARE(withScript.first().displayName, QStringLiteral("alpha.mp4"));

        QVERIFY(MediaLibraryStore::filterAndSort(
                    items, QString(), false, MediaLibraryStore::SortMode::Name, false,
                    MediaLibraryStore::FavoriteFilter::OnlyFavorites, QSet<QString>())
                    .isEmpty());

        // Existing callers keep seeing the whole library.
        QCOMPARE(MediaLibraryStore::filterAndSort(
                     items, QString(), false, MediaLibraryStore::SortMode::Name)
                     .size(),
                 3);
    }

    // 轴数必须按脚本内容算。合并成单个文件的多轴脚本只对应一个
    // .funscript，只数文件会把它标成"单轴"，与实际加载出来的轴数不符。
    void scriptAxesCountsAxesInTheFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString mediaPath = dir.filePath(QStringLiteral("clip.mp4"));
        const QString scriptPath = dir.filePath(QStringLiteral("clip.funscript"));

        const auto writeFile = [](const QString& path, const char* body) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly))
                return false;
            return file.write(body) == static_cast<qint64>(qstrlen(body));
        };

        QVERIFY(writeFile(mediaPath, "video"));
        QCOMPARE(MediaLibraryStore::scriptAxesFor(mediaPath), 0);

        // 单轴文件：一个顶层 actions 数组就是一条轴。
        QVERIFY(writeFile(scriptPath, R"({"actions":[{"at":0,"pos":0},{"at":100,"pos":50}]})"));
        QCOMPARE(MediaLibraryStore::scriptAxesFor(mediaPath), 1);

        // 合并多轴：同一个文件里带 axes 数组，主轴由顶层 actions 提供。
        QVERIFY(writeFile(scriptPath,
                          R"({"range":100,"axes":[)"
                          R"({"id":"L1","actions":[{"at":0,"pos":0}]},)"
                          R"({"id":"L2","actions":[{"at":0,"pos":0}]},)"
                          R"({"id":"R0","actions":[{"at":0,"pos":0}]},)"
                          R"({"id":"R1","actions":[{"at":0,"pos":0}]},)"
                          R"({"id":"R2","actions":[{"at":0,"pos":0}]})"
                          R"(],"actions":[{"at":0,"pos":0},{"at":100,"pos":100}]})"));
        QCOMPARE(MediaLibraryStore::scriptAxesFor(mediaPath), 6);

        // 拆分多轴：主脚本加上五个后缀文件，同样是六轴。
        QVERIFY(writeFile(scriptPath, R"({"actions":[{"at":0,"pos":0}]})"));
        const QStringList suffixes = {
            QStringLiteral("surge"), QStringLiteral("sway"), QStringLiteral("twist"),
            QStringLiteral("roll"), QStringLiteral("pitch")
        };
        for (const QString& suffix : suffixes)
        {
            QVERIFY(writeFile(dir.filePath(QStringLiteral("clip.") + suffix
                                           + QStringLiteral(".funscript")),
                              R"({"actions":[{"at":0,"pos":0}]})"));
        }
        QCOMPARE(MediaLibraryStore::scriptAxesFor(mediaPath), 6);
    }

    void durationAndExtensionHelpers()
    {
        QCOMPARE(MediaLibraryStore::formatDuration(0), QStringLiteral("--:--"));
        QCOMPARE(MediaLibraryStore::formatDuration(65000), QStringLiteral("1:05"));
        QCOMPARE(MediaLibraryStore::formatDuration(3725000), QStringLiteral("1:02:05"));

        QVERIFY(MediaLibraryStore::isVideoFile(QStringLiteral("C:/a/b.MP4")));
        QVERIFY(MediaLibraryStore::isVideoFile(QStringLiteral("C:/a/b.mkv")));
        QVERIFY(!MediaLibraryStore::isVideoFile(QStringLiteral("C:/a/b.funscript")));
        QVERIFY(!MediaLibraryStore::isVideoFile(QStringLiteral("C:/a/b.jpg")));
    }
};

QTEST_MAIN(MediaLibraryStoreTest)
#include "test_media_library_store.moc"
