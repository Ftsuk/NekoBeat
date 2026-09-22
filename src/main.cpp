#include "core/Loc.h"
#include "ui/MainWindow.h"

#include "core/AppLogger.h"
#include "core/LegacyMigration.h"
#include "device/DeviceSession.h"
#include "device/SerialTransport.h"
#include "media/MpvEngine.h"
#include "device/SerialPortScanner.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QIcon>
#include <QStringList>
#include <QTimer>

#include <cstdlib>

#ifndef NEKOBEAT_VERSION
#define NEKOBEAT_VERSION "0.0.0"
#endif

#ifndef NEKOBEAT_BUILD
#define NEKOBEAT_BUILD "unknown"
#endif

namespace
{
// A windowed build has no console, so Qt's own warnings (thread affinity, GL
// context creation, timers) would otherwise be lost. Routing them into the
// application log keeps a shutdown or rendering problem diagnosable instead of
// invisible, which matters for "the picture froze but the sound kept playing"
// reports.
void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    Q_UNUSED(context);
    QString level;
    switch (type)
    {
    case QtDebugMsg: level = QStringLiteral("debug"); break;
    case QtInfoMsg: level = QStringLiteral("info"); break;
    case QtWarningMsg: level = QStringLiteral("warning"); break;
    case QtCriticalMsg: level = QStringLiteral("critical"); break;
    case QtFatalMsg: level = QStringLiteral("fatal"); break;
    }
    AppLogger::log(QStringLiteral("qt"), QStringLiteral("%1: %2").arg(level, message));
    if (type == QtFatalMsg)
        std::abort();
}
}

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("NekoBeat"));
    application.setOrganizationName(QStringLiteral("NekoBeat"));
    application.setApplicationVersion(QStringLiteral(NEKOBEAT_VERSION));
    // Renaming the product must not wipe what the user had: settings, axis
    // limits, media folders, favourites, loop clips and the thumbnail cache all
    // live under the old name. This runs before any store is created, and the
    // notes are logged once the logger exists.
    const QStringList migrationNotes = LegacyMigration::run();
    // The interface language has to be settled before anything is built: every
    // user-visible string is looked up at construction time. Default is English.
    Loc::loadFromSettings();
    // 品牌名不翻译：两种语言下都叫 NekoBeat。
    application.setApplicationDisplayName(QStringLiteral("NekoBeat"));
    application.setWindowIcon(QIcon(QStringLiteral(":/icons/NekoBeat.png")));
    AppLogger::init();
    qInstallMessageHandler(qtMessageHandler);
    AppLogger::log(QStringLiteral("app"), QStringLiteral("=== SESSION START ==="));
    for (const QString& note : migrationNotes)
        AppLogger::log(QStringLiteral("app"), note);
    AppLogger::log(QStringLiteral("app"),
                   QStringLiteral("exe=%1 | cwd=%2 | version=%3 | build=%4 | log=%5")
                       .arg(application.applicationFilePath(),
                            QDir::currentPath(),
                            application.applicationVersion(),
                            QStringLiteral(NEKOBEAT_BUILD),
                            AppLogger::logFilePath()));

    if (application.arguments().contains(QStringLiteral("--self-test")))
    {
        MpvEngine engine;
        return engine.initialize() ? 0 : 2;
    }

    // Diagnostic: list every serial port with its real state (available /
    // occupied / driver problem) and exit. The connection dialog shows the same
    // information, but this can be run without touching the UI.
    if (application.arguments().contains(QStringLiteral("--serial-scan")))
    {
        const QList<SerialPortEntry> entries = SerialPortScanner::scan();
        for (const SerialPortEntry& entry : entries)
        {
            AppLogger::log(
                QStringLiteral("serial"),
                QStringLiteral("扫描结果: %1 | %2 | %3 | %4%5")
                    .arg(entry.portName,
                         SerialPortScanner::statusText(entry.status),
                         entry.description,
                         entry.detail,
                         entry.isBluetooth
                             ? QStringLiteral(" | 蓝牙%1")
                                   .arg(entry.bluetoothOutbound
                                            ? QStringLiteral("出站")
                                            : QStringLiteral("入站"))
                             : QString()));
        }
        AppLogger::shutdown();
        return entries.isEmpty() ? 1 : 0;
    }

    const int serialTestIndex = application.arguments().indexOf(QStringLiteral("--serial-selftest"));
    if (serialTestIndex >= 0 && serialTestIndex + 1 < application.arguments().size())
    {
        SerialTransport transport;
        DeviceSession session(&transport);
        QEventLoop loop;
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        if (!session.connectToPort(application.arguments().at(serialTestIndex + 1)))
            return 3;
        loop.exec();
        const bool valid = session.capabilities().valid;
        session.disconnectFromDevice();
        return valid ? 0 : 4;
    }

    Theme::apply(application);

    const QStringList arguments = application.arguments();
    // Scoped so the window (and with it the video widget and the mpv handle) is
    // destroyed while the application object is still alive and before the log
    // is closed; a QOpenGLWidget that outlives QApplication is a crash waiting
    // to happen on Windows.
    int result = 0;
    {
        MainWindow window;

        const int libraryIndex = arguments.indexOf(QStringLiteral("--library"));
        if (libraryIndex >= 0 && libraryIndex + 1 < arguments.size())
            window.addLibraryFolder(arguments.at(libraryIndex + 1));

        const int mediaIndex = arguments.indexOf(QStringLiteral("--media"));
        if (mediaIndex >= 0 && mediaIndex + 1 < arguments.size())
        {
            const int scriptIndex = arguments.indexOf(QStringLiteral("--script"));
            const QString script = (scriptIndex >= 0 && scriptIndex + 1 < arguments.size())
                                       ? arguments.at(scriptIndex + 1)
                                       : QString();
            window.openMedia(arguments.at(mediaIndex + 1), script);
        }
        window.show();
        // Diagnostic:进出全屏若干组，走的是双击画面/F11 同一条路径。
        const int fullScreenProbeIndex = arguments.indexOf(QStringLiteral("--fullscreen-probe"));
        if (fullScreenProbeIndex >= 0)
        {
            const int rounds = (fullScreenProbeIndex + 1 < arguments.size())
                                   ? arguments.at(fullScreenProbeIndex + 1).toInt()
                                   : 0;
            window.runFullScreenProbe(rounds > 0 ? rounds : 3);
        }
        // Diagnostic: sample the media clock and the render counters without
        // touching anything else. Results land in the log as "[probe] ...".
        const int foldProbeIndex = arguments.indexOf(QStringLiteral("--fold-probe"));
        if (foldProbeIndex >= 0)
        {
            const int steps = (foldProbeIndex + 1 < arguments.size())
                                  ? arguments.at(foldProbeIndex + 1).toInt()
                                  : 0;
            window.runFoldProbe(steps > 0 ? steps : 14);
        }
        const int overlayProbeIndex = arguments.indexOf(QStringLiteral("--overlay-probe"));
        if (overlayProbeIndex >= 0)
        {
            const int rounds = (overlayProbeIndex + 1 < arguments.size())
                                   ? arguments.at(overlayProbeIndex + 1).toInt()
                                   : 0;
            window.runOverlayProbe(rounds > 0 ? rounds : 3);
        }
        const int clickProbeIndex = arguments.indexOf(QStringLiteral("--click-probe"));
        if (clickProbeIndex >= 0)
        {
            const int seconds = (clickProbeIndex + 1 < arguments.size())
                                    ? arguments.at(clickProbeIndex + 1).toInt()
                                    : 0;
            window.runClickProbe(seconds > 0 ? seconds : 20);
        }
        const int probeIndex = arguments.indexOf(QStringLiteral("--playback-probe"));
        if (probeIndex >= 0)
        {
            const int seconds = (probeIndex + 1 < arguments.size())
                                    ? arguments.at(probeIndex + 1).toInt()
                                    : 0;
            window.runPlaybackProbe(seconds > 0 ? seconds : 12);
        }
        const int scrubProbeIndex = arguments.indexOf(QStringLiteral("--scrub-probe"));
        if (scrubProbeIndex >= 0)
        {
            const int moves = (scrubProbeIndex + 1 < arguments.size())
                                  ? arguments.at(scrubProbeIndex + 1).toInt()
                                  : 0;
            // Wait for the file to report its duration before scrubbing.
            QTimer::singleShot(2500, &window,
                               [&window, moves] { window.runScrubProbe(moves > 0 ? moves : 200); });
        }
        if (arguments.contains(QStringLiteral("--simulate-device-load")))
            window.startDeviceLoadSimulation();
        result = application.exec();
        AppLogger::log(QStringLiteral("app"), QStringLiteral("=== SESSION END ==="));
    }
    AppLogger::log(QStringLiteral("app"), QStringLiteral("主窗口已析构"));
    AppLogger::shutdown();
    return result;
}
