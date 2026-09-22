#pragma once

#include "core/LoopClip.h"

#include <QCache>
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QStringList>
#include <QWidget>

class LoopGridWidget;
class LoopStore;
class MediaProbeQueue;
class QLabel;
class QLineEdit;
class QMenu;
class QScrollArea;
class QToolButton;

// Right hand panel that mirrors the media library: same search row plus a cover
// grid, but its data is a list of loop clips with their own folders and tags.
class LoopListPanel : public QWidget
{
    Q_OBJECT

public:
    enum class FolderView
    {
        All = 0,
        Unassigned = 1,
        Folder = 2
    };

    enum class TagView
    {
        All = 0,
        Untagged = 1,
        Tag = 2
    };

    enum class SortMode
    {
        RecentlyAdded = 0,
        MediaName = 1,
        Duration = 2
    };

    explicit LoopListPanel(QWidget* parent = nullptr);
    ~LoopListPanel() override;

    void setStore(LoopStore* store);
    LoopStore* store() const { return m_store; }

    // Root of the media library cache; clip frames live in a "loops" subfolder.
    void setCacheRoot(const QString& root);
    // Normalised paths of the videos that currently exist.
    void setAvailablePaths(const QSet<QString>& paths);
    // Normalised media path -> existing cover thumbnail, used as fallback while
    // the A point frame is still being generated.
    void setMediaThumbnails(const QHash<QString, QString>& thumbnails);
    // Normalised media path -> script axis count, mirroring the library badge.
    void setScriptAxes(const QHash<QString, int>& axes);
    // Whether the 单轴 / 多轴 badge is drawn in front of the title.
    void setAxisTagVisible(bool visible);
    bool axisTagVisible() const { return m_axisTagVisible; }

    void refresh();
    void setActiveClipId(const QString& clipId);
    QString selectedClipId() const;
    void ensureVisibleClip(const QString& clipId);

    FolderView folderView() const { return m_folderView; }
    QString folderId() const { return m_folderId; }
    TagView tagView() const { return m_tagView; }
    QString tag() const { return m_tag; }
    SortMode sortMode() const { return m_sortMode; }
    bool sortDescending() const { return m_sortDescending; }

    void showAllClips();
    void showUnassignedClips();
    void showFolder(const QString& folderId);
    void showAllTags();
    void showUntagged();
    void showTag(const QString& tag);
    void setSortMode(SortMode mode, bool descending);
    // Card width and font follow the media library, so both panels always show
    // the same metrics. The value passed here is the library's thumbnail size.
    void setCardWidth(int pixels);
    void setFontPercent(int percent);
    int cardWidth() const { return m_cardWidth; }

    int clipCount() const;
    int visibleCount() const { return m_visibleCount; }
    // Next clip of the currently visible (filtered + sorted) list; empty at the
    // end. Used by the sequential playback mode.
    QString nextClipId(const QString& currentClipId) const;
    int visibleClipCount() const { return m_visibleOrder.size(); }

    bool autoNext() const { return m_autoNext; }
    void setAutoNext(bool enabled);

    // The panel has to stay draggable down to the fold threshold, so the search
    // row is allowed to clip instead of pinning the splitter open.
    QSize minimumSizeHint() const override;

signals:
    void clipActivated(const QString& clipId);
    void message(const QString& text, int timeoutMs);
    void manageRequested();
    void autoNextChanged(bool enabled);
    // The store changed (clip edited/removed/folders toggled); the window
    // refreshes its menus and the media library favourites state.
    void storeChanged();

private:
    void resizeEvent(QResizeEvent* event) override;
    void buildUi();
    void loadSettings();
    void saveSettings() const;
    void rebuild(const QString& keepSelection = QString());
    void startThumbnailJobs(const QList<LoopClip>& clips);
    void updateCount();
    void updateSortButtonText();
    void updateFolderButtonText();
    void updateTagButtonText();
    void rebuildFolderMenu();
    void rebuildTagMenu();
    void showContextMenu(const QString& clipId, const QPoint& globalPos);
    void editClip(const QString& clipId);
    void removeClip(const QString& clipId);
    void copyClipInfo(const QString& clipId);
    void revealClipMedia(const QString& clipId);
    void toggleClipFolder(const QString& clipId, const QString& folderId, bool member);
    void createFolderForClip(const QString& clipId);
    QString thumbnailPathFor(const LoopClip& clip) const;
    QString fallbackThumbnailFor(const QString& mediaPath) const;
    bool isMissing(const QString& mediaPath) const;
    QString hoverTextFor(const QString& clipId) const;
    void persistStore();

    LoopStore* m_store = nullptr;
    LoopGridWidget* m_grid = nullptr;
    QScrollArea* m_scroll = nullptr;
    MediaProbeQueue* m_probe = nullptr;
    QCache<QString, QPixmap>* m_pixmapCache = nullptr;

    QLineEdit* m_search = nullptr;
    QLabel* m_count = nullptr;
    QToolButton* m_folderButton = nullptr;
    QToolButton* m_tagButton = nullptr;
    QToolButton* m_sortButton = nullptr;
    QToolButton* m_autoNextButton = nullptr;
    QMenu* m_folderMenu = nullptr;
    QMenu* m_tagMenu = nullptr;
    QMenu* m_sortMenu = nullptr;

    QString m_cacheRoot;
    QString m_thumbDir;
    QSet<QString> m_availablePaths;
    QHash<QString, QString> m_mediaThumbnails;
    QHash<QString, int> m_scriptAxes;
    QHash<QString, QSet<QString>> m_pendingThumbs;
    // Frames that failed once; retrying them on every library refresh would
    // keep a broken video spinning in the probe queue forever.
    QSet<QString> m_failedThumbs;

    FolderView m_folderView = FolderView::All;
    QString m_folderId;
    TagView m_tagView = TagView::All;
    QString m_tag;
    SortMode m_sortMode = SortMode::RecentlyAdded;
    bool m_sortDescending = true;
    QString m_searchText;
    QStringList m_visibleOrder;
    bool m_autoNext = false;
    bool m_axisTagVisible = true;
    int m_cardWidth = 200;
    int m_fontPercent = 100;
    int m_visibleCount = 0;
};
