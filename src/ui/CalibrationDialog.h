#pragma once

#include "core/Types.h"

#include <QDialog>

class IDeviceControl;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

class CalibrationDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CalibrationDialog(IDeviceControl* device,
                               const ScriptBundle& bundle,
                               QWidget* parent = nullptr);

signals:
    void axisConfigChanged(Track track, AxisConfig config);

private slots:
    void loadCurrentAxis();
    void applyCurrentAxis();
    void testLow();
    void testHigh();
    void testHome();
    // The three test buttons drive the machine, so they only make sense with a
    // connected device. Everything else (min/max/home/invert/offset) can be
    // prepared without one.
    void refreshTestAvailability();

private:
    AxisConfig currentConfig() const;
    Track currentTrack() const;
    void sendTest(int posPercent);

    IDeviceControl* m_device = nullptr;
    ScriptBundle m_bundle;
    QComboBox* m_axisCombo = nullptr;
    QSpinBox* m_minSpin = nullptr;
    QSpinBox* m_maxSpin = nullptr;
    QSpinBox* m_homeSpin = nullptr;
    QSpinBox* m_offsetSpin = nullptr;
    QCheckBox* m_invertCheck = nullptr;
    QLabel* m_testHint = nullptr;
    QPushButton* m_lowButton = nullptr;
    QPushButton* m_highButton = nullptr;
    QPushButton* m_homeButton = nullptr;
};
