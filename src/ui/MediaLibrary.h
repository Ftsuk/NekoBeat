#pragma once

#include "core/FavoritesStore.h"
#include "core/MediaItem.h"
#include "core/MediaLibraryStore.h"
#include "media/MediaProbe.h"

#include <QCache>
#include <QHash>
#include <QList>
#include <QPixmap>
#include <QStringList>
#include <QThreadPool>
#include <QWidget>

class MediaGridWidget;
class QScrollArea;
class MediaLibraryStore;
class MediaProbeQueue;
class QLabel;
class QLineEdit;
class QListView;
class QMenu;
class QTimer;
class QToolButton;

struct MediaPlaybackRequest
{
    QString mediaPath;
    QString scriptPath;
};

// Cover-wall media library. The panel itself only keeps the search box and the
// grid; sorting, sizes, folders and maintenance live in the menu bar so the
// covers get as much room as possible.
class MediaLibrary : public QWidget
{
    Q_OBJECT

public:
    // cacheRoot is a test seam: an empty value keeps the normal per-user cache.
    explicit MediaLibrary(QWidget* parent = nullptr, const QString& cacheRoot = QString());
    ~MediaLibrary() override;

    void addRootFolder(const QString& folder);
    QString selectedMediaPath() const;
    QString selectedScriptPath() const;

    void setRootFolders(const QStringList& folders);
    QStringList rootFolders() const { return m_roots; }
    void removeRootFolder(const QString& folder);
    void refreshAll();

    // Thumbnail width in pixels (like XTPlayer's size presets).
    void setThumbSize(int pixels);
    int thumbSize() const { return m_thumbSize; }
    void setScriptFilter(bool onlyWithScript);
    bool onlyWithScript() const { return m_onlyWithScript; }
    void setSortMode(MediaLibraryStore::SortMode mode);
    MediaLibraryStore::SortMode sortMode() const { return m_sortMode; }
    void setSortDescending(bool descending);
    bool sortDescending() const { return m_sortDescending; }
    // Font scale in percent, 100 % = default card/detail size.
    void setFontPercent(int percent);
    int fontPercent() const { return m_fontPercent; }
    // Whether the 单轴 / 多轴 badge is drawn in front of the file name.
    void setAxisTagVisible(bool visible);
    bool axisTagVisible() const { return m_axisTagVisible; }

    // Which slice of the library the grid currently shows.
    enum class FavoriteView
    {
        All = 0,
        NotFavorites = 1,
        Folder = 2
    };

    FavoriteView favoriteView() const { return m_favoriteView; }
    QString favoriteFolderId() const { return m_favoriteFolderId; }
    QList<FavoriteFolder> favoriteFolders() const;
    QString favoriteFolderName(const QString& folderId) const;
    bool hasFavoriteFolder(const QString& folderId) const;
    int favoriteFolderMemberCount(const QString& folderId) const;
    int favoriteMissingCount(const QString& folderId) const;
    int favoriteMissingTotal() const;
    bool isFavorite(const QString& mediaPath) const;
    QStringList favoriteNamesFor(const QString& mediaPath) const;
    QSet<QString> availablePathSet() const;
    // Media referenced by the loop list as well as by the favourites. The
    // library owns the availability decision for both panels, so the loop
    // side only has to hand over its media paths.
    void setAdditionalReferences(const QStringList& mediaPaths);
    // Available plus temporarily unreachable; what the stores use to decide
    // whether a record is stale.
    const QSet<QString>& availabilityPaths() const { return m_availablePaths; }
    // Confirmed gone: file absent and its location reachable.
    const QSet<QString>& missingPaths() const { return m_missingPaths; }
    // Volume roots that could not be reached; non-empty forbids any clean-up.
    QStringList unreachableRoots() const { return m_unreachableRoots; }
    bool hasUnreachableMedia() const { return !m_unreachableRoots.isEmpty(); }
    // Referenced but missing entries, shown as grey cards until the user
    // relinks or cleans them.
    QList<MediaItem> missingItems() const { return m_missingItems; }
    // Normalised media path -> ready cover thumbnail; other panels use it as a
    // fallback while their own thumbnails are still being generated.
    QHash<QString, QString> thumbnailPathsByMedia() const;
    // Normalised media path -> script axis count, for the panels that show the
    // same 单轴/多轴 badge as the library cards.
    QHash<QString, int> scriptAxesByMedia() const;
    FavoritesStore* favoritesStore() const { return m_favorites; }

    void showAllVideos();
    void showUnfavorited();
    void showFavoriteFolder(const QString& folderId);
    // Re-reads nothing from disk; refreshes the grid after the management
    // dialog edited the shared FavoritesStore in place.
    void refreshFavorites();

    bool isGeneratingThumbnails() const;
    void stopThumbnailGeneration();
    void clearThumbnailCache();
    void regenerateAllThumbnails();
    QString cacheRoot() const;
    // Callback of the background scan worker, queued into the GUI thread.
    // Public only because that worker job lives outside this class.
    void applyScanResult(const QHash<QString, MediaItem>& index,
                         const QList<MediaItem>& items);

    int itemCount() const { return m_items.size(); }
    // Videos that are present right now; the grey cards are separate.
    QList<MediaItem> items() const { return m_items; }
    int visibleCount() const { return m_visible.size(); }
    int thumbnailsFinished() const;
    int thumbnailsTotal() const;

