#include "core/Loc.h"
#include "ui/CalibrationDialog.h"

#include "core/AppLogger.h"
#include "device/IDeviceControl.h"
#include "device/TCodeEncoder.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

CalibrationDialog::CalibrationDialog(IDeviceControl* device,
                                     const ScriptBundle& bundle,
                                     QWidget* parent)
    : QDialog(parent)
    , m_device(device)
    , m_bundle(bundle)
{
    setWindowTitle(LT("轴校准与测试"));
    resize(460, 360);

    auto* root = new QVBoxLayout(this);

    auto* warning = new QLabel(
        LT("测试按钮会实际驱动设备。请确认设备周围安全，并保持可以随时断电。"),
        this);
    warning->setWordWrap(true);
    warning->setStyleSheet(QStringLiteral("color: #ffb020;"));
    root->addWidget(warning);

    auto* form = new QFormLayout();
    m_axisCombo = new QComboBox(this);
    // The axes of the loaded script, plus every axis the device already
    // reported: calibrating must not require opening a video first.
    const DeviceCapabilities capabilities =
        m_device ? m_device->capabilities() : DeviceCapabilities{};
    for (Track track : TrackInfo::sr6Tracks())
    {
        const bool inScript = m_bundle.tracks.contains(static_cast<int>(track));
        if (inScript || !capabilities.probeComplete
            || capabilities.supportedTracks.contains(static_cast<int>(track)))
            m_axisCombo->addItem(TrackInfo::displayName(track), static_cast<int>(track));
    }
    form->addRow(LT("轴"), m_axisCombo);

    m_minSpin = new QSpinBox(this);
    m_minSpin->setRange(0, 9999);
    form->addRow(LT("最小位置"), m_minSpin);

    m_maxSpin = new QSpinBox(this);
    m_maxSpin->setRange(0, 9999);
    form->addRow(LT("最大位置"), m_maxSpin);

    m_homeSpin = new QSpinBox(this);
    m_homeSpin->setRange(0, 9999);
    form->addRow(LT("中位/归位"), m_homeSpin);

    m_offsetSpin = new QSpinBox(this);
    m_offsetSpin->setRange(-1000, 1000);
    m_offsetSpin->setSuffix(QStringLiteral(" ms"));
    form->addRow(LT("脚本偏移"), m_offsetSpin);

    m_invertCheck = new QCheckBox(LT("反向"), this);
    form->addRow(QString(), m_invertCheck);
    root->addLayout(form);

    auto* testBox = new QGroupBox(LT("单轴测试"), this);
    auto* testColumn = new QVBoxLayout(testBox);
    auto* testLayout = new QHBoxLayout();
    m_lowButton = new QPushButton(LT("测试 0%"), testBox);
    m_highButton = new QPushButton(LT("测试 100%"), testBox);
    m_homeButton = new QPushButton(LT("回到中位"), testBox);
    testLayout->addWidget(m_lowButton);
    testLayout->addWidget(m_highButton);
    testLayout->addWidget(m_homeButton);
    testColumn->addLayout(testLayout);

    m_testHint = new QLabel(testBox);
    m_testHint->setWordWrap(true);
    m_testHint->setStyleSheet(QStringLiteral("color: #8a8a90;"));
    testColumn->addWidget(m_testHint);
    root->addWidget(testBox);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
    root->addWidget(buttons);

    connect(m_axisCombo, &QComboBox::currentIndexChanged, this, &CalibrationDialog::loadCurrentAxis);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &CalibrationDialog::applyCurrentAxis);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_lowButton, &QPushButton::clicked, this, &CalibrationDialog::testLow);
    connect(m_highButton, &QPushButton::clicked, this, &CalibrationDialog::testHigh);
    connect(m_homeButton, &QPushButton::clicked, this, &CalibrationDialog::testHome);

    if (m_device)
    {
        connect(m_device, &IDeviceControl::connectionChanged, this,
                [this](bool, const QString&) { refreshTestAvailability(); });
    }

    loadCurrentAxis();
    refreshTestAvailability();
}

