#include "core/Loc.h"
#include "core/AxisProfileStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSettings>

#include <algorithm>

namespace
{
constexpr int kFormatVersion = 1;
const char* kDataKey = "axisProfiles/data";

QJsonObject axisToJson(const AxisConfig& config)
{
    QJsonObject object;
    object.insert(QStringLiteral("min"), config.min);
    object.insert(QStringLiteral("max"), config.max);
    object.insert(QStringLiteral("home"), config.home);
    object.insert(QStringLiteral("inverted"), config.inverted);
    object.insert(QStringLiteral("offsetMs"), static_cast<double>(config.offsetMs));
    return object;
}

AxisConfig axisFromJson(const QJsonObject& object, const AxisConfig& fallback)
{
    AxisConfig config = fallback;
    config.min = object.value(QStringLiteral("min")).toInt(config.min);
    config.max = object.value(QStringLiteral("max")).toInt(config.max);
    config.home = object.value(QStringLiteral("home")).toInt(config.home);
    config.inverted = object.value(QStringLiteral("inverted")).toBool(config.inverted);
    config.offsetMs = static_cast<qint64>(
        object.value(QStringLiteral("offsetMs")).toDouble(static_cast<double>(config.offsetMs)));
    config.min = std::clamp(config.min, 0, 9999);
    config.max = std::clamp(config.max, 0, 9999);
    config.home = std::clamp(config.home, 0, 9999);
    return config;
}
}

AxisProfileStore::AxisProfileStore(QSettings* settings)
    : m_settings(settings)
{
    if (!m_settings)
        m_ownedSettings = std::make_unique<QSettings>();
}

AxisProfileStore::~AxisProfileStore() = default;

QSettings& AxisProfileStore::settings() const
{
    return m_settings ? *m_settings : *m_ownedSettings;
}

QString AxisProfileStore::defaultProfileName()
{
    return LT("默认");
}

void AxisProfileStore::load()
{
    if (m_loaded)
        return;
    m_loaded = true;

    const QByteArray raw = settings().value(QLatin1String(kDataKey)).toByteArray();
    if (raw.isEmpty())
    {
        ensureDefaultProfile();
        return;
    }

    const QJsonDocument document = QJsonDocument::fromJson(raw);
    if (!document.isObject())
    {
        ensureDefaultProfile();
        return;
    }

    const QJsonObject root = document.object();
    const QJsonArray profiles = root.value(QStringLiteral("profiles")).toArray();
    m_profiles.clear();
    for (const QJsonValue& value : profiles)
    {
        if (!value.isObject())
            continue;
        const QJsonObject object = value.toObject();
        AxisProfile profile;
        profile.name = object.value(QStringLiteral("name")).toString().trimmed();
        if (profile.name.isEmpty())
            continue;

        const QJsonObject axes = object.value(QStringLiteral("axes")).toObject();
        for (auto it = axes.constBegin(); it != axes.constEnd(); ++it)
        {
            const auto track = TrackInfo::fromIdentifier(it.key());
            if (!track || !it.value().isObject())
                continue;
            profile.axes.insert(static_cast<int>(*track),
                                axisFromJson(it.value().toObject(), AxisConfig{}));
        }
        m_profiles.append(profile);
    }

    m_active = root.value(QStringLiteral("active")).toString().trimmed();
    ensureDefaultProfile();
}

void AxisProfileStore::save() const
{
    settings().setValue(QLatin1String(kDataKey),
                        QJsonDocument(toJson()).toJson(QJsonDocument::Compact));
    settings().sync();
}

void AxisProfileStore::ensureDefaultProfile()
{
    if (m_profiles.isEmpty())
    {
        AxisProfile profile;
        profile.name = defaultProfileName();
        m_profiles.append(profile);
    }

    if (indexOf(m_active) < 0)
        m_active = m_profiles.first().name;
}

int AxisProfileStore::indexOf(const QString& name) const
{
    for (int i = 0; i < m_profiles.size(); ++i)
    {
        if (m_profiles.at(i).name.compare(name, Qt::CaseInsensitive) == 0)
            return i;
    }
    return -1;
}

QStringList AxisProfileStore::profileNames() const
{
    const_cast<AxisProfileStore*>(this)->load();
    QStringList names;
    names.reserve(m_profiles.size());
    for (const AxisProfile& profile : m_profiles)
        names.append(profile.name);
    return names;
}

QString AxisProfileStore::activeName() const
{
    const_cast<AxisProfileStore*>(this)->load();
    return m_active;
}

AxisProfile AxisProfileStore::activeProfile() const
{
    const_cast<AxisProfileStore*>(this)->load();
    const int index = indexOf(m_active);
    if (index >= 0)
        return m_profiles.at(index);

    AxisProfile fallback;
    fallback.name = defaultProfileName();
    return fallback;
}

