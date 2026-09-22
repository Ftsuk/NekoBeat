#include "device/TCodeEncoder.h"

#include <algorithm>

QString TCodeEncoder::encodeLine(const QList<AxisTarget>& targets,
                                 const QMap<int, AxisConfig>& configs)
{
    QStringList parts;
    for (const AxisTarget& target : targets)
    {
        const QString id = TrackInfo::tcodeId(target.track);
        if (id.isEmpty())
            continue;

        const auto it = configs.constFind(static_cast<int>(target.track));
        const AxisConfig config = it != configs.constEnd() ? it.value() : AxisConfig{};
        // TCode positions are four digits. A hand-edited config could otherwise
        // emit a five-digit value, which the firmware would misparse.
        const int value = std::clamp(mapValue(target.posPercent, config), 0, 9999);
        const qint64 interval = std::max<qint64>(10, target.intervalMs);

        parts.append(QStringLiteral("%1%2I%3")
                         .arg(id)
                         .arg(value, 4, 10, QLatin1Char('0'))
                         .arg(interval));
    }
    return parts.join(QLatin1Char(' '));
}

QString TCodeEncoder::stopCommand()
{
    return QStringLiteral("DSTOP");
}

int TCodeEncoder::percentForHome(const AxisConfig& config)
{
    const int minimum = std::min(config.min, config.max);
    const int maximum = std::max(config.min, config.max);
    if (maximum <= minimum)
        return 50;

    const double ratio =
        (config.home - minimum) / static_cast<double>(maximum - minimum);
    const int percent =
        config.inverted ? qRound((1.0 - ratio) * 100.0) : qRound(ratio * 100.0);
    return std::clamp(percent, 0, 100);
}

int TCodeEncoder::mapValue(int posPercent, const AxisConfig& config)
{
    int percent = std::clamp(posPercent, 0, 100);
    if (config.inverted)
        percent = 100 - percent;

    const int minimum = std::min(config.min, config.max);
    const int maximum = std::max(config.min, config.max);
    return minimum + qRound((maximum - minimum) * percent / 100.0);
}
