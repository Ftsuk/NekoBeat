#include "core/Loc.h"
#include "ui/LoopListPanel.h"

#include "core/LoopStore.h"
#include "core/MediaLibraryStore.h"
#include "core/PathUtils.h"
#include "media/MediaProbeQueue.h"
#include "ui/LoopClipDialog.h"
#include "ui/LoopGridWidget.h"

#include <QApplication>
#include <QActionGroup>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QStyle>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
QString shortFolderName(const QString& name)
{
    return name.size() > 7 ? name.left(6) + QStringLiteral("…") : name;
}

QString shortTagName(const QString& name)
{
    return name.size() > 8 ? name.left(7) + QStringLiteral("…") : name;
}
}

LoopListPanel::LoopListPanel(QWidget* parent)
    : QWidget(parent)
{
    m_pixmapCache = new QCache<QString, QPixmap>(192);
    m_probe = new MediaProbeQueue(this);
    m_probe->setConcurrency(1);
    buildUi();
    loadSettings();

    connect(m_probe, &MediaProbeQueue::probed, this,
            [this](const QString& mediaPath, const MediaProbeResult& result) {
                const QSet<QString> pending = m_pendingThumbs.take(mediaPath);
                if (pending.isEmpty())
                    return;
                for (const QString& path : pending)
                {
                    m_grid->invalidatePixmap(path);
                    if (!result.thumbnailOk)
                        m_failedThumbs.insert(path);
                }
                m_grid->update();
            });
}

LoopListPanel::~LoopListPanel()
{
    if (m_probe)
    {
        m_probe->setParent(nullptr);
        delete m_probe;
        m_probe = nullptr;
    }
    delete m_pixmapCache;
    m_pixmapCache = nullptr;
}

