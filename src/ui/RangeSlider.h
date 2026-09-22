#pragma once

#include <QColor>
#include <QWidget>

// Compact double-handle slider used by the axis limit rows. Values are plain
// integers in the device domain (0..9999 by default).
class RangeSlider : public QWidget
{
    Q_OBJECT

public:
    explicit RangeSlider(QWidget* parent = nullptr);

    void setRange(int minimum, int maximum);
    int minimum() const { return m_minimum; }
    int maximum() const { return m_maximum; }

    int lowerValue() const { return m_lower; }
    int upperValue() const { return m_upper; }
    void setValues(int lower, int upper);
    void setLowerValue(int value);
    void setUpperValue(int value);

    void setMinimumGap(int gap);
    int minimumGap() const { return m_minimumGap; }

    void setAccentColor(const QColor& color);
    QColor accentColor() const { return m_accent; }

    // Current script position in slider units: the track fills from the lower
    // limit up to this value. Pass -1 to hide the fill.
    void setFillValue(int value);
    int fillValue() const { return m_fill; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void lowerValueChanged(int value);
    void upperValueChanged(int value);
    void rangeChanged(int lower, int upper);
    // Emitted when an interaction with a handle ends (mouse release, wheel or
    // keyboard). `upper` names the edge the user touched. The signal fires even
    // when the value did not change, so a limit that already sits at 0 % / 100 %
    // can still be previewed by clicking its handle.
    void handleInteractionFinished(bool upper);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    enum class Handle
    {
        None,
        Lower,
        Upper
    };

    QRectF trackRect() const;
    qreal positionForValue(int value) const;
    int valueForPosition(qreal x) const;
    Handle handleAt(const QPointF& position) const;
    void applyDrag(qreal x);
    void nudge(Handle handle, int delta);

    int m_minimum = 0;
    int m_maximum = 9999;
    int m_lower = 0;
    int m_upper = 9999;
    int m_minimumGap = 100;
    int m_fill = -1;
    QColor m_accent;

    Handle m_dragHandle = Handle::None;
    Handle m_hoverHandle = Handle::None;
};
