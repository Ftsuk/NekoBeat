#include "ui/RangeSlider.h"

#include "ui/Theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace
{
constexpr int kTrackHeight = 6;
constexpr qreal kHandleRadius = 8.0;
// Just enough room for the handle to sit fully inside the widget at either
// end, so dragging to 0 % / 100 % visually reaches the very edge.
constexpr qreal kSideMargin = kHandleRadius + 1.0;
}

RangeSlider::RangeSlider(QWidget* parent)
    : QWidget(parent)
    , m_accent(Theme::accent())
{
    setMinimumHeight(26);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
}

void RangeSlider::setRange(int minimum, int maximum)
{
    if (maximum < minimum)
        std::swap(minimum, maximum);
    if (maximum == minimum)
        maximum = minimum + 1;
    m_minimum = minimum;
    m_maximum = maximum;
    m_lower = std::clamp(m_lower, m_minimum, m_maximum);
    m_upper = std::clamp(m_upper, m_minimum, m_maximum);
    if (m_upper < m_lower)
        std::swap(m_lower, m_upper);
    update();
}

void RangeSlider::setValues(int lower, int upper)
{
    lower = std::clamp(lower, m_minimum, m_maximum);
    upper = std::clamp(upper, m_minimum, m_maximum);
    if (upper < lower)
        std::swap(lower, upper);
    m_lower = lower;
    m_upper = upper;
    update();
}

void RangeSlider::setLowerValue(int value)
{
    const int clamped = std::clamp(value, m_minimum, std::max(m_minimum, m_upper - m_minimumGap));
    if (clamped == m_lower)
        return;
    m_lower = clamped;
    update();
    emit lowerValueChanged(m_lower);
    emit rangeChanged(m_lower, m_upper);
}

void RangeSlider::setUpperValue(int value)
{
    const int clamped = std::clamp(value, std::min(m_maximum, m_lower + m_minimumGap), m_maximum);
    if (clamped == m_upper)
        return;
    m_upper = clamped;
    update();
    emit upperValueChanged(m_upper);
    emit rangeChanged(m_lower, m_upper);
}

void RangeSlider::setMinimumGap(int gap)
{
    m_minimumGap = std::max(0, gap);
    if (m_upper - m_lower < m_minimumGap)
        setUpperValue(m_lower + m_minimumGap);
}

void RangeSlider::setAccentColor(const QColor& color)
{
    m_accent = color;
    update();
}

void RangeSlider::setFillValue(int value)
{
    const int next = value < 0 ? -1 : std::clamp(value, m_minimum, m_maximum);
    if (next == m_fill)
        return;
    m_fill = next;
    update();
}

QSize RangeSlider::sizeHint() const
{
    return QSize(200, 26);
}

QSize RangeSlider::minimumSizeHint() const
{
    return QSize(80, 26);
}

QRectF RangeSlider::trackRect() const
{
    const qreal top = (height() - kTrackHeight) / 2.0;
    return QRectF(kSideMargin, top, std::max<qreal>(1.0, width() - kSideMargin * 2), kTrackHeight);
}

qreal RangeSlider::positionForValue(int value) const
{
    const QRectF track = trackRect();
    const double span = std::max(1, m_maximum - m_minimum);
    const double ratio = (value - m_minimum) / span;
    return track.left() + ratio * track.width();
}

int RangeSlider::valueForPosition(qreal x) const
{
    const QRectF track = trackRect();
    const double ratio = std::clamp((x - track.left()) / std::max<qreal>(1.0, track.width()), 0.0, 1.0);
    return m_minimum + qRound(ratio * (m_maximum - m_minimum));
}

RangeSlider::Handle RangeSlider::handleAt(const QPointF& position) const
{
    const qreal lowerX = positionForValue(m_lower);
    const qreal upperX = positionForValue(m_upper);
    const qreal hit = kHandleRadius + 6.0;
    const qreal lowerDistance = std::abs(position.x() - lowerX);
    const qreal upperDistance = std::abs(position.x() - upperX);

    if (lowerDistance <= hit && lowerDistance <= upperDistance)
        return Handle::Lower;
    if (upperDistance <= hit)
        return Handle::Upper;
    return Handle::None;
}

void RangeSlider::applyDrag(qreal x)
{
    const int value = valueForPosition(x);
    if (m_dragHandle == Handle::Lower)
    {
        const int next = std::clamp(value, m_minimum, m_upper - m_minimumGap);
        if (next == m_lower)
            return;
        m_lower = next;
        update();
        emit lowerValueChanged(m_lower);
        emit rangeChanged(m_lower, m_upper);
    }
    else if (m_dragHandle == Handle::Upper)
    {
        const int next = std::clamp(value, m_lower + m_minimumGap, m_maximum);
        if (next == m_upper)
            return;
        m_upper = next;
        update();
        emit upperValueChanged(m_upper);
        emit rangeChanged(m_lower, m_upper);
    }
}

void RangeSlider::nudge(Handle handle, int delta)
{
    if (handle == Handle::Lower)
        setLowerValue(m_lower + delta);
    else
        setUpperValue(m_upper + delta);
}

