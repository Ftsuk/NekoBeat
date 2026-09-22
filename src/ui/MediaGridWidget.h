#pragma once

#include "core/MediaItem.h"

#include <QCache>
#include <QHash>
#include <QList>
#include <QPixmap>
#include <QSet>
#include <QWidget>

class QScrollArea;
class QFontMetrics;

// Cover grid with a fixed card size: the cards keep the size the user picked
// and the columns simply re-flow (one, two, three, ...) when the panel is
// resized - the same behaviour as XTPlayer.
class MediaGridWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MediaGridWidget(QWidget* parent = nullptr);

    void setItems(const QList<MediaItem>& items);
    void updateItem(const MediaItem& item);
    void setCardWidth(int width);
    void setFontSize(int pixelSize);
    void setPixmapCache(QCache<QString, QPixmap>* cache);
    void invalidatePixmap(const QString& path);
    // Normalised media paths that currently belong to at least one favourite
    // folder; only used for the star badge, the model stays untouched.
    void setFavoritePaths(const QSet<QString>& paths);
    // Whether the 单轴 / 多轴 badge is drawn in front of the file name.
    void setShowAxisTag(bool show);

    QString selectedPath() const { return m_selectedPath; }
    void setSelectedPath(const QString& path);
    void ensureVisiblePath(const QString& path);
    // Columns the covers are currently laid out in. Public because the layout
    // rule (keep the last column until it is half covered, add the next one only
    // once it fits) is covered by a unit test.
    int columnCount() const;

signals:
    void itemActivated(const QString& mediaPath);
    void selectionChanged(const QString& mediaPath);
    void contextMenuRequested(const QString& mediaPath, const QPoint& globalPos);
    void hoveredItemChanged(const QString& mediaPath, const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QScrollArea* scrollArea() const;
    // Column count for a given panel width. The previous count is the
    // hysteresis anchor: the right-most column is only dropped once the panel
    // edge has covered more than half of it, and a new column only appears once
    // it fits completely - the feel of XTPlayer's cover wall.
    int columnCountFor(int available, int previous) const;
    // Recomputes the column count from the current width and, when it changed,
    // refreshes the widget height.
    void refreshColumnCount();
    int columnGap() const;
    int cardHeight() const;
    int rowGap() const { return 8; }
    QRect cardRectAt(int index) const;
    int indexAt(const QPoint& pos) const;
    void updateGeometryForItems();
    // Cover already scaled and rounded for the requested size. Clipping each
    // card with a QPainterPath on every repaint was the most expensive part of
    // the grid, so the result is cached and blitted 1:1 instead.
    const QPixmap* coverPixmapFor(const QString& path, const QSize& size) const;
    // Cached title wrapping. wrapName() measures every prefix of the string,
    // which is far too expensive to repeat for every card on every repaint.
    QStringList wrappedName(const QString& text, const QFontMetrics& metrics,
                            int firstLineWidth, int otherLineWidth) const;
    void drawCard(QPainter& painter, const QRect& card, const MediaItem& item,
                  bool selected, bool hovered) const;
    int lineHeight() const { return m_fontSize + 6; }
    int textAreaHeight() const { return lineHeight() * 2 + 6; }
    void moveSelection(int delta);

    QList<MediaItem> m_items;
    QSet<QString> m_favoritePaths;
    QCache<QString, QPixmap>* m_cache = nullptr;
    mutable QHash<QString, QStringList> m_wrapCache;
    int m_cardWidth = 190;
    int m_fontSize = 12;
    // Column count of the last layout pass; the anchor of the hysteresis.
    int m_columns = 0;
    int m_hoveredIndex = -1;
    QString m_selectedPath;
    bool m_showAxisTag = true;
};
