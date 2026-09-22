#include "TestLanguage.h"
#include "ui/LoopGridWidget.h"
#include "ui/MediaGridWidget.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QtTest>

namespace
{
constexpr int kCardWidth = 200;
constexpr int kGap = 4;

// Width a row of `columns` covers fully.
int fullWidth(int columns)
{
    return columns * kCardWidth + (columns - 1) * kGap;
}

QList<MediaItem> mediaItems(int count)
{
    QList<MediaItem> items;
    for (int i = 0; i < count; ++i)
    {
        MediaItem item;
        item.path = QStringLiteral("C:/library/clip-%1.mp4").arg(i);
        item.displayName = QStringLiteral("clip-%1.mp4").arg(i);
        item.fileSize = 1024;
        items.append(item);
    }
    return items;
}

QList<LoopGridItem> loopItems(int count)
{
    QList<LoopGridItem> items;
    for (int i = 0; i < count; ++i)
    {
        LoopGridItem item;
        item.clip.id = QStringLiteral("clip-%1").arg(i);
        item.clip.title = QStringLiteral("clip-%1").arg(i);
        items.append(item);
    }
    return items;
}

// A hidden widget does not receive resize events (Qt defers them until it is
// shown), so the test drives the same handler a layout would: resize the widget
// and hand it the resize event.
void resizeWidget(QWidget* widget, int width, int height)
{
    widget->resize(width, height);
    QResizeEvent event(QSize(width, height), QSize(widget->width(), widget->height()));
    QApplication::sendEvent(widget, &event);
}
} // namespace

// The cover wall has to behave like XTPlayer's library list: the last column
// stays visible while at least half of it still fits, the next column only
// appears once it fits completely, and the width that is left over is never
// spread between the covers.
class GridLayoutTest : public QObject
{
    Q_OBJECT

private slots:
    void mediaGridKeepsHalfCoveredColumn()
    {
        MediaGridWidget grid;
        grid.setCardWidth(kCardWidth);
        grid.setItems(mediaItems(24));

        resizeWidget(&grid, fullWidth(3), 400);
        QCOMPARE(grid.columnCount(), 3);

        // Half of the third column is still visible: it must stay.
        resizeWidget(&grid, fullWidth(3) - kCardWidth / 2 + 1, 400);
        QCOMPARE(grid.columnCount(), 3);

        // More than half covered: the column goes away.
        resizeWidget(&grid, fullWidth(3) - kCardWidth / 2 - 1, 400);
        QCOMPARE(grid.columnCount(), 2);
    }

    void mediaGridOnlyAddsColumnWhenItFits()
    {
        MediaGridWidget grid;
        grid.setCardWidth(kCardWidth);
        grid.setItems(mediaItems(24));

        resizeWidget(&grid, fullWidth(2), 400);
        QCOMPARE(grid.columnCount(), 2);

        // One pixel short of fitting: still two columns.
        resizeWidget(&grid, fullWidth(3) - 1, 400);
        QCOMPARE(grid.columnCount(), 2);

        resizeWidget(&grid, fullWidth(3), 400);
        QCOMPARE(grid.columnCount(), 3);
    }

    void mediaGridColumnsFollowSeveralSteps()
    {
        MediaGridWidget grid;
        grid.setCardWidth(kCardWidth);
        grid.setItems(mediaItems(40));

        resizeWidget(&grid, fullWidth(1), 400);
        QCOMPARE(grid.columnCount(), 1);

        // A window resize (or a swapped layout) can jump straight to five
        // columns; the layout has to follow without a drag in between.
        resizeWidget(&grid, fullWidth(5), 400);
        QCOMPARE(grid.columnCount(), 5);

        resizeWidget(&grid, fullWidth(2), 400);
        QCOMPARE(grid.columnCount(), 2);
    }

    void mediaGridDropsHalfCoveredColumnWhenCoverSizeChanges()
    {
        MediaGridWidget grid;
        grid.setItems(mediaItems(24));

        grid.setCardWidth(kCardWidth);
        resizeWidget(&grid, fullWidth(3), 400);
        QCOMPARE(grid.columnCount(), 3);

        // 640 px is inside the hysteresis band: with 200 px covers the third
        // column is still more than half visible, so it stays.
        resizeWidget(&grid, 640, 400);
        QCOMPARE(grid.columnCount(), 3);

        // Picking a bigger cover means a fresh layout. Keeping the old column
        // count as the anchor would leave a half covered third column behind.
        grid.setCardWidth(250);
        QCOMPARE(grid.columnCount(), 2);
    }

    void loopGridUsesTheSameRule()
    {
        LoopGridWidget grid;
        grid.setCardWidth(kCardWidth);
        grid.setItems(loopItems(24));

        resizeWidget(&grid, fullWidth(3), 400);
        QCOMPARE(grid.columnCount(), 3);

        resizeWidget(&grid, fullWidth(3) - kCardWidth / 2 + 1, 400);
        QCOMPARE(grid.columnCount(), 3);

        resizeWidget(&grid, fullWidth(3) - kCardWidth / 2 - 1, 400);
        QCOMPARE(grid.columnCount(), 2);

        resizeWidget(&grid, fullWidth(2), 400);
        QCOMPARE(grid.columnCount(), 2);
        resizeWidget(&grid, fullWidth(3) - 1, 400);
        QCOMPARE(grid.columnCount(), 2);
        resizeWidget(&grid, fullWidth(3), 400);
        QCOMPARE(grid.columnCount(), 3);
    }

    void mediaGridHeightFollowsRowCount()
    {
        MediaGridWidget grid;
        grid.setCardWidth(kCardWidth);
        grid.setItems(mediaItems(7));

        resizeWidget(&grid, fullWidth(3), 400);
        const int threeColumnHeight = grid.minimumHeight();
        QVERIFY(threeColumnHeight > 0);

        // Only two columns fit, so the same seven covers need four rows.
        resizeWidget(&grid, fullWidth(2), 400);
        QCOMPARE(grid.columnCount(), 2);
        QVERIFY2(grid.minimumHeight() > threeColumnHeight,
                 "列数减少后内容必须变高，滚动条才知道要出现");
    }
};

QTEST_MAIN(GridLayoutTest)
#include "test_grid_layout.moc"
