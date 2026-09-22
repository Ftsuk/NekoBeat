#include "core/Loc.h"
#include "ui/SidePanelFold.h"

#include "ui/LibraryDragHint.h"
#include "ui/Theme.h"

#include <QCursor>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QSplitter>
#include <QSplitterHandle>
#include <QTimer>
#include <QToolButton>
#include <QWidget>

#include <algorithm>
#include <numeric>

namespace
{
// Folding threshold, and at the same time the fixed width of the drag glow:
// wider than this the release keeps the panel open, narrower it folds away.
constexpr int kSnapWidth = 48;
constexpr int kEdgeGrabWidth = 26;
// How far the pointer may wander while the button is down and the gesture still
// counts as a click rather than a drag. Small enough that dragging the panel out
// follows the pointer immediately, large enough to survive a shaky hand.
constexpr int kEdgeClickSlop = 5;
// Must stay clearly above kSnapWidth: a panel expanded at exactly the snap
// width would otherwise be folded again by the snap logic one tick later.
constexpr int kMinimumPanelWidth = 260;
constexpr int kMinimumVideoWidth = 200;
// Fallback fold threshold for the case where no usable splitter signal arrived.
// Kept well below kSnapWidth so a legitimately narrow panel (the user parked it
// just above the snap threshold) is never folded behind their back.
constexpr int kSafetyFoldWidth = 18;
// Strength of the glow once the panel sits right on the fold threshold.
constexpr qreal kGlowIntensity = 0.9;
// After a programmatic fold/unfold the panel is left alone for this long so a
// transient width cannot immediately snap it back.
constexpr int kSettleMs = 450;
// The chevron of a folded panel stays faintly visible instead of disappearing
// completely, so the edge never looks like a dead area.
constexpr qreal kIdleToggleOpacity = 0.7;
// The glow marks the fold zone. It only fades in once the panel is this close to
// the snap threshold, so a comfortably wide panel can be dragged around without
// carrying a permanent blue band over the covers.
constexpr int kGlowRevealWidth = 168;

// 0 while the panel is still wide, kGlowIntensity right at the fold threshold.
// The ramp is what makes "keep going and it folds" feel magnetic: the closer the
// divider gets to the edge, the more the strip lights up.
qreal glowIntensityForWidth(int panelWidth, int snapWidth)
{
    if (panelWidth >= kGlowRevealWidth)
        return 0.0;
    const int span = std::max(1, kGlowRevealWidth - snapWidth);
    const qreal ratio =
        std::clamp((kGlowRevealWidth - panelWidth) / static_cast<qreal>(span), 0.0, 1.0);
    return kGlowIntensity * ratio;
}
}

