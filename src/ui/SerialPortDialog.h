#pragma once

#include "device/SerialPortScanner.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QString>

class QLabel;
class QTableWidget;
class QPushButton;

// Port picker for "connect SR6". Unlike the plain text list it used to be, this
// one shows what state every port is really in - because "the port is right
// there but the app cannot see it" had three very different causes (occupied,
// driver not started, or a stale registry entry) and the old dialog could not
// tell them apart.
class SerialPortDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SerialPortDialog(const QString& preferredPort, QWidget* parent = nullptr);

    // Valid after the dialog was accepted.
    QString selectedPort() const { return m_selectedPort; }

public slots:
    void refresh();

signals:
    // The user asked to release whatever this program still holds. The window
    // performs the actual release and then refreshes this dialog.
    void releaseRequested();

private:
    // Runs on the GUI thread once the worker finished probing the ports.
    void applyScanResults();
    void acceptSelection();
    int rowForPort(const QString& portName) const;

    QTableWidget* m_table = nullptr;
    QLabel* m_summary = nullptr;
    QPushButton* m_connectButton = nullptr;
    QFutureWatcher<QList<SerialPortEntry>>* m_watcher = nullptr;
    QString m_preferredPort;
    QString m_selectedPort;
    bool m_scanRunning = false;
};
