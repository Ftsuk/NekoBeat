#include "core/Loc.h"
#include "core/ScriptLoader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace
{
// Anything past this is not a real media timeline (~11.5 days) and would only
// come from a broken or hostile file.
constexpr double kMaxActionTimeMs = 1.0e9;
// The largest real scripts run a few thousand points per axis. The cap keeps a
// malformed file from exhausting memory while still leaving a 50x margin.
constexpr int kMaxActionsPerTrack = 200000;

QString stripFunscriptExtension(const QString& path)
{
    QString value = path;
    if (value.endsWith(QStringLiteral(".funscript"), Qt::CaseInsensitive))
        value.chop(QStringLiteral(".funscript").size());
    return value;
}

QString normalizeBase(const QString& path)
{
    QString base = QDir::cleanPath(path);
    if (base.endsWith(QStringLiteral(".funscript"), Qt::CaseInsensitive))
        base.chop(QStringLiteral(".funscript").size());
    else
    {
        const int slash = std::max(base.lastIndexOf(QLatin1Char('/')), base.lastIndexOf(QLatin1Char('\\')));
        const int dot = base.lastIndexOf(QLatin1Char('.'));
        if (dot > slash)
            base.truncate(dot);
    }
    return base;
}

// Returns the group key of a script file and, when the file ends with a known
// axis identifier, the axis it belongs to.
QString scriptGroupKeyFromPath(const QString& path, Track* track)
{
    const QString stem = stripFunscriptExtension(QDir::cleanPath(path));
    const int slash = stem.lastIndexOf(QLatin1Char('/'));
    const int dot = stem.lastIndexOf(QLatin1Char('.'));
    if (dot <= slash)
        return stem;

    const QString suffix = stem.mid(dot + 1);
    const auto parsed = TrackInfo::fromIdentifier(suffix);
    if (!parsed)
        return stem;

    if (track)
        *track = *parsed;
    return stem.left(dot);
}

// Two names belong together when one is a prefix of the other that ends on a
// separator, e.g. "clip" and "clip v2" or "clip" and "clip.mp4".
bool relatedNames(const QString& scriptName, const QString& videoName)
{
    if (scriptName.compare(videoName, Qt::CaseInsensitive) == 0)
        return true;

    const auto prefixed = [](const QString& longer, const QString& shorter) {
        if (longer.size() <= shorter.size())
            return false;
        if (!longer.startsWith(shorter, Qt::CaseInsensitive))
            return false;
        return !longer.at(shorter.size()).isLetterOrNumber();
    };
    return prefixed(scriptName, videoName) || prefixed(videoName, scriptName);
}

QString replaceSuffixWithTrack(const QString& selectedScript, Track track)
{
    const QString base = stripFunscriptExtension(selectedScript);
    const QString canonical = TrackInfo::canonicalName(track);
    const QString tcode = TrackInfo::tcodeId(track).toLower();

    if (base.endsWith(QLatin1Char('.') + canonical, Qt::CaseInsensitive))
        return base.left(base.size() - canonical.size() - 1) + QStringLiteral(".funscript");
    if (base.endsWith(QLatin1Char('.') + tcode, Qt::CaseInsensitive))
        return base.left(base.size() - tcode.size() - 1) + QStringLiteral(".funscript");
    return {};
}
}

QList<Action> ScriptLoader::parseActions(const QJsonArray& array, int range, QStringList* warnings)
{
    QMap<qint64, int> byTime;
    const int safeRange = range > 0 ? range : 100;
    bool limitWarned = false;

    for (const QJsonValue& value : array)
    {
        if (!value.isObject())
            continue;

        const QJsonObject object = value.toObject();
        if (!object.contains(QStringLiteral("at")) || !object.contains(QStringLiteral("pos")))
            continue;

        const double atValue = object.value(QStringLiteral("at")).toDouble(-1.0);
        const double posValue = object.value(QStringLiteral("pos")).toDouble(-1.0);
        // Reject non-finite and out-of-range values. Casting a huge double to
        // qint64 below is undefined behaviour, so the check has to happen here
        // rather than after the conversion.
        if (!std::isfinite(atValue) || !std::isfinite(posValue))
            continue;
        if (atValue < 0.0 || atValue > kMaxActionTimeMs || posValue < 0.0)
            continue;

        if (byTime.size() >= kMaxActionsPerTrack && !byTime.contains(static_cast<qint64>(atValue)))
        {
            if (!limitWarned && warnings)
            {
                limitWarned = true;
                warnings->append(LT("动作点超过 %1 个，多余部分已忽略")
                                     .arg(kMaxActionsPerTrack));
            }
            continue;
        }

        int pos = qRound(posValue * 100.0 / safeRange);
        pos = std::clamp(pos, 0, 100);
        byTime.insert(static_cast<qint64>(atValue), pos);
    }

    QList<Action> actions;
    actions.reserve(byTime.size());
    for (auto it = byTime.constBegin(); it != byTime.constEnd(); ++it)
        actions.append({it.key(), it.value()});

    if (actions.isEmpty() && warnings)
        warnings->append(LT("脚本中没有可用动作点"));

    return actions;
}

