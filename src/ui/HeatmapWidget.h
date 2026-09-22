#pragma once

#include "core/Types.h"

#include <QVector>
#include <QWidget>

class QPainter;

// Thin strip under the player controls that visualises how intense the script
// is over time (like XTPlayer's heat map). Clicking seeks.
class HeatmapWidget : public QWidget
{
    Q_OBJECT

public:
    explicit HeatmapWidget(QWidget* parent = nullptr);

    void setBundle(const ScriptBundle& bundle, qint64 durationMs);
    void clear();
    void setPosition(qint64 positionMs);
    // Read-only A/B overlay used while the user marks a loop. startMs >= 0
    // paints the A marker on its own, endMs > startMs additionally paints the
    // highlighted band and the B marker. Dragging is deliberately not
    // supported: moving the playhead from the heat map also moves the device,
    // so the markers can only be changed with the A/B buttons or the dialog.
    void setLoopMarkers(qint64 startMs, qint64 endMs);
    void clearLoopMarkers();

signals:
    void seekRequested(qint64 positionMs);
    // Pressing starts a scrub; the picture follows the pointer but nothing is
    // committed until it ends.
    void scrubStarted();
    // Emitted once, when the pointer really moved (a few pixels) - a plain click
    // with a bit of tremor must not count as a drag.
    void scrubMoved();
    // Emitted when a click or drag on the band ends. The receiver uses it to
    // flush the last coalesced seek immediately instead of waiting for the
    // next tick of its scrubbing timer.
    void scrubFinished();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QRect barArea() const;
    int xForTime(qint64 atMs, const QRect& area) const;
    // Band goes under the script strokes, markers go on top of them.
    void paintLoopBand(QPainter& painter, const QRect& area) const;
    void paintLoopMarkers(QPainter& painter, const QRect& area) const;
    void seekToX(int x);

    // Time-ordered actions of the main stroke axis; drawn the same way
    // XTPlayer's heat map does it - one coloured line per action pair.
    QVector<Action> m_actions;
    qint64 m_durationMs = 0;
    qint64 m_positionMs = 0;
    qint64 m_loopStartMs = -1;
    qint64 m_loopEndMs = -1;
    bool m_dragging = false;
    // X of the press, used to tell a click from a drag.
    int m_pressX = 0;
    bool m_moved = false;
};
