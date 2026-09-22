#include "ui/LibraryDragHint.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>

#include <algorithm>

LibraryDragHint::LibraryDragHint(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_TranslucentBackground, true);

    m_fade = new QPropertyAnimation(this, "intensity", this);
    m_fade->setDuration(140);
    // Linear easing keeps the perceived speed constant; the duration is scaled
    // by the starting intensity in fadeOut() so every fade looks identical.
    m_fade->setEasingCurve(QEasingCurve::Linear);
    connect(m_fade, &QPropertyAnimation::finished, this, [this]() {
        if (m_intensity <= 0.02)
            hide();
    });
}

void LibraryDragHint::fadeOut()
{
    if (!isVisible() || m_intensity <= 0.02)
        return;
    m_fade->stop();
    m_fade->setDuration(qMax(50, qRound(140 * m_intensity)));
    m_fade->setStartValue(m_intensity);
    m_fade->setEndValue(0.0);
    m_fade->start();
}

void LibraryDragHint::stopFade()
{
    if (m_fade->state() != QAbstractAnimation::Stopped)
        m_fade->stop();
}

void LibraryDragHint::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    update();
}

void LibraryDragHint::setSide(Side side)
{
    if (m_side == side)
        return;
    m_side = side;
    update();
}

void LibraryDragHint::setIntensity(qreal intensity)
{
    const qreal clamped = qBound(0.0, intensity, 1.0);
    if (qFuzzyCompare(m_intensity + 1.0, clamped + 1.0))
        return;
    m_intensity = clamped;
    update();
}

void LibraryDragHint::paintEvent(QPaintEvent*)
{
    const qreal intensity = qBound(0.0, m_intensity, 1.0);
    if (intensity <= 0.02 || width() <= 0 || height() <= 0)
        return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // The strip hugs the window edge: fully opaque blue on the outermost
    // pixels, fading to nothing towards the inside of the panel. The arrow -
    // not the colour - tells which way the panel will go.
    const bool pointsLeft = (m_side == Side::Left) == (m_mode == Mode::Collapse);
    const QColor accent(0x0A, 0x84, 0xFF);
    QColor strong(accent);
    strong.setAlpha(qRound(255 * intensity));
    QColor mid(accent);
    mid.setAlpha(qRound(150 * intensity));
    QColor clear(accent);
    clear.setAlpha(0);

    // Both sides fade towards the inside of the window; only the solid edge
    // changes place.
    QLinearGradient gradient = m_side == Side::Left
                                   ? QLinearGradient(0, 0, width(), 0)
                                   : QLinearGradient(width(), 0, 0, 0);
    gradient.setColorAt(0.0, strong);
    gradient.setColorAt(0.35, mid);
    gradient.setColorAt(1.0, clear);
    painter.fillRect(rect(), gradient);

    // Big, unmistakable direction badge: a filled disc with a double chevron,
    // parked a fixed distance from the window edge so it stays easy to spot
    // even while the panel is still wide.
    const bool highlight = intensity > 0.85;
    const qreal radius = highlight ? 20.0 : 16.0;
    const qreal edgeOffset = 32.0;
    qreal cx = m_side == Side::Left ? edgeOffset : width() - edgeOffset;
    cx = qBound(radius + 4.0, cx, std::max(radius + 4.0, width() - radius - 4.0));
    const qreal cy = height() / 2.0;

    QColor disc(accent);
    disc.setAlpha(qRound(230 * intensity));
    painter.setPen(Qt::NoPen);
    painter.setBrush(disc);
    painter.drawEllipse(QPointF(cx, cy), radius, radius);

    const QColor arrowColor(0xFF, 0xFF, 0xFF, qRound(255 * intensity));
    painter.setPen(QPen(arrowColor, 3.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    const qreal half = highlight ? 8.0 : 6.8;
    const qreal gap = 7.0;
    for (int i = 0; i < 2; ++i)
    {
        const qreal offset = (i == 0 ? -gap * 0.5 : gap * 0.5);
        QPainterPath chevron;
        if (pointsLeft)
        {
            chevron.moveTo(cx + offset + half, cy - half);
            chevron.lineTo(cx + offset - half, cy);
            chevron.lineTo(cx + offset + half, cy + half);
        }
        else
        {
            chevron.moveTo(cx + offset - half, cy - half);
            chevron.lineTo(cx + offset + half, cy);
            chevron.lineTo(cx + offset - half, cy + half);
        }
        painter.drawPath(chevron);
    }
}
