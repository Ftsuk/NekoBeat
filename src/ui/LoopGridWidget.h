#pragma once

#include "core/LoopClip.h"

#include <QCache>
#include <QHash>
#include <QList>
#include <QPixmap>
#include <QStringList>
#include <QWidget>

class QScrollArea;
class QFontMetrics;

// One card of the loop list. The panel resolves thumbnails and folder names so
// the widget itself stays a pure painter.
struct LoopGridItem
{
    LoopClip clip;
    // Frame grabbed at the A point; empty until the background job finished.
    QString thumbnailPath;
    // Cover of the source video, used while the clip frame is missing.
    QString fallbackThumbnailPath;
    QStringList folderNames;
    // Script axis count of the source video, shown as 单轴/多轴 before the
    // title exactly like the media library does.
    int scriptAxes = 0;
    bool missing = false;
};

// Cover grid for loop clips. Same layout rules as the media library: the cards
// keep their pixel width and the columns simply re-flow.
class LoopGridWidget : public QWidget
{
    Q_OBJECT

public:
    explicit LoopGridWidget(QWidget* parent = nullptr);

    void setItems(const QList<LoopGridItem>& items);
    void setCardWidth(int width);
    void setFontSize(int pixelSize);
    void setPixmapCache(QCache<QString, QPixmap>* cache);
    void invalidatePixmap(const QString& path);
    // Clip that is currently looping; gets a highlighted border.
    void setActiveClipId(const QString& clipId);
    // Whether the 单轴 / 多轴 badge is drawn in front of the title.
    void setShowAxisTag(bool show);

    QString selectedClipId() const { return m_selectedClipId; }
    void setSelectedClipId(const QString& clipId);
    void ensureVisibleClip(const QString& clipId);
    const LoopGridItem* itemFor(const QString& clipId) const;
    // Columns the covers are currently laid out in; public for the unit test
    // that pins the XTPlayer-like reflow rule.
    int columnCount() const;

signals:
    void clipActivated(const QString& clipId);
    void selectionChanged(const QString& clipId);
    void contextMenuRequested(const QString& clipId, const QPoint& globalPos);
    void hoveredClipChanged(const QString& clipId, const QPoint& globalPos);

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
    // Same layout rule as the media library grid: the previous column count is
    // the hysteresis anchor, so the right-most column only disappears once it is
    // covered by more than half and a new one only appears once it fits.
    int columnCountFor(int available, int previous) const;
    void refreshColumnCount();
    int columnGap() const;
    int cardHeight() const;
    int rowGap() const { return 8; }
    int lineHeight() const { return m_fontSize + 6; }
    int textAreaHeight() const;
    QRect cardRectAt(int index) const;
    int indexAt(const QPoint& pos) const;
    int indexOfClip(const QString& clipId) const;
    void updateGeometryForItems();
    // Cover already scaled and rounded for the requested size. Clipping each
    // card with a QPainterPath on every repaint was the most expensive part of
    // the grid, so the result is cached and blitted 1:1 instead.
    const QPixmap* coverPixmapFor(const QString& path, const QSize& size) const;
    // Cached title wrapping. wrapName() measures every prefix of the string,
    // which is far too expensive to repeat for every card on every repaint.
    QStringList wrappedTitle(const QString& text, const QFontMetrics& metrics,
                             int firstLineWidth, int otherLineWidth) const;
    void drawCard(QPainter& painter, const QRect& card, const LoopGridItem& item,
                  bool selected, bool hovered, bool active) const;
    void moveSelection(int delta);

    QList<LoopGridItem> m_items;
    QCache<QString, QPixmap>* m_cache = nullptr;
    mutable QHash<QString, QStringList> m_wrapCache;
    int m_cardWidth = 200;
    int m_fontSize = 12;
    // Column count of the last layout pass; the anchor of the hysteresis.
    int m_columns = 0;
    int m_hoveredIndex = -1;
    QString m_selectedClipId;
    QString m_activeClipId;
    bool m_showAxisTag = true;
};
