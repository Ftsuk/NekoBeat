#pragma once

#include "core/Types.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>

// One selectable script set for a video: either a single merged multi-axis
// script, or one main script plus the matching per-axis files (for example
// "<video>.funscript" plus "<video>.roll.funscript"). Variants such as
// "<video>.v2.funscript" end up in their own group.
struct ScriptGroup
{
    QString key;                     // absolute base path without .funscript
    QString label;                   // display label relative to the video name
    QString mainScript;              // merged or main script file, may be empty
    QMap<int, QString> axisScripts;  // track -> file path

    bool isEmpty() const { return mainScript.isEmpty() && axisScripts.isEmpty(); }
};

class ScriptLoader
{
public:
    static ScriptBundle load(const QString& mediaPath, const QString& selectedScript = {});

    // All script sets that belong to the video, most specific first.
    static QList<ScriptGroup> findScriptGroups(const QString& mediaPath);
    static QString scriptGroupKey(const QString& scriptPath);
    static QString scriptGroupLabel(const QString& mediaPath, const QString& groupKey);
    static QString groupSummary(const ScriptGroup& group);

private:
    static QList<Action> parseActions(const QJsonArray& array, int range, QStringList* warnings);
    static QList<Action> parseActionsValue(const QJsonValue& value, int range, QStringList* warnings);
    static void addTimeline(ScriptBundle& bundle, Track track, QList<Action> actions, ScriptFormat format);
    static void parseMerged(ScriptBundle& bundle, const QJsonObject& root);
    static bool parseScriptFile(ScriptBundle& bundle, const QString& filePath);
    static void loadSplitTracks(ScriptBundle& bundle, const QString& groupKey);
    static void loadSingleAxisFile(ScriptBundle& bundle, const QString& filePath);
    static QString findSiblingScript(const QString& mediaBase, const QString& suffix);
};
