#include "core/Loc.h"
#include "ui/LoopGridWidget.h"

#include <QContextMenuEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollArea>

#include <algorithm>

namespace
{
constexpr int kCardPadding = 4;
// Constant column spacing, exactly like the media library grid and XTPlayer:
// the leftover width stays at the right edge instead of being spread between
// the covers.
constexpr int kColumnGap = 4;

QString formatClipDuration(qint64 ms)
{
    const qint64 total = std::max<qint64>(0, ms);
    const qint64 minutes = total / 60000;
    const qint64 seconds = (total % 60000) / 1000;
    if (minutes > 0)
        return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1s").arg(total / 1000.0, 0, 'f', 1);
}

// Same helper as the media library grid: the first line is short by the width
// of the 单轴/多轴 badge that is painted in front of it.
QStringList wrapName(const QString& name, const QFontMetrics& metrics,
                     int firstLineWidth, int otherLineWidth, int maxLines)
{
    QStringList lines;
    QString remaining = name.trimmed();
    while (!remaining.isEmpty() && lines.size() < maxLines)
    {
        const int width = lines.isEmpty() ? firstLineWidth : otherLineWidth;
        if (width <= 0)
            break;
        if (metrics.horizontalAdvance(remaining) <= width)
        {
            lines.append(remaining);
            remaining.clear();
            break;
        }

        int fit = 0;
        int lastBreak = -1;
        for (int i = 0; i < remaining.size(); ++i)
        {
            if (metrics.horizontalAdvance(remaining.left(i + 1)) > width)
                break;
            fit = i + 1;
            const QChar character = remaining.at(i);
            if (character.isSpace() || character == QLatin1Char('_')
                || character == QLatin1Char('-') || character == QLatin1Char('.')
                || character == QLatin1Char(']') || character == QLatin1Char(')'))
            {
                lastBreak = i + 1;
            }
        }
        if (fit <= 0)
            fit = 1;

        const int cut = (lastBreak > 0 && lastBreak <= fit) ? lastBreak : fit;
        lines.append(remaining.left(cut).trimmed());
        remaining = remaining.mid(cut).trimmed();
    }

    if (!remaining.isEmpty() && !lines.isEmpty())
    {
        const QString tail = lines.last() + QLatin1Char(' ') + remaining;
        const int width = lines.size() == 1 ? firstLineWidth : otherLineWidth;
        lines.last() = metrics.elidedText(tail, Qt::ElideRight, width);
    }
    return lines;
}
}

LoopGridWidget::LoopGridWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MediaGrid"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void LoopGridWidget::setItems(const QList<LoopGridItem>& items)
{
    m_items = items;
    if (!m_selectedClipId.isEmpty() && indexOfClip(m_selectedClipId) < 0)
        m_selectedClipId.clear();
    m_hoveredIndex = -1;
    refreshColumnCount();
    update();
}

void LoopGridWidget::setCardWidth(int width)
{
    m_cardWidth = qBound(90, width, 480);
    // A different cover size is a fresh layout; the old column count would only
    // keep the last column half covered.
    m_columns = 0;
    refreshColumnCount();
    update();
}

void LoopGridWidget::setFontSize(int pixelSize)
{
    m_fontSize = qBound(10, pixelSize, 22);
    m_wrapCache.clear();
    updateGeometryForItems();
    update();
}

void LoopGridWidget::setPixmapCache(QCache<QString, QPixmap>* cache)
{
    m_cache = cache;
}

void LoopGridWidget::invalidatePixmap(const QString& path)
{
    if (m_cache && !path.isEmpty())
    {
        // Covers are cached as "<path>@<width>x<height>". Dropping only the
        // plain key would leave the stale, already scaled image behind, so a
        // regenerated thumbnail would never show up.
        const QString prefix = path + QLatin1Char('@');
        const QStringList keys = m_cache->keys();
        for (const QString& key : keys)
        {
            if (key == path || key.startsWith(prefix))
                m_cache->remove(key);
        }
    }
    update();
}

