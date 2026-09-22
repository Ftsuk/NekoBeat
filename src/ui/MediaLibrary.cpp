#include "core/Loc.h"
#include "ui/MediaLibrary.h"

#include "core/AppLogger.h"
#include "core/MediaReferenceIndex.h"
#include "core/PathUtils.h"
#include "core/ScriptLoader.h"
#include "media/MediaProbeQueue.h"
#include "ui/MediaGridWidget.h"
#include "ui/Theme.h"

#include <QActionGroup>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QRandomGenerator>
#include <QRunnable>
#include <QThreadPool>
#include <QScrollArea>
#include <QSettings>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <utility>

namespace
{
constexpr int kPixmapCacheLimit = 300;
// Probing decodes in software; two workers are fine while nothing plays, but
// one is enough to keep covers progressing during playback.
constexpr int kIdleProbeConcurrency = 2;

// Folder scanning is thousands of file-system calls (size, mtime, six
// axis-script probes and a cover probe per video). It used to run on the GUI
// thread, which meant every rescan - at startup, after adding a folder, after
// clearing the cache - froze the picture and, with a device attached, stopped
// the machine for as long as the disk took. It now runs here, in its own
// thread, and hands the result back in one queued call.
class LibraryScanRunnable : public QRunnable
{
public:
    LibraryScanRunnable(QPointer<MediaLibrary> owner, QString cacheRoot, QStringList roots)
        : m_owner(std::move(owner))
        , m_cacheRoot(std::move(cacheRoot))
        , m_roots(std::move(roots))
    {
        setAutoDelete(true);
    }

    void run() override
    {
        // Its own store instance: scanning must not touch the object the GUI
        // thread is using (the cache root stays the same, so the files it finds
        // and the thumbnails it reuses are identical).
        MediaLibraryStore store(m_cacheRoot);
        const QHash<QString, MediaItem> index = store.loadIndex();
        const QList<MediaItem> items = store.scanRoots(m_roots);

        MediaLibrary* owner = m_owner.data();
        if (!owner)
            return;
        QMetaObject::invokeMethod(
            owner, [owner, index, items] { owner->applyScanResult(index, items); },
            Qt::QueuedConnection);
    }

private:
    QPointer<MediaLibrary> m_owner;
    QString m_cacheRoot;
    QStringList m_roots;
};

QString formatFileSize(qint64 bytes)
{
    if (bytes <= 0)
        return LT("未知");

    const double kb = bytes / 1024.0;
    const double mb = kb / 1024.0;
    const double gb = mb / 1024.0;
    if (gb >= 1.0)
        return QStringLiteral("%1 GB").arg(gb, 0, 'f', 2);
    if (mb >= 1.0)
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(kb, 0, 'f', 0);
}

QString codecLabel(const QString& codec)
{
    if (codec.isEmpty())
        return LT("未知");

    QString value = codec;
    const int separator = value.indexOf(QStringLiteral(" / "));
    if (separator > 0)
        value = value.left(separator);
    const int parenthesis = value.indexOf(QLatin1Char('('));
    if (parenthesis > 0)
        value = value.left(parenthesis);
    return value.trimmed();
}

QString containerLabel(const QString& container)
{
    if (container.isEmpty())
        return LT("未知");

    const QString lower = container.toLower();
    if (lower.contains(QStringLiteral("mp4")))
        return QStringLiteral("MP4");
    if (lower.contains(QStringLiteral("matroska")))
        return QStringLiteral("MKV");
    if (lower.contains(QStringLiteral("webm")))
        return QStringLiteral("WebM");
    if (lower.contains(QStringLiteral("avi")))
        return QStringLiteral("AVI");
    if (lower.contains(QStringLiteral("asf")))
        return QStringLiteral("WMV");
    if (lower.contains(QStringLiteral("mpegts")))
        return QStringLiteral("TS");
    if (lower.contains(QStringLiteral("mov")))
        return QStringLiteral("MOV");

    const int comma = container.indexOf(QLatin1Char(','));
    return (comma > 0 ? container.left(comma) : container).trimmed();
}

QString scriptSummary(int axes)
{
    if (axes <= 0)
        return LT("无脚本");
    if (axes == 1)
        return LT("单轴");
    return LT("%1 轴").arg(axes);
}
}

MediaLibrary::MediaLibrary(QWidget* parent, const QString& cacheRoot)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("SidePanelLeft"));

    m_store = new MediaLibraryStore(cacheRoot);
    m_store->ensureDirectories();

    m_favorites = new FavoritesStore();
    if (!m_favorites->load())
    {
        AppLogger::log(QStringLiteral("library"),
                       QStringLiteral("收藏夹文件无法读取，已备份到 %1.bad")
                           .arg(m_favorites->filePath()));
    }

    m_probe = new MediaProbeQueue(this);
    m_probe->setTemporaryRoot(m_store->temporaryDir());

    m_pixmapCache = new QCache<QString, QPixmap>(kPixmapCacheLimit);

    buildUi();
    loadSettings();

    connect(m_probe, &MediaProbeQueue::probed, this, &MediaLibrary::applyProbeResult);
    connect(m_probe, &MediaProbeQueue::progressChanged, this,
            [this](int, int) { updateCount(); });
    connect(m_probe, &MediaProbeQueue::idle, this, [this]() {
        scheduleIndexSave();
        updateCount();
    });

    QTimer::singleShot(0, this, &MediaLibrary::rescan);
}

MediaLibrary::~MediaLibrary()
{
    saveSettings();
    if (m_probe)
        m_probe->clearPending();
    if (m_store && !m_items.isEmpty())
        m_store->saveIndex(indexSnapshot());
    if (m_probe)
    {
        m_probe->setParent(nullptr);
        delete m_probe;
        m_probe = nullptr;
    }
    delete m_store;
    m_store = nullptr;
    delete m_favorites;
    m_favorites = nullptr;
    delete m_pixmapCache;
    m_pixmapCache = nullptr;
}

