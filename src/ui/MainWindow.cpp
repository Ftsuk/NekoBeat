#include "core/Loc.h"
#include "ui/MainWindow.h"

#include "core/AppLogger.h"
#include "core/DurationFormat.h"
#include "core/LoopStore.h"
#include "core/MediaCleanup.h"
#include "core/PathUtils.h"
#include "core/ScriptLoader.h"
#include "device/DeviceSession.h"
#include "device/DeviceLink.h"
#include "device/SerialPortScanner.h"
#include "device/TCodeEncoder.h"
#include "media/MpvEngine.h"
#include "media/MpvVideoWidget.h"
#include "ui/AxisLimitPanel.h"
#include "ui/CalibrationDialog.h"
#include "ui/HeatmapWidget.h"
#include "ui/LoopClipDialog.h"
#include "ui/LoopListPanel.h"
#include "ui/LoopManagerDialog.h"
#include "ui/MediaLibrary.h"
#include "ui/MediaLibraryDialog.h"
#include "ui/MediaCleanupPrompt.h"
#include "ui/NativeFullScreen.h"
#include "ui/RelinkDialog.h"
#include "ui/SerialPortDialog.h"
#include "ui/SeekSafetyDialog.h"
#include "ui/SidePanelFold.h"
#include "ui/Theme.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QSerialPortInfo>
#include <QSettings>
#include <QProcess>
#include <QPushButton>
#include <QStringList>
#include <QSlider>
#include <QPropertyAnimation>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTime>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
int scriptPositionAt(const Timeline& timeline, qint64 positionMs)
{
    const QList<Action>& actions = timeline.actions;
    if (actions.isEmpty())
        return -1;

    const qint64 timeMs = positionMs + timeline.config.offsetMs;
    if (timeMs <= actions.first().atMs)
        return actions.first().pos;
    if (timeMs >= actions.last().atMs)
        return actions.last().pos;

    int low = 0;
    int high = static_cast<int>(actions.size()) - 1;
    while (low + 1 < high)
    {
        const int mid = (low + high) / 2;
        if (actions.at(mid).atMs <= timeMs)
            low = mid;
        else
            high = mid;
    }

    const Action& from = actions.at(low);
    const Action& to = actions.at(high);
    const qint64 span = to.atMs - from.atMs;
    const double ratio = span > 0 ? static_cast<double>(timeMs - from.atMs) / span : 0.0;
    return qRound(from.pos + (to.pos - from.pos) * ratio);
}

// Full screen playback shows nothing but the picture: the control bar is hidden
// by default and only comes back once the cursor reaches the strip along the
// bottom edge. That strip is the bar itself plus this much approach room, and
// the bar disappears the moment the cursor leaves it again.
constexpr int kFullScreenControlZoneMargin = 48;
// Nothing moves for this long in full screen and the overlays step aside: the
// control bar goes away and the pointer is hidden until the mouse moves again.
constexpr int kFullScreenIdleHideMs = 3000;
// Gap between the floating control bar and the edges of the picture.
constexpr int kFloatingPlayerBarMargin = 10;
// The GUI heartbeat only reports a gap at least this long: below that, the
// event loop is simply turning normally.
constexpr qint64 kUiHeartbeatLogMs = 80;
// Full screen: how close to the screen edge the cursor has to come before the
// media library / loop list slides in. Matches the fold strips used in windowed
// mode.
constexpr int kFullScreenPanelEdgeWidth = 26;
// Gap between the transport buttons (play / emergency stop / full stop / reset).
// Wider than the rest of the bar so the emergency stop cannot be hit by accident
// while aiming for play or stop.
constexpr int kTransportButtonSpacing = 18;
// Tolerance around a pulled out panel before it is put away again, so a pixel of
// jitter on the divider does not close it under the pointer.
constexpr int kFullScreenPanelKeepMargin = 6;
// The edge panels are re-checked from a timer as well: a mouse move can be
// swallowed by a child widget that has no tracking, and the panel has to follow
// the pointer reliably, not almost reliably.
constexpr int kFullScreenPanelPollMs = 60;

// Pointer positions of the --overlay-probe walk: left edge top, left edge
// bottom, middle, right edge top, right edge bottom, middle. The bottom points
// are the interesting ones - the old top level hosts kept the windowed geometry
// and therefore only reacted over the upper part of the screen edge.
struct OverlayProbeStep
{
    double x; // 0 = left edge, 1 = right edge
    double y; // 0 = top, 1 = bottom
    QString text;
};
const OverlayProbeStep kOverlayProbeSteps[] = {
    {0.0, 0.18, LT("左边缘上部")},
    {0.0, 0.92, LT("左边缘下部（旧版会失效）")},
    {0.5, 0.5, LT("画面中间（应收起）")},
    {1.0, 0.18, LT("右边缘上部")},
    {1.0, 0.92, LT("右边缘下部（旧版会失效）")},
    {0.5, 0.5, LT("画面中间（应收起）")},
};
constexpr int kOverlayProbeStepCount =
    int(sizeof(kOverlayProbeSteps) / sizeof(kOverlayProbeSteps[0]));
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    // mpv only takes the hwdec option while it is being initialised, so the
    // stored choice has to be read before the engine comes up.
    m_hwdecMode = std::clamp(
        QSettings().value(QStringLiteral("playback/hwdec"), 0).toInt(), 0, 2);
    m_engine = new MpvEngine(this);
    m_engine->setHardwareDecoding(
        static_cast<MpvEngine::HardwareDecoding>(m_hwdecMode));
    if (!m_engine->initialize())
    {
        QMessageBox::critical(this, LT("启动失败"),
                              LT("libmpv 初始化失败，请确认 libmpv-2.dll 已放在程序目录。"));
    }

    // The whole device chain (serial port, handshake, action timer, TCode
    // encoding) lives in this facade, which owns its own worker thread so a busy
    // interface can no longer delay a command.
    m_link = new DeviceLink(m_engine, this);
    m_previewClock.start();

    // Loop clips live in their own file, independent of the media library.
    m_loopStore = new LoopStore();
    if (!m_loopStore->load())
    {
        AppLogger::log(QStringLiteral("loop"),
                       QStringLiteral("循环片段文件损坏，已备份为 .bad 并重建"));
    }

    buildUi();
    buildMenus();
    wireSignals();
    loadPersistedSettings();
    AppLogger::log(QStringLiteral("ui"), QStringLiteral("主窗口创建完成"));
    setWindowTitle(QStringLiteral("NekoBeat"));
    resize(1360, 860);
    setMinimumSize(1080, 660);
    startRenderWatch();
    startUiHeartbeat();
}

MainWindow::~MainWindow()
{
    // Put the side panels back into the window before the overlay windows are
    // destroyed - they are their parents while full screen, so tearing them down
    // first would take the media library and the loop list with them.
    applyFullScreenPanelHosting(false);

    delete m_loopStore;
    m_loopStore = nullptr;

    // QObject deletes its children in creation order, which is the opposite of
    // what these objects need: m_engine is created first and would be destroyed
    // before the video widget that owns its render context (libmpv then aborts
    // inside mpv_terminate_destroy). Tear the dependent objects down explicitly,
    // in dependency order, instead of leaving it to the child list.
    //
    // The device link goes first: its shutdown is the one synchronous step, it
    // stops the machine (DSTOP), closes the port and joins the worker thread
    // while the media clock it reads is still alive.
    delete m_link;
    m_link = nullptr;
    delete m_video;
    m_video = nullptr;
}

void MainWindow::openMedia(const QString& mediaPath, const QString& selectedScript)
{
    loadMedia(mediaPath, selectedScript);
}

void MainWindow::addLibraryFolder(const QString& folder)
{
    if (m_library && !folder.isEmpty())
        m_library->addRootFolder(folder);
}

qint64 MainWindow::renderedFrameCount() const
{
    return m_video ? m_video->renderedFrameCount() : -1;
}

void MainWindow::startDeviceLoadSimulation()
{
    // The simulation exists to reproduce the cost of the per-command log line,
    // so it has to force that logging on even when the user switched it off.
    AppLogger::setTcodeDetailEnabled(true);
    if (m_tcodeDetailAction)
    {
        const QSignalBlocker blocker(m_tcodeDetailAction);
        m_tcodeDetailAction->setChecked(true);
    }
    auto* timer = new QTimer(this);
    timer->setInterval(2);
    connect(timer, &QTimer::timeout, this, [] {
        // Same shape and rate as the scheduler's per-command log line on a
        // connected device.
        AppLogger::log(QStringLiteral("tcode"),
                       QStringLiteral("t=12345ms #1 | L050L150R050"));
    });
    timer->start();
    AppLogger::log(QStringLiteral("probe"),
                   QStringLiteral("已开启设备负载模拟：每 2 ms 写一条 TCode 日志"));
}

void MainWindow::startUiHeartbeat()
{
    // The scheduler has its own late-tick detector, but that one only runs while
    // a device is connected. This heartbeat always runs, so a hitch is visible in
    // the log even when the machine is not attached - which is exactly the state
    // a "the picture and the machine both stutter" report arrives in.
    auto* timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    timer->setInterval(20);
    auto* last = new QElapsedTimer();
    last->start();
    connect(timer, &QTimer::timeout, this, [this, last] {
        const qint64 gap = last->restart();
        if (gap < kUiHeartbeatLogMs)
            return;
        // At most one line per 200 ms, so a burst cannot flood the log.
        if (m_uiHeartbeatLogClock.isValid() && m_uiHeartbeatLogClock.elapsed() < 200)
            return;
        m_uiHeartbeatLogClock.restart();
        AppLogger::log(QStringLiteral("ui"),
                       QStringLiteral("事件循环阻塞 %1 ms（画面与设备在这段时间都会停）")
                           .arg(gap));
    });
    timer->start();
}

void MainWindow::startRenderWatch()
{
    // Sampled twice a second: a full screen visit is often shorter than the old
    // two-second granularity, and a user reporting "it froze" should not have to
    // sit in full screen for four seconds before evidence is written.
    auto* timer = new QTimer(this);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, [this] {
        const qint64 frames = renderedFrameCount();
        const qint64 requests = renderRequestCount();
        const bool playing = m_engine && m_engine->isPlaying();

        // Only watching while something is supposed to be moving.
        if (!playing)
        {
            m_lastRenderFrames = -1;
            m_lastRenderRequests = -1;
            m_renderStallTicks = 0;
            return;
        }

        if (m_lastRenderFrames >= 0 && frames == m_lastRenderFrames
            && requests == m_lastRenderRequests)
        {
            ++m_renderStallTicks;
            if (m_renderStallTicks == 3)
            {
                AppLogger::log(
                    QStringLiteral("probe"),
                    QStringLiteral("画面停止更新：播放中连续 %1 秒没有新帧"
                                   "（全屏=%2, frames=%3, requests=%4, vo=%5）")
                        .arg(m_renderStallTicks)
                        .arg(m_fullScreen ? 1 : 0)
                        .arg(frames)
                        .arg(requests)
                        .arg(m_engine && m_engine->hasVideoOutput() ? 1 : 0));

                // Level 1: force one repaint. This is free - a mistaken trigger
                // only draws one more frame - and it covers a stalled repaint
                // path that no amount of media handling could repair, because it
                // is exactly the "Qt stopped painting" case.
                if (m_video)
                {
                    AppLogger::log(QStringLiteral("probe"),
                                   QStringLiteral("渲染停滞自救：强制重绘"));
                    m_video->update();
                }

                // Level 2: mpv can lose - or never get - its video output while
                // the media keeps playing. The picture then only refreshes when
                // the window repaints itself, which reads as "frozen" as soon as
                // something changes the window (a full screen switch). Forcing a
                // repaint cannot repair that, so re-open the file instead of
                // leaving the session like that; the same file gets two attempts
                // only, so a video without a video track cannot loop forever.
                if (m_engine && !m_engine->hasVideoOutput() && !m_mediaPath.isEmpty()
                    && m_renderRecoveryAttempts < 2)
                {
                    ++m_renderRecoveryAttempts;
                    AppLogger::log(QStringLiteral("ui"),
                                   QStringLiteral("mpv 没有可用的视频输出，第 %1 次重新打开当前视频")
                                       .arg(m_renderRecoveryAttempts));
                    loadMedia(m_mediaPath, m_mediaScript, true);
                }
            }
        }
        else
        {
            m_renderStallTicks = 0;
        }
        m_lastRenderFrames = frames;
        m_lastRenderRequests = requests;
    });
    timer->start();
}

qint64 MainWindow::renderRequestCount() const
{
    return m_video ? m_video->renderRequestCount() : -1;
}

qint64 MainWindow::renderNanos() const
{
    return m_video ? m_video->renderNanos() : -1;
}

bool MainWindow::videoSurfaceReady() const
{
    return m_video && m_video->hasRenderContext();
}

void MainWindow::runFullScreenProbe(int rounds)
{
    m_fullScreenProbeRemaining = std::max(0, rounds) * 2;
    if (m_fullScreenProbeRemaining == 0)
        return;

    AppLogger::log(QStringLiteral("ui"),
                   QStringLiteral("全屏性能探针: %1 组进出").arg(rounds));

    auto* timer = new QTimer(this);
    timer->setInterval(1200);
    connect(timer, &QTimer::timeout, this, [this, timer] {
        if (m_fullScreenProbeRemaining <= 0)
        {
            timer->stop();
            timer->deleteLater();
            AppLogger::log(QStringLiteral("ui"), QStringLiteral("全屏性能探针结束"));
            QCoreApplication::quit();
            return;
        }
        // Deliberately routed through the action: that is the code path the
        // double click on the picture and the F11 shortcut use, toggled state
        // and all.
        if (m_fullScreenAction)
            m_fullScreenAction->setChecked(!m_fullScreen);
        --m_fullScreenProbeRemaining;
    });
    timer->start();
}

QString MainWindow::foldStateLine(const QString& tag) const
{
    // What a click on the chevron would really hit: the top level window below
    // the pointer is part of the answer. A stray overlay window covering the
    // chevron would swallow every click while looking perfectly normal.
    const auto hitAt = [](QWidget* widget, int x) -> QString {
        if (!widget || widget->width() <= 0 || widget->height() <= 0)
            return LT("无");
        const QPoint point = widget->mapToGlobal(QPoint(x, widget->height() / 2));
        QWidget* hit = QApplication::widgetAt(point);
        QString text = hit
                           ? QString::fromLatin1(hit->metaObject()->className())
                           : QStringLiteral("null");
        if (hit && !hit->objectName().isEmpty())
            text += QLatin1Char('/') + hit->objectName();
        return text;
    };
    const auto hitText = [&hitAt](QWidget* widget) -> QString {
        if (!widget || widget->width() <= 0 || widget->height() <= 0)
            return LT("无");
        const QPoint center = widget->mapToGlobal(QPoint(widget->width() / 2,
                                                        widget->height() / 2));
        // The three probes tell whether the whole chevron really receives the
        // click - a strip of it covered by the splitter handle is exactly what
        // makes a click look lost.
        return LT("%1@%2,%3 左=%4 中=%5 右=%6")
            .arg(hitAt(widget, widget->width() / 2))
            .arg(center.x())
            .arg(center.y())
            .arg(hitAt(widget, 1))
            .arg(hitAt(widget, widget->width() / 2))
            .arg(hitAt(widget, widget->width() - 2));
    };
    const auto sideText = [&hitText](const QString& name, SidePanelFold* fold,
                                     QWidget* side, QWidget* panel, QToolButton* button,
                                     bool rememberedCollapsed, int rememberedWidth) {
        QString text = name + LT("(控制器折叠=");
        text += (fold && fold->isCollapsed()) ? QStringLiteral("1") : QStringLiteral("0");
        text += LT(" 记住折叠=");
        text += rememberedCollapsed ? QStringLiteral("1") : QStringLiteral("0");
        text += LT(" 记住宽=%1 面板可见=%2 侧栏可见=%3 侧栏宽=%4 按钮可见=%5 %6x%7 命中=%8)")
                    .arg(rememberedWidth)
                    .arg(panel && panel->isVisible() ? 1 : 0)
                    .arg(side && side->isVisible() ? 1 : 0)
                    .arg(side ? side->width() : -1)
                    .arg(button && button->isVisible() ? 1 : 0)
                    .arg(button ? button->width() : -1)
                    .arg(button ? button->height() : -1)
                    .arg(hitText(button));
        return text;
    };
    const auto hostText = [](const QString& name, QWidget* host) {
        if (!host)
            return name + QStringLiteral("(null)");
        const QRect rect = host->geometry();
        return LT("%1(可见=%2 %3,%4 %5x%6)")
            .arg(name)
            .arg(host->isVisible() ? 1 : 0)
            .arg(rect.left())
            .arg(rect.top())
            .arg(rect.width())
            .arg(rect.height());
    };

    QString text = LT("折叠探针[") + tag + LT("] 全屏=");
    text += m_fullScreen ? QStringLiteral("1") : QStringLiteral("0");
    text += LT(" 窗口=");
    text += QStringLiteral("%1,%2 %3x%4").arg(geometry().left()).arg(geometry().top())
                .arg(width()).arg(height());
    text += QStringLiteral(" | ");
    text += sideText(LT("媒体库"), m_libraryFold, m_librarySide, m_library,
                     m_libraryToggleButton, m_libraryCollapsed, m_libraryWidth);
    text += QStringLiteral(" | ");
    text += sideText(LT("循环表"), m_loopFold, m_loopSide, m_loopPanel,
                     m_loopToggleButton, m_loopCollapsed, m_loopWidth);
    text += QStringLiteral(" | ");
    text += hostText(LT("浮层左"), m_swappedSides ? m_loopOverlayHost
                                                             : m_libraryOverlayHost);
    text += QLatin1Char(' ');
    text += hostText(LT("浮层右"), m_swappedSides ? m_libraryOverlayHost
                                                             : m_loopOverlayHost);
    return text;
}

void MainWindow::runFoldProbe(int steps)
{
    const int totalSteps = std::max(14, steps);
    AppLogger::log(QStringLiteral("probe"), QStringLiteral("折叠探针开始"));

    auto* timer = new QTimer(this);
    timer->setInterval(650);
    connect(timer, &QTimer::timeout, this, [this, timer, step = 0, totalSteps]() mutable {
        ++step;
        const auto log = [](const QString& text) {
            AppLogger::log(QStringLiteral("probe"), text);
        };
        const auto clickChevron = [this] {
            if (m_libraryToggleButton)
                m_libraryToggleButton->click();
        };
        const auto setFullScreen = [this](bool enabled) {
            if (m_fullScreenAction)
                m_fullScreenAction->setChecked(enabled);
        };

        switch (step)
        {
        case 1:
            log(foldStateLine(LT("初始")));
            break;
        case 2:
            log(LT("折叠探针: 点击折叠/展开按钮 #1"));
            clickChevron();
            break;
        case 3:
            log(foldStateLine(LT("点击 #1 之后")));
            break;
        case 4:
            log(LT("折叠探针: 点击折叠/展开按钮 #2"));
            clickChevron();
            break;
        case 5:
            log(foldStateLine(LT("点击 #2 之后")));
            break;
        case 6:
            log(LT("折叠探针: 进入完全全屏"));
            setFullScreen(true);
            break;
        case 7:
            log(foldStateLine(LT("全屏中")));
            break;
        case 8:
            log(LT("折叠探针: 退出完全全屏"));
            setFullScreen(false);
            break;
        case 9:
            log(foldStateLine(LT("退出全屏后 0.65 秒")));
            break;
        case 10:
            log(LT("折叠探针: 退出全屏后点击 #1"));
            clickChevron();
            break;
        case 11:
            log(foldStateLine(LT("退出后点击 #1 之后")));
            break;
        case 12:
            log(LT("折叠探针: 退出全屏后点击 #2"));
            clickChevron();
            break;
        case 13:
            log(foldStateLine(LT("退出后点击 #2 之后")));
            break;
        default:
            timer->stop();
            timer->deleteLater();
            log(LT("折叠探针结束（步数 %1）").arg(totalSteps));
            QCoreApplication::quit();
            break;
        }
    });
    timer->start();
}

void MainWindow::runOverlayProbe(int rounds)
{
    // The probe drives the real thing: it moves the pointer to the screen edges
    // and back, so the hover logic (not a direct call) decides whether the
    // panels come out. Every step logs the state and the render counters; the
    // screen can be captured while it runs to look for a flash.
    const int repeat = std::max(1, rounds);
    const QPoint savedCursor = QCursor::pos();
    AppLogger::log(QStringLiteral("probe"),
                   QStringLiteral("浮层探针开始（%1 轮，光标已保存 %2,%3）")
                       .arg(repeat)
                       .arg(savedCursor.x())
                       .arg(savedCursor.y()));

    QTimer::singleShot(1500, this, [this] {
        if (m_fullScreenAction)
            m_fullScreenAction->setChecked(true);
        AppLogger::log(QStringLiteral("probe"), QStringLiteral("浮层探针: 已进入全屏"));
    });

    const int totalSteps = kOverlayProbeStepCount * repeat;

    auto* timer = new QTimer(this);
    timer->setInterval(900);
    connect(timer, &QTimer::timeout, this,
            [this, timer, step = 0, repeat, totalSteps, savedCursor]() mutable {
                Q_UNUSED(repeat);
                const int index = step % kOverlayProbeStepCount;
                const bool last = (step + 1) >= totalSteps;
                if (index == 0 && step > 0)
                    AppLogger::log(QStringLiteral("probe"), QStringLiteral("浮层探针: 下一轮"));

                const OverlayProbeStep& target = kOverlayProbeSteps[index];
                const QRect screenRect = screen() ? screen()->geometry() : geometry();
                const int x = target.x <= 0.0 ? screenRect.left() + 4
                                              : (target.x >= 1.0 ? screenRect.right() - 4
                                                                 : screenRect.center().x());
                const int y = screenRect.top()
                              + int((screenRect.height() - 1) * target.y);
                QCursor::setPos(x, y);

                // Give the events a moment, then report what the app decided.
                QTimer::singleShot(260, this, [this, index, x, y, last, totalSteps,
                                               step, savedCursor] {
                    const QString visiblePanel =
                        m_fullScreenPanelVisible == m_libraryOverlayHost
                            ? LT("媒体库")
                            : (m_fullScreenPanelVisible == m_loopOverlayHost
                                   ? LT("循环列表")
                                   : LT("无"));
                    AppLogger::log(
                        QStringLiteral("probe"),
                        QStringLiteral("浮层探针 %1/%2 %3: 光标 %4,%5 → 浮层=%6 frames=%7 requests=%8")
                            .arg(step + 1)
                            .arg(totalSteps)
                            .arg(kOverlayProbeSteps[index].text)
                            .arg(x)
                            .arg(y)
                            .arg(visiblePanel)
                            .arg(renderedFrameCount())
                            .arg(renderRequestCount()));
                    if (last)
                    {
                        if (m_fullScreenAction)
                            m_fullScreenAction->setChecked(false);
                        QCursor::setPos(savedCursor);
                        AppLogger::log(QStringLiteral("probe"),
                                       QStringLiteral("浮层探针结束，光标已还原"));
                        QTimer::singleShot(500, this, [] { QCoreApplication::quit(); });
                    }
                });
                ++step;
                if (last)
                    timer->stop();
            });
    QTimer::singleShot(2300, this, [timer] { timer->start(); });
}

