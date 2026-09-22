#include "core/Loc.h"
#include "ui/MediaGridWidget.h"

#include "core/FavoritesStore.h"
#include "core/MediaLibraryStore.h"

#include <QContextMenuEvent>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollArea>

namespace
{
constexpr int kCardPadding = 4;
// Space between two columns. Deliberately a constant: XTPlayer lays its covers
// out in a fixed cell and lets the leftover width sit at the right edge, it
// never spreads it between the cards. Stretching the gap made the covers crawl
// sideways for every pixel of the divider drag.
constexpr int kColumnGap = 4;

QString formatFileSize(qint64 bytes)
{
    if (bytes <= 0)
        return LT("未知");

    const double kb = bytes / 1024.0;
    const double mb = kb / 1024.0;
    const double gb = mb / 1024.0;
    if (gb >= 1.0)
        return QStringLiteral("%1 GB").arg(gb, 0, 'f', 2);
    if (mb >= 1.0)
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(kb, 0, 'f', 0);
}

QString codecLabel(const QString& codec)
{
    if (codec.isEmpty())
        return LT("未知");

    QString value = codec;
    const int separator = value.indexOf(QStringLiteral(" / "));
    if (separator > 0)
        value = value.left(separator);
    const int parenthesis = value.indexOf(QLatin1Char('('));
    if (parenthesis > 0)
        value = value.left(parenthesis);
    return value.trimmed();
}

QString containerLabel(const QString& container)
{
    if (container.isEmpty())
        return LT("未知");

    const QString lower = container.toLower();
    if (lower.contains(QStringLiteral("mp4")))
        return QStringLiteral("MP4");
    if (lower.contains(QStringLiteral("matroska")))
        return QStringLiteral("MKV");
    if (lower.contains(QStringLiteral("webm")))
        return QStringLiteral("WebM");
    if (lower.contains(QStringLiteral("avi")))
        return QStringLiteral("AVI");
    if (lower.contains(QStringLiteral("asf")))
        return QStringLiteral("WMV");
    if (lower.contains(QStringLiteral("mpegts")))
        return QStringLiteral("TS");
    if (lower.contains(QStringLiteral("mov")))
        return QStringLiteral("MOV");

    const int comma = container.indexOf(QLatin1Char(','));
    return (comma > 0 ? container.left(comma) : container).trimmed();
}

QString scriptTag(int axes)
{
    if (axes <= 0)
        return LT("无脚本");
    return axes == 1 ? LT("单轴") : LT("多轴");
}

QColor scriptTagColor(int axes)
{
    if (axes <= 0)
        return QColor(0x98, 0x98, 0x9D);
    return axes == 1 ? QColor(0x6B, 0xB4, 0xFF) : QColor(0x30, 0xD1, 0x58);
}

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

MediaGridWidget::MediaGridWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("MediaGrid"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void MediaGridWidget::setItems(const QList<MediaItem>& items)
{
    m_items = items;
    if (!m_selectedPath.isEmpty()
        && std::none_of(m_items.cbegin(), m_items.cend(),
                        [this](const MediaItem& item) {
                            return item.path.compare(m_selectedPath,
                                                     Qt::CaseInsensitive) == 0;
                        }))
    {
        m_selectedPath.clear();
    }
    m_hoveredIndex = -1;
    refreshColumnCount();
    update();
}

void MediaGridWidget::updateItem(const MediaItem& item)
{
    for (int i = 0; i < m_items.size(); ++i)
    {
        if (m_items.at(i).path.compare(item.path, Qt::CaseInsensitive) != 0)
            continue;
        m_items[i] = item;
        update(cardRectAt(i));
        return;
    }
}

void MediaGridWidget::setCardWidth(int width)
{
    m_cardWidth = qBound(90, width, 480);
    // A different cover size is a fresh layout: keeping the old column count as
    // the anchor would leave the last column half covered for no reason.
    m_columns = 0;
    refreshColumnCount();
    update();
}

void MediaGridWidget::setFontSize(int pixelSize)
{
    m_fontSize = qBound(10, pixelSize, 22);
    m_wrapCache.clear();
    updateGeometryForItems();
    update();
}

void MediaGridWidget::setPixmapCache(QCache<QString, QPixmap>* cache)
{
    m_cache = cache;
}

void MediaGridWidget::invalidatePixmap(const QString& path)
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

void MediaGridWidget::setFavoritePaths(const QSet<QString>& paths)
{
    if (m_favoritePaths == paths)
        return;
    m_favoritePaths = paths;
    update();
}

void MediaGridWidget::setShowAxisTag(bool show)
{
    if (m_showAxisTag == show)
        return;
    m_showAxisTag = show;
    update();
}

void MediaGridWidget::setSelectedPath(const QString& path)
{
    if (m_selectedPath.compare(path, Qt::CaseInsensitive) == 0)
        return;
    m_selectedPath = path;
    update();
    emit selectionChanged(m_selectedPath);
}

void MediaGridWidget::ensureVisiblePath(const QString& path)
{
    for (int i = 0; i < m_items.size(); ++i)
    {
        if (m_items.at(i).path.compare(path, Qt::CaseInsensitive) != 0)
            continue;
        const QRect rect = cardRectAt(i);
        if (QScrollArea* area = scrollArea())
            area->ensureVisible(rect.center().x(), rect.center().y(), 20, 40);
        return;
    }
}

int MediaGridWidget::columnCount() const
{
    return qMax(1, m_columns);
}

int MediaGridWidget::columnCountFor(int available, int previous) const
{
    if (available <= 0)
        return 1;

    // Width the last column of `columns` columns needs before the panel edge
    // starts covering it.
    const auto fullWidth = [this](int columns) {
        return columns * m_cardWidth + (columns - 1) * kColumnGap;
    };

    int columns = qMax(1, previous);
    // Narrowing: keep the column until the edge covers more than half of it.
    while (columns > 1 && available < fullWidth(columns) - m_cardWidth / 2)
        --columns;
    // Widening: only bring the next column in once it fits completely.
    while (available >= fullWidth(columns + 1))
        ++columns;
    return columns;
}

void MediaGridWidget::refreshColumnCount()
{
    const int columns = columnCountFor(width(), m_columns);
    if (columns != m_columns)
        m_columns = columns;
    updateGeometryForItems();
}

int MediaGridWidget::columnGap() const
{
    return columnCount() > 1 ? kColumnGap : 0;
}

int MediaGridWidget::cardHeight() const
{
    return m_cardWidth * 9 / 16 + textAreaHeight() + 14;
}

QRect MediaGridWidget::cardRectAt(int index) const
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

int MediaGridWidget::indexAt(const QPoint& pos) const
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

void MediaGridWidget::updateGeometryForItems()
{
    const int columns = columnCount();
    const int rows = columns > 0 ? (m_items.size() + columns - 1) / columns : 1;
    const int neededHeight = qMax(0, rows * (cardHeight() + rowGap()));
    if (height() != neededHeight)
        setMinimumHeight(neededHeight);
}

QStringList MediaGridWidget::wrappedName(const QString& text, const QFontMetrics& metrics,
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

const QPixmap* MediaGridWidget::coverPixmapFor(const QString& path, const QSize& size) const
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

void MediaGridWidget::drawCard(QPainter& painter, const QRect& card,
                               const MediaItem& item, bool selected,
                               bool hovered) const
{
    // A missing file keeps its card and cover, drained of colour, with an
    // explicit badge saying why. Silently dropping the entry would hide the
    // fact that favourite and loop records still point at it.
    if (item.missing)
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(hovered ? QColor(0x2E, 0x2A, 0x2A) : QColor(0x26, 0x23, 0x23));
        painter.drawRoundedRect(card, 10, 10);

        const QRect content = card.adjusted(kCardPadding, kCardPadding, -kCardPadding,
                                            -kCardPadding);
        const int textArea = textAreaHeight();
        const QRect coverRect(content.left(), content.top(), content.width(),
                              content.height() - textArea);
        const QRect textRect(content.left() + 2, coverRect.bottom() + 3,
                             content.width() - 4, textArea - 4);

        const QPixmap* cover = item.thumbnailState == ThumbnailState::Ready
                                   ? coverPixmapFor(item.thumbnailPath, coverRect.size())
                                   : nullptr;
        painter.setOpacity(0.32);
        if (cover)
        {
            painter.drawPixmap(coverRect.topLeft(), *cover);
        }
        else
        {
            painter.setBrush(QColor(0x1B, 0x1B, 0x1D));
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(coverRect, 8, 8);
        }
        painter.setOpacity(1.0);

        QFont badgeFont = painter.font();
        badgeFont.setPixelSize(qMax(9, m_fontSize - 1));
        const QFontMetrics badgeMetrics(badgeFont);
        const QString label = LT("失效");
        QRect badge(0, 0, badgeMetrics.horizontalAdvance(label) + 12, m_fontSize + 5);
        badge.moveTopLeft(coverRect.topLeft() + QPoint(6, 6));
        painter.setFont(badgeFont);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x8A, 0x2B, 0x2B, 225));
        painter.drawRoundedRect(badge, 5, 5);
        painter.setPen(QColor(0xFF, 0xD5, 0xD5));
        painter.drawText(badge, Qt::AlignCenter, label);

        QFont nameFont = painter.font();
        nameFont.setPixelSize(m_fontSize);
        const QFontMetrics metrics(nameFont);
        const QStringList lines = wrappedName(item.displayName, metrics,
                                              textRect.width(), textRect.width());
        painter.setFont(nameFont);
        painter.setPen(QColor(0x8C, 0x8C, 0x93));
        int lineTop = textRect.top();
        for (const QString& line : lines)
        {
            painter.drawText(QRect(textRect.left(), lineTop, textRect.width(), lineHeight()),
                             Qt::AlignLeft | Qt::AlignVCenter, line);
            lineTop += lineHeight();
        }

        if (selected)
        {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(0x0A, 0x84, 0xFF), 2));
            painter.drawRoundedRect(card.adjusted(1, 1, -1, -1), 10, 10);
        }
        return;
    }

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

    const QPixmap* cover = item.thumbnailState == ThumbnailState::Ready
                               ? coverPixmapFor(item.thumbnailPath, coverRect.size())
                               : nullptr;

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

    if (m_favoritePaths.contains(FavoritesStore::normalizePath(item.path)))
    {
        painter.save();
        QFont starFont = painter.font();
        starFont.setPixelSize(qMax(11, m_fontSize + 1));
        const QString star = QStringLiteral("★");
        const QFontMetrics starMetrics(starFont);
        QRect starBadge(0, 0, starMetrics.horizontalAdvance(star) + 10,
                        m_fontSize + 4);
        starBadge.moveTopLeft(coverRect.topLeft() + QPoint(6, 6));
        painter.setFont(starFont);
        painter.setBrush(QColor(0, 0, 0, 160));
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(starBadge, 5, 5);
        painter.setPen(QColor(0xFF, 0xD6, 0x0A));
        painter.drawText(starBadge, Qt::AlignCenter, star);
        painter.restore();
    }

    QFont badgeFont = painter.font();
    badgeFont.setPixelSize(qMax(8, m_fontSize - 3));
    const QFontMetrics badgeMetrics(badgeFont);
    const QString duration = item.metadataReady
                                 ? MediaLibraryStore::formatDuration(item.durationMs)
                                 : QStringLiteral("…");
    const int badgeWidth = badgeMetrics.horizontalAdvance(duration) + 12;
    const int badgeHeight = m_fontSize + 4;
    QRect badge(0, 0, badgeWidth, badgeHeight);
    badge.moveBottomRight(coverRect.bottomRight() - QPoint(6, 6));
    painter.setFont(badgeFont);
    painter.setBrush(QColor(0, 0, 0, 175));
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(badge, 5, 5);
    painter.setPen(QColor(0xFF, 0xFF, 0xFF));
    painter.drawText(badge, Qt::AlignCenter, duration);

    QFont nameFont = painter.font();
    nameFont.setPixelSize(m_fontSize);
    nameFont.setBold(false);
    const QFontMetrics metrics(nameFont);
    const QString tag = scriptTag(item.scriptAxes);
    const int tagWidth = m_showAxisTag ? metrics.horizontalAdvance(tag) + 8 : 0;
    const QStringList lines = wrappedName(item.displayName, metrics,
                                          textRect.width() - tagWidth,
                                          textRect.width());

    painter.setFont(nameFont);
    int lineTop = textRect.top();
    if (m_showAxisTag && tagWidth < textRect.width())
    {
        painter.setPen(scriptTagColor(item.scriptAxes));
        painter.drawText(QRect(textRect.left(), lineTop, tagWidth, lineHeight()),
                         Qt::AlignLeft | Qt::AlignVCenter, tag);
    }
    painter.setPen(selected ? QColor(0xFF, 0xFF, 0xFF) : QColor(0xEC, 0xEC, 0xF1));
    for (int i = 0; i < lines.size(); ++i)
    {
        const QRect lineRect = (i == 0 && m_showAxisTag && tagWidth < textRect.width())
                                   ? QRect(textRect.left() + tagWidth, lineTop,
                                           textRect.width() - tagWidth, lineHeight())
                                   : QRect(textRect.left(), lineTop, textRect.width(),
                                           lineHeight());
        painter.drawText(lineRect, Qt::AlignLeft | Qt::AlignVCenter, lines.at(i));
        lineTop += lineHeight();
    }

    if (selected)
    {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0x0A, 0x84, 0xFF), 2));
        painter.drawRoundedRect(card.adjusted(1, 1, -1, -1), 10, 10);
    }
}

