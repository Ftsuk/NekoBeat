#pragma once

#include <QList>
#include <QString>

// One COM port as the connection dialog sees it. The status is what the user
// actually needs: "the device is plugged in but nothing can open it" is a very
// different problem from "the driver never loaded", and the plain
// QSerialPortInfo list cannot tell them apart - which is why "the port is right
// there in Device Manager but the app cannot find it" used to be a dead end.
struct SerialPortEntry
{
    enum class Status
    {
        Available,    // opened exclusively and closed again right away
        Busy,         // exists but another program holds it (access denied)
        DriverProblem,// present in the device tree, but not usable as a port
        Unavailable   // listed somewhere, but cannot be opened at all
    };

    QString portName;
    QString description;
    QString manufacturer;
    Status status = Status::Unavailable;
    QString detail;
    // True when only the registry/devices list knows about it (Qt did not).
    bool extraSource = false;

    // Windows maps a paired Bluetooth serial device (SPP) onto a virtual COM
    // port, so a wireless link shows up as an ordinary port. Without the fields
    // below the picker can only offer two identical "standard serial over
    // Bluetooth link" rows, and nothing tells the user which one is the device
    // and which one is this PC's own inbound port.
    bool isBluetooth = false;
    // False for the inbound port Windows creates for the same service: nothing
    // is behind it, so it must never be offered as a connection target.
    bool bluetoothOutbound = false;
    QString bluetoothName;
    QString bluetoothAddress;
};

namespace SerialPortScanner
{
// Merges QSerialPortInfo, the Windows port class devices and the SERIALCOMM
// registry key, then probes every candidate with an exclusive open. Probing can
// reset CH340/ESP32 style adapters once, exactly like connecting does.
QList<SerialPortEntry> scan();

// Human readable label for the status column.
QString statusText(SerialPortEntry::Status status);

// Bluetooth details for one port, without probing (opening) it. The connect
// path uses this to give a Bluetooth link its own retry timing and hints.
// Returns false when the port is not a paired Bluetooth serial port.
bool bluetoothInfo(const QString& portName, SerialPortEntry* entry);
}
