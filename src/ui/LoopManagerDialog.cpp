#include "core/Loc.h"
#include "ui/LoopManagerDialog.h"

#include "core/LoopStore.h"
#include "ui/MediaCleanupPrompt.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

LoopManagerDialog::LoopManagerDialog(QWidget* parent, LoopStore* store,
                                     const QSet<QString>& availablePaths,
                                     const QStringList& unreachableRoots)
    : QDialog(parent)
    , m_store(store)
    , m_availablePaths(availablePaths)
    , m_unreachableRoots(unreachableRoots)
{
    setWindowTitle(LT("循环列表管理"));
    resize(520, 460);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 12);
    layout->setSpacing(10);

    m_tabs = new QTabWidget(this);
    layout->addWidget(m_tabs, 1);

    auto* foldersTab = new QWidget(m_tabs);
    auto* foldersLayout = new QVBoxLayout(foldersTab);
    foldersLayout->setContentsMargins(10, 10, 10, 10);
    buildFoldersTab(foldersLayout);
    m_tabs->addTab(foldersTab, LT("循环收藏夹"));

    auto* tagsTab = new QWidget(m_tabs);
    auto* tagsLayout = new QVBoxLayout(tagsTab);
    tagsLayout->setContentsMargins(10, 10, 10, 10);
    buildTagsTab(tagsLayout);
    m_tabs->addTab(tagsTab, LT("标签"));

    auto* missingRow = new QHBoxLayout();
    m_missingLabel = new QLabel(this);
    m_missingLabel->setObjectName(QStringLiteral("Muted"));
    missingRow->addWidget(m_missingLabel, 1);
    m_missingPruneButton = new QPushButton(LT("清理失效片段"), this);
    missingRow->addWidget(m_missingPruneButton);
    layout->addLayout(missingRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(LT("关闭"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_missingPruneButton, &QPushButton::clicked, this,
            &LoopManagerDialog::pruneMissingClips);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this]() {
        updateFolderButtons();
        updateTagButtons();
    });

    refreshFolders();
    refreshTags();
    refreshMissing();
}