void LoopListPanel::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    auto* searchRow = new QHBoxLayout();
    searchRow->setContentsMargins(0, 0, 0, 0);
    searchRow->setSpacing(6);

    m_folderButton = new QToolButton(this);
    m_folderButton->setObjectName(QStringLiteral("Compact"));
    m_folderButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_folderButton->setPopupMode(QToolButton::InstantPopup);
    m_folderButton->setCursor(Qt::PointingHandCursor);
    m_folderButton->setMaximumWidth(130);
    m_folderButton->setToolTip(LT("按循环收藏夹筛选"));
    searchRow->addWidget(m_folderButton);

    m_tagButton = new QToolButton(this);
    m_tagButton->setObjectName(QStringLiteral("Compact"));
    m_tagButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_tagButton->setPopupMode(QToolButton::InstantPopup);
    m_tagButton->setCursor(Qt::PointingHandCursor);
    m_tagButton->setMaximumWidth(130);
    m_tagButton->setToolTip(LT("按标签筛选"));
    searchRow->addWidget(m_tagButton);

    m_sortButton = new QToolButton(this);
    m_sortButton->setObjectName(QStringLiteral("Compact"));
    m_sortButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_sortButton->setPopupMode(QToolButton::InstantPopup);
    m_sortButton->setCursor(Qt::PointingHandCursor);
    m_sortButton->setToolTip(LT("排序方式"));
    searchRow->addWidget(m_sortButton);

    m_folderMenu = new QMenu(this);
    connect(m_folderMenu, &QMenu::aboutToShow, this, &LoopListPanel::rebuildFolderMenu);
    m_folderButton->setMenu(m_folderMenu);

    m_tagMenu = new QMenu(this);
    connect(m_tagMenu, &QMenu::aboutToShow, this, &LoopListPanel::rebuildTagMenu);
    m_tagButton->setMenu(m_tagMenu);

    m_sortMenu = new QMenu(this);
    {
        const struct
        {
            QString label;
            SortMode mode;
            bool descending;
        } entries[] = {
            {LT("添加时间 新→旧"), SortMode::RecentlyAdded, true},
            {LT("添加时间 旧→新"), SortMode::RecentlyAdded, false},
            {LT("视频名称 A→Z"), SortMode::MediaName, false},
            {LT("视频名称 Z→A"), SortMode::MediaName, true},
            {LT("片段时长 短→长"), SortMode::Duration, false},
            {LT("片段时长 长→短"), SortMode::Duration, true}
        };
        auto* group = new QActionGroup(this);
        for (const auto& entry : entries)
        {
            QAction* action = m_sortMenu->addAction(entry.label);
            action->setCheckable(true);
            action->setData(static_cast<int>(entry.mode) * 2 + (entry.descending ? 1 : 0));
            group->addAction(action);
            const SortMode mode = entry.mode;
            const bool descending = entry.descending;
            connect(action, &QAction::triggered, this,
                    [this, mode, descending]() { setSortMode(mode, descending); });
        }
        connect(m_sortMenu, &QMenu::aboutToShow, this, [this]() {
            const int value = static_cast<int>(m_sortMode) * 2
                              + (m_sortDescending ? 1 : 0);
            for (QAction* action : m_sortMenu->actions())
                action->setChecked(action->data().toInt() == value);
        });
    }
    m_sortButton->setMenu(m_sortMenu);
    updateSortButtonText();

    // Sequential playback: walk the visible list one clip after another.
    m_autoNextButton = new QToolButton(this);
    m_autoNextButton->setObjectName(QStringLiteral("MediaButton"));
    m_autoNextButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_autoNextButton->setCheckable(true);
    m_autoNextButton->setCursor(Qt::PointingHandCursor);
    m_autoNextButton->setText(LT("连播"));
    m_autoNextButton->setToolTip(
        LT("连续播放：一段播完自动接着播列表里的下一段"));
    searchRow->addWidget(m_autoNextButton);
    connect(m_autoNextButton, &QToolButton::toggled, this, [this](bool enabled) {
        setAutoNext(enabled);
        emit autoNextChanged(enabled);
    });

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(LT("搜索片段、标签、备注…"));
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(40);
    searchRow->addWidget(m_search, 1);

    m_count = new QLabel(LT("空"), this);
    m_count->setObjectName(QStringLiteral("Muted"));
    searchRow->addWidget(m_count);
    layout->addLayout(searchRow);

    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("MediaGridScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_grid = new LoopGridWidget(m_scroll);
    m_grid->setPixmapCache(m_pixmapCache);
    m_grid->setCardWidth(m_cardWidth);
    m_scroll->setWidget(m_grid);
    layout->addWidget(m_scroll, 1);

    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_searchText = text.trimmed();
        rebuild();
    });
    connect(m_grid, &LoopGridWidget::clipActivated, this, &LoopListPanel::clipActivated);
    connect(m_grid, &LoopGridWidget::contextMenuRequested, this,
            [this](const QString& clipId, const QPoint& globalPos) {
                showContextMenu(clipId, globalPos);
            });
    connect(m_grid, &LoopGridWidget::hoveredClipChanged, this,
            [this](const QString& clipId, const QPoint& globalPos) {
                if (clipId.isEmpty())
                {
                    QToolTip::hideText();
                    return;
                }
                const QString info = hoverTextFor(clipId);
                if (!info.isEmpty())
                    QToolTip::showText(globalPos, info, m_grid);
            });

    updateFolderButtonText();
    updateTagButtonText();
}

void LoopListPanel::setStore(LoopStore* store)
{
    m_store = store;
    rebuild();
}

void LoopListPanel::setCacheRoot(const QString& root)
{
    m_cacheRoot = root;
    m_thumbDir = QDir(root).absoluteFilePath(QStringLiteral("loops"));
    QDir().mkpath(m_thumbDir);
    if (m_probe)
        m_probe->setTemporaryRoot(QDir(root).absoluteFilePath(QStringLiteral("tmp")));
    rebuild();
}

void LoopListPanel::setAvailablePaths(const QSet<QString>& paths)
{
    m_availablePaths = paths;
    rebuild(m_grid ? m_grid->selectedClipId() : QString());
}

void LoopListPanel::setMediaThumbnails(const QHash<QString, QString>& thumbnails)
{
    m_mediaThumbnails = thumbnails;
    rebuild(m_grid ? m_grid->selectedClipId() : QString());
}

void LoopListPanel::setScriptAxes(const QHash<QString, int>& axes)
{
    m_scriptAxes = axes;
    rebuild(m_grid ? m_grid->selectedClipId() : QString());
}

