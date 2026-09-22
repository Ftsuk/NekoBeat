#pragma once

#include <QString>

// Small helpers shared by the stores that remember media by path. Keeping the
// normalisation in one place means a favourite and a loop clip always agree on
// whether two paths point at the same file.
namespace PathUtils
{
// Normalised comparison key for media paths (lower case, clean separators).
// Returns an empty string for empty input.
QString normalizeMediaPath(const QString& path);
}