void LoopManagerDialog::buildFoldersTab(QVBoxLayout* layout)
{
    m_folders = new QListWidget(this);
    m_folders->setAlternatingRowColors(true);
    layout->addWidget(m_folders, 1);

    auto* row = new QHBoxLayout();
    auto* createButton = new QPushButton(LT("新建…"), this);
    m_folderRenameButton = new QPushButton(LT("重命名…"), this);
    m_folderRemoveButton = new QPushButton(LT("删除"), this);
    m_folderUpButton = new QPushButton(LT("上移"), this);
    m_folderDownButton = new QPushButton(LT("下移"), this);
    row->addWidget(createButton);
    row->addWidget(m_folderRenameButton);
    row->addWidget(m_folderRemoveButton);
    row->addStretch(1);
    row->addWidget(m_folderUpButton);
    row->addWidget(m_folderDownButton);
    layout->addLayout(row);

    auto* hint = new QLabel(
        LT("一个循环片段可以同时属于多个收藏夹；删除收藏夹不会删除片段本身。"),
        this);
    hint->setObjectName(QStringLiteral("Muted"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    connect(createButton, &QPushButton::clicked, this, &LoopManagerDialog::createFolder);
    connect(m_folderRenameButton, &QPushButton::clicked, this,
            &LoopManagerDialog::renameFolder);
    connect(m_folderRemoveButton, &QPushButton::clicked, this,
            &LoopManagerDialog::removeFolder);
    connect(m_folderUpButton, &QPushButton::clicked, this,
            [this]() { moveFolder(-1); });
    connect(m_folderDownButton, &QPushButton::clicked, this,
            [this]() { moveFolder(1); });
    connect(m_folders, &QListWidget::currentRowChanged, this,
            [this]() { updateFolderButtons(); });
    connect(m_folders, &QListWidget::itemDoubleClicked, this,
            [this]() { renameFolder(); });
}

void LoopManagerDialog::buildTagsTab(QVBoxLayout* layout)
{
    m_tags = new QListWidget(this);
    m_tags->setAlternatingRowColors(true);
    layout->addWidget(m_tags, 1);

    auto* row = new QHBoxLayout();
    m_tagRenameButton = new QPushButton(LT("重命名…"), this);
    m_tagRemoveButton = new QPushButton(LT("删除"), this);
    m_tagPruneButton = new QPushButton(LT("清理未使用标签"), this);
    row->addWidget(m_tagRenameButton);
    row->addWidget(m_tagRemoveButton);
    row->addStretch(1);
    row->addWidget(m_tagPruneButton);
    layout->addLayout(row);

    auto* hint = new QLabel(
        LT("重命名到已有标签会把两个标签合并；删除只会移除标签，不会删除片段。"),
        this);
    hint->setObjectName(QStringLiteral("Muted"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    connect(m_tagRenameButton, &QPushButton::clicked, this, &LoopManagerDialog::renameTag);
    connect(m_tagRemoveButton, &QPushButton::clicked, this, &LoopManagerDialog::removeTag);
    connect(m_tagPruneButton, &QPushButton::clicked, this,
            &LoopManagerDialog::pruneUnusedTags);
    connect(m_tags, &QListWidget::currentRowChanged, this,
            [this]() { updateTagButtons(); });
    connect(m_tags, &QListWidget::itemDoubleClicked, this,
            [this]() { renameTag(); });
}

void LoopManagerDialog::refreshFolders()
{
    if (!m_folders || !m_store)
        return;

    const QString keep = selectedFolderId();
    m_folders->clear();
    for (const LoopFolder& folder : m_store->folders())
    {
        auto* item = new QListWidgetItem(
            LT("%1（%2 个片段）")
                .arg(folder.name)
                .arg(m_store->folderClipCount(folder.id)),
            m_folders);
        item->setData(Qt::UserRole, folder.id);
        if (folder.id == keep)
            m_folders->setCurrentItem(item);
    }
    if (m_folders->count() == 0)
    {
        auto* item = new QListWidgetItem(LT("（还没有循环收藏夹）"), m_folders);
        item->setFlags(Qt::NoItemFlags);
    }
    updateFolderButtons();
}

void LoopManagerDialog::refreshTags()
{
    if (!m_tags || !m_store)
        return;

    const QString keep = selectedTag();
    m_tags->clear();
    for (const QString& tag : m_store->tagsByUsage())
    {
        auto* item = new QListWidgetItem(
            LT("%1（%2 个片段）").arg(tag).arg(m_store->tagUsage(tag)), m_tags);
        item->setData(Qt::UserRole, tag);
        if (tag.compare(keep, Qt::CaseInsensitive) == 0)
            m_tags->setCurrentItem(item);
    }
    if (m_tags->count() == 0)
    {
        auto* item = new QListWidgetItem(LT("（还没有标签）"), m_tags);
        item->setFlags(Qt::NoItemFlags);
    }
    updateTagButtons();
}

void LoopManagerDialog::refreshMissing()
{
    if (!m_missingLabel || !m_store)
        return;
    const int missing = m_store->missingClipCount(m_availablePaths);
    m_missingLabel->setText(missing > 0
                                ? LT("有 %1 个片段找不到视频文件").arg(missing)
                                : LT("所有片段的视频文件都在"));
    if (m_missingPruneButton)
        m_missingPruneButton->setEnabled(missing > 0);
}

void LoopManagerDialog::updateFolderButtons()
{
    const bool has = !selectedFolderId().isEmpty();
    for (QPushButton* button : {m_folderRenameButton, m_folderRemoveButton})
    {
        if (button)
            button->setEnabled(has);
    }
}

void LoopManagerDialog::updateTagButtons()
{
    const bool has = !selectedTag().isEmpty();
    for (QPushButton* button : {m_tagRenameButton, m_tagRemoveButton})
    {
        if (button)
            button->setEnabled(has);
    }
    if (m_tagPruneButton && m_store)
        m_tagPruneButton->setEnabled(!m_store->tags().isEmpty());
}

QString LoopManagerDialog::selectedFolderId() const
{
    if (!m_folders)
        return QString();
    QListWidgetItem* item = m_folders->currentItem();
    if (!item || !item->flags().testFlag(Qt::ItemIsSelectable))
        return QString();
    return item->data(Qt::UserRole).toString();
}

QString LoopManagerDialog::selectedTag() const
{
    if (!m_tags)
        return QString();
    QListWidgetItem* item = m_tags->currentItem();
    if (!item || !item->flags().testFlag(Qt::ItemIsSelectable))
        return QString();
    return item->data(Qt::UserRole).toString();
}

void LoopManagerDialog::saveStore()
{
    if (m_store)
        m_store->save();
    refreshFolders();
    refreshTags();
    refreshMissing();
    emit clipsChanged();
}

void LoopManagerDialog::createFolder()
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
    if (m_store->createFolder(name, &error).isEmpty())
    {
        QMessageBox::warning(this, LT("新建收藏夹失败"), error);
        return;
    }
    saveStore();
}

void LoopManagerDialog::renameFolder()
{
    const QString id = selectedFolderId();
    if (!m_store || id.isEmpty())
        return;

    bool ok = false;
    const QString name = QInputDialog::getText(this, LT("重命名循环收藏夹"),
                                               LT("名称"), QLineEdit::Normal,
                                               m_store->folderName(id), &ok);
    if (!ok)
        return;

    QString error;
    if (!m_store->renameFolder(id, name, &error))
    {
        QMessageBox::warning(this, LT("重命名失败"), error);
        return;
    }
    saveStore();
}

void LoopManagerDialog::removeFolder()
{
    const QString id = selectedFolderId();
    if (!m_store || id.isEmpty())
        return;

    const QString name = m_store->folderName(id);
    const auto answer = QMessageBox::question(
        this, LT("删除循环收藏夹"),
        LT("删除收藏夹「%1」？\n片段本身会保留，只是不再属于这个收藏夹。")
            .arg(name));
    if (answer != QMessageBox::Yes)
        return;

    QString error;
    if (!m_store->removeFolder(id, &error))
    {
        QMessageBox::warning(this, LT("删除失败"), error);
        return;
    }
    saveStore();
}

void LoopManagerDialog::moveFolder(int delta)
{
    const QString id = selectedFolderId();
    if (!m_store || id.isEmpty())
        return;

    QString error;
    if (!m_store->moveFolder(id, delta, &error))
    {
        // Moving past the end is not an error worth a dialog.
        return;
    }
    saveStore();
    for (int i = 0; i < m_folders->count(); ++i)
    {
        if (m_folders->item(i)->data(Qt::UserRole).toString() == id)
        {
            m_folders->setCurrentRow(i);
            break;
        }
    }
}

void LoopManagerDialog::renameTag()
{
    const QString tag = selectedTag();
    if (!m_store || tag.isEmpty())
        return;

    bool ok = false;
    const QString name = QInputDialog::getText(this, LT("重命名标签"),
                                               LT("标签名称"), QLineEdit::Normal,
                                               tag, &ok);
    if (!ok)
        return;

    QString error;
    if (!m_store->renameTag(tag, name, &error))
    {
        QMessageBox::warning(this, LT("重命名失败"), error);
        return;
    }
    saveStore();
}

void LoopManagerDialog::removeTag()
{
    const QString tag = selectedTag();
    if (!m_store || tag.isEmpty())
        return;

    const auto answer = QMessageBox::question(
        this, LT("删除标签"),
        LT("删除标签「%1」？\n它会从所有片段上移除，片段本身会保留。").arg(tag));
    if (answer != QMessageBox::Yes)
        return;

    QString error;
    if (!m_store->removeTag(tag, &error))
    {
        QMessageBox::warning(this, LT("删除失败"), error);
        return;
    }
    saveStore();
}

void LoopManagerDialog::pruneUnusedTags()
{
    if (!m_store)
        return;
    const int removed = m_store->pruneUnusedTags();
    saveStore();
    QMessageBox::information(this, LT("清理未使用标签"),
                            removed > 0 ? LT("已清理 %1 个标签。").arg(removed)
                                        : LT("没有未使用的标签。"));
}

void LoopManagerDialog::setAvailability(const QSet<QString>& availablePaths,
                                        const QStringList& unreachableRoots)
{
    m_availablePaths = availablePaths;
    m_unreachableRoots = unreachableRoots;
    refreshFolders();
    refreshTags();
}

void LoopManagerDialog::pruneMissingClips()
{
    if (!m_store)
        return;

    if (!m_unreachableRoots.isEmpty())
    {
        MediaCleanupPrompt::warnUnreachable(this, m_unreachableRoots);
        return;
    }

    // The window runs the shared clean-up; see MediaLibraryDialog.
    emit cleanupRequested();
    refreshFolders();
    refreshTags();
}