void MediaLibrary::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    auto* searchRow = new QHBoxLayout();
    searchRow->setContentsMargins(0, 0, 0, 0);
    searchRow->setSpacing(6);

    m_favoriteButton = new QToolButton(this);
    m_favoriteButton->setObjectName(QStringLiteral("Compact"));
    m_favoriteButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_favoriteButton->setPopupMode(QToolButton::InstantPopup);
    m_favoriteButton->setCursor(Qt::PointingHandCursor);
    m_favoriteButton->setMaximumWidth(130);
    m_favoriteButton->setToolTip(LT("切换收藏夹视图"));
    searchRow->addWidget(m_favoriteButton);

    m_sortButton = new QToolButton(this);
    m_sortButton->setObjectName(QStringLiteral("Compact"));
    m_sortButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_sortButton->setPopupMode(QToolButton::InstantPopup);
    m_sortButton->setCursor(Qt::PointingHandCursor);
    m_sortButton->setToolTip(LT("排序方式"));
    searchRow->addWidget(m_sortButton);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(LT("搜索文件名…"));
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(40);
    searchRow->addWidget(m_search, 1);

    m_count = new QLabel(LT("空"), this);
    m_count->setObjectName(QStringLiteral("Muted"));
    searchRow->addWidget(m_count);

    // Sequential playback switch, mirroring the loop panel's button so both
    // panels expose the same control.
    m_autoNextButton = new QToolButton(this);
    m_autoNextButton->setObjectName(QStringLiteral("MediaButton"));
    m_autoNextButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_autoNextButton->setCheckable(true);
    m_autoNextButton->setCursor(Qt::PointingHandCursor);
    m_autoNextButton->setText(LT("连播"));
    m_autoNextButton->setToolTip(
        LT("连续播放：一个视频播完自动接着播列表里的下一个"));
    searchRow->addWidget(m_autoNextButton);
    connect(m_autoNextButton, &QToolButton::toggled, this, [this](bool enabled) {
        setAutoNext(enabled);
        emit autoNextChanged(enabled);
    });

    layout->addLayout(searchRow);

    buildSortMenu();
    buildFavoritesMenu();
    updateFavoritesButtonText();

    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("MediaGridScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_grid = new MediaGridWidget(m_scroll);
    m_grid->setPixmapCache(m_pixmapCache);
    m_scroll->setWidget(m_grid);
    layout->addWidget(m_scroll, 1);

    m_indexSaveTimer = new QTimer(this);
    m_indexSaveTimer->setSingleShot(true);
    m_indexSaveTimer->setInterval(1500);
    connect(m_indexSaveTimer, &QTimer::timeout, this, [this]() {
        if (m_store)
            // Missing entries are written too: without them the grey card would
            // lose its cover and details after a restart.
            m_store->saveIndex(indexSnapshot());
    });

    m_reorderTimer = new QTimer(this);
    m_reorderTimer->setSingleShot(true);
    m_reorderTimer->setInterval(400);
    connect(m_reorderTimer, &QTimer::timeout, this, [this]() {
        rebuildModel(selectedPath());
    });

    connect(m_search, &QLineEdit::textChanged, this,
            [this]() { rebuildModel(selectedPath()); });
    connect(m_grid, &MediaGridWidget::itemActivated, this, [this](const QString& path) {
        // A grey card has no file to play, so the double click stays harmless.
        if (m_missingPaths.contains(PathUtils::normalizeMediaPath(path)))
            return;
        emit mediaActivated(path, scriptForMedia(path));
    });
    connect(m_grid, &MediaGridWidget::selectionChanged, this,
            [this](const QString&) { updateDetails(); });
    connect(m_grid, &MediaGridWidget::contextMenuRequested, this,
            [this](const QString& path, const QPoint& globalPos) {
                showContextMenu(path, globalPos);
            });
    connect(m_grid, &MediaGridWidget::hoveredItemChanged, this,
            [this](const QString& path, const QPoint& globalPos) {
                if (path.isEmpty())
                {
                    QToolTip::hideText();
                    return;
                }
                const QString info = mediaInfoFor(path);
                if (!info.isEmpty())
                    QToolTip::showText(globalPos, info, m_grid);
            });
}

void MediaLibrary::buildFavoritesMenu()
{
    m_favoriteMenu = new QMenu(this);
    connect(m_favoriteMenu, &QMenu::aboutToShow, this, [this]() {
        m_favoriteMenu->clear();

        QAction* allAction = m_favoriteMenu->addAction(LT("全部视频"));
        allAction->setCheckable(true);
        allAction->setChecked(m_favoriteView == FavoriteView::All);
        connect(allAction, &QAction::triggered, this, &MediaLibrary::showAllVideos);

        QAction* noneAction = m_favoriteMenu->addAction(LT("未收藏"));
        noneAction->setCheckable(true);
        noneAction->setChecked(m_favoriteView == FavoriteView::NotFavorites);
        connect(noneAction, &QAction::triggered, this, &MediaLibrary::showUnfavorited);

        m_favoriteMenu->addSeparator();
        if (m_favorites->folders().isEmpty())
        {
            QAction* emptyAction = m_favoriteMenu->addAction(LT("（还没有收藏夹）"));
            emptyAction->setEnabled(false);
        }
        for (const FavoriteFolder& folder : m_favorites->folders())
        {
            QAction* action = m_favoriteMenu->addAction(
                QStringLiteral("%1 (%2)").arg(folder.name).arg(folder.members.size()));
            action->setCheckable(true);
            action->setChecked(m_favoriteView == FavoriteView::Folder
                               && m_favoriteFolderId == folder.id);
            const QString folderId = folder.id;
            connect(action, &QAction::triggered, this, [this, folderId]() {
                showFavoriteFolder(folderId);
            });
        }

        m_favoriteMenu->addSeparator();
        QAction* manageAction = m_favoriteMenu->addAction(LT("管理收藏夹…"));
        connect(manageAction, &QAction::triggered, this, [this]() {
            QTimer::singleShot(0, this, [this]() { emit manageFavoritesRequested(); });
        });
    });

    m_favoriteButton->setMenu(m_favoriteMenu);
}

void MediaLibrary::buildSortMenu()
{
    m_sortMenu = new QMenu(this);
    const struct
    {
        QString label;
        MediaLibraryStore::SortMode mode;
        bool descending;
    } entries[] = {
        {LT("名称 A→Z"), MediaLibraryStore::SortMode::Name, false},
        {LT("名称 Z→A"), MediaLibraryStore::SortMode::Name, true},
        {LT("时长 短→长"), MediaLibraryStore::SortMode::Duration, false},
        {LT("时长 长→短"), MediaLibraryStore::SortMode::Duration, true},
        {LT("修改日期 旧→新"), MediaLibraryStore::SortMode::RecentlyModified, false},
        {LT("修改日期 新→旧"), MediaLibraryStore::SortMode::RecentlyModified, true}
    };

    auto* group = new QActionGroup(this);
    for (const auto& entry : entries)
    {
        QAction* action = m_sortMenu->addAction(entry.label);
        action->setCheckable(true);
        action->setData(static_cast<int>(entry.mode) * 2 + (entry.descending ? 1 : 0));
        group->addAction(action);
        const auto mode = entry.mode;
        const bool descending = entry.descending;
        connect(action, &QAction::triggered, this, [this, mode, descending]() {
            setSortMode(mode);
            setSortDescending(descending);
        });
    }

    connect(m_sortMenu, &QMenu::aboutToShow, this, [this]() {
        const int value = static_cast<int>(m_sortMode) * 2 + (m_sortDescending ? 1 : 0);
        for (QAction* action : m_sortMenu->actions())
            action->setChecked(action->data().toInt() == value);
    });

    m_sortButton->setMenu(m_sortMenu);
    updateSortButtonText();
}