void LoopListPanel::setAxisTagVisible(bool visible)
{
    if (m_axisTagVisible == visible)
        return;
    m_axisTagVisible = visible;
    if (m_grid)
        m_grid->setShowAxisTag(visible);
}

void LoopListPanel::refresh()
{
    rebuild(m_grid ? m_grid->selectedClipId() : QString());
}

void LoopListPanel::setActiveClipId(const QString& clipId)
{
    if (m_grid)
        m_grid->setActiveClipId(clipId);
}

QString LoopListPanel::selectedClipId() const
{
    return m_grid ? m_grid->selectedClipId() : QString();
}

void LoopListPanel::ensureVisibleClip(const QString& clipId)
{
    if (m_grid)
        m_grid->ensureVisibleClip(clipId);
}

int LoopListPanel::clipCount() const
{
    return m_store ? m_store->clipCount() : 0;
}

void LoopListPanel::showAllClips()
{
    m_folderView = FolderView::All;
    m_folderId.clear();
    updateFolderButtonText();
    rebuild();
}

void LoopListPanel::showUnassignedClips()
{
    m_folderView = FolderView::Unassigned;
    m_folderId.clear();
    updateFolderButtonText();
    rebuild();
}

void LoopListPanel::showFolder(const QString& folderId)
{
    m_folderView = FolderView::Folder;
    m_folderId = folderId;
    updateFolderButtonText();
    rebuild();
}

void LoopListPanel::showAllTags()
{
    m_tagView = TagView::All;
    m_tag.clear();
    updateTagButtonText();
    rebuild();
}

void LoopListPanel::showUntagged()
{
    m_tagView = TagView::Untagged;
    m_tag.clear();
    updateTagButtonText();
    rebuild();
}

void LoopListPanel::showTag(const QString& tag)
{
    m_tagView = TagView::Tag;
    m_tag = tag;
    updateTagButtonText();
    rebuild();
}

void LoopListPanel::setSortMode(SortMode mode, bool descending)
{
    m_sortMode = mode;
    m_sortDescending = descending;
    updateSortButtonText();
    saveSettings();
    rebuild(m_grid ? m_grid->selectedClipId() : QString());
}

void LoopListPanel::setCardWidth(int pixels)
{
    // Same metric as the media library: the grid card is slightly wider than
    // the picture so the frame keeps a little breathing room.
    m_cardWidth = qBound(80, pixels, 600) + 6;
    if (m_grid)
        m_grid->setCardWidth(m_cardWidth);
    if (m_count)
        m_count->setVisible(width() >= 400);
}

void LoopListPanel::setFontPercent(int percent)
{
    m_fontPercent = qBound(70, percent, 200);
    if (m_grid)
        m_grid->setFontSize(qBound(10, qRound(12 * m_fontPercent / 100.0), 22));
}

void LoopListPanel::setAutoNext(bool enabled)
{
    m_autoNext = enabled;
    if (m_autoNextButton)
    {
        m_autoNextButton->blockSignals(true);
        m_autoNextButton->setChecked(enabled);
        m_autoNextButton->blockSignals(false);
        m_autoNextButton->setObjectName(enabled ? QStringLiteral("MediaButtonActive")
                                                : QStringLiteral("MediaButton"));
        m_autoNextButton->style()->unpolish(m_autoNextButton);
        m_autoNextButton->style()->polish(m_autoNextButton);
    }
    QSettings settings;
    settings.setValue(QStringLiteral("loop/autoNext"), enabled);
}

QString LoopListPanel::nextClipId(const QString& currentClipId) const
{
    if (m_visibleOrder.isEmpty())
        return QString();

    const int index = m_visibleOrder.indexOf(currentClipId);
    if (index < 0)
        return m_visibleOrder.first();

    // Sequential playback walks the list as a loop: after the last clip it
    // continues with the first one instead of falling through to the rest of
    // the video. A one-clip list therefore keeps repeating that clip.
    return m_visibleOrder.at((index + 1) % m_visibleOrder.size());
}

QSize LoopListPanel::minimumSizeHint() const
{
    // Zero width on purpose: the panel has to be shrinkable all the way to the
    // window edge without a hard stop.
    return QSize(0, 120);
}