SidePanelFold::SidePanelFold(Side side, QSplitter* splitter, int panelIndex,
                             QWidget* panelHost, QWidget* overlayHost, QObject* parent)
    : QObject(parent)
    , m_side(side)
    , m_splitter(splitter)
    , m_panelIndex(panelIndex)
    , m_panelHost(panelHost)
    , m_overlayHost(overlayHost)
    , m_snapWidth(kSnapWidth)
{
    m_stateTimer.start();
    if (!m_overlayHost)
        m_overlayHost = m_splitter;

    // Invisible grab strip on the window edge: while the panel is folded the
    // user can simply drag it open from the border.
    m_edgeHandle = new QWidget(m_overlayHost);
    m_edgeHandle->setCursor(Qt::SplitHCursor);
    m_edgeHandle->setToolTip(m_side == Side::Left
                                 ? LT("向右拖动展开媒体库")
                                 : LT("向左拖动展开循环列表"));
    m_edgeHandle->setAttribute(Qt::WA_TranslucentBackground, true);
    m_edgeHandle->installEventFilter(this);
    m_edgeHandle->setVisible(false);

    m_dragHint = new LibraryDragHint(m_overlayHost);
    m_dragHint->setSide(m_side == Side::Left ? LibraryDragHint::Side::Left
                                             : LibraryDragHint::Side::Right);
    m_dragHint->hide();

    m_toggleHoverTimer = new QTimer(this);
    m_toggleHoverTimer->setInterval(120);
    connect(m_toggleHoverTimer, &QTimer::timeout, this, [this]() {
        // Nothing of this controller is on screen while the window is full
        // screen, and there is no splitter handle to drag either.
        if (!m_splitter || m_fullScreen || m_hosted)
            return;

        if (!m_collapsed)
        {
            // Safety net: if the drag ended narrower than the snap width but
            // produced no usable splitterMoved signal, fold the panel here.
            // The check is skipped while the user still holds the mouse.
            if (!m_applyingLayout && m_stateTimer.elapsed() > kSettleMs
                && m_panelWidget && m_panelWidget->isVisible()
                && !(QGuiApplication::mouseButtons() & Qt::LeftButton)
                && panelWidth() > 0 && panelWidth() <= kSafetyFoldWidth)
            {
                setCollapsed(true);
                return;
            }
            // Keep the floating chevron glued to the panel while it is open.
            updateGeometry();
            return;
        }

        // While dragging the panel out of the edge the drag hint takes over;
        // the chevron would sit on top of the arrow.
        if (m_edgeDragging || m_hosted)
        {
            if (m_toggleHovered)
            {
                m_toggleHovered = false;
                setToggleVisible(false);
            }
            return;
        }
        const bool inside = edgeHotZone().contains(QCursor::pos());
        if (inside == m_toggleHovered)
            return;
        m_toggleHovered = inside;
        setToggleVisible(inside);
    });
    m_toggleHoverTimer->start();

    m_snapTimer = new QTimer(this);
    m_snapTimer->setSingleShot(true);
    connect(m_snapTimer, &QTimer::timeout, this, [this]() {
        if (m_collapsed || m_hosted)
            return;
        // While the button is still held the user is in the middle of a drag:
        // keep the glow on screen and keep waiting, so the decision is always
        // taken from the position where they let go.
        if (isDragging())
        {
            m_snapTimer->start(120);
            return;
        }
        hideDragHint();
        if (panelWidth() <= m_snapWidth)
            setCollapsed(true);
    });

    if (m_splitter)
    {
        // Double clicking the splitter handle toggles this panel instead of
        // letting QSplitter collapse it silently (which would desync the state).
        if (QSplitterHandle* handle = m_splitter->handle(handleIndex()))
        {
            m_splitterHandle = handle;
            handle->installEventFilter(this);
        }
        connect(m_splitter, &QSplitter::splitterMoved, this, [this](int, int index) {
            // Layout changes made by this controller must never be treated as
            // user drags (they would overwrite the remembered panel width).
            // The same applies while the panel is hosted in the full screen
            // overlay: the reparenting of the switch resizes the splitter and
            // must not be mistaken for a drag.
            if (m_applyingLayout || m_hosted)
                return;
            // QSplitter reports the index of the widget to the *right* of the
            // dragged handle, so a [left][video][right] splitter sends 1 for the
            // left handle and 2 for the right one. Accepting the handle index as
            // well used to make the opposite panel treat the other divider's
            // drag as its own and light up its glow.
            if (index != handleIndex() + 1)
                return;
            m_lastDragTick.restart();
            if (!m_collapsed && m_panelWidget && m_panelWidget->isVisible())
            {
                const int width = panelWidth();
                if (width > 0)
                {
                    m_width = width;
                    emit widthChanged(width);
                }
            }
            updateGeometry();
            updateDragHint();
            if (m_snapTimer)
                m_snapTimer->start(120);
        });
    }
}

SidePanelFold::~SidePanelFold() = default;