void RangeSlider::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (!isEnabled())
        painter.setOpacity(0.35);

    const QRectF track = trackRect();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0xE5, 0xE5, 0xEA));
    painter.drawRoundedRect(track, track.height() / 2.0, track.height() / 2.0);

    // "Charging" bar: the lower limit is the origin and the current script
    // value fills the track, like the funscript progress bar in XTPlayer.
    const qreal lowerX = positionForValue(m_lower);
    const qreal upperX = positionForValue(m_upper);

    // Enabled travel range. Without it the track gives no clue how much of the
    // stroke is in use, and it is impossible to tell whether a handle really
    // reached 0 % / 100 %.
    if (upperX - lowerX > 0.5)
    {
        QColor band = m_accent;
        band.setAlpha(64);
        painter.setPen(Qt::NoPen);
        painter.setBrush(band);
        painter.drawRoundedRect(
            QRectF(lowerX, track.top(), upperX - lowerX, track.height()),
            track.height() / 2.0, track.height() / 2.0);
    }

    const int fillValue = m_fill < 0 ? m_lower : std::clamp(m_fill, m_lower, m_upper);
    const qreal fillX = positionForValue(fillValue);
    const qreal fillWidth = std::max<qreal>(0.0, fillX - lowerX);
    if (fillWidth > 0.5)
    {
        QRectF fillRect(lowerX, track.top(), fillWidth, track.height());
        const qreal radius = std::min<qreal>(track.height() / 2.0, fillWidth / 2.0);
        painter.setBrush(m_accent);
        painter.drawRoundedRect(fillRect, radius, radius);
    }
    if (m_fill >= 0)
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0xFF, 0xFF, 0xFF, 0xE6));
        painter.drawRoundedRect(
            QRectF(fillX - 1.0, height() / 2.0 - 8.0, 2.0, 16.0), 1.0, 1.0);
    }

    const qreal centerY = height() / 2.0;
    const auto drawHandle = [&](qreal x, Handle handle) {
        const bool active = m_dragHandle == handle || (m_dragHandle == Handle::None && m_hoverHandle == handle);
        painter.setPen(QPen(active ? m_accent : QColor(0xC7, 0xC7, 0xCC), active ? 2.0 : 1.4));
        painter.setBrush(QColor(0xFF, 0xFF, 0xFF));
        const qreal radius = active ? kHandleRadius + 0.5 : kHandleRadius - 0.5;
        painter.drawEllipse(QPointF(x, centerY), radius, radius);
    };

    if (hasFocus())
    {
        painter.setPen(QPen(QColor(m_accent.red(), m_accent.green(), m_accent.blue(), 70), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(0.5, 0.5, width() - 1.0, height() - 1.0), 8.0, 8.0);
    }

    drawHandle(lowerX, Handle::Lower);
    drawHandle(upperX, Handle::Upper);
}

void RangeSlider::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !isEnabled())
        return;

    m_dragHandle = handleAt(event->position());
    if (m_dragHandle == Handle::None)
    {
        const qreal lowerX = positionForValue(m_lower);
        const qreal upperX = positionForValue(m_upper);
        m_dragHandle = std::abs(event->position().x() - lowerX) <= std::abs(event->position().x() - upperX)
                           ? Handle::Lower
                           : Handle::Upper;
    }
    applyDrag(event->position().x());
    update();
}

void RangeSlider::mouseMoveEvent(QMouseEvent* event)
{
    if (!isEnabled())
        return;

    if (m_dragHandle != Handle::None && (event->buttons() & Qt::LeftButton))
    {
        applyDrag(event->position().x());
        return;
    }

    const Handle hover = handleAt(event->position());
    if (hover != m_hoverHandle)
    {
        m_hoverHandle = hover;
        update();
    }
}

void RangeSlider::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (m_dragHandle == Handle::None)
        return;

    // Report the touched edge even when nothing moved: a limit already sitting
    // at 0 % / 100 % has to move the device there when its handle is clicked.
    const bool upper = m_dragHandle == Handle::Upper;
    m_dragHandle = Handle::None;
    update();
    emit handleInteractionFinished(upper);
}

void RangeSlider::wheelEvent(QWheelEvent* event)
{
    if (!isEnabled())
        return;

    const int steps = event->angleDelta().y() / 120;
    if (steps == 0)
        return;

    const int step = std::max(1, (m_maximum - m_minimum) / 200);
    const Handle handle = m_hoverHandle != Handle::None
                              ? m_hoverHandle
                              : (std::abs(event->position().x() - positionForValue(m_lower))
                                         <= std::abs(event->position().x() - positionForValue(m_upper))
                                     ? Handle::Lower
                                     : Handle::Upper);
    nudge(handle, steps * step);
    emit handleInteractionFinished(handle == Handle::Upper);
    event->accept();
}

void RangeSlider::keyPressEvent(QKeyEvent* event)
{
    const int step = (event->modifiers() & Qt::ShiftModifier)
                         ? std::max(1, (m_maximum - m_minimum) / 200)
                         : std::max(1, (m_maximum - m_minimum) / 2000);
    switch (event->key())
    {
    case Qt::Key_Left:
        nudge(Handle::Lower, -step);
        emit handleInteractionFinished(false);
        event->accept();
        return;
    case Qt::Key_Right:
        nudge(Handle::Lower, step);
        emit handleInteractionFinished(false);
        event->accept();
        return;
    case Qt::Key_Up:
        nudge(Handle::Upper, step);
        emit handleInteractionFinished(true);
        event->accept();
        return;
    case Qt::Key_Down:
        nudge(Handle::Upper, -step);
        emit handleInteractionFinished(true);
        event->accept();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void RangeSlider::leaveEvent(QEvent* event)
{
    m_hoverHandle = Handle::None;
    update();
    QWidget::leaveEvent(event);
}