void MediaGridWidget::paintEvent(QPaintEvent* event)
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
            const MediaItem& item = m_items.at(index);
            const bool selected =
                item.path.compare(m_selectedPath, Qt::CaseInsensitive) == 0;
            drawCard(painter, cardRectAt(index), item, selected,
                     index == m_hoveredIndex);
        }
    }

}

void MediaGridWidget::mousePressEvent(QMouseEvent* event)
{
    const int index = indexAt(event->pos());
    if (index < 0)
    {
        setSelectedPath(QString());
        return;
    }
    setSelectedPath(m_items.at(index).path);
}

void MediaGridWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    const int index = indexAt(event->pos());
    if (index >= 0)
        emit itemActivated(m_items.at(index).path);
}

void MediaGridWidget::mouseMoveEvent(QMouseEvent* event)
{
    const int index = indexAt(event->pos());
    if (index == m_hoveredIndex)
        return;
    m_hoveredIndex = index;
    emit hoveredItemChanged(index >= 0 ? m_items.at(index).path : QString(),
                            event->globalPosition().toPoint());
    update();
}

void MediaGridWidget::leaveEvent(QEvent*)
{
    if (m_hoveredIndex < 0)
        return;
    m_hoveredIndex = -1;
    emit hoveredItemChanged(QString(), QPoint());
    update();
}

