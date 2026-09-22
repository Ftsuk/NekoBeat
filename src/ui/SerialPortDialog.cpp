#include "core/Loc.h"
#include "ui/SerialPortDialog.h"

#include "device/SerialPortScanner.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QColor>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>

SerialPortDialog::SerialPortDialog(const QString& preferredPort, QWidget* parent)
    : QDialog(parent)
    , m_preferredPort(preferredPort)
{
    setWindowTitle(LT("连接 SR6"));
    // Windows paints a fresh window in the system background colour until the
    // widget draws for the first time, which shows up as a white flash on a dark
    // theme. Setting the palette here means the very first frame is already dark.
    QPalette windowPalette = palette();
    windowPalette.setColor(QPalette::Window, QColor(0x1C, 0x1C, 0x1E));
    setPalette(windowPalette);
    setAutoFillBackground(true);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(10);

    auto* hint = new QLabel(
        LT("选一个端口。“状态”会说明它能不能用：被其他程序占用时可以点“强制释放”先放开本程序仍持有的句柄，再重试。\n蓝牙设备在 Windows 里配对后会出现在这里，并标出配对时的设备名（例如“蓝牙串口 · SR6-BT”）；只列出真正能连的那个蓝牙口，系统多余的“入站端口”不会显示。"),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(3);
    m_table->setHorizontalHeaderLabels(
        {LT("端口"), LT("描述"), LT("状态")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->setMinimumSize(620, 300);
    layout->addWidget(m_table, 1);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    auto* buttons = new QHBoxLayout();
    auto* rescanButton = new QPushButton(LT("重新扫描"), this);
    auto* releaseButton = new QPushButton(LT("强制释放"), this);
    releaseButton->setToolTip(
        LT("关闭本程序仍持有的串口句柄并重新检测（不会动其它程序）"));
    m_connectButton = new QPushButton(LT("连接"), this);
    m_connectButton->setDefault(true);
    auto* cancelButton = new QPushButton(LT("取消"), this);
    buttons->addWidget(rescanButton);
    buttons->addWidget(releaseButton);
    buttons->addStretch(1);
    buttons->addWidget(m_connectButton);
    buttons->addWidget(cancelButton);
    layout->addLayout(buttons);

    connect(rescanButton, &QPushButton::clicked, this, &SerialPortDialog::refresh);
    connect(releaseButton, &QPushButton::clicked, this, &SerialPortDialog::releaseRequested);
    connect(m_connectButton, &QPushButton::clicked, this, &SerialPortDialog::acceptSelection);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_table, &QTableWidget::cellDoubleClicked, this,
            [this](int, int) { acceptSelection(); });

    refresh();
}

void SerialPortDialog::refresh()
{
    if (m_scanRunning)
        return;
    m_scanRunning = true;
    m_summary->setText(LT("正在检测端口…"));
    m_table->setRowCount(0);
    // Connecting needs a port from the table, so it waits for the scan.
    if (m_connectButton)
        m_connectButton->setEnabled(false);

    // Probing means opening every port for a moment, and a Bluetooth port has to
    // bring its radio link up first - a second or two. On the GUI thread that
    // froze the dialog on its first (white) frame; running it on a worker keeps
    // the window responsive and lets the table fill in when the answers arrive.
    if (!m_watcher)
    {
        m_watcher = new QFutureWatcher<QList<SerialPortEntry>>(this);
        connect(m_watcher, &QFutureWatcherBase::finished, this,
                &SerialPortDialog::applyScanResults);
    }
    m_watcher->setFuture(QtConcurrent::run(&SerialPortScanner::scan));
}

void SerialPortDialog::applyScanResults()
{
    m_scanRunning = false;
    if (m_connectButton)
        m_connectButton->setEnabled(true);

    // Every paired Bluetooth serial device also owns an inbound port. Nothing is
    // behind it - it is this PC offering a service, not a cable to the device - and
    // it is labelled exactly like the outbound one, so listing it only invites
    // picking the wrong row. It is dropped here rather than in the scanner, so
    // `--serial-scan` can still report it when the link itself is being diagnosed.
    QList<SerialPortEntry> entries;
    for (const SerialPortEntry& entry : m_watcher->result())
    {
        if (entry.isBluetooth && !entry.bluetoothOutbound)
            continue;
        entries.append(entry);
    }

    m_table->setRowCount(entries.size());
    int availableCount = 0;
    int busyCount = 0;
    int problemCount = 0;
    QStringList bluetoothNames;
    int preferredRow = -1;
    int firstUsableRow = -1;

    for (int row = 0; row < entries.size(); ++row)
    {
        const SerialPortEntry& entry = entries.at(row);
        auto* portItem = new QTableWidgetItem(entry.portName);
        portItem->setData(Qt::UserRole, entry.portName);
        // The inbound Bluetooth row exists for every paired device but reaches
        // nothing, so it is flagged here and refused in acceptSelection().
        portItem->setData(Qt::UserRole + 1,
                          entry.isBluetooth && !entry.bluetoothOutbound);
        // Only paired devices are worth naming in the summary; the inbound port
        // has no device behind it and would just repeat the word "蓝牙".
        if (entry.isBluetooth && !entry.bluetoothName.isEmpty())
        {
            const QString label = LT("蓝牙 · %1").arg(entry.bluetoothName);
            if (!bluetoothNames.contains(label))
                bluetoothNames.append(label);
        }
        m_table->setItem(row, 0, portItem);

        QString description = entry.description;
        if (description.isEmpty())
            description = entry.manufacturer;
        if (entry.extraSource && entry.status != SerialPortEntry::Status::DriverProblem)
            description += LT("（注册表条目）");
        auto* descriptionItem = new QTableWidgetItem(description);
        descriptionItem->setToolTip(entry.detail);
        m_table->setItem(row, 1, descriptionItem);

        QString status = SerialPortScanner::statusText(entry.status);
        if (!entry.detail.isEmpty())
            status += QStringLiteral("（%1）").arg(entry.detail);
        auto* statusItem = new QTableWidgetItem(status);
        switch (entry.status)
        {
        case SerialPortEntry::Status::Available:
            statusItem->setForeground(QColor(0x30, 0xD1, 0x58));
            ++availableCount;
            if (firstUsableRow < 0)
                firstUsableRow = row;
            break;
        case SerialPortEntry::Status::Busy:
            statusItem->setForeground(QColor(0xFF, 0x9F, 0x0A));
            ++busyCount;
            break;
        case SerialPortEntry::Status::DriverProblem:
            statusItem->setForeground(QColor(0xFF, 0x45, 0x3A));
            ++problemCount;
            break;
        case SerialPortEntry::Status::Unavailable:
            statusItem->setForeground(QColor(0x8E, 0x8E, 0x93));
            break;
        }
        m_table->setItem(row, 2, statusItem);

        if (!entry.portName.isEmpty() && entry.portName == m_preferredPort)
            preferredRow = row;
    }

    const int rowToSelect = preferredRow >= 0 ? preferredRow : firstUsableRow;
    if (rowToSelect >= 0)
        m_table->selectRow(rowToSelect);

    QString summary;
    if (entries.isEmpty())
    {
        summary = LT("没有检测到任何串口。请确认设备已插好、驱动已安装，然后点“重新扫描”；如果蓝牙设备已经配对却一直不出现在这里，说明它的蓝牙不是串口类型，本程序无法通过蓝牙连接它。");
    }
    else
    {
        summary = LT("共 %1 项：可用 %2、被占用 %3、驱动异常 %4。")
                      .arg(entries.size())
                      .arg(availableCount)
                      .arg(busyCount)
                      .arg(problemCount);
        if (!bluetoothNames.isEmpty())
        {
            summary += LT(" 蓝牙：%1。")
                           .arg(bluetoothNames.join(QStringLiteral("、")));
        }
        if (availableCount == 0 && busyCount > 0)
            summary += LT(" 端口被占用：关闭串口助手 / Arduino IDE 之类的程序，或先点“强制释放”。");
        else if (availableCount == 0 && problemCount > 0)
            summary += LT(" 设备在设备管理器里显示为黄色感叹号时，重新安装 USB 串口驱动后点“重新扫描”。");
    }
    m_summary->setText(summary);
    m_selectedPort.clear();
}

void SerialPortDialog::acceptSelection()
{
    const int row = m_table->currentRow();
    if (row < 0)
        return;
    QTableWidgetItem* item = m_table->item(row, 0);
    if (!item)
        return;

    // Every paired Bluetooth device also owns an inbound port. Choosing it can
    // only produce a silent, confusing failure, so say so instead.
    if (item->data(Qt::UserRole + 1).toBool())
    {
        const QString name = item->data(Qt::UserRole).toString();
        QMessageBox::information(
            this, LT("这个端口连不上设备"),
            LT("“%1”是本机蓝牙的入站端口，后面没有设备。\n\n请改选同一个设备名下的另一个蓝牙串口（标着“蓝牙串口 · 设备名”的那个）。")
                .arg(name.isEmpty() ? item->text() : name));
        return;
    }

    m_selectedPort = item->data(Qt::UserRole).toString();
    if (m_selectedPort.isEmpty())
        m_selectedPort = item->text();
    if (!m_selectedPort.isEmpty())
        accept();
}

int SerialPortDialog::rowForPort(const QString& portName) const
{
    for (int row = 0; row < m_table->rowCount(); ++row)
    {
        if (QTableWidgetItem* item = m_table->item(row, 0))
        {
            const QString candidate = item->data(Qt::UserRole).toString();
            if ((candidate.isEmpty() ? item->text() : candidate) == portName)
                return row;
        }
    }
    return -1;
}