void MainWindow::runClickProbe(int seconds)
{
    auto* timer = new QTimer(this);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this,
            [this] { AppLogger::log(QStringLiteral("probe"), foldStateLine(QStringLiteral("周期"))); });
    timer->start();
    AppLogger::log(QStringLiteral("probe"),
                   QStringLiteral("点击探针开始（%1 秒，只记录折叠状态）").arg(seconds));
    QTimer::singleShot(std::max(3, seconds) * 1000, this, [timer] {
        timer->stop();
        QCoreApplication::quit();
    });
}

void MainWindow::runPlaybackProbe(int seconds)
{
    const int duration = std::max(1, seconds);
    AppLogger::log(QStringLiteral("probe"),
                   QStringLiteral("播放探针开始: %1 秒（只采样）").arg(duration));

    auto* timer = new QTimer(this);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, [this] {
        AppLogger::log(QStringLiteral("probe"),
                       QStringLiteral("pos=%1 playing=%2 frames=%3 requests=%4 "
                                      "fullscreen=%5 vo=%6 gl=%7")
                           .arg(m_engine ? m_engine->positionMs() : -1)
                           .arg(m_engine && m_engine->isPlaying() ? 1 : 0)
                           .arg(renderedFrameCount())
                           .arg(renderRequestCount())
                           .arg(m_fullScreen ? 1 : 0)
                           .arg(m_engine && m_engine->hasVideoOutput() ? 1 : 0)
                           .arg(m_video ? m_video->renderContextGeneration() : -1));
    });
    QTimer::singleShot(duration * 1000, this, [timer] {
        timer->stop();
        AppLogger::log(QStringLiteral("probe"), QStringLiteral("播放探针结束"));
        QCoreApplication::quit();
    });
    timer->start();
}

void MainWindow::buildUi()
{
    menuBar()->setNativeMenuBar(false);

    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(6, 6, 6, 4);
    rootLayout->setSpacing(0);

    m_library = new MediaLibrary(central);
    m_library->setMinimumWidth(0);

    auto* mediaColumn = new QWidget(central);
    m_mediaColumn = mediaColumn;
    auto* mediaLayout = new QVBoxLayout(mediaColumn);
    mediaLayout->setContentsMargins(0, 0, 0, 0);
    mediaLayout->setSpacing(6);

    auto* videoFrame = new QFrame(mediaColumn);
    m_videoFrame = videoFrame;
    videoFrame->setObjectName(QStringLiteral("VideoArea"));
    // The control bar floats inside this frame while full screen, so the frame
    // reports its size to keep the overlay glued to the bottom.
    videoFrame->installEventFilter(this);
    auto* videoLayout = new QVBoxLayout(videoFrame);
    videoLayout->setContentsMargins(1, 1, 1, 1);
    videoLayout->setSpacing(0);

    m_videoStack = new QStackedWidget(videoFrame);
    m_videoStack->setStyleSheet(QStringLiteral("background: #000000;"));
    m_video = new MpvVideoWidget(m_videoStack);
    m_video->setMpvHandle(m_engine->handle());

    m_blackScreen = new QWidget(m_videoStack);
    m_blackScreen->setStyleSheet(QStringLiteral("background: #000000;"));
    auto* blackLayout = new QVBoxLayout(m_blackScreen);
    auto* blackLabel = new QLabel(
        LT("拖入视频文件，或使用“文件 › 打开视频”"), m_blackScreen);
    blackLabel->setAlignment(Qt::AlignCenter);
    blackLabel->setStyleSheet(QStringLiteral("color: #6E6E73; font-size: 14px;"));
    blackLayout->addWidget(blackLabel);

    m_videoStack->addWidget(m_video);
    m_videoStack->addWidget(m_blackScreen);
    // Keep the GL widget visible from the start so the first video gets a
    // render context immediately. With no media loaded mpv renders black.
    m_videoStack->setCurrentWidget(m_video);
    videoLayout->addWidget(m_videoStack);
    mediaLayout->addWidget(videoFrame, 1);

    auto* playerBar = new QFrame(mediaColumn);
    m_playerBar = playerBar;
    playerBar->setObjectName(QStringLiteral("PlayerBar"));
    // Two rows: a pure button row on top, the heat map / progress strip below.
    auto* playerColumns = new QVBoxLayout(playerBar);
    playerColumns->setContentsMargins(10, 6, 10, 6);
    playerColumns->setSpacing(6);

    auto* playerLayout = new QHBoxLayout();
    playerLayout->setContentsMargins(0, 0, 0, 0);
    playerLayout->setSpacing(8);

    m_playButton = new QToolButton(playerBar);
    m_playButton->setObjectName(QStringLiteral("MediaButtonAccent"));
    m_playButton->setIcon(Theme::playIcon(Qt::white, 28));
    m_playButton->setIconSize(QSize(22, 22));
    m_playButton->setToolTip(LT("播放 / 暂停（Space）"));
    m_playButton->setCursor(Qt::PointingHandCursor);
    m_playButton->setFixedSize(32, 32);

    m_stopButton = new QToolButton(playerBar);
    m_stopButton->setObjectName(QStringLiteral("MediaButton"));
    m_stopButton->setIcon(Theme::stopIcon(QColor(0xF2, 0xF2, 0xF7), 28));
    m_stopButton->setIconSize(QSize(22, 22));
    m_stopButton->setToolTip(LT("终止播放"));
    m_stopButton->setCursor(Qt::PointingHandCursor);
    m_stopButton->setFixedSize(32, 32);

    // Emergency stop. It stays on the control bar (also while full screen) so
    // it is always reachable the moment the device does something unexpected.
    m_panicButton = new QToolButton(playerBar);
    m_panicButton->setObjectName(QStringLiteral("MediaButton"));
    m_panicButton->setText(LT("急停"));
    m_panicButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_panicButton->setCursor(Qt::PointingHandCursor);
    m_panicButton->setFixedHeight(32);
    m_panicButton->setToolTip(LT("急停（Ctrl+Esc）\n立即发送 DSTOP 并暂停视频"));
    m_panicButton->setStyleSheet(QStringLiteral(
        "QToolButton { color: #FF453A; font-weight: 600;"
        " border: 1px solid rgba(255, 69, 58, 0.55); border-radius: 6px;"
        " padding: 0 10px; }"
        "QToolButton:hover { background: rgba(255, 69, 58, 0.18); }"
        "QToolButton:pressed { background: rgba(255, 69, 58, 0.32); }"));

    m_resetButton = new QToolButton(playerBar);
    m_resetButton->setObjectName(QStringLiteral("MediaButton"));
    m_resetButton->setIcon(Theme::resetIcon(QColor(0xF2, 0xF2, 0xF7), 28));
    m_resetButton->setIconSize(QSize(22, 22));
    m_resetButton->setToolTip(LT("复位（回到中位）"));
    m_resetButton->setCursor(Qt::PointingHandCursor);
    m_resetButton->setFixedSize(32, 32);

    // A-B marking: pause wherever the user is, remember the point, then save
    // the marked range as a loop clip.
    m_loopAButton = new QToolButton(playerBar);
    m_loopAButton->setObjectName(QStringLiteral("MediaButton"));
    m_loopAButton->setText(QStringLiteral("A"));
    m_loopAButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_loopAButton->setCursor(Qt::PointingHandCursor);
    m_loopAButton->setFixedSize(32, 32);
    m_loopAButton->setToolTip(LT("标记循环起点 A（[）\n右键清除标记"));
    m_loopAButton->setContextMenuPolicy(Qt::CustomContextMenu);

    m_loopBButton = new QToolButton(playerBar);
    m_loopBButton->setObjectName(QStringLiteral("MediaButton"));
    m_loopBButton->setText(QStringLiteral("B"));
    m_loopBButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_loopBButton->setCursor(Qt::PointingHandCursor);
    m_loopBButton->setFixedSize(32, 32);
    m_loopBButton->setToolTip(LT("标记循环终点 B（]）\n右键清除标记"));
    m_loopBButton->setContextMenuPolicy(Qt::CustomContextMenu);

    m_loopAddButton = new QToolButton(playerBar);
    m_loopAddButton->setObjectName(QStringLiteral("MediaButton"));
    m_loopAddButton->setText(LT("添加循环"));
    m_loopAddButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_loopAddButton->setCursor(Qt::PointingHandCursor);
    m_loopAddButton->setFixedHeight(32);
    m_loopAddButton->setToolTip(LT("把 A-B 区间保存为循环片段（Ctrl+L）"));

    m_loopClearButton = new QToolButton(playerBar);
    m_loopClearButton->setObjectName(QStringLiteral("MediaButton"));
    m_loopClearButton->setText(LT("清除标记"));
    m_loopClearButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_loopClearButton->setCursor(Qt::PointingHandCursor);
    m_loopClearButton->setFixedHeight(32);
    m_loopClearButton->setToolTip(LT("取消当前的 A/B 标记，不保存片段"));

    m_statusTime = new QLabel(QStringLiteral("00:00 / 00:00"), playerBar);
    m_statusTime->setObjectName(QStringLiteral("TimeLabel"));
    m_statusTime->setAlignment(Qt::AlignCenter);
    m_statusTime->setMinimumWidth(104);
    m_statusTime->setTextFormat(Qt::RichText);

    m_volumeButton = new QToolButton(playerBar);
    m_volumeButton->setObjectName(QStringLiteral("Compact"));
    m_volumeButton->setIcon(Theme::volumeIcon(QColor(0xF2, 0xF2, 0xF7), 16));
    m_volumeButton->setIconSize(QSize(16, 16));
    m_volumeButton->setToolTip(LT("静音 / 取消静音"));
    m_volumeButton->setCursor(Qt::PointingHandCursor);

    m_volumeSlider = new QSlider(Qt::Horizontal, playerBar);
    m_volumeSlider->setObjectName(QStringLiteral("Volume"));
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setFixedWidth(90);
    m_volumeSlider->setToolTip(LT("音量"));

    m_volumeLabel = new QLabel(QStringLiteral("100%"), playerBar);
    m_volumeLabel->setObjectName(QStringLiteral("Muted"));
    m_volumeLabel->setFixedWidth(38);
    m_volumeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_volumeLabel->setToolTip(LT("当前音量"));

    // Three blocks instead of one long row: the loop markers keep the left end,
    // the transport buttons (play/pause, emergency stop, full stop, reset) own the
    // centre of the bar, and the position / volume readouts close the right end.
    const auto makeBlock = [playerBar](const QList<QWidget*>& widgets, int spacing = 8) {
        auto* block = new QWidget(playerBar);
        auto* blockLayout = new QHBoxLayout(block);
        blockLayout->setContentsMargins(0, 0, 0, 0);
        blockLayout->setSpacing(spacing);
        for (QWidget* widget : widgets)
            blockLayout->addWidget(widget);
        return block;
    };

    auto* loopBlock = makeBlock({m_loopAButton, m_loopBButton, m_loopAddButton,
                                 m_loopClearButton});
    // The transport buttons are the ones pressed blindly while the picture is
    // running, so they get a wider gap than the rest of the bar: with 8 px the
    // four icons read as one cluster and "急停" sat right next to "播放".
    auto* transportBlock = makeBlock({m_playButton, m_panicButton, m_stopButton,
                                      m_resetButton},
                                     kTransportButtonSpacing);
    auto* statusBlock = makeBlock({m_statusTime, m_volumeButton, m_volumeSlider,
                                   m_volumeLabel});

    // Equal side widths put the transport cluster on the centre line of the bar
    // itself instead of merely between its neighbours.
    const int sideWidth =
        std::max(loopBlock->sizeHint().width(), statusBlock->sizeHint().width());
    loopBlock->setMinimumWidth(sideWidth);
    statusBlock->setMinimumWidth(sideWidth);

    playerLayout->addWidget(loopBlock);
    playerLayout->addStretch(1);
    playerLayout->addWidget(transportBlock);
    playerLayout->addStretch(1);
    playerLayout->addWidget(statusBlock);

    playerColumns->addLayout(playerLayout);

    // The heat map doubles as the progress bar: it shows the script intensity
    // over time, the current position, and seeks on click or drag. It fills the
    // whole second row so the A-B overlay has the full width to work with.
    m_heatmap = new HeatmapWidget(playerBar);
    m_heatmap->setVisible(m_heatmapVisible);
    playerColumns->addWidget(m_heatmap);

    mediaLayout->addWidget(playerBar);

    // Left column: the media library. The fold handle floats above the splitter
    // instead of occupying its own strip, so a folded panel takes no space.
    m_librarySide = new QWidget(central);
    auto* sideLayout = new QHBoxLayout(m_librarySide);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(0);
    sideLayout->addWidget(m_library, 1);

    // Right column: the loop list, folded and dragged exactly like the library.
    m_loopPanel = new LoopListPanel(central);
    m_loopPanel->setMinimumWidth(0);
    m_loopSide = new QWidget(central);
    auto* loopLayout = new QHBoxLayout(m_loopSide);
    loopLayout->setContentsMargins(0, 0, 0, 0);
    loopLayout->setSpacing(0);
    loopLayout->addWidget(m_loopPanel, 1);

    m_libraryToggleButton = new QToolButton(central);
    m_loopToggleButton = new QToolButton(central);

    m_splitter = new QSplitter(Qt::Horizontal, central);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(12);
    m_splitter->addWidget(m_librarySide);
    m_splitter->addWidget(mediaColumn);
    m_splitter->addWidget(m_loopSide);
    // The two side panels may be dragged all the way to zero - that is what
    // makes folding feel continuous instead of stopping at a minimum width.
    // The video column stays protected.
    m_splitter->setCollapsible(0, true);
    m_splitter->setCollapsible(1, false);
    m_splitter->setCollapsible(2, true);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setStretchFactor(2, 0);

    rootLayout->addWidget(m_splitter);

    setCentralWidget(central);

    // Full screen overlay hosts for the two side panels. They are plain child
    // widgets of the player column, created after the picture and after the
    // control bar, so they sit on top of both: Qt composites the QOpenGLWidget
    // into the window, which means a normal widget can be drawn above the
    // picture. An extra top level window was the earlier approach and it had to
    // fight the window manager for every show/hide - that is what made the
    // panels flicker, and its geometry went stale on the full screen switch,
    // which is what made the edge trigger unreliable.
    m_libraryOverlayHost = new QWidget(m_mediaColumn);
    m_libraryOverlayHost->setObjectName(QStringLiteral("FullScreenPanel"));
    m_libraryOverlayHost->setAutoFillBackground(true);
    auto* libraryOverlayLayout = new QVBoxLayout(m_libraryOverlayHost);
    libraryOverlayLayout->setContentsMargins(0, 0, 0, 0);
    libraryOverlayLayout->setSpacing(0);
    m_libraryOverlayHost->installEventFilter(this);
    m_libraryOverlayHost->hide();

    m_loopOverlayHost = new QWidget(m_mediaColumn);
    m_loopOverlayHost->setObjectName(QStringLiteral("FullScreenPanel"));
    m_loopOverlayHost->setAutoFillBackground(true);
    auto* loopOverlayLayout = new QVBoxLayout(m_loopOverlayHost);
    loopOverlayLayout->setContentsMargins(0, 0, 0, 0);
    loopOverlayLayout->setSpacing(0);
    m_loopOverlayHost->installEventFilter(this);
    m_loopOverlayHost->hide();

    // Both side panels share one fold controller so their feel stays identical.
    m_libraryFold = new SidePanelFold(SidePanelFold::Side::Left, m_splitter, 0,
                                      m_librarySide, central, this);
    m_libraryFold->setPanelWidget(m_library);
    m_libraryFold->attachToggleButton(m_libraryToggleButton, LT("折叠媒体库"),
                                      LT("展开媒体库"));

    m_loopFold = new SidePanelFold(SidePanelFold::Side::Right, m_splitter, 2, m_loopSide,
                                   central, this);
    m_loopFold->setPanelWidget(m_loopPanel);
    m_loopFold->attachToggleButton(m_loopToggleButton, LT("折叠循环列表"),
                                   LT("展开循环列表"));

    connect(m_libraryFold, &SidePanelFold::collapsedChanged, this, [this](bool collapsed) {
        m_libraryCollapsed = collapsed;
        if (m_libraryAction)
        {
            m_libraryAction->blockSignals(true);
            m_libraryAction->setChecked(!collapsed);
            m_libraryAction->blockSignals(false);
        }
    });
    // A width of 0 means the panel is folded away or the whole side is hidden
    // (full screen) - that must not overwrite the width the user chose, which the
    // full screen overlay needs.
    connect(m_libraryFold, &SidePanelFold::widthChanged, this,
            [this](int width) {
                if (width > 0)
                    m_libraryWidth = std::max(200, width);
            });
    connect(m_loopFold, &SidePanelFold::collapsedChanged, this, [this](bool collapsed) {
        m_loopCollapsed = collapsed;
        if (m_loopListAction)
        {
            m_loopListAction->blockSignals(true);
            m_loopListAction->setChecked(!collapsed);
            m_loopListAction->blockSignals(false);
        }
    });
    connect(m_loopFold, &SidePanelFold::widthChanged, this,
            [this](int width) {
                if (width > 0)
                    m_loopWidth = std::max(200, width);
            });

    m_statusDevice = new QLabel(LT("设备未连接"), this);
    m_statusScript = new QLabel(LT("脚本未加载"), this);
    m_statusDevice->setObjectName(QStringLiteral("StatusChip"));
    m_statusScript->setObjectName(QStringLiteral("StatusChip"));
    // Hard caps, so no single chip can grow into the row's whole budget: the link
    // detail (firmware, TCode, Bluetooth device name) lives in the tooltip and the
    // axis list is elided by updateStatus().
    m_statusDevice->setMaximumWidth(130);
    m_statusScript->setMaximumWidth(150);
    statusBar()->setSizeGripEnabled(false);
    // The status row is gone: device state, messages and script state all live
    // in the menu bar corner next to the connect button.
    statusBar()->hide();

    m_messageTimer = new QTimer(this);
    m_messageTimer->setSingleShot(true);
    connect(m_messageTimer, &QTimer::timeout, this, [this]() {
        if (m_messageLabel)
        {
            m_messageLabel->clear();
            m_messageLabel->setToolTip(QString());
            m_messageLabel->setVisible(false);
            updateStatusChips();
        }
    });

    // Scrubbing cadence: one device seek per 40 ms of dragging at most, so a
    // pointer that reports 100+ moves per second cannot flood the serial link
    // (each seek writes one command per axis) or the decoder. The final
    // position is flushed on mouse release.
    m_seekCoalesceTimer = new QTimer(this);
    m_seekCoalesceTimer->setSingleShot(true);
    m_seekCoalesceTimer->setInterval(40);
    connect(m_seekCoalesceTimer, &QTimer::timeout, this, &MainWindow::applyPendingSeek);

    m_axisPanel = new AxisLimitPanel(this);
    setupFullScreenControls();
}

void MainWindow::toggleLibraryCollapsed()
{
    if (m_libraryFold)
        m_libraryFold->toggle();
}

void MainWindow::setLibraryCollapsed(bool collapsed)
{
    if (m_libraryFold)
        m_libraryFold->setCollapsed(collapsed);
}

void MainWindow::toggleLoopListCollapsed()
{
    if (m_loopFold)
        m_loopFold->toggle();
}

void MainWindow::setLoopListCollapsed(bool collapsed)
{
    if (m_loopFold)
        m_loopFold->setCollapsed(collapsed);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    if (m_libraryFold)
        m_libraryFold->updateGeometry();
    if (m_loopFold)
        m_loopFold->updateGeometry();
    updateFullScreenPanelGeometry();
    updateStatusChips();
}

