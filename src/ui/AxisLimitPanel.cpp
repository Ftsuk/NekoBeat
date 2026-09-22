#include "core/Loc.h"
#include "ui/AxisLimitPanel.h"

#include "core/AppLogger.h"
#include "ui/AxisLimitRow.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

namespace
{
QFrame* makeSeparator(QWidget* parent)
{
    auto* line = new QFrame(parent);
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral("background: #E5E5EA; border: none;"));
    return line;
}
}

AxisLimitPanel::AxisLimitPanel(QWidget* parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    buildUi();
    m_store.ensureDefaultProfile();
    refreshProfileCombo();
    m_draft = m_store.activeProfile().axes;
    applyProfileToRows();
}

void AxisLimitPanel::buildUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 12, 14, 16);
    outer->setSpacing(0);

    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("PopupCard"));
    auto* shadow = new QGraphicsDropShadowEffect(card);
    shadow->setBlurRadius(30.0);
    shadow->setOffset(0.0, 8.0);
    shadow->setColor(QColor(0, 0, 0, 46));
    card->setGraphicsEffect(shadow);
    outer->addWidget(card);

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(12);

    auto* headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(8);
    auto* titleColumn = new QVBoxLayout();
    titleColumn->setContentsMargins(0, 0, 0, 0);
    titleColumn->setSpacing(1);
    auto* title = new QLabel(LT("轴限制"), card);
    title->setObjectName(QStringLiteral("PopupTitle"));
    auto* subtitle = new QLabel(
        LT("全局生效 · 0–100% 按比例缩放行程，不会截断动作"), card);
    subtitle->setObjectName(QStringLiteral("PopupSubtitle"));
    titleColumn->addWidget(title);
    titleColumn->addWidget(subtitle);
    headerRow->addLayout(titleColumn);
    headerRow->addStretch();
    m_dirtyLabel = new QLabel(LT("未保存"), card);
    m_dirtyLabel->setObjectName(QStringLiteral("PopupSubtitle"));
    m_dirtyLabel->setVisible(false);
    headerRow->addWidget(m_dirtyLabel, 0, Qt::AlignTop);
    layout->addLayout(headerRow);
    layout->addWidget(makeSeparator(card));

    for (Track track : TrackInfo::sr6Tracks())
    {
        auto* row = new AxisLimitRow(track, card);
        connect(row, &AxisLimitRow::configEdited, this, [this](Track editedTrack, AxisConfig config) {
            if (m_updating)
                return;
            m_draft.insert(static_cast<int>(editedTrack), config);
            m_dirty = true;
            updateDirtyState();
            emit axisConfigEdited(editedTrack, config);
            // Persist right away so the active profile always survives a restart.
            commitEdits();
        });
        connect(row, &AxisLimitRow::previewRequested, this,
                [this](Track editedTrack, int scriptPercent) {
            if (m_updating)
                return;
            emit previewRequested(editedTrack, scriptPercent);
        });
        m_rows.insert(static_cast<int>(track), row);
        layout->addWidget(row);
    }

    layout->addWidget(makeSeparator(card));

    auto* footer = new QHBoxLayout();
    footer->setContentsMargins(0, 0, 0, 0);
    footer->setSpacing(8);
    auto* profileLabel = new QLabel(LT("配置"), card);
    profileLabel->setObjectName(QStringLiteral("Muted"));
    footer->addWidget(profileLabel);

    m_profileCombo = new QComboBox(card);
    m_profileCombo->setMinimumWidth(150);
    m_profileCombo->setToolTip(LT("切换已保存的轴限制配置"));
    footer->addWidget(m_profileCombo);

    auto* addButton = new QToolButton(card);
    addButton->setObjectName(QStringLiteral("Compact"));
    addButton->setText(LT("新建"));
    addButton->setToolTip(LT("新建配置（复制当前限位）"));
    addButton->setCursor(Qt::PointingHandCursor);
    footer->addWidget(addButton);

    m_menuButton = new QToolButton(card);
    m_menuButton->setObjectName(QStringLiteral("Compact"));
    m_menuButton->setText(LT("更多"));
    m_menuButton->setToolTip(LT("配置档管理"));
    m_menuButton->setCursor(Qt::PointingHandCursor);
    footer->addWidget(m_menuButton);
    footer->addStretch();

    m_resetButton = new QToolButton(card);
    m_resetButton->setObjectName(QStringLiteral("Compact"));
    m_resetButton->setText(LT("复位"));
    m_resetButton->setToolTip(LT("所有轴回到行程中点"));
    m_resetButton->setCursor(Qt::PointingHandCursor);
    footer->addWidget(m_resetButton);
    layout->addLayout(footer);

    auto* menu = new QMenu(this);
    menu->addAction(LT("保存到当前配置"), this, &AxisLimitPanel::commitEdits);
    menu->addSeparator();
    menu->addAction(LT("另存为…"), this, &AxisLimitPanel::saveProfileAs);
    menu->addAction(LT("重命名…"), this, &AxisLimitPanel::renameProfile);
    menu->addAction(LT("删除配置"), this, &AxisLimitPanel::removeProfile);
    menu->addSeparator();
    menu->addAction(LT("全部恢复默认限位"), this, &AxisLimitPanel::resetLimits);
    menu->addSeparator();
    menu->addAction(LT("导入配置…"), this, &AxisLimitPanel::importProfiles);
    menu->addAction(LT("导出配置…"), this, &AxisLimitPanel::exportProfiles);
    m_menuButton->setPopupMode(QToolButton::InstantPopup);
    m_menuButton->setArrowType(Qt::NoArrow);
    m_menuButton->setMenu(menu);

    connect(m_profileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AxisLimitPanel::onProfileSelected);
    connect(addButton, &QToolButton::clicked, this, &AxisLimitPanel::createProfile);
    connect(m_resetButton, &QToolButton::clicked, this, [this] { emit homeRequested(); });

    setFixedWidth(690);
}