QList<Action> ScriptLoader::parseActionsValue(const QJsonValue& value, int range, QStringList* warnings)
{
    if (value.isArray())
        return parseActions(value.toArray(), range, warnings);

    if (value.isObject())
        return parseActions(value.toObject().value(QStringLiteral("actions")).toArray(), range, warnings);

    return {};
}

void ScriptLoader::addTimeline(ScriptBundle& bundle, Track track, QList<Action> actions, ScriptFormat format)
{
    if (!TrackInfo::isSupported(track) || actions.isEmpty())
        return;

    Timeline timeline;
    timeline.track = track;
    timeline.actions = std::move(actions);
    bundle.tracks.insert(static_cast<int>(track), timeline);

    if (bundle.format == ScriptFormat::Unknown || bundle.format == ScriptFormat::Single)
        bundle.format = format;
}

void ScriptLoader::parseMerged(ScriptBundle& bundle, const QJsonObject& root)
{
    const int range = root.value(QStringLiteral("range")).toInt(100);
    bool explicitStroke = false;

    if (const QJsonValue axesValue = root.value(QStringLiteral("axes")); axesValue.isArray())
    {
        bundle.format = ScriptFormat::Axes;
        for (const QJsonValue& axisValue : axesValue.toArray())
        {
            if (!axisValue.isObject())
                continue;
            const QJsonObject axisObject = axisValue.toObject();
            const QString identifier = axisObject.value(QStringLiteral("id")).toString();
            const auto track = TrackInfo::fromIdentifier(identifier);
            if (!track)
            {
                bundle.warnings.append(LT("未知轴标识：%1").arg(identifier));
                continue;
            }
            if (*track == Track::Stroke)
                explicitStroke = true;
            addTimeline(bundle, *track, parseActionsValue(axisObject, range, &bundle.warnings), ScriptFormat::Axes);
        }
    }

    const QString objectKeys[] = {QStringLiteral("channels"), QStringLiteral("tracks")};
    for (const QString& key : objectKeys)
    {
        const QJsonValue objectValue = root.value(key);
        if (!objectValue.isObject())
            continue;

        bundle.format = (key == QStringLiteral("channels")) ? ScriptFormat::Channels : ScriptFormat::Tracks;
        const QJsonObject tracksObject = objectValue.toObject();
        for (auto it = tracksObject.constBegin(); it != tracksObject.constEnd(); ++it)
        {
            const auto track = TrackInfo::fromIdentifier(it.key());
            if (!track)
            {
                bundle.warnings.append(LT("未知轨道：%1").arg(it.key()));
                continue;
            }
            if (*track == Track::Stroke)
                explicitStroke = true;
            addTimeline(bundle, *track, parseActionsValue(it.value(), range, &bundle.warnings), bundle.format);
        }
    }

    const QJsonValue topActions = root.value(QStringLiteral("actions"));
    if (topActions.isArray() && !explicitStroke)
    {
        addTimeline(bundle, Track::Stroke, parseActions(topActions.toArray(), range, &bundle.warnings),
                    bundle.format == ScriptFormat::Unknown ? ScriptFormat::Single : bundle.format);
    }
}