void MediaLibrary::updateSortButtonText()
{
    if (!m_sortButton)
        return;

    // Below this width the long labels would squeeze the search box away, so
    // the button falls back to a two-character shorthand.
    const bool compact = width() < 260;
    QString text;
    switch (m_sortMode)
    {
    case MediaLibraryStore::SortMode::Duration:
        text = compact ? (m_sortDescending ? LT("时↓") : LT("时↑"))
                       : (m_sortDescending ? LT("时长 长→短")
                                           : LT("时长 短→长"));
        break;
    case MediaLibraryStore::SortMode::RecentlyModified:
        text = compact ? (m_sortDescending ? LT("日↓") : LT("日↑"))
                       : (m_sortDescending ? LT("日期 新→旧")
                                           : LT("日期 旧→新"));
        break;
    case MediaLibraryStore::SortMode::Name:
        text = compact ? (m_sortDescending ? LT("名↓") : LT("名↑"))
                       : (m_sortDescending ? LT("名称 Z→A")
                                           : LT("名称 A→Z"));
        break;
    }
    m_sortButton->setText(text);
}

void MediaLibrary::updateFavoritesButtonText()
{
    if (!m_favoriteButton)
        return;

    QString text;
    QString tooltip;
    switch (m_favoriteView)
    {
    case FavoriteView::NotFavorites:
        text = LT("★ 未收藏");
        tooltip = LT("只显示不属于任何收藏夹的视频");
        break;
    case FavoriteView::Folder:
    {
        const QString name = m_favorites->folderName(m_favoriteFolderId);
        const QString shortName = name.size() > 7 ? name.left(6) + QStringLiteral("…")
                                                  : name;
        text = QStringLiteral("★ %1").arg(shortName);
        tooltip = LT("收藏夹「%1」（%2 个视频）")
                      .arg(name)
                      .arg(m_favorites->memberCount(m_favoriteFolderId));
        break;
    }
    case FavoriteView::All:
        text = LT("★ 全部");
        tooltip = LT("显示全部视频");
        break;
    }

    m_favoriteButton->setText(text);
    m_favoriteButton->setToolTip(tooltip);
}

void MediaLibrary::loadSettings()
{
    QSettings settings;
    m_roots = MediaLibraryStore::normalizeRoots(
        settings.value(QStringLiteral("library/roots")).toStringList());
    m_sortMode = static_cast<MediaLibraryStore::SortMode>(
        settings.value(QStringLiteral("library/sortMode"),
                       static_cast<int>(MediaLibraryStore::SortMode::Name)).toInt());
    m_sortDescending = settings.value(QStringLiteral("library/sortDescending"), false).toBool();
    m_onlyWithScript = settings.value(QStringLiteral("library/onlyWithScript"), false).toBool();
    m_thumbSize = settings.value(QStringLiteral("library/thumbSize"),
                                 settings.value(QStringLiteral("library/thumbPercent"), 200)
                                     .toInt())
                      .toInt();
    m_fontPercent = settings.value(QStringLiteral("library/fontPercent"),
                                   settings.value(QStringLiteral("library/fontStep"), 1).toInt()
                                       == 2 ? 140 : 100)
                        .toInt();

    if (m_sortMode != MediaLibraryStore::SortMode::Name
        && m_sortMode != MediaLibraryStore::SortMode::Duration
        && m_sortMode != MediaLibraryStore::SortMode::RecentlyModified)
    {
        m_sortMode = MediaLibraryStore::SortMode::Name;
    }
    m_thumbSize = qBound(80, m_thumbSize, 600);
    m_fontPercent = qBound(70, m_fontPercent, 200);

    applyCardMetrics();
    updateSortButtonText();
}

void MediaLibrary::saveSettings()
{
    QSettings settings;
    settings.setValue(QStringLiteral("library/roots"), m_roots);
    settings.setValue(QStringLiteral("library/sortMode"), static_cast<int>(m_sortMode));
    settings.setValue(QStringLiteral("library/sortDescending"), m_sortDescending);
    settings.setValue(QStringLiteral("library/onlyWithScript"), m_onlyWithScript);
    settings.setValue(QStringLiteral("library/thumbSize"), m_thumbSize);
    settings.setValue(QStringLiteral("library/fontPercent"), m_fontPercent);
}

void MediaLibrary::applyCardMetrics()
{
    if (!m_grid)
        return;
    m_grid->setCardWidth(m_thumbSize + 6);
    m_grid->setFontSize(qBound(10, qRound(12 * m_fontPercent / 100.0), 22));
}

void MediaLibrary::addRootFolder(const QString& folder)
{
    if (folder.trimmed().isEmpty())
        return;

    const QString normalized = QDir::cleanPath(QFileInfo(folder).absoluteFilePath());
    AppLogger::log(QStringLiteral("library"),
                   QStringLiteral("添加媒体文件夹: %1").arg(normalized));

    if (!m_roots.contains(normalized, Qt::CaseInsensitive))
        m_roots.append(normalized);

    MediaLibraryStore::saveRootFolders(m_roots);
    emit rootsChanged();
    rescan();
}

void MediaLibrary::setRootFolders(const QStringList& folders)
{
    m_roots = MediaLibraryStore::normalizeRoots(folders);
    MediaLibraryStore::saveRootFolders(m_roots);
    emit rootsChanged();
    rescan();
}

void MediaLibrary::removeRootFolder(const QString& folder)
{
    const QString normalized = QDir::cleanPath(QFileInfo(folder).absoluteFilePath());
    const int index = m_roots.indexOf(normalized);
    if (index < 0)
        return;

    AppLogger::log(QStringLiteral("library"),
                   QStringLiteral("移除媒体文件夹: %1").arg(normalized));
    m_roots.removeAt(index);
    MediaLibraryStore::saveRootFolders(m_roots);
    emit rootsChanged();
    rescan();
    emit message(LT("已移除媒体库文件夹：%1").arg(normalized), 4000);
}

void MediaLibrary::refreshAll()
{
    AppLogger::log(QStringLiteral("library"), LT("刷新媒体库"));
    rescan();
    emit message(LT("媒体库已刷新"), 2500);
}

void MediaLibrary::setThumbSize(int pixels)
{
    m_thumbSize = qBound(80, pixels, 600);
    applyCardMetrics();
    saveSettings();
    emit settingsChanged();
}

void MediaLibrary::setFontPercent(int percent)
{
    m_fontPercent = qBound(70, percent, 200);
    applyCardMetrics();
    saveSettings();
    emit settingsChanged();
}

void MediaLibrary::setAxisTagVisible(bool visible)
{
    if (m_axisTagVisible == visible)
        return;
    m_axisTagVisible = visible;
    if (m_grid)
        m_grid->setShowAxisTag(visible);
}

