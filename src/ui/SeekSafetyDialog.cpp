#include "core/Loc.h"
#include "ui/SeekSafetyDialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace
{
constexpr int kSafeValue = 30;
constexpr int kStandardValue = 18;
constexpr int kFastValue = 8;

QString formatDuration(double seconds)
{
    return LT("%1 秒").arg(seconds, 0, 'f', 1);
}

QString presetText(const QString& title, const QString& description)
{
    return title + QLatin1Char('\n') + description;
}
}

SeekSafetyDialog::SeekSafetyDialog(int currentMsPerPercent, bool enabled,
                                   bool playPauseEasing, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(LT("跳转安全"));
    resize(560, 640);

    auto* root = new QVBoxLayout(this);

    // Master switch. Jumping across the timeline can make the device travel a
    // long way, so the easing is on by default; turning it off makes seeks and
    // video switches reach the target as fast as the script allows.
    m_useEasing = new QCheckBox(LT("启用跳转安全缓动（推荐）"), this);
    m_useEasing->setChecked(enabled);
    root->addWidget(m_useEasing);

    // The play/pause transition lives in this dialog too: it uses the very same
    // ramp, and this is where the ramp is explained.
    m_playPauseEasing = new QCheckBox(LT("播放 / 暂停也使用缓动"), this);
    m_playPauseEasing->setChecked(playPauseEasing);
    m_playPauseEasing->setToolTip(
        LT("只作用于播放/暂停这个动作（控制条按钮、Space、画面右键）：\n按下播放时设备按上面的速度档位缓动进入脚本；\n按下暂停时设备滑行到当前动作段的终点再停（不超过 0.6 秒）。"));
    root->addWidget(m_playPauseEasing);

    // State the whole picture here: there are three kinds of safety behaviour
    // and this switch only owns the first one, which is easy to misread.
    m_easingHint = new QLabel(
        LT("设备的安全移动一共分三类，本开关只控制第一类：<br><br><b>① 需要缓动（受本开关控制）</b><br>进度条跳转（播放中或暂停中）、媒体库连播切换视频、循环列表切换片段与循环回跳、从停止状态打开新视频、设备刚连接时接管联动。<br><br><b>② 固定时长（不受本开关影响）</b><br>播放结束回中约 1 秒、复位约 1.2 秒、轴限制拖动预览约 0.7 秒。<br><br><b>③ 播放 / 暂停（由上面的「播放 / 暂停也使用缓动」开关控制）</b><br>开启时，按下播放会按同样的斜坡缓动进入脚本；按下暂停时设备滑行到当前动作段的终点再停下（滑行时长按上面的速度档位计算，限制在 0.12–0.6 秒），不会停在半程，也不会反向。<br>关闭时：恢复播放直接到位，暂停立即发送 DSTOP。<br>无论开关如何，切换视频、终止、断开设备、退出程序、复位与急停都会立即停止。<br><br>关闭本开关后，①类场景会让设备按脚本时间尽快到达目标位置。"),
        this);
    m_easingHint->setWordWrap(true);
    m_easingHint->setTextFormat(Qt::RichText);
    m_easingHint->setStyleSheet(QStringLiteral("color:#8f9aba;"));
    root->addWidget(m_easingHint);

    m_presetGroup = new QGroupBox(LT("选择跳转速度"), this);
    auto* presetGroup = m_presetGroup;
    auto* presetLayout = new QVBoxLayout(presetGroup);

    m_safe = new QRadioButton(
        presetText(LT("安全（推荐）"),
                   LT("50% 行程约 %1，100% 行程约 %2")
                       .arg(formatDuration(1.5), formatDuration(3.0))),
        presetGroup);
    m_standard = new QRadioButton(
        presetText(LT("标准"),
                   LT("50% 行程约 %1，100% 行程约 %2")
                       .arg(formatDuration(0.9), formatDuration(1.8))),
        presetGroup);
    m_fast = new QRadioButton(
        presetText(LT("快速"),
                   LT("50% 行程约 %1，100% 行程约 %2")
                       .arg(formatDuration(0.4), formatDuration(0.8))),
        presetGroup);

    presetLayout->addWidget(m_safe);
    presetLayout->addWidget(m_standard);
    presetLayout->addWidget(m_fast);
    root->addWidget(presetGroup);

    m_advancedGroup =
        new QGroupBox(LT("高级自定义（仅供了解参数的用户）"), this);
    auto* advancedGroup = m_advancedGroup;
    auto* advancedLayout = new QVBoxLayout(advancedGroup);

    m_customEnabled = new QCheckBox(LT("使用自定义参数"), advancedGroup);
    advancedLayout->addWidget(m_customEnabled);

    auto* customRow = new QFormLayout();
    m_customValue = new QSpinBox(advancedGroup);
    m_customValue->setRange(5, 120);
    m_customValue->setSuffix(QStringLiteral(" ms / 1%"));
    customRow->addRow(LT("每 1% 行程耗时"), m_customValue);
    advancedLayout->addLayout(customRow);

    m_customDescription = new QLabel(advancedGroup);
    m_customDescription->setWordWrap(true);
    m_customDescription->setStyleSheet(QStringLiteral("color:#8f9aba;"));
    advancedLayout->addWidget(m_customDescription);
    root->addWidget(advancedGroup);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);

    auto* group = new QButtonGroup(this);
    group->addButton(m_safe);
    group->addButton(m_standard);
    group->addButton(m_fast);

    bool matchedPreset = false;
    if (currentMsPerPercent == kSafeValue)
    {
        m_safe->setChecked(true);
        matchedPreset = true;
    }
    else if (currentMsPerPercent == kStandardValue)
    {
        m_standard->setChecked(true);
        matchedPreset = true;
    }
    else if (currentMsPerPercent == kFastValue)
    {
        m_fast->setChecked(true);
        matchedPreset = true;
    }

    if (!matchedPreset)
    {
        m_customEnabled->setChecked(true);
        m_customValue->setValue(currentMsPerPercent);
        m_safe->setChecked(false);
        m_standard->setChecked(false);
        m_fast->setChecked(false);
    }
    else
    {
        m_customValue->setValue(currentMsPerPercent);
    }

    connect(m_customEnabled, &QCheckBox::toggled, this, &SeekSafetyDialog::updateEnabledState);
    connect(m_useEasing, &QCheckBox::toggled, this, &SeekSafetyDialog::updateEnabledState);
    connect(m_playPauseEasing, &QCheckBox::toggled, this, &SeekSafetyDialog::updateEnabledState);
    connect(m_customValue, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SeekSafetyDialog::updateCustomDescription);
    connect(m_safe, &QRadioButton::toggled, this, &SeekSafetyDialog::updateEnabledState);
    connect(m_standard, &QRadioButton::toggled, this, &SeekSafetyDialog::updateEnabledState);
    connect(m_fast, &QRadioButton::toggled, this, &SeekSafetyDialog::updateEnabledState);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    updateEnabledState();
    updateCustomDescription();
}