    // The panel has to stay draggable down to the collapse threshold; the
    // search row is allowed to clip instead of pinning the splitter open.
    QSize minimumSizeHint() const override;

    // Script chosen for a media file (empty = automatic).
    QString scriptForMedia(const QString& mediaPath) const;
    // Human readable description used by tooltips and the info dialog.
    QString mediaInfoFor(const QString& mediaPath) const;
    // Next entry of the currently visible list; empty when at the end.
    MediaPlaybackRequest nextMedia(const QString& currentMediaPath) const;

    // Play the next video of the visible list when the current one ends.
    bool autoNext() const { return m_autoNext; }
    void setAutoNext(bool enabled);

    void setPlayingMedia(const QString& mediaPath, const QString& hwdec);
    // Thumbnail/metadata probing decodes in software; while a video is playing
    // the queue steps down to one worker so it cannot fight the player (and the
    // device link, which shares the GUI thread) for CPU.
    void setPlaybackActive(bool active);

protected:
    void resizeEvent(QResizeEvent* event) override;

signals:
    void mediaActivated(const QString& mediaPath, const QString& scriptPath);
    void rootsChanged();
    void settingsChanged();
    void message(const QString& text, int timeoutMs);
    void manageFavoritesRequested();
    // A grey card asked to be pointed at its new location.
    void relinkRequested(const QString& mediaPath);
    // Remove the records of this one missing entry (favourites, clips, tags).
    void cleanupRequested(const QString& mediaPath);
    void autoNextChanged(bool enabled);
    // The set of missing / unreachable media changed; the clean-up entries
    // re-evaluate their guards from this.
    void availabilityChanged();
    // The visible list or its metadata changed; other panels refresh their
    // derived context (available paths, cover fallbacks) from this.
    void contentChanged();

private:
    void buildUi();
    void buildSortMenu();
    void buildFavoritesMenu();
    void applyCardMetrics();
    void loadSettings();
    void saveSettings();
    void rescan();
    void refreshAvailability();
    void rebuildModel(const QString& keepSelection = QString(), int fallbackRow = -1);
    void startThumbnailJobs();
    void applyProbeResult(const QString& mediaPath, const MediaProbeResult& result);
    // Everything the index file has to keep: the entries of the library as it is
    // now, plus the entries of folders that are not part of it at the moment
    // (their covers and probe results are still valid and must survive a
    // remove/re-add cycle).
    QList<MediaItem> indexSnapshot() const;
    void scheduleIndexSave();
    void scheduleReorder();
    void updateCount();
    void updateDetails();
    void updateSortButtonText();
    void updateFavoritesButtonText();
    void showContextMenu(const QString& mediaPath, const QPoint& globalPos);
    void setMediaFavorite(const QString& folderId, const QString& mediaPath, bool favorite);
    void createFolderForMedia(const QString& mediaPath);
    void removeFromAllFavorites(const QString& mediaPath);
    void persistFavorites();
    void regenerateThumbnail(const QString& mediaPath);
    void revealInExplorer(const QString& mediaPath);
    void chooseScriptWithDialog(const QString& mediaPath);
    void chooseScript(const QString& mediaPath, const QString& scriptPath);
    void showMediaInfo(const QString& mediaPath);
    void refreshItem(const MediaItem& item);
    int indexOfItem(const QString& mediaPath) const;
    QString selectedPath() const;
    void selectPath(const QString& mediaPath);

    MediaLibraryStore* m_store = nullptr;
    MediaProbeQueue* m_probe = nullptr;
    bool m_playbackActive = false;
    MediaGridWidget* m_grid = nullptr;
    QScrollArea* m_scroll = nullptr;
    QCache<QString, QPixmap>* m_pixmapCache = nullptr;

    QList<MediaItem> m_items;
    // Referenced but missing: kept out of m_items so thumbnails are never
    // queued for them, merged back in for display.
    QList<MediaItem> m_missingItems;
    // Index snapshot from the last scan; carries the cover and probe results of
    // entries whose file has since disappeared.
    QHash<QString, MediaItem> m_indexEntries;
    QSet<QString> m_availablePaths;
    QSet<QString> m_missingPaths;
    QStringList m_unreachableRoots;
    QStringList m_additionalReferences;
    QList<MediaItem> m_visible;
    QStringList m_roots;
    MediaLibraryStore::SortMode m_sortMode = MediaLibraryStore::SortMode::Name;
    bool m_sortDescending = false;
    int m_thumbSize = 200;
    bool m_onlyWithScript = false;
    int m_fontPercent = 100;
    bool m_axisTagVisible = true;
    bool m_rescanning = false;
    bool m_rescanQueued = false;
    QString m_scanKeepSelection;
    // One scan at a time, off the GUI thread.
    QThreadPool m_scanPool;

    QLineEdit* m_search = nullptr;
    QLabel* m_count = nullptr;
    QToolButton* m_sortButton = nullptr;
    QMenu* m_sortMenu = nullptr;
    QToolButton* m_favoriteButton = nullptr;
    QMenu* m_favoriteMenu = nullptr;
    QToolButton* m_autoNextButton = nullptr;

    FavoritesStore* m_favorites = nullptr;
    FavoriteView m_favoriteView = FavoriteView::All;
    QString m_favoriteFolderId;
    QTimer* m_indexSaveTimer = nullptr;
    QTimer* m_reorderTimer = nullptr;
    bool m_gridUpdatePending = false;
    bool m_autoNext = true;
    QString m_playingPath;
    QString m_playingHwdec;
};
