#include "core/Loc.h"
#include "ui/LoopClipDialog.h"

#include "core/LoopStore.h"

#include <QCompleter>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShowEvent>
#include <QVBoxLayout>

LoopClipDialog::LoopClipDialog(QWidget* parent, LoopStore* store, const LoopClip& clip,
                               qint64 mediaDurationMs)
    : QDialog(parent)
    , m_store(store)
    , m_clip(clip)
    , m_mediaDurationMs(mediaDurationMs)
{
    setWindowTitle(clip.id.isEmpty() ? LT("添加循环片段")
                                     : LT("编辑循环片段"));
    setMinimumWidth(440);
    buildUi();
}

void LoopClipDialog::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(10);

    auto* form = new QFormLayout();
    form->setSpacing(8);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_titleEdit = new QLineEdit(this);
    m_titleEdit->setPlaceholderText(LT("留空则自动使用「视频名 · 时间段」"));
    const QString title = m_clip.title.trimmed().isEmpty()
                              ? LoopClipRules::defaultTitle(m_clip.mediaPath, m_clip.startMs,
                                                            m_clip.endMs)
                              : m_clip.title;
    m_titleEdit->setText(title);
    form->addRow(LT("标题"), m_titleEdit);

    auto* rangeRow = new QHBoxLayout();
    rangeRow->setSpacing(6);
    m_startEdit = new QLineEdit(LoopClipRules::formatTimecode(m_clip.startMs), this);
    m_startEdit->setMinimumWidth(110);
    m_endEdit = new QLineEdit(LoopClipRules::formatTimecode(m_clip.endMs), this);
    m_endEdit->setMinimumWidth(110);
    rangeRow->addWidget(new QLabel(QStringLiteral("A"), this));
    rangeRow->addWidget(m_startEdit, 1);
    rangeRow->addSpacing(8);
    rangeRow->addWidget(new QLabel(QStringLiteral("B"), this));
    rangeRow->addWidget(m_endEdit, 1);
    form->addRow(LT("时间段"), rangeRow);

    auto* tagRow = new QHBoxLayout();
    tagRow->setSpacing(6);
    m_tagEdit = new QLineEdit(this);
    m_tagEdit->setPlaceholderText(LT("输入标签后回车，可一次输入多个（逗号分隔）"));
    QStringList tagPool;
    if (m_store)
        tagPool = m_store->tagsByUsage();
    if (!tagPool.isEmpty())
    {
        auto* completer = new QCompleter(tagPool, this);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setCompletionMode(QCompleter::PopupCompletion);
        m_tagEdit->setCompleter(completer);
    }
    auto* addTagButton = new QPushButton(LT("添加"), this);
    tagRow->addWidget(m_tagEdit, 1);
    tagRow->addWidget(addTagButton);
    form->addRow(LT("标签"), tagRow);

    auto* tagArea = new QVBoxLayout();
    tagArea->setSpacing(4);
    m_tagList = new QListWidget(this);
    m_tagList->setMaximumHeight(84);
    m_tagList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tagList->setAlternatingRowColors(true);
    tagArea->addWidget(m_tagList);
    auto* tagButtons = new QHBoxLayout();
    auto* removeTagButton = new QPushButton(LT("移除所选标签"), this);
    tagButtons->addStretch(1);
    tagButtons->addWidget(removeTagButton);
    tagArea->addLayout(tagButtons);
    form->addRow(QString(), tagArea);

    for (const QString& tag : m_clip.tags)
        m_tagList->addItem(tag);

    auto* folderRow = new QVBoxLayout();
    folderRow->setSpacing(6);
    // Plain check boxes instead of a checkable list: clicking anywhere on the
    // row toggles it, so a folder never has to be "activated" first.
    auto* folderScroll = new QScrollArea(this);
    folderScroll->setObjectName(QStringLiteral("FolderChecks"));
    folderScroll->setWidgetResizable(true);
    folderScroll->setFrameShape(QFrame::NoFrame);
    folderScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    folderScroll->setFixedHeight(120);
    m_folderContainer = new QWidget(folderScroll);
    m_folderLayout = new QVBoxLayout(m_folderContainer);
    m_folderLayout->setContentsMargins(4, 4, 4, 4);
    m_folderLayout->setSpacing(4);
    folderScroll->setWidget(m_folderContainer);
    folderRow->addWidget(folderScroll);
    auto* folderButtons = new QHBoxLayout();
    auto* newFolderButton = new QPushButton(LT("新建收藏夹…"), this);
    folderButtons->addStretch(1);
    folderButtons->addWidget(newFolderButton);
    folderRow->addLayout(folderButtons);
    form->addRow(LT("循环收藏夹"), folderRow);

    m_noteEdit = new QPlainTextEdit(this);
    m_noteEdit->setPlaceholderText(LT("可选备注"));
    m_noteEdit->setPlainText(m_clip.note);
    m_noteEdit->setFixedHeight(64);
    form->addRow(LT("备注"), m_noteEdit);

    layout->addLayout(form);

    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("Muted"));
    m_hint->setWordWrap(true);
    layout->addWidget(m_hint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,
                                         this);
    buttons->button(QDialogButtonBox::Save)->setText(LT("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(LT("取消"));
    // Enter belongs to the field that has focus (the tag box adds a tag), it
    // must never submit the whole dialog by accident.
    for (QPushButton* button : buttons->findChildren<QPushButton*>())
        button->setAutoDefault(false);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &LoopClipDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &LoopClipDialog::reject);
    connect(m_tagEdit, &QLineEdit::returnPressed, this,
            [this]() { addTag(m_tagEdit->text()); });
    connect(addTagButton, &QPushButton::clicked, this,
            [this]() { addTag(m_tagEdit->text()); });
    connect(removeTagButton, &QPushButton::clicked, this,
            &LoopClipDialog::removeSelectedTag);
    connect(newFolderButton, &QPushButton::clicked, this, &LoopClipDialog::createFolder);

    refreshFolders();
    m_tagEdit->setFocus();
}

void LoopClipDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    // Put the caret in the tag field instead of letting the dialog focus a
    // button, which used to make Enter submit the whole form.
    if (m_tagEdit)
        m_tagEdit->setFocus();
}

void LoopClipDialog::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
    {
        // Enter means "add this tag" while typing a tag, and nothing anywhere
        // else. It must never accept the dialog.
        if (m_tagEdit && m_tagEdit->hasFocus())
            addTag(m_tagEdit->text());
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

QSet<QString> LoopClipDialog::checkedFolderIds() const
{
    QSet<QString> ids;
    for (const auto& entry : m_folderChecks)
    {
        if (entry.second && entry.second->isChecked())
            ids.insert(entry.first);
    }
    return ids;
}

void LoopClipDialog::refreshFolders(const QSet<QString>& checked)
{
    if (!m_folderContainer || !m_folderLayout || !m_store)
        return;

    // An empty request keeps the current ticks; on the first build that is the
    // clip's own folder list.
    QSet<QString> selected = checked;
    if (selected.isEmpty())
    {
        if (m_folderChecks.isEmpty())
        {
            for (const QString& folderId : m_clip.folderIds)
                selected.insert(folderId);
        }
        else
        {
            selected = checkedFolderIds();
        }
    }

    while (QLayoutItem* item = m_folderLayout->takeAt(0))
    {
        if (QWidget* widget = item->widget())
        {
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }
    m_folderChecks.clear();

    for (const LoopFolder& folder : m_store->folders())
    {
        auto* box = new QCheckBox(
            LT("%1（%2 个片段）")
                .arg(folder.name)
                .arg(m_store->folderClipCount(folder.id)),
            m_folderContainer);
        box->setChecked(selected.contains(folder.id));
        box->setCursor(Qt::PointingHandCursor);
        m_folderLayout->addWidget(box);
        m_folderChecks.append(qMakePair(folder.id, box));
    }
    if (m_store->folders().isEmpty())
    {
        auto* empty = new QLabel(LT("（还没有循环收藏夹，可点“新建收藏夹…”）"),
                                 m_folderContainer);
        empty->setObjectName(QStringLiteral("Muted"));
        m_folderLayout->addWidget(empty);
    }
    m_folderLayout->addStretch(1);
}

void LoopClipDialog::addTag(const QString& text)
{
    if (!m_tagEdit)
        return;

    const QStringList parts = text.split(QRegularExpression(QStringLiteral("[,，;；]")),
                                         Qt::SkipEmptyParts);
    for (const QString& part : parts)
    {
        const QString tag = part.trimmed();
        if (tag.isEmpty())
            continue;
        if (tag.size() > LoopStore::maxTagLength())
        {
            if (m_hint)
                m_hint->setText(LT("标签「%1」不能超过 %2 个字符")
                                    .arg(tag)
                                    .arg(LoopStore::maxTagLength()));
            continue;
        }

        bool duplicate = false;
        for (int i = 0; i < m_tagList->count(); ++i)
        {
            if (m_tagList->item(i)->text().compare(tag, Qt::CaseInsensitive) == 0)
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            m_tagList->addItem(tag);
    }
    m_tagEdit->clear();
    if (m_hint)
        m_hint->clear();
}

void LoopClipDialog::removeSelectedTag()
{
    if (!m_tagList)
        return;
    const QList<QListWidgetItem*> selected = m_tagList->selectedItems();
    for (QListWidgetItem* item : selected)
        delete m_tagList->takeItem(m_tagList->row(item));
}

void LoopClipDialog::createFolder()
{
    if (!m_store)
        return;

    bool ok = false;
    const QString name = QInputDialog::getText(this, LT("新建循环收藏夹"),
                                               LT("名称"),
                                               QLineEdit::Normal, QString(), &ok);
    if (!ok)
        return;

    QString error;
    const QString id = m_store->createFolder(name, &error);
    if (id.isEmpty())
    {
        if (m_hint)
            m_hint->setText(error);
        return;
    }
    m_store->save();
    QSet<QString> checked = checkedFolderIds();
    checked.insert(id);
    refreshFolders(checked);
    if (m_hint)
        m_hint->clear();
}

bool LoopClipDialog::commitTimecodes()
{
    qint64 start = 0;
    qint64 end = 0;
    if (!LoopClipRules::parseTimecode(m_startEdit->text(), &start)
        || !LoopClipRules::parseTimecode(m_endEdit->text(), &end))
    {
        if (m_hint)
            m_hint->setText(LT("时间码格式应为 mm:ss.mmm，例如 01:23.500"));
        return false;
    }

    LoopClipRules::normalizeRange(&start, &end);
    const QString problem = LoopClipRules::validateRange(start, end, m_mediaDurationMs);
    if (!problem.isEmpty())
    {
        if (m_hint)
            m_hint->setText(problem);
        return false;
    }

    m_clip.startMs = start;
    m_clip.endMs = end;
    return true;
}

void LoopClipDialog::accept()
{
    if (!commitTimecodes())
        return;

    m_clip.title = m_titleEdit->text().trimmed();
    if (m_clip.title.isEmpty())
        m_clip.title = LoopClipRules::defaultTitle(m_clip.mediaPath, m_clip.startMs,
                                                   m_clip.endMs);

    QStringList tags;
    for (int i = 0; i < m_tagList->count(); ++i)
        tags.append(m_tagList->item(i)->text());
    m_clip.tags = tags;

    m_clip.folderIds = checkedFolderIds().values();
    m_clip.folderIds.sort();
    m_clip.note = m_noteEdit->toPlainText();

    QDialog::accept();
}