void MainWindow::updateStatusChips()
{
    if (!m_uptimeLabel || !m_statusLink || !m_statusScript || !m_statusDevice
        || !m_deviceChip)
    {
        return;
    }
    if (menuBar()->width() <= 0)
        return;  // Not laid out yet; the first resize will do it.

    // The menu bar gives its corner widget a fixed slice of the window and simply
    // crops whatever overflows - the chips are neither wrapped nor elided by the
    // layout - so a narrow window cuts every label in half ("脚本未加", "断...6").
    // Whatever does not fit is dropped from the least important end instead.
    int menusRight = 0;
    for (QAction* action : menuBar()->actions())
    {
        const QRect geometry = menuBar()->actionGeometry(action);
        if (geometry.isValid())
            menusRight = std::max(menusRight, geometry.right());
    }
    const int spacing = 14;  // cornerLayout spacing, set where the chips are built
    const int rightMargin = 10;
    const int barWidth = menuBar()->width();
    // QMenuBar only ever hands this corner a slice of the bar, and on a narrow
    // window that slice - not the empty space after the menus - is the real
    // budget. Measuring against the free space alone made the row look roomier
    // than it was, so the chips were still squeezed.
    const int available = std::min(barWidth - menusRight - rightMargin, barWidth / 2);

    const QList<QWidget*> chips = {m_uptimeLabel, m_statusDevice, m_statusLink,
                                   m_messageLabel, m_statusScript, m_deviceChip};
    // The link chip and the connect button always stay; the rest give way in this
    // order. The hint chip is not in the list - showStatusMessage() hides it while
    // there is nothing to say.
    const QList<QWidget*> dropOrder = {m_uptimeLabel, m_statusLink, m_statusScript};

    m_uptimeLabel->setVisible(true);
    // The sync chip only means something once a statistics window has arrived;
    // before that it says "同步 --", which is a whole chip spent on nothing.
    m_statusLink->setVisible(m_linkStatsSeen);
    m_statusScript->setVisible(true);

    // A squeezed QLabel crops its text rather than eliding it, so hold every chip
    // at the width its own text needs; the loop below removes chips until the row
    // fits, instead of shrinking all of them and cutting every word in half.
    for (QWidget* chip : chips)
    {
        chip->setMinimumWidth(chip->isVisible() ? chip->sizeHint().width() : 0);
    }

    const auto needed = [&]() {
        int sum = rightMargin;
        for (QWidget* chip : chips)
        {
            if (chip->isVisible())
                sum += chip->sizeHint().width() + spacing;
        }
        return sum;
    };

    for (QWidget* chip : dropOrder)
    {
        if (needed() <= available)
            break;
        chip->setVisible(false);
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    // While full screen the control bar floats inside the picture, so it has to
    // follow every resize of the picture (window resize, panel drag, ...). The
    // bar itself is a sibling of the video frame now, hence both widgets.
    if (m_controlsFloating && event->type() == QEvent::Resize
        && (watched == m_videoFrame || watched == m_mediaColumn))
        positionFloatingPlayerBar();
    // The edge panels live in the player column, so they follow every resize of
    // it - the full screen switch included, which is where the earlier version
    // kept a stale windowed geometry and only reacted over part of the screen.
    if (event->type() == QEvent::Resize && watched == m_mediaColumn)
        updateFullScreenPanelGeometry();

    // Both side panels run their own edge dragging through SidePanelFold; what
    // is handled here is the full screen control bar auto-hide.
    // Full screen edge panels: the pointer leaving the panel puts it away again.
    // The pointer position is re-checked as well (the panel can be left over a
    // child widget while a button is held), so this is only the fast path.
    if (m_fullScreen && m_fullScreenPanelVisible
        && (watched == m_libraryOverlayHost || watched == m_loopOverlayHost)
        && event->type() == QEvent::Leave)
    {
        updateFullScreenPanelsForCursor();
    }

    if (m_fullScreen)
    {
        switch (event->type())
        {
        case QEvent::MouseMove:
        case QEvent::Enter:
            // Enter is handled as well as motion: the cursor can land straight
            // on a button, which reports no motion of its own. Either way the
            // pointer comes back and the idle countdown starts over.
            setFullScreenCursorHidden(false);
            if (m_cursorIdleTimer)
                m_cursorIdleTimer->start();
            syncFullScreenControlsWithCursor();
            updateFullScreenPanelsForCursor();
            break;
        case QEvent::Leave:
        case QEvent::MouseButtonRelease:
            // The cursor may have left the strip or the window, and an
            // interaction (seek, volume, A/B marks) may have just ended:
            // re-decide right away from the real cursor position.
            setFullScreenCursorHidden(false);
            syncFullScreenControlsWithCursor();
            if (m_cursorIdleTimer)
                m_cursorIdleTimer->start();
            break;
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(eventType);
    // Win+D and the task bar button minimize the window without going through
    // the full screen bookkeeping. The stripped style has to be put back before
    // that happens, otherwise the restored window comes back without a frame.
    if (m_nativeFullscreen && NativeFullScreen::isMinimizeNotification(message))
    {
        // Deferred: changing the window style from inside the message handler
        // would re-enter the window procedure while Windows is still resizing.
        QTimer::singleShot(0, this, [this] {
            if (m_fullScreenAction && m_fullScreenAction->isChecked())
                m_fullScreenAction->setChecked(false);
        });
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::syncLibraryMenu()
{
    if (!m_library)
        return;

    const auto checkByData = [](QMenu* menu, int value) {
        if (!menu)
            return;
        for (QAction* action : menu->actions())
            action->setChecked(action->data().toInt() == value);
    };
    checkByData(m_librarySortMenu,
                static_cast<int>(m_library->sortMode()) * 2
                    + (m_library->sortDescending() ? 1 : 0));
    checkByData(m_viewThumbMenu, m_library->thumbSize());
    checkByData(m_viewFontMenu, m_library->fontPercent());
    if (m_libraryOnlyScriptAction)
        m_libraryOnlyScriptAction->setChecked(m_library->onlyWithScript());
    buildLibraryFavoritesMenu();
}

void MainWindow::buildLibraryFavoritesMenu()
{
    if (!m_libraryFavoritesMenu || !m_library)
        return;

    m_libraryFavoritesMenu->clear();

    QAction* allAction = m_libraryFavoritesMenu->addAction(LT("全部视频"));
    allAction->setCheckable(true);
    allAction->setChecked(m_library->favoriteView()
                          == MediaLibrary::FavoriteView::All);
    connect(allAction, &QAction::triggered, this, [this]() {
        if (m_library)
            m_library->showAllVideos();
    });

    QAction* noneAction = m_libraryFavoritesMenu->addAction(LT("未收藏"));
    noneAction->setCheckable(true);
    noneAction->setChecked(m_library->favoriteView()
                           == MediaLibrary::FavoriteView::NotFavorites);
    connect(noneAction, &QAction::triggered, this, [this]() {
        if (m_library)
            m_library->showUnfavorited();
    });

    m_libraryFavoritesMenu->addSeparator();
    const QList<FavoriteFolder> folders = m_library->favoriteFolders();
    if (folders.isEmpty())
    {
        QAction* emptyAction =
            m_libraryFavoritesMenu->addAction(LT("（还没有收藏夹）"));
        emptyAction->setEnabled(false);
    }
    for (const FavoriteFolder& folder : folders)
    {
        QAction* action = m_libraryFavoritesMenu->addAction(
            QStringLiteral("%1 (%2)").arg(folder.name).arg(folder.members.size()));
        action->setCheckable(true);
        action->setChecked(m_library->favoriteView() == MediaLibrary::FavoriteView::Folder
                           && m_library->favoriteFolderId() == folder.id);
        const QString folderId = folder.id;
        connect(action, &QAction::triggered, this, [this, folderId]() {
            if (m_library)
                m_library->showFavoriteFolder(folderId);
        });
    }

    m_libraryFavoritesMenu->addSeparator();
    QAction* manageAction =
        m_libraryFavoritesMenu->addAction(LT("管理收藏夹…"));
    connect(manageAction, &QAction::triggered, this, &MainWindow::openFavoritesDialog);
}

void MainWindow::buildLoopMenu()
{
    m_loopMenu = menuBar()->addMenu(LT("循环列表"));

    auto* manageAction = m_loopMenu->addAction(LT("循环列表管理…"));
    auto* clearMarkersAction = m_loopMenu->addAction(LT("清除当前 A/B 标记"));
    m_loopMenu->addSeparator();

    m_loopAutoNextAction = m_loopMenu->addAction(LT("连续播放片段"));
    m_loopAutoNextAction->setCheckable(true);
    m_loopAutoNextAction->setChecked(m_loopPanel && m_loopPanel->autoNext());
    m_loopAutoNextAction->setToolTip(
        LT("一段播完自动接着播当前列表里的下一段"));
    m_loopMenu->addSeparator();

    m_loopFolderMenu = m_loopMenu->addMenu(LT("循环收藏夹"));
    m_loopTagMenu = m_loopMenu->addMenu(LT("标签"));
    m_loopMenu->addSeparator();

    m_loopSortMenu = m_loopMenu->addMenu(LT("排序"));
    {
        const struct
        {
            QString label;
            LoopListPanel::SortMode mode;
            bool descending;
        } entries[] = {
            {LT("添加时间 新→旧"), LoopListPanel::SortMode::RecentlyAdded, true},
            {LT("添加时间 旧→新"), LoopListPanel::SortMode::RecentlyAdded, false},
            {LT("视频名称 A→Z"), LoopListPanel::SortMode::MediaName, false},
            {LT("视频名称 Z→A"), LoopListPanel::SortMode::MediaName, true},
            {LT("片段时长 短→长"), LoopListPanel::SortMode::Duration, false},
            {LT("片段时长 长→短"), LoopListPanel::SortMode::Duration, true}
        };
        auto* group = new QActionGroup(this);
        for (const auto& entry : entries)
        {
            QAction* action = m_loopSortMenu->addAction(entry.label);
            action->setCheckable(true);
            action->setData(static_cast<int>(entry.mode) * 2 + (entry.descending ? 1 : 0));
            group->addAction(action);
            const LoopListPanel::SortMode mode = entry.mode;
            const bool descending = entry.descending;
            connect(action, &QAction::triggered, this, [this, mode, descending]() {
                if (m_loopPanel)
                    m_loopPanel->setSortMode(mode, descending);
            });
        }
    }

    m_loopMenu->addSeparator();
    auto* pruneAction = m_loopMenu->addAction(LT("清理失效片段"));

    connect(m_loopMenu, &QMenu::aboutToShow, this, &MainWindow::syncLoopMenu);
    connect(manageAction, &QAction::triggered, this, &MainWindow::openLoopManagerDialog);
    connect(clearMarkersAction, &QAction::triggered, this, &MainWindow::clearLoopMarkers);
    connect(m_loopAutoNextAction, &QAction::triggered, this, [this](bool enabled) {
        if (m_loopPanel)
            m_loopPanel->setAutoNext(enabled);
    });
    connect(pruneAction, &QAction::triggered, this, [this]() {
        if (!m_loopStore || !m_library)
            return;
        // Both panels share one clean-up, so a stale video can never be half
        // removed (favourite gone, clip left behind, tag still hanging around).
        cleanupMissingMedia();
    });
}

void MainWindow::syncLoopMenu()
{
    if (!m_loopPanel)
        return;

    if (m_loopSortMenu)
    {
        const int value = static_cast<int>(m_loopPanel->sortMode()) * 2
                          + (m_loopPanel->sortDescending() ? 1 : 0);
        for (QAction* action : m_loopSortMenu->actions())
            action->setChecked(action->data().toInt() == value);
    }
    if (m_loopListAction)
    {
        m_loopListAction->blockSignals(true);
        m_loopListAction->setChecked(!m_loopCollapsed);
        m_loopListAction->blockSignals(false);
    }
    if (m_loopAutoNextAction && m_loopPanel)
    {
        m_loopAutoNextAction->blockSignals(true);
        m_loopAutoNextAction->setChecked(m_loopPanel->autoNext());
        m_loopAutoNextAction->blockSignals(false);
    }
    buildLoopFolderMenu();
    buildLoopTagMenu();
}

void MainWindow::buildLoopFolderMenu()
{
    if (!m_loopFolderMenu || !m_loopPanel || !m_loopStore)
        return;
    m_loopFolderMenu->clear();

    QAction* allAction = m_loopFolderMenu->addAction(LT("全部片段"));
    allAction->setCheckable(true);
    allAction->setChecked(m_loopPanel->folderView() == LoopListPanel::FolderView::All);
    connect(allAction, &QAction::triggered, m_loopPanel, &LoopListPanel::showAllClips);

    QAction* noneAction = m_loopFolderMenu->addAction(LT("未归类"));
    noneAction->setCheckable(true);
    noneAction->setChecked(m_loopPanel->folderView() == LoopListPanel::FolderView::Unassigned);
    connect(noneAction, &QAction::triggered, m_loopPanel,
            &LoopListPanel::showUnassignedClips);

    m_loopFolderMenu->addSeparator();
    if (m_loopStore->folders().isEmpty())
    {
        QAction* emptyAction = m_loopFolderMenu->addAction(LT("（还没有收藏夹）"));
        emptyAction->setEnabled(false);
    }
    for (const LoopFolder& folder : m_loopStore->folders())
    {
        QAction* action = m_loopFolderMenu->addAction(
            QStringLiteral("%1 (%2)")
                .arg(folder.name)
                .arg(m_loopStore->folderClipCount(folder.id)));
        action->setCheckable(true);
        action->setChecked(m_loopPanel->folderView() == LoopListPanel::FolderView::Folder
                           && m_loopPanel->folderId() == folder.id);
        const QString folderId = folder.id;
        connect(action, &QAction::triggered, m_loopPanel, [this, folderId]() {
            m_loopPanel->showFolder(folderId);
        });
    }

    m_loopFolderMenu->addSeparator();
    QAction* manageAction = m_loopFolderMenu->addAction(LT("管理循环收藏夹…"));
    connect(manageAction, &QAction::triggered, this, &MainWindow::openLoopManagerDialog);
}

void MainWindow::buildLoopTagMenu()
{
    if (!m_loopTagMenu || !m_loopPanel || !m_loopStore)
        return;
    m_loopTagMenu->clear();

    QAction* allAction = m_loopTagMenu->addAction(LT("全部标签"));
    allAction->setCheckable(true);
    allAction->setChecked(m_loopPanel->tagView() == LoopListPanel::TagView::All);
    connect(allAction, &QAction::triggered, m_loopPanel, &LoopListPanel::showAllTags);

    QAction* noneAction = m_loopTagMenu->addAction(LT("无标签"));
    noneAction->setCheckable(true);
    noneAction->setChecked(m_loopPanel->tagView() == LoopListPanel::TagView::Untagged);
    connect(noneAction, &QAction::triggered, m_loopPanel, &LoopListPanel::showUntagged);

    m_loopTagMenu->addSeparator();
    const QStringList tags = m_loopStore->tagsByUsage();
    if (tags.isEmpty())
    {
        QAction* emptyAction = m_loopTagMenu->addAction(LT("（还没有标签）"));
        emptyAction->setEnabled(false);
    }
    for (const QString& tag : tags)
    {
        QAction* action = m_loopTagMenu->addAction(
            QStringLiteral("%1 (%2)").arg(tag).arg(m_loopStore->tagUsage(tag)));
        action->setCheckable(true);
        action->setChecked(m_loopPanel->tagView() == LoopListPanel::TagView::Tag
                           && m_loopPanel->tag().compare(tag, Qt::CaseInsensitive) == 0);
        connect(action, &QAction::triggered, m_loopPanel,
                [this, tag]() { m_loopPanel->showTag(tag); });
    }

    m_loopTagMenu->addSeparator();
    QAction* manageAction = m_loopTagMenu->addAction(LT("管理标签…"));
    connect(manageAction, &QAction::triggered, this, &MainWindow::openLoopManagerDialog);
}

void MainWindow::openLoopManagerDialog()
{
    if (!m_loopStore || !m_library)
        return;

    LoopManagerDialog dialog(this, m_loopStore, m_library->availablePathSet(),
                             m_library->unreachableRoots());
    connect(&dialog, &LoopManagerDialog::cleanupRequested, this, [this, &dialog]() {
        cleanupMissingMedia();
        dialog.setAvailability(m_library->availablePathSet(),
                               m_library->unreachableRoots());
    });
    connect(&dialog, &LoopManagerDialog::clipsChanged, this, [this]() {
        if (m_loopPanel)
            m_loopPanel->refresh();
        if (!m_activeLoopClipId.isEmpty() && !m_loopStore->containsClip(m_activeLoopClipId))
            stopLoopPlayback(LT("片段已被删除"));
    });
    dialog.exec();
    if (m_loopPanel)
        m_loopPanel->refresh();
}

void MainWindow::openRelinkDialog(const QString& onlyPath)
{
    if (!m_library || !m_loopStore)
        return;

    QList<MediaItem> missing = m_library->missingItems();
    if (!onlyPath.isEmpty())
    {
        const QString key = PathUtils::normalizeMediaPath(onlyPath);
        QList<MediaItem> filtered;
        for (const MediaItem& item : missing)
        {
            if (PathUtils::normalizeMediaPath(item.path) == key)
                filtered.append(item);
        }
        if (filtered.isEmpty())
            return;
        missing = filtered;
    }

    if (missing.isEmpty())
    {
        showStatusMessage(LT("没有失效条目需要关联"), 3000);
        return;
    }

    RelinkDialog dialog(this, missing, m_library->items());
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QHash<QString, QString> mapping = dialog.mapping();
    if (mapping.isEmpty())
        return;

    MediaCleanup cleanup(m_library->favoritesStore(), m_loopStore);
    // Relinking rewrites the same files a clean-up does, so the same safety net
    // applies.
    cleanup.backup();
    const int moved = cleanup.relinkAll(mapping);

    m_library->favoritesStore()->save();
    m_loopStore->save();

    m_library->refreshFavorites();
    m_library->refreshAll();
    if (m_loopPanel)
        m_loopPanel->refresh();
    refreshLoopPanelContext();

    showStatusMessage(LT("已重新关联 %1 条记录").arg(moved), 4000);
}

void MainWindow::cleanupMissingMedia()
{
    if (!m_library || !m_loopStore)
        return;

    if (m_library->hasUnreachableMedia())
    {
        MediaCleanupPrompt::warnUnreachable(this, m_library->unreachableRoots());
        return;
    }

    const QSet<QString> stale = m_library->missingPaths();
    if (!MediaCleanupPrompt::run(this, m_library->favoritesStore(), m_loopStore, stale))
        return;

    m_library->refreshFavorites();
    m_library->refreshAll();
    if (m_loopPanel)
        m_loopPanel->refresh();
    refreshLoopPanelContext();
}

void MainWindow::openLibraryDialog()
{
    showLibraryDialog(false);
}

void MainWindow::openFavoritesDialog()
{
    showLibraryDialog(true);
}

void MainWindow::showLibraryDialog(bool showFavorites)
{
    if (!m_library)
        return;

    MediaLibraryDialog dialog(this, m_library->favoritesStore());
    dialog.setRoots(m_library->rootFolders());
    dialog.setFavoritesContext(m_library->availablePathSet(),
                               m_library->unreachableRoots());

    connect(&dialog, &MediaLibraryDialog::addRequested, this,
            [this, &dialog](const QString& folder) {
                m_library->addRootFolder(folder);
                dialog.setRoots(m_library->rootFolders());
            });
    connect(&dialog, &MediaLibraryDialog::removeRequested, this,
            [this, &dialog](const QString& folder) {
                m_library->removeRootFolder(folder);
                dialog.setRoots(m_library->rootFolders());
            });
    connect(&dialog, &MediaLibraryDialog::favoritesChanged, this, [this, &dialog]() {
        if (m_library)
            m_library->refreshFavorites();
        dialog.setFavoritesContext(m_library->availablePathSet(),
                                   m_library->unreachableRoots());
    });
    connect(&dialog, &MediaLibraryDialog::cleanupRequested, this, [this, &dialog]() {
        cleanupMissingMedia();
        dialog.setFavoritesContext(m_library->availablePathSet(),
                                   m_library->unreachableRoots());
    });

    if (showFavorites)
        dialog.showFavoritesTab();
    dialog.exec();
}

void MainWindow::updateDeviceIndicator(bool connected)
{
    const QString color = connected ? QStringLiteral("#30D158")
                                    : QStringLiteral("#FF453A");
    if (m_statusDevice)
        m_statusDevice->setStyleSheet(QStringLiteral("color: %1;").arg(color));
    if (m_deviceChip)
    {
        m_deviceChip->setText(connected ? LT("断开 SR6")
                                        : LT("连接 SR6"));
        m_deviceChip->setStyleSheet(QStringLiteral("QToolButton { color: %1; }").arg(color));
    }
}

void MainWindow::buildMenus()
{
    auto* fileMenu = menuBar()->addMenu(LT("文件"));
    auto* openVideoAction = fileMenu->addAction(LT("打开视频…"));
    openVideoAction->setShortcut(QKeySequence::Open);
    auto* openFolderAction = fileMenu->addAction(LT("打开文件夹…"));
    openFolderAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")));
    auto* stopVideoAction = fileMenu->addAction(LT("终止播放"));
    stopVideoAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    fileMenu->addSeparator();
    auto* quitAction = fileMenu->addAction(LT("退出"));
    quitAction->setShortcut(QKeySequence::Quit);

    auto* viewMenu = menuBar()->addMenu(LT("视图"));
    m_libraryAction = viewMenu->addAction(LT("显示媒体库"));
    m_libraryAction->setCheckable(true);
    m_libraryAction->setChecked(true);
    m_loopListAction = viewMenu->addAction(LT("显示循环列表"));
    m_loopListAction->setCheckable(true);
    m_loopListAction->setChecked(!m_loopCollapsed);
    viewMenu->addSeparator();
    m_fullScreenAction = viewMenu->addAction(LT("视频全屏"));
    m_fullScreenAction->setCheckable(true);
    m_fullScreenAction->setShortcut(QKeySequence::FullScreen);
    viewMenu->addSeparator();
    m_heatmapAction = viewMenu->addAction(LT("显示热力图"));
    m_heatmapAction->setCheckable(true);
    m_heatmapAction->setChecked(m_heatmapVisible);
    m_heatmapAction->setToolTip(LT("在控制栏下方显示脚本动作热力图"));
    m_axisTagAction = viewMenu->addAction(LT("显示单轴/多轴标签"));
    m_axisTagAction->setCheckable(true);
    m_axisTagAction->setChecked(m_axisTagVisible);
    m_axisTagAction->setToolTip(LT("在媒体库与循环列表的文件名前显示脚本轴数"));
    viewMenu->addSeparator();
    m_swapSidesAction = viewMenu->addAction(LT("交换媒体库与循环列表位置"));
    m_swapSidesAction->setCheckable(true);
    m_swapSidesAction->setChecked(m_swappedSides);
    m_swapSidesAction->setToolTip(
        LT("把循环列表放到左侧、媒体库放到右侧（默认相反）"));

    // Card size and font size are view settings, not library settings: the loop
    // list mirrors the media library's card width and font size, so one menu has
    // to drive both. Under "媒体库" they looked like they only affected the
    // library grid.
    viewMenu->addSeparator();
    m_viewThumbMenu = viewMenu->addMenu(LT("缩略图大小"));
    m_viewThumbMenu->setToolTip(
        LT("同时作用于媒体库与循环列表的封面尺寸"));
    auto* thumbGroup = new QActionGroup(this);
    for (int pixels : {100, 150, 200, 250, 300, 400})
    {
        QAction* action = m_viewThumbMenu->addAction(
            QStringLiteral("%1 px").arg(pixels));
        action->setCheckable(true);
        action->setData(pixels);
        thumbGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, pixels]() {
            if (m_library)
                m_library->setThumbSize(pixels);
        });
    }

    m_viewFontMenu = viewMenu->addMenu(LT("字体大小"));
    m_viewFontMenu->setToolTip(
        LT("同时作用于媒体库与循环列表的文件名字号"));
    auto* fontGroup = new QActionGroup(this);
    for (int percent : {80, 100, 120, 140, 160, 200})
    {
        QAction* action = m_viewFontMenu->addAction(
            QStringLiteral("%1%").arg(percent));
        action->setCheckable(true);
        action->setData(percent);
        fontGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, percent]() {
            if (m_library)
                m_library->setFontPercent(percent);
        });
    }

    m_libraryMenu = menuBar()->addMenu(LT("媒体库"));
    auto* manageLibraryAction = m_libraryMenu->addAction(LT("媒体库管理…"));
    auto* refreshLibraryAction = m_libraryMenu->addAction(LT("刷新媒体库"));
    refreshLibraryAction->setShortcut(QKeySequence(QStringLiteral("F5")));
    m_libraryMenu->addSeparator();
    auto* relinkAction = m_libraryMenu->addAction(LT("重新关联失效条目…"));
    relinkAction->setToolTip(
        LT("整理过文件夹后，把失效的收藏与循环片段认到视频的新位置"));
    auto* cleanupAction = m_libraryMenu->addAction(LT("清理失效数据…"));
    cleanupAction->setToolTip(
        LT("一次清掉失效的收藏记录、循环片段和不再使用的标签"));
    m_libraryMenu->addSeparator();

    m_libraryFavoritesMenu = m_libraryMenu->addMenu(LT("收藏夹"));
    m_libraryMenu->addSeparator();

    m_librarySortMenu = m_libraryMenu->addMenu(LT("排序"));
    auto* sortGroup = new QActionGroup(this);
    const struct
    {
        QString label;
        MediaLibraryStore::SortMode mode;
        bool descending;
    } sortEntries[] = {
        {LT("名称 A→Z"), MediaLibraryStore::SortMode::Name, false},
        {LT("名称 Z→A"), MediaLibraryStore::SortMode::Name, true},
        {LT("时长 短→长"), MediaLibraryStore::SortMode::Duration, false},
        {LT("时长 长→短"), MediaLibraryStore::SortMode::Duration, true},
        {LT("修改日期 旧→新"), MediaLibraryStore::SortMode::RecentlyModified, false},
        {LT("修改日期 新→旧"), MediaLibraryStore::SortMode::RecentlyModified, true}
    };
    for (const auto& entry : sortEntries)
    {
        QAction* action = m_librarySortMenu->addAction(entry.label);
        action->setCheckable(true);
        action->setData(static_cast<int>(entry.mode) * 2 + (entry.descending ? 1 : 0));
        sortGroup->addAction(action);
        const auto mode = entry.mode;
        const bool descending = entry.descending;
        connect(action, &QAction::triggered, this, [this, mode, descending]() {
            if (m_library)
            {
                m_library->setSortMode(mode);
                m_library->setSortDescending(descending);
            }
        });
    }

    m_libraryOnlyScriptAction = m_libraryMenu->addAction(LT("仅显示有脚本"));
    m_libraryOnlyScriptAction->setCheckable(true);

    m_autoNextAction = m_libraryMenu->addAction(LT("媒体库自动连播"));
    m_autoNextAction->setCheckable(true);
    m_autoNextAction->setChecked(m_autoNext);
    m_autoNextAction->setToolTip(
        LT("一个视频播完自动接着播媒体库列表里的下一个"));

    m_libraryMenu->addSeparator();
    auto* regenerateAllAction = m_libraryMenu->addAction(LT("重新生成全部缩略图"));
    auto* stopThumbsAction = m_libraryMenu->addAction(LT("停止生成缩略图"));
    auto* clearCacheAction = m_libraryMenu->addAction(LT("清理缩略图缓存"));

    buildLoopMenu();

    m_settingsMenu = menuBar()->addMenu(LT("设置"));
    m_axisLimitAction = m_settingsMenu->addAction(LT("轴限制…"));
    m_axisLimitAction->setToolTip(LT("全局行程比例与多套配置"));
    auto* calibrationAction = m_settingsMenu->addAction(LT("轴校准与测试…"));
    calibrationAction->setToolTip(
        LT("逐轴设置最小/最大/归位/反向/时间偏移，并可驱动设备试位"));
    auto* seekSafetyAction = m_settingsMenu->addAction(LT("跳转安全…"));
    // The play/pause easing switch lives inside this dialog: it shares the ramp
    // parameters, and that is where the easing is explained.
    seekSafetyAction->setToolTip(
        LT("跳转 / 切换的缓动速度，以及播放 / 暂停是否同样缓动"));

    // Decoding choice. Hardware decoding keeps the CPU free, but every interop
    // backend measured on this machine renders one uninitialised frame (a light
    // block) while the window changes size - visible as a white flash when the
    // picture goes full screen. Software decoding is the escape hatch, and the
    // menu says so.
    auto* hwdecMenu = m_settingsMenu->addMenu(LT("视频解码"));
    hwdecMenu->setToolTip(LT("切换全屏时画面闪白的话，选“软件解码”"));
    {
        const struct
        {
            QString label;
            int mode;
            QString tip;
        } entries[] = {
            {LT("自动（推荐）"), 0, LT("能用硬件解码就用，失败自动回退软件解码")},
            {LT("优先硬件解码"), 1, LT("尝试更多硬件解码后端，失败仍回退软件解码")},
            {LT("软件解码"), 2, LT("完全由 CPU 解码。切换全屏时若画面闪白，选这一项即可消除")}
        };
        auto* hwdecGroup = new QActionGroup(this);
        for (const auto& entry : entries)
        {
            QAction* action = hwdecMenu->addAction(entry.label);
            action->setCheckable(true);
            action->setChecked(entry.mode == m_hwdecMode);
            action->setToolTip(entry.tip);
            hwdecGroup->addAction(action);
            const int mode = entry.mode;
            connect(action, &QAction::triggered, this, [this, mode] {
                m_hwdecMode = mode;
                QSettings().setValue(QStringLiteral("playback/hwdec"), mode);
                if (m_engine)
                {
                    m_engine->setHardwareDecoding(
                        static_cast<MpvEngine::HardwareDecoding>(mode));
                }
                AppLogger::log(QStringLiteral("ui"),
                               QStringLiteral("用户切换解码方式: %1")
                                   .arg(MpvEngine::hardwareDecodingName(
                                       static_cast<MpvEngine::HardwareDecoding>(mode))));
                showStatusMessage(LT("解码方式已保存，下次打开视频时生效"), 4000);
            });
        }
    }

    // Per-command log detail. Over half of a session's log was [tcode] lines,
    // which rotated the useful part away within minutes; the switch brings them
    // back when the link itself is being diagnosed. Off by default - the five
    // second summary still reports the command rate and the worst tick gap.
    m_settingsMenu->addSeparator();
    m_tcodeDetailAction = m_settingsMenu->addAction(LT("记录每条 TCode 命令（诊断）"));
    m_tcodeDetailAction->setCheckable(true);
    m_tcodeDetailAction->setChecked(AppLogger::tcodeDetailEnabled());
    m_tcodeDetailAction->setToolTip(
        LT("关闭时只记录每 5 秒的联动统计；排查串口问题时打开"));
    connect(m_tcodeDetailAction, &QAction::toggled, this, [this](bool enabled) {
        AppLogger::setTcodeDetailEnabled(enabled);
        QSettings().setValue(QStringLiteral("sync/tcodeDetail"), enabled);
        AppLogger::log(QStringLiteral("ui"),
                       QStringLiteral("TCode 逐条日志: %1")
                           .arg(enabled ? QStringLiteral("开启") : LT("关闭")));
        showStatusMessage(enabled ? LT("已开启 TCode 逐条日志（诊断用）")
                                  : LT("已关闭 TCode 逐条日志，仅保留联动统计"),
                          4000);
    });

    // 界面语言。文本在窗口构造时就取好词了，所以切换只能靠重启换一套；
    // 这里只负责记住选择，并在用户同意后立刻重启。
    m_settingsMenu->addSeparator();
    auto* languageMenu = m_settingsMenu->addMenu(LT("语言"));
    languageMenu->setToolTip(LT("界面语言，切换后重新启动生效"));
    {
        const struct
        {
            Loc::Language language;
            // 语言名写成它自己的语言，切错了也认得回来。
            QString label;
        } languages[] = {
            {Loc::Language::English, QStringLiteral("English")},
            {Loc::Language::Chinese, QStringLiteral("简体中文")}
        };
        auto* languageGroup = new QActionGroup(this);
        for (const auto& entry : languages)
        {
            QAction* action = languageMenu->addAction(entry.label);
            action->setCheckable(true);
            action->setChecked(entry.language == Loc::language());
            languageGroup->addAction(action);
            const Loc::Language language = entry.language;
            connect(action, &QAction::triggered, this,
                    [this, language] { setInterfaceLanguage(language); });
        }
    }

    auto* helpMenu = menuBar()->addMenu(LT("帮助"));
    auto* logAction = helpMenu->addAction(LT("打开日志目录"));
    helpMenu->addSeparator();
    auto* aboutAction = helpMenu->addAction(LT("关于 NekoBeat"));

    auto* corner = new QWidget(this);
    auto* cornerLayout = new QHBoxLayout(corner);
    cornerLayout->setContentsMargins(0, 0, 10, 0);
    // The chips carry no stylesheet padding (see Theme.cpp), so the gap between
    // them is purely this spacing - and unlike padding, the layout accounts for
    // it when the status row is measured.
    cornerLayout->setSpacing(14);

    // Uptime of this session, in the empty stretch left of the status chips.
    m_uptimeLabel = new QLabel(corner);
    m_uptimeLabel->setObjectName(QStringLiteral("StatusChip"));
    m_uptimeLabel->setToolTip(LT("本次启动时长"));
    cornerLayout->addWidget(m_uptimeLabel);

    cornerLayout->addWidget(m_statusDevice);
    // Link quality, updated once per statistics window by the scheduler.
    m_statusLink = new QLabel(corner);
    m_statusLink->setObjectName(QStringLiteral("StatusChip"));
    m_statusLink->setText(LT("同步 --"));
    m_statusLink->setToolTip(LT("设备指令与脚本时基的偏差（每 5 秒更新）"));
    cornerLayout->addWidget(m_statusLink);
    m_messageLabel = new QLabel(corner);
    m_messageLabel->setObjectName(QStringLiteral("StatusChip"));
    // The menu bar only hands its right-hand corner so much room, so the hint chip
    // is capped hard: a long hint used to push the row past that budget and every
    // chip in it got squeezed. showStatusMessage() elides to this same width, so
    // the text ends in an ellipsis instead of being cropped at both ends.
    m_messageLabel->setMaximumWidth(150);
    // Starts hidden: showStatusMessage() reveals it only while it has something
    // to say, so an idle session does not spend the row's width on an empty chip.
    m_messageLabel->setVisible(false);
    cornerLayout->addWidget(m_messageLabel);
    cornerLayout->addWidget(m_statusScript);
    m_deviceChip = new QToolButton(corner);
    m_deviceChip->setObjectName(QStringLiteral("Compact"));
    m_deviceChip->setText(LT("连接 SR6"));
    m_deviceChip->setToolTip(LT("连接或断开设备"));
    m_deviceChip->setCursor(Qt::PointingHandCursor);
    cornerLayout->addWidget(m_deviceChip);
    menuBar()->setCornerWidget(corner, Qt::TopRightCorner);

    m_uptimeClock.start();
    m_uptimeTimer = new QTimer(this);
    m_uptimeTimer->setInterval(1000);
    connect(m_uptimeTimer, &QTimer::timeout, this, &MainWindow::updateUptime);
    m_uptimeTimer->start();
    updateUptime();

    connect(openVideoAction, &QAction::triggered, this, &MainWindow::openVideo);
    connect(openFolderAction, &QAction::triggered, this, &MainWindow::openFolder);
    connect(stopVideoAction, &QAction::triggered, this, &MainWindow::stopPlayback);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);
    connect(m_libraryAction, &QAction::toggled, this, [this](bool visible) {
        if (!m_fullScreen)
            setLibraryCollapsed(!visible);
    });
    connect(m_loopListAction, &QAction::toggled, this, [this](bool visible) {
        if (!m_fullScreen)
            setLoopListCollapsed(!visible);
    });
    connect(m_fullScreenAction, &QAction::toggled, this, &MainWindow::toggleFullScreen);
    connect(m_heatmapAction, &QAction::toggled, this, [this](bool visible) {
        m_heatmapVisible = visible;
        if (m_heatmap)
            m_heatmap->setVisible(visible);
        // The bar changes height with its second row, so the floating overlay
        // has to be re-glued to the bottom of the picture.
        if (m_controlsFloating)
            positionFloatingPlayerBar();
    });
    connect(m_axisTagAction, &QAction::toggled, this, [this](bool visible) {
        m_axisTagVisible = visible;
        if (m_library)
            m_library->setAxisTagVisible(visible);
        if (m_loopPanel)
            m_loopPanel->setAxisTagVisible(visible);
    });
    connect(m_swapSidesAction, &QAction::toggled, this, &MainWindow::setSwappedSides);
    connect(m_libraryMenu, &QMenu::aboutToShow, this, &MainWindow::syncLibraryMenu);
    connect(manageLibraryAction, &QAction::triggered, this,
            &MainWindow::openLibraryDialog);
    connect(refreshLibraryAction, &QAction::triggered, this, [this]() {
        if (m_library)
            m_library->refreshAll();
    });
    connect(relinkAction, &QAction::triggered, this,
            [this]() { openRelinkDialog(); });
    connect(cleanupAction, &QAction::triggered, this,
            &MainWindow::cleanupMissingMedia);
    connect(m_libraryOnlyScriptAction, &QAction::toggled, this, [this](bool enabled) {
        if (m_library)
            m_library->setScriptFilter(enabled);
    });
    connect(regenerateAllAction, &QAction::triggered, this, [this]() {
        if (m_library)
            m_library->regenerateAllThumbnails();
    });
    connect(stopThumbsAction, &QAction::triggered, this, [this]() {
        if (m_library)
            m_library->stopThumbnailGeneration();
    });
    connect(clearCacheAction, &QAction::triggered, this, [this]() {
        if (!m_library)
            return;
        if (QMessageBox::question(
                this, LT("清理缩略图缓存"),
                LT("将删除本地缩略图与元数据缓存并重新生成，不会删除任何视频或脚本。继续？"))
            != QMessageBox::Yes)
        {
            return;
        }
        m_library->clearThumbnailCache();
    });
    connect(m_axisLimitAction, &QAction::triggered, this, &MainWindow::showAxisLimitPanel);
    connect(calibrationAction, &QAction::triggered, this, &MainWindow::openCalibrationDialog);
    connect(seekSafetyAction, &QAction::triggered, this, &MainWindow::configureSeekSafety);
    connect(m_autoNextAction, &QAction::toggled, this, [this](bool enabled) {
        setAutoNextEnabled(enabled);
        showStatusMessage(enabled ? LT("已开启自动连播")
                                         : LT("已关闭自动连播"),
                                 3000);
    });
    connect(logAction, &QAction::triggered, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(AppLogger::logDirectory()));
    });
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
    connect(m_deviceChip, &QToolButton::clicked, this, &MainWindow::toggleDeviceConnection);
}

