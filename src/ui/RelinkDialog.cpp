#include "core/Loc.h"
#include "ui/RelinkDialog.h"

#include "core/MediaRelink.h"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace
{
RelinkMediaInfo toInfo(const MediaItem& item)
{
    RelinkMediaInfo info;
    info.path = item.path;
    info.displayName = item.displayName.isEmpty() ? QFileInfo(item.path).fileName()
                                                  : item.displayName;
    info.fileSize = item.fileSize;
    info.modifiedMs =
        item.modified.isValid() ? item.modified.toMSecsSinceEpoch() : -1;
    info.durationMs = item.durationMs;
    return info;
}

QString describe(const RelinkCandidate& candidate)
{
    QStringList bits;
    bits.append(LT("同名"));
    if (candidate.sameSize)
        bits.append(LT("同大小"));
    if (candidate.sameModified)
        bits.append(LT("同修改时间"));
    if (candidate.sameDuration)
        bits.append(LT("同时长"));
    return bits.join(QStringLiteral(" · "));
}
}

RelinkDialog::RelinkDialog(QWidget* parent, const QList<MediaItem>& missing,
                           const QList<MediaItem>& available)
    : QDialog(parent)
{
    setWindowTitle(LT("重新关联失效条目"));
    resize(880, 460);

    auto* layout = new QVBoxLayout(this);

    auto* hint = new QLabel(
        LT("下面的条目指向的文件已经不在了。挑出它们的新位置后，收藏夹、循环片段和标签都会一起跟过去。\n只会勾选证据充分的匹配（同大小、同修改时间或同时长），仅同名的需要你自己确认。"),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({LT("失效条目"),
                             LT("重新关联到"),
                             LT("依据")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    layout->addWidget(m_tree, 1);

    QList<RelinkMediaInfo> missingInfo;
    missingInfo.reserve(missing.size());
    for (const MediaItem& item : missing)
        missingInfo.append(toInfo(item));

    QList<RelinkMediaInfo> availableInfo;
    availableInfo.reserve(available.size());
    for (const MediaItem& item : available)
    {
        if (!item.missing)
            availableInfo.append(toInfo(item));
    }

    const QList<RelinkSuggestion> suggestions =
        MediaRelink::suggest(missingInfo, availableInfo);

    for (const RelinkSuggestion& suggestion : suggestions)
    {
        auto* row = new QTreeWidgetItem(m_tree);
        row->setText(0, suggestion.missingName.isEmpty()
                            ? QFileInfo(suggestion.missingPath).fileName()
                            : suggestion.missingName);
        row->setToolTip(0, QDir::toNativeSeparators(suggestion.missingPath));

        Row entry;
        entry.missingPath = suggestion.missingPath;

        if (!suggestion.hasCandidate())
        {
            row->setText(1, LT("没有找到同名文件"));
            row->setText(2, QStringLiteral("—"));
            row->setFlags(row->flags() & ~Qt::ItemIsUserCheckable);
            m_rows.append(entry);
            continue;
        }

        // Only a confident, unambiguous match starts ticked; everything else is
        // offered but left for the user to switch on.
        row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
        row->setCheckState(0, suggestion.confident() ? Qt::Checked : Qt::Unchecked);

        auto* chooser = new QComboBox(m_tree);
        for (const RelinkCandidate& candidate : suggestion.candidates)
        {
            chooser->addItem(
                QStringLiteral("%1 — %2")
                    .arg(candidate.displayName,
                         QDir::toNativeSeparators(
                             QFileInfo(candidate.path).absolutePath())),
                candidate.path);
        }
        chooser->setToolTip(QDir::toNativeSeparators(
            suggestion.candidates.first().path));
        m_tree->setItemWidget(row, 1, chooser);
        entry.chooser = chooser;

        if (suggestion.ambiguous)
            row->setText(2, LT("多个同名候选，请选择"));
        else
            row->setText(2, describe(suggestion.candidates.first()));

        m_rows.append(entry);
    }

    m_summary = new QLabel(this);
    layout->addWidget(m_summary);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* cancel = new QPushButton(LT("取消"), this);
    m_applyButton = new QPushButton(LT("重新关联"), this);
    m_applyButton->setDefault(true);
    buttons->addWidget(cancel);
    buttons->addWidget(m_applyButton);
    layout->addLayout(buttons);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_applyButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_tree, &QTreeWidget::itemChanged, this, [this]() { updateSummary(); });

    updateSummary();
}

QHash<QString, QString> RelinkDialog::mapping() const
{
    QHash<QString, QString> result;
    for (int i = 0; i < m_rows.size() && i < m_tree->topLevelItemCount(); ++i)
    {
        const Row& row = m_rows.at(i);
        if (!row.chooser)
            continue;

        QTreeWidgetItem* item = m_tree->topLevelItem(i);
        if (!item || item->checkState(0) != Qt::Checked)
            continue;

        auto* chooser = qobject_cast<QComboBox*>(row.chooser);
        if (!chooser || chooser->currentIndex() < 0)
            continue;

        result.insert(row.missingPath, chooser->currentData().toString());
    }
    return result;
}

void RelinkDialog::updateSummary()
{
    const int count = mapping().size();
    if (m_summary)
    {
        m_summary->setText(count > 0
                               ? LT("将重新关联 %1 条记录。").arg(count)
                               : LT("还没有勾选任何条目。"));
    }
    if (m_applyButton)
        m_applyButton->setEnabled(count > 0);
}
