#include "core/Loc.h"
#include "ui/AxisCard.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
QColor axisColor(Track track)
{
    switch (track)
    {
    case Track::Stroke: return QColor(0x29, 0x9D, 0xFF);
    case Track::Surge: return QColor(0xFF, 0x8A, 0x00);
    case Track::Sway: return QColor(0x2E, 0xC4, 0x4D);
    case Track::Twist: return QColor(0xFF, 0x3D, 0x8B);
    case Track::Roll: return QColor(0xB0, 0x6C, 0xFF);
    case Track::Pitch: return QColor(0xFF, 0xD5, 0x2E);
    case Track::Vib: return QColor(0x00, 0xD5, 0xD5);
    case Track::Lube: return QColor(0xFF, 0x4D, 0x4D);
    case Track::Suck: return QColor(0xFF, 0x9E, 0x40);
    case Track::SuckPosition: return QColor(0x9E, 0xB8, 0xFF);
    case Track::None: break;
    }
    return QColor(0xFF, 0x40, 0x81);
}
}

AxisCard::AxisCard(Track track, QWidget* parent)
    : QFrame(parent)
    , m_track(track)
{
    setFrameShape(QFrame::StyledPanel);
    setStyleSheet(QStringLiteral(
        "AxisCard { border: 1px solid #39415f; border-radius: 8px; background: #151b31; }"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(6);

    auto* header = new QHBoxLayout();
    m_title = new QLabel(TrackInfo::displayName(track), this);
    m_title->setStyleSheet(QStringLiteral("font-weight: 600; color: #eef1ff;"));
    m_value = new QLabel(QStringLiteral("--"), this);
    m_value->setStyleSheet(QStringLiteral("color: #a9b3d6;"));
    header->addWidget(m_title);
    header->addStretch();
    header->addWidget(m_value);
    root->addLayout(header);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(50);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(10);
    m_progress->setStyleSheet(QStringLiteral(
        "QProgressBar { border: 1px solid #2b3555; border-radius: 4px; background: #0e1430; }"
        "QProgressBar::chunk { background: %1; border-radius: 3px; }")
                                  .arg(axisColor(track).name()));
    root->addWidget(m_progress);

    m_enabled = new QCheckBox(LT("启用"), this);
    m_enabled->setChecked(true);
    root->addWidget(m_enabled);

    connect(m_enabled, &QCheckBox::toggled, this, [this](bool checked) {
        emit enabledChanged(m_track, checked);
    });
}

void AxisCard::setEnabled(bool enabled)
{
    m_enabled->setChecked(enabled);
}

bool AxisCard::isEnabled() const
{
    return m_enabled->isChecked();
}

void AxisCard::setPosition(int posPercent)
{
    const int value = std::clamp(posPercent, 0, 100);
    m_progress->setValue(value);
    m_value->setText(QStringLiteral("%1%").arg(value));
}

void AxisCard::setAvailable(bool available)
{
    setVisible(available);
}
