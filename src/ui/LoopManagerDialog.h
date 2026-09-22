#pragma once

#include <QDialog>
#include <QSet>
#include <QString>

class LoopStore;
class QLabel;
class QListWidget;
class QPushButton;
class QTabWidget;
class QVBoxLayout;

// Loop list management: the folders a clip may belong to, the shared tag pool
// and the cleanup of clips whose video disappeared.
class LoopManagerDialog : public QDialog
{
    Q_OBJECT

public:
    LoopManagerDialog(QWidget* parent, LoopStore* store,
                      const QSet<QString>& availablePaths,
                      const QStringList& unreachableRoots = QStringList());

    // Updated after the shared clean-up so the missing counts and the guard
    // reflect the new state.
    void setAvailability(const QSet<QString>& availablePaths,
                         const QStringList& unreachableRoots);

signals:
    // Emitted whenever the store changed so the panel can refresh its grid.
    void clipsChanged();
    // Same shared clean-up as the media library dialog.
    void cleanupRequested();

private:
    void buildFoldersTab(QVBoxLayout* layout);
    void buildTagsTab(QVBoxLayout* layout);
    void refreshFolders();
    void refreshTags();
    void refreshMissing();
    void updateFolderButtons();
    void updateTagButtons();
    QString selectedFolderId() const;
    QString selectedTag() const;
    void saveStore();
    void createFolder();
    void renameFolder();
    void removeFolder();
    void moveFolder(int delta);
    void renameTag();
    void removeTag();
    void pruneUnusedTags();
    void pruneMissingClips();

    LoopStore* m_store = nullptr;
    QSet<QString> m_availablePaths;
    QStringList m_unreachableRoots;
    QTabWidget* m_tabs = nullptr;
    QListWidget* m_folders = nullptr;
    QListWidget* m_tags = nullptr;
    QLabel* m_missingLabel = nullptr;
    QPushButton* m_folderRenameButton = nullptr;
    QPushButton* m_folderRemoveButton = nullptr;
    QPushButton* m_folderUpButton = nullptr;
    QPushButton* m_folderDownButton = nullptr;
    QPushButton* m_tagRenameButton = nullptr;
    QPushButton* m_tagRemoveButton = nullptr;
    QPushButton* m_tagPruneButton = nullptr;
    QPushButton* m_missingPruneButton = nullptr;
};
