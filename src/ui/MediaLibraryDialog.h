#pragma once

#include <QDialog>
#include <QSet>
#include <QStringList>

class FavoritesStore;
class QLabel;
class QListWidget;
class QPushButton;
class QTabWidget;
class QVBoxLayout;

// Media library management: the root folders that are scanned recursively on
// every start and refresh, plus the favourite folders the user maintains.
class MediaLibraryDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MediaLibraryDialog(QWidget* parent = nullptr,
                                FavoritesStore* favorites = nullptr);

    void setRoots(const QStringList& roots);
    // Media files that currently exist in the library; used to show how many
    // entries of a favourite folder went missing.
    // availablePaths counts reachable entries plus the ones that are merely
    // offline. unreachableRoots names the volumes that could not be opened;
    // while that list is not empty no clean-up is allowed.
    void setFavoritesContext(const QSet<QString>& availablePaths,
                             const QStringList& unreachableRoots = QStringList());
    void showFavoritesTab();

signals:
    void addRequested(const QString& folder);
    void removeRequested(const QString& folder);
    void favoritesChanged();
    // The window owns the shared clean-up because it also owns the loop list;
    // asking it keeps both panels from removing a video half way.
    void cleanupRequested();

private:
    void buildRootsTab(QVBoxLayout* layout);
    void buildFavoritesTab(QVBoxLayout* layout);
    void refreshFavorites();
    void updateButtons();
    void updateFavoriteButtons();
    QString selectedFolderId() const;
    bool saveFavorites();
    void createFolder();
    void renameFolder();
    void removeFolder();
    void moveFolder(int delta);
    void pruneMissing();

    FavoritesStore* m_favorites = nullptr;
    QSet<QString> m_availablePaths;
    QStringList m_unreachableRoots;
    QTabWidget* m_tabs = nullptr;
    QListWidget* m_roots = nullptr;
    QListWidget* m_folders = nullptr;
    QLabel* m_favoriteHint = nullptr;
    QPushButton* m_removeButton = nullptr;
    QPushButton* m_favoriteRenameButton = nullptr;
    QPushButton* m_favoriteRemoveButton = nullptr;
    QPushButton* m_favoriteUpButton = nullptr;
    QPushButton* m_favoriteDownButton = nullptr;
    QPushButton* m_favoritePruneButton = nullptr;
};
