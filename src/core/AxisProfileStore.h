#pragma once

#include "core/Types.h"

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <memory>

class QSettings;

struct AxisProfile
{
    QString name;
    QMap<int, AxisConfig> axes;
};

// Stores named axis limit profiles. The layout is a single JSON document inside
// QSettings so profile names never have to be escaped as settings keys.
class AxisProfileStore
{
public:
    explicit AxisProfileStore(QSettings* settings = nullptr);
    ~AxisProfileStore();

    AxisProfileStore(const AxisProfileStore&) = delete;
    AxisProfileStore& operator=(const AxisProfileStore&) = delete;

    QStringList profileNames() const;
    QString activeName() const;
    AxisProfile activeProfile() const;
    bool setActiveName(const QString& name);

    bool contains(const QString& name) const;
    AxisProfile profile(const QString& name) const;
    bool saveProfile(const AxisProfile& profile);
    bool removeProfile(const QString& name);
    bool renameProfile(const QString& from, const QString& to);

    // Seeds the first profile from the legacy axis/<id>/ settings layout.
    void migrateLegacy(const QMap<int, AxisConfig>& legacy);
    void ensureDefaultProfile();

    QJsonObject toJson() const;
    // Merges profiles from the document; returns the names that were imported.
    QStringList mergeJson(const QJsonObject& root);

    static QString defaultProfileName();

private:
    QSettings& settings() const;
    void load();
    void save() const;
    int indexOf(const QString& name) const;

    mutable std::unique_ptr<QSettings> m_ownedSettings;
    QSettings* m_settings = nullptr;
    QList<AxisProfile> m_profiles;
    QString m_active;
    bool m_loaded = false;
};