void MainWindow::showAxisLimitPanel()
{
    // Showing a Qt::Popup directly from the menu action can be swallowed by the
    // same mouse event, so let the event finish first.
    QTimer::singleShot(0, this, [this] {
        // Anchor the panel under the "设置" menu entry it lives in.
        const QRect rect = menuBar()->actionGeometry(m_settingsMenu->menuAction());
        QPoint pos = menuBar()->mapToGlobal(QPoint(rect.left() - 14, rect.bottom() + 2));
        pos.setX(std::max(0, pos.x()));
        m_axisPanel->showAt(pos);
    });
}

void MainWindow::openCalibrationDialog()
{
    if (m_engine && m_engine->isPlaying() && m_link && m_link->isRunning())
    {
        // The test buttons drive the device directly; during playback the
        // scheduler owns the output and the two would fight over it.
        showStatusMessage(LT("播放联动中不可校准，请先暂停或终止"), 4000);
        return;
    }

    // Limits, home, invert and offset are worth editing before the machine is
    // even plugged in, so the dialog always opens; only the three "drive there"
    // buttons need a device (they enable themselves once one is connected).
    const bool deviceReady = m_link && m_link->isConnected()
                             && m_link->capabilities().probeComplete;
    if (!deviceReady)
    {
        showStatusMessage(
            LT("未连接设备：可以先编辑并保存校准值，试位按钮连上设备后自动可用"),
            5000);
    }

    AppLogger::log(QStringLiteral("calibration"),
                   QStringLiteral("打开轴校准对话框（设备就绪=%1）").arg(deviceReady ? 1 : 0));
    CalibrationDialog dialog(m_link, m_bundle, this);
    connect(&dialog, &CalibrationDialog::axisConfigChanged, this,
            [this](Track track, AxisConfig config) {
                // The profile is the source of truth for limits, so the change
                // is stored there first and then applied to the running script.
                if (m_axisPanel)
                    m_axisPanel->applyAxisConfig(track, config);

                const int key = static_cast<int>(track);
                if (m_bundle.tracks.contains(key))
                {
                    m_bundle.tracks[key].config = config;
                    m_link->setBundle(m_bundle);
                    realignDeviceIfNeeded();
                }
                showStatusMessage(LT("已应用 %1 的校准值")
                                      .arg(TrackInfo::tcodeId(track)),
                                  3000);
            });
    dialog.exec();
}

void MainWindow::toggleFullScreen(bool enabled)
{
    if (m_fullScreen == enabled)
        return;

    QElapsedTimer transitionClock;
    transitionClock.start();
    const qint64 transitionStartedAt = QDateTime::currentMSecsSinceEpoch();
    m_fullScreen = enabled;

    if (enabled)
    {
        // Capture the panel widths while the side panels are still laid out:
        // hiding them makes the splitter report a width of 0, and the full screen
        // overlays need the real numbers.
        m_libraryOverlayWidth =
            (m_libraryFold && !m_libraryCollapsed)
                ? std::max(200, m_libraryFold->panelWidth())
                : std::max(200, m_libraryWidth);
        m_loopOverlayWidth = (m_loopFold && !m_loopCollapsed)
                                 ? std::max(200, m_loopFold->panelWidth())
                                 : std::max(200, m_loopWidth);
    }

    auto* rootLayout = centralWidget() ? qobject_cast<QVBoxLayout*>(centralWidget()->layout()) : nullptr;
    // Batch the whole transition into a single layout pass. A disabled layout
    // does not move the widgets it manages, so showing the menu bar, the panels
    // and the control bar can no longer resize the picture one step at a time.
    // Measured on a 4090: going back to the window used to cost 117 ms of main
    // thread time (30 ms menu bar, 32 ms panel, 52 ms window resize), because
    // every one of those steps resized the picture and made mpv reconfigure its
    // render target. With the layouts batched the synchronous part is a few
    // milliseconds and the final geometry is applied in a single pass. The
    // layouts are always re-enabled and activated before returning.
    const QList<QLayout*> batchedLayouts = {
        layout(), rootLayout, m_splitter ? m_splitter->layout() : nullptr
    };
    for (QLayout* batched : batchedLayouts)
    {
        if (batched)
            batched->setEnabled(false);
    }

    struct LayoutGuard
    {
        const QList<QLayout*>& layouts;
        ~LayoutGuard()
        {
            for (QLayout* batched : layouts)
            {
                if (batched)
                    batched->setEnabled(true);
            }
            for (QLayout* batched : layouts)
            {
                if (batched)
                    batched->activate();
            }
        }
    } layoutGuard{batchedLayouts};    if (enabled)
    {
        // Remember whether the window is maximized: leaving full screen has to
        // restore that state directly instead of going through showNormal(),
        // which would un-maximize the window and cost a second window state
        // transition.
        m_maximizedBeforeFullScreen = isMaximized();
        m_nativeFullscreen = false;
        if (m_useNativeFullScreen && NativeFullScreen::isSupported())
        {
            // Captured before the layout changes below, while the window still
            // has its windowed frame and rectangle.
            NativeFullScreen::captureWindowState(winId(), &m_nativeRestoreRect,
                                                 &m_savedWindowStyle);
            // New areas of a growing window are erased with the window class
            // brush before anything is painted; the default white brush is what
            // flashes along the edges when the picture grows to full screen.
            NativeFullScreen::useBlackBackground(winId());
        }
        menuBar()->setVisible(false);
        statusBar()->setVisible(false);
        if (m_librarySide)
            m_librarySide->setVisible(false);
        if (m_loopSide)
            m_loopSide->setVisible(false);
        if (rootLayout)
            rootLayout->setContentsMargins(0, 0, 0, 0);
        // The fold chevrons and the edge grab strips belong to the windowed
        // layout; over the picture they would sit right on top of the video.
        if (m_libraryFold)
            m_libraryFold->setFullScreen(true);
        if (m_loopFold)
            m_loopFold->setFullScreen(true);
        // The control bar leaves the player column for the picture before the
        // window grows, so freeing that height is already part of the same
        // single resize.
        setFullScreenControlsFloating(true);
        // Nothing but the picture by default; the cursor brings the control bar
        // back when it reaches the bottom edge.
        setFullScreenControlsVisible(false);
        // The media library and the loop list leave the splitter and wait at the
        // screen edges; the cursor brings them in.
        applyFullScreenPanelHosting(true);
        if (m_useNativeFullScreen && NativeFullScreen::isSupported())
        {
            // One style change plus one SetWindowPos: the window never passes
            // through an intermediate "small window" state, so the picture does
            // not jump and mpv does not have to re-configure its video chain
            // twice.
            NativeFullScreen::enter(winId());
            m_nativeFullscreen = true;
        }
        else
        {
            showFullScreen();
        }
        setFullScreenCursorHidden(false);
        if (m_cursorIdleTimer)
            m_cursorIdleTimer->start();
        if (m_fullScreenPanelTimer)
            m_fullScreenPanelTimer->start();
    }
    else
    {
        // Mirror image: rebuild the windowed layout first (the bar goes back
        // into the player column and the panels come back), then let the window
        // shrink to its windowed geometry in one step.
        setFullScreenControlsFloating(false);
        setFullScreenControlsVisible(true);
        if (m_libraryFold)
            m_libraryFold->setFullScreen(false);
        if (m_loopFold)
            m_loopFold->setFullScreen(false);
        menuBar()->setVisible(true);
        statusBar()->setVisible(true);
        if (m_librarySide)
            m_librarySide->setVisible(true);
        m_library->setVisible(!m_libraryCollapsed && m_libraryAction->isChecked());
        if (m_loopSide)
            m_loopSide->setVisible(true);
        if (m_loopPanel)
            m_loopPanel->setVisible(!m_loopCollapsed && m_loopListAction->isChecked());
        applyFullScreenPanelHosting(false);
        if (rootLayout)
            rootLayout->setContentsMargins(6, 6, 6, 4);
        // Return to the state the window had before full screen. showNormal() on
        // a maximized window would restore it to its normal size first, and that
        // extra transition is what made leaving full screen hitch.
        if (m_nativeFullscreen)
        {
            NativeFullScreen::leave(winId(), m_savedWindowStyle,
                                    m_maximizedBeforeFullScreen, m_nativeRestoreRect);
            m_nativeFullscreen = false;
        }
        else if (m_maximizedBeforeFullScreen)
        {
            showMaximized();
        }
        else
        {
            showNormal();
        }
        if (m_cursorIdleTimer)
            m_cursorIdleTimer->stop();
        if (m_fullScreenPanelTimer)
            m_fullScreenPanelTimer->stop();
        setFullScreenCursorHidden(false);
    }

    // The scheduler shares this thread, so a slow transition is exactly what the
    // device feels as a hitch. Two numbers are logged: the synchronous part of
    // the switch, and how long it takes until the event loop is free again
    // (deferred layout, repaint and mpv render target reconfiguration included).
    // The second one is what the device feels. Logging them keeps a remaining
    // stall measurable instead of a guess.
    const qint64 synchronousMs = transitionClock.elapsed();
    QTimer::singleShot(0, this, [this, transitionStartedAt, enabled, synchronousMs]() {
        // The frame counters belong in this line: comparing the value logged on
        // "进入" with the one on the next "退出" tells immediately whether the
        // picture kept being rendered while that mode was active.
        AppLogger::log(QStringLiteral("ui"),
                       QStringLiteral("全屏切换: %1，同步 %2 ms / 事件循环空闲 %3 ms（frames=%4 requests=%5）")
                           .arg(enabled ? QStringLiteral("进入") : LT("退出"))
                           .arg(synchronousMs)
                           .arg(QDateTime::currentMSecsSinceEpoch() - transitionStartedAt)
                           .arg(renderedFrameCount())
                           .arg(renderRequestCount()));
    });
}

void MainWindow::setupFullScreenControls()
{
    m_cursorIdleTimer = new QTimer(this);
    m_cursorIdleTimer->setSingleShot(true);
    m_cursorIdleTimer->setInterval(kFullScreenIdleHideMs);
    connect(m_cursorIdleTimer, &QTimer::timeout,
            this, &MainWindow::hideIdleFullScreenOverlays);

    // Only runs while full screen; it is the safety net for the edge panels.
    m_fullScreenPanelTimer = new QTimer(this);
    m_fullScreenPanelTimer->setInterval(kFullScreenPanelPollMs);
    connect(m_fullScreenPanelTimer, &QTimer::timeout, this, [this] {
        if (m_fullScreen)
            updateFullScreenPanelsForCursor();
    });

    // A widget only reports plain mouse motion once tracking is on, and the
    // cursor can sit on any of these while the window is full screen, so each
    // of them feeds the show/hide decision.
    m_cursorSources = {
        centralWidget(), m_mediaColumn, m_videoStack, m_video, m_blackScreen,
        m_playerBar, m_heatmap
    };
    if (m_blackScreen)
        m_cursorSources += m_blackScreen->findChildren<QWidget*>();
    if (m_playerBar)
        m_cursorSources += m_playerBar->findChildren<QWidget*>();

    for (QWidget* source : m_cursorSources)
    {
        if (!source)
            continue;
        source->setMouseTracking(true);
        source->installEventFilter(this);
    }
}

bool MainWindow::fullScreenControlZoneContainsCursor() const
{
    if (!m_fullScreen || !m_playerBar)
        return false;

    const QRect windowRect(mapToGlobal(QPoint(0, 0)), size());
    const QPoint cursor = QCursor::pos();
    if (!windowRect.contains(cursor))
        return false;

    // Reveal strip = the control bar plus the approach margin above it, so the
    // bar is already there by the time the cursor reaches it.
    const int barHeight = std::max(m_playerBar->sizeHint().height(), 44);
    const int strip = std::min(barHeight + kFullScreenControlZoneMargin, height() / 3);
    return cursor.y() >= windowRect.bottom() - strip;
}