void MediaLibrary::setScriptFilter(bool onlyWithScript)
{
    if (m_onlyWithScript == onlyWithScript)
        return;
    m_onlyWithScript = onlyWithScript;
    saveSettings();
    rebuildModel(selectedPath());
    emit settingsChanged();
}

void MediaLibrary::setSortMode(MediaLibraryStore::SortMode mode)
{
    if (m_sortMode == mode)
        return;
    m_sortMode = mode;
    saveSettings();
    rebuildModel(selectedPath());
    emit settingsChanged();
    updateSortButtonText();
}

void MediaLibrary::setSortDescending(bool descending)
{
    if (m_sortDescending == descending)
        return;
    m_sortDescending = descending;
    saveSettings();
    rebuildModel(selectedPath());
    emit settingsChanged();
    updateSortButtonText();
}

QList<FavoriteFolder> MediaLibrary::favoriteFolders() const
{
    return m_favorites ? m_favorites->folders() : QList<FavoriteFolder>();
}

QString MediaLibrary::favoriteFolderName(const QString& folderId) const
{
    return m_favorites ? m_favorites->folderName(folderId) : QString();
}

bool MediaLibrary::hasFavoriteFolder(const QString& folderId) const
{
    return m_favorites && m_favorites->contains(folderId);
}

int MediaLibrary::favoriteFolderMemberCount(const QString& folderId) const
{
    return m_favorites ? m_favorites->memberCount(folderId) : 0;
}

int MediaLibrary::favoriteMissingCount(const QString& folderId) const
{
    return m_favorites ? m_favorites->missingCount(folderId, availablePathSet()) : 0;
}

int MediaLibrary::favoriteMissingTotal() const
{
    return m_favorites ? m_favorites->missingCount(availablePathSet()) : 0;
}

bool MediaLibrary::isFavorite(const QString& mediaPath) const
{
    return m_favorites && m_favorites->isFavorite(mediaPath);
}

QStringList MediaLibrary::favoriteNamesFor(const QString& mediaPath) const
{
    return m_favorites ? m_favorites->folderNamesFor(mediaPath) : QStringList();
}

QSet<QString> MediaLibrary::availablePathSet() const
{
    // Not "everything the scan found" any more: the availability check decides
    // what counts as usable, and it also knows which paths are merely offline.
    return m_availablePaths;
}

void MediaLibrary::setAdditionalReferences(const QStringList& mediaPaths)
{
    if (m_additionalReferences == mediaPaths)
        return;
    m_additionalReferences = mediaPaths;
    refreshAvailability();
    rebuildModel(selectedPath());
}

void MediaLibrary::refreshAvailability()
{
    if (!m_store)
        return;

    MediaReferenceIndex index;
    for (const MediaItem& item : m_items)
        index.addKnownAvailable(item.path);

    // The normalised key is what everything compares on, but the cards should
    // show the file name exactly as the user has it on disk.
    QHash<QString, QString> originalByKey;
    if (m_favorites)
    {
        for (const FavoriteFolder& folder : m_favorites->folders())
        {
            for (const FavoriteMember& member : folder.members)
            {
                const QString key = FavoritesStore::normalizePath(member.path);
                if (key.isEmpty())
                    continue;
                originalByKey.insert(key, member.path);
                index.addReference(member.path);
            }
        }
    }
    for (const QString& raw : std::as_const(m_additionalReferences))
    {
        const QString key = PathUtils::normalizeMediaPath(raw);
        if (key.isEmpty())
            continue;
        originalByKey.insert(key, raw);
        index.addReference(raw);
    }
    index.resolve();

    m_availablePaths = index.availablePaths();
    m_missingPaths = index.missingPaths();
    m_unreachableRoots = index.unreachableRoots();

    // Rebuild the grey cards from the previous index so a missing entry still
    // shows its cover and the details probed before it disappeared.
    QList<MediaItem> orphans;
    orphans.reserve(m_missingPaths.size());
    for (const QString& key : std::as_const(m_missingPaths))
    {
        const QString original = originalByKey.value(key, key);
        MediaItem item;
        const auto known = m_indexEntries.constFind(key);
        if (known != m_indexEntries.constEnd())
        {
            item = known.value();
            // The stored key may predate the folder-independent scheme, while
            // the thumbnail file has already been relocated under the new one,
            // so recompute instead of trusting the value written to disk.
            if (item.fileSize > 0 && item.modified.isValid())
            {
                item.cacheKey =
                    m_store->cacheKeyFor(item.path, item.fileSize, item.modified);
                item.thumbnailPath = m_store->thumbnailPathFor(item.cacheKey);
            }
        }
        item.path = original;
        if (item.displayName.isEmpty())
            item.displayName = QFileInfo(original).fileName();
        item.missing = true;
        // Never queue a probe for a file that is not there; keep the cached
        // cover when the thumbnail file survived.
        item.thumbnailState =
            (!item.thumbnailPath.isEmpty() && QFileInfo::exists(item.thumbnailPath))
                ? ThumbnailState::Ready
                : ThumbnailState::Failed;
        orphans.append(item);
    }
    m_missingItems = orphans;

    emit availabilityChanged();
}

QHash<QString, QString> MediaLibrary::thumbnailPathsByMedia() const
{
    QHash<QString, QString> thumbnails;
    thumbnails.reserve(m_items.size());
    for (const MediaItem& item : m_items)
    {
        if (item.thumbnailState != ThumbnailState::Ready || item.thumbnailPath.isEmpty())
            continue;
        const QString key = MediaLibraryStore::normalizedMediaPath(item.path);
        if (!key.isEmpty())
            thumbnails.insert(key, item.thumbnailPath);
    }
    return thumbnails;
}

QHash<QString, int> MediaLibrary::scriptAxesByMedia() const
{
    QHash<QString, int> axes;
    axes.reserve(m_items.size());
    for (const MediaItem& item : m_items)
    {
        const QString key = MediaLibraryStore::normalizedMediaPath(item.path);
        if (!key.isEmpty())
            axes.insert(key, item.scriptAxes);
    }
    return axes;
}

void MediaLibrary::showAllVideos()
{
    if (m_favoriteView == FavoriteView::All)
    {
        updateFavoritesButtonText();
        return;
    }

    m_favoriteView = FavoriteView::All;
    m_favoriteFolderId.clear();
    rebuildModel(selectedPath());
    updateFavoritesButtonText();
}

void MediaLibrary::showUnfavorited()
{
    if (m_favoriteView == FavoriteView::NotFavorites)
        return;

    m_favoriteView = FavoriteView::NotFavorites;
    m_favoriteFolderId.clear();
    rebuildModel(selectedPath());
    updateFavoritesButtonText();
}

