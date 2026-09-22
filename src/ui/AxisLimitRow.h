#pragma once

#include "core/Types.h"

#include <QFrame>

class RangeSlider;
class QLabel;
class QSpinBox;
class QTimer;

// One axis on a single line: the limit slider scales the 0-100% script range
// onto the chosen percentage window. When a handle interaction ends (and also
// when a moved handle is let go) the row asks the device to move to that edge so
// the user can see where the limit points.
class AxisLimitRow : public QFrame
{
    Q_OBJECT

public:
    explicit AxisLimitRow(Track track, QWidget* parent = nullptr);

    Track track() const { return m_track; }

    void setAxisConfig(const AxisConfig& config);
    AxisConfig axisConfig() const;

    // Script position in percent; pass a negative value when no script is loaded.
    void setPosition(int posPercent);
    // Last previewed edge: 0 = lower limit, 100 = upper limit.
    int lastPreviewPercent() const { return m_previewPercent; }

signals:
    void configEdited(Track track, AxisConfig config);
    // Edge preview while dragging: 0 = lower limit, 100 = upper limit.
    void previewRequested(Track track, int scriptPercent);

protected:
    // Watches the two inputs so a focus change also selects which end of the
    // travel a later preview means.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    static int percentFromValue(int value);
    static int valueFromPercent(int percent);

    void applyInputsToSlider();
    void emitConfigEdited();
    void refreshPositionMarker();

    Track m_track;
    AxisConfig m_config;
    int m_positionPercent = -1;
    int m_previewPercent = 0;

    RangeSlider* m_slider = nullptr;
    QSpinBox* m_lowerSpin = nullptr;
    QSpinBox* m_upperSpin = nullptr;
    QLabel* m_positionLabel = nullptr;
    QTimer* m_previewTimer = nullptr;
};