bool MainWindow::fullScreenPanelPopupOpen() const
{
    QWidget* popup = QApplication::activePopupWidget();
    if (!popup)
        return false;

    // QMenu is a top level popup, so QWidget::isAncestorOf() stops at the menu
    // itself; the parent chain has to be walked by hand. Both panels are checked
    // instead of only the visible one: a popup that was opened from a panel must
    // never let that panel be pulled away, whatever the cursor does meanwhile.
    for (QWidget* widget = popup; widget; widget = widget->parentWidget())
    {
        if (widget == m_library || widget == m_loopPanel
            || widget == m_fullScreenPanelVisible)
        {
            return true;
        }
    }
    // A tool button with InstantPopup hands the click to QMenu::exec(), and the
    // menu is not the active popup widget until it is on screen. Both panels are
    // therefore asked directly as well: a dropdown that is already visible must
    // keep its panel alive whatever the popup bookkeeping says in that moment.
    const auto hasOpenMenu = [](QWidget* panel) {
        if (!panel)
            return false;
        const auto menus = panel->findChildren<QMenu*>(QString(),
                                                      Qt::FindDirectChildrenOnly);
        for (QMenu* menu : menus)
        {
            if (menu->isVisible())
                return true;
        }
        return false;
    };
    return hasOpenMenu(m_library) || hasOpenMenu(m_loopPanel);
}

void MainWindow::setFullScreenControlsVisible(bool visible)
{
    if (!m_playerBar)
        return;
    m_playerBar->setVisible(visible);
    if (visible && m_controlsFloating)
        m_playerBar->raise();
}

void MainWindow::applyFullScreenPanelHosting(bool fullScreen)
{
    if (!m_libraryOverlayHost || !m_loopOverlayHost || !m_librarySide || !m_loopSide)
        return;

    auto* sideLayout = qobject_cast<QHBoxLayout*>(m_librarySide->layout());
    auto* loopSideLayout = qobject_cast<QHBoxLayout*>(m_loopSide->layout());
    auto* libraryOverlayLayout = qobject_cast<QVBoxLayout*>(m_libraryOverlayHost->layout());
    auto* loopOverlayLayout = qobject_cast<QVBoxLayout*>(m_loopOverlayHost->layout());
    if (!sideLayout || !loopSideLayout || !libraryOverlayLayout || !loopOverlayLayout)
        return;

    if (fullScreen)
    {
        // Widths were captured by toggleFullScreen before the side panels were
        // hidden; the fold handles themselves stay behind (they belong to the
        // splitter).
        //
        // The controllers are told first: reparenting the panels resizes the
        // splitter, and those signals must not be mistaken for a user drag (that
        // is what used to fold a panel behind the user's back and leave the
        // chevron showing the wrong direction).
        if (m_libraryFold)
            m_libraryFold->setHosted(true);
        if (m_loopFold)
            m_loopFold->setHosted(true);
        sideLayout->removeWidget(m_library);
        libraryOverlayLayout->addWidget(m_library);
        loopSideLayout->removeWidget(m_loopPanel);
        loopOverlayLayout->addWidget(m_loopPanel);

        // Moving a widget into another layout hides it; the overlay has to bring
        // both panels back explicitly (a folded panel is shown here too - in full
        // screen this is the only way to reach the media library).
        m_library->show();
        m_loopPanel->show();

        hideFullScreenPanels();
        updateFullScreenPanelGeometry();
    }
    else
    {
        libraryOverlayLayout->removeWidget(m_library);
        sideLayout->addWidget(m_library, 1);
        loopOverlayLayout->removeWidget(m_loopPanel);
        loopSideLayout->addWidget(m_loopPanel, 1);

        // Same again: put the panels back into the state the windowed layout
        // wants (a folded panel stays folded).
        m_library->setVisible(!m_libraryCollapsed && m_libraryAction
                              && m_libraryAction->isChecked());
        m_loopPanel->setVisible(!m_loopCollapsed && m_loopListAction
                                && m_loopListAction->isChecked());

        hideFullScreenPanels();
        // Only now is the panel really back in the splitter, so the controller
        // may touch it again.
        if (m_libraryFold)
            m_libraryFold->setHosted(false);
        if (m_loopFold)
            m_loopFold->setHosted(false);
    }
}

void MainWindow::updateFullScreenPanelGeometry()
{
    if (!m_fullScreen || !m_libraryOverlayHost || !m_loopOverlayHost || !m_mediaColumn)
        return;

    // Left/right follow the "swap panels" setting, exactly like the splitter.
    QWidget* leftHost = m_swappedSides ? m_loopOverlayHost : m_libraryOverlayHost;
    QWidget* rightHost = m_swappedSides ? m_libraryOverlayHost : m_loopOverlayHost;
    const int leftWidth = m_swappedSides ? m_loopOverlayWidth : m_libraryOverlayWidth;
    const int rightWidth = m_swappedSides ? m_libraryOverlayWidth : m_loopOverlayWidth;

    // The hosts live in the player column, which in full screen is exactly the
    // screen - so the panels hug the real screen edges, at every height (the old
    // top level windows kept the windowed geometry, which is why the edge strip
    // only reacted over part of the screen).
    const int columnWidth = m_mediaColumn->width();
    const int columnHeight = std::max(1, m_mediaColumn->height());
    leftHost->setGeometry(0, 0, leftWidth, columnHeight);
    rightHost->setGeometry(std::max(0, columnWidth - rightWidth), 0, rightWidth, columnHeight);
    if (leftHost->isVisible())
        leftHost->raise();
    if (rightHost->isVisible())
        rightHost->raise();
}

void MainWindow::showFullScreenPanel(QWidget* host)
{
    if (!host)
        return;
    if (m_fullScreenPanelVisible != host)
    {
        if (m_libraryOverlayHost && m_libraryOverlayHost != host)
            m_libraryOverlayHost->hide();
        if (m_loopOverlayHost && m_loopOverlayHost != host)
            m_loopOverlayHost->hide();
        m_fullScreenPanelVisible = host;
        AppLogger::log(QStringLiteral("ui"),
                       QStringLiteral("全屏浮层: 浮出「%1」（光标 %2,%3）")
                           .arg(host == m_libraryOverlayHost ? LT("媒体库")
                                                            : LT("循环列表"))
                           .arg(QCursor::pos().x())
                           .arg(QCursor::pos().y()));
    }
    // Show and raise only on the switch: repeating it on every mouse move would
    // repaint the panel area for nothing.
    if (!host->isVisible())
    {
        host->show();
        host->raise();
    }
}

void MainWindow::hideFullScreenPanels()
{
    if (m_fullScreenPanelVisible)
    {
        // The reason matters when this is read back from a log: a panel that keeps
        // its dropdown open must not be reported as a plain "收回".
        AppLogger::log(QStringLiteral("ui"),
                       QStringLiteral("全屏浮层: 收回%1")
                           .arg(fullScreenPanelPopupOpen() ? QStringLiteral("（下拉菜单仍打开）")
                                                           : QString()));
    }
    m_fullScreenPanelVisible = nullptr;
    if (m_libraryOverlayHost)
        m_libraryOverlayHost->hide();
    if (m_loopOverlayHost)
        m_loopOverlayHost->hide();
}

void MainWindow::updateFullScreenPanelsForCursor()
{
    if (!m_fullScreen || !m_libraryOverlayHost || !m_loopOverlayHost || !m_mediaColumn)
        return;

    QWidget* leftHost = m_swappedSides ? m_loopOverlayHost : m_libraryOverlayHost;
    QWidget* rightHost = m_swappedSides ? m_libraryOverlayHost : m_loopOverlayHost;
    const QPoint cursor = mapFromGlobal(QCursor::pos());

    if (m_fullScreenPanelVisible)
    {
        // A dropdown opened from the panel (收藏夹 / 排序 / 标签 …) is a popup
        // window of its own, so the cursor over it counts as "outside the panel"
        // and the panel used to be pulled away - which closed the menu under the
        // user's hand and made those controls look dead in full screen.
        if (fullScreenPanelPopupOpen())
            return;
        // A held button means the user is scrolling or clicking inside it: never
        // pull the panel away under their hand.
        if (QApplication::mouseButtons() != Qt::NoButton)
            return;
        // Stay while the pointer is on the panel. The check runs on every mouse
        // move and a few times per second from the timer, so a Leave event that
        // never arrived cannot leave the panel stuck on the picture.
        const QPoint inColumn = m_mediaColumn->mapFromGlobal(QCursor::pos());
        if (m_fullScreenPanelVisible->geometry()
                .adjusted(-kFullScreenPanelKeepMargin, 0, kFullScreenPanelKeepMargin, 0)
                .contains(inColumn))
            return;
        hideFullScreenPanels();
        return;
    }

    if (!rect().contains(cursor))
        return;
    if (cursor.x() <= kFullScreenPanelEdgeWidth)
        showFullScreenPanel(leftHost);
    else if (cursor.x() >= width() - kFullScreenPanelEdgeWidth)
        showFullScreenPanel(rightHost);
}

void MainWindow::setFullScreenControlsFloating(bool floating)
{
    if (!m_playerBar || !m_videoFrame || !m_mediaColumn
        || m_controlsFloating == floating)
        return;
    m_controlsFloating = floating;

    auto* mediaLayout = qobject_cast<QVBoxLayout*>(m_mediaColumn->layout());
    const bool wasVisible = m_playerBar->isVisible();

    if (floating)
    {
        // Out of the column layout, but deliberately *not* reparented: moving the
        // bar into the video frame re-polishes the stylesheet of the whole bar and
        // every child it owns, and that main-thread work is exactly what makes the
        // device hitch during a full screen switch. As a sibling of the video
        // frame it just gets raised over the picture instead.
        if (mediaLayout)
            mediaLayout->removeWidget(m_playerBar);
        positionFloatingPlayerBar();
    }
    else
    {
        // Back under the video, exactly where the windowed layout wants it.
        if (mediaLayout)
            mediaLayout->addWidget(m_playerBar);
    }

    // Reparenting hides the widget; restore what it was.
    m_playerBar->setVisible(wasVisible);
    if (m_controlsFloating)
        m_playerBar->raise();
}

void MainWindow::positionFloatingPlayerBar()
{
    if (!m_playerBar || !m_videoFrame)
        return;

    const int height = m_playerBar->sizeHint().height();
    const int width = std::max(1, m_videoFrame->width() - 2 * kFloatingPlayerBarMargin);
    // The bar is a sibling of the video frame inside the media column, so the
    // picture's rect has to be mapped into that parent's coordinates.
    const QPoint videoOrigin = m_videoFrame->pos();
    m_playerBar->setGeometry(videoOrigin.x() + kFloatingPlayerBarMargin,
                             videoOrigin.y() + std::max(0, m_videoFrame->height() - height
                                                            - kFloatingPlayerBarMargin),
                             width, height);
    m_playerBar->raise();
}

void MainWindow::syncFullScreenControlsWithCursor()
{
    if (!m_fullScreen)
        return;
    if (fullScreenControlZoneContainsCursor())
    {
        setFullScreenControlsVisible(true);
        return;
    }
    // A held button means a drag is in flight: never pull the bar out from
    // under the cursor, no matter where the cursor has wandered to.
    if (QApplication::mouseButtons() == Qt::NoButton)
        setFullScreenControlsVisible(false);
}

void MainWindow::hideIdleFullScreenOverlays()
{
    if (!m_fullScreen)
        return;
    // A held button means a drag is in flight; wait until it is over instead
    // of hiding the bar (and the pointer) under the user's hand.
    if (QApplication::mouseButtons() != Qt::NoButton)
    {
        if (m_cursorIdleTimer)
            m_cursorIdleTimer->start();
        return;
    }
    AppLogger::log(QStringLiteral("ui"), QStringLiteral("全屏空闲，隐藏控制条与鼠标指针"));
    setFullScreenControlsVisible(false);
    setFullScreenCursorHidden(true);
}

void MainWindow::setFullScreenCursorHidden(bool hidden)
{
    if (m_cursorHidden == hidden)
        return;
    m_cursorHidden = hidden;

    // Every original cursor is collected *before* any of them is touched: a
    // child that never set its own cursor reports the one it inherits, so
    // blanking a parent first would poison the backup with the blank cursor
    // itself and the pointer would never come back.
    if (hidden)
    {
        m_cursorBackup.clear();
        for (QWidget* source : m_cursorSources)
        {
            if (source)
                m_cursorBackup.insert(source, source->cursor());
        }
        for (QWidget* source : m_cursorSources)
        {
            if (source)
                source->setCursor(Qt::BlankCursor);
        }
        return;
    }

    for (QWidget* source : m_cursorSources)
    {
        if (!source)
            continue;
        if (const auto it = m_cursorBackup.constFind(source);
            it != m_cursorBackup.constEnd())
            source->setCursor(it.value());
    }
    m_cursorBackup.clear();
}

void MainWindow::showAbout()
{
    QMessageBox::about(
        this, LT("关于 NekoBeat"),
        LT("<b>NekoBeat %1</b><br>多轴脚本律动播放器<br><br>“设置 › 轴限制”可保存多套全局行程比例配置。")
            .arg(QApplication::applicationVersion()));
}

void MainWindow::setInterfaceLanguage(Loc::Language language)
{
    if (language == Loc::language())
        return;

    // Save first, ask second: even "later" has to keep the choice, otherwise the
    // next start would silently fall back to English.
    Loc::setLanguage(language);
    Loc::saveToSettings();

    // The dialog is written in the language that was just picked, so the person
    // who cannot read the current one still understands what happens next.
    QMessageBox box(this);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(LT("界面语言"));
    box.setText(LT("界面语言已切换为 %1。\n重新启动后所有界面文字都会换成新语言。")
                    .arg(Loc::language() == Loc::Language::Chinese
                             ? QStringLiteral("简体中文")
                             : QStringLiteral("English")));
    QPushButton* restart = box.addButton(LT("立即重启"), QMessageBox::AcceptRole);
    box.addButton(LT("稍后"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == restart)
        restartApplication();
}

void MainWindow::restartApplication()
{
    // Start the replacement first, then close this instance through the normal
    // close path - closeEvent() sends DSTOP, stops the scheduler and shuts the
    // device thread down, and the new process would otherwise find the port busy.
    const QString executable = QCoreApplication::applicationFilePath();
    QStringList arguments = QCoreApplication::arguments();
    if (!arguments.isEmpty())
        arguments.removeFirst();
    QProcess::startDetached(executable, arguments, QCoreApplication::applicationDirPath());
    close();
}

void MainWindow::wireSignals()
{
    connect(m_library, &MediaLibrary::mediaActivated, this, &MainWindow::onMediaActivated);
    connect(m_library, &MediaLibrary::message, this,
            [this](const QString& text, int timeoutMs) {
                showStatusMessage(text, timeoutMs);
            });
    connect(m_library, &MediaLibrary::manageFavoritesRequested, this,
            &MainWindow::openFavoritesDialog);
    connect(m_library, &MediaLibrary::relinkRequested, this,
            [this](const QString& mediaPath) { openRelinkDialog(mediaPath); });
    connect(m_library, &MediaLibrary::cleanupRequested, this,
            [this](const QString& mediaPath) {
                if (!m_library || !m_loopStore)
                    return;
                if (m_library->hasUnreachableMedia())
                {
                    MediaCleanupPrompt::warnUnreachable(this,
                                                        m_library->unreachableRoots());
                    return;
                }

                const QSet<QString> stale = {
                    PathUtils::normalizeMediaPath(mediaPath)
                };
                if (!MediaCleanupPrompt::run(this, m_library->favoritesStore(),
                                             m_loopStore, stale))
                {
                    return;
                }

                m_library->refreshFavorites();
                m_library->refreshAll();
                if (m_loopPanel)
                    m_loopPanel->refresh();
                refreshLoopPanelContext();
            });
    // The media library has its own 连播 button; keep it and the menu action in
    // sync so either entry point controls the same setting.
    connect(m_library, &MediaLibrary::autoNextChanged, this, [this](bool enabled) {
        setAutoNextEnabled(enabled);
    });

    // ---- loop list -----------------------------------------------------
    if (m_loopPanel)
    {
        if (m_library)
        {
            m_loopPanel->setCacheRoot(m_library->cacheRoot());
            m_loopPanel->setStore(m_loopStore);
            // Both panels share the media library's card size and font size.
            m_loopPanel->setCardWidth(m_library->thumbSize());
            m_loopPanel->setFontPercent(m_library->fontPercent());
            m_loopPanel->setAxisTagVisible(m_axisTagVisible);
        }
        connect(m_loopPanel, &LoopListPanel::clipActivated, this,
                &MainWindow::onLoopClipActivated);
        connect(m_loopPanel, &LoopListPanel::message, this,
                [this](const QString& text, int timeoutMs) {
                    showStatusMessage(text, timeoutMs);
                });
        connect(m_loopPanel, &LoopListPanel::manageRequested, this,
                &MainWindow::openLoopManagerDialog);
        connect(m_loopPanel, &LoopListPanel::storeChanged, this,
                [this]() {
                    refreshLoopPanelContext();
                    // The clip that is looping right now may have just been
                    // deleted (or had its media removed) from the panel.
                    if (!m_activeLoopClipId.isEmpty()
                        && m_loopStore && !m_loopStore->containsClip(m_activeLoopClipId))
                    {
                        stopLoopPlayback(LT("循环片段已被删除"));
                    }
                });
        connect(m_loopPanel, &LoopListPanel::autoNextChanged, this, [this](bool enabled) {
            if (!m_loopAutoNextAction)
                return;
            m_loopAutoNextAction->blockSignals(true);
            m_loopAutoNextAction->setChecked(enabled);
            m_loopAutoNextAction->blockSignals(false);
        });
    }
    // The library scans and generates covers in the background; coalesce those
    // updates instead of rebuilding the loop grid on every single result.
    m_loopContextTimer = new QTimer(this);
    m_loopContextTimer->setSingleShot(true);
    m_loopContextTimer->setInterval(900);
    connect(m_loopContextTimer, &QTimer::timeout, this,
            &MainWindow::refreshLoopPanelContext);
    if (m_library)
    {
        connect(m_library, &MediaLibrary::contentChanged, this,
                [this]() { m_loopContextTimer->start(); });
        connect(m_library, &MediaLibrary::settingsChanged, this, [this]() {
            if (!m_loopPanel)
                return;
            m_loopPanel->setCardWidth(m_library->thumbSize());
            m_loopPanel->setFontPercent(m_library->fontPercent());
        });
    }
    connect(m_loopAButton, &QToolButton::clicked, this, &MainWindow::markLoopPointA);
    connect(m_loopBButton, &QToolButton::clicked, this, &MainWindow::markLoopPointB);
    connect(m_loopAddButton, &QToolButton::clicked, this, &MainWindow::addLoopClip);
    connect(m_loopClearButton, &QToolButton::clicked, this, &MainWindow::clearLoopMarkers);
    connect(m_loopAButton, &QToolButton::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                QMenu menu(this);
                QAction* clearAction = menu.addAction(LT("清除 A/B 标记"));
                connect(clearAction, &QAction::triggered, this,
                        &MainWindow::clearLoopMarkers);
                menu.exec(m_loopAButton->mapToGlobal(pos));
            });
    connect(m_loopBButton, &QToolButton::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                QMenu menu(this);
                QAction* clearAction = menu.addAction(LT("清除 B 标记"));
                connect(clearAction, &QAction::triggered, this, [this]() {
                    m_loopBMs = -1;
                    updateLoopMarkerUi();
                    showStatusMessage(LT("已清除 B 标记"), 2000);
                });
                QAction* clearAll = menu.addAction(LT("清除全部标记"));
                connect(clearAll, &QAction::triggered, this,
                        &MainWindow::clearLoopMarkers);
                menu.exec(m_loopBButton->mapToGlobal(pos));
            });

    // Marking shortcuts; they stay out of the way while a text field has focus.
    const auto textInputFocused = []() {
        QWidget* focus = QApplication::focusWidget();
        if (!focus)
            return false;
        const QString className = QString::fromLatin1(focus->metaObject()->className());
        return className.contains(QStringLiteral("QLineEdit"))
               || className.contains(QStringLiteral("QTextEdit"))
               || className.contains(QStringLiteral("QPlainTextEdit"))
               || className.contains(QStringLiteral("QAbstractSpinBox"))
               || className.contains(QStringLiteral("QComboBox"));
    };
    const auto addLoopShortcut = [this, textInputFocused](const QKeySequence& sequence,
                                                          void (MainWindow::*method)()) {
        auto* action = new QAction(this);
        action->setShortcut(sequence);
        action->setShortcutContext(Qt::WindowShortcut);
        addAction(action);
        connect(action, &QAction::triggered, this, [this, textInputFocused, method]() {
            if (!textInputFocused())
                (this->*method)();
        });
    };
    addLoopShortcut(QKeySequence(QStringLiteral("[")), &MainWindow::markLoopPointA);
    addLoopShortcut(QKeySequence(QStringLiteral("]")), &MainWindow::markLoopPointB);
    addLoopShortcut(QKeySequence(QStringLiteral("Ctrl+L")), &MainWindow::addLoopClip);

    connect(m_playButton, &QToolButton::clicked, this, &MainWindow::togglePlayback);
    connect(m_stopButton, &QToolButton::clicked, this, &MainWindow::stopPlayback);
    connect(m_panicButton, &QToolButton::clicked, this, &MainWindow::panicStop);
    connect(m_resetButton, &QToolButton::clicked, this, &MainWindow::resetAxes);
    connect(m_heatmap, &HeatmapWidget::seekRequested, this, [this](qint64 positionMs) {
        // Dragging out of the marked range is the user's way of leaving a loop.
        if (!m_activeLoopClipId.isEmpty()
            && LoopClipRules::isOutsideRange(positionMs, m_activeLoopStartMs,
                                             m_activeLoopEndMs))
        {
            stopLoopPlayback(LT("已离开循环区间，退出循环播放"));
        }
        requestSeek(positionMs);
    });
    connect(m_heatmap, &HeatmapWidget::scrubStarted, this, &MainWindow::beginScrub);
    connect(m_heatmap, &HeatmapWidget::scrubMoved, this, &MainWindow::holdMachineForScrub);
    connect(m_heatmap, &HeatmapWidget::scrubFinished, this, &MainWindow::finishScrub);
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        m_volume = value;
        m_engine->setVolume(value);
        if (m_volumeLabel)
            m_volumeLabel->setText(QStringLiteral("%1%").arg(value));
        if (m_volumeButton)
        {
            m_volumeButton->setIcon(Theme::volumeIcon(
                value > 0 ? QColor(0xF2, 0xF2, 0xF7) : QColor(0x98, 0x98, 0x9D), 16));
        }
    });
    connect(m_volumeButton, &QToolButton::clicked, this, [this]() {
        if (m_volume > 0)
        {
            m_volumeBeforeMute = m_volume;
            m_volumeSlider->setValue(0);
        }
        else
        {
            m_volumeSlider->setValue(m_volumeBeforeMute > 0 ? m_volumeBeforeMute : 100);
        }
    });

    connect(m_engine, &MpvEngine::positionChanged, this, &MainWindow::onPositionChanged);
    connect(m_video, &MpvVideoWidget::doubleClicked, this, [this] {
        if (m_fullScreenAction)
            m_fullScreenAction->setChecked(!m_fullScreen);
    });
    // Right click on the video toggles pause/resume, also while full screen
    // (the video widget stays the same child of the window there).
    connect(m_video, &MpvVideoWidget::rightClicked, this, [this] {
        if (m_mediaPath.isEmpty())
            return;
        togglePlayback();
    });
    // mpv can only keep a video output while a render context exists. When Qt
    // rebuilds the GL context (a full screen transition on some drivers, moving
    // the window to another screen, a display mode change) mpv does not always
    // recover on its own; without this the picture would stay frozen for the
    // rest of the session while the audio keeps running. Re-open the current
    // file so the video output is configured again.
    connect(m_video, &MpvVideoWidget::renderContextReady, this, [this] {
        if (m_mediaPath.isEmpty() || !m_engine || m_engine->hasVideoOutput())
            return;
        AppLogger::log(QStringLiteral("ui"),
                       QStringLiteral("渲染上下文重建后 mpv 没有视频输出，重新打开当前视频: %1")
                           .arg(m_mediaPath));
        loadMedia(m_mediaPath, m_mediaScript, m_engine->isPlaying());
    });
    connect(m_engine, &MpvEngine::durationChanged, this, &MainWindow::onDurationChanged);
    connect(m_engine, &MpvEngine::playingChanged, this, [this](bool playing) {
        onPlayingChanged(playing);
        // While the heat map is being scrubbed the machine is intentionally held
        // still; the release handler decides when it picks the script up again.
        if (playing && !m_scrubbing)
            m_link->start();
    });
    connect(m_engine, &MpvEngine::mediaEnded, this, [this] {
        // A loop clip keeps running even when the playhead reaches the end of
        // the file: jump back to A instead of stopping or auto-advancing.
        if (!m_activeLoopClipId.isEmpty() && m_activeLoopStartMs >= 0)
        {
            // The end of the file is also the end of the clip, so sequential
            // playback has to be honoured here too - otherwise a clip that runs
            // to the end of the video would fall through to the remaining
            // timeline instead of handing over to the next clip.
            if (advanceToNextLoopClip())
                return;

            m_loopRewinding = true;
            m_loopRewindTicks = 0;
            // The rewind must not race a scrub target that is still pending.
            cancelPendingSeek();
            m_engine->seek(m_activeLoopStartMs);
            m_link->seek(m_activeLoopStartMs);
            m_engine->play();
            m_link->start(true);
            return;
        }
        m_link->stop(true);
        if (!m_autoNext || !m_library || m_mediaPath.isEmpty())
            return;

        const MediaPlaybackRequest next = m_library->nextMedia(m_mediaPath);
        if (next.mediaPath.isEmpty())
        {
            showStatusMessage(LT("已播放到媒体库末尾"), 5000);
            return;
        }

        AppLogger::log(QStringLiteral("ui"),
                       QStringLiteral("自动连播: %1").arg(next.mediaPath));
        QTimer::singleShot(250, this, [this, next]() {
            loadMedia(next.mediaPath, next.scriptPath);
        });
    });
    connect(m_link, &DeviceLink::axisPositionChanged, this, &MainWindow::onAxisPositionChanged);
    connect(m_link, &DeviceLink::capabilitiesChanged, this, &MainWindow::onDeviceCapabilitiesChanged);
    connect(m_link, &DeviceLink::errorOccurred, this, [this](const QString& message) {
        AppLogger::log(QStringLiteral("serial"), QStringLiteral("错误: %1").arg(message));
        showStatusMessage(message, 8000);
        // A modal dialog grabs the mouse and keeps the shared GUI thread busy
        // while the device may still be moving. Only interrupt with a dialog
        // when nothing is playing; otherwise the status line is enough.
        // While the connection dialog is retrying the user already sees the port
        // state there, and a modal box would stop the retries.
        if (!m_engine->isPlaying() && !m_connectingSerial)
            QMessageBox::warning(this, LT("串口错误"), message);
    });
    connect(m_link, &DeviceLink::connectionChanged, this, [this](bool connected, const QString& message) {
        AppLogger::log(QStringLiteral("serial"),
                       QStringLiteral("连接状态: %1 | %2")
                           .arg(connected ? QStringLiteral("已连接") : QStringLiteral("已断开"), message));
        updateDeviceIndicator(connected);

        if (connected)
        {
            m_connectingSerial = false;
            m_connectRetriesLeft = 0;
            // The port is open but D0/D1/D2 have not answered yet, so the axis
            // list is unknown. Hold the link until the probe says what this
            // device actually supports instead of driving a guessed axis set.
            m_link->setDeviceAxesConfirmed(false);
            showStatusMessage(LT("正在识别设备支持的轴，识别完成前不会联动"), 5000);
        }

        // A failed open is retried a couple of times before bothering the user:
        // a port that was freed a moment ago usually opens again right away. The
        // retry is scheduled on the GUI thread but the open itself happens in the
        // worker, so nothing here waits for the driver.
        if (!connected && m_connectingSerial && m_connectRetriesLeft > 0)
        {
            const QString port = m_connectingPort;
            const int left = --m_connectRetriesLeft;
            // Bluetooth gets a longer pause: the radio link has to come up
            // first, so a retry 250 ms later would fire before the device has
            // had a chance to answer.
            const int retryDelayMs = m_lastPortIsBluetooth ? 1200 : 250;
            AppLogger::log(QStringLiteral("serial"),
                           QStringLiteral("打开 %1 失败，%2 ms 后重试（剩余 %3 次）")
                               .arg(port)
                               .arg(retryDelayMs)
                               .arg(left));
            QTimer::singleShot(retryDelayMs, this,
                               [this, port, left] { openSerialPort(port, left); });
            return;
        }
        if (!connected && m_connectingSerial)
        {
            m_connectingSerial = false;
            AppLogger::log(QStringLiteral("serial"), QStringLiteral("连接结果: 失败"));
            showStatusMessage(LT("无法连接 %1").arg(m_connectingPort), 8000);
            const QString advice =
                m_lastPortIsBluetooth
                    ? LT("这是一个蓝牙串口。请确认：\n· 设备已开机，并且在电脑附近；\n· Windows“蓝牙和其他设备”里设备显示为已连接（未连接就点一下它重新连）；\n· 没有其它程序（串口助手、另一个播放器）占着这个蓝牙串口——蓝牙串口同一时间只能被一个程序打开。\n\n如果刚给设备上电，等待两秒再点一次连接通常就好了。")
                    : LT("如果这个端口显示“被占用”，请关闭占用它的程序（串口助手、Arduino IDE、另一个播放器）后重试；如果显示“驱动异常”，请在设备管理器里重新安装 USB 串口驱动。");
            QMessageBox::warning(
                this, LT("无法连接 SR6"),
                LT("无法打开 %1。\n\n%2").arg(m_connectingPort, advice));
        }

        // The adapter was unplugged or the port failed: stop driving it at
        // once. The scheduler would otherwise keep ticking against a port that
        // silently accepts nothing.
        if (!connected && m_link && m_link->isRunning())
        {
            m_link->pause();
            showStatusMessage(LT("设备已断开，播放联动已停止"), 8000);
        }
    });
    connect(m_link, &DeviceLink::transportStalled, this, [this] {
        AppLogger::log(QStringLiteral("sync"), QStringLiteral("串口拥堵，联动已停止"));
        showStatusMessage(LT("串口持续拥堵，播放联动已停止"), 8000);
        updateResetAvailability();
    });
    // The scheduler now runs in the worker thread, so "running" changes arrive
    // asynchronously; the reset button tracks them.
    connect(m_link, &DeviceLink::runningChanged, this,
            [this](bool) { updateResetAvailability(); });
    connect(m_link, &DeviceLink::linkStats, this, [this](int windowMs, int commands,
                                                        int maxTickGapMs, int maxPhaseLagMs) {
        if (m_statusLink)
        {
            m_statusLink->setText(LT("同步 ±%1 ms").arg(maxPhaseLagMs));
            m_statusLink->setToolTip(
                LT("最近 %1 秒：%2 条指令，最大 tick 间隔 %3 ms，最大相位滞后 %4 ms")
                    .arg(windowMs / 1000.0, 0, 'f', 1)
                    .arg(commands)
                    .arg(maxTickGapMs)
                    .arg(maxPhaseLagMs));
            if (!m_linkStatsSeen)
            {
                m_linkStatsSeen = true;
                updateStatusChips();
            }
        }
    });

    connect(m_axisPanel, &AxisLimitPanel::axisConfigEdited, this, &MainWindow::onAxisConfigEdited);
    connect(m_axisPanel, &AxisLimitPanel::previewRequested, this, &MainWindow::onAxisPreviewRequested);
    connect(m_axisPanel, &AxisLimitPanel::profileApplied, this, &MainWindow::onProfileApplied);
    connect(m_axisPanel, &AxisLimitPanel::homeRequested, this, &MainWindow::resetAxes);

    auto* playShortcut = new QAction(this);
    playShortcut->setShortcut(Qt::Key_Space);
    connect(playShortcut, &QAction::triggered, this, &MainWindow::togglePlayback);
    addAction(playShortcut);

    // Emergency stop must work no matter which widget has focus, including
    // full screen where the control bar is hidden by default.
    auto* panicShortcut = new QAction(this);
    panicShortcut->setShortcut(QKeySequence(QStringLiteral("Ctrl+Esc")));
    panicShortcut->setShortcutContext(Qt::WindowShortcut);
    connect(panicShortcut, &QAction::triggered, this, &MainWindow::panicStop);
    addAction(panicShortcut);

    m_escapeAction = new QAction(this);
    m_escapeAction->setShortcut(Qt::Key_Escape);
    connect(m_escapeAction, &QAction::triggered, this, [this] {
        if (m_fullScreen && m_fullScreenAction)
            m_fullScreenAction->setChecked(false);
    });
    addAction(m_escapeAction);

    updateResetAvailability();
    updateDeviceIndicator(false);
    setAcceptDrops(true);
}