int SeekSafetyDialog::value() const
{
    if (m_customEnabled->isChecked())
        return m_customValue->value();
    if (m_fast->isChecked())
        return kFastValue;
    if (m_standard->isChecked())
        return kStandardValue;
    return kSafeValue;
}

bool SeekSafetyDialog::enabled() const
{
    return m_useEasing->isChecked();
}

bool SeekSafetyDialog::playPauseEasing() const
{
    return m_playPauseEasing->isChecked();
}

void SeekSafetyDialog::updateEnabledState()
{
    const bool easing = m_useEasing->isChecked();
    // The speed preset belongs to both switches: the play/pause transition uses
    // exactly the same ramp, so it has to stay adjustable while only that switch
    // is on.
    const bool anyEasing = easing || m_playPauseEasing->isChecked();
    m_presetGroup->setEnabled(anyEasing);
    m_advancedGroup->setEnabled(anyEasing);
    if (!anyEasing)
        return;

    const bool custom = m_customEnabled->isChecked();
    m_customValue->setEnabled(custom);
    m_safe->setEnabled(!custom);
    m_standard->setEnabled(!custom);
    m_fast->setEnabled(!custom);
}

void SeekSafetyDialog::updateCustomDescription()
{
    const int value = m_customValue->value();
    m_customDescription->setText(
        LT("当前约：50% 行程 %1，100% 行程 %2")
            .arg(formatDuration(value * 50 / 1000.0),
                 formatDuration(value * 100 / 1000.0)));
}