void LoopGridWidget::setActiveClipId(const QString& clipId)
{
    if (m_activeClipId == clipId)
        return;
    m_activeClipId = clipId;
    update();
}

void LoopGridWidget::setShowAxisTag(bool show)
{
    if (m_showAxisTag == show)
        return;
    m_showAxisTag = show;
    update();
}

void LoopGridWidget::setSelectedClipId(const QString& clipId)
{
    if (m_selectedClipId == clipId)
        return;
    m_selectedClipId = clipId;
    update();
    emit selectionChanged(m_selectedClipId);
}

void LoopGridWidget::ensureVisibleClip(const QString& clipId)
{
    const int index = indexOfClip(clipId);
    if (index < 0)
        return;
    const QRect rect = cardRectAt(index);
    if (QScrollArea* area = scrollArea())
        area->ensureVisible(rect.center().x(), rect.center().y(), 20, 40);
}

const LoopGridItem* LoopGridWidget::itemFor(const QString& clipId) const
{
    const int index = indexOfClip(clipId);
    return index >= 0 ? &m_items.at(index) : nullptr;
}

int LoopGridWidget::indexOfClip(const QString& clipId) const
{
    for (int i = 0; i < m_items.size(); ++i)
    {
        if (m_items.at(i).clip.id == clipId)
            return i;
    }
    return -1;
}

int LoopGridWidget::columnCount() const
{
    return qMax(1, m_columns);
}

int LoopGridWidget::columnCountFor(int available, int previous) const
{
    if (available <= 0)
        return 1;

    const auto fullWidth = [this](int columns) {
        return columns * m_cardWidth + (columns - 1) * kColumnGap;
    };

    int columns = qMax(1, previous);
    // Narrowing: the last column stays until more than half of it is covered.
    while (columns > 1 && available < fullWidth(columns) - m_cardWidth / 2)
        --columns;
    // Widening: the next column only appears once it fits completely.
    while (available >= fullWidth(columns + 1))
        ++columns;
    return columns;
}

void LoopGridWidget::refreshColumnCount()
{
    const int columns = columnCountFor(width(), m_columns);
    if (columns != m_columns)
        m_columns = columns;
    updateGeometryForItems();
}

int LoopGridWidget::columnGap() const
{
    return columnCount() > 1 ? kColumnGap : 0;
}

int LoopGridWidget::textAreaHeight() const
{
    return lineHeight() * 3 + m_fontSize + 8;
}

int LoopGridWidget::cardHeight() const
{
    return m_cardWidth * 9 / 16 + textAreaHeight() + 14;
}

QRect LoopGridWidget::cardRectAt(int index) const
{
    if (index < 0 || index >= m_items.size())
        return {};
    const int columns = columnCount();
    const int column = index % columns;
    const int row = index / columns;
    const int x = column * (m_cardWidth + columnGap());
    const int y = row * (cardHeight() + rowGap());
    return QRect(x, y, m_cardWidth, cardHeight());
}

int LoopGridWidget::indexAt(const QPoint& pos) const
{
    const int columns = columnCount();
    if (columns <= 0)
        return -1;
    const int stride = m_cardWidth + columnGap();
    const int rowStride = cardHeight() + rowGap();
    if (stride <= 0 || rowStride <= 0)
        return -1;

    const int column = pos.x() / qMax(1, stride);
    const int row = pos.y() / qMax(1, rowStride);
    if (column < 0 || column >= columns)
        return -1;

    const int index = row * columns + column;
    if (index < 0 || index >= m_items.size())
        return -1;
    if (!cardRectAt(index).contains(pos))
        return -1;
    return index;
}

void LoopGridWidget::updateGeometryForItems()
{
    const int columns = columnCount();
    const int rows = columns > 0 ? (m_items.size() + columns - 1) / columns : 1;
    const int neededHeight = qMax(0, rows * (cardHeight() + rowGap()));
    if (height() != neededHeight)
        setMinimumHeight(neededHeight);
}

