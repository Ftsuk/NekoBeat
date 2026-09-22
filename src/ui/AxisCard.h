#pragma once

#include "core/Track.h"

#include <QFrame>

class QLabel;
class QProgressBar;
class QCheckBox;

class AxisCard : public QFrame
{
    Q_OBJECT

public:
    explicit AxisCard(Track track, QWidget* parent = nullptr);

    Track track() const { return m_track; }
    void setEnabled(bool enabled);
    bool isEnabled() const;
    void setPosition(int posPercent);
    void setAvailable(bool available);

signals:
    void enabledChanged(Track track, bool enabled);

private:
    Track m_track;
    QLabel* m_title = nullptr;
    QLabel* m_value = nullptr;
    QProgressBar* m_progress = nullptr;
    QCheckBox* m_enabled = nullptr;
};