void MediaLibrary::showFavoriteFolder(const QString& folderId)
{
    if (!m_favorites || !m_favorites->contains(folderId))
        return;

    m_favoriteView = FavoriteView::Folder;
    m_favoriteFolderId = folderId;
    rebuildModel(selectedPath());
    updateFavoritesButtonText();
    AppLogger::log(QStringLiteral("library"),
                   QStringLiteral("切换到收藏夹: %1").arg(m_favorites->folderName(folderId)));
}

void MediaLibrary::refreshFavorites()
{
    if (!m_favorites)
        return;

    if (m_favoriteView == FavoriteView::Folder
        && !m_favorites->contains(m_favoriteFolderId))
    {
        m_favoriteView = FavoriteView::All;
        m_favoriteFolderId.clear();
    }

    rebuildModel(selectedPath());
    updateFavoritesButtonText();
}

QString MediaLibrary::selectedMediaPath() const
{
    return selectedPath();
}

QString MediaLibrary::selectedScriptPath() const
{
    return scriptForMedia(selectedPath());
}

QString MediaLibrary::scriptForMedia(const QString& mediaPath) const
{
    const int index = indexOfItem(mediaPath);
    return index >= 0 ? m_items.at(index).preferredScript : QString();
}

QString MediaLibrary::mediaInfoFor(const QString& mediaPath) const
{
    const int index = indexOfItem(mediaPath);
    if (index < 0)
        return {};

    const MediaItem& item = m_items.at(index);
    const QString resolution = (item.width > 0 && item.height > 0)
                                   ? QStringLiteral("%1×%2").arg(item.width).arg(item.height)
                                   : LT("未知");
    const QString duration = item.metadataReady
                                 ? MediaLibraryStore::formatDuration(item.durationMs)
                                 : LT("获取中");
    QString hwdec = LT("自动（播放时生效）");
    if (!m_playingPath.isEmpty()
        && m_playingPath.compare(item.path, Qt::CaseInsensitive) == 0
        && !m_playingHwdec.isEmpty())
    {
        hwdec = m_playingHwdec;
    }

    QString scriptLine = scriptSummary(item.scriptAxes);
    if (!item.preferredScript.isEmpty())
        scriptLine += QStringLiteral("（%1）").arg(QFileInfo(item.preferredScript).fileName());

    const QStringList favoriteNames = favoriteNamesFor(item.path);
    const QString favoritesLine = favoriteNames.isEmpty()
                                      ? LT("无")
                                      : favoriteNames.join(QStringLiteral("、"));

    return LT("文件：%1\n路径：%2\n分辨率：%3\n时长：%4\n大小：%5\n容器：%6\n视频编码：%7\n音频编码：%8\n硬解：%9\n脚本：%10\n收藏夹：%11")
        .arg(item.displayName, item.path, resolution, duration,
             formatFileSize(item.fileSize), containerLabel(item.container),
             codecLabel(item.videoCodec), codecLabel(item.audioCodec), hwdec, scriptLine,
             favoritesLine);
}

MediaPlaybackRequest MediaLibrary::nextMedia(const QString& currentMediaPath) const
{
    if (m_visible.isEmpty())
        return {};

    int current = -1;
    for (int i = 0; i < m_visible.size(); ++i)
    {
        if (m_visible.at(i).path.compare(currentMediaPath, Qt::CaseInsensitive) == 0)
        {
            current = i;
            break;
        }
    }
    if (current < 0 || current + 1 >= m_visible.size())
        return {};

    const MediaItem& next = m_visible.at(current + 1);
    return {next.path, next.preferredScript};
}

QString MediaLibrary::selectedPath() const
{
    return m_grid ? m_grid->selectedPath() : QString();
}

void MediaLibrary::selectPath(const QString& mediaPath)
{
    if (!m_grid)
        return;
    m_grid->setSelectedPath(mediaPath);
}

bool MediaLibrary::isGeneratingThumbnails() const
{
    return m_probe && !m_probe->isIdle();
}

int MediaLibrary::thumbnailsFinished() const
{
    return m_probe ? m_probe->finishedCount() : 0;
}

int MediaLibrary::thumbnailsTotal() const
{
    return m_probe ? m_probe->totalCount() : 0;
}

QString MediaLibrary::cacheRoot() const
{
    return m_store ? m_store->cacheRoot() : QString();
}

void MediaLibrary::stopThumbnailGeneration()
{
    if (m_probe)
        m_probe->clearPending();
    updateCount();
    emit message(LT("已停止生成缩略图"), 3000);
}

void MediaLibrary::clearThumbnailCache()
{
    if (m_probe)
        m_probe->clearPending();
    m_store->clearCache();
    m_store->ensureDirectories();
    if (m_pixmapCache)
        m_pixmapCache->clear();
    AppLogger::log(QStringLiteral("library"), LT("清理缩略图缓存"));
    rescan();
    emit message(LT("缩略图缓存已清理"), 3000);
}

void MediaLibrary::regenerateAllThumbnails()
{
    if (!m_probe)
        return;

    int queued = 0;
    for (MediaItem& item : m_items)
    {
        if (!item.thumbnailPath.isEmpty())
        {
            QFile::remove(item.thumbnailPath);
            if (m_grid)
                m_grid->invalidatePixmap(item.thumbnailPath);
        }
        item.thumbnailState = ThumbnailState::Pending;
        if (m_grid)
            m_grid->updateItem(item);
        m_probe->enqueue(item.path, item.thumbnailPath,
                         QRandomGenerator::global()->bounded(5, 91) / 100.0, true, true);
        ++queued;
    }
    updateCount();
    emit message(LT("正在重新生成 %1 个缩略图…").arg(queued), 4000);
}

void MediaLibrary::rescan()
{
    if (!m_store)
        return;
    if (m_rescanning)
    {
        // A scan is already running: remember that one more pass is wanted and
        // start it when the current one lands instead of dropping the request or
        // running two scans over the same folders.
        m_rescanQueued = true;
        return;
    }

    m_rescanning = true;
    m_scanKeepSelection = selectedPath();
    AppLogger::log(QStringLiteral("library"),
                   QStringLiteral("开始扫描 %1 个根目录（后台线程）").arg(m_roots.size()));
    m_scanPool.start(new LibraryScanRunnable(QPointer<MediaLibrary>(this),
                                             m_store->cacheRoot(), m_roots));
}

void MediaLibrary::applyScanResult(const QHash<QString, MediaItem>& index,
                                   const QList<MediaItem>& items)
{
    if (!m_rescanning)
        return; // A shutdown or a cache clear cancelled this pass.

    m_indexEntries = index;
    m_items = items;
    refreshAvailability();
    rebuildModel(m_scanKeepSelection);
    startThumbnailJobs();
    m_rescanning = false;

    AppLogger::log(QStringLiteral("library"),
                   QStringLiteral("扫描完成: %1 个根目录，%2 个视频")
                       .arg(m_roots.size())
                       .arg(m_items.size()));

    if (m_rescanQueued)
    {
        m_rescanQueued = false;
        rescan();
    }
}

