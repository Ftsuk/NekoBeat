#include "TestLanguage.h"
#include "core/AxisProfileStore.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

class AxisProfileStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void defaults();
    void saveSwitchAndRename();
    void jsonRoundTrip();
    void legacyMigration();
};

namespace
{
QSettings* makeSettings(const QTemporaryDir& dir)
{
    return new QSettings(dir.filePath(QStringLiteral("profiles.ini")), QSettings::IniFormat);
}
}

void AxisProfileStoreTest::defaults()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings* settings = makeSettings(dir);

    AxisProfileStore store(settings);
    store.ensureDefaultProfile();
    QCOMPARE(store.profileNames().size(), 1);
    QCOMPARE(store.activeName(), AxisProfileStore::defaultProfileName());
    delete settings;
}

void AxisProfileStoreTest::saveSwitchAndRename()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings* settings = makeSettings(dir);
    AxisProfileStore store(settings);

    AxisProfile gentle;
    gentle.name = QStringLiteral("柔和");
    AxisConfig config;
    config.min = 1200;
    config.max = 7600;
    config.home = 4400;
    config.inverted = true;
    config.offsetMs = -120;
    gentle.axes.insert(static_cast<int>(Track::Stroke), config);
    QVERIFY(store.saveProfile(gentle));

    AxisProfile extreme;
    extreme.name = QStringLiteral("全行程");
    extreme.axes.insert(static_cast<int>(Track::Stroke), AxisConfig{});
    QVERIFY(store.saveProfile(extreme));
    // The store keeps an automatically created "默认" profile.
    QCOMPARE(store.profileNames().size(), 3);

    QVERIFY(store.setActiveName(QStringLiteral("柔和")));
    QCOMPARE(store.activeName(), QStringLiteral("柔和"));

    const AxisConfig loaded = store.activeProfile().axes.value(static_cast<int>(Track::Stroke));
    QCOMPARE(loaded.min, 1200);
    QCOMPARE(loaded.max, 7600);
    QCOMPARE(loaded.home, 4400);
    QCOMPARE(loaded.inverted, true);
    QCOMPARE(loaded.offsetMs, -120);

    QVERIFY(store.renameProfile(QStringLiteral("柔和"), QStringLiteral("轻柔")));
    QCOMPARE(store.activeName(), QStringLiteral("轻柔"));
    QVERIFY(store.contains(QStringLiteral("轻柔")));
    QVERIFY(!store.contains(QStringLiteral("柔和")));

    QVERIFY(store.removeProfile(QStringLiteral("全行程")));
    QCOMPARE(store.profileNames().size(), 2);
    QVERIFY(store.removeProfile(AxisProfileStore::defaultProfileName()));
    QCOMPARE(store.profileNames().size(), 1);
    QVERIFY(!store.removeProfile(QStringLiteral("轻柔")));
    delete settings;
}

void AxisProfileStoreTest::jsonRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings* settings = makeSettings(dir);
    AxisProfileStore store(settings);

    AxisProfile profile;
    profile.name = QStringLiteral("测试档");
    AxisConfig config;
    config.min = 300;
    config.max = 9700;
    config.home = 5000;
    config.inverted = false;
    config.offsetMs = 250;
    profile.axes.insert(static_cast<int>(Track::Roll), config);
    store.saveProfile(profile);

    const QJsonObject document = store.toJson();

    QSettings* otherSettings = new QSettings(dir.filePath(QStringLiteral("other.ini")),
                                             QSettings::IniFormat);
    AxisProfileStore other(otherSettings);
    const QStringList imported = other.mergeJson(document);
    QVERIFY(imported.contains(QStringLiteral("测试档")));
    const AxisConfig loaded = other.profile(QStringLiteral("测试档"))
                                  .axes.value(static_cast<int>(Track::Roll));
    QCOMPARE(loaded.min, 300);
    QCOMPARE(loaded.max, 9700);
    QCOMPARE(loaded.offsetMs, 250);
    delete settings;
    delete otherSettings;
}

void AxisProfileStoreTest::legacyMigration()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings* settings = makeSettings(dir);
    AxisProfileStore store(settings);

    QMap<int, AxisConfig> legacy;
    AxisConfig config;
    config.min = 900;
    config.max = 8100;
    config.enabled = false;
    legacy.insert(static_cast<int>(Track::Sway), config);
    store.migrateLegacy(legacy);

    const AxisConfig migrated = store.activeProfile().axes.value(static_cast<int>(Track::Sway));
    QCOMPARE(migrated.min, 900);
    QCOMPARE(migrated.max, 8100);
    QCOMPARE(migrated.enabled, true);
    delete settings;
}

QTEST_APPLESS_MAIN(AxisProfileStoreTest)
#include "test_axis_profiles.moc"