void MainWindow::updateResetAvailability()
{
    const bool playing = m_engine && m_engine->isPlaying();
    if (m_resetButton)
    {
        m_resetButton->setEnabled(!playing);
        m_resetButton->setToolTip(playing ? LT("播放联动中不可复位")
                                          : LT("复位（回到中位）"));
    }
    if (m_axisPanel)
        m_axisPanel->setResetEnabled(!playing);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty())
        return;

    const QFileInfo info(urls.first().toLocalFile());
    if (info.isDir())
        m_library->addRootFolder(info.absoluteFilePath());
    else if (info.isFile())
        loadMedia(info.absoluteFilePath());

    event->acceptProposedAction();
}

void MainWindow::openVideo()
{
    const QString file = QFileDialog::getOpenFileName(
        this, LT("打开视频"), m_lastMediaFolder, LT("视频文件 (*.*)"));
    if (!file.isEmpty())
    {
        AppLogger::log(QStringLiteral("ui"), QStringLiteral("选择视频: %1").arg(file));
        m_lastMediaFolder = QFileInfo(file).absolutePath();
        loadMedia(file);
    }
}

void MainWindow::openFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(
        this, LT("打开媒体文件夹"), m_lastMediaFolder);
    if (!folder.isEmpty())
    {
        AppLogger::log(QStringLiteral("ui"), QStringLiteral("选择媒体文件夹: %1").arg(folder));
        m_lastMediaFolder = folder;
        m_library->addRootFolder(folder);
    }
}

void MainWindow::connectDevice()
{
    if (m_link && m_link->isConnected())
        return;

    // The dialog lists every port together with its real state (available /
    // occupied / driver problem), which is what makes "the port is right there
    // but the app cannot find it" answerable.
    //
    // No remembered port and no preselection: the last used port used to be
    // selected automatically, so a user who had switched to Bluetooth once would
    // get Bluetooth again the next time even while sitting on the USB cable. The
    // connection is always an explicit, visible choice.
    SerialPortDialog dialog(QString(), this);
    connect(&dialog, &SerialPortDialog::releaseRequested, this, [this, &dialog] {
        releaseSerialPort();
        dialog.refresh();
    });
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString port = dialog.selectedPort();
    if (port.isEmpty())
        return;

    AppLogger::log(QStringLiteral("serial"), QStringLiteral("用户选择串口: %1").arg(port));
    // A Bluetooth serial device is reachable only through the port Windows
    // created when it was paired, and opening that port has to bring the radio
    // link up first - which takes seconds when the device just woke up.
    // -1 lets openSerialPort() pick the budget once it knows which link it got.
    openSerialPort(port, -1);
}

void MainWindow::releaseSerialPort()
{
    // Close whatever this program still holds. The UI may already believe it is
    // disconnected while the handle is still open - that is exactly the state
    // that makes the port look "mysteriously occupied" to the next attempt.
    if (m_link)
        m_link->pause();
    if (m_link)
        m_link->disconnectDevice();
    AppLogger::log(QStringLiteral("serial"),
                   QStringLiteral("强制释放：已关闭本程序持有的串口句柄"));
}

void MainWindow::openSerialPort(const QString& port, int retriesLeft)
{
    // The port is opened in the worker thread, so this returns immediately and
    // the outcome arrives through connectionChanged(). Retries are driven from
    // there as well - nothing on the GUI thread ever waits for the driver.
    //
    // The link type is decided here, on the port that is actually about to be
    // opened, and not by the caller: over Bluetooth and over USB the device
    // answers identically, so a label that came from anywhere else could end up
    // saying "蓝牙" about a USB cable (or the reverse) and send the user looking
    // in the wrong place.
    SerialPortEntry bluetooth;
    m_lastPortIsBluetooth =
        SerialPortScanner::bluetoothInfo(port, &bluetooth) && bluetooth.bluetoothOutbound;
    m_lastPortBluetoothName = m_lastPortIsBluetooth ? bluetooth.bluetoothName : QString();
    if (m_lastPortIsBluetooth)
    {
        AppLogger::log(QStringLiteral("serial"),
                       QStringLiteral("按蓝牙串口连接：%1（%2）")
                           .arg(port, m_lastPortBluetoothName.isEmpty()
                                          ? QStringLiteral("设备名未知")
                                          : m_lastPortBluetoothName));
    }
    m_connectingSerial = true;
    m_connectingPort = port;
    // Bluetooth gets one extra attempt: the first open has to bring the radio
    // link up, and a device that is still waking up drops the first one.
    m_connectRetriesLeft = retriesLeft >= 0 ? retriesLeft
                                            : (m_lastPortIsBluetooth ? 3 : 2);
    AppLogger::log(QStringLiteral("serial"),
                   QStringLiteral("请求连接 %1（可用重试 %2 次）")
                       .arg(port)
                       .arg(m_connectRetriesLeft));
    m_link->connectDevice(port);
}

void MainWindow::toggleDeviceConnection()
{
    if (m_link->isConnected())
        disconnectDevice();
    else
        connectDevice();
}

void MainWindow::disconnectDevice()
{
    if (!m_link)
        return;
    AppLogger::log(QStringLiteral("ui"), QStringLiteral("用户断开 SR6"));
    m_link->pause();
    m_link->disconnectDevice();
    m_statusDevice->setText(LT("设备未连接"));
    updateDeviceIndicator(false);
}

void MainWindow::togglePlayback()
{
    AppLogger::log(QStringLiteral("ui"),
                   QStringLiteral("切换播放，当前 isPlaying=%1")
                       .arg(m_engine->isPlaying() ? QStringLiteral("true") : QStringLiteral("false")));
    if (m_engine->isPlaying())
    {
        m_engine->pause();
        m_link->pauseEased();
    }
    else
    {
        m_engine->play();
        m_link->start();
    }
}

void MainWindow::stopPlayback()
{
    AppLogger::log(QStringLiteral("ui"), QStringLiteral("终止当前视频"));
    stopLoopPlayback();
    m_pendingLoopSeekMs = -1;
    cancelPendingSeek();
    if (m_loopAMs >= 0 || m_loopBMs >= 0)
    {
        m_loopAMs = -1;
        m_loopBMs = -1;
        updateLoopMarkerUi();
    }
    m_link->stop(false);
    m_engine->stopPlayback();
    m_bundle = {};
    m_mediaPath.clear();
    m_link->setBundle({});
    clearAxisPositions();

    m_statusTime->setText(QStringLiteral("00:00 / 00:00"));
    m_statusScript->setText(LT("脚本未加载"));
    m_playButton->setIcon(Theme::playIcon(Qt::white, 16));
    m_videoStack->setCurrentWidget(m_blackScreen);
    if (m_library)
        m_library->setPlayingMedia(QString(), QString());
    if (m_heatmap)
        m_heatmap->clear();
    showStatusMessage(LT("已终止当前视频"), 3000);
}

void MainWindow::panicStop()
{
    AppLogger::log(QStringLiteral("ui"), QStringLiteral("急停：发送 DSTOP 并暂停"));

    // panic() always emits DSTOP, even when the scheduler is not running, so a
    // device that is still moving for any reason is stopped right away.
    if (m_link)
        m_link->panic();
    // A scrub that is still in progress must not move the machine when the
    // pointer is finally released - 急停 wins over a pending drag target.
    cancelPendingSeek();
    if (m_engine && m_engine->isPlaying())
        m_engine->pause();

    updateResetAvailability();
    showStatusMessage(LT("已急停：设备已停止，视频已暂停（再按播放即可恢复）"),
                      6000);
}

void MainWindow::resetAxes()
{
    if (m_engine && m_engine->isPlaying())
    {
        showStatusMessage(LT("播放联动中不可复位，请先暂停或终止"), 3000);
        return;
    }
    if (!m_link || !m_link->isConnected())
    {
        showStatusMessage(LT("请先连接设备，再执行复位"), 3000);
        return;
    }
    // Reset drives every enabled axis, so it must wait for the probe as well.
    if (!m_link->capabilities().probeComplete)
    {
        showStatusMessage(LT("正在识别设备支持的轴，请稍候再试"), 3000);
        return;
    }

    QMap<int, AxisConfig> configs;
    if (m_axisPanel)
        configs = m_axisPanel->activeConfigs();

    const DeviceCapabilities capabilities = m_link->capabilities();
    QList<AxisTarget> targets;
    for (auto it = configs.constBegin(); it != configs.constEnd(); ++it)
    {
        if (capabilities.probeComplete && !capabilities.supportedTracks.contains(it.key()))
            continue;
        if (m_bundle.tracks.contains(it.key()) && !m_bundle.tracks.value(it.key()).config.enabled)
            continue;
        // Same rule as the end-of-video home: position axes go to the
        // configured home, amplitude channels are switched off.
        const int percent =
            TrackInfo::kind(static_cast<Track>(it.key())) == TrackInfo::AxisKind::Amplitude
                ? 0
                : TCodeEncoder::percentForHome(it.value());
        targets.append({static_cast<Track>(it.key()), percent, 1200});
    }
    if (targets.isEmpty())
        return;

    const QString line = TCodeEncoder::encodeLine(targets, configs);
    if (line.isEmpty())
        return;
    m_link->sendRawLine(line);
    AppLogger::log(QStringLiteral("tcode"), QStringLiteral("复位: %1").arg(line));
    showStatusMessage(LT("已发送复位指令"), 3000);
}

void MainWindow::setPlayPauseEasingEnabled(bool enabled, bool persist)
{
    if (m_link)
        m_link->setPlayPauseEasingEnabled(enabled);

    if (persist)
    {
        QSettings settings;
        settings.setValue(QStringLiteral("sync/playPauseEasing"), enabled);
    }
}

