#pragma once

#include <QString>

// Clock-style formatting shared by the player bar, the loop markers and the
// media library cards. QTime must not be used for this: it represents a time of
// day, so "mm" wraps at 60 and a 1 h 20 min video would render as "20:00".
namespace DurationFormat
{
// "mm:ss" below one hour, "h:mm:ss" from one hour on. Negative input is
// clamped to zero, so a fresh session reads "00:00" instead of something odd.
QString clock(qint64 milliseconds);
}
