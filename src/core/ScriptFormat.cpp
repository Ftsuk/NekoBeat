#include "core/Loc.h"
#include "core/Types.h"

QString scriptFormatName(ScriptFormat format)
{
    switch (format)
    {
    case ScriptFormat::Single: return LT("单轴");
    case ScriptFormat::Mfs: return LT("拆分多轴");
    case ScriptFormat::Channels: return QStringLiteral("channels");
    case ScriptFormat::Tracks: return QStringLiteral("tracks");
    case ScriptFormat::Axes: return QStringLiteral("axes");
    case ScriptFormat::Unknown: break;
    }
    return LT("未知");
}

