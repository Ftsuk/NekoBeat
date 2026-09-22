#include "TestLanguage.h"
#include "core/Types.h"
#include "ui/AxisLimitRow.h"
#include "ui/RangeSlider.h"

#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QSpinBox>
#include <QtTest>

namespace
{
// QTest::mouseMove() carries no held button, which the slider legitimately
// ignores, so the cases hand-build their mouse events.
void sendMouse(QWidget* widget, QEvent::Type type, const QPoint& pos, Qt::MouseButton button,
               Qt::MouseButtons buttons)
{
    QMouseEvent event(type, QPointF(pos), QPointF(widget->mapToGlobal(pos)), button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

void clickAt(QWidget* widget, const QPoint& pos)
{
    sendMouse(widget, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
}

AxisConfig travel(int lowerPercent, int upperPercent)
{
    AxisConfig config;
    config.enabled = true;
    config.min = qRound(lowerPercent * 9999 / 100.0);
    config.max = qRound(upperPercent * 9999 / 100.0);
    config.home = qRound((config.min + config.max) / 2.0);
    return config;
}

// The row is built for a 690 px panel; laying the slider out by hand keeps the
// test off the screen while still giving it a real width to click on.
RangeSlider* sliderOf(AxisLimitRow& row)
{
    auto* slider = row.findChild<RangeSlider*>();
    if (slider)
        slider->resize(300, 26);
    return slider;
}
} // namespace

// Regression: the user checks travel by tapping the two ends of the limit
// slider. A limit that already spans 0 % - 100 % has nothing left to change, so
// the row used to stay silent and the device never moved; the tap has to ask for
// a preview of that end anyway.
class AxisLimitRowTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { qRegisterMetaType<Track>("Track"); }

    void tappingFullTravelLowerEndPreviewsZeroPercent()
    {
        AxisLimitRow row(Track::Stroke);
        row.setAxisConfig(travel(0, 100));
        RangeSlider* slider = sliderOf(row);
        QVERIFY(slider);

        QSignalSpy spy(&row, &AxisLimitRow::previewRequested);
        QSignalSpy editSpy(&row, &AxisLimitRow::configEdited);
        clickAt(slider, QPoint(0, 13));

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).value<Track>(), Track::Stroke);
        QCOMPARE(spy.first().at(1).toInt(), 0);
        // Tapping an end must not shrink the travel that was already full.
        QCOMPARE(row.axisConfig().min, 0);
        QCOMPARE(row.axisConfig().max, 9999);
        QCOMPARE(editSpy.count(), 0); // Nothing changed, so nothing to save.
    }