bool ScriptLoader::parseScriptFile(ScriptBundle& bundle, const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
    {
        bundle.warnings.append(LT("无法打开脚本：%1").arg(filePath));
        return false;
    }

    // Reject absurdly large files before reading them. A damaged or hostile
    // script must not be able to exhaust memory inside readAll().
    constexpr qint64 kMaxScriptBytes = 64 * 1024 * 1024;
    if (file.size() > kMaxScriptBytes)
    {
        bundle.warnings.append(LT("脚本文件过大，已跳过：%1").arg(filePath));
        return false;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (document.isNull() || !document.isObject())
    {
        bundle.warnings.append(LT("脚本 JSON 无效：%1 (%2)")
                                   .arg(filePath, error.errorString()));
        return false;
    }

    bundle.scriptPath = QDir::cleanPath(filePath);
    parseMerged(bundle, document.object());
    return true;
}

QString ScriptLoader::findSiblingScript(const QString& mediaBase, const QString& suffix)
{
    const QString directoryPath = QFileInfo(mediaBase).absolutePath();
    const QString baseName = QFileInfo(mediaBase).fileName();
    QDir directory(directoryPath);

    const QStringList entries = directory.entryList(QDir::Files | QDir::Readable, QDir::Name);
    const QString wanted = (baseName + QLatin1Char('.') + suffix + QStringLiteral(".funscript")).toLower();
    for (const QString& entry : entries)
    {
        if (entry.toLower() == wanted)
            return directory.absoluteFilePath(entry);
    }
    return {};
}

void ScriptLoader::loadSplitTracks(ScriptBundle& bundle, const QString& groupKey)
{
    if (groupKey.isEmpty())
        return;

    for (Track track : TrackInfo::sr6Tracks())
    {
        // The main script already covers the stroke axis; other axes are only
        // added when the group does not provide them yet.
        if (bundle.tracks.contains(static_cast<int>(track)))
            continue;

        QString path = findSiblingScript(groupKey, TrackInfo::canonicalName(track));
        if (path.isEmpty())
            path = findSiblingScript(groupKey, TrackInfo::tcodeId(track).toLower());
        if (path.isEmpty())
            continue;

        ScriptBundle split;
        split.mediaBase = groupKey;
        split.mediaPath = bundle.mediaPath;
        if (!parseScriptFile(split, path))
            continue;

        QList<Action> actions;
        if (split.tracks.contains(static_cast<int>(track)))
            actions = split.tracks.value(static_cast<int>(track)).actions;
        else if (split.tracks.contains(static_cast<int>(Track::Stroke)))
        {
            actions = split.tracks.value(static_cast<int>(Track::Stroke)).actions;
        }

        if (actions.isEmpty())
            continue;

        Timeline timeline;
        timeline.track = track;
        timeline.actions = std::move(actions);
        bundle.tracks.insert(static_cast<int>(track), timeline);
        bundle.format = ScriptFormat::Mfs;
    }

    if (!bundle.tracks.isEmpty() && bundle.format == ScriptFormat::Unknown)
        bundle.format = ScriptFormat::Single;
}

void ScriptLoader::loadSingleAxisFile(ScriptBundle& bundle, const QString& filePath)
{
    Track track = Track::None;
    scriptGroupKeyFromPath(filePath, &track);
    if (track == Track::None)
        return;
    if (bundle.tracks.contains(static_cast<int>(track)))
        return;

    ScriptBundle single;
    single.mediaPath = bundle.mediaPath;
    single.mediaBase = scriptGroupKeyFromPath(filePath, nullptr);
    if (!parseScriptFile(single, filePath))
        return;

    QList<Action> actions;
    if (single.tracks.contains(static_cast<int>(track)))
        actions = single.tracks.value(static_cast<int>(track)).actions;
    else if (single.tracks.contains(static_cast<int>(Track::Stroke)))
        actions = single.tracks.value(static_cast<int>(Track::Stroke)).actions;
    if (actions.isEmpty())
        return;

    Timeline timeline;
    timeline.track = track;
    timeline.actions = std::move(actions);
    bundle.tracks.insert(static_cast<int>(track), timeline);
    if (bundle.format == ScriptFormat::Unknown || bundle.format == ScriptFormat::Single)
        bundle.format = ScriptFormat::Mfs;
}

ScriptBundle ScriptLoader::load(const QString& mediaPath, const QString& selectedScript)
{
    ScriptBundle bundle;
    bundle.mediaPath = QDir::cleanPath(mediaPath);

    if (!mediaPath.isEmpty())
        bundle.mediaBase = normalizeBase(mediaPath);
    else if (!selectedScript.isEmpty())
        bundle.mediaBase = normalizeBase(selectedScript);

    // A selected script decides which group is loaded; otherwise the group
    // that matches the media file name is used.
    const QString groupKey = !selectedScript.isEmpty()
                                 ? scriptGroupKey(selectedScript)
                                 : bundle.mediaBase;

    const QString mainScript = groupKey.isEmpty()
                                   ? QString()
                                   : groupKey + QStringLiteral(".funscript");

    if (!mainScript.isEmpty() && QFileInfo::exists(mainScript))
        parseScriptFile(bundle, mainScript);

    if (!selectedScript.isEmpty() && QFileInfo::exists(selectedScript))
        loadSingleAxisFile(bundle, selectedScript);

    loadSplitTracks(bundle, groupKey);

    // A stale or foreign selection should never leave the user without a
    // script: fall back to the group named after the media file.
    if (bundle.tracks.isEmpty() && !bundle.mediaBase.isEmpty() && groupKey != bundle.mediaBase)
    {
        const QString fallbackMain = bundle.mediaBase + QStringLiteral(".funscript");
        if (QFileInfo::exists(fallbackMain))
            parseScriptFile(bundle, fallbackMain);
        loadSplitTracks(bundle, bundle.mediaBase);
    }

    return bundle;
}

QList<ScriptGroup> ScriptLoader::findScriptGroups(const QString& mediaPath)
{
    QList<ScriptGroup> result;
    const QString mediaBase = normalizeBase(mediaPath);
    if (mediaBase.isEmpty())
        return result;

    const QFileInfo mediaInfo(mediaBase);
    const QDir directory(mediaInfo.absolutePath());
    const QString videoName = mediaInfo.fileName();

    QMap<QString, ScriptGroup> grouped;
    const QStringList entries = directory.entryList(
        {QStringLiteral("*.funscript")}, QDir::Files | QDir::Readable, QDir::Name);
    for (const QString& entry : entries)
    {
        const QString path = directory.absoluteFilePath(entry);
        Track track = Track::None;
        const QString key = scriptGroupKeyFromPath(path, &track);
        const QString keyName = QFileInfo(key).fileName();
        if (!relatedNames(keyName, videoName))
            continue;

        ScriptGroup& group = grouped[key.toLower()];
        if (group.key.isEmpty())
        {
            group.key = key;
            group.label = scriptGroupLabel(mediaPath, key);
        }

        if (track != Track::None)
            group.axisScripts.insert(static_cast<int>(track), path);
        else if (group.mainScript.isEmpty())
            group.mainScript = path;
    }

    result = grouped.values();
    const QString defaultKey = mediaBase.toLower();
    std::stable_sort(result.begin(), result.end(),
                     [&defaultKey](const ScriptGroup& left, const ScriptGroup& right) {
                         const bool leftDefault = left.key.toLower() == defaultKey;
                         const bool rightDefault = right.key.toLower() == defaultKey;
                         if (leftDefault != rightDefault)
                             return leftDefault;
                         return QString::compare(left.label, right.label,
                                                 Qt::CaseInsensitive)
                                < 0;
                     });
    return result;
}

QString ScriptLoader::scriptGroupKey(const QString& scriptPath)
{
    return scriptGroupKeyFromPath(scriptPath, nullptr);
}

QString ScriptLoader::scriptGroupLabel(const QString& mediaPath, const QString& groupKey)
{
    const QString videoName = QFileInfo(normalizeBase(mediaPath)).fileName();
    const QString keyName = QFileInfo(groupKey).fileName();
    if (keyName.compare(videoName, Qt::CaseInsensitive) == 0)
        return LT("默认脚本");

    if (keyName.size() > videoName.size()
        && keyName.startsWith(videoName, Qt::CaseInsensitive))
    {
        QString suffix = keyName.mid(videoName.size());
        while (!suffix.isEmpty() && !suffix.at(0).isLetterOrNumber())
            suffix.remove(0, 1);
        if (!suffix.isEmpty())
            return suffix;
    }
    return LT("默认脚本");
}

QString ScriptLoader::groupSummary(const ScriptGroup& group)
{
    if (group.axisScripts.isEmpty())
        return group.mainScript.isEmpty() ? LT("无脚本")
                                          : LT("合并脚本");

    const int axes = group.axisScripts.size() + (group.mainScript.isEmpty() ? 0 : 1);
    return LT("%1 轴文件").arg(axes);
}