void SidePanelFold::attachToggleButton(QToolButton* button, const QString& collapseText,
                                       const QString& expandText)
{
    m_toggleButton = button;
    m_collapseText = collapseText;
    m_expandText = expandText;
    if (!m_toggleButton)
        return;

    m_toggleButton->setObjectName(QStringLiteral("LibraryToggle"));
    m_toggleButton->setIconSize(QSize(16, 16));
    m_toggleButton->setCursor(Qt::PointingHandCursor);
    m_toggleButton->setFixedSize(22, 46);

    // While folded the handle hides and fades in when the pointer reaches the
    // window edge.
    m_toggleOpacity = new QGraphicsOpacityEffect(m_toggleButton);
    m_toggleOpacity->setOpacity(1.0);
    m_toggleButton->setGraphicsEffect(m_toggleOpacity);
    m_toggleAnimation = new QPropertyAnimation(m_toggleOpacity, "opacity", this);
    m_toggleAnimation->setDuration(160);

    connect(m_toggleButton, &QToolButton::clicked, this, [this]() {
        emit toggleClicked();
        toggle();
    });
    m_toggleButton->installEventFilter(this);
    refreshToggleAppearance();
}

void SidePanelFold::setPanelWidget(QWidget* widget)
{
    m_panelWidget = widget;
}

int SidePanelFold::middleIndex() const
{
    // The layout is always [left panel][video column][right panel].
    return 1;
}

int SidePanelFold::handleIndex() const
{
    return m_side == Side::Left ? m_panelIndex : m_panelIndex - 1;
}

int SidePanelFold::panelWidth() const
{
    if (!m_splitter)
        return 0;
    const QList<int> sizes = m_splitter->sizes();
    return m_panelIndex >= 0 && m_panelIndex < sizes.size() ? sizes.at(m_panelIndex) : 0;
}

void SidePanelFold::applyPanelWidth(int width)
{
    if (!m_splitter)
        return;
    QList<int> sizes = m_splitter->sizes();
    const int middle = middleIndex();
    if (m_panelIndex >= sizes.size() || middle >= sizes.size())
        return;

    const int total = std::accumulate(sizes.cbegin(), sizes.cend(), 0);
    int fixed = 0;
    for (int i = 0; i < sizes.size(); ++i)
    {
        if (i != m_panelIndex && i != middle)
            fixed += sizes.at(i);
    }

    const int maxPanel = std::max(kMinimumPanelWidth, total - fixed - kMinimumVideoWidth);
    const int panel = std::clamp(width, 0, maxPanel);
    sizes[m_panelIndex] = panel;
    sizes[middle] = std::max(1, total - fixed - panel);
    m_splitter->setSizes(sizes);
}

void SidePanelFold::setPanelWidth(int width)
{
    m_width = std::max(kMinimumPanelWidth, width);
    if (!m_collapsed)
        applyPanelWidth(m_width);
}