void LoopListPanel::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    // Drop the counter first when the panel gets tight so the row stays
    // readable on the way down to the collapse threshold.
    if (m_count)
        m_count->setVisible(width() >= 400);
    updateSortButtonText();
}

void LoopListPanel::loadSettings()
{
    QSettings settings;
    m_sortMode = static_cast<SortMode>(
        settings.value(QStringLiteral("loop/sortMode"), 0).toInt());
    m_sortDescending = settings.value(QStringLiteral("loop/sortDescending"), true).toBool();
    m_autoNext = settings.value(QStringLiteral("loop/autoNext"), false).toBool();
    updateSortButtonText();
    setAutoNext(m_autoNext);
}

void LoopListPanel::saveSettings() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("loop/sortMode"), static_cast<int>(m_sortMode));
    settings.setValue(QStringLiteral("loop/sortDescending"), m_sortDescending);
}

void LoopListPanel::updateSortButtonText()
{
    if (!m_sortButton)
        return;
    const bool compact = width() < 300;
    QString text;
    switch (m_sortMode)
    {
    case SortMode::MediaName:
        text = compact ? (m_sortDescending ? LT("名↓") : LT("名↑"))
                       : (m_sortDescending ? LT("名称 Z→A")
                                           : LT("名称 A→Z"));
        break;
    case SortMode::Duration:
        text = compact ? (m_sortDescending ? LT("长↓") : LT("长↑"))
                       : (m_sortDescending ? LT("时长 长→短")
                                           : LT("时长 短→长"));
        break;
    case SortMode::RecentlyAdded:
    default:
        text = compact ? (m_sortDescending ? LT("新↓") : LT("新↑"))
                       : (m_sortDescending ? LT("添加 新→旧")
                                           : LT("添加 旧→新"));
        break;
    }
    m_sortButton->setText(text);
}

void LoopListPanel::updateFolderButtonText()
{
    if (!m_folderButton)
        return;
    switch (m_folderView)
    {
    case FolderView::Unassigned:
        m_folderButton->setText(LT("★ 未归类"));
        m_folderButton->setToolTip(LT("只显示不属于任何循环收藏夹的片段"));
        break;
    case FolderView::Folder:
        m_folderButton->setText(
            QStringLiteral("★ %1").arg(shortFolderName(
                m_store ? m_store->folderName(m_folderId) : QString())));
        m_folderButton->setToolTip(
            LT("循环收藏夹「%1」（%2 个片段）")
                .arg(m_store ? m_store->folderName(m_folderId) : QString())
                .arg(m_store ? m_store->folderClipCount(m_folderId) : 0));
        break;
    case FolderView::All:
    default:
        m_folderButton->setText(LT("★ 全部"));
        m_folderButton->setToolTip(LT("显示全部循环片段"));
        break;
    }
}

void LoopListPanel::updateTagButtonText()
{
    if (!m_tagButton)
        return;
    switch (m_tagView)
    {
    case TagView::Untagged:
        m_tagButton->setText(LT("# 无标签"));
        m_tagButton->setToolTip(LT("只显示没有标签的片段"));
        break;
    case TagView::Tag:
        m_tagButton->setText(QStringLiteral("# %1").arg(shortTagName(m_tag)));
        m_tagButton->setToolTip(LT("标签「%1」").arg(m_tag));
        break;
    case TagView::All:
    default:
        m_tagButton->setText(LT("# 全部"));
        m_tagButton->setToolTip(LT("显示全部标签"));
        break;
    }
}

