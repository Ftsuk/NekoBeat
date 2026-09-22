#include "media/MpvVideoWidget.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QtTest>

namespace
{
// Sends a mouse event straight to the widget, so the test does not need a
// window system window (and therefore no OpenGL context) to run.
void sendMouse(QWidget* widget, QEvent::Type type, Qt::MouseButton button)
{
    const QPointF local(10, 10);
    QMouseEvent event(type, local, local, button, button, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}
} // namespace

class VideoWidgetTest : public QObject
{
    Q_OBJECT

private slots:
    void rightClickTogglesPlayback()
    {
        MpvVideoWidget widget;
        QSignalSpy toggles(&widget, &MpvVideoWidget::rightClicked);
        QSignalSpy doubles(&widget, &MpvVideoWidget::doubleClicked);

        sendMouse(&widget, QEvent::MouseButtonPress, Qt::RightButton);
        sendMouse(&widget, QEvent::MouseButtonRelease, Qt::RightButton);

        QCOMPARE(toggles.count(), 1);
        QCOMPARE(doubles.count(), 0);
    }

    void leftClickDoesNotTogglePlayback()
    {
        MpvVideoWidget widget;
        QSignalSpy toggles(&widget, &MpvVideoWidget::rightClicked);

        sendMouse(&widget, QEvent::MouseButtonPress, Qt::LeftButton);
        sendMouse(&widget, QEvent::MouseButtonRelease, Qt::LeftButton);

        QCOMPARE(toggles.count(), 0);
    }

    void leftDoubleClickStillTogglesFullScreen()
    {
        MpvVideoWidget widget;
        QSignalSpy toggles(&widget, &MpvVideoWidget::rightClicked);
        QSignalSpy doubles(&widget, &MpvVideoWidget::doubleClicked);

        sendMouse(&widget, QEvent::MouseButtonDblClick, Qt::LeftButton);

        QCOMPARE(doubles.count(), 1);
        QCOMPARE(toggles.count(), 0);
    }

    // A right double click is two pause/resume gestures, never a full screen
    // switch: the two gestures must not fight over the same double click.
    void rightDoubleClickDoesNotToggleFullScreen()
    {
        MpvVideoWidget widget;
        QSignalSpy toggles(&widget, &MpvVideoWidget::rightClicked);
        QSignalSpy doubles(&widget, &MpvVideoWidget::doubleClicked);

        sendMouse(&widget, QEvent::MouseButtonPress, Qt::RightButton);
        sendMouse(&widget, QEvent::MouseButtonDblClick, Qt::RightButton);

        QCOMPARE(toggles.count(), 2);
        QCOMPARE(doubles.count(), 0);
    }

    void contextMenuRequestIsConsumed()
    {
        MpvVideoWidget widget;
        QContextMenuEvent event(QContextMenuEvent::Mouse, QPoint(10, 10), QPoint(10, 10));

        QApplication::sendEvent(&widget, &event);

        QVERIFY(event.isAccepted());
    }
};

QTEST_MAIN(VideoWidgetTest)
#include "test_video_widget.moc"