void SidePanelFold::setCollapsed(bool collapsed, bool restoreWidth)
{
    if (!m_splitter || !m_panelHost || !m_panelWidget)
        return;

    // Update the state first: everything below (layout, geometry, the
    // splitter's own signals) reads it, so a half-applied state can never be
    // observed and flash the panel.
    const bool changed = m_collapsed != collapsed;
    m_collapsed = collapsed;

    if (m_hosted)
    {
        // Hosted in the full screen overlay: the panel is not part of the
        // splitter right now, so only its own visibility changes here. Touching
        // the splitter would collapse the (already empty) column and leave the
        // windowed layout in the wrong state after leaving full screen.
        if (collapsed && m_panelWidget->isVisible() && panelWidth() > m_snapWidth)
            m_width = panelWidth();
        m_panelWidget->setVisible(!collapsed);
        if (m_toggleOpacity)
            m_toggleOpacity->setOpacity(collapsed ? kIdleToggleOpacity : 1.0);
        refreshToggleAppearance();
        if (m_toggleButton)
            m_toggleButton->setAttribute(Qt::WA_TransparentForMouseEvents, collapsed);
        if (changed)
            emit collapsedChanged(collapsed);
        return;
    }

    m_applyingLayout = true;

    if (collapsed)
    {
        // Only remember a sensible width; snapping from a sliver should not
        // make the next expand just as narrow.
        if (panelWidth() > m_snapWidth)
            m_width = panelWidth();
        // Collapsing removes the column entirely; the handle floats above the
        // splitter and fades in at the window edge.
        applyPanelWidth(0);
        m_panelWidget->setVisible(false);
        m_panelHost->setFixedWidth(0);
        if (m_toggleOpacity)
            m_toggleOpacity->setOpacity(kIdleToggleOpacity);
        m_toggleHovered = false;
    }
    else
    {
        m_panelHost->setMaximumWidth(QWIDGETSIZE_MAX);
        m_panelHost->setMinimumWidth(0);
        m_panelWidget->setVisible(true);
        if (m_toggleOpacity)
            m_toggleOpacity->setOpacity(1.0);

        if (restoreWidth)
        {
            QList<int> sizes = m_splitter->sizes();
            const int middle = middleIndex();
            int fixed = 0;
            if (middle < sizes.size())
            {
                for (int i = 0; i < sizes.size(); ++i)
                {
                    if (i != m_panelIndex && i != middle)
                        fixed += sizes.at(i);
                }
            }
            const int total = std::accumulate(sizes.cbegin(), sizes.cend(), 0);
            // Keep the video area usable on narrow windows even when a wider
            // panel was remembered from a bigger layout.
            const int width = std::clamp(m_width, kMinimumPanelWidth,
                                         std::max(kMinimumPanelWidth + 60,
                                                  total - fixed - kMinimumVideoWidth));
            applyPanelWidth(width);
        }
    }

    m_applyingLayout = false;
    m_stateTimer.restart();
    refreshToggleAppearance();
    // Collapsed: the button is purely decorative and the edge grab strip owns
    // every mouse event, otherwise a faded-in button swallows the press and the
    // panel cannot be dragged out. Expanded: it is the click-to-fold entry.
    if (m_toggleButton)
        m_toggleButton->setAttribute(Qt::WA_TransparentForMouseEvents, collapsed);
    if (m_dragHint && !m_edgeDragging)
        m_dragHint->hide();
    applyEdgeUi();
    if (changed)
        emit collapsedChanged(collapsed);
}

void SidePanelFold::setFullScreen(bool fullScreen)
{
    if (m_fullScreen == fullScreen)
        return;
    m_fullScreen = fullScreen;
    applyEdgeUi();
}

void SidePanelFold::setHosted(bool hosted)
{
    if (m_hosted == hosted)
        return;
    m_hosted = hosted;
    if (m_snapTimer)
        m_snapTimer->stop();
    if (m_dragHint)
        m_dragHint->hide();
    if (!m_hosted)
    {
        // Back in the splitter: the remembered width was captured before the
        // switch, so the state is already consistent - just put the chevron and
        // the edge strip back where the windowed layout wants them.
        m_hasLastWidth = false;
        m_lastDragTick.invalidate();
        m_stateTimer.restart();
        applyEdgeUi();
    }
}

void SidePanelFold::setPlacement(Side side, int panelIndex)
{
    if (m_side == side && m_panelIndex == panelIndex)
        return;

    // The divider this controller listens to belongs to its position, so the
    // filter has to move with it.
    if (m_splitterHandle)
    {
        m_splitterHandle->removeEventFilter(this);
        m_splitterHandle = nullptr;
    }

    m_side = side;
    m_panelIndex = panelIndex;

    if (m_splitter)
    {
        if (QSplitterHandle* handle = m_splitter->handle(handleIndex()))
        {
            m_splitterHandle = handle;
            handle->installEventFilter(this);
        }
    }

    if (m_dragHint)
    {
        m_dragHint->setSide(side == Side::Left ? LibraryDragHint::Side::Left
                                               : LibraryDragHint::Side::Right);
    }

    // The remembered width belongs to the panel, not to the side; keep it so a
    // swap does not jump back to a default size.
    refreshToggleAppearance();
    applyEdgeUi();
}