void AxisLimitPanel::showAt(const QPoint& globalPos)
{
    adjustSize();
    move(globalPos);
    show();
    raise();
}

void AxisLimitPanel::hideEvent(QHideEvent* event)
{
    if (!m_inDialog)
        commitEdits();
    QWidget::hideEvent(event);
}

void AxisLimitPanel::setResetEnabled(bool enabled)
{
    if (!m_resetButton)
        return;
    m_resetButton->setEnabled(enabled);
    m_resetButton->setToolTip(enabled ? LT("所有轴回到行程中点")
                                      : LT("播放联动中不可复位"));
}

void AxisLimitPanel::runDialog(const std::function<void()>& task)
{
    const QPoint pos = frameGeometry().topLeft();
    m_inDialog = true;
    // A Qt::Popup hides itself as soon as a child dialog takes the grab, which
    // looked like the panel was closing. Temporarily behave like a tool window
    // so the dialog stacks on top and the panel stays visible.
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    move(pos);
    show();
    task();
    m_inDialog = false;
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    move(pos);
    show();
    raise();
}

void AxisLimitPanel::applyProfileToRows()
{
    const bool wasUpdating = m_updating;
    m_updating = true;
    for (auto it = m_rows.constBegin(); it != m_rows.constEnd(); ++it)
    {
        AxisConfig config = m_draft.value(it.key());
        it.value()->setAxisConfig(config);
    }
    m_updating = wasUpdating;
}

void AxisLimitPanel::setAxisPosition(int track, int posPercent)
{
    if (m_rows.contains(track))
        m_rows.value(track)->setPosition(posPercent);
}

int AxisLimitPanel::lastPreviewPercent(int track) const
{
    return m_rows.contains(track) ? m_rows.value(track)->lastPreviewPercent() : 0;
}

QMap<int, AxisConfig> AxisLimitPanel::activeConfigs() const
{
    QMap<int, AxisConfig> configs;
    for (auto it = m_rows.constBegin(); it != m_rows.constEnd(); ++it)
        configs.insert(it.key(), it.value()->axisConfig());
    return configs;
}

