#pragma once

#include "core/Track.h"

#include <QList>
#include <QMetaType>
#include <QMap>
#include <QString>
#include <QStringList>

struct Action
{
    qint64 atMs = 0;
    int pos = 0;
};

struct AxisConfig
{
    bool enabled = true;
    int min = 0;
    int max = 9999;
    int home = 5000;
    bool inverted = false;
    qint64 offsetMs = 0;
};

struct Timeline
{
    Track track = Track::None;
    QList<Action> actions;
    AxisConfig config;
};

enum class ScriptFormat
{
    Unknown,
    Single,
    Mfs,
    Channels,
    Tracks,
    Axes
};

QString scriptFormatName(ScriptFormat format);

struct ScriptBundle
{
    QString mediaPath;
    QString mediaBase;
    QString scriptPath;
    ScriptFormat format = ScriptFormat::Unknown;
    QMap<int, Timeline> tracks;
    QStringList warnings;

    bool isEmpty() const { return tracks.isEmpty(); }
};

// Queued signals pass these by value across the device worker thread boundary.
Q_DECLARE_METATYPE(AxisConfig)
Q_DECLARE_METATYPE(ScriptBundle)