void SidePanelFold::applyEdgeUi()
{
    if (m_toggleAnimation)
        m_toggleAnimation->stop();

    if (m_fullScreen)
    {
        // Over the picture: no chevron, no grab strip, no drag glow.
        if (m_toggleButton)
            m_toggleButton->hide();
        if (m_edgeHandle && !m_edgeDragging)
            m_edgeHandle->hide();
        if (m_dragHint)
            m_dragHint->hide();
        m_toggleHovered = false;
        return;
    }

    if (m_toggleButton)
    {
        if (m_toggleOpacity)
            m_toggleOpacity->setOpacity(m_collapsed ? kIdleToggleOpacity : 1.0);
        m_toggleButton->show();
    }
    if (m_edgeHandle && !m_edgeDragging)
        m_edgeHandle->setVisible(m_collapsed);
    updateGeometry();
}

void SidePanelFold::refreshToggleAppearance()
{
    if (!m_toggleButton)
        return;
    // The chevron always points the way the click will move the panel.
    const bool pointingLeft = m_side == Side::Left ? !m_collapsed : m_collapsed;
    m_toggleButton->setIcon(
        Theme::chevronIcon(QColor(0xF2, 0xF2, 0xF7), 16, pointingLeft));
    m_toggleButton->setToolTip(m_collapsed ? m_expandText : m_collapseText);
}

void SidePanelFold::setToggleVisible(bool visible)
{
    if (!m_toggleButton || !m_toggleOpacity || !m_toggleAnimation)
        return;
    if (m_fullScreen)
        return;

    m_toggleAnimation->stop();
    m_toggleButton->show();
    if (visible)
    {
        m_toggleButton->raise();
        m_toggleAnimation->setStartValue(m_toggleOpacity->opacity());
        m_toggleAnimation->setEndValue(1.0);
    }
    else
    {
        m_toggleAnimation->setStartValue(m_toggleOpacity->opacity());
        m_toggleAnimation->setEndValue(kIdleToggleOpacity);
    }
    m_toggleAnimation->start();
}

QRect SidePanelFold::edgeHotZone() const
{
    if (!m_splitter)
        return {};
    const QPoint origin = m_splitter->mapToGlobal(QPoint(0, 0));
    if (m_side == Side::Left)
        return QRect(origin, QSize(kEdgeGrabWidth, m_splitter->height()));
    return QRect(QPoint(origin.x() + m_splitter->width() - kEdgeGrabWidth, origin.y()),
                 QSize(kEdgeGrabWidth, m_splitter->height()));
}

void SidePanelFold::updateGeometry()
{
    // Everything this controller positions is off screen while the window is
    // full screen; the geometry is rebuilt once full screen ends.
    if (m_fullScreen || m_hosted)
        return;
    if (!m_toggleButton || !m_splitter || !m_overlayHost)
        return;

    const int width = m_toggleButton->width() > 0 ? m_toggleButton->width() : 18;
    const int height = m_toggleButton->height() > 0 ? m_toggleButton->height() : 40;
    const QPoint origin = m_splitter->mapTo(m_overlayHost, QPoint(0, 0));
    const int y = origin.y() + (m_splitter->height() - height) / 2;

    int x = origin.x();
    // While the panel is open the chevron sits centred on the splitter instead
    // of floating over the cards; folded, it parks on the window edge.
    const int handleHalf = 6;
    if (m_side == Side::Left)
    {
        if (!m_collapsed)
            x += panelWidth() + handleHalf - width / 2;
    }
    else
    {
        x += m_splitter->width() - width;
        if (!m_collapsed)
            x -= panelWidth() + handleHalf - width / 2;
    }

    // Never let the chevron slide outside the window, whatever the layout says.
    x = std::clamp(x, 0, std::max(0, m_overlayHost->width() - width));
    m_toggleButton->setGeometry(x, y, width, height);
    m_toggleButton->raise();

    // The chevron tracks the divider while the panel is open, so during a drag
    // it keeps jumping between its open and closed positions and reads as a
    // flicker. Hide it completely until the drag is over.
    if (isDragging())
        m_toggleButton->hide();
    else
        m_toggleButton->show();

    if (m_edgeHandle)
    {
        const int hostWidth = m_overlayHost->width();
        const int handleX = m_side == Side::Left ? 0 : std::max(0, hostWidth - kEdgeGrabWidth);
        m_edgeHandle->setGeometry(handleX, 0, kEdgeGrabWidth, m_overlayHost->height());
        if (m_collapsed && !m_edgeDragging)
            m_edgeHandle->raise();
    }
}