void AxisLimitPanel::applyAxisConfig(Track track, AxisConfig config)
{
    const int key = static_cast<int>(track);
    if (const auto row = m_rows.constFind(key); row != m_rows.constEnd())
    {
        QSignalBlocker blocker(row.value());
        row.value()->setAxisConfig(config);
    }

    m_draft.insert(key, config);
    m_dirty = true;
    updateDirtyState();
    // Persist right away, exactly like a slider edit does.
    commitEdits();
}

QString AxisLimitPanel::activeProfileName() const
{
    return m_store.activeName();
}

void AxisLimitPanel::migrateLegacy(const QMap<int, AxisConfig>& legacy)
{
    m_store.migrateLegacy(legacy);
    m_draft = m_store.activeProfile().axes;
    refreshProfileCombo();
    applyProfileToRows();
}

void AxisLimitPanel::commitEdits()
{
    if (!m_dirty)
        return;

    AxisProfile profile;
    profile.name = m_store.activeName();
    profile.axes = activeConfigs();
    m_store.saveProfile(profile);
    m_dirty = false;
    updateDirtyState();
}

void AxisLimitPanel::updateDirtyState()
{
    if (m_dirtyLabel)
        m_dirtyLabel->setVisible(m_dirty);
}

void AxisLimitPanel::refreshProfileCombo()
{
    const QSignalBlocker blocker(m_profileCombo);
    m_profileCombo->clear();
    for (const QString& name : m_store.profileNames())
        m_profileCombo->addItem(name);
    const int index = m_profileCombo->findText(m_store.activeName());
    m_profileCombo->setCurrentIndex(index >= 0 ? index : 0);
}

void AxisLimitPanel::onProfileSelected(int index)
{
    if (m_updating || index < 0)
        return;
    const QString name = m_profileCombo->itemText(index);
    if (name.compare(m_store.activeName(), Qt::CaseInsensitive) == 0)
        return;

    commitEdits();
    if (!m_store.setActiveName(name))
        return;

    m_draft = m_store.activeProfile().axes;
    m_dirty = false;
    updateDirtyState();
    applyProfileToRows();
    AppLogger::log(QStringLiteral("ui"),
                   QStringLiteral("切换轴限制配置: %1").arg(name));
    emit profileApplied(activeConfigs());
}

void AxisLimitPanel::createProfile()
{
    runDialog([this] {
    bool ok = false;
    const QString suggestion = LT("配置 %1").arg(m_store.profileNames().size() + 1);
    const QString name = QInputDialog::getText(this, LT("新建配置"),
                                               LT("配置名称"), QLineEdit::Normal,
                                               suggestion, &ok)
                             .trimmed();
    if (!ok || name.isEmpty())
        return;
    if (m_store.contains(name))
    {
        QMessageBox::warning(this, LT("名称已存在"),
                             LT("配置“%1”已存在，请换一个名称。").arg(name));
        return;
    }

    AxisProfile profile;
    profile.name = name;
    profile.axes = activeConfigs();
    m_store.saveProfile(profile);
    m_store.setActiveName(name);
    m_draft = profile.axes;
    m_dirty = false;
    refreshProfileCombo();
    updateDirtyState();
    QMessageBox::information(this, LT("已新建配置"),
                             LT("当前限位已保存为“%1”。").arg(name));
    });
}

void AxisLimitPanel::saveProfileAs()
{
    runDialog([this] {
    bool ok = false;
    const QString name = QInputDialog::getText(this, LT("另存为"),
                                               LT("新配置名称"), QLineEdit::Normal,
                                               m_store.activeName() + LT(" 副本"), &ok)
                             .trimmed();
    if (!ok || name.isEmpty())
        return;
    if (m_store.contains(name)
        && QMessageBox::question(this, LT("覆盖配置"),
                                 LT("配置“%1”已存在，是否覆盖？").arg(name))
               != QMessageBox::Yes)
        return;

    AxisProfile profile;
    profile.name = name;
    profile.axes = activeConfigs();
    m_store.saveProfile(profile);
    m_store.setActiveName(name);
    m_draft = profile.axes;
    m_dirty = false;
    refreshProfileCombo();
    updateDirtyState();
    });
}

