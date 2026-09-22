#include "core/Loc.h"
#include "device/SerialPortScanner.h"

#include "core/AppLogger.h"

#include <QMap>
#include <QSerialPortInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#endif

namespace
{
#ifdef Q_OS_WIN
// Tries to take the port exclusively for the shortest possible moment. Success
// means nobody else holds it; ERROR_ACCESS_DENIED means somebody does.
SerialPortEntry::Status probePort(const QString& portName, QString* detail)
{
    const QString path = QStringLiteral("\\\\.\\") + portName;
    const std::wstring wide = path.toStdWString();
    HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(handle);
        if (detail)
            *detail = LT("可以打开");
        return SerialPortEntry::Status::Available;
    }

    const DWORD error = GetLastError();
    switch (error)
    {
    case ERROR_ACCESS_DENIED:
        if (detail)
            *detail = LT("被其他程序占用（拒绝访问）");
        return SerialPortEntry::Status::Busy;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        if (detail)
            *detail = LT("系统里没有这个端口");
        return SerialPortEntry::Status::Unavailable;
    default:
        if (detail)
            *detail = LT("无法打开（错误 %1）").arg(error);
        return SerialPortEntry::Status::Unavailable;
    }
}

// Devices that exist in the port class but do not show up as a usable COM port -
// usually a driver that failed to start (the yellow exclamation mark in Device
// Manager). Without this the app can only say "no serial port found".
QList<QPair<QString, QString>> devicesWithoutPortName()
{
    QList<QPair<QString, QString>> result;
    HDEVINFO deviceInfo = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr,
                                               DIGCF_PRESENT);
    if (deviceInfo == INVALID_HANDLE_VALUE)
        return result;

    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(SP_DEVINFO_DATA);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(deviceInfo, index, &data); ++index)
    {
        wchar_t friendly[256] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(deviceInfo, &data, SPDRP_FRIENDLYNAME,
                                               nullptr,
                                               reinterpret_cast<PBYTE>(friendly),
                                               sizeof(friendly), nullptr))
        {
            continue;
        }
        const QString name = QString::fromWCharArray(friendly);

        // Devices that already carry a COM name are normal ports and are listed
        // from the Qt/registry side; only the ones without a usable name are the
        // "device is there but the port never came up" case.
        static const QRegularExpression comInName(
            QStringLiteral("\\(COM\\d+\\)|^COM\\d+$"),
            QRegularExpression::CaseInsensitiveOption);
        if (comInName.match(name).hasMatch())
            continue;

        ULONG status = 0;
        ULONG problem = 0;
        const bool hasProblem = CM_Get_DevNode_Status(&status, &problem, data.DevInst, 0)
                                    == CR_SUCCESS
                                && problem != 0;
        result.append({name, hasProblem
                                 ? LT("驱动异常（问题代码 %1）").arg(problem)
                                 : LT("设备未提供端口名")});
    }
    SetupDiDestroyDeviceInfoList(deviceInfo);
    return result;
}

QStringList registryPorts()
{
    QStringList ports;
    QSettings registry(QStringLiteral("HKEY_LOCAL_MACHINE\\HARDWARE\\DEVICEMAP\\SERIALCOMM"),
                       QSettings::NativeFormat);
    for (const QString& key : registry.allKeys())
    {
        const QString value = registry.value(key).toString();
        if (!value.isEmpty())
            ports.append(value);
    }
    return ports;
}

QString formatBluetoothAddress(const QString& address)
{
    QStringList parts;
    for (int i = 0; i + 1 < address.size(); i += 2)
        parts.append(address.mid(i, 2));
    return parts.join(QLatin1Char(':'));
}

// The cached name Windows keeps for a paired device. The port itself is only
// ever called "standard serial over Bluetooth link", so this is the only way to
// tell the user that the port in front of them is their SR6 and not some other
// gadget that happens to be paired.
QString bluetoothDeviceName(const QString& addressLower)
{
    if (addressLower.isEmpty())
        return QString();
    const QString subKey =
        QStringLiteral("SYSTEM\\CurrentControlSet\\Services\\BTHPORT\\Parameters\\Devices\\")
        + addressLower;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      reinterpret_cast<const wchar_t*>(subKey.utf16()), 0, KEY_READ,
                      &key) != ERROR_SUCCESS)
    {
        return QString();
    }

    wchar_t raw[512] = {};
    DWORD size = sizeof(raw);
    DWORD type = 0;
    QString name;
    if (RegQueryValueExW(key, L"Name", nullptr, &type, reinterpret_cast<LPBYTE>(raw),
                         &size) == ERROR_SUCCESS)
    {
        // The cached name is stored as UTF-8 bytes, usually NUL terminated.
        if (type == REG_BINARY)
            name = QString::fromUtf8(reinterpret_cast<const char*>(raw),
                                     static_cast<int>(size));
        else if (type == REG_SZ)
            name = QString::fromWCharArray(raw);
        name.remove(QChar(0));
        name = name.trimmed();
    }
    RegCloseKey(key);
    return name;
}