void CalibrationDialog::refreshTestAvailability()
{
    const bool connected = m_device && m_device->isConnected();
    for (QPushButton* button : {m_lowButton, m_highButton, m_homeButton})
    {
        if (!button)
            continue;
        button->setEnabled(connected);
        button->setToolTip(connected
                               ? LT("会真实驱动设备，请确认周围安全")
                               : LT("连接设备后才能试位"));
    }
    if (m_testHint)
    {
        m_testHint->setText(
            connected
                ? LT("测试按钮会按上面的行程直接驱动设备，用于目视确认方向与限位。")
                : LT("未连接设备：下面的数值可以现在填好并“应用”保存，连接设备后测试按钮会自动可用。"));
    }
}

Track CalibrationDialog::currentTrack() const
{
    if (m_axisCombo->count() == 0)
        return Track::None;
    return static_cast<Track>(m_axisCombo->currentData().toInt());
}

AxisConfig CalibrationDialog::currentConfig() const
{
    AxisConfig config;
    config.enabled = true;
    config.min = m_minSpin->value();
    config.max = m_maxSpin->value();
    config.home = m_homeSpin->value();
    config.inverted = m_invertCheck->isChecked();
    config.offsetMs = m_offsetSpin->value();
    return config;
}

void CalibrationDialog::loadCurrentAxis()
{
    const Track track = currentTrack();
    if (track == Track::None)
        return;
    AppLogger::log(QStringLiteral("calibration"),
                   QStringLiteral("选择轴: %1").arg(TrackInfo::tcodeId(track)));

    // A track that is not part of the current script starts from the defaults.
    const AxisConfig config =
        m_bundle.tracks.contains(static_cast<int>(track))
            ? m_bundle.tracks.value(static_cast<int>(track)).config
            : AxisConfig{};
    m_minSpin->setValue(config.min);
    m_maxSpin->setValue(config.max);
    m_homeSpin->setValue(config.home);
    m_invertCheck->setChecked(config.inverted);
    m_offsetSpin->setValue(static_cast<int>(config.offsetMs));
}

void CalibrationDialog::applyCurrentAxis()
{
    const Track track = currentTrack();
    if (track == Track::None)
        return;
    AxisConfig config = currentConfig();
    config.enabled = !m_bundle.tracks.contains(static_cast<int>(track))
                     || m_bundle.tracks.value(static_cast<int>(track)).config.enabled;
    AppLogger::log(QStringLiteral("calibration"),
                   QStringLiteral("应用轴 %1: min=%2 max=%3 home=%4 invert=%5 offset=%6")
                       .arg(TrackInfo::tcodeId(track))
                       .arg(config.min)
                       .arg(config.max)
                       .arg(config.home)
                       .arg(config.inverted)
                       .arg(config.offsetMs));
    emit axisConfigChanged(track, config);
}

void CalibrationDialog::sendTest(int posPercent)
{
    const Track track = currentTrack();
    if (track == Track::None || !m_device || !m_device->isConnected())
        return;

    QMap<int, AxisConfig> configs;
    configs.insert(static_cast<int>(track), currentConfig());
    const QString line = TCodeEncoder::encodeLine({{track, posPercent, 1500}}, configs);
    if (!line.isEmpty())
    {
        AppLogger::log(QStringLiteral("calibration"),
                       QStringLiteral("测试轴 %1 -> %2%: %3")
                           .arg(TrackInfo::tcodeId(track))
                           .arg(posPercent)
                           .arg(line));
        m_device->sendRawLine(line);
    }
}

void CalibrationDialog::testLow()
{
    sendTest(0);
}

void CalibrationDialog::testHigh()
{
    sendTest(100);
}

void CalibrationDialog::testHome()
{
    sendTest(50);
}