bool AxisProfileStore::setActiveName(const QString& name)
{
    load();
    if (indexOf(name) < 0)
        return false;
    m_active = m_profiles.at(indexOf(name)).name;
    save();
    return true;
}

bool AxisProfileStore::contains(const QString& name) const
{
    const_cast<AxisProfileStore*>(this)->load();
    return indexOf(name) >= 0;
}

AxisProfile AxisProfileStore::profile(const QString& name) const
{
    const_cast<AxisProfileStore*>(this)->load();
    const int index = indexOf(name);
    if (index < 0)
        return {};
    return m_profiles.at(index);
}

bool AxisProfileStore::saveProfile(const AxisProfile& profile)
{
    load();
    const QString name = profile.name.trimmed();
    if (name.isEmpty())
        return false;

    AxisProfile stored = profile;
    stored.name = name;
    const int index = indexOf(name);
    if (index >= 0)
        m_profiles[index] = stored;
    else
        m_profiles.append(stored);
    save();
    return true;
}

bool AxisProfileStore::removeProfile(const QString& name)
{
    load();
    const int index = indexOf(name);
    if (index < 0)
        return false;
    if (m_profiles.size() <= 1)
        return false;

    m_profiles.removeAt(index);
    if (m_active.compare(name, Qt::CaseInsensitive) == 0)
        m_active = m_profiles.first().name;
    save();
    return true;
}

bool AxisProfileStore::renameProfile(const QString& from, const QString& to)
{
    load();
    const QString target = to.trimmed();
    if (target.isEmpty())
        return false;
    const int index = indexOf(from);
    if (index < 0)
        return false;

    const int clash = indexOf(target);
    if (clash >= 0 && clash != index)
        return false;

    const bool wasActive = m_active.compare(m_profiles.at(index).name, Qt::CaseInsensitive) == 0;
    m_profiles[index].name = target;
    if (wasActive)
        m_active = target;
    save();
    return true;
}

void AxisProfileStore::migrateLegacy(const QMap<int, AxisConfig>& legacy)
{
    load();
    const int index = indexOf(defaultProfileName());
    if (index < 0 || legacy.isEmpty())
        return;

    if (!m_profiles.at(index).axes.isEmpty())
        return;

    AxisProfile& profile = m_profiles[index];
    for (auto it = legacy.constBegin(); it != legacy.constEnd(); ++it)
    {
        AxisConfig config = it.value();
        config.enabled = true;
        profile.axes.insert(it.key(), config);
    }
    save();
}

QJsonObject AxisProfileStore::toJson() const
{
    const_cast<AxisProfileStore*>(this)->load();
    QJsonObject root;
    root.insert(QStringLiteral("version"), kFormatVersion);
    root.insert(QStringLiteral("active"), m_active);

    QJsonArray profiles;
    for (const AxisProfile& profile : m_profiles)
    {
        QJsonObject object;
        object.insert(QStringLiteral("name"), profile.name);
        QJsonObject axes;
        for (auto it = profile.axes.constBegin(); it != profile.axes.constEnd(); ++it)
        {
            const QString id = TrackInfo::tcodeId(static_cast<Track>(it.key()));
            if (id.isEmpty())
                continue;
            axes.insert(id, axisToJson(it.value()));
        }
        object.insert(QStringLiteral("axes"), axes);
        profiles.append(object);
    }
    root.insert(QStringLiteral("profiles"), profiles);
    return root;
}

QStringList AxisProfileStore::mergeJson(const QJsonObject& root)
{
    load();
    QStringList imported;
    const QJsonArray profiles = root.value(QStringLiteral("profiles")).toArray();
    for (const QJsonValue& value : profiles)
    {
        if (!value.isObject())
            continue;
        const QJsonObject object = value.toObject();
        AxisProfile profile;
        profile.name = object.value(QStringLiteral("name")).toString().trimmed();
        if (profile.name.isEmpty())
            continue;

        const QJsonObject axes = object.value(QStringLiteral("axes")).toObject();
        for (auto it = axes.constBegin(); it != axes.constEnd(); ++it)
        {
            const auto track = TrackInfo::fromIdentifier(it.key());
            if (!track || !it.value().isObject())
                continue;
            profile.axes.insert(static_cast<int>(*track),
                                axisFromJson(it.value().toObject(), AxisConfig{}));
        }

        const int index = indexOf(profile.name);
        if (index >= 0)
            m_profiles[index] = profile;
        else
            m_profiles.append(profile);
        imported.append(profile.name);
    }

    if (!imported.isEmpty())
    {
        const QString active = root.value(QStringLiteral("active")).toString().trimmed();
        if (indexOf(active) >= 0)
            m_active = active;
        save();
    }
    return imported;
}