// Every paired Bluetooth serial device shows up twice in the port class: once
// as the outbound port that reaches the device and once as the inbound port
// this PC serves. Their device instance IDs look like
//   BTHENUM\{00001101-...-00805F9B34FB}_LOCALMFG&0002\7&601D955&0&1C69202C1E56_C00000000
// where the trailing token is the device address; the all-zero variant is the
// inbound port, which has no device behind it.
QMap<QString, SerialPortEntry> bluetoothPorts()
{
    QMap<QString, SerialPortEntry> result;
    HDEVINFO deviceInfo = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr,
                                               DIGCF_PRESENT);
    if (deviceInfo == INVALID_HANDLE_VALUE)
        return result;

    static const QRegularExpression comInName(QStringLiteral("\\((COM\\d+)\\)"),
                                              QRegularExpression::CaseInsensitiveOption);

    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(SP_DEVINFO_DATA);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(deviceInfo, index, &data); ++index)
    {
        wchar_t instanceId[512] = {};
        if (!SetupDiGetDeviceInstanceIdW(deviceInfo, &data, instanceId,
                                         static_cast<DWORD>(sizeof(instanceId)
                                                            / sizeof(instanceId[0])),
                                         nullptr))
        {
            continue;
        }
        const QString id = QString::fromWCharArray(instanceId);
        if (!id.startsWith(QStringLiteral("BTHENUM\\"), Qt::CaseInsensitive))
            continue;

        // The friendly name is what carries the COM number.
        wchar_t friendly[256] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(deviceInfo, &data, SPDRP_FRIENDLYNAME,
                                               nullptr, reinterpret_cast<PBYTE>(friendly),
                                               sizeof(friendly), nullptr))
        {
            continue;
        }
        const QRegularExpressionMatch comMatch =
            comInName.match(QString::fromWCharArray(friendly));
        if (!comMatch.hasMatch())
            continue;  // paired Bluetooth device without a serial port behind it

        const QString tail = id.section(QLatin1Char('\\'), -1);
        const QString address = tail.section(QLatin1Char('_'), 0, 0)
                                    .section(QLatin1Char('&'), -1)
                                    .toUpper();

        SerialPortEntry entry;
        entry.portName = comMatch.captured(1).toUpper();
        entry.isBluetooth = true;
        entry.bluetoothOutbound =
            address.size() == 12 && address != QStringLiteral("000000000000");
        if (entry.bluetoothOutbound)
        {
            entry.bluetoothAddress = formatBluetoothAddress(address);
            entry.bluetoothName = bluetoothDeviceName(address.toLower());
        }
        result.insert(entry.portName, entry);
    }
    SetupDiDestroyDeviceInfoList(deviceInfo);
    return result;
}
#endif
}

QString SerialPortScanner::statusText(SerialPortEntry::Status status)
{
    switch (status)
    {
    case SerialPortEntry::Status::Available:
        return LT("可用");
    case SerialPortEntry::Status::Busy:
        return LT("被占用");
    case SerialPortEntry::Status::DriverProblem:
        return LT("驱动异常");
    case SerialPortEntry::Status::Unavailable:
        break;
    }
    return LT("不可用");
}

