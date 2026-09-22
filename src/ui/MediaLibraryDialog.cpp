#include "core/Loc.h"
#include "ui/MediaLibraryDialog.h"

#include "core/AppLogger.h"
#include "core/FavoritesStore.h"
#include "ui/MediaCleanupPrompt.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

MediaLibraryDialog::MediaLibraryDialog(QWidget* parent, FavoritesStore* favorites)
    : QDialog(parent)
    , m_favorites(favorites)
{
    setWindowTitle(LT("媒体库管理"));
    setMinimumSize(620, 430);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    m_tabs = new QTabWidget(this);
    layout->addWidget(m_tabs, 1);

    auto* rootsTab = new QWidget(m_tabs);
    auto* rootsLayout = new QVBoxLayout(rootsTab);
    rootsLayout->setContentsMargins(12, 12, 12, 12);
    rootsLayout->setSpacing(10);
    buildRootsTab(rootsLayout);
    m_tabs->addTab(rootsTab, LT("媒体库文件夹"));

    auto* favoritesTab = new QWidget(m_tabs);
    auto* favoritesLayout = new QVBoxLayout(favoritesTab);
    favoritesLayout->setContentsMargins(12, 12, 12, 12);
    favoritesLayout->setSpacing(10);
    buildFavoritesTab(favoritesLayout);
    m_tabs->addTab(favoritesTab, LT("收藏夹"));

    auto* closeRow = new QHBoxLayout();
    closeRow->addStretch();
    auto* closeButton = new QPushButton(LT("关闭"), this);
    closeButton->setDefault(true);
    closeRow->addWidget(closeButton);
    layout->addLayout(closeRow);

    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);

    updateButtons();
    refreshFavorites();
}