void LoopListPanel::rebuildFolderMenu()
{
    if (!m_folderMenu)
        return;
    m_folderMenu->clear();
    if (!m_store)
        return;

    QAction* allAction = m_folderMenu->addAction(LT("全部片段"));
    allAction->setCheckable(true);
    allAction->setChecked(m_folderView == FolderView::All);
    connect(allAction, &QAction::triggered, this, &LoopListPanel::showAllClips);

    QAction* noneAction = m_folderMenu->addAction(LT("未归类"));
    noneAction->setCheckable(true);
    noneAction->setChecked(m_folderView == FolderView::Unassigned);
    connect(noneAction, &QAction::triggered, this, &LoopListPanel::showUnassignedClips);

    m_folderMenu->addSeparator();
    if (m_store->folders().isEmpty())
    {
        QAction* emptyAction = m_folderMenu->addAction(LT("（还没有收藏夹）"));
        emptyAction->setEnabled(false);
    }
    for (const LoopFolder& folder : m_store->folders())
    {
        QAction* action = m_folderMenu->addAction(
            QStringLiteral("%1 (%2)").arg(folder.name).arg(m_store->folderClipCount(folder.id)));
        action->setCheckable(true);
        action->setChecked(m_folderView == FolderView::Folder && m_folderId == folder.id);
        const QString folderId = folder.id;
        connect(action, &QAction::triggered, this,
                [this, folderId]() { showFolder(folderId); });
    }

    m_folderMenu->addSeparator();
    QAction* manageAction = m_folderMenu->addAction(LT("管理循环收藏夹…"));
    connect(manageAction, &QAction::triggered, this, &LoopListPanel::manageRequested);
}

void LoopListPanel::rebuildTagMenu()
{
    if (!m_tagMenu)
        return;
    m_tagMenu->clear();
    if (!m_store)
        return;

    QAction* allAction = m_tagMenu->addAction(LT("全部标签"));
    allAction->setCheckable(true);
    allAction->setChecked(m_tagView == TagView::All);
    connect(allAction, &QAction::triggered, this, &LoopListPanel::showAllTags);

    QAction* noneAction = m_tagMenu->addAction(LT("无标签"));
    noneAction->setCheckable(true);
    noneAction->setChecked(m_tagView == TagView::Untagged);
    connect(noneAction, &QAction::triggered, this, &LoopListPanel::showUntagged);

    m_tagMenu->addSeparator();
    const QStringList tags = m_store->tagsByUsage();
    if (tags.isEmpty())
    {
        QAction* emptyAction = m_tagMenu->addAction(LT("（还没有标签）"));
        emptyAction->setEnabled(false);
    }
    for (const QString& tag : tags)
    {
        QAction* action =
            m_tagMenu->addAction(QStringLiteral("%1 (%2)").arg(tag).arg(m_store->tagUsage(tag)));
        action->setCheckable(true);
        action->setChecked(m_tagView == TagView::Tag
                           && m_tag.compare(tag, Qt::CaseInsensitive) == 0);
        connect(action, &QAction::triggered, this, [this, tag]() { showTag(tag); });
    }

    m_tagMenu->addSeparator();
    QAction* manageAction = m_tagMenu->addAction(LT("管理标签…"));
    connect(manageAction, &QAction::triggered, this, &LoopListPanel::manageRequested);
}