void MainWindow::configureSeekSafety()
{
    QSettings settings;
    const int current = settings.value(QStringLiteral("sync/seekRampMsPerPercent"),
                                       m_link->seekRampMsPerPercent()).toInt();
    const bool easingEnabled = settings.value(QStringLiteral("sync/seekSafetyEnabled"),
                                              m_link->seekSafetyEnabled()).toBool();
    const bool playPauseEasing =
        settings.value(QStringLiteral("sync/playPauseEasing"),
                       m_link->playPauseEasingEnabled())
            .toBool();
    SeekSafetyDialog dialog(current, easingEnabled, playPauseEasing, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const int value = dialog.value();
    settings.setValue(QStringLiteral("sync/seekRampMsPerPercent"), value);
    m_link->setSeekRampMsPerPercent(value);
    settings.setValue(QStringLiteral("sync/seekSafetyEnabled"), dialog.enabled());
    m_link->setSeekSafetyEnabled(dialog.enabled());
    setPlayPauseEasingEnabled(dialog.playPauseEasing());
    AppLogger::log(QStringLiteral("ui"),
                   QStringLiteral("跳转安全：%1，速度 %2 ms/1%，播放/暂停缓动：%3")
                       .arg(dialog.enabled() ? LT("启用")
                                             : LT("关闭"))
                       .arg(value)
                       .arg(dialog.playPauseEasing() ? LT("启用")
                                                     : LT("关闭")));
    showStatusMessage(dialog.enabled() || dialog.playPauseEasing()
                          ? LT("缓动设置已更新")
                          : LT("缓动已全部关闭"),
                      3000);
}

void MainWindow::updateLoopOverlay()
{
    if (!m_heatmap)
        return;
    if (m_loopAMs >= 0 || m_loopBMs >= 0)
        m_heatmap->setLoopMarkers(m_loopAMs, m_loopBMs);
    else
        m_heatmap->clearLoopMarkers();
}

void MainWindow::updateLoopButtonsEnabled()
{
    const bool hasMedia = !m_mediaPath.isEmpty();
    if (m_loopAButton)
        m_loopAButton->setEnabled(hasMedia);
    if (m_loopBButton)
        m_loopBButton->setEnabled(hasMedia && m_loopAMs >= 0);
    if (m_loopAddButton)
        m_loopAddButton->setEnabled(hasMedia && m_loopAMs >= 0 && m_loopBMs > m_loopAMs);
    if (m_loopClearButton)
        m_loopClearButton->setEnabled(m_loopAMs >= 0 || m_loopBMs >= 0);
}

void MainWindow::updateLoopMarkerUi()
{
    const auto applyMarker = [](QToolButton* button, const QString& prefix, qint64 ms) {
        if (!button)
            return;
        const bool marked = ms >= 0;
        const QString shortTime =
            marked ? DurationFormat::clock(ms) : QString();
        button->setText(marked ? QStringLiteral("%1 %2").arg(prefix, shortTime) : prefix);
        if (marked)
        {
            const int width =
                std::max(32, button->fontMetrics().horizontalAdvance(button->text()) + 18);
            button->setFixedSize(width, 32);
        }
        else
        {
            button->setFixedSize(32, 32);
        }
        button->setObjectName(marked ? QStringLiteral("MediaButtonActive")
                                     : QStringLiteral("MediaButton"));
        // Object names feed the style sheet, so the widget has to be re-polished.
        button->style()->unpolish(button);
        button->style()->polish(button);
    };

    applyMarker(m_loopAButton, QStringLiteral("A"), m_loopAMs);
    applyMarker(m_loopBButton, QStringLiteral("B"), m_loopBMs);

    if (m_loopAButton)
    {
        m_loopAButton->setToolTip(
            m_loopAMs >= 0
                ? LT("循环起点 A：%1\n点击重新标记，右键清除标记")
                      .arg(LoopClipRules::formatTimecode(m_loopAMs))
                : LT("标记循环起点 A（[）\n点击时自动暂停"));
    }
    if (m_loopBButton)
    {
        m_loopBButton->setToolTip(
            m_loopBMs >= 0
                ? LT("循环终点 B：%1\n点击重新标记，右键清除标记")
                      .arg(LoopClipRules::formatTimecode(m_loopBMs))
                : LT("标记循环终点 B（]）\n需先标记 A"));
    }
    if (m_loopAddButton)
    {
        m_loopAddButton->setToolTip(
            m_loopAMs >= 0 && m_loopBMs > m_loopAMs
                ? LT("保存 %1 – %2 为循环片段（Ctrl+L）")
                      .arg(LoopClipRules::formatTimecode(m_loopAMs),
                           LoopClipRules::formatTimecode(m_loopBMs))
                : LT("先标记 A 和 B，再把区间保存为循环片段（Ctrl+L）"));
    }
    if (m_loopClearButton)
    {
        m_loopClearButton->setToolTip(
            m_loopAMs >= 0 || m_loopBMs >= 0
                ? LT("取消当前的 A/B 标记（不会保存片段）")
                : LT("当前没有 A/B 标记"));
    }

    updateLoopButtonsEnabled();
    updateLoopOverlay();
}

void MainWindow::clearLoopMarkers()
{
    if (m_loopAMs < 0 && m_loopBMs < 0)
        return;
    m_loopAMs = -1;
    m_loopBMs = -1;
    updateLoopMarkerUi();
    showStatusMessage(LT("已清除 A/B 标记"), 2000);
}

void MainWindow::markLoopPointA()
{
    if (m_mediaPath.isEmpty())
    {
        showStatusMessage(LT("请先打开一个视频"), 2500);
        return;
    }
    // Marking while playing would let the position drift away immediately.
    if (m_engine->isPlaying())
        togglePlayback();

    m_loopAMs = m_engine->positionMs();
    if (m_loopBMs >= 0 && m_loopBMs <= m_loopAMs)
    {
        m_loopBMs = -1;
        showStatusMessage(
            LT("已标记 A %1，B 早于 A 已自动清除")
                .arg(LoopClipRules::formatTimecode(m_loopAMs)),
            3500);
    }
    else
    {
        showStatusMessage(LT("已标记 A：%1")
                              .arg(LoopClipRules::formatTimecode(m_loopAMs)),
                          2500);
    }
    updateLoopMarkerUi();
}

void MainWindow::markLoopPointB()
{
    if (m_mediaPath.isEmpty() || m_loopAMs < 0)
    {
        showStatusMessage(LT("请先标记起点 A"), 2500);
        return;
    }
    if (m_engine->isPlaying())
        togglePlayback();

    qint64 position = m_engine->positionMs();
    QString message;
    if (position <= m_loopAMs)
    {
        // Marked the other way round: swap instead of rejecting the input.
        std::swap(m_loopAMs, position);
        m_loopBMs = position;
        message = LT("已自动交换 A/B：%1 – %2")
                      .arg(LoopClipRules::formatTimecode(m_loopAMs),
                           LoopClipRules::formatTimecode(m_loopBMs));
    }
    else
    {
        m_loopBMs = position;
        message = LT("已标记 B：%1")
                      .arg(LoopClipRules::formatTimecode(m_loopBMs));
    }

    if (m_loopBMs - m_loopAMs < LoopClipRules::kMinClipDurationMs)
    {
        m_loopBMs = -1;
        showStatusMessage(LT("片段太短（至少 %1 毫秒），请重新标记 B")
                              .arg(LoopClipRules::kMinClipDurationMs),
                          4000);
    }
    else
    {
        showStatusMessage(message, 3000);
    }
    updateLoopMarkerUi();
}

void MainWindow::addLoopClip()
{
    if (!m_loopStore || m_mediaPath.isEmpty() || m_loopAMs < 0 || m_loopBMs <= m_loopAMs)
        return;

    LoopClip clip;
    clip.mediaPath = m_mediaPath;
    clip.startMs = m_loopAMs;
    clip.endMs = m_loopBMs;
    clip.title = LoopClipRules::defaultTitle(m_mediaPath, clip.startMs, clip.endMs);

    LoopClipDialog dialog(this, m_loopStore, clip, m_engine->durationMs());
    if (dialog.exec() != QDialog::Accepted)
    {
        // Cancelling keeps the markers so the user can adjust and try again.
        return;
    }

    QString error;
    const QString clipId = m_loopStore->addClip(dialog.clip(), &error);
    if (clipId.isEmpty())
    {
        QMessageBox::warning(this, LT("添加循环片段失败"), error);
        return;
    }
    m_loopStore->save();

    m_loopAMs = -1;
    m_loopBMs = -1;
    updateLoopMarkerUi();
    refreshLoopPanelContext();
    if (m_loopPanel)
        m_loopPanel->ensureVisibleClip(clipId);
    showStatusMessage(LT("已添加循环片段：%1").arg(dialog.clip().title), 3500);
}

void MainWindow::onLoopClipActivated(const QString& clipId)
{
    if (!m_loopStore || clipId.isEmpty())
        return;
    const LoopClip clip = m_loopStore->clip(clipId);
    if (clip.id.isEmpty())
        return;
    if (!QFileInfo::exists(clip.mediaPath))
    {
        showStatusMessage(LT("视频文件不存在，无法播放该片段"), 4000);
        return;
    }

    const bool sameMedia =
        !m_mediaPath.isEmpty()
        && PathUtils::normalizeMediaPath(m_mediaPath)
               == PathUtils::normalizeMediaPath(clip.mediaPath);
    if (!sameMedia)
    {
        QString script;
        if (m_library)
            script = m_library->scriptForMedia(clip.mediaPath);
        // The strip still holds the previous video's script. Showing the new
        // clip's A-B markers on top of it looks like a stray loop band, so the
        // heat map is cleared here and repainted once the new file reports its
        // duration (see onDurationChanged).
        if (m_heatmap)
            m_heatmap->clear();
        showStatusMessage(LT("正在加载循环片段：%1").arg(clip.title), 4000);
        // mpv loads asynchronously: the seek to A can only be applied once the
        // file reports its duration, otherwise it would be ignored and playback
        // would start from 0. loadMedia also clears the previous loop state, so
        // the new one is set up after it returns.
        loadMedia(clip.mediaPath, script, false);
        m_pendingLoopSeekMs = clip.startMs;
    }

    // A clip switch inside the same video keeps the loop running, so the
    // auto-next setting must only be remembered when the loop actually starts.
    // Saving it again on every switch would overwrite the remembered value with
    // the suppressed one and the user's setting could never be restored.
    const bool enteringLoop = m_activeLoopClipId.isEmpty();

    m_activeLoopClipId = clip.id;
    m_activeLoopStartMs = clip.startMs;
    m_activeLoopEndMs = clip.endMs;
    m_loopRewinding = false;
    m_loopRewindTicks = 0;

    if (enteringLoop)
    {
        m_autoNextBeforeLoop = m_autoNext;
        if (m_autoNext)
            setAutoNextEnabled(false, false);
    }

    if (sameMedia && m_pendingLoopSeekMs < 0 && m_engine->durationMs() > 0)
    {
        // Same file and it is fully loaded: the seek can be applied right away.
        m_engine->seek(clip.startMs);
        m_link->seek(clip.startMs);
        m_engine->play();
        m_link->start(true);
    }
    else
    {
        // Either the file was just handed to mpv (seek would be ignored), or a
        // previous jump is still waiting for it: retarget that pending jump.
        m_pendingLoopSeekMs = clip.startMs;
    }
    if (m_heatmap)
    {
        m_heatmap->setPosition(clip.startMs);
        // While looping, the strip shows the clip being played rather than the
        // manual A/B markers. Skipped while another video is loading: the
        // markers would land on the previous timeline.
        if (sameMedia)
            m_heatmap->setLoopMarkers(clip.startMs, clip.endMs);
    }
    if (m_loopPanel)
        m_loopPanel->setActiveClipId(clip.id);

    AppLogger::log(QStringLiteral("loop"),
                   QStringLiteral("循环播放: %1 [%2 - %3]")
                       .arg(clip.title)
                       .arg(clip.startMs)
                       .arg(clip.endMs));
    showStatusMessage(LT("循环中：%1（%2–%3）")
                          .arg(clip.title, LoopClipRules::formatTimecode(clip.startMs),
                               LoopClipRules::formatTimecode(clip.endMs)),
                      4000);
}

void MainWindow::stopLoopPlayback(const QString& reason)
{
    if (m_activeLoopClipId.isEmpty())
        return;

    AppLogger::log(QStringLiteral("loop"), QStringLiteral("退出循环播放"));
    m_activeLoopClipId.clear();
    m_activeLoopStartMs = -1;
    m_activeLoopEndMs = -1;
    m_loopRewinding = false;
    m_loopRewindTicks = 0;
    if (m_loopPanel)
        m_loopPanel->setActiveClipId(QString());
    updateLoopOverlay();

    // Restoring the auto-next setting keeps the user's choice intact.
    if (m_autoNext != m_autoNextBeforeLoop)
        setAutoNextEnabled(m_autoNextBeforeLoop);

    if (!reason.isEmpty())
        showStatusMessage(reason, 3000);
}

bool MainWindow::advanceToNextLoopClip()
{
    if (m_activeLoopClipId.isEmpty() || !m_loopPanel || !m_loopPanel->autoNext())
        return false;

    // Walk the visible list until a playable clip turns up. Activating a clip
    // whose file disappeared is a no-op, and the caller would retry it on every
    // position update.
    const int count = m_loopPanel->visibleClipCount();
    QString candidate = m_activeLoopClipId;
    for (int attempt = 0; attempt < count; ++attempt)
    {
        candidate = m_loopPanel->nextClipId(candidate);
        // Empty list, or wrapped back onto the running clip: nothing else to
        // play, so the caller falls back to a normal rewind.
        if (candidate.isEmpty() || candidate == m_activeLoopClipId)
            return false;

        const LoopClip clip = m_loopStore ? m_loopStore->clip(candidate) : LoopClip();
        if (clip.id.isEmpty() || !QFileInfo::exists(clip.mediaPath))
        {
            AppLogger::log(QStringLiteral("loop"),
                           QStringLiteral("跳过失效片段: %1").arg(candidate));
            continue;
        }

        AppLogger::log(QStringLiteral("loop"),
                       QStringLiteral("连播下一个片段: %1").arg(candidate));
        onLoopClipActivated(candidate);
        return true;
    }
    return false;
}

void MainWindow::refreshLoopPanelContext()
{
    if (!m_loopPanel)
        return;
    if (m_library)
    {
        // The library owns the availability decision for both panels, so hand
        // it the loop list's media paths first. The call is a no-op unless the
        // set actually changed, which stops the refresh from recursing.
        if (m_loopStore)
        {
            QStringList loopPaths;
            loopPaths.reserve(m_loopStore->clipCount());
            for (const LoopClip& clip : m_loopStore->clips())
                loopPaths.append(clip.mediaPath);
            m_library->setAdditionalReferences(loopPaths);
        }
        m_loopPanel->setAvailablePaths(m_library->availablePathSet());
        m_loopPanel->setMediaThumbnails(m_library->thumbnailPathsByMedia());
        m_loopPanel->setScriptAxes(m_library->scriptAxesByMedia());
    }
    updateLoopButtonsEnabled();
}

void MainWindow::onMediaActivated(const QString& mediaPath, const QString& scriptPath)
{
    AppLogger::log(QStringLiteral("ui"), QStringLiteral("媒体库双击: %1").arg(mediaPath));
    loadMedia(mediaPath, scriptPath);
}

void MainWindow::loadMedia(const QString& mediaPath, const QString& selectedScript, bool autoPlay)
{
    // mpv's libmpv video output can only bind to a render context that already
    // exists. Opening a file before the video widget's first paint makes mpv
    // report "vo/libmpv: No render context set" and give up on video for the
    // whole session: the audio keeps playing while the picture never updates,
    // and because the picture then only refreshes when the window repaints, it
    // looks like playback freezes whenever something changes the window - most
    // visibly on a full screen switch. Every load path (media library, drag and
    // drop, command line, auto-next) comes through here, so the wait lives here.
    if (!videoSurfaceReady())
    {
        m_pendingMediaPath = mediaPath;
        m_pendingMediaScript = selectedScript;
        m_pendingMediaAutoPlay = autoPlay;
        AppLogger::log(QStringLiteral("ui"),
                       QStringLiteral("视频表面尚未就绪，延迟加载: %1").arg(mediaPath));
        if (m_pendingMediaTimer)
            return;
        m_pendingMediaTimer = new QTimer(this);
        m_pendingMediaTimer->setInterval(20);
        connect(m_pendingMediaTimer, &QTimer::timeout, this, [this] {
            if (!videoSurfaceReady())
                return;
            m_pendingMediaTimer->stop();
            m_pendingMediaTimer->deleteLater();
            m_pendingMediaTimer = nullptr;
            const QString path = m_pendingMediaPath;
            const QString script = m_pendingMediaScript;
            const bool play = m_pendingMediaAutoPlay;
            m_pendingMediaPath.clear();
            m_pendingMediaScript.clear();
            if (!path.isEmpty())
                loadMedia(path, script, play);
        });
        m_pendingMediaTimer->start();
        return;
    }

    AppLogger::log(QStringLiteral("ui"),
                   QStringLiteral("loadMedia: %1 | selected: %2").arg(mediaPath, selectedScript));
    // Loading another video always ends an active clip loop and drops the
    // markers, which belonged to the previous timeline.
    if (!m_activeLoopClipId.isEmpty())
        stopLoopPlayback();
    if (m_loopAMs >= 0 || m_loopBMs >= 0)
    {
        m_loopAMs = -1;
        m_loopBMs = -1;
        updateLoopMarkerUi();
    }
    m_link->pause();
    // Any jump that was waiting for the previous file is no longer relevant.
    m_pendingLoopSeekMs = -1;
    cancelPendingSeek();
    // A different file gets a fresh recovery budget; re-opening the same one
    // (the stall watchdog) must not reset it, or it could loop forever.
    if (mediaPath != m_mediaPath)
        m_renderRecoveryAttempts = 0;
    m_mediaPath = mediaPath;
    m_mediaScript = selectedScript;
    m_bundle = ScriptLoader::load(mediaPath, selectedScript);
    applySavedAxisConfigs(m_bundle);
    m_link->setBundle(m_bundle);

    m_engine->loadFile(mediaPath);
    if (autoPlay)
    {
        m_engine->play();
        // A different file means the device may be nowhere near the new
        // timeline, so this one keeps the safety ramp.
        m_link->start(true);
    }
    m_videoStack->setCurrentWidget(m_video);
    m_video->update();
    AppLogger::log(QStringLiteral("ui"),
                   QStringLiteral("bundle format=%1 tracks=%2")
                       .arg(scriptFormatName(m_bundle.format))
                       .arg(m_bundle.tracks.size()));
    for (const QString& warning : m_bundle.warnings)
        AppLogger::log(QStringLiteral("script"), QStringLiteral("警告: %1").arg(warning));
    updateStatus();
    if (m_library)
        m_library->setPlayingMedia(mediaPath, QString());
    if (m_bundle.tracks.isEmpty())
    {
        AppLogger::log(QStringLiteral("ui"), QStringLiteral("未找到匹配脚本，仅播放视频"));
        showStatusMessage(
            LT("未找到匹配脚本，仅播放视频（右键卡片可手动选择脚本）"), 6000);
    }
    updateLoopButtonsEnabled();
}

void MainWindow::loadPersistedSettings()
{
    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);

    m_lastMediaFolder = settings.value(QStringLiteral("paths/lastMediaFolder")).toString();
    // Escape hatch for the native full screen path: flipping this key to false
    // brings back the plain showFullScreen() route on the next start.
    m_useNativeFullScreen =
        settings.value(QStringLiteral("ui/nativeFullscreen"), true).toBool();
    m_link->setSeekRampMsPerPercent(
        settings.value(QStringLiteral("sync/seekRampMsPerPercent"), 30).toInt());
    m_link->setSeekSafetyEnabled(
        settings.value(QStringLiteral("sync/seekSafetyEnabled"), true).toBool());
    // Per-command log detail: off unless asked for. NEKOBEAT_TCODE_LOG
    // overrides the stored setting so a headless probe run can capture the
    // exact command sequence without touching the user's configuration.
    const bool tcodeDetailOverride = !qEnvironmentVariableIsEmpty("NEKOBEAT_TCODE_LOG");
    const bool tcodeDetail =
        tcodeDetailOverride
            ? QString::fromLocal8Bit(qgetenv("NEKOBEAT_TCODE_LOG")) != QLatin1String("0")
            : settings.value(QStringLiteral("sync/tcodeDetail"), false).toBool();
    AppLogger::setTcodeDetailEnabled(tcodeDetail);
    if (m_tcodeDetailAction)
    {
        const QSignalBlocker blocker(m_tcodeDetailAction);
        m_tcodeDetailAction->setChecked(tcodeDetail);
    }
    setPlayPauseEasingEnabled(
        settings.value(QStringLiteral("sync/playPauseEasing"), true).toBool(), false);
    m_autoNext = settings.value(QStringLiteral("playback/autoNext"), true).toBool();
    m_heatmapVisible = settings.value(QStringLiteral("view/heatmap"), true).toBool();
    m_axisTagVisible = settings.value(QStringLiteral("view/axisTag"), true).toBool();
    if (m_library)
        m_library->setAxisTagVisible(m_axisTagVisible);
    if (m_loopPanel)
        m_loopPanel->setAxisTagVisible(m_axisTagVisible);
    if (m_heatmapAction)
    {
        m_heatmapAction->blockSignals(true);
        m_heatmapAction->setChecked(m_heatmapVisible);
        m_heatmapAction->blockSignals(false);
    }
    if (m_heatmap)
        m_heatmap->setVisible(m_heatmapVisible);
    if (m_axisTagAction)
    {
        m_axisTagAction->blockSignals(true);
        m_axisTagAction->setChecked(m_axisTagVisible);
        m_axisTagAction->blockSignals(false);
    }
    m_volume = std::clamp(settings.value(QStringLiteral("playback/volume"), 100).toInt(),
                          0, 100);
    if (m_volumeSlider)
        m_volumeSlider->setValue(m_volume);
    if (m_volumeLabel)
        m_volumeLabel->setText(QStringLiteral("%1%").arg(m_volume));
    m_engine->setVolume(m_volume);
    // Push the restored value into both the menu action and the library button.
    setAutoNextEnabled(m_autoNext, false);

    // Seed the first axis profile from the pre-profile settings layout.
    QMap<int, AxisConfig> legacy;
    for (Track track : TrackInfo::sr6Tracks())
    {
        const QString prefix = QStringLiteral("axis/%1/").arg(TrackInfo::tcodeId(track));
        if (!settings.contains(prefix + QStringLiteral("min")))
            continue;
        AxisConfig config;
        config.min = settings.value(prefix + QStringLiteral("min"), 0).toInt();
        config.max = settings.value(prefix + QStringLiteral("max"), 9999).toInt();
        config.home = settings.value(prefix + QStringLiteral("home"), 5000).toInt();
        config.inverted = settings.value(prefix + QStringLiteral("inverted"), false).toBool();
        config.offsetMs = settings.value(prefix + QStringLiteral("offsetMs"), 0).toLongLong();
        legacy.insert(static_cast<int>(track), config);
    }
    if (m_axisPanel && !legacy.isEmpty())
        m_axisPanel->migrateLegacy(legacy);

    // Media library panel width and fold state.
    // No upper bound: the library may be dragged as wide as the window allows.
    m_libraryWidth = std::max(200,
                              settings.value(QStringLiteral("library/panelWidth"), 430).toInt());
    m_libraryCollapsed = settings.value(QStringLiteral("library/collapsed"), false).toBool();
    m_loopWidth = std::max(200,
                           settings.value(QStringLiteral("loop/panelWidth"), 360).toInt());
    m_loopCollapsed = settings.value(QStringLiteral("loop/collapsed"), true).toBool();
    QTimer::singleShot(0, this, [this]() {
        if (!m_libraryFold)
            return;
        m_libraryFold->setPanelWidth(m_libraryWidth);
        m_libraryFold->setCollapsed(false);
        if (m_libraryCollapsed)
            m_libraryFold->setCollapsed(true);
    });
    QTimer::singleShot(0, this, [this]() {
        if (!m_loopFold)
            return;
        m_loopFold->setPanelWidth(m_loopWidth);
        m_loopFold->setCollapsed(false);
        if (m_loopCollapsed)
            m_loopFold->setCollapsed(true);
    });
    QTimer::singleShot(0, this, [this]() { refreshLoopPanelContext(); });
    updateLoopMarkerUi();

    // Panel layout. Applied after the widths and fold states above so the
    // swap moves the panels with their sizes already restored.
    if (settings.value(QStringLiteral("layout/swappedSides"), false).toBool())
    {
        QTimer::singleShot(0, this, [this]() { setSwappedSides(true); });
    }
}

void MainWindow::savePersistedSettings()
{
    QSettings settings;
    settings.setValue(QStringLiteral("settings/axisSchemaVersion"), 2);
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("paths/lastMediaFolder"), m_lastMediaFolder);
    // While a clip loops auto-next is switched off internally; store what the
    // user actually chose.
    settings.setValue(QStringLiteral("playback/autoNext"),
                      m_activeLoopClipId.isEmpty() ? m_autoNext : m_autoNextBeforeLoop);
    settings.setValue(QStringLiteral("view/heatmap"), m_heatmapVisible);
    settings.setValue(QStringLiteral("view/axisTag"), m_axisTagVisible);
    settings.setValue(QStringLiteral("playback/volume"), m_volume);
    settings.setValue(QStringLiteral("sync/tcodeDetail"), AppLogger::tcodeDetailEnabled());
    settings.setValue(QStringLiteral("library/panelWidth"),
                      std::max(200, m_libraryWidth));
    settings.setValue(QStringLiteral("library/collapsed"), m_libraryCollapsed);
    settings.setValue(QStringLiteral("loop/panelWidth"), std::max(200, m_loopWidth));
    settings.setValue(QStringLiteral("loop/collapsed"), m_loopCollapsed);

    for (auto it = m_bundle.tracks.constBegin(); it != m_bundle.tracks.constEnd(); ++it)
    {
        const QString prefix = QStringLiteral("axis/%1/").arg(TrackInfo::tcodeId(it.value().track));
        const AxisConfig& config = it.value().config;
        settings.setValue(prefix + QStringLiteral("min"), config.min);
        settings.setValue(prefix + QStringLiteral("max"), config.max);
        settings.setValue(prefix + QStringLiteral("home"), config.home);
        settings.setValue(prefix + QStringLiteral("inverted"), config.inverted);
        settings.setValue(prefix + QStringLiteral("offsetMs"), config.offsetMs);
    }

    if (m_axisPanel)
        m_axisPanel->commitEdits();
}

void MainWindow::applySavedAxisConfigs(ScriptBundle& bundle)
{
    const QMap<int, AxisConfig> profile = m_axisPanel ? m_axisPanel->activeConfigs()
                                                      : QMap<int, AxisConfig>{};
    const DeviceCapabilities capabilities = m_link ? m_link->capabilities()
                                                     : DeviceCapabilities{};
    for (auto it = bundle.tracks.begin(); it != bundle.tracks.end(); ++it)
    {
        AxisConfig& config = it.value().config;
        // Safety: never drive an axis the connected device did not report. While
        // the probe is still running everything stays enabled and is filtered
        // once the capabilities arrive.
        config.enabled = capabilities.probeComplete
                             ? capabilities.supportedTracks.contains(it.key())
                             : true;
        if (profile.contains(it.key()))
        {
            const AxisConfig& saved = profile.value(it.key());
            config.min = saved.min;
            config.max = saved.max;
            config.home = saved.home;
            config.inverted = saved.inverted;
            config.offsetMs = saved.offsetMs;
        }
    }
}

void MainWindow::realignDeviceIfNeeded()
{
    if (!m_engine->isPlaying() || !m_link->isConnected())
        return;
    m_link->seek(m_engine->positionMs());
}

void MainWindow::onAxisConfigEdited(Track track, AxisConfig config)
{
    const int key = static_cast<int>(track);
    if (m_bundle.tracks.contains(key))
    {
        m_bundle.tracks[key].config = config;
        m_link->setBundle(m_bundle);
        realignDeviceIfNeeded();
    }

    AppLogger::log(QStringLiteral("ui"),
                   QStringLiteral("轴 %1 限位更新: %2 – %3")
                       .arg(TrackInfo::tcodeId(track))
                       .arg(config.min)
                       .arg(config.max));
}