void MediaLibraryDialog::buildRootsTab(QVBoxLayout* layout)
{
    auto* title = new QLabel(LT("媒体库文件夹"), this);
    title->setObjectName(QStringLiteral("SectionTitle"));
    layout->addWidget(title);

    auto* hint = new QLabel(
        LT("软件每次启动和刷新时都会递归扫描这些文件夹，新复制的视频与脚本会自动出现在媒体库里。"),
        this);
    hint->setObjectName(QStringLiteral("Muted"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_roots = new QListWidget(this);
    m_roots->setMinimumHeight(180);
    layout->addWidget(m_roots, 1);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(8);
    auto* addButton = new QPushButton(LT("添加文件夹…"), this);
    m_removeButton = new QPushButton(LT("移除选中"), this);
    buttonRow->addWidget(addButton);
    buttonRow->addWidget(m_removeButton);
    buttonRow->addStretch();
    layout->addLayout(buttonRow);

    connect(addButton, &QPushButton::clicked, this, [this]() {
        const QString folder = QFileDialog::getExistingDirectory(
            this, LT("添加媒体库文件夹"));
        if (!folder.isEmpty())
            emit addRequested(folder);
    });
    connect(m_removeButton, &QPushButton::clicked, this, [this]() {
        const QListWidgetItem* item = m_roots->currentItem();
        if (item)
            emit removeRequested(item->data(Qt::UserRole).toString());
    });
    connect(m_roots, &QListWidget::currentRowChanged, this,
            [this](int) { updateButtons(); });
}

void MediaLibraryDialog::buildFavoritesTab(QVBoxLayout* layout)
{
    auto* title = new QLabel(LT("收藏夹"), this);
    title->setObjectName(QStringLiteral("SectionTitle"));
    layout->addWidget(title);

    m_favoriteHint = new QLabel(this);
    m_favoriteHint->setObjectName(QStringLiteral("Muted"));
    m_favoriteHint->setWordWrap(true);
    layout->addWidget(m_favoriteHint);

    m_folders = new QListWidget(this);
    m_folders->setMinimumHeight(150);
    layout->addWidget(m_folders, 1);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(8);
    auto* createButton = new QPushButton(LT("新建…"), this);
    m_favoriteRenameButton = new QPushButton(LT("重命名…"), this);
    m_favoriteRemoveButton = new QPushButton(LT("删除"), this);
    buttonRow->addWidget(createButton);
    buttonRow->addWidget(m_favoriteRenameButton);
    buttonRow->addWidget(m_favoriteRemoveButton);
    buttonRow->addStretch();
    layout->addLayout(buttonRow);

    auto* orderRow = new QHBoxLayout();
    orderRow->setSpacing(8);
    m_favoriteUpButton = new QPushButton(LT("上移"), this);
    m_favoriteDownButton = new QPushButton(LT("下移"), this);
    m_favoritePruneButton = new QPushButton(LT("清理失效项"), this);
    orderRow->addWidget(m_favoriteUpButton);
    orderRow->addWidget(m_favoriteDownButton);
    orderRow->addSpacing(12);
    orderRow->addWidget(m_favoritePruneButton);
    orderRow->addStretch();
    layout->addLayout(orderRow);

    connect(createButton, &QPushButton::clicked, this, &MediaLibraryDialog::createFolder);
    connect(m_favoriteRenameButton, &QPushButton::clicked,
            this, &MediaLibraryDialog::renameFolder);
    connect(m_favoriteRemoveButton, &QPushButton::clicked,
            this, &MediaLibraryDialog::removeFolder);
    connect(m_favoriteUpButton, &QPushButton::clicked, this,
            [this]() { moveFolder(-1); });
    connect(m_favoriteDownButton, &QPushButton::clicked, this,
            [this]() { moveFolder(1); });
    connect(m_favoritePruneButton, &QPushButton::clicked,
            this, &MediaLibraryDialog::pruneMissing);
    connect(m_folders, &QListWidget::currentRowChanged, this,
            [this](int) { updateFavoriteButtons(); });
    connect(m_folders, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { renameFolder(); });
}

void MediaLibraryDialog::setRoots(const QStringList& roots)
{
    m_roots->clear();
    for (const QString& root : roots)
    {
        auto* item = new QListWidgetItem(root, m_roots);
        item->setData(Qt::UserRole, root);
        item->setToolTip(root);
    }
    updateButtons();
}

void MediaLibraryDialog::setFavoritesContext(const QSet<QString>& availablePaths,
                                             const QStringList& unreachableRoots)
{
    m_availablePaths = availablePaths;
    m_unreachableRoots = unreachableRoots;
    refreshFavorites();
}

void MediaLibraryDialog::showFavoritesTab()
{
    if (m_tabs)
        m_tabs->setCurrentIndex(1);
}

void MediaLibraryDialog::refreshFavorites()
{
    if (!m_folders)
        return;
    if (!m_favorites)
    {
        m_folders->setEnabled(false);
        m_favoriteHint->setText(LT("收藏夹不可用。"));
        updateFavoriteButtons();
        return;
    }

    const QString keepId = selectedFolderId();
    m_folders->clear();
    for (const FavoriteFolder& folder : m_favorites->folders())
    {
        const int missing = m_favorites->missingCount(folder.id, m_availablePaths);
        const int valid = folder.members.size() - missing;
        QString text = LT("%1  ·  有效 %2").arg(folder.name).arg(valid);
        if (missing > 0)
            text += LT(" / 失效 %1").arg(missing);

        auto* item = new QListWidgetItem(text, m_folders);
        item->setData(Qt::UserRole, folder.id);
        item->setToolTip(folder.name);
        if (folder.id == keepId)
            m_folders->setCurrentItem(item);
    }

    const int missingTotal = m_favorites->missingCount(m_availablePaths);
    QString hint = LT("收藏夹只记录视频位置，不会复制或移动文件；视频被移走或删除后会显示为失效项，可以一键清理。当前共 %1 个收藏夹，%2 个失效引用。")
                       .arg(m_favorites->count())
                       .arg(missingTotal);
    if (!m_unreachableRoots.isEmpty())
    {
        // Those volumes may still hold every "missing" file, so cleaning is
        // blocked until they are reachable again.
        hint += LT("\n⚠ 这些位置当前打不开，清理已暂停：%1")
                    .arg(m_unreachableRoots.join(QStringLiteral("、")));
    }
    m_favoriteHint->setText(hint);
    if (m_favoritePruneButton)
        m_favoritePruneButton->setEnabled(missingTotal > 0 && m_unreachableRoots.isEmpty());
    updateFavoriteButtons();
}

void MediaLibraryDialog::updateButtons()
{
    if (m_removeButton)
        m_removeButton->setEnabled(m_roots && m_roots->currentItem() != nullptr);
}

void MediaLibraryDialog::updateFavoriteButtons()
{
    const bool hasSelection = !selectedFolderId().isEmpty();
    const int row = m_folders ? m_folders->currentRow() : -1;
    const int count = m_favorites ? m_favorites->count() : 0;
    if (m_favoriteRenameButton)
        m_favoriteRenameButton->setEnabled(hasSelection);
    if (m_favoriteRemoveButton)
        m_favoriteRemoveButton->setEnabled(hasSelection);
    if (m_favoriteUpButton)
        m_favoriteUpButton->setEnabled(hasSelection && row > 0);
    if (m_favoriteDownButton)
        m_favoriteDownButton->setEnabled(hasSelection && row >= 0 && row < count - 1);
}

QString MediaLibraryDialog::selectedFolderId() const
{
    if (!m_folders)
        return QString();
    const QListWidgetItem* item = m_folders->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

bool MediaLibraryDialog::saveFavorites()
{
    if (!m_favorites)
        return false;
    if (m_favorites->save())
        return true;
    QMessageBox::warning(this, LT("收藏夹"),
                         LT("收藏夹未能保存，请检查磁盘权限后重试。"));
    return false;
}

void MediaLibraryDialog::createFolder()
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
    AppLogger::log(QStringLiteral("library"),
                   QStringLiteral("新建收藏夹: %1").arg(m_favorites->folderName(folderId)));
    // On a save failure the in-memory state is kept and the dialog still
    // reflects it; saveFavorites() already warned the user.
    saveFavorites();
    refreshFavorites();
    for (int i = 0; i < m_folders->count(); ++i)
    {
        if (m_folders->item(i)->data(Qt::UserRole).toString() == folderId)
        {
            m_folders->setCurrentRow(i);
            break;
        }
    }
    emit favoritesChanged();
}

void MediaLibraryDialog::renameFolder()
{
    const QString folderId = selectedFolderId();
    if (!m_favorites || folderId.isEmpty())
        return;

    bool accepted = false;
    const QString previousName = m_favorites->folderName(folderId);
    const QString name = QInputDialog::getText(
        this, LT("重命名收藏夹"), LT("收藏夹名称："),
        QLineEdit::Normal, previousName, &accepted);
    if (!accepted)
        return;

    QString error;
    if (!m_favorites->renameFolder(folderId, name, &error))
    {
        QMessageBox::warning(this, LT("重命名收藏夹"),
                             error.isEmpty() ? LT("无法重命名收藏夹") : error);
        return;
    }
    AppLogger::log(QStringLiteral("library"),
                   QStringLiteral("重命名收藏夹: %1 -> %2")
                       .arg(previousName, m_favorites->folderName(folderId)));
    saveFavorites();
    refreshFavorites();
    emit favoritesChanged();
}

void MediaLibraryDialog::removeFolder()
{
    const QString folderId = selectedFolderId();
    if (!m_favorites || folderId.isEmpty())
        return;

    const QString name = m_favorites->folderName(folderId);
    const int count = m_favorites->memberCount(folderId);
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, LT("删除收藏夹"),
        LT("删除收藏夹「%1」？\n\n只移除这个收藏夹和其中的 %2 条收藏记录，不会删除任何视频或脚本文件。")
            .arg(name)
            .arg(count),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    QString error;
    if (!m_favorites->removeFolder(folderId, &error))
    {
        QMessageBox::warning(this, LT("删除收藏夹"),
                             error.isEmpty() ? LT("无法删除收藏夹") : error);
        return;
    }
    AppLogger::log(QStringLiteral("library"),
                   QStringLiteral("删除收藏夹: %1（%2 条记录）").arg(name).arg(count));
    saveFavorites();
    refreshFavorites();
    emit favoritesChanged();
}

void MediaLibraryDialog::moveFolder(int delta)
{
    const QString folderId = selectedFolderId();
    if (!m_favorites || folderId.isEmpty())
        return;

    QString error;
    if (!m_favorites->moveFolder(folderId, delta, &error))
        return;
    AppLogger::log(QStringLiteral("library"),
                   QStringLiteral("调整收藏夹顺序: %1 %2")
                       .arg(m_favorites->folderName(folderId),
                            delta < 0 ? LT("上移") : LT("下移")));
    saveFavorites();
    refreshFavorites();
    emit favoritesChanged();
}

void MediaLibraryDialog::pruneMissing()
{
    if (!m_favorites)
        return;

    if (!m_unreachableRoots.isEmpty())
    {
        MediaCleanupPrompt::warnUnreachable(this, m_unreachableRoots);
        return;
    }

    // The window performs the wipe: it also holds the loop store, so favourite
    // records, clips and tags always go together.
    emit cleanupRequested();
    refreshFavorites();
}