QStringList LoopGridWidget::wrappedTitle(const QString& text, const QFontMetrics& metrics,
                                         int firstLineWidth, int otherLineWidth) const
{
    // The font size is not part of the key: setFontSize() clears this cache
    // instead, which keeps the lookup cheap.
    const QString key = QStringLiteral("%1\u0001%2\u0001%3")
                            .arg(text)
                            .arg(firstLineWidth)
                            .arg(otherLineWidth);
    const auto cached = m_wrapCache.constFind(key);
    if (cached != m_wrapCache.constEnd())
        return cached.value();

    QStringList lines = wrapName(text, metrics, firstLineWidth, otherLineWidth, 2);
    if (m_wrapCache.size() > 4000)
        m_wrapCache.clear();
    m_wrapCache.insert(key, lines);
    return lines;
}

const QPixmap* LoopGridWidget::coverPixmapFor(const QString& path, const QSize& size) const
{
    if (!m_cache || path.isEmpty() || size.isEmpty())
        return nullptr;

    const QString key = path + QLatin1Char('@') + QString::number(size.width())
                        + QLatin1Char('x') + QString::number(size.height());
    if (const QPixmap* cached = m_cache->object(key))
        return cached;
    if (!QFileInfo::exists(path))
        return nullptr;

    QPixmap raw(path);
    if (raw.isNull())
        return nullptr;

    const qreal ratio = devicePixelRatioF();
    QPixmap target(QSize(qRound(size.width() * ratio), qRound(size.height() * ratio)));
    target.setDevicePixelRatio(ratio);
    target.fill(Qt::transparent);

    QPainter painter(&target);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, size.width(), size.height()), 8, 8);
    painter.setClipPath(clip);
    painter.drawPixmap(QRect(0, 0, size.width(), size.height()), raw);
    painter.end();

    m_cache->insert(key, new QPixmap(target));
    return m_cache->object(key);
}

