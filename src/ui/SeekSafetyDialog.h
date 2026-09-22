#pragma once

#include <QDialog>

class QCheckBox;
class QGroupBox;
class QLabel;
class QRadioButton;
class QSpinBox;

class SeekSafetyDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SeekSafetyDialog(int currentMsPerPercent, bool enabled,
                              bool playPauseEasing,
                              QWidget* parent = nullptr);

    int value() const;
    bool enabled() const;
    // The play/pause transition has its own switch; it shares the speed preset
    // above but is set independently of the jump-safety master switch.
    bool playPauseEasing() const;

private:
    void updateEnabledState();
    void updateCustomDescription();

    QRadioButton* m_safe = nullptr;
    QRadioButton* m_standard = nullptr;
    QRadioButton* m_fast = nullptr;
    QCheckBox* m_useEasing = nullptr;
    QCheckBox* m_playPauseEasing = nullptr;
    QLabel* m_easingHint = nullptr;
    QCheckBox* m_customEnabled = nullptr;
    QSpinBox* m_customValue = nullptr;
    QLabel* m_customDescription = nullptr;
    QGroupBox* m_presetGroup = nullptr;
    QGroupBox* m_advancedGroup = nullptr;
};
