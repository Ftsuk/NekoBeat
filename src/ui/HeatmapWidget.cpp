#include "core/Loc.h"
#include "ui/HeatmapWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>

#include <algorithm>

namespace
{
// How far the pointer has to travel before a press counts as a drag.
constexpr int kDragThresholdPx = 4;

// XTPlayer's heat map colouring, verbatim: the pen is chosen from the
// *absolute* stroke speed (percent of travel per 1/100 s, i.e.
// |deltaPos| / deltaMs * 100) with fixed thresholds. It deliberately does not
// normalise against the fastest action, otherwise every script would end up
// drowning in red.
QPen heatPen(float velocity)
{
    static const QPen darkRed(Qt::darkRed, 1, Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin);
    static const QPen red(Qt::red, 1, Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin);
    static const QPen orange(QColor(255, 165, 0), 1, Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin);
    static const QPen yellow(Qt::yellow, 1, Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin);
    static const QPen green(Qt::green, 1, Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin);

    if (velocity > 45)
        return darkRed;
    if (velocity > 35)
        return red;
    if (velocity > 25)
        return orange;
    if (velocity > 15)
        return yellow;
    return green;
}

// Same formula as XMath::calculateSpeed() in XTEngine.
float strokeSpeed(const Action& from, const Action& to)
{
    const qint64 deltaMs = to.atMs - from.atMs;
    const int deltaPos = qAbs(to.pos - from.pos);
    if (deltaMs <= 0 || deltaPos <= 0)
        return 0.0f;
    return static_cast<float>(deltaPos) / static_cast<float>(deltaMs) * 100.0f;
}
}

