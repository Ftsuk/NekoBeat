#include "core/Types.h"
#include "ui/HeatmapWidget.h"

#include <QApplication>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QtTest>

namespace
{
void sendMouse(QWidget* widget, QEvent::Type type, const QPoint& pos,
               Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent event(type, QPointF(pos), QPointF(widget->mapToGlobal(pos)), button,
                      buttons, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

ScriptBundle bundleForHeatmap()
{
    ScriptBundle bundle;
    Timeline timeline;
    timeline.track = Track::Stroke;
    timeline.actions = {{0, 0}, {1000, 100}, {2000, 0}, {3000, 100}};
    bundle.tracks.insert(static_cast<int>(Track::Stroke), timeline);
    return bundle;
}
} // namespace

// Dragging the heat map previews the picture and holds the machine still, so the
// widget has to announce the three phases separately: press, real move, release.
class HeatmapScrubTest : public QObject
{
    Q_OBJECT

private slots:
    void clickAnnouncesPressAndReleaseOnly()
    {
        HeatmapWidget heatmap;
        heatmap.resize(400, 36);
        heatmap.setBundle(bundleForHeatmap(), 4000);

        QSignalSpy started(&heatmap, &HeatmapWidget::scrubStarted);
        QSignalSpy moved(&heatmap, &HeatmapWidget::scrubMoved);
        QSignalSpy finished(&heatmap, &HeatmapWidget::scrubFinished);
        QSignalSpy seeks(&heatmap, &HeatmapWidget::seekRequested);

        sendMouse(&heatmap, QEvent::MouseButtonPress, QPoint(200, 18), Qt::LeftButton,
                  Qt::LeftButton);
        sendMouse(&heatmap, QEvent::MouseButtonRelease, QPoint(200, 18), Qt::LeftButton,
                  Qt::NoButton);

        QCOMPARE(started.count(), 1);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(moved.count(), 0); // A click must not hold the machine still.
        QCOMPARE(seeks.count(), 1);
    }

    // Tremor around the press point stays a click.
    void smallJitterIsNotADrag()
    {
        HeatmapWidget heatmap;
        heatmap.resize(400, 36);
        heatmap.setBundle(bundleForHeatmap(), 4000);

        QSignalSpy moved(&heatmap, &HeatmapWidget::scrubMoved);

        sendMouse(&heatmap, QEvent::MouseButtonPress, QPoint(200, 18), Qt::LeftButton,
                  Qt::LeftButton);
        sendMouse(&heatmap, QEvent::MouseMove, QPoint(202, 18), Qt::NoButton, Qt::LeftButton);
        sendMouse(&heatmap, QEvent::MouseMove, QPoint(198, 18), Qt::NoButton, Qt::LeftButton);
        sendMouse(&heatmap, QEvent::MouseButtonRelease, QPoint(198, 18), Qt::LeftButton,
                  Qt::NoButton);

        QCOMPARE(moved.count(), 0);
    }

    void dragAnnouncesPressMoveAndReleaseOnce()
    {
        HeatmapWidget heatmap;
        heatmap.resize(400, 36);
        heatmap.setBundle(bundleForHeatmap(), 4000);

        QSignalSpy started(&heatmap, &HeatmapWidget::scrubStarted);
        QSignalSpy moved(&heatmap, &HeatmapWidget::scrubMoved);
        QSignalSpy finished(&heatmap, &HeatmapWidget::scrubFinished);
        QSignalSpy seeks(&heatmap, &HeatmapWidget::seekRequested);

        sendMouse(&heatmap, QEvent::MouseButtonPress, QPoint(100, 18), Qt::LeftButton,
                  Qt::LeftButton);
        for (int x = 120; x <= 300; x += 20)
            sendMouse(&heatmap, QEvent::MouseMove, QPoint(x, 18), Qt::NoButton,
                      Qt::LeftButton);
        sendMouse(&heatmap, QEvent::MouseButtonRelease, QPoint(300, 18), Qt::LeftButton,
                  Qt::NoButton);

        QCOMPARE(started.count(), 1);
        QCOMPARE(moved.count(), 1); // Announced once, not once per move.
        QCOMPARE(finished.count(), 1);
        // One request per pointer sample: the window is free to coalesce them.
        QCOMPARE(seeks.count(), 1 + 10);
    }
};

QTEST_MAIN(HeatmapScrubTest)

#include "test_heatmap_scrub.moc"