void AxisLimitPanel::renameProfile()
{
    runDialog([this] {
    const QString current = m_store.activeName();
    bool ok = false;
    const QString name = QInputDialog::getText(this, LT("重命名配置"),
                                               LT("新名称"), QLineEdit::Normal,
                                               current, &ok)
                             .trimmed();
    if (!ok || name.isEmpty() || name == current)
        return;
    if (!m_store.renameProfile(current, name))
    {
        QMessageBox::warning(this, LT("重命名失败"),
                             LT("名称为空或与现有配置重复。"));
        return;
    }
    refreshProfileCombo();
    });
}

void AxisLimitPanel::removeProfile()
{
    runDialog([this] {
    if (m_store.profileNames().size() <= 1)
    {
        QMessageBox::information(this, LT("无法删除"),
                                 LT("至少需要保留一个配置。"));
        return;
    }
    const QString name = m_store.activeName();
    if (QMessageBox::question(this, LT("删除配置"),
                              LT("确定删除配置“%1”吗？").arg(name))
        != QMessageBox::Yes)
        return;
    if (!m_store.removeProfile(name))
        return;

    m_draft = m_store.activeProfile().axes;
    m_dirty = false;
    refreshProfileCombo();
    updateDirtyState();
    applyProfileToRows();
    emit profileApplied(activeConfigs());
    });
}

void AxisLimitPanel::resetLimits()
{
    m_draft.clear();
    for (auto it = m_rows.constBegin(); it != m_rows.constEnd(); ++it)
    {
        AxisConfig config;
        config.enabled = it.value()->axisConfig().enabled;
        m_draft.insert(it.key(), config);
    }
    m_updating = true;
    applyProfileToRows();
    m_updating = false;
    m_dirty = true;
    updateDirtyState();
    commitEdits();
    emit profileApplied(activeConfigs());
}

void AxisLimitPanel::exportProfiles()
{
    runDialog([this] {
    const QString path = QFileDialog::getSaveFileName(
        this, LT("导出轴限制配置"), LT("NekoBeat-轴限制配置.json"),
        LT("JSON 配置 (*.json)"));
    if (path.isEmpty())
        return;

    commitEdits();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        QMessageBox::warning(this, LT("导出失败"), file.errorString());
        return;
    }
    file.write(QJsonDocument(m_store.toJson()).toJson(QJsonDocument::Indented));
    AppLogger::log(QStringLiteral("ui"), QStringLiteral("导出轴限制配置: %1").arg(path));
    });
}

void AxisLimitPanel::importProfiles()
{
    runDialog([this] {
    const QString path = QFileDialog::getOpenFileName(
        this, LT("导入轴限制配置"), QString(),
        LT("JSON 配置 (*.json)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        QMessageBox::warning(this, LT("导入失败"), file.errorString());
        return;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        QMessageBox::warning(this, LT("导入失败"),
                             LT("文件不是有效的 NekoBeat 配置：%1")
                                 .arg(error.errorString()));
        return;
    }

    const QStringList imported = m_store.mergeJson(document.object());
    if (imported.isEmpty())
    {
        QMessageBox::warning(this, LT("导入失败"),
                             LT("文件里没有可用的配置。"));
        return;
    }
    m_draft = m_store.activeProfile().axes;
    m_dirty = false;
    refreshProfileCombo();
    applyProfileToRows();
    emit profileApplied(activeConfigs());
    QMessageBox::information(this, LT("导入完成"),
                             LT("已导入 %1 个配置：%2")
                                 .arg(imported.size())
                                 .arg(imported.join(QStringLiteral("、"))));
    });
}