HeatmapWidget::HeatmapWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("Heatmap"));
    setFixedHeight(36);
    setCursor(Qt::PointingHandCursor);
    setToolTip(LT("脚本热力图（XTPlayer 同款配色）：绿色最慢，依次黄、橙、红，深红最快；上下边缘即 100 % 与 0 %，点击可跳转"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void HeatmapWidget::setBundle(const ScriptBundle& bundle, qint64 durationMs)
{
    m_durationMs = durationMs;
    m_actions.clear();

    if (durationMs <= 0 || bundle.tracks.isEmpty())
    {
        update();
        return;
    }

    // One strip only, driven by the main stroke axis (L0) like the reference
    // heat map; fall back to the first available axis when it is missing.
    const QList<Action>* mainActions = nullptr;
    const auto stroke = bundle.tracks.constFind(static_cast<int>(Track::Stroke));
    if (stroke != bundle.tracks.constEnd() && !stroke.value().actions.isEmpty())
        mainActions = &stroke.value().actions;
    else
    {
        for (auto it = bundle.tracks.constBegin(); it != bundle.tracks.constEnd(); ++it)
        {
            if (!it.value().actions.isEmpty())
            {
                mainActions = &it.value().actions;
                break;
            }
        }
    }
    if (!mainActions)
    {
        update();
        return;
    }

    // Keep every action point; XTPlayer connects them one by one instead of
    // resampling the curve, which is what gives the heat map its dense,
    // spiky look on fast sections.
    m_actions = QVector<Action>(mainActions->cbegin(), mainActions->cend());
    std::stable_sort(m_actions.begin(), m_actions.end(),
                     [](const Action& a, const Action& b) { return a.atMs < b.atMs; });

    update();
}

void HeatmapWidget::clear()
{
    m_actions.clear();
    m_durationMs = 0;
    m_positionMs = 0;
    m_loopStartMs = -1;
    m_loopEndMs = -1;
    update();
}

void HeatmapWidget::setPosition(qint64 positionMs)
{
    if (m_positionMs == positionMs)
        return;
    m_positionMs = positionMs;
    update();
}

void HeatmapWidget::setLoopMarkers(qint64 startMs, qint64 endMs)
{
    if (startMs < 0)
    {
        clearLoopMarkers();
        return;
    }
    // A lone A marker is valid; a B that does not follow A is dropped.
    if (endMs <= startMs)
        endMs = -1;
    if (m_loopStartMs == startMs && m_loopEndMs == endMs)
        return;
    m_loopStartMs = startMs;
    m_loopEndMs = endMs;
    update();
}

void HeatmapWidget::clearLoopMarkers()
{
    if (m_loopStartMs < 0 && m_loopEndMs < 0)
        return;
    m_loopStartMs = -1;
    m_loopEndMs = -1;
    update();
}

QRect HeatmapWidget::barArea() const
{
    // No vertical padding: 0 % must sit on the very bottom row and 100 % on the
    // very top row so that a script hitting its extremes is unmistakable.
    return rect().adjusted(1, 0, -1, 0);
}

int HeatmapWidget::xForTime(qint64 atMs, const QRect& area) const
{
    const int maxX = area.width() - 1;
    if (maxX <= 0 || area.height() <= 0)
        return area.left();

    const double duration = static_cast<double>(qMax<qint64>(1, m_durationMs));
    const qint64 clamped = qBound<qint64>(0, atMs, qMax<qint64>(1, m_durationMs));
    return area.left() + static_cast<int>(qRound64(maxX * (clamped / duration)));
}

void HeatmapWidget::paintLoopBand(QPainter& painter, const QRect& area) const
{
    if (m_durationMs <= 0 || m_loopStartMs < 0 || m_loopEndMs <= m_loopStartMs)
        return;

    const int startX = xForTime(m_loopStartMs, area);
    const int endX = xForTime(m_loopEndMs, area);

    // This band is painted *before* the script strokes, so highlighting the
    // clip never hides the heat map itself.
    QLinearGradient band(startX, 0, qMax(startX + 1, endX), 0);
    QColor bandStart(0x2E, 0x9C, 0xFF);
    bandStart.setAlpha(80);
    QColor bandEnd(0xFF, 0x9F, 0x0A);
    bandEnd.setAlpha(80);
    band.setColorAt(0.0, bandStart);
    band.setColorAt(1.0, bandEnd);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(QRect(QPoint(startX, area.top()), QPoint(endX, area.bottom())), band);
    painter.restore();
}

void HeatmapWidget::paintLoopMarkers(QPainter& painter, const QRect& area) const
{
    if (m_durationMs <= 0 || m_loopStartMs < 0)
        return;

    const int startX = xForTime(m_loopStartMs, area);
    const bool hasEnd = m_loopEndMs > m_loopStartMs;
    const int endX = hasEnd ? xForTime(m_loopEndMs, area) : -1;

    // A and B get their own colour so the two ends of the marked range are
    // unmistakable even on a dense heat map.
    const QColor colorA(0x2E, 0x9C, 0xFF);
    const QColor colorB(0xFF, 0x9F, 0x0A);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);

    // Full height 2 px posts; A shows up as soon as it is marked.
    painter.setPen(QPen(colorA, 2));
    painter.drawLine(startX, area.top() + 3, startX, area.bottom());
    if (hasEnd)
    {
        painter.setPen(QPen(colorB, 2));
        painter.drawLine(endX, area.top() + 3, endX, area.bottom());
    }

    painter.setRenderHint(QPainter::Antialiasing, true);

    // Badge + downward arrow at the top edge of the strip.
    constexpr int badgeWidth = 15;
    constexpr int badgeHeight = 13;
    const auto drawMarker = [&](int centerX, const QColor& color, const QString& text) {
        const int left = qBound(area.left(), centerX - badgeWidth / 2,
                                qMax(area.left(), area.right() - badgeWidth));
        const QRect badge(left, area.top(), badgeWidth, badgeHeight);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(badge, 3, 3);

        QFont font = painter.font();
        font.setPixelSize(9);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(QColor(0xFF, 0xFF, 0xFF));
        painter.drawText(badge, Qt::AlignCenter, text);

        const qreal tipX = badge.center().x();
        QPolygonF arrow;
        arrow << QPointF(tipX - 4.5, badge.bottom()) << QPointF(tipX + 4.5, badge.bottom())
              << QPointF(tipX, badge.bottom() + 5);
        painter.setBrush(color);
        painter.drawPolygon(arrow);
    };

    // Keep the two badges apart when the clip is very short.
    int badgeAX = startX;
    int badgeBX = endX;
    if (hasEnd && endX - startX < badgeWidth + 8)
    {
        badgeAX = startX - badgeWidth / 2 - 2;
        badgeBX = endX + badgeWidth / 2 + 2;
    }
    drawMarker(badgeAX, colorA, QStringLiteral("A"));
    if (hasEnd)
        drawMarker(badgeBX, colorB, QStringLiteral("B"));
    painter.restore();
}

void HeatmapWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0x08, 0x08, 0x08));

    const QRect area = barArea();
    if (area.width() <= 0 || area.height() <= 0)
        return;

    // Highlight band first so the script strokes stay perfectly readable.
    paintLoopBand(painter, area);

    if (m_actions.isEmpty() || m_durationMs <= 0)
    {
        painter.setPen(QColor(0x5A, 0x5A, 0x60));
        painter.drawText(area, Qt::AlignCenter, LT("无脚本"));
        paintLoopMarkers(painter, area);
        return;
    }

    // XTPlayer's HeatMap::paint() walks the sorted action list and draws one
    // line per neighbouring pair, picking the pen from that stroke's absolute
    // speed. We reproduce that exactly, with a single deliberate change: the
    // vertical range spans the whole strip (0 % at the bottom row, 100 % at the
    // top row) instead of XTPlayer's inner band, so extremes are readable.
    const int width = area.width();
    const int drawHeight = area.height();
    if (width <= 1 || drawHeight <= 2 || m_actions.size() < 2)
        return;

    const double duration = static_cast<double>(qMax<qint64>(1, m_durationMs));
    const int maxX = width - 1;
    const int maxY = drawHeight - 1;

    const auto xAt = [duration, maxX](qint64 atMs) {
        return static_cast<int>(qRound64(maxX * (atMs / duration)));
    };
    const auto yAt = [maxY](int pos) {
        return static_cast<int>(qRound(maxY * (100 - qBound(0, pos, 100)) / 100.0));
    };

    painter.save();
    painter.translate(area.topLeft());
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setClipRect(QRect(0, 0, width, area.height()));

    for (int i = 0; i + 1 < m_actions.size(); ++i)
    {
        const Action& from = m_actions.at(i);
        const Action& to = m_actions.at(i + 1);

        // XTPlayer skips strokes that start beyond the media duration.
        if (from.atMs > m_durationMs)
            break;

        painter.setPen(heatPen(strokeSpeed(from, to)));
        painter.drawLine(xAt(from.atMs), yAt(from.pos), xAt(to.atMs), yAt(to.pos));
    }
    painter.restore();

    // A/B marking badges and posts, on top of the strokes but still purely
    // informational: they never react to the mouse.
    paintLoopMarkers(painter, area);

    // Current position: bright double line with a marker on top.
    if (m_positionMs > 0)
    {
        const int x = area.left()
                      + static_cast<int>(area.width() * m_positionMs
                                         / qMax<qint64>(1, m_durationMs));
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.fillRect(QRect(x - 2, area.top(), 5, area.height()),
                         QColor(0xFF, 0xFF, 0xFF, 45));
        painter.setPen(QPen(QColor(0xFF, 0xFF, 0xFF), 2));
        painter.drawLine(x, area.top(), x, area.bottom());

        QPolygonF marker;
        marker << QPointF(x - 4, area.top()) << QPointF(x + 4, area.top())
               << QPointF(x, area.top() + 6);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0xFF, 0xFF, 0xFF));
        painter.drawPolygon(marker);
    }
}

void HeatmapWidget::mousePressEvent(QMouseEvent* event)
{
    if (m_durationMs <= 0)
        return;

    m_dragging = true;
    m_moved = false;
    m_pressX = event->pos().x();
    emit scrubStarted();
    seekToX(event->pos().x());
}

void HeatmapWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging)
        return;
    // A click with a little tremor stays a click; only a real move counts as a
    // drag, which is what tells the window to hold the machine still.
    if (!m_moved && qAbs(event->pos().x() - m_pressX) >= kDragThresholdPx)
    {
        m_moved = true;
        emit scrubMoved();
    }
    seekToX(event->pos().x());
}

void HeatmapWidget::mouseReleaseEvent(QMouseEvent*)
{
    if (!m_dragging)
        return;
    m_dragging = false;
    emit scrubFinished();
}

void HeatmapWidget::seekToX(int x)
{
    const QRect area = barArea();
    const double ratio = qBound(0.0,
                                (x - area.left())
                                    / static_cast<double>(qMax(1, area.width())),
                                1.0);
    const qint64 position = static_cast<qint64>(m_durationMs * ratio);
    m_positionMs = position;
    update();
    emit seekRequested(position);
}
