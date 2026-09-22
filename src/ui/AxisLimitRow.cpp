#include "core/Loc.h"
#include "ui/AxisLimitRow.h"

#include "ui/RangeSlider.h"
#include "ui/Theme.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
constexpr int kDeviceMax = 9999;
constexpr int kMinGapPercent = 2;

QSpinBox* makePercentSpin(QWidget* parent)
{
    auto* spin = new QSpinBox(parent);
    spin->setRange(0, 100);
    spin->setSuffix(QStringLiteral("%"));
    spin->setKeyboardTracking(false);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setAlignment(Qt::AlignCenter);
    spin->setFixedWidth(68);
    return spin;
}
}

AxisLimitRow::AxisLimitRow(Track track, QWidget* parent)
    : QFrame(parent)
    , m_track(track)
{
    setFixedHeight(42);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(10);

    auto* dot = new QLabel(this);
    dot->setFixedSize(8, 8);
    dot->setStyleSheet(
        QStringLiteral("background: %1; border-radius: 4px;").arg(Theme::axisColor(track).name()));
    layout->addWidget(dot);

    auto* nameBox = new QWidget(this);
    nameBox->setFixedWidth(132);
    auto* nameLayout = new QHBoxLayout(nameBox);
    nameLayout->setContentsMargins(0, 0, 0, 0);
    nameLayout->setSpacing(6);
    auto* name = new QLabel(TrackInfo::displayName(track), nameBox);
    name->setObjectName(QStringLiteral("AxisName"));
    auto* english = new QLabel(TrackInfo::canonicalName(track), nameBox);
    english->setObjectName(QStringLiteral("AxisCode"));
    nameLayout->addWidget(name);
    nameLayout->addWidget(english);
    nameLayout->addStretch();
    nameBox->setToolTip(QStringLiteral("%1 %2（%3）")
                            .arg(TrackInfo::displayName(track),
                                 TrackInfo::canonicalName(track),
                                 TrackInfo::tcodeId(track)));
    layout->addWidget(nameBox);

    m_slider = new RangeSlider(this);
    m_slider->setRange(0, 100);
    m_slider->setMinimumGap(kMinGapPercent);
    m_slider->setAccentColor(Theme::axisColor(track));
    m_slider->setToolTip(LT("拖动两端设置行程比例，设备会跟随移动到限位位置"));
    m_slider->setMinimumWidth(150);
    layout->addWidget(m_slider, 1);

    m_lowerSpin = makePercentSpin(this);
    m_lowerSpin->setToolTip(LT("行程下限：脚本 0% 对应这里"));
    layout->addWidget(m_lowerSpin);

    auto* dash = new QLabel(QStringLiteral("–"), this);
    dash->setObjectName(QStringLiteral("Muted"));
    layout->addWidget(dash);

    m_upperSpin = makePercentSpin(this);
    m_upperSpin->setToolTip(LT("行程上限：脚本 100% 对应这里"));
    layout->addWidget(m_upperSpin);

    m_positionLabel = new QLabel(QStringLiteral("--"), this);
    m_positionLabel->setObjectName(QStringLiteral("AxisPosition"));
    m_positionLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_positionLabel->setFixedWidth(40);
    m_positionLabel->setToolTip(LT("当前脚本位置"));
    layout->addWidget(m_positionLabel);

    // Typing a value would otherwise move the device on every keystroke; wait
    // until the input settles before previewing. Created before the connections
    // below so the handlers can always rely on it.
    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    m_previewTimer->setInterval(400);
    connect(m_previewTimer, &QTimer::timeout, this, [this] {
        emit previewRequested(m_track, m_previewPercent);
    });

    connect(m_slider, &RangeSlider::rangeChanged, this, [this](int lower, int upper) {
        const QSignalBlocker lowerBlocker(m_lowerSpin);
        const QSignalBlocker upperBlocker(m_upperSpin);
        m_lowerSpin->setValue(lower);
        m_upperSpin->setValue(upper);
        refreshPositionMarker();
    });
    // The device is moved once, when the interaction ends: following the handle
    // while it is still moving would send a stream of reversing commands to the
    // hardware. The slider names the edge the user touched, and it reports it
    // even when the value did not change — tapping the 0 % / 100 % handle of a
    // limit that already spans the full travel has to move the device there.
    connect(m_slider, &RangeSlider::handleInteractionFinished, this, [this](bool upper) {
        if (m_previewTimer)
            m_previewTimer->stop();
        m_previewPercent = upper ? 100 : 0;
        emitConfigEdited();
        emit previewRequested(m_track, m_previewPercent);
    });
    connect(m_lowerSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) {
        applyInputsToSlider();
        emitConfigEdited();
        m_previewPercent = 0;
        m_previewTimer->start();
    });
    connect(m_upperSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) {
        applyInputsToSlider();
        emitConfigEdited();
        m_previewPercent = 100;
        m_previewTimer->start();
    });
    connect(m_lowerSpin, &QSpinBox::editingFinished, this, [this] {
        m_previewTimer->stop();
        emit previewRequested(m_track, m_previewPercent);
    });
    connect(m_upperSpin, &QSpinBox::editingFinished, this, [this] {
        m_previewTimer->stop();
        emit previewRequested(m_track, m_previewPercent);
    });

    // Focusing an input also selects its edge, so ending the edit without
    // changing the number (click away, Enter on an unchanged value) still
    // previews the end the user is looking at instead of the previous one.
    m_lowerSpin->installEventFilter(this);
    m_upperSpin->installEventFilter(this);

    setAxisConfig(m_config);
}