void LoopGridWidget::drawCard(QPainter& painter, const QRect& card,
                              const LoopGridItem& item, bool selected, bool hovered,
                              bool active) const
{
    QColor cardColor(0x2A, 0x2A, 0x2D);
    if (hovered)
        cardColor = QColor(0x33, 0x33, 0x37);
    painter.setPen(Qt::NoPen);
    painter.setBrush(cardColor);
    painter.drawRoundedRect(card, 10, 10);

    const QRect content = card.adjusted(kCardPadding, kCardPadding, -kCardPadding,
                                        -kCardPadding);
    const int textArea = textAreaHeight();
    const QRect coverRect(content.left(), content.top(), content.width(),
                          content.height() - textArea);
    const QRect textRect(content.left() + 2, coverRect.bottom() + 3,
                         content.width() - 4, textArea - 4);

    const QPixmap* cover = coverPixmapFor(item.thumbnailPath, coverRect.size());
    if (!cover)
        cover = coverPixmapFor(item.fallbackThumbnailPath, coverRect.size());

    if (cover)
    {
        // Already scaled and rounded: a plain 1:1 blit.
        painter.drawPixmap(coverRect.topLeft(), *cover);
    }
    else
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x1E, 0x1E, 0x21));
        painter.drawRoundedRect(coverRect, 8, 8);
        painter.setPen(QColor(0x6B, 0x6B, 0x70));
        QFont placeholderFont = painter.font();
        placeholderFont.setPixelSize(20);
        painter.setFont(placeholderFont);
        painter.drawText(coverRect, Qt::AlignCenter, QStringLiteral("▶"));
    }
    if (item.missing)
    {
        QPainterPath mask;
        mask.addRoundedRect(coverRect, 8, 8);
        painter.save();
        painter.setClipPath(mask);
        painter.fillRect(coverRect, QColor(0, 0, 0, 150));
        painter.restore();
    }

    const auto drawBadge = [&](const QRect& rect, const QColor& background,
                               const QColor& foreground, const QString& text,
                               const QFont& font) {
        painter.save();
        painter.setFont(font);
        painter.setPen(Qt::NoPen);
        painter.setBrush(background);
        painter.drawRoundedRect(rect, 5, 5);
        painter.setPen(foreground);
        painter.drawText(rect, Qt::AlignCenter, text);
        painter.restore();
    };

    QFont badgeFont = painter.font();
    badgeFont.setPixelSize(qMax(8, m_fontSize - 3));
    const QFontMetrics badgeMetrics(badgeFont);

    if (!item.folderNames.isEmpty())
    {
        const QString star = QStringLiteral("★");
        QFont starFont = painter.font();
        starFont.setPixelSize(qMax(11, m_fontSize + 1));
        const QFontMetrics starMetrics(starFont);
        QRect starBadge(0, 0, starMetrics.horizontalAdvance(star) + 10, m_fontSize + 4);
        starBadge.moveTopLeft(coverRect.topLeft() + QPoint(6, 6));
        drawBadge(starBadge, QColor(0, 0, 0, 160), QColor(0xFF, 0xD6, 0x0A), star, starFont);
    }

    if (active)
    {
        const QString label = LT("循环中");
        QFont activeFont = badgeFont;
        activeFont.setBold(true);
        const QFontMetrics activeMetrics(activeFont);
        QRect activeBadge(0, 0, activeMetrics.horizontalAdvance(label) + 14,
                          m_fontSize + 4);
        activeBadge.moveTopRight(coverRect.topRight() + QPoint(-6, 6));
        drawBadge(activeBadge, QColor(0x0A, 0x84, 0xFF, 220), QColor(0xFF, 0xFF, 0xFF),
                  label, activeFont);
    }
    else if (item.missing)
    {
        const QString label = LT("失效");
        QRect activeBadge(0, 0, badgeMetrics.horizontalAdvance(label) + 14, m_fontSize + 4);
        activeBadge.moveTopRight(coverRect.topRight() + QPoint(-6, 6));
        drawBadge(activeBadge, QColor(0xC0, 0x39, 0x2B, 200), QColor(0xFF, 0xFF, 0xFF),
                  label, badgeFont);
    }

    const QString duration = formatClipDuration(item.clip.durationMs());
    const int badgeWidth = badgeMetrics.horizontalAdvance(duration) + 12;
    const int badgeHeight = m_fontSize + 4;
    QRect durationBadge(0, 0, badgeWidth, badgeHeight);
    durationBadge.moveBottomRight(coverRect.bottomRight() - QPoint(6, 6));
    drawBadge(durationBadge, QColor(0, 0, 0, 175), QColor(0xFF, 0xFF, 0xFF), duration,
              badgeFont);

    // Title (two lines), then the A-B timecode and finally the tag chips.
    QFont titleFont = painter.font();
    titleFont.setPixelSize(m_fontSize);
    titleFont.setBold(false);
    const QFontMetrics titleMetrics(titleFont);
    const QString title = item.clip.title.trimmed().isEmpty()
                              ? LoopClipRules::defaultTitle(item.clip.mediaPath,
                                                            item.clip.startMs,
                                                            item.clip.endMs)
                              : item.clip.title;

    // Same 单轴/多轴 badge as the media library cards, drawn in front of the
    // first title line. Videos that are gone keep the row clean.
    const bool showTag = m_showAxisTag && !item.missing;
    const QString axisTag = item.scriptAxes <= 0
                                ? LT("无脚本")
                                : (item.scriptAxes == 1 ? LT("单轴")
                                                        : LT("多轴"));
    const QColor axisColor = item.scriptAxes <= 0
                                 ? QColor(0x98, 0x98, 0x9D)
                                 : (item.scriptAxes == 1 ? QColor(0x6B, 0xB4, 0xFF)
                                                         : QColor(0x30, 0xD1, 0x58));
    const int tagWidth =
        showTag ? titleMetrics.horizontalAdvance(axisTag) + 8 : 0;
    const QStringList lines =
        wrappedTitle(title, titleMetrics, textRect.width() - tagWidth, textRect.width());

    painter.setFont(titleFont);
    int lineTop = textRect.top();
    if (showTag && tagWidth < textRect.width())
    {
        painter.setPen(axisColor);
        painter.drawText(QRect(textRect.left(), lineTop, tagWidth, lineHeight()),
                         Qt::AlignLeft | Qt::AlignVCenter, axisTag);
    }
    painter.setPen(selected ? QColor(0xFF, 0xFF, 0xFF) : QColor(0xEC, 0xEC, 0xF1));
    for (int i = 0; i < lines.size(); ++i)
    {
        const QRect lineRect =
            (i == 0 && showTag && tagWidth < textRect.width())
                ? QRect(textRect.left() + tagWidth, lineTop, textRect.width() - tagWidth,
                        lineHeight())
                : QRect(textRect.left(), lineTop, textRect.width(), lineHeight());
        painter.drawText(lineRect, Qt::AlignLeft | Qt::AlignVCenter, lines.at(i));
        lineTop += lineHeight();
    }

    QFont metaFont = titleFont;
    metaFont.setPixelSize(qMax(9, m_fontSize - 2));
    painter.setFont(metaFont);
    painter.setPen(QColor(0x9A, 0x9A, 0xA0));
    const QString range = QStringLiteral("%1 → %2")
                              .arg(LoopClipRules::formatTimecode(item.clip.startMs),
                                   LoopClipRules::formatTimecode(item.clip.endMs));
    painter.drawText(QRect(textRect.left(), lineTop, textRect.width(), lineHeight()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(metaFont).elidedText(range, Qt::ElideRight,
                                                       textRect.width()));
    lineTop += lineHeight();

    if (!item.clip.tags.isEmpty())
    {
        QFont chipFont = metaFont;
        chipFont.setPixelSize(qMax(8, m_fontSize - 2));
        const QFontMetrics chipMetrics(chipFont);
        painter.setFont(chipFont);
        const int chipHeight = m_fontSize + 2;
        int x = textRect.left();
        int shown = 0;
        for (const QString& tag : item.clip.tags)
        {
            if (shown >= 3)
                break;
            const int width = chipMetrics.horizontalAdvance(tag) + 12;
            if (x + width > textRect.right() - 22 && shown > 0)
                break;
            const QRect chip(x, lineTop, width, chipHeight);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0x3A, 0x3A, 0x3F));
            painter.drawRoundedRect(chip, 5, 5);
            painter.setPen(QColor(0xC7, 0xC7, 0xCC));
            painter.drawText(chip, Qt::AlignCenter, tag);
            x += width + 4;
            ++shown;
        }
        if (shown < item.clip.tags.size())
        {
            const QString more = QStringLiteral("+%1").arg(item.clip.tags.size() - shown);
            const int width = chipMetrics.horizontalAdvance(more) + 12;
            if (x + width <= textRect.right())
            {
                const QRect chip(x, lineTop, width, chipHeight);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0x2F, 0x3A, 0x4A));
                painter.drawRoundedRect(chip, 5, 5);
                painter.setPen(QColor(0x8F, 0xB8, 0xE8));
                painter.drawText(chip, Qt::AlignCenter, more);
            }
        }
    }

    if (selected || active)
    {
        painter.setBrush(Qt::NoBrush);
        // The two frames are independent: green marks whatever is playing right
        // now, blue marks the clip the user picked. When both land on the same
        // card they must both stay visible - drawing only one of them made the
        // blue frame look like it vanished while the first clip played and came
        // back once sequential playback moved on.
        if (active)
        {
            painter.setPen(QPen(QColor(0x30, 0xD1, 0x58), 3));
            painter.drawRoundedRect(card.adjusted(1, 1, -1, -1), 10, 10);
        }
        if (selected)
        {
            const int inset = active ? 4 : 1;
            painter.setPen(QPen(QColor(0x0A, 0x84, 0xFF), 2));
            painter.drawRoundedRect(card.adjusted(inset, inset, -inset, -inset), 9, 9);
        }
    }
}