void SidePanelFold::updateDragHint()
{
    if (!m_dragHint || !m_splitter || !m_overlayHost)
        return;

    const QPoint origin = m_splitter->mapTo(m_overlayHost, QPoint(0, 0));
    const int hintHeight = m_splitter->height();
    // Fixed width strip hugging the window edge - exactly as wide as the fold
    // threshold, so the glow itself shows where "release folds it" begins.
    const int stripWidth = kSnapWidth;
    const int hintX = m_side == Side::Left
                          ? origin.x()
                          : origin.x() + m_splitter->width() - stripWidth;

    const auto placeEdgeGlow = [&](LibraryDragHint::Mode mode, qreal intensity) {
        m_dragHint->stopFade();
        m_dragHint->setMode(mode);
        m_dragHint->setIntensity(intensity);
        m_dragHint->setGeometry(hintX, origin.y(), stripWidth, hintHeight);
        m_dragHint->show();
        m_dragHint->raise();
    };

    const int width = panelWidth();
    const int directionOf = [this](int current) {
        if (!m_hasLastWidth)
            return m_dragDirection;
        if (current < m_lastHintWidth)
            return static_cast<int>(LibraryDragHint::Mode::Collapse);
        if (current > m_lastHintWidth)
            return static_cast<int>(LibraryDragHint::Mode::Expand);
        return m_dragDirection;
    }(width);
    m_dragDirection = directionOf;
    m_lastHintWidth = width;
    m_hasLastWidth = true;

    // Pulling the panel out of the folded edge: the panel is growing, so the
    // chevron points away from the window edge.
    if (m_edgeDragging && m_edgeDragMoved)
    {
        m_dragDirection = static_cast<int>(LibraryDragHint::Mode::Expand);
        placeEdgeGlow(LibraryDragHint::Mode::Expand, kGlowIntensity);
        return;
    }

    // Dragging an open panel: the strip is visible for the whole drag (even
    // while the pointer stands still) and its chevron follows the direction
    // the panel is actually moving.
    if (!m_collapsed && isDragging() && m_panelWidget && m_panelWidget->isVisible())
    {
        const auto mode = static_cast<LibraryDragHint::Mode>(m_dragDirection);
        const qreal intensity = glowIntensityForWidth(width, m_snapWidth);
        if (intensity > 0.02)
        {
            placeEdgeGlow(mode, intensity);
            return;
        }
    }

    m_hasLastWidth = false;
    m_dragHint->hide();
}

void SidePanelFold::hideDragHint()
{
    // Always fade instead of snapping away, so releasing the mouse reads as a
    // soft "the drag is over" rather than a flash.
    if (m_dragHint)
        m_dragHint->fadeOut();
}

bool SidePanelFold::isDragging() const
{
    // A held button only counts as a drag while the pointer is actually on the
    // divider this controller owns. Qt's global button state can be stale (a
    // release delivered to another window never arrives here), and treating that
    // as a drag used to hide the chevron for a second after an ordinary click -
    // the next click then landed on nothing.
    if ((QGuiApplication::mouseButtons() & Qt::LeftButton) && m_splitterHandle
        && m_splitterHandle->underMouse())
        return true;
    // Splitter signals keep arriving while the handle is being dragged, which
    // is a more reliable indicator than the global mouse state.
    return m_lastDragTick.isValid() && m_lastDragTick.elapsed() < 400;
}

void SidePanelFold::finishEdgeDrag(bool clicked)
{
    m_edgeDragging = false;
    if (m_edgeHandle)
        m_edgeHandle->releaseMouse();

    if (clicked)
    {
        toggle();
    }
    else if (!m_collapsed && panelWidth() <= m_snapWidth)
    {
        setCollapsed(true);
    }

    m_edgeDragMoved = false;
    hideDragHint();
    if (m_edgeHandle)
        m_edgeHandle->setVisible(m_collapsed);
}