void MediaLibrary::rebuildModel(const QString& keepSelection, int fallbackRow)
{
    if (m_favoriteView == FavoriteView::Folder
        && !m_favorites->contains(m_favoriteFolderId))
    {
        m_favoriteView = FavoriteView::All;
        m_favoriteFolderId.clear();
        updateFavoritesButtonText();
    }

    MediaLibraryStore::FavoriteFilter favoriteFilter =
        MediaLibraryStore::FavoriteFilter::All;
    QSet<QString> favoritePaths;
    if (m_favoriteView == FavoriteView::NotFavorites)
    {
        favoriteFilter = MediaLibraryStore::FavoriteFilter::NotFavorites;
    }
    else if (m_favoriteView == FavoriteView::Folder)
    {
        favoriteFilter = MediaLibraryStore::FavoriteFilter::OnlyFavorites;
        favoritePaths = m_favorites->memberPaths(m_favoriteFolderId);
    }

    // Missing entries ride along in the same list so they sort next to the file
    // they belong to; that is what makes a reorganised library easy to spot.
    QList<MediaItem> allItems = m_items;
    allItems.append(m_missingItems);
    m_visible = MediaLibraryStore::filterAndSort(
        allItems, m_search ? m_search->text().trimmed() : QString(),
        m_onlyWithScript, m_sortMode, m_sortDescending, favoriteFilter, favoritePaths);
    if (m_grid)
    {
        m_grid->setFavoritePaths(m_favorites->allMemberPaths());
        m_grid->setItems(m_visible);
    }

    const auto visibleContains = [this](const QString& path) {
        for (const MediaItem& item : m_visible)
        {
            if (item.path.compare(path, Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };

    if (keepSelection.isEmpty())
    {
        selectPath(QString());
    }
    else if (visibleContains(keepSelection))
    {
        selectPath(keepSelection);
    }
    else if (fallbackRow >= 0 && !m_visible.isEmpty())
    {
        selectPath(m_visible.at(qBound(0, fallbackRow, m_visible.size() - 1)).path);
    }
    else
    {
        selectPath(QString());
    }

    updateCount();
    updateDetails();
    emit contentChanged();
}

void MediaLibrary::startThumbnailJobs()
{
    if (!m_probe)
        return;

    for (const MediaItem& item : m_items)
    {
        if (item.thumbnailState == ThumbnailState::Ready)
        {
            // The cover is already on disk and is reused, so nothing may render it
            // again. Only the metadata (duration, resolution, codecs) may still be
            // missing - that is read without touching the cover at all.
            if (!item.metadataReady)
                m_probe->enqueue(item.path, QString());
            continue;
        }
        m_probe->enqueue(item.path, item.thumbnailPath);
    }
    updateCount();
}

void MediaLibrary::refreshItem(const MediaItem& item)
{
    if (m_grid)
        m_grid->updateItem(item);
}

void MediaLibrary::applyProbeResult(const QString& mediaPath,
                                    const MediaProbeResult& result)
{
    const int index = indexOfItem(mediaPath);
    if (index < 0)
        return;

    MediaItem& item = m_items[index];
    item.durationMs = result.durationMs;
    item.width = result.width;
    item.height = result.height;
    if (!result.container.isEmpty())
        item.container = result.container;
    if (!result.videoCodec.isEmpty())
        item.videoCodec = result.videoCodec;
    if (!result.audioCodec.isEmpty())
        item.audioCodec = result.audioCodec;
    item.metadataReady = item.metadataReady || result.metadataOk;
    // A metadata-only probe (the cover was already cached and deliberately kept)
    // must not overwrite the state of that cover.
    if (result.thumbnailRequested)
    {
        item.thumbnailState = result.thumbnailOk ? ThumbnailState::Ready
                                                 : ThumbnailState::Failed;
    }

    refreshItem(item);
    if (result.thumbnailOk && m_grid)
        m_grid->invalidatePixmap(item.thumbnailPath);
    scheduleIndexSave();

    if (m_sortMode == MediaLibraryStore::SortMode::Duration
        || m_sortMode == MediaLibraryStore::SortMode::RecentlyModified)
    {
        scheduleReorder();
    }
    updateCount();
}

void MediaLibrary::scheduleIndexSave()
{
    if (m_indexSaveTimer)
        m_indexSaveTimer->start();
}

QList<MediaItem> MediaLibrary::indexSnapshot() const
{
    QList<MediaItem> items = m_items;
    items += m_missingItems;

    QSet<QString> known;
    known.reserve(items.size());
    for (const MediaItem& item : items)
        known.insert(MediaLibraryStore::normalizedMediaPath(item.path));

    // Entries of folders that are not part of the library right now stay in the
    // file. Removing a media folder used to drop the index entries of everything
    // inside it, and re-adding the folder then had to probe - and draw - every
    // cover again. Keeping the entry is what makes "same path, same cover" hold
    // across a remove/re-add or a library that lives on an unplugged drive.
    for (auto it = m_indexEntries.constBegin(); it != m_indexEntries.constEnd(); ++it)
    {
        const MediaItem& item = it.value();
        if (item.path.isEmpty() || known.contains(it.key()))
            continue;
        // Only keep what is still backed by something on disk: the video itself
        // or its cached cover. Anything else is dead weight in the index.
        const bool mediaThere = QFileInfo::exists(item.path);
        const bool coverThere = !item.thumbnailPath.isEmpty()
                                && QFileInfo::exists(item.thumbnailPath);
        if (!mediaThere && !coverThere)
            continue;
        items.append(item);
        known.insert(it.key());
    }

    return items;
}

void MediaLibrary::scheduleReorder()
{
    if (m_reorderTimer && !m_reorderTimer->isActive())
        m_reorderTimer->start();
}

int MediaLibrary::indexOfItem(const QString& mediaPath) const
{
    for (int index = 0; index < m_items.size(); ++index)
    {
        if (m_items.at(index).path.compare(mediaPath, Qt::CaseInsensitive) == 0)
            return index;
    }
    return -1;
}

void MediaLibrary::updateCount()
{
    if (!m_count)
        return;

    const QString needle = m_search ? m_search->text().trimmed() : QString();
    QString text;
    if (m_items.isEmpty())
        text = LT("空");
    else if (needle.isEmpty() && !m_onlyWithScript
             && m_favoriteView == FavoriteView::All)
        text = LT("%1 个").arg(m_items.size());
    else
        text = QStringLiteral("%1/%2").arg(m_visible.size()).arg(m_items.size());

    if (isGeneratingThumbnails() && thumbnailsTotal() > 0)
        text += QStringLiteral(" · %1/%2").arg(thumbnailsFinished()).arg(thumbnailsTotal());
    m_count->setText(text);
}

void MediaLibrary::updateDetails()
{
    // Details are shown in the hover tooltip and the info dialog.
}

QSize MediaLibrary::minimumSizeHint() const
{
    // Zero width on purpose: the panel has to be shrinkable all the way to the
    // window edge without a hard stop. The search row simply clips on the way.
    return QSize(0, 120);
}

void MediaLibrary::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);

    // Drop the counter first when the panel gets tight so the row stays
    // readable on the way down to the collapse threshold.
    if (m_count)
        m_count->setVisible(width() >= 260);
    updateSortButtonText();
}

void MediaLibrary::setPlayingMedia(const QString& mediaPath, const QString& hwdec)
{
    m_playingPath = mediaPath;
    m_playingHwdec = hwdec;
}

void MediaLibrary::setPlaybackActive(bool active)
{
    if (m_playbackActive == active)
        return;
    m_playbackActive = active;
    if (!m_probe)
        return;

    // One worker while playing, two when idle: covers have to keep progressing
    // (a stalled queue never finishes), but not at the cost of the picture.
    m_probe->setConcurrency(active ? 1 : kIdleProbeConcurrency);
    AppLogger::log(QStringLiteral("library"),
                   QStringLiteral("缩略图探测并发：%1（%2）")
                       .arg(active ? 1 : kIdleProbeConcurrency)
                       .arg(active ? QStringLiteral("播放中") : QStringLiteral("空闲")));
}

void MediaLibrary::setAutoNext(bool enabled)
{
    m_autoNext = enabled;
    if (!m_autoNextButton)
        return;

    m_autoNextButton->blockSignals(true);
    m_autoNextButton->setChecked(enabled);
    m_autoNextButton->blockSignals(false);
    m_autoNextButton->setObjectName(enabled ? QStringLiteral("MediaButtonActive")
                                            : QStringLiteral("MediaButton"));
    m_autoNextButton->style()->unpolish(m_autoNextButton);
    m_autoNextButton->style()->polish(m_autoNextButton);
}

void MediaLibrary::showContextMenu(const QString& mediaPath, const QPoint& globalPos)
{
    QMenu menu(this);

    if (mediaPath.isEmpty())
    {
        menu.addAction(LT("刷新媒体库"), this, &MediaLibrary::refreshAll);
        menu.exec(globalPos);
        return;
    }

    // A grey card has nothing to play and nothing to probe; its menu is about
    // deciding what happens to the records that still point at the old path.
    if (m_missingPaths.contains(PathUtils::normalizeMediaPath(mediaPath)))
    {
        menu.addAction(LT("重新关联到新位置…"), this, [this, mediaPath]() {
            QTimer::singleShot(0, this, [this, mediaPath]() {
                emit relinkRequested(mediaPath);
            });
        });
        menu.addSeparator();
        menu.addAction(LT("从收藏夹移除"), this, [this, mediaPath]() {
            QTimer::singleShot(0, this, [this, mediaPath]() {
                removeFromAllFavorites(mediaPath);
            });
        });
        menu.addAction(LT("清理此条失效记录…"), this, [this, mediaPath]() {
            QTimer::singleShot(0, this, [this, mediaPath]() {
                emit cleanupRequested(mediaPath);
            });
        });
        menu.addSeparator();
        menu.addAction(LT("打开所在文件夹"), this, [this, mediaPath]() {
            QTimer::singleShot(0, this, [this, mediaPath]() {
                revealInExplorer(mediaPath);
            });
        });
        menu.exec(globalPos);
        return;
    }

    menu.addAction(LT("播放"), this, [this, mediaPath]() {
        QTimer::singleShot(0, this, [this, mediaPath]() {
            emit mediaActivated(mediaPath, scriptForMedia(mediaPath));
        });
    });
    menu.addAction(LT("选择脚本…"), this, [this, mediaPath]() {
        QTimer::singleShot(0, this, [this, mediaPath]() { chooseScriptWithDialog(mediaPath); });
    });
    menu.addAction(LT("媒体信息…"), this, [this, mediaPath]() {
        QTimer::singleShot(0, this, [this, mediaPath]() { showMediaInfo(mediaPath); });
    });

    QMenu* favoritesMenu = menu.addMenu(LT("收藏夹操作"));
    const QStringList memberOf = m_favorites->folderIdsFor(mediaPath);
    for (const FavoriteFolder& folder : m_favorites->folders())
    {
        QAction* action = favoritesMenu->addAction(folder.name);
        action->setCheckable(true);
        action->setChecked(memberOf.contains(folder.id));
        const QString folderId = folder.id;
        connect(action, &QAction::triggered, this,
                [this, folderId, mediaPath](bool checked) {
                    QTimer::singleShot(0, this, [this, folderId, mediaPath, checked]() {
                        setMediaFavorite(folderId, mediaPath, checked);
                    });
                });
    }
    if (m_favorites->folders().isEmpty())
    {
        QAction* emptyAction = favoritesMenu->addAction(LT("（还没有收藏夹）"));
        emptyAction->setEnabled(false);
    }
    favoritesMenu->addSeparator();
    QAction* createAction = favoritesMenu->addAction(LT("新建收藏夹…"));
    connect(createAction, &QAction::triggered, this, [this, mediaPath]() {
        QTimer::singleShot(0, this, [this, mediaPath]() { createFolderForMedia(mediaPath); });
    });
    favoritesMenu->addSeparator();

    // "移出当前收藏夹" only makes sense while a folder view is active; the
    // entry stays visible but disabled when the video is not inside it.
    if (m_favoriteView == FavoriteView::Folder
        && m_favorites->contains(m_favoriteFolderId))
    {
        const QString currentFolderId = m_favoriteFolderId;
        QAction* removeCurrentAction = favoritesMenu->addAction(
            LT("移出当前收藏夹「%1」")
                .arg(m_favorites->folderName(currentFolderId)));
        removeCurrentAction->setEnabled(
            m_favorites->isMember(currentFolderId, mediaPath));
        connect(removeCurrentAction, &QAction::triggered, this,
                [this, currentFolderId, mediaPath]() {
                    QTimer::singleShot(0, this, [this, currentFolderId, mediaPath]() {
                        setMediaFavorite(currentFolderId, mediaPath, false);
                    });
                });
    }

    QAction* removeAllAction = favoritesMenu->addAction(LT("移出所有收藏夹"));
    removeAllAction->setEnabled(!memberOf.isEmpty());
    connect(removeAllAction, &QAction::triggered, this, [this, mediaPath]() {
        QTimer::singleShot(0, this, [this, mediaPath]() { removeFromAllFavorites(mediaPath); });
    });

    menu.addSeparator();
    menu.addAction(LT("重新生成缩略图"), this, [this, mediaPath]() {
        QTimer::singleShot(0, this, [this, mediaPath]() { regenerateThumbnail(mediaPath); });
    });
    menu.addAction(LT("在资源管理器中打开"), this, [this, mediaPath]() {
        QTimer::singleShot(0, this, [this, mediaPath]() { revealInExplorer(mediaPath); });
    });
    menu.addAction(LT("刷新媒体库"), this, &MediaLibrary::refreshAll);
    menu.exec(globalPos);
}

void MediaLibrary::setMediaFavorite(const QString& folderId, const QString& mediaPath,
                                    bool favorite)
{
    if (!m_favorites)
        return;

    QString error;
    if (!m_favorites->setMember(folderId, mediaPath, favorite, &error))
    {
        if (!error.isEmpty())
            emit message(error, 4000);
        return;
    }

    persistFavorites();

    const QString folderName = m_favorites->folderName(folderId);
    const QString fileName = QFileInfo(mediaPath).fileName();
    AppLogger::log(QStringLiteral("library"),
                   QStringLiteral("%1 %2 收藏夹 %3")
                       .arg(favorite ? QStringLiteral("加入") : QStringLiteral("移出"),
                            fileName, folderName));
    emit message(favorite ? LT("已加入收藏夹「%1」").arg(folderName)
                          : LT("已从收藏夹「%1」移除").arg(folderName),
                 3000);

    int row = -1;
    for (int i = 0; i < m_visible.size(); ++i)
    {
        if (m_visible.at(i).path.compare(mediaPath, Qt::CaseInsensitive) == 0)
        {
            row = i;
            break;
        }
    }
    rebuildModel(mediaPath, row);
    updateFavoritesButtonText();
}

void MediaLibrary::createFolderForMedia(const QString& mediaPath)
{
    if (!m_favorites)
        return;

    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, LT("新建收藏夹"), LT("收藏夹名称："),
        QLineEdit::Normal, QString(), &accepted);
    if (!accepted)
        return;

    QString error;
    const QString folderId = m_favorites->createFolder(name, &error);
    if (folderId.isEmpty())
    {
        QMessageBox::warning(this, LT("新建收藏夹"),
                             error.isEmpty() ? LT("无法创建收藏夹") : error);
        return;
    }

    if (!m_favorites->addMember(folderId, mediaPath, &error))
    {
        QMessageBox::warning(this, LT("新建收藏夹"),
                             error.isEmpty() ? LT("无法加入收藏夹") : error);
        return;
    }
    persistFavorites();

    const QString folderName = m_favorites->folderName(folderId);
    AppLogger::log(QStringLiteral("library"),
                   QStringLiteral("新建收藏夹 %1，并加入 %2")
                       .arg(folderName, QFileInfo(mediaPath).fileName()));
    emit message(LT("已新建收藏夹「%1」并加入该视频").arg(folderName), 3000);
    showFavoriteFolder(folderId);
}