void LoopListPanel::rebuild(const QString& keepSelection)
{
    if (!m_grid)
        return;

    QString selection = keepSelection.isEmpty() ? m_grid->selectedClipId() : keepSelection;

    QList<LoopClip> clips;
    if (m_store)
        clips = m_store->clips();

    const auto matchesFolder = [this](const LoopClip& clip) {
        switch (m_folderView)
        {
        case FolderView::Unassigned:
            return clip.folderIds.isEmpty();
        case FolderView::Folder:
            return clip.folderIds.contains(m_folderId);
        case FolderView::All:
        default:
            return true;
        }
    };
    const auto matchesTag = [this](const LoopClip& clip) {
        switch (m_tagView)
        {
        case TagView::Untagged:
            return clip.tags.isEmpty();
        case TagView::Tag:
            return std::any_of(clip.tags.cbegin(), clip.tags.cend(),
                               [this](const QString& tag) {
                                   return tag.compare(m_tag, Qt::CaseInsensitive) == 0;
                               });
        case TagView::All:
        default:
            return true;
        }
    };
    const auto matchesSearch = [this](const LoopClip& clip) {
        if (m_searchText.isEmpty())
            return true;
        const QString needle = m_searchText;
        if (clip.title.contains(needle, Qt::CaseInsensitive))
            return true;
        if (QFileInfo(clip.mediaPath).fileName().contains(needle, Qt::CaseInsensitive))
            return true;
        if (clip.note.contains(needle, Qt::CaseInsensitive))
            return true;
        return std::any_of(clip.tags.cbegin(), clip.tags.cend(),
                           [&needle](const QString& tag) {
                               return tag.contains(needle, Qt::CaseInsensitive);
                           });
    };

    QList<LoopClip> filtered;
    filtered.reserve(clips.size());
    for (const LoopClip& clip : clips)
    {
        if (matchesFolder(clip) && matchesTag(clip) && matchesSearch(clip))
            filtered.append(clip);
    }

    std::stable_sort(filtered.begin(), filtered.end(),
                     [this](const LoopClip& left, const LoopClip& right) {
        int result = 0;
        switch (m_sortMode)
        {
        case SortMode::MediaName:
            result = MediaLibraryStore::naturalCompare(QFileInfo(left.mediaPath).fileName(),
                                                       QFileInfo(right.mediaPath).fileName());
            break;
        case SortMode::Duration:
            result = left.durationMs() == right.durationMs()
                         ? 0
                         : (left.durationMs() < right.durationMs() ? -1 : 1);
            break;
        case SortMode::RecentlyAdded:
        default:
            if (left.created != right.created)
                result = left.created < right.created ? -1 : 1;
            break;
        }
        if (result == 0)
            result = MediaLibraryStore::naturalCompare(left.title, right.title);
        return m_sortDescending ? result > 0 : result < 0;
    });

    QList<LoopGridItem> items;
    items.reserve(filtered.size());
    for (const LoopClip& clip : filtered)
    {
        LoopGridItem item;
        item.clip = clip;
        item.thumbnailPath = thumbnailPathFor(clip);
        item.fallbackThumbnailPath = fallbackThumbnailFor(clip.mediaPath);
        item.missing = isMissing(clip.mediaPath);
        item.scriptAxes =
            m_scriptAxes.value(PathUtils::normalizeMediaPath(clip.mediaPath), 0);
        for (const QString& folderId : clip.folderIds)
        {
            const QString name = m_store ? m_store->folderName(folderId) : QString();
            if (!name.isEmpty())
                item.folderNames.append(name);
        }
        items.append(item);
    }

    m_visibleCount = items.size();
    m_visibleOrder.clear();
    m_visibleOrder.reserve(filtered.size());
    for (const LoopClip& clip : filtered)
        m_visibleOrder.append(clip.id);
    m_grid->setItems(items);
    // Only restore a selection that survived the current filter.
    if (!selection.isEmpty() && m_grid->itemFor(selection) != nullptr)
        m_grid->setSelectedClipId(selection);
    updateCount();
    startThumbnailJobs(filtered);
}

void LoopListPanel::startThumbnailJobs(const QList<LoopClip>& clips)
{
    if (!m_probe || m_thumbDir.isEmpty())
        return;

    for (const LoopClip& clip : clips)
    {
        if (isMissing(clip.mediaPath))
            continue;
        const QString path = thumbnailPathFor(clip);
        if (path.isEmpty() || QFileInfo::exists(path) || m_failedThumbs.contains(path))
            continue;
        m_pendingThumbs[clip.mediaPath].insert(path);
        m_probe->enqueueAt(clip.mediaPath, clip.startMs, path);
    }
}

void LoopListPanel::updateCount()
{
    if (!m_count)
        return;
    const int total = clipCount();
    if (total == 0)
        m_count->setText(LT("空"));
    else if (m_visibleCount == total)
        m_count->setText(QString::number(total));
    else
        m_count->setText(QStringLiteral("%1/%2").arg(m_visibleCount).arg(total));
}

QString LoopListPanel::thumbnailPathFor(const LoopClip& clip) const
{
    if (m_thumbDir.isEmpty())
        return QString();
    const QString key = clip.thumbKey.isEmpty()
                            ? LoopStore::thumbnailKeyFor(clip.mediaPath, clip.startMs)
                            : clip.thumbKey;
    if (key.isEmpty())
        return QString();
    const QString name =
        QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(),
                                                     QCryptographicHash::Sha1)
                                .toHex())
        + QStringLiteral(".jpg");
    return QDir(m_thumbDir).absoluteFilePath(name);
}