bool AxisLimitRow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::FocusIn || event->type() == QEvent::MouseButtonPress)
    {
        if (watched == m_lowerSpin)
            m_previewPercent = 0;
        else if (watched == m_upperSpin)
            m_previewPercent = 100;
    }
    return QFrame::eventFilter(watched, event);
}

int AxisLimitRow::percentFromValue(int value)
{
    return std::clamp(qRound(std::clamp(value, 0, kDeviceMax) * 100.0 / kDeviceMax), 0, 100);
}

int AxisLimitRow::valueFromPercent(int percent)
{
    return qRound(std::clamp(percent, 0, 100) * kDeviceMax / 100.0);
}

void AxisLimitRow::setAxisConfig(const AxisConfig& config)
{
    m_config = config;
    int lower = percentFromValue(std::min(config.min, config.max));
    int upper = percentFromValue(std::max(config.min, config.max));
    if (upper < lower + kMinGapPercent)
        upper = std::min(100, lower + kMinGapPercent);

    const QSignalBlocker lowerBlocker(m_lowerSpin);
    const QSignalBlocker upperBlocker(m_upperSpin);
    m_lowerSpin->setValue(lower);
    m_upperSpin->setValue(upper);
    m_slider->setValues(lower, upper);
    refreshPositionMarker();
}

AxisConfig AxisLimitRow::axisConfig() const
{
    AxisConfig config = m_config;
    const int lowerPercent = m_lowerSpin->value();
    const int upperPercent = m_upperSpin->value();

    // The spin boxes are percent based, so converting them back would quantise
    // a calibrated travel (and overwrite a calibrated home with the middle).
    // As long as the sliders still sit on the stored values, the exact numbers
    // win; once the user moves a slider, the panel owns min/max again and the
    // home follows the new middle, exactly as before.
    if (lowerPercent != percentFromValue(m_config.min)
        || upperPercent != percentFromValue(m_config.max))
    {
        config.min = valueFromPercent(lowerPercent);
        config.max = valueFromPercent(upperPercent);
        config.home = qRound((config.min + config.max) / 2.0);
    }
    return config;
}

void AxisLimitRow::setPosition(int posPercent)
{
    m_positionPercent = posPercent < 0 ? -1 : std::clamp(posPercent, 0, 100);
    m_positionLabel->setText(m_positionPercent < 0 ? QStringLiteral("--")
                                                   : QStringLiteral("%1%").arg(m_positionPercent));
    refreshPositionMarker();
}

void AxisLimitRow::refreshPositionMarker()
{
    if (m_positionPercent < 0)
    {
        m_slider->setFillValue(-1);
        return;
    }

    const int lower = m_lowerSpin->value();
    const int upper = m_upperSpin->value();
    m_slider->setFillValue(lower + qRound((upper - lower) * m_positionPercent / 100.0));
}

void AxisLimitRow::applyInputsToSlider()
{
    int lower = m_lowerSpin->value();
    int upper = m_upperSpin->value();
    if (upper < lower + kMinGapPercent)
    {
        upper = std::min(100, lower + kMinGapPercent);
        const QSignalBlocker blocker(m_upperSpin);
        m_upperSpin->setValue(upper);
    }
    m_slider->setValues(lower, upper);
    refreshPositionMarker();
}

void AxisLimitRow::emitConfigEdited()
{
    const AxisConfig next = axisConfig();
    const bool changed = next.min != m_config.min || next.max != m_config.max
                         || next.home != m_config.home;
    m_config = next;
    // Tapping a handle that cannot move (the travel already reaches that end)
    // only asks for a preview; it must not look like a limit edit and rewrite
    // the saved profile.
    if (changed)
        emit configEdited(m_track, m_config);
}