void MediaLibrary::removeFromAllFavorites(const QString& mediaPath)
{
    if (!m_favorites)
        return;

    const QStringList folderIds = m_favorites->folderIdsFor(mediaPath);
    if (folderIds.isEmpty())
        return;

    int row = -1;
    for (int i = 0; i < m_visible.size(); ++i)
    {
        if (m_visible.at(i).path.compare(mediaPath, Qt::CaseInsensitive) == 0)
        {
            row = i;
            break;
        }
    }

    for (const QString& folderId : folderIds)
        m_favorites->removeMember(folderId, mediaPath);
    persistFavorites();

    AppLogger::log(QStringLiteral("library"),
                   QStringLiteral("从所有收藏夹移除 %1")
                       .arg(QFileInfo(mediaPath).fileName()));
    emit message(LT("已从所有收藏夹移除"), 3000);
    rebuildModel(mediaPath, row);
    updateFavoritesButtonText();
}

void MediaLibrary::persistFavorites()
{
    if (m_favorites && !m_favorites->save())
    {
        AppLogger::log(QStringLiteral("library"),
                       QStringLiteral("收藏夹保存失败: %1").arg(m_favorites->filePath()));
        emit message(LT("收藏夹未能保存"), 5000);
    }
}

void MediaLibrary::chooseScriptWithDialog(const QString& mediaPath)
{
    const QFileInfo info(mediaPath);
    const QString selected = QFileDialog::getOpenFileName(
        this, LT("选择脚本（可进入子文件夹）"), info.absolutePath(),
        LT("funscript 脚本 (*.funscript);;所有文件 (*.*)"));
    if (selected.isEmpty())
        return;

    chooseScript(mediaPath, selected);
}