QString LoopListPanel::fallbackThumbnailFor(const QString& mediaPath) const
{
    const QString key = PathUtils::normalizeMediaPath(mediaPath);
    if (key.isEmpty())
        return QString();
    return m_mediaThumbnails.value(key);
}

bool LoopListPanel::isMissing(const QString& mediaPath) const
{
    // While the media library has not scanned anything yet every path would
    // look missing, so the badge stays hidden in that case.
    if (m_availablePaths.isEmpty())
        return false;
    const QString key = PathUtils::normalizeMediaPath(mediaPath);
    return key.isEmpty() || !m_availablePaths.contains(key);
}

QString LoopListPanel::hoverTextFor(const QString& clipId) const
{
    if (!m_store)
        return QString();
    const LoopClip clip = m_store->clip(clipId);
    if (clip.id.isEmpty())
        return QString();

    QStringList lines;
    lines.append(QStringLiteral("<b>%1</b>").arg(clip.title.toHtmlEscaped()));
    lines.append(QFileInfo(clip.mediaPath).fileName().toHtmlEscaped());
    lines.append(QStringLiteral("%1 → %2（%3）")
                     .arg(LoopClipRules::formatTimecode(clip.startMs),
                          LoopClipRules::formatTimecode(clip.endMs),
                          MediaLibraryStore::formatDuration(clip.durationMs())));
    if (!clip.tags.isEmpty())
        lines.append(LT("标签：%1").arg(clip.tags.join(QStringLiteral("、")).toHtmlEscaped()));
    if (!clip.folderIds.isEmpty())
    {
        QStringList names;
        for (const QString& folderId : clip.folderIds)
            names.append(m_store->folderName(folderId));
        names.removeAll(QString());
        if (!names.isEmpty())
            lines.append(LT("收藏夹：%1").arg(names.join(QStringLiteral("、")).toHtmlEscaped()));
    }
    if (!clip.note.trimmed().isEmpty())
        lines.append(LT("备注：%1").arg(clip.note.trimmed().toHtmlEscaped()));
    if (isMissing(clip.mediaPath))
        lines.append(LT("<span style='color:#FF6961;'>视频文件已失效</span>"));
    return lines.join(QStringLiteral("<br>"));
}

void LoopListPanel::showContextMenu(const QString& clipId, const QPoint& globalPos)
{
    if (clipId.isEmpty() || !m_store)
        return;
    const LoopClip clip = m_store->clip(clipId);
    if (clip.id.isEmpty())
        return;

    QMenu menu(this);
    QAction* playAction = menu.addAction(LT("循环播放此片段"));
    QAction* editAction = menu.addAction(LT("编辑信息…"));
    menu.addSeparator();

    QMenu* folderMenu = menu.addMenu(LT("收藏夹操作"));
    for (const LoopFolder& folder : m_store->folders())
    {
        QAction* action = folderMenu->addAction(folder.name);
        action->setCheckable(true);
        action->setChecked(clip.folderIds.contains(folder.id));
        const QString folderId = folder.id;
        connect(action, &QAction::triggered, this, [this, clipId, folderId](bool checked) {
            toggleClipFolder(clipId, folderId, checked);
        });
    }
    if (!m_store->folders().isEmpty())
        folderMenu->addSeparator();
    QAction* newFolderAction = folderMenu->addAction(LT("新建收藏夹…"));
    connect(newFolderAction, &QAction::triggered, this,
            [this, clipId]() { createFolderForClip(clipId); });

    menu.addSeparator();
    QAction* copyAction = menu.addAction(LT("复制片段信息"));
    QAction* revealAction = menu.addAction(LT("在资源管理器中显示视频"));
    const bool missing = isMissing(clip.mediaPath);
    revealAction->setEnabled(!missing);
    menu.addSeparator();
    QAction* removeAction = menu.addAction(LT("删除片段…"));

    QAction* chosen = menu.exec(globalPos);
    if (!chosen)
        return;
    if (chosen == playAction)
        emit clipActivated(clipId);
    else if (chosen == editAction)
        editClip(clipId);
    else if (chosen == copyAction)
        copyClipInfo(clipId);
    else if (chosen == revealAction)
        revealClipMedia(clipId);
    else if (chosen == removeAction)
        removeClip(clipId);
}