QList<SerialPortEntry> SerialPortScanner::scan()
{
    QList<SerialPortEntry> entries;
    QSet<QString> seen;

    QMap<QString, SerialPortEntry> bluetoothByPort;
#ifdef Q_OS_WIN
    bluetoothByPort = bluetoothPorts();
#endif

    const auto appendEntry = [&entries, &seen, &bluetoothByPort](SerialPortEntry entry) {
        if (entry.portName.isEmpty() || seen.contains(entry.portName))
            return;
        // A Bluetooth serial port looks like any other COM port to Qt, so its
        // real identity is filled in here: the paired device's name for the
        // outbound port, and "nothing behind it" for the inbound one.
        const auto bluetooth = bluetoothByPort.constFind(entry.portName.toUpper());
        if (bluetooth != bluetoothByPort.constEnd())
        {
            entry.isBluetooth = true;
            entry.bluetoothOutbound = bluetooth->bluetoothOutbound;
            entry.bluetoothName = bluetooth->bluetoothName;
            entry.bluetoothAddress = bluetooth->bluetoothAddress;
            if (bluetooth->bluetoothOutbound)
            {
                entry.description =
                    bluetooth->bluetoothName.isEmpty()
                        ? LT("蓝牙串口（已配对设备）")
                        : LT("蓝牙串口 · %1").arg(bluetooth->bluetoothName);
                if (!bluetooth->bluetoothAddress.isEmpty())
                {
                    entry.detail =
                        LT("蓝牙地址 %1").arg(bluetooth->bluetoothAddress);
                }
            }
            else
            {
                entry.description = LT("蓝牙串口（本机入站端口）");
                entry.status = SerialPortEntry::Status::Unavailable;
                entry.detail = LT("后面没有设备，连不上；请选带设备名的那个蓝牙串口");
            }
        }
        seen.insert(entry.portName);
        entries.append(entry);
    };

    // 1. What Qt can see (the port name and description it reports).
    for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts())
    {
        SerialPortEntry entry;
        entry.portName = info.portName();
        entry.description = info.description();
        entry.manufacturer = info.manufacturer();
#ifdef Q_OS_WIN
        entry.status = probePort(entry.portName, &entry.detail);
#else
        entry.status = SerialPortEntry::Status::Available;
        entry.detail = LT("Qt 枚举到（未探测）");
#endif
        appendEntry(entry);
    }

#ifdef Q_OS_WIN
    // 2. Ports only the registry knows about (some virtual adapters do not reach
    //    Qt's device enumeration).
    for (const QString& portName : registryPorts())
    {
        if (seen.contains(portName))
            continue;
        SerialPortEntry entry;
        entry.portName = portName;
        entry.description = LT("注册表记录的串口");
        entry.extraSource = true;
        entry.status = probePort(portName, &entry.detail);
        appendEntry(entry);
    }

    // 3. Devices in the port class that never became a usable COM port.
    for (const auto& device : devicesWithoutPortName())
    {
        SerialPortEntry entry;
        entry.portName = device.first;
        entry.description = LT("设备存在，但没有可用的 COM 名称");
        entry.detail = device.second;
        entry.status = SerialPortEntry::Status::DriverProblem;
        entry.extraSource = true;
        // These have no COM name, so the duplicate check uses the device name.
        if (seen.contains(entry.portName))
            continue;
        seen.insert(entry.portName);
        entries.append(entry);
    }
#endif

    // Ports the user can actually pick come first: a dead row listed above a
    // live one is how "it refused to connect" starts.
    const auto rank = [](SerialPortEntry::Status status) {
        switch (status)
        {
        case SerialPortEntry::Status::Available:
            return 0;
        case SerialPortEntry::Status::Busy:
            return 1;
        case SerialPortEntry::Status::DriverProblem:
            return 2;
        case SerialPortEntry::Status::Unavailable:
            break;
        }
        return 3;
    };
    std::stable_sort(entries.begin(), entries.end(),
                     [&rank](const SerialPortEntry& a, const SerialPortEntry& b) {
                         return rank(a.status) < rank(b.status);
                     });

    for (const SerialPortEntry& entry : entries)
    {
        AppLogger::log(QStringLiteral("serial"),
                       QStringLiteral("端口扫描: %1 | %2 | %3 | %4%5")
                           .arg(entry.portName,
                                statusText(entry.status),
                                entry.description,
                                entry.detail,
                                entry.isBluetooth
                                    ? QStringLiteral(" | 蓝牙%1")
                                          .arg(entry.bluetoothOutbound
                                                   ? QStringLiteral("出站")
                                                   : QStringLiteral("入站"))
                                    : QString()));
    }
    return entries;
}

bool SerialPortScanner::bluetoothInfo(const QString& portName, SerialPortEntry* entry)
{
    if (entry)
        *entry = SerialPortEntry();
#ifdef Q_OS_WIN
    const QMap<QString, SerialPortEntry> ports = bluetoothPorts();
    const auto it = ports.constFind(portName.toUpper());
    if (it == ports.constEnd())
        return false;
    if (entry)
        *entry = it.value();
    return true;
#else
    Q_UNUSED(portName);
    return false;
#endif
}