void MediaLibrary::chooseScript(const QString& mediaPath, const QString& scriptPath)
{
    const int index = indexOfItem(mediaPath);
    if (index < 0)
        return;

    MediaItem& item = m_items[index];
    item.preferredScript = scriptPath;

    const ScriptBundle preview = ScriptLoader::load(mediaPath, scriptPath);
    item.scriptAxes = preview.tracks.size();
    refreshItem(item);
    scheduleIndexSave();

    AppLogger::log(QStringLiteral("library"),
                   QStringLiteral("为 %1 选择脚本: %2（%3 轴）")
                       .arg(QFileInfo(mediaPath).fileName(), QFileInfo(scriptPath).fileName())
                       .arg(preview.tracks.size()));

    if (preview.tracks.isEmpty())
        emit message(LT("该脚本没有可用动作，已按视频播放"), 5000);
    emit mediaActivated(mediaPath, scriptPath);
}

void MediaLibrary::showMediaInfo(const QString& mediaPath)
{
    const QString info = mediaInfoFor(mediaPath);
    if (info.isEmpty())
        return;
    QMessageBox::information(this, LT("媒体信息"), info);
}

void MediaLibrary::regenerateThumbnail(const QString& mediaPath)
{
    const int index = indexOfItem(mediaPath);
    if (index < 0 || !m_probe)
        return;

    MediaItem& item = m_items[index];
    if (!item.thumbnailPath.isEmpty())
        QFile::remove(item.thumbnailPath);
    item.thumbnailState = ThumbnailState::Pending;
    if (m_grid)
    {
        m_grid->invalidatePixmap(item.thumbnailPath);
        m_grid->updateItem(item);
    }

    // A random position gives a visibly different frame each time.
    const double ratio = QRandomGenerator::global()->bounded(5, 91) / 100.0;
    m_probe->enqueue(item.path, item.thumbnailPath, ratio, true, true);
    updateCount();
    emit message(LT("正在重新生成缩略图…"), 3000);
}

void MediaLibrary::revealInExplorer(const QString& mediaPath)
{
    const QString native = QDir::toNativeSeparators(mediaPath);
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            {QStringLiteral("/select,") + native});
}