void MediaGridWidget::contextMenuEvent(QContextMenuEvent* event)
{
    const int index = indexAt(event->pos());
    if (index < 0)
    {
        emit contextMenuRequested(QString(), event->globalPos());
        return;
    }
    setSelectedPath(m_items.at(index).path);
    emit contextMenuRequested(m_items.at(index).path, event->globalPos());
}

void MediaGridWidget::moveSelection(int delta)
{
    if (m_items.isEmpty())
        return;

    int index = -1;
    for (int i = 0; i < m_items.size(); ++i)
    {
        if (m_items.at(i).path.compare(m_selectedPath, Qt::CaseInsensitive) == 0)
        {
            index = i;
            break;
        }
    }
    index = qBound(0, index < 0 ? 0 : index + delta, m_items.size() - 1);
    setSelectedPath(m_items.at(index).path);

    const QRect rect = cardRectAt(index);
    if (QScrollArea* area = scrollArea())
        area->ensureVisible(rect.center().x(), rect.center().y(), 20, 40);
}

QScrollArea* MediaGridWidget::scrollArea() const
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

void MediaGridWidget::keyPressEvent(QKeyEvent* event)
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
        if (!m_selectedPath.isEmpty())
            emit itemActivated(m_selectedPath);
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void MediaGridWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    refreshColumnCount();
}