void LoopGridWidget::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRect clip = event->rect();
    const int rowStride = cardHeight() + rowGap();
    const int firstRow = qMax(0, clip.top() / qMax(1, rowStride));
    const int lastRow = clip.bottom() / qMax(1, rowStride);
    const int columns = columnCount();

    for (int row = firstRow; row <= lastRow; ++row)
    {
        for (int column = 0; column < columns; ++column)
        {
            const int index = row * columns + column;
            if (index >= m_items.size())
                break;
            const LoopGridItem& item = m_items.at(index);
            const bool selected = item.clip.id == m_selectedClipId;
            const bool active = item.clip.id == m_activeClipId;
            drawCard(painter, cardRectAt(index), item, selected, index == m_hoveredIndex,
                     active);
        }
    }
}

void LoopGridWidget::mousePressEvent(QMouseEvent* event)
{
    const int index = indexAt(event->pos());
    if (index < 0)
    {
        setSelectedClipId(QString());
        return;
    }
    setSelectedClipId(m_items.at(index).clip.id);
}

void LoopGridWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    const int index = indexAt(event->pos());
    if (index >= 0)
        emit clipActivated(m_items.at(index).clip.id);
}

void LoopGridWidget::mouseMoveEvent(QMouseEvent* event)
{
    const int index = indexAt(event->pos());
    if (index == m_hoveredIndex)
        return;
    m_hoveredIndex = index;
    emit hoveredClipChanged(index >= 0 ? m_items.at(index).clip.id : QString(),
                            event->globalPosition().toPoint());
    update();
}

