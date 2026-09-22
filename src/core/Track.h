#pragma once

#include <QMetaType>
#include <QString>
#include <optional>
#include <vector>

enum class Track
{
    Stroke,
    Surge,
    Sway,
    Twist,
    Roll,
    Pitch,
    Vib,
    Lube,
    Suck,
    SuckPosition,
    None
};

// Needed for queued (cross-thread) signals: the scheduler lives in the device
// worker thread while the interface lives on the GUI thread.
Q_DECLARE_METATYPE(Track)

namespace TrackInfo
{
// Position axes are told "where to be"; amplitude axes (vibration, lube, pump)
// are told "how strong". Going home means the middle of the travel for the
// first kind and off for the second.
enum class AxisKind
{
    Position,
    Amplitude
};

std::optional<Track> fromIdentifier(const QString& identifier);
QString tcodeId(Track track);
QString canonicalName(Track track);
QString displayName(Track track);
bool isSupported(Track track);
AxisKind kind(Track track);
std::vector<Track> sr6Tracks();
}