bool SidePanelFold::eventFilter(QObject* watched, QEvent* event)
{
    // The chevron folds / unfolds on the press. Waiting for the release was what
    // made a plain click feel random: the chevron sits across the divider, and as
    // soon as the panel starts moving (or the divider takes the pointer over) the
    // release can end up somewhere else, so the button never reported a click.
    // Reacting on the press cannot lose the click, and it feels immediate.
    if (watched == m_toggleButton && event->type() == QEvent::MouseButtonPress)
    {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton)
        {
            // Swallowed on purpose: QToolButton would otherwise report a click of
            // its own on the release and fold the panel twice.
            toggle();
            return true;
        }
    }

    // Releasing the splitter: decide immediately from the position where the
    // user let go, and start fading the glow out right away (no lingering).
    if (watched == m_splitterHandle && event->type() == QEvent::MouseButtonRelease)
    {
        if (m_snapTimer)
            m_snapTimer->stop();
        m_lastDragTick.invalidate();
        if (m_dragHint)
            m_dragHint->fadeOut();
        // The chevron was hidden for the duration of the drag; bring it back
        // right away instead of waiting for the next geometry update.
        if (m_toggleButton && !m_fullScreen)
            m_toggleButton->show();
        if (!m_collapsed && m_panelWidget && m_panelWidget->isVisible()
            && panelWidth() > 0 && panelWidth() <= m_snapWidth)
        {
            setCollapsed(true);
        }
        return false;
    }
    if (watched != m_edgeHandle || !m_edgeHandle || !m_splitter || !m_overlayHost)
        return QObject::eventFilter(watched, event);

    if (event->type() == QEvent::MouseButtonPress)
    {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton)
        {
            m_edgeDragging = true;
            m_edgeDragMoved = false;
            m_edgePressPos = mouse->globalPosition().toPoint();
            m_toggleHovered = false;
            setToggleVisible(false);
            m_edgeHandle->grabMouse();
            return true;
        }
    }
    else if (event->type() == QEvent::MouseMove && m_edgeDragging)
    {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const int localX = m_overlayHost->mapFromGlobal(mouse->globalPosition().toPoint()).x();
        const QList<int> sizes = m_splitter->sizes();
        const int total = std::accumulate(sizes.cbegin(), sizes.cend(), 0);
        const int raw = m_side == Side::Left ? localX : m_overlayHost->width() - localX;
        const int width = std::clamp(raw, 0, std::max(1, total - kMinimumVideoWidth));

        // A click is told apart from a drag by how far the pointer moved since
        // the press. Comparing the *position on the strip* instead - the earlier
        // logic - meant a click on the outer half of the chevron (further than
        // the old fixed threshold from the window edge) was already treated as a
        // drag: the panel opened a sliver, the release folded it again and the
        // click looked lost.
        if (!m_edgeDragMoved
            && std::abs(mouse->globalPosition().toPoint().x() - m_edgePressPos.x())
                   < kEdgeClickSlop)
            return true;

        m_edgeDragMoved = true;
            if (m_collapsed)
            {
                // Pulling out of the edge: the panel grows with the pointer
                // instead of jumping to its remembered width first.
                setCollapsed(false, false);
            }
        if (!m_collapsed)
        {
            applyPanelWidth(width);
            m_width = std::max(kMinimumPanelWidth, panelWidth());
            updateGeometry();
            updateDragHint();
        }
        return true;
    }
    else if (event->type() == QEvent::MouseButtonRelease && m_edgeDragging)
    {
        finishEdgeDrag(!m_edgeDragMoved);
        return true;
    }
    else if (event->type() == QEvent::UngrabMouse && m_edgeDragging)
    {
        // The grab can be revoked without a release (window deactivation, a
        // modal dialog, Alt-Tab). Treat it as a release, otherwise the edge
        // drag stays stuck and the hover chevron never comes back.
        finishEdgeDrag(false);
        return true;
    }

    return QObject::eventFilter(watched, event);
}
