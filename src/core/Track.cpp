#include "core/Loc.h"
#include "core/Track.h"

#include <QHash>

namespace
{
const QHash<QString, Track>& aliasMap()
{
    static const QHash<QString, Track> aliases = {
        {QStringLiteral("stroke"), Track::Stroke},
        {QStringLiteral("l0"), Track::Stroke},
        {QStringLiteral("up"), Track::Stroke},
        {QStringLiteral("surge"), Track::Surge},
        {QStringLiteral("l1"), Track::Surge},
        {QStringLiteral("forward"), Track::Surge},
        {QStringLiteral("sway"), Track::Sway},
        {QStringLiteral("l2"), Track::Sway},
        {QStringLiteral("left"), Track::Sway},
        {QStringLiteral("twist"), Track::Twist},
        {QStringLiteral("r0"), Track::Twist},
        {QStringLiteral("roll"), Track::Roll},
        {QStringLiteral("r1"), Track::Roll},
        {QStringLiteral("pitch"), Track::Pitch},
        {QStringLiteral("r2"), Track::Pitch},
        {QStringLiteral("vib"), Track::Vib},
        {QStringLiteral("vibration"), Track::Vib},
        {QStringLiteral("v0"), Track::Vib},
        {QStringLiteral("lube"), Track::Lube},
        {QStringLiteral("a2"), Track::Lube},
        {QStringLiteral("suck"), Track::Suck},
        {QStringLiteral("a1"), Track::Suck},
        {QStringLiteral("suckmanual"), Track::SuckPosition},
        {QStringLiteral("suckposition"), Track::SuckPosition},
        {QStringLiteral("a0"), Track::SuckPosition}
    };
    return aliases;
}
}

std::optional<Track> TrackInfo::fromIdentifier(const QString& identifier)
{
    const auto key = identifier.trimmed().toLower();
    if (key.isEmpty())
        return std::nullopt;

    if (const auto it = aliasMap().constFind(key); it != aliasMap().constEnd())
        return it.value();

    return std::nullopt;
}

QString TrackInfo::tcodeId(Track track)
{
    switch (track)
    {
    case Track::Stroke: return QStringLiteral("L0");
    case Track::Surge: return QStringLiteral("L1");
    case Track::Sway: return QStringLiteral("L2");
    case Track::Twist: return QStringLiteral("R0");
    case Track::Roll: return QStringLiteral("R1");
    case Track::Pitch: return QStringLiteral("R2");
    case Track::Vib: return QStringLiteral("V0");
    case Track::Lube: return QStringLiteral("A2");
    case Track::Suck: return QStringLiteral("A1");
    case Track::SuckPosition: return QStringLiteral("A0");
    case Track::None: break;
    }
    return {};
}

QString TrackInfo::canonicalName(Track track)
{
    switch (track)
    {
    case Track::Stroke: return QStringLiteral("stroke");
    case Track::Surge: return QStringLiteral("surge");
    case Track::Sway: return QStringLiteral("sway");
    case Track::Twist: return QStringLiteral("twist");
    case Track::Roll: return QStringLiteral("roll");
    case Track::Pitch: return QStringLiteral("pitch");
    case Track::Vib: return QStringLiteral("vib");
    case Track::Lube: return QStringLiteral("lube");
    case Track::Suck: return QStringLiteral("suck");
    case Track::SuckPosition: return QStringLiteral("suckManual");
    case Track::None: break;
    }
    return {};
}

QString TrackInfo::displayName(Track track)
{
    switch (track)
    {
    case Track::Stroke: return LT("主轴 L0");
    case Track::Surge: return LT("前后 L1");
    case Track::Sway: return LT("左右 L2");
    case Track::Twist: return LT("扭转 R0");
    case Track::Roll: return LT("翻滚 R1");
    case Track::Pitch: return LT("俯仰 R2");
    case Track::Vib: return LT("震动 V0");
    case Track::Lube: return LT("润滑 A2");
    case Track::Suck: return LT("吸吮 A1");
    case Track::SuckPosition: return LT("吸吮行程 A0");
    case Track::None: break;
    }
    return LT("未知轴");
}

bool TrackInfo::isSupported(Track track)
{
    return track != Track::None && !tcodeId(track).isEmpty();
}

TrackInfo::AxisKind TrackInfo::kind(Track track)
{
    switch (track)
    {
    case Track::Vib:
    case Track::Lube:
    case Track::Suck:
        return AxisKind::Amplitude;
    case Track::Stroke:
    case Track::Surge:
    case Track::Sway:
    case Track::Twist:
    case Track::Roll:
    case Track::Pitch:
    case Track::SuckPosition:
    case Track::None:
        break;
    }
    return AxisKind::Position;
}

std::vector<Track> TrackInfo::sr6Tracks()
{
    return {
        Track::Stroke,
        Track::Surge,
        Track::Sway,
        Track::Twist,
        Track::Roll,
        Track::Pitch
    };
}
