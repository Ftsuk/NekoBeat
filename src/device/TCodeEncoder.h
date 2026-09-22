#pragma once

#include "core/Types.h"

#include <QList>
#include <QMap>
#include <QString>

struct AxisTarget
{
    Track track = Track::None;
    int posPercent = 50;
    qint64 intervalMs = 100;
};

class TCodeEncoder
{
public:
    static QString encodeLine(const QList<AxisTarget>& targets,
                              const QMap<int, AxisConfig>& configs);
    static QString stopCommand();
    // Script percentage that lands on the axis' configured home value, so a
    // reset follows a calibrated home position instead of a hard coded middle.
    static int percentForHome(const AxisConfig& config);

private:
    static int mapValue(int posPercent, const AxisConfig& config);
};
