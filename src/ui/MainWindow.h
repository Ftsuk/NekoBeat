#pragma once

#include "core/Loc.h"
#include "core/Types.h"

#include <QCursor>
#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QMap>

class AxisLimitPanel;
class DeviceLink;
class MpvEngine;
class MpvVideoWidget;
class MediaLibrary;
class LoopListPanel;
class LoopStore;
class SidePanelFold;
class HeatmapWidget;
class QLabel;
class QAction;
class QMenu;
class QSlider;
class QStackedWidget;
class QToolButton;
class QSplitter;
class QWidget;
class QDragEnterEvent;
class QDropEvent;
class QCloseEvent;
class QResizeEvent;
class QEvent;
class QGraphicsOpacityEffect;
class QPropertyAnimation;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openMedia(const QString& mediaPath, const QString& selectedScript = {});
    void addLibraryFolder(const QString& folder);
    // Diagnostic (--playback-probe): samples the media clock and the render
    // counters every 500 ms without changing any behaviour.
    void runPlaybackProbe(int seconds);
    // Diagnostic (--fullscreen-probe): toggles full screen through the same
    // action the double click and F11 use, so the logged frame counters describe
    // what a person would see, then quits.
    void runFullScreenProbe(int rounds);
    // Diagnostic (--fold-probe): logs the fold state of both side panels, the
    // geometry of the full screen overlay hosts and what a click on the chevron
    // would actually hit, then walks through a click / full screen round trip.
    void runFoldProbe(int steps);
    // Diagnostic (--overlay-probe): goes full screen and pulls the edge panels
    // out and back in on a timer, logging the render counters around every step
    // so a screen capture / the log shows whether that still costs a frame.
    void runOverlayProbe(int rounds);
    // Diagnostic (--click-probe): logs the fold state twice a second and does
    // nothing else, so a script can drive real mouse clicks at the chevron and
    // read back what the window did.
    void runClickProbe(int seconds);
    // Diagnostic: fires a burst of seek requests at pointer speed and reports
    // how many of them actually reached the engine/device after coalescing.
    void runScrubProbe(int moves);
    qint64 renderedFrameCount() const;
    qint64 renderRequestCount() const;
    qint64 renderNanos() const;
    // True once the video surface owns a render context. Opening a file before
    // that makes mpv give up on its video output for the rest of the session.
    bool videoSurfaceReady() const;
    // Silent watchdog: writes a log line only when the picture really stops
    // updating while the media is playing, so a report of "it froze" comes with
    // evidence instead of guesswork.
    void startRenderWatch();
    // Silent heartbeat on the GUI thread: logs only when the event loop was
    // blocked long enough for the picture (and the device) to visibly stall.
    void startUiHeartbeat();
    // Diagnostic (--simulate-device-load): writes one TCode line every 2 ms,
    // reproducing the log/port traffic a connected device puts on the GUI
    // thread. Used to test whether that traffic is what makes the picture stall.
    void startDeviceLoadSimulation();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private slots:
    void openVideo();
    void openFolder();
    void connectDevice();
    // Opens the port, retrying a couple of times with the handle released in
    // between: a port that another program just let go is often free a moment
    // later, and that is the case users hit most often.
    void openSerialPort(const QString& port, int retriesLeft);
    // Keeps the menu bar's right-hand status row readable: the bar gives that
    // corner a fixed slice and crops whatever overflows, so on a narrow window
    // the least important chips are hidden instead of being cut in half.
    void updateStatusChips();
    // Closes whatever this program still holds (the serial handle and the motion
    // link), so the port is really free for the next attempt.
    void releaseSerialPort();
    void toggleDeviceConnection();
    void disconnectDevice();
    void togglePlayback();
    void stopPlayback();
    void resetAxes();
    void configureSeekSafety();
    void showAxisLimitPanel();
    // Per-axis min/max/home/invert/offset plus "drive there" test buttons. The
    // dialog existed but had no entry point, so the reverse and offset fields
    // could only be changed by editing JSON.
    void openCalibrationDialog();
    void toggleFullScreen(bool enabled);
    void setLibraryCollapsed(bool collapsed);
    void setLoopListCollapsed(bool collapsed);
    void syncLibraryMenu();
    void syncLoopMenu();
    void openLibraryDialog();
    void openFavoritesDialog();
    void openLoopManagerDialog();
    // Re-points missing entries at their new location; onlyPath restricts the
    // dialog to a single grey card.
    void openRelinkDialog(const QString& onlyPath = QString());
    // One clean-up pass over favourites, loop clips and tags.
    void cleanupMissingMedia();
    void onMediaActivated(const QString& mediaPath, const QString& scriptPath);
    void onLoopClipActivated(const QString& clipId);
    void markLoopPointA();
    void markLoopPointB();
    void addLoopClip();
    void clearLoopMarkers();
    void onPositionChanged(qint64 positionMs);
    void onDurationChanged(qint64 durationMs);
    void onPlayingChanged(bool playing);
    void onAxisPositionChanged(int track, int posPercent);
    void onDeviceCapabilitiesChanged();
    void onAxisConfigEdited(Track track, AxisConfig config);
    void onAxisPreviewRequested(Track track, int scriptPercent);
    void onProfileApplied(const QMap<int, AxisConfig>& configs);
    void panicStop();
    void showAbout();
    // Interface language: the choice is written to QSettings and the program
    // restarts, because every label was already looked up when the window was
    // built. The dialog is shown in the language that was just picked.
    void setInterfaceLanguage(Loc::Language language);

