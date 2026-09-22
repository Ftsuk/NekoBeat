#pragma once

#include <QObject>
#include <QRect>
#include <QString>
#include <QElapsedTimer>

class QSplitter;
class QToolButton;
class QWidget;
class QTimer;
class QGraphicsOpacityEffect;
class QPropertyAnimation;
class LibraryDragHint;
class QSplitterHandle;

// Owns the complete fold behaviour of one side panel: the floating chevron,
// the invisible grab strip on the window edge, the blue drag feedback, the
// snap-to-fold threshold and the splitter bookkeeping.
//
// The media library and the loop list share this controller so both sides feel
// identical; only the geometry is mirrored.
class SidePanelFold : public QObject
{
    Q_OBJECT

public:
    enum class Side
    {
        Left = 0,
        Right = 1
    };

    SidePanelFold(Side side, QSplitter* splitter, int panelIndex,
                  QWidget* panelHost, QWidget* overlayHost, QObject* parent = nullptr);
    ~SidePanelFold() override;

    // The button itself is created by the window (it needs a parent for
    // stacking); the controller owns its icon, tooltip and fade animation.
    void attachToggleButton(QToolButton* button, const QString& collapseText,
                            const QString& expandText);
    // Actual content widget; hidden while the panel is folded away.
    void setPanelWidget(QWidget* widget);

    bool isCollapsed() const { return m_collapsed; }
    // restoreWidth=false is used while dragging the panel out of the folded
    // edge: the width comes from the pointer instead of the remembered one.
    void setCollapsed(bool collapsed, bool restoreWidth = true);
    void toggle() { setCollapsed(!m_collapsed); }

    int panelWidth() const;
    void setPanelWidth(int width);
    int snapWidth() const { return m_snapWidth; }

    // Full screen playback hides everything this controller draws on the window
    // edges: the chevron and the invisible grab strip would otherwise float on
    // top of the picture.
    void setFullScreen(bool fullScreen);
    bool isFullScreen() const { return m_fullScreen; }

    // While the window is full screen the panel itself is hosted in the overlay
    // of the video area, i.e. it is out of the splitter. The controller must not
    // read the splitter or react to its signals in that state: the layout churn
    // of the switch looks exactly like a user drag and used to fold the panel
    // behind the user's back (or leave the chevron showing the wrong state).
    void setHosted(bool hosted);
    bool isHosted() const { return m_hosted; }

    // Called from the window's resizeEvent and after layout changes.
    void updateGeometry();
    void updateDragHint();
    void hideDragHint();

    // Moves this controller to the other side of the window. Used by the
    // "swap panels" layout option: the panels keep their widgets and state, only
    // their position (and therefore the mirrored geometry) changes.
    void setPlacement(Side side, int panelIndex);

signals:
    void collapsedChanged(bool collapsed);
    void widthChanged(int width);
    void toggleClicked();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    int middleIndex() const;
    int handleIndex() const;
    void applyPanelWidth(int width);
    void applyEdgeUi();
    void setToggleVisible(bool visible);
    void refreshToggleAppearance();
    void finishEdgeDrag(bool clicked);
    QRect edgeHotZone() const;
    // True while the user is actively dragging this panel (mouse held down or
    // splitter signals still arriving); drives both the glow and the snap check.
    bool isDragging() const;

    Side m_side = Side::Left;
    QSplitter* m_splitter = nullptr;
    int m_panelIndex = 0;
    QWidget* m_panelHost = nullptr;
    QWidget* m_overlayHost = nullptr;
    QWidget* m_panelWidget = nullptr;
    QToolButton* m_toggleButton = nullptr;
    QWidget* m_edgeHandle = nullptr;
    QSplitterHandle* m_splitterHandle = nullptr;
    LibraryDragHint* m_dragHint = nullptr;
    QGraphicsOpacityEffect* m_toggleOpacity = nullptr;
    QPropertyAnimation* m_toggleAnimation = nullptr;
    QTimer* m_toggleHoverTimer = nullptr;
    QTimer* m_snapTimer = nullptr;
    QString m_collapseText;
    QString m_expandText;
    int m_width = 360;
    int m_snapWidth = 210;
    bool m_collapsed = false;
    bool m_fullScreen = false;
    bool m_hosted = false;
    bool m_edgeDragging = false;
    bool m_edgeDragMoved = false;
    // Where the press on the edge strip started, so a click can be told apart
    // from a drag by how far the pointer actually moved - not by where on the
    // strip it happened to land.
    QPoint m_edgePressPos;
    bool m_toggleHovered = false;
    // Set while the controller is applying its own layout, so the splitter
    // signals fired by that change are not mistaken for user drags.
    bool m_applyingLayout = false;
    // Width of the previous drag step, used to tell whether the user is
    // currently shrinking or growing the panel.
    int m_lastHintWidth = 0;
    int m_dragDirection = 0; // LibraryDragHint::Mode as int (0 = Collapse)
    bool m_hasLastWidth = false;
    bool m_wasMouseDown = false;
    QElapsedTimer m_lastDragTick;
    QElapsedTimer m_stateTimer;
};
