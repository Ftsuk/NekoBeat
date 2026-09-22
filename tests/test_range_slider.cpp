#include "ui/RangeSlider.h"

#include <QApplication>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QtTest>

#include <memory>

namespace
{
// QTest::mouseMove() carries no held button, which the slider legitimately
// ignores, so the drag cases hand-build their mouse events instead.
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

const QPoint kLeftEnd(0, 13);
const QPoint kRightEnd(299, 13);
const QPoint kMiddle(150, 13);

std::unique_ptr<RangeSlider> makeSlider()
{
    auto slider = std::make_unique<RangeSlider>();
    slider->setRange(0, 100);
    // The axis limit rows pin the gap to 2 %; the widget's default gap would
    // keep the two handles 100 units apart in this 0-100 domain.
    slider->setMinimumGap(2);
    slider->setValues(0, 100);
    slider->resize(300, 26);
    return slider;
}
} // namespace

// The axis limit rows move the device when the user lets go of a handle, so the
// slider has to name the touched edge even when the value does not change.
// Otherwise a limit that already spans 0 % - 100 % cannot move the device to
// either end by tapping its handle, which is exactly how the user checks travel.
class RangeSliderTest : public QObject
{
    Q_OBJECT

private slots:
    void tappingRestingLowerHandleReportsLower()
    {
        const auto slider = makeSlider();

        QSignalSpy spy(slider.get(), &RangeSlider::handleInteractionFinished);
        clickAt(slider.get(), kLeftEnd);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toBool(), false);
        // The limit already covered 0 %: the tap must not have moved it, it only
        // has to ask for a preview of that end.
        QCOMPARE(slider->lowerValue(), 0);
        QCOMPARE(slider->upperValue(), 100);
    }

    void tappingRestingUpperHandleReportsUpper()
    {
        const auto slider = makeSlider();

        QSignalSpy spy(slider.get(), &RangeSlider::handleInteractionFinished);
        clickAt(slider.get(), kRightEnd);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toBool(), true);
        QCOMPARE(slider->lowerValue(), 0);
        QCOMPARE(slider->upperValue(), 100);
    }

    void tappingTrackMovesNearestHandleAndReportsIt()
    {
        const auto slider = makeSlider();

        QSignalSpy spy(slider.get(), &RangeSlider::handleInteractionFinished);
        // Halfway sits the same distance from both handles; the lower one wins,
        // as before, and the report has to follow the handle that moved.
        clickAt(slider.get(), kMiddle);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toBool(), false);
        QCOMPARE(slider->lowerValue(), 50);
        QCOMPARE(slider->upperValue(), 100);
    }

    void draggingReportsOnlyOnceOnRelease()
    {
        const auto slider = makeSlider();

        QSignalSpy spy(slider.get(), &RangeSlider::handleInteractionFinished);
        sendMouse(slider.get(), QEvent::MouseButtonPress, kRightEnd, Qt::LeftButton,
                  Qt::LeftButton);
        sendMouse(slider.get(), QEvent::MouseMove, QPoint(220, 13), Qt::NoButton,
                  Qt::LeftButton);
        sendMouse(slider.get(), QEvent::MouseMove, QPoint(180, 13), Qt::NoButton,
                  Qt::LeftButton);
        QCOMPARE(spy.count(), 0); // Still dragging: the device must not move yet.
        sendMouse(slider.get(), QEvent::MouseButtonRelease, QPoint(180, 13), Qt::LeftButton,
                  Qt::NoButton);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toBool(), true);
        QVERIFY(slider->upperValue() < 100);
    }

    void unrelatedMouseButtonsAreIgnored()
    {
        const auto slider = makeSlider();

        QSignalSpy spy(slider.get(), &RangeSlider::handleInteractionFinished);
        sendMouse(slider.get(), QEvent::MouseButtonPress, kLeftEnd, Qt::RightButton,
                  Qt::RightButton);
        sendMouse(slider.get(), QEvent::MouseButtonRelease, kLeftEnd, Qt::RightButton,
                  Qt::NoButton);

        QCOMPARE(spy.count(), 0);
    }

    void disabledSliderStaysSilent()
    {
        const auto slider = makeSlider();
        slider->setEnabled(false);

        QSignalSpy spy(slider.get(), &RangeSlider::handleInteractionFinished);
        clickAt(slider.get(), kLeftEnd);

        QCOMPARE(spy.count(), 0);
    }
};

QTEST_MAIN(RangeSliderTest)
#include "test_range_slider.moc"