private:
    void buildUi();
    void buildMenus();
    void buildLibraryFavoritesMenu();
    void showLibraryDialog(bool showFavorites);
    void wireSignals();
    // autoPlay=false is used when a loop clip is opened: the file has to finish
    // loading before the seek to A can be applied, so playback starts later.
    void loadMedia(const QString& mediaPath, const QString& selectedScript = {},
                   bool autoPlay = true);
    void updateStatus();
    void loadPersistedSettings();
    void savePersistedSettings();
    void applySavedAxisConfigs(ScriptBundle& bundle);
    void realignDeviceIfNeeded();
    void updateScriptPreview(qint64 positionMs);
    void clearAxisPositions();
    void sendAxisPreview(Track track, int scriptPercent);
    void updateResetAvailability();
    void toggleLibraryCollapsed();
    void toggleLoopListCollapsed();
    void updateLoopMarkerUi();
    // Applies the jump that had to wait for a freshly loaded file.
    void applyPendingLoopSeek();
    // Heat map scrubbing produces one seek request per mouse move. The requests
    // are coalesced (latest wins, ~40 ms cadence) so the decoder, the serial
    // link and the log are not driven at pointer speed; the last position of a
    // drag is always applied when the pointer is released.
    void requestSeek(qint64 positionMs);
    void applyPendingSeek();
    void cancelPendingSeek();
    // Scrubbing the heat map: the picture follows the pointer, the machine is
    // held still and only moves once the pointer is released.
    void beginScrub();
    void holdMachineForScrub();
    void finishScrub();
    void updateLoopButtonsEnabled();
    void updateLoopOverlay();
    void setupFullScreenControls();
    void setFullScreenControlsVisible(bool visible);
    void syncFullScreenControlsWithCursor();
    void hideIdleFullScreenOverlays();
    void setFullScreenCursorHidden(bool hidden);
    // Full screen edge panels: the media library and the loop list leave the
    // splitter and live in their own overlay hosts, sliding in from the screen
    // edges without resizing the picture.
    void applyFullScreenPanelHosting(bool fullScreen);
    void updateFullScreenPanelGeometry();
    void updateFullScreenPanelsForCursor();
    // Brings one full screen edge panel out / puts both of them away.
    void showFullScreenPanel(QWidget* host);
    void hideFullScreenPanels();
    // True while a dropdown opened from one of the side panels (收藏夹 / 排序 /
    // 标签 …) is on screen. Such a popup is a window of its own and lives outside
    // the panel's rectangle, so the panel has to stay put while it is open.
    bool fullScreenPanelPopupOpen() const;
    // Diagnostic helper for --fold-probe: one line describing the fold state of
    // both side panels plus the overlay hosts.
    QString foldStateLine(const QString& tag) const;
    void setFullScreenControlsFloating(bool floating);
    void positionFloatingPlayerBar();
    bool fullScreenControlZoneContainsCursor() const;
    void stopLoopPlayback(const QString& reason = QString());
    // Sequential clip playback: starts the next clip of the visible list.
    // Returns true when another clip took over.
    bool advanceToNextLoopClip();
    void refreshLoopPanelContext();
    void buildLoopMenu();
    void buildLoopFolderMenu();
    void buildLoopTagMenu();
    void showStatusMessage(const QString& text, int timeoutMs = 3000);
    void updateDeviceIndicator(bool connected);
    // Single entry point for the auto-next setting so the menu action, the
    // media library button and the persisted value never drift apart.
    void setAutoNextEnabled(bool enabled, bool persist = true);
    void updateUptime();
    // Play/pause easing has its own switch, set in 设置 › 跳转安全…; this keeps
    // the scheduler and the persisted value in step.
    void setPlayPauseEasingEnabled(bool enabled, bool persist = true);
    void setSwappedSides(bool swapped);
    // Relaunches the same executable and closes this instance through the normal
    // close path, so the device still gets its DSTOP and the link thread is shut
    // down before the new process takes over the port.
    void restartApplication();

    MpvEngine* m_engine = nullptr;
    // 0 = auto (mpv auto-safe), 1 = prefer hardware (mpv auto), 2 = software
    // (mpv no). Mirrors QSettings playback/hwdec; applied before mpv starts.
    int m_hwdecMode = 0;
    MpvVideoWidget* m_video = nullptr;
    QStackedWidget* m_videoStack = nullptr;
    QWidget* m_blackScreen = nullptr;
    QWidget* m_videoFrame = nullptr;
    QWidget* m_playerBar = nullptr;
    HeatmapWidget* m_heatmap = nullptr;
    // Facade over the device worker thread (serial port, handshake, scheduler).
    // Every call is a posted request and every getter reads a cached copy, so
    // the interface never waits for the device.
    DeviceLink* m_link = nullptr;
    MediaLibrary* m_library = nullptr;
    AxisLimitPanel* m_axisPanel = nullptr;
    LoopListPanel* m_loopPanel = nullptr;
    LoopStore* m_loopStore = nullptr;

    QAction* m_axisLimitAction = nullptr;
    QAction* m_tcodeDetailAction = nullptr;
    QAction* m_libraryAction = nullptr;
    QAction* m_fullScreenAction = nullptr;
    QAction* m_autoNextAction = nullptr;
    QAction* m_heatmapAction = nullptr;
    QAction* m_axisTagAction = nullptr;
    QAction* m_swapSidesAction = nullptr;
    QAction* m_loopListAction = nullptr;
    QAction* m_loopAutoNextAction = nullptr;
    QAction* m_escapeAction = nullptr;
    QMenu* m_settingsMenu = nullptr;
    QMenu* m_libraryMenu = nullptr;
    QMenu* m_libraryFavoritesMenu = nullptr;
    QMenu* m_librarySortMenu = nullptr;
    QMenu* m_viewThumbMenu = nullptr;
    QMenu* m_viewFontMenu = nullptr;
    QMenu* m_loopMenu = nullptr;
    QMenu* m_loopFolderMenu = nullptr;
    QMenu* m_loopTagMenu = nullptr;
    QMenu* m_loopSortMenu = nullptr;
    QAction* m_libraryOnlyScriptAction = nullptr;
    QLabel* m_statusDevice = nullptr;
    QLabel* m_statusScript = nullptr;
    QLabel* m_statusTime = nullptr;
    QLabel* m_messageLabel = nullptr;
    QLabel* m_volumeLabel = nullptr;
    QLabel* m_uptimeLabel = nullptr;
    QTimer* m_messageTimer = nullptr;
    QTimer* m_loopContextTimer = nullptr;
    QTimer* m_cursorIdleTimer = nullptr;
    QTimer* m_uptimeTimer = nullptr;
    // Window state to return to when leaving full screen. Restoring the exact
    // previous state matters: calling showNormal() on a window that was
    // maximized makes Windows restore it to its normal size first, which is a
    // second window state transition (measured ~300 ms of blocked event loop on
    // the user's machine) and loses the maximized layout.
    bool m_maximizedBeforeFullScreen = false;
    // Native full screen (Win32 style + SetWindowPos) instead of Qt's window
    // state machine: showFullScreen()/showMaximized() make Windows walk a
    // "restore to a small window, then maximize" path, which is visible as the
    // window shrinking for a moment and costs hundreds of milliseconds while mpv
    // re-configures its video chain. Escape hatch: QSettings ui/nativeFullscreen.
    bool m_useNativeFullScreen = true;
    bool m_nativeFullscreen = false;
    QRect m_nativeRestoreRect;
    qintptr m_savedWindowStyle = 0;
    QElapsedTimer m_uptimeClock;
    QList<QWidget*> m_cursorSources;
    QHash<QWidget*, QCursor> m_cursorBackup;
    bool m_cursorHidden = false;
    bool m_controlsFloating = false;
    QToolButton* m_deviceChip = nullptr;
    // Link quality chip: worst phase lag of the last statistics window.
    QLabel* m_statusLink = nullptr;
    QToolButton* m_playButton = nullptr;
    QToolButton* m_stopButton = nullptr;
    QToolButton* m_panicButton = nullptr;
    QToolButton* m_resetButton = nullptr;
    QToolButton* m_loopAButton = nullptr;
    QToolButton* m_loopBButton = nullptr;
    QToolButton* m_loopAddButton = nullptr;
    QToolButton* m_loopClearButton = nullptr;
    QSlider* m_volumeSlider = nullptr;
    QToolButton* m_volumeButton = nullptr;
    QSplitter* m_splitter = nullptr;
    QWidget* m_mediaColumn = nullptr;
    QWidget* m_librarySide = nullptr;
    QToolButton* m_libraryToggleButton = nullptr;
    SidePanelFold* m_libraryFold = nullptr;
    QWidget* m_loopSide = nullptr;
    // Overlay hosts for the two side panels while full screen. They are plain
    // children of the player column, created after the picture and the control
    // bar, so they are drawn above both - Qt composites the QOpenGLWidget into
    // the window, which makes this possible without a separate top level window
    // (and without the window juggling that made the panels flicker).
    QWidget* m_libraryOverlayHost = nullptr;
    QWidget* m_loopOverlayHost = nullptr;
    int m_libraryOverlayWidth = 430;
    int m_loopOverlayWidth = 360;
    // Which panel is currently pulled out at an edge (null while both are away).
    QWidget* m_fullScreenPanelVisible = nullptr;
    // Re-checks the pointer position a few times per second while full screen,
    // so the panels also appear when the last mouse move was swallowed by a
    // widget that does not report motion.
    QTimer* m_fullScreenPanelTimer = nullptr;
    QToolButton* m_loopToggleButton = nullptr;
    SidePanelFold* m_loopFold = nullptr;
    int m_libraryWidth = 430;
    bool m_libraryCollapsed = false;
    int m_loopWidth = 360;
    bool m_loopCollapsed = true;

    ScriptBundle m_bundle;
    QString m_mediaPath;
    // Script chosen for m_mediaPath, kept so the file can be re-opened after the
    // GL context (and with it mpv's video output) was rebuilt.
    QString m_mediaScript;
    // A load request that arrived before the video surface had a render context.
    // mpv cannot configure its video output without one, so the request waits and
    // only the newest one is kept.
    QString m_pendingMediaPath;
    QString m_pendingMediaScript;
    bool m_pendingMediaAutoPlay = true;
    QTimer* m_pendingMediaTimer = nullptr;
    QTimer* m_seekCoalesceTimer = nullptr;
    qint64 m_pendingSeekMs = -1;
    QElapsedTimer m_seekLogClock;
    qint64 m_seekRequestCount = 0;
    qint64 m_seekApplyCount = 0;
    int m_seekRequestsSinceApply = 0;
    bool m_scrubbing = false;
    // True once the machine was stopped for this scrub and has to be brought
    // back (and realigned) when the pointer is released.
    bool m_scrubHeldMachine = false;
    // Remaining full screen toggles of the --fullscreen-probe run.
    int m_fullScreenProbeRemaining = 0;
    QString m_lastMediaFolder;
    // Whether the port the user picked is a paired Bluetooth serial device, and
    // its cached name. Bluetooth links need their own retry timing and hints, and
    // the status line should say "蓝牙" instead of pretending it is a cable.
    bool m_lastPortIsBluetooth = false;
    QString m_lastPortBluetoothName;
    // True once the link statistics window has produced a sync figure; the chip
    // stays hidden until then so an idle session does not spend its width on "--".
    bool m_linkStatsSeen = false;
    // True while connectDevice() is retrying: the connection dialog reports the
    // state itself, and a modal error box would only interrupt the retries.
    bool m_connectingSerial = false;
    QString m_connectingPort;
    int m_connectRetriesLeft = 0;
    QElapsedTimer m_previewClock;
    QHash<int, qint64> m_lastPreviewAt;
    bool m_fullScreen = false;
    bool m_autoNext = true;
    bool m_swappedSides = false;
    bool m_heatmapVisible = true;
    bool m_axisTagVisible = true;
    int m_volume = 100;
    int m_volumeBeforeMute = 100;
    qint64 m_lastRenderFrames = -1;
    qint64 m_lastRenderRequests = -1;
    int m_renderStallTicks = 0;
    // How often a stalled picture was answered by re-opening the file, so a
    // video that simply has no video track cannot loop forever.
    int m_renderRecoveryAttempts = 0;
    // Rate limit for the GUI heartbeat log line.
    QElapsedTimer m_uiHeartbeatLogClock;
    qint64 m_loopAMs = -1;
    qint64 m_loopBMs = -1;
    // Position to jump to once the freshly loaded file reports its duration.
    qint64 m_pendingLoopSeekMs = -1;
    QString m_activeLoopClipId;
    qint64 m_activeLoopStartMs = -1;
    qint64 m_activeLoopEndMs = -1;
    bool m_loopRewinding = false;
    int m_loopRewindTicks = 0;
    bool m_autoNextBeforeLoop = true;
};