    void tappingFullTravelUpperEndPreviewsHundredPercent()
    {
        AxisLimitRow row(Track::Surge);
        row.setAxisConfig(travel(0, 100));
        RangeSlider* slider = sliderOf(row);
        QVERIFY(slider);

        QSignalSpy spy(&row, &AxisLimitRow::previewRequested);
        QSignalSpy editSpy(&row, &AxisLimitRow::configEdited);
        clickAt(slider, QPoint(299, 13));

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).value<Track>(), Track::Surge);
        QCOMPARE(spy.first().at(1).toInt(), 100);
        QCOMPARE(row.axisConfig().min, 0);
        QCOMPARE(row.axisConfig().max, 9999);
        QCOMPARE(editSpy.count(), 0);
    }

    void draggingAnEndPreviewsTheEndThatMoved()
    {
        AxisLimitRow row(Track::Sway);
        row.setAxisConfig(travel(20, 80));
        RangeSlider* slider = sliderOf(row);
        QVERIFY(slider);

        QSignalSpy spy(&row, &AxisLimitRow::previewRequested);
        QSignalSpy editSpy(&row, &AxisLimitRow::configEdited);
        // Grab the lower handle and pull it to the far left: the travel shrinks
        // to 0 % - 80 %, and the device has to be shown the new lower limit.
        sendMouse(slider, QEvent::MouseButtonPress, QPoint(57, 13), Qt::LeftButton,
                  Qt::LeftButton);
        sendMouse(slider, QEvent::MouseMove, QPoint(0, 13), Qt::NoButton, Qt::LeftButton);
        QCOMPARE(spy.count(), 0); // Still dragging: the device must not move yet.
        sendMouse(slider, QEvent::MouseButtonRelease, QPoint(0, 13), Qt::LeftButton, Qt::NoButton);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(1).toInt(), 0);
        QCOMPARE(row.axisConfig().min, 0);
        QVERIFY(row.axisConfig().max > 0);
        QCOMPARE(editSpy.count(), 1); // The travel really changed.
    }

    void typingTheSameUpperLimitStillPreviewsThatEnd()
    {
        AxisLimitRow row(Track::Twist);
        row.setAxisConfig(travel(0, 100));
        RangeSlider* slider = sliderOf(row);
        QVERIFY(slider);
        QSpinBox* upper = nullptr;
        for (QSpinBox* spin : row.findChildren<QSpinBox*>())
        {
            if (spin->toolTip().contains(QStringLiteral("上限")))
                upper = spin;
        }
        QVERIFY(upper);

        // Reproduce the panel state where the last preview was the lower end.
        QSignalSpy spy(&row, &AxisLimitRow::previewRequested);
        clickAt(slider, QPoint(0, 13));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(1).toInt(), 0);
        spy.clear();

        // Clicking into the upper field and confirming the unchanged number must
        // preview the upper end, not repeat the previous one.
        QFocusEvent focus(QEvent::FocusIn, Qt::OtherFocusReason);
        QApplication::sendEvent(upper, &focus);
        QKeyEvent confirm(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(upper, &confirm);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(1).toInt(), 100);
    }

    // Regression: the panel edits min/max as percentages, so reading the row
    // back used to quantise a calibrated travel and replace a calibrated home
    // with the middle of the range. Values that came from the calibration
    // dialog have to survive an untouched row.
    void calibratedValuesSurviveAnUntouchedRow()
    {
        AxisLimitRow row(Track::Stroke);
        AxisConfig calibrated;
        calibrated.enabled = true;
        calibrated.min = 7400;   // not a whole percentage of 9999
        calibrated.max = 9800;
        calibrated.home = 8500;  // deliberately not the middle (8600)
        calibrated.inverted = true;
        calibrated.offsetMs = 120;
        row.setAxisConfig(calibrated);

        const AxisConfig read = row.axisConfig();
        QCOMPARE(read.min, 7400);
        QCOMPARE(read.max, 9800);
        QCOMPARE(read.home, 8500);
        QVERIFY(read.inverted);
        QCOMPARE(read.offsetMs, 120);
    }

    // Once the user moves a slider the panel owns min/max again, and the home
    // follows the new middle - the behaviour the panel always had.
    void movingASliderRecomputesTheHome()
    {
        AxisLimitRow row(Track::Stroke);
        AxisConfig calibrated;
        calibrated.min = 0;
        calibrated.max = 9999;
        calibrated.home = 9000;
        row.setAxisConfig(calibrated);

        RangeSlider* slider = sliderOf(row);
        QVERIFY(slider);
        sendMouse(slider, QEvent::MouseButtonPress, QPoint(285, 13), Qt::LeftButton,
                  Qt::LeftButton);
        sendMouse(slider, QEvent::MouseMove, QPoint(150, 13), Qt::NoButton, Qt::LeftButton);
        sendMouse(slider, QEvent::MouseButtonRelease, QPoint(150, 13), Qt::LeftButton,
                  Qt::NoButton);

        const AxisConfig read = row.axisConfig();
        QVERIFY2(read.max < 9999, "拖动上限后行程应当变化");
        QCOMPARE(read.home, qRound((read.min + read.max) / 2.0));
    }
};

QTEST_MAIN(AxisLimitRowTest)
#include "test_axis_limit_row.moc"