void LoopListPanel::editClip(const QString& clipId)
{
    if (!m_store)
        return;
    const LoopClip original = m_store->clip(clipId);
    if (original.id.isEmpty())
        return;

    LoopClipDialog dialog(this, m_store, original, 0);
    if (dialog.exec() != QDialog::Accepted)
        return;

    QString error;
    if (!m_store->updateClip(dialog.clip(), &error))
    {
        QMessageBox::warning(this, LT("保存失败"), error);
        return;
    }
    persistStore();
    // The A point may have moved, so the cached frame has to be re-grabbed and
    // the file for the old position is no longer referenced.
    if (dialog.clip().startMs != original.startMs)
    {
        const QString stale = thumbnailPathFor(original);
        m_grid->invalidatePixmap(stale);
        if (!stale.isEmpty())
            QFile::remove(stale);
    }
    refresh();
    emit message(LT("循环片段已更新"), 2500);
}

void LoopListPanel::removeClip(const QString& clipId)
{
    if (!m_store)
        return;
    const LoopClip clip = m_store->clip(clipId);
    if (clip.id.isEmpty())
        return;

    const auto answer = QMessageBox::question(
        this, LT("删除循环片段"),
        LT("删除片段「%1」？\n只删除这条收藏，不会影响视频文件。").arg(clip.title));
    if (answer != QMessageBox::Yes)
        return;

    QString error;
    if (!m_store->removeClip(clipId, &error))
    {
        QMessageBox::warning(this, LT("删除失败"), error);
        return;
    }
    m_store->pruneUnusedTags();
    persistStore();
    const QString thumb = thumbnailPathFor(clip);
    m_grid->invalidatePixmap(thumb);
    if (!thumb.isEmpty())
        QFile::remove(thumb);
    refresh();
    emit message(LT("已删除循环片段"), 2500);
}

void LoopListPanel::copyClipInfo(const QString& clipId)
{
    if (!m_store)
        return;
    const LoopClip clip = m_store->clip(clipId);
    if (clip.id.isEmpty())
        return;

    QStringList lines;
    lines.append(clip.title);
    lines.append(clip.mediaPath);
    lines.append(QStringLiteral("%1 - %2")
                     .arg(LoopClipRules::formatTimecode(clip.startMs),
                          LoopClipRules::formatTimecode(clip.endMs)));
    if (!clip.tags.isEmpty())
        lines.append(LT("标签: %1").arg(clip.tags.join(QStringLiteral(", "))));
    if (!clip.note.trimmed().isEmpty())
        lines.append(LT("备注: %1").arg(clip.note.trimmed()));
    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    emit message(LT("片段信息已复制到剪贴板"), 2500);
}

void LoopListPanel::revealClipMedia(const QString& clipId)
{
    if (!m_store)
        return;
    const LoopClip clip = m_store->clip(clipId);
    if (clip.id.isEmpty())
        return;
    const QString native = QDir::toNativeSeparators(clip.mediaPath);
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            {QStringLiteral("/select,") + native});
}

void LoopListPanel::toggleClipFolder(const QString& clipId, const QString& folderId,
                                     bool member)
{
    if (!m_store)
        return;
    QString error;
    if (!m_store->setClipFolder(clipId, folderId, member, &error))
    {
        QMessageBox::warning(this, LT("操作失败"), error);
        return;
    }
    persistStore();
    refresh();
}

void LoopListPanel::createFolderForClip(const QString& clipId)
{
    if (!m_store)
        return;

    bool ok = false;
    const QString name = QInputDialog::getText(this, LT("新建循环收藏夹"),
                                               LT("名称"), QLineEdit::Normal,
                                               QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;

    QString error;
    const QString folderId = m_store->createFolder(name, &error);
    if (folderId.isEmpty())
    {
        QMessageBox::warning(this, LT("新建失败"), error);
        return;
    }
    m_store->setClipFolder(clipId, folderId, true, &error);
    persistStore();
    refresh();
}

void LoopListPanel::persistStore()
{
    if (m_store)
        m_store->save();
    emit storeChanged();
}