void MainWindow::onAxisPreviewRequested(Track track, int scriptPercent)
{
    if (!m_link || !m_link->isConnected())
        return;
    // During playback the scheduler owns the output; limit edits are applied by
    // the safe re-align instead of raw preview commands.
    if (m_engine->isPlaying() && m_link->isRunning())
        return;

    const int key = static_cast<int>(track);
    const qint64 now = m_previewClock.elapsed();
    if (now - m_lastPreviewAt.value(key, -10000) < 60)
        return;
    m_lastPreviewAt.insert(key, now);
    sendAxisPreview(track, scriptPercent);
}

void MainWindow::sendAxisPreview(Track track, int scriptPercent)
{
    if (!m_link || !m_link->isConnected() || !m_axisPanel)
        return;

    const DeviceCapabilities capabilities = m_link->capabilities();
    // Same rule as reset: no preview before the probe says what exists.
    if (!capabilities.probeComplete
        || !capabilities.supportedTracks.contains(static_cast<int>(track)))
        return;

    const QMap<int, AxisConfig> configs = m_axisPanel->activeConfigs();
    const QString line = TCodeEncoder::encodeLine({{track, scriptPercent, 700}}, configs);
    if (line.isEmpty())
        return;
    m_link->sendRawLine(line);
    AppLogger::log(QStringLiteral("tcode"), QStringLiteral("限位预览: %1").arg(line));
}

void MainWindow::onProfileApplied(const QMap<int, AxisConfig>& configs)
{
    bool changed = false;
    for (auto it = m_bundle.tracks.begin(); it != m_bundle.tracks.end(); ++it)
    {
        if (!configs.contains(it.key()))
            continue;
        AxisConfig config = configs.value(it.key());
        config.enabled = it.value().config.enabled;
        it.value().config = config;
        changed = true;
    }
    if (!changed)
        return;

    m_link->setBundle(m_bundle);
    realignDeviceIfNeeded();
    showStatusMessage(LT("已应用轴限制配置"), 2500);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    AppLogger::log(QStringLiteral("ui"),
                   QStringLiteral("关闭程序，日志文件: %1").arg(AppLogger::logFilePath()));
    if (m_link)
        m_link->pause();
    if (m_link)
        m_link->disconnectDevice();
    if (m_engine)
        m_engine->stopPlayback();
    savePersistedSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::setAutoNextEnabled(bool enabled, bool persist)
{
    m_autoNext = enabled;

    if (m_autoNextAction)
    {
        m_autoNextAction->blockSignals(true);
        m_autoNextAction->setChecked(enabled);
        m_autoNextAction->blockSignals(false);
    }
    if (m_library)
        m_library->setAutoNext(enabled);

    // Loop playback suppresses auto-next while it owns the playlist. That
    // temporary suppression must never reach the settings file, otherwise the
    // user's choice would be silently turned off.
    if (persist)
    {
        QSettings settings;
        settings.setValue(QStringLiteral("playback/autoNext"), enabled);
    }
}

void MainWindow::setSwappedSides(bool swapped)
{
    if (m_swappedSides == swapped || !m_splitter || !m_librarySide || !m_loopSide)
        return;

    const int libraryWidth = m_libraryFold ? m_libraryFold->panelWidth() : m_libraryWidth;
    const int loopWidth = m_loopFold ? m_loopFold->panelWidth() : m_loopWidth;

    m_swappedSides = swapped;

    // insertWidget moves the existing widgets, so both panels keep their state,
    // their thumbnails and their fold controllers.
    m_splitter->insertWidget(0, swapped ? m_loopSide : m_librarySide);
    m_splitter->insertWidget(2, swapped ? m_librarySide : m_loopSide);

    // Re-apply the splitter flags: re-inserting widgets resets some of them.
    m_splitter->setCollapsible(0, true);
    m_splitter->setCollapsible(1, false);
    m_splitter->setCollapsible(2, true);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setStretchFactor(2, 0);

    // The controllers follow their panel to the other side.
    if (m_libraryFold)
        m_libraryFold->setPlacement(swapped ? SidePanelFold::Side::Right
                                            : SidePanelFold::Side::Left,
                                    swapped ? 2 : 0);
    if (m_loopFold)
        m_loopFold->setPlacement(swapped ? SidePanelFold::Side::Left
                                         : SidePanelFold::Side::Right,
                                 swapped ? 0 : 2);

    // Give each panel back the width it had before the swap.
    if (m_libraryFold && !m_libraryCollapsed)
        m_libraryFold->setPanelWidth(libraryWidth);
    if (m_loopFold && !m_loopCollapsed)
        m_loopFold->setPanelWidth(loopWidth);

    if (m_libraryFold)
        m_libraryFold->updateGeometry();
    if (m_loopFold)
        m_loopFold->updateGeometry();

    // While full screen the overlays show the two panels; they follow the swap
    // immediately and go back into hiding so the picture is not covered.
    if (m_fullScreen)
    {
        hideFullScreenPanels();
        // Re-inserting the columns can bring them back, and over the picture the
        // empty columns must stay away until full screen ends.
        if (m_librarySide)
            m_librarySide->setVisible(false);
        if (m_loopSide)
            m_loopSide->setVisible(false);
        updateFullScreenPanelGeometry();
    }

    QSettings settings;
    settings.setValue(QStringLiteral("layout/swappedSides"), swapped);
    if (m_swapSidesAction)
    {
        m_swapSidesAction->blockSignals(true);
        m_swapSidesAction->setChecked(swapped);
        m_swapSidesAction->blockSignals(false);
    }
    AppLogger::log(QStringLiteral("ui"),
                   QStringLiteral("面板布局: %1")
                       .arg(swapped ? QStringLiteral("循环列表在左")
                                    : QStringLiteral("媒体库在左")));
}

void MainWindow::updateUptime()
{
    if (!m_uptimeLabel)
        return;

    const qint64 total = m_uptimeClock.elapsed() / 1000;
    const qint64 hours = total / 3600;
    const qint64 minutes = (total % 3600) / 60;
    const qint64 seconds = total % 60;

    m_uptimeLabel->setText(
        hours > 0
            ? QStringLiteral("%1:%2:%3")
                  .arg(hours)
                  .arg(minutes, 2, 10, QLatin1Char('0'))
                  .arg(seconds, 2, 10, QLatin1Char('0'))
            : QStringLiteral("%1:%2")
                  .arg(minutes, 2, 10, QLatin1Char('0'))
                  .arg(seconds, 2, 10, QLatin1Char('0')));
}

void MainWindow::showStatusMessage(const QString& text, int timeoutMs)
{
    if (!m_messageLabel)
        return;
    // Elide here rather than letting the layout crop: the chip is capped, and a
    // cropped sentence reads like a riddle. The chips carry no stylesheet padding
    // (see Theme.cpp), so the cap is exactly the text width available.
    const int available = std::max(40, m_messageLabel->maximumWidth());
    m_messageLabel->setText(
        m_messageLabel->fontMetrics().elidedText(text, Qt::ElideRight, available));
    // The full sentence stays reachable on hover.
    m_messageLabel->setToolTip(text);
    // A chip that has nothing to say must not hold its place in the row: on a
    // narrow window that space is what pushes the others into ellipses.
    m_messageLabel->setVisible(!text.isEmpty());
    updateStatusChips();
    if (m_messageTimer)
    {
        m_messageTimer->stop();
        if (timeoutMs > 0)
            m_messageTimer->start(timeoutMs);
    }
}

void MainWindow::updateStatus()
{
    QString text;
    if (m_bundle.isEmpty())
    {
        text = LT("脚本未加载");
    }
    else
    {
        QStringList axes;
        for (auto it = m_bundle.tracks.constBegin(); it != m_bundle.tracks.constEnd(); ++it)
            axes.append(TrackInfo::tcodeId(it.value().track));
        text = LT("%1 | %2 轴：%3")
                   .arg(scriptFormatName(m_bundle.format))
                   .arg(axes.size())
                   .arg(axes.join(QStringLiteral(", ")));
    }
    // A six axis list is long enough to eat the whole status row on its own, and
    // whatever does not fit gets cropped. Cap it and keep the full list on hover.
    const int cap = std::max(60, m_statusScript->maximumWidth());
    m_statusScript->setText(
        m_statusScript->fontMetrics().elidedText(text, Qt::ElideRight, cap));
    m_statusScript->setToolTip(text);
    updateStatusChips();
}

void MainWindow::clearAxisPositions()
{
    if (!m_axisPanel)
        return;
    for (Track track : TrackInfo::sr6Tracks())
        m_axisPanel->setAxisPosition(static_cast<int>(track), -1);
}

void MainWindow::onPositionChanged(qint64 positionMs)
{
    // Fallback: if the duration signal never arrives for the freshly loaded
    // file, the pending jump to A is applied on the first position update.
    if (m_pendingLoopSeekMs >= 0)
    {
        applyPendingLoopSeek();
        if (m_pendingLoopSeekMs < 0)
            return;
    }

    // Loop clip: jump back to A as soon as B is reached. The scheduler follows
    // with its usual safety ramp so the device never snaps across the axis.
    if (m_activeLoopStartMs >= 0 && m_activeLoopEndMs > m_activeLoopStartMs)
    {
        if (m_loopRewinding)
        {
            // Stay quiet until the seek really landed inside the range again.
            if (positionMs < m_activeLoopEndMs)
            {
                m_loopRewinding = false;
                m_loopRewindTicks = 0;
            }
            else if (++m_loopRewindTicks > 60)
            {
                // ~3 s without landing back inside the clip means the seek
                // never took effect; leave the loop instead of freezing the UI
                // and the device on the B point forever.
                AppLogger::log(QStringLiteral("loop"),
                               QStringLiteral("回跳未生效，自动退出循环播放"));
                m_loopRewinding = false;
                m_loopRewindTicks = 0;
                stopLoopPlayback(LT("循环回跳未生效，已退出循环播放"));
            }
            return;
        }
        if (LoopClipRules::shouldRewind(positionMs, m_activeLoopStartMs, m_activeLoopEndMs))
        {
            // Sequential mode: hand over to the next clip of the visible list
            // instead of jumping back to A.
            if (advanceToNextLoopClip())
                return;

            AppLogger::log(QStringLiteral("loop"),
                           QStringLiteral("回跳到 A: %1 ms").arg(m_activeLoopStartMs));
            m_loopRewinding = true;
            m_loopRewindTicks = 0;
            m_engine->seek(m_activeLoopStartMs);
            m_link->seek(m_activeLoopStartMs);
            if (m_heatmap)
                m_heatmap->setPosition(m_activeLoopStartMs);
            return;
        }
    }

    if (m_heatmap)
        m_heatmap->setPosition(positionMs);

    const QString current = DurationFormat::clock(positionMs);
    const QString total = DurationFormat::clock(m_engine->durationMs());
    const QString text = QStringLiteral(
        "<span style='color:#F2F2F7;font-weight:600;'>%1</span>"
        "<span style='color:#6E6E74;'> / %2</span>")
                             .arg(current, total);
    m_statusTime->setText(text);

    // Without a device the scheduler does not tick, so drive the axis panel
    // from the script itself to keep the position readout alive.
    if (m_link && !m_link->isConnected())
        updateScriptPreview(positionMs);
}

void MainWindow::updateScriptPreview(qint64 positionMs)
{
    if (!m_axisPanel)
        return;

    for (Track track : TrackInfo::sr6Tracks())
    {
        const int key = static_cast<int>(track);
        const int percent = m_bundle.tracks.contains(key)
                                ? scriptPositionAt(m_bundle.tracks.value(key), positionMs)
                                : -1;
        m_axisPanel->setAxisPosition(key, percent);
    }
}

void MainWindow::onDurationChanged(qint64 durationMs)
{
    if (m_heatmap)
    {
        m_heatmap->setBundle(m_bundle, durationMs);
        // A clip that had to load another video only gets its band painted once
        // the matching script is in place.
        if (!m_activeLoopClipId.isEmpty() && m_activeLoopStartMs >= 0)
            m_heatmap->setLoopMarkers(m_activeLoopStartMs, m_activeLoopEndMs);
    }
    if (m_library && !m_mediaPath.isEmpty())
        m_library->setPlayingMedia(m_mediaPath, m_engine->hwdecCurrent());

    applyPendingLoopSeek();
}

void MainWindow::applyPendingLoopSeek()
{
    // A loop clip opened a fresh file: only after mpv reports a duration is a
    // seek really accepted, so this is where the jump to A (and the start of
    // playback) happens.
    if (m_pendingLoopSeekMs < 0 || m_engine->durationMs() <= 0)
        return;

    const qint64 target = m_pendingLoopSeekMs;
    m_pendingLoopSeekMs = -1;
    AppLogger::log(QStringLiteral("loop"),
                   QStringLiteral("文件已就绪，跳转到 A: %1 ms").arg(target));
    m_engine->seek(target);
    m_link->seek(target);
    m_engine->play();
    m_link->start(true);
    if (m_heatmap)
        m_heatmap->setPosition(target);
}

void MainWindow::requestSeek(qint64 positionMs)
{
    ++m_seekRequestCount;
    ++m_seekRequestsSinceApply;
    m_pendingSeekMs = positionMs;
    if (!m_seekCoalesceTimer)
    {
        applyPendingSeek();
        return;
    }
    // The heat map already moved its own position marker, so the pointer feels
    // immediate; the picture and the device follow on the coalescing cadence.
    if (!m_seekCoalesceTimer->isActive())
        m_seekCoalesceTimer->start();
}

void MainWindow::applyPendingSeek()
{
    if (m_seekCoalesceTimer)
        m_seekCoalesceTimer->stop();
    if (m_pendingSeekMs < 0)
        return;

    const qint64 target = m_pendingSeekMs;
    m_pendingSeekMs = -1;

    ++m_seekApplyCount;
    if (m_seekRequestsSinceApply > 1)
    {
        AppLogger::log(QStringLiteral("ui"),
                       QStringLiteral("跳转合并：%1 次请求合并为 1 次执行（%2 ms）")
                           .arg(m_seekRequestsSinceApply)
                           .arg(target));
    }
    m_seekRequestsSinceApply = 0;

    // A line per drag step made the log unreadable; a drag now reports at most
    // two lines per second.
    if (!m_seekLogClock.isValid() || m_seekLogClock.elapsed() >= 500)
    {
        m_seekLogClock.restart();
        AppLogger::log(QStringLiteral("ui"),
                       QStringLiteral("画面预览: %1 ms").arg(target));
    }

    // Picture only: while a scrub is in progress the machine is deliberately
    // left alone, and the device half is sent by finishScrub() on release.
    m_engine->seek(target);
}

void MainWindow::cancelPendingSeek()
{
    if (m_seekCoalesceTimer)
        m_seekCoalesceTimer->stop();
    m_pendingSeekMs = -1;
    m_seekRequestsSinceApply = 0;
}

void MainWindow::beginScrub()
{
    m_scrubbing = true;
    m_scrubHeldMachine = false;
}

void MainWindow::holdMachineForScrub()
{
    if (!m_scrubbing || !m_link || !m_link->isConnected())
        return;
    if (!m_link->isRunning())
        return; // Already idle (paused): nothing to hold.

    // The pointer really moved: stop chasing the video and let the machine rest
    // where it is. This is the same eased stop the play/pause key uses (glide to
    // the end of the current stroke when that is close, otherwise DSTOP), so it
    // is not a hard cut in the middle of a movement.
    m_scrubHeldMachine = true;
    m_link->pauseEased();
    AppLogger::log(QStringLiteral("ui"),
                   QStringLiteral("开始拖动热力图：机器暂停跟随，松手后再滑到目标位置"));
}

void MainWindow::finishScrub()
{
    const bool wasScrub = m_scrubbing;
    const bool heldMachine = m_scrubHeldMachine;
    const qint64 releasedAt = m_pendingSeekMs >= 0 ? m_pendingSeekMs
                                                   : (m_engine ? m_engine->positionMs() : 0);
    m_scrubbing = false;
    m_scrubHeldMachine = false;

    // Always land the picture on the released position first, without waiting
    // for the next coalescing tick.
    applyPendingSeek();

    if (!wasScrub || !m_link)
        return;

    const qint64 target = releasedAt;
    // Emergency stop outranks a release: after 急停 the link is disarmed, and
    // letting go of the mouse must not move the machine or resume playback.
    if (!m_link->isArmed() || !m_link->isConnected())
    {
        AppLogger::log(QStringLiteral("ui"),
                       QStringLiteral("松手：机器处于急停/未连接状态，只跳转画面"));
        return;
    }

    const bool playing = m_engine->isPlaying();
    if (!heldMachine)
    {
        // A plain click: the machine never stopped, so this is the usual
        // realignment (the running scheduler glides to the new position).
        m_link->seek(target);
        return;
    }

    // A real drag: the machine was held still and now has to travel to the
    // released position and pick the script up again. start() performs the
    // realignment with the safety ramp (the device may be far away) and resumes
    // the normal action stream; a single command, no double start.
    if (playing)
    {
        m_link->start(true);
    }
    else
    {
        // The video was paused during the drag: realign and stay idle.
        m_link->seek(target);
    }
    AppLogger::log(QStringLiteral("ui"),
                   QStringLiteral("松手：机器%1到 %2 ms")
                       .arg(playing ? QStringLiteral("缓动并恢复联动")
                                    : QStringLiteral("缓动"))
                       .arg(target));
}

void MainWindow::runScrubProbe(int moves)
{
    const qint64 duration = m_engine ? m_engine->durationMs() : 0;
    if (duration <= 0)
    {
        AppLogger::log(QStringLiteral("probe"),
                       QStringLiteral("拖动合并探针：没有可用时长，跳过"));
        QTimer::singleShot(500, qApp, &QCoreApplication::quit);
        return;
    }

    const int total = std::max(1, moves);
    AppLogger::log(QStringLiteral("probe"),
                   QStringLiteral("拖动合并探针开始：%1 次请求，每 5 ms 一次").arg(total));

    // Mimic a real drag: press, move, release. The machine is held still for the
    // whole sweep and only moves once, on release.
    beginScrub();
    holdMachineForScrub();

    // Pointers report moves far faster than either the decoder or the serial
    // link wants to hear about them, so the probe reproduces that rate.
    auto* timer = new QTimer(this);
    timer->setInterval(5);
    const qint64 requestsBefore = m_seekRequestCount;
    const qint64 appliesBefore = m_seekApplyCount;
    connect(timer, &QTimer::timeout, this,
            [this, timer, total, duration, requestsBefore, appliesBefore, step = 0]() mutable {
                if (step >= total)
                {
                    timer->stop();
                    timer->deleteLater();
                    // Exactly what releasing the mouse does.
                    finishScrub();
                    AppLogger::log(
                        QStringLiteral("probe"),
                        QStringLiteral("拖动合并探针结束：%1 次请求 → %2 次画面跳转"
                                       "（设备只在松手后重对齐 1 次）")
                            .arg(m_seekRequestCount - requestsBefore)
                            .arg(m_seekApplyCount - appliesBefore));
                    QTimer::singleShot(300, qApp, &QCoreApplication::quit);
                    return;
                }
                requestSeek(duration * (step + 1) / (total + 1));
                ++step;
            });
    timer->start();
}

void MainWindow::onPlayingChanged(bool playing)
{
    AppLogger::log(QStringLiteral("ui"),
                   QStringLiteral("mpv 播放状态变化: %1").arg(playing ? LT("播放")
                                                                      : QStringLiteral("暂停")));
    m_playButton->setIcon(playing ? Theme::pauseIcon(Qt::white, 16)
                                  : Theme::playIcon(Qt::white, 16));
    m_playButton->setToolTip(playing ? LT("暂停（Space）")
                                     : LT("播放（Space）"));
    updateResetAvailability();
    if (m_library && !m_mediaPath.isEmpty())
        m_library->setPlayingMedia(m_mediaPath, m_engine->hwdecCurrent());
    if (m_library)
        m_library->setPlaybackActive(playing);
}

void MainWindow::onAxisPositionChanged(int track, int posPercent)
{
    if (m_axisPanel)
        m_axisPanel->setAxisPosition(track, posPercent);
}

void MainWindow::onDeviceCapabilitiesChanged()
{
    const DeviceCapabilities capabilities = m_link->capabilities();
    // A disconnected link must not leave the last session's port and firmware in
    // the status row: those are long strings, and a stale value is what squeezed
    // the rest of the row into ellipses.
    if (!m_link->isConnected())
    {
        m_statusDevice->setText(LT("设备未连接"));
        m_statusDevice->setToolTip(QString());
        updateStatusChips();
        return;
    }
    // The chip carries the link itself - which port, cable or Bluetooth - because
    // the row also holds the uptime, the sync offset, the hint line, the script
    // state and the connect button. Firmware and TCode version move to the
    // tooltip, where they cost no width and cannot squeeze the other chips.
    const QString portName = capabilities.port.isEmpty() ? LT("未知端口")
                                                        : capabilities.port;
    // Keep this as short as it can be while still answering "which link am I on".
    // The corner of the menu bar is a tight budget - every extra word here comes
    // out of the chips next to it, and a chip that overflows gets its tail cut
    // off, which is exactly the "COM3 | SR6-" mess this replaced. The paired
    // device name therefore lives in the tooltip.
    const QString linkText =
        m_lastPortIsBluetooth
            ? LT("%1 · 蓝牙").arg(portName)
            : QStringLiteral("%1 · USB").arg(portName);
    m_statusDevice->setText(m_statusDevice->fontMetrics().elidedText(
        linkText, Qt::ElideRight, std::max(60, m_statusDevice->maximumWidth())));
    m_statusDevice->setToolTip(
        LT("%1\n%2固件：%3\nTCode：%4")
            .arg(linkText,
                 m_lastPortIsBluetooth && !m_lastPortBluetoothName.isEmpty()
                     ? LT("蓝牙设备：%1\n").arg(m_lastPortBluetoothName)
                     : QString(),
                 capabilities.firmware.isEmpty() ? LT("未知") : capabilities.firmware,
                 capabilities.tcodeVersion.isEmpty() ? LT("未知") : capabilities.tcodeVersion));
    updateStatusChips();

    if (!capabilities.probeComplete)
        return;

    if (!capabilities.valid)
    {
        // Nothing answered the handshake, so there is no axis list to trust.
        // Keep the scheduler locked instead of declaring the axes confirmed and
        // starting a link that would never move - the window used to show
        // "running" while the device stayed silent.
        m_link->setDeviceAxesConfirmed(false);
        showStatusMessage(
            LT("设备未回应轴识别（D0/D1/D2），未开始联动；可断开后重连重试"), 8000);
        return;
    }

    m_link->setDeviceAxesConfirmed(true);

    QStringList supportedAxes;
    for (int track : capabilities.supportedTracks)
        supportedAxes.append(TrackInfo::tcodeId(static_cast<Track>(track)));
    AppLogger::log(QStringLiteral("serial"),
                   QStringLiteral("探测完成，支持轴: %1")
                       .arg(supportedAxes.join(QStringLiteral(", "))));

    bool bundleChanged = false;
    for (auto it = m_bundle.tracks.begin(); it != m_bundle.tracks.end(); ++it)
    {
        const bool supported = capabilities.supportedTracks.contains(static_cast<int>(it.value().track));
        AxisConfig& config = it.value().config;
        if (config.enabled != supported)
        {
            config.enabled = supported;
            bundleChanged = true;
        }
    }
    if (bundleChanged)
        m_link->setBundle(m_bundle);

    if (m_engine->isPlaying())
    {
        if (!m_scrubbing)
            m_link->start(true);
    }
}
