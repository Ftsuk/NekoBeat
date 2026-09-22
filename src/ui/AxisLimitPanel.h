#pragma once

#include "core/AxisProfileStore.h"
#include "core/Types.h"

#include <QMap>
#include <QWidget>

#include <functional>

class AxisLimitRow;
class QComboBox;
class QLabel;
class QToolButton;
class QVBoxLayout;
class QHideEvent;

// Global axis limit panel. It lives in the menu bar and drops down as a popup,
// independent of the currently loaded media. Limits scale the 0-100% script
// range proportionally instead of cutting the motion short.
class AxisLimitPanel : public QWidget
{
    Q_OBJECT

public:
    explicit AxisLimitPanel(QWidget* parent = nullptr);

    void showAt(const QPoint& globalPos);
    void setAxisPosition(int track, int posPercent);
    int lastPreviewPercent(int track) const;
    void setResetEnabled(bool enabled);

    void migrateLegacy(const QMap<int, AxisConfig>& legacy);
    QMap<int, AxisConfig> activeConfigs() const;
    // Applies a config that came from outside (the calibration dialog): the
    // row, the draft and the saved profile all have to follow, and the track may
    // not be part of the current script at all.
    void applyAxisConfig(Track track, AxisConfig config);
    QString activeProfileName() const;
    void commitEdits();

signals:
    void axisConfigEdited(Track track, AxisConfig config);
    void previewRequested(Track track, int scriptPercent);
    void profileApplied(const QMap<int, AxisConfig>& configs);
    void homeRequested();

protected:
    void hideEvent(QHideEvent* event) override;

private:
    void buildUi();
    void runDialog(const std::function<void()>& task);
    void refreshProfileCombo();
    void applyProfileToRows();
    void updateDirtyState();
    void onProfileSelected(int index);
    void createProfile();
    void saveProfileAs();
    void renameProfile();
    void removeProfile();
    void resetLimits();
    void exportProfiles();
    void importProfiles();

    AxisProfileStore m_store;
    QMap<int, AxisConfig> m_draft;
    QMap<int, AxisLimitRow*> m_rows;

    QComboBox* m_profileCombo = nullptr;
    QToolButton* m_menuButton = nullptr;
    QToolButton* m_resetButton = nullptr;
    QLabel* m_dirtyLabel = nullptr;
    bool m_updating = false;
    bool m_dirty = false;
    bool m_inDialog = false;
};