void LoopGridWidget::leaveEvent(QEvent*)
{
    if (m_hoveredIndex < 0)
        return;
    m_hoveredIndex = -1;
    emit hoveredClipChanged(QString(), QPoint());
    update();
}

void LoopGridWidget::contextMenuEvent(QContextMenuEvent* event)
{
    const int index = indexAt(event->pos());
    if (index < 0)
    {
        emit contextMenuRequested(QString(), event->globalPos());
        return;
    }
    setSelectedClipId(m_items.at(index).clip.id);
    emit contextMenuRequested(m_items.at(index).clip.id, event->globalPos());
}

void LoopGridWidget::moveSelection(int delta)
{
    if (m_items.isEmpty())
        return;

    int index = indexOfClip(m_selectedClipId);
    index = qBound(0, index < 0 ? 0 : index + delta, m_items.size() - 1);
    setSelectedClipId(m_items.at(index).clip.id);

    const QRect rect = cardRectAt(index);
    if (QScrollArea* area = scrollArea())
        area->ensureVisible(rect.center().x(), rect.center().y(), 20, 40);
}

QScrollArea* LoopGridWidget::scrollArea() const
{
    QWidget* parent = parentWidget();
    while (parent)
    {
        if (auto* area = qobject_cast<QScrollArea*>(parent))
            return area;
        parent = parent->parentWidget();
    }
    return nullptr;
}

void LoopGridWidget::keyPressEvent(QKeyEvent* event)
{
    switch (event->key())
    {
    case Qt::Key_Left:
        moveSelection(-1);
        break;
    case Qt::Key_Right:
        moveSelection(1);
        break;
    case Qt::Key_Up:
        moveSelection(-columnCount());
        break;
    case Qt::Key_Down:
        moveSelection(columnCount());
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (!m_selectedClipId.isEmpty())
            emit clipActivated(m_selectedClipId);
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void LoopGridWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    refreshColumnCount();
}
