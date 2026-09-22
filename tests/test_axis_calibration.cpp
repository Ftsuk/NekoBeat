#include "TestLanguage.h"
#include "core/Types.h"
#include "device/IDeviceControl.h"
#include "ui/AxisLimitPanel.h"
#include "ui/CalibrationDialog.h"

#include <QComboBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>

namespace
{
// The dialog only needs the GUI-side view of the device: cached state plus a
// queued "send this line" request. No thread, no serial port.
class FakeControl : public IDeviceControl
{
public:
    bool isConnected() const override { return m_connected; }
    DeviceCapabilities capabilities() const override { return m_capabilities; }
    void sendRawLine(const QString& line) override { lines.append(line); }

    void setConnected(bool connected)
    {
        m_connected = connected;
        if (connected)
        {
            m_capabilities.valid = true;
            m_capabilities.probeComplete = true;
            for (Track track : TrackInfo::sr6Tracks())
                m_capabilities.supportedTracks.insert(static_cast<int>(track));
        }
        emit connectionChanged(connected,
                               connected ? QStringLiteral("已连接") : QStringLiteral("已断开"));
    }

    QStringList lines;

private:
    bool m_connected = false;
    DeviceCapabilities m_capabilities;
};

ScriptBundle bundleWithStroke()
{
    ScriptBundle bundle;
    Timeline timeline;
    timeline.track = Track::Stroke;
    timeline.actions = {{0, 0}, {1000, 100}};
    timeline.config.min = 1000;
    timeline.config.max = 9000;
    timeline.config.home = 6000;
    bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);
    return bundle;
}
} // namespace

// The calibration dialog had no entry point in the window; these cases pin the
// behaviour the menu action now relies on.
class AxisCalibrationTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<Track>("Track");
        qRegisterMetaType<AxisConfig>("AxisConfig");

        // Keep the profile store out of the user's real settings.
        QVERIFY(m_settingsDir.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("NekoBeatTest"));
        QCoreApplication::setApplicationName(QStringLiteral("NekoBeatTest"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
    }

    void dialogListsScriptAxesAndAppliesEdits()
    {
        FakeControl control;
        
        const ScriptBundle bundle = bundleWithStroke();

        CalibrationDialog dialog(&control, bundle);
        auto* combo = dialog.findChild<QComboBox*>();
        QVERIFY(combo);
        // Nothing is known about the device yet, so every axis is offered; the
        // script's own axis comes first because it is the one to calibrate.
        QCOMPARE(combo->count(), int(TrackInfo::sr6Tracks().size()));
        QCOMPARE(combo->currentData().toInt(), static_cast<int>(Track::Stroke));

        // The fields start from the script's own values.
        QList<QSpinBox*> spins = dialog.findChildren<QSpinBox*>();
        QCOMPARE(spins.size(), 4);
        QCOMPARE(spins.at(0)->value(), 1000); // min
        QCOMPARE(spins.at(1)->value(), 9000); // max
        QCOMPARE(spins.at(2)->value(), 6000); // home

        // Editing the home and pressing Apply must publish the new config.
        QSignalSpy spy(&dialog, &CalibrationDialog::axisConfigChanged);
        spins.at(2)->setValue(7500);
        QMetaObject::invokeMethod(&dialog, "applyCurrentAxis");

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).value<Track>(), Track::Stroke);
        const AxisConfig applied = spy.first().at(1).value<AxisConfig>();
        QCOMPARE(applied.home, 7500);
        QCOMPARE(applied.min, 1000);
        QCOMPARE(applied.max, 9000);
        QVERIFY(applied.enabled);
    }

    // A track that is not part of the loaded script still has to be testable,
    // which is what the "no video open yet" case looks like.
    void dialogFallsBackToDefaultsWithoutAScript()
    {
        FakeControl control;
        
        const ScriptBundle empty;

        CalibrationDialog dialog(&control, empty);
        auto* combo = dialog.findChild<QComboBox*>();
        QVERIFY(combo);

        // No probe result yet and no script: nothing to calibrate is a valid
        // state, but it must not crash or offer a bogus axis.
        QCOMPARE(combo->count(), TrackInfo::sr6Tracks().size());
    }

    // Without a device the dialog has to stay useful: the values can be edited
    // and applied, only the three "drive there" buttons are unavailable.
    void testButtonsWaitForADevice()
    {
        FakeControl control;
        
        const ScriptBundle bundle = bundleWithStroke();

        CalibrationDialog dialog(&control, bundle);
        const QList<QPushButton*> buttons = dialog.findChildren<QPushButton*>();
        QList<QPushButton*> testButtons;
        for (QPushButton* button : buttons)
        {
            if (button->text().startsWith(QStringLiteral("测试"))
                || button->text() == QStringLiteral("回到中位"))
            {
                testButtons.append(button);
            }
        }
        QCOMPARE(testButtons.size(), 3);
        for (QPushButton* button : testButtons)
            QVERIFY2(!button->isEnabled(), "未连接设备时试位按钮必须不可用");

        // The settings half stays usable, and applying still publishes.
        QSignalSpy spy(&dialog, &CalibrationDialog::axisConfigChanged);
        QMetaObject::invokeMethod(&dialog, "applyCurrentAxis");
        QCOMPARE(spy.count(), 1);

        // Connecting (a fake port is enough) has to enable them again.
        control.setConnected(true);
        for (QPushButton* button : testButtons)
            QVERIFY2(button->isEnabled(), "设备连接后试位按钮应当可用");
    }

    // The panel is the source of truth for limits: a config that arrives from
    // the dialog has to survive the round trip through the row untouched.
    void panelStoresCalibratedValuesExactly()
    {
        AxisLimitPanel panel;

        AxisConfig calibrated;
        calibrated.enabled = true;
        calibrated.min = 7400;
        calibrated.max = 9800;
        calibrated.home = 8500;
        calibrated.inverted = true;
        calibrated.offsetMs = 120;
        panel.applyAxisConfig(Track::Stroke, calibrated);

        const AxisConfig stored = panel.activeConfigs().value(static_cast<int>(Track::Stroke));
        QCOMPARE(stored.min, 7400);
        QCOMPARE(stored.max, 9800);
        QCOMPARE(stored.home, 8500);
        QVERIFY(stored.inverted);
        QCOMPARE(stored.offsetMs, 120);
    }

private:
    QTemporaryDir m_settingsDir;
};

QTEST_MAIN(AxisCalibrationTest)

#include "test_axis_calibration.moc"
