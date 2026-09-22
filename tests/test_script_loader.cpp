#include "TestLanguage.h"
#include "core/ScriptLoader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

namespace
{
QString fixturePath(const QString& relative)
{
    const QString root = QString::fromLocal8Bit(qgetenv("NEKOBEAT_TEST_FIXTURES"));
    return root.isEmpty() ? QString() : QDir(root).absoluteFilePath(relative);
}

bool writeFile(const QString& path, const QByteArray& content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(content) == content.size();
}

QByteArray scriptJson(qint64 at, int pos)
{
    return QStringLiteral("{\"range\":100,\"actions\":[{\"at\":%1,\"pos\":%2}]}")
        .arg(at)
        .arg(pos)
        .toUtf8();
}
}

class ScriptLoaderTest : public QObject
{
    Q_OBJECT

private slots:
    void loadSingle()
    {
        const QString path = fixturePath(QStringLiteral("[DeliriumBear] Marin - NTR 2/[DeliriumBear] Marin - NTR 2.mp4"));
        if (path.isEmpty() || !QFileInfo::exists(path))
            QSKIP("回归媒体样本不存在");
        const ScriptBundle bundle = ScriptLoader::load(path);
        QCOMPARE(bundle.tracks.size(), 1);
        QVERIFY(bundle.tracks.contains(static_cast<int>(Track::Stroke)));
        QVERIFY(bundle.tracks.value(static_cast<int>(Track::Stroke)).actions.size() > 1000);
    }

    void loadSplit()
    {
        const QString path = fixturePath(QStringLiteral("[Chr] ホタル 2 Collab with ナクル/[Chr] ホタル 2 Collab with ナクル.mp4"));
        if (path.isEmpty() || !QFileInfo::exists(path))
            QSKIP("回归媒体样本不存在");
        const ScriptBundle bundle = ScriptLoader::load(path);
        QCOMPARE(bundle.tracks.size(), 6);
        for (Track track : TrackInfo::sr6Tracks())
            QVERIFY(bundle.tracks.contains(static_cast<int>(track)));
    }

    void loadAxes()
    {
        const QString path = fixturePath(QStringLiteral("[MMD] Loli mettyamottya-Hirotakerご指名解禁腰振りダンス/[MMD] Loli mettyamottya-Hirotakerご指名解禁腰振りダンス.mp4"));
        if (path.isEmpty() || !QFileInfo::exists(path))
            QSKIP("回归媒体样本不存在");
        const ScriptBundle bundle = ScriptLoader::load(path);
        QCOMPARE(bundle.tracks.size(), 6);
        for (Track track : TrackInfo::sr6Tracks())
            QVERIFY(bundle.tracks.contains(static_cast<int>(track)));
    }

    void scriptGroupKeys()
    {
        QCOMPARE(ScriptLoader::scriptGroupKey(QStringLiteral("C:/media/clip.funscript")),
                 QStringLiteral("C:/media/clip"));
        QCOMPARE(ScriptLoader::scriptGroupKey(QStringLiteral("C:/media/clip.roll.funscript")),
                 QStringLiteral("C:/media/clip"));
        QCOMPARE(ScriptLoader::scriptGroupKey(QStringLiteral("C:/media/clip.R1.funscript")),
                 QStringLiteral("C:/media/clip"));
        QCOMPARE(ScriptLoader::scriptGroupKey(QStringLiteral("C:/media/clip.v2.pitch.funscript")),
                 QStringLiteral("C:/media/clip.v2"));
    }

    void scriptGroupsAndSelection()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString video = dir.filePath(QStringLiteral("clip.mp4"));
        QVERIFY(writeFile(video, QByteArray("stub")));
        QVERIFY(writeFile(dir.filePath(QStringLiteral("clip.funscript")), scriptJson(0, 10)));
        QVERIFY(writeFile(dir.filePath(QStringLiteral("clip.roll.funscript")), scriptJson(0, 20)));
        QVERIFY(writeFile(dir.filePath(QStringLiteral("clip.v2.funscript")), scriptJson(0, 30)));
        QVERIFY(writeFile(dir.filePath(QStringLiteral("clip.v2.pitch.funscript")), scriptJson(0, 40)));
        QVERIFY(writeFile(dir.filePath(QStringLiteral("unrelated.funscript")), scriptJson(0, 50)));

        const QList<ScriptGroup> groups = ScriptLoader::findScriptGroups(video);
        QCOMPARE(groups.size(), 2);
        QCOMPARE(groups.at(0).label, QStringLiteral("默认脚本"));
        QCOMPARE(groups.at(0).axisScripts.size(), 1);
        QVERIFY(!groups.at(0).mainScript.isEmpty());
        QCOMPARE(groups.at(1).label, QStringLiteral("v2"));
        QCOMPARE(groups.at(1).axisScripts.size(), 1);
        QCOMPARE(ScriptLoader::groupSummary(groups.at(0)), QStringLiteral("2 轴文件"));

        const ScriptBundle automatic = ScriptLoader::load(video);
        QCOMPARE(automatic.tracks.size(), 2);
        QVERIFY(automatic.tracks.contains(static_cast<int>(Track::Stroke)));
        QVERIFY(automatic.tracks.contains(static_cast<int>(Track::Roll)));
        QVERIFY(!automatic.tracks.contains(static_cast<int>(Track::Pitch)));

        const ScriptBundle v2 = ScriptLoader::load(
            video, dir.filePath(QStringLiteral("clip.v2.pitch.funscript")));
        QCOMPARE(v2.tracks.size(), 2);
        QVERIFY(v2.tracks.contains(static_cast<int>(Track::Stroke)));
        QVERIFY(v2.tracks.contains(static_cast<int>(Track::Pitch)));
        QVERIFY(!v2.tracks.contains(static_cast<int>(Track::Roll)));

        // A missing selection falls back to the group named after the video.
        const ScriptBundle fallback = ScriptLoader::load(
            video, dir.filePath(QStringLiteral("clip.v9.funscript")));
        QCOMPARE(fallback.tracks.size(), 2);
        QVERIFY(fallback.tracks.contains(static_cast<int>(Track::Stroke)));
        QVERIFY(fallback.tracks.contains(static_cast<int>(Track::Roll)));
    }

    void missingScriptsProduceEmptyBundle()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString video = dir.filePath(QStringLiteral("lonely.mp4"));
        QVERIFY(writeFile(video, QByteArray("stub")));

        QVERIFY(ScriptLoader::findScriptGroups(video).isEmpty());
        const ScriptBundle bundle = ScriptLoader::load(video);
        QVERIFY(bundle.tracks.isEmpty());
    }

    // A malformed script must never reach the double -> qint64 conversion,
    // which is undefined behaviour for values outside the representable range.
    void outOfRangeActionValuesAreRejected()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString video = dir.filePath(QStringLiteral("clip.mp4"));
        QVERIFY(writeFile(video, QByteArray("stub")));

        const QByteArray script = QByteArray(
            "{\"range\":100,\"actions\":["
            "{\"at\":0,\"pos\":10},"
            "{\"at\":1e30,\"pos\":90},"
            "{\"at\":-5,\"pos\":30},"
            "{\"at\":500,\"pos\":-20},"
            "{\"at\":500,\"pos\":20}]}");
        QVERIFY(writeFile(dir.filePath(QStringLiteral("clip.funscript")), script));

        const ScriptBundle bundle = ScriptLoader::load(video);
        QCOMPARE(bundle.tracks.size(), 1);
        const QList<Action> actions =
            bundle.tracks.value(static_cast<int>(Track::Stroke)).actions;
        QCOMPARE(static_cast<int>(actions.size()), 2);
        QCOMPARE(actions.first().atMs, 0);
        QCOMPARE(actions.first().pos, 10);
        QCOMPARE(actions.last().atMs, 500);
        QCOMPARE(actions.last().pos, 20);
    }

    void scriptGroupsOnSplitSample()
    {
        const QString path = fixturePath(QStringLiteral(
            "[Chr] ホタル 2 Collab with ナクル/[Chr] ホタル 2 Collab with ナクル.mp4"));
        if (path.isEmpty() || !QFileInfo::exists(path))
            QSKIP("回归媒体样本不存在");

        const QList<ScriptGroup> groups = ScriptLoader::findScriptGroups(path);
        QCOMPARE(groups.size(), 1);
        QCOMPARE(groups.first().label, QStringLiteral("默认脚本"));
        QCOMPARE(groups.first().axisScripts.size(), 5);
        QVERIFY(!groups.first().mainScript.isEmpty());
        QCOMPARE(ScriptLoader::groupSummary(groups.first()), QStringLiteral("6 轴文件"));
    }
};

QTEST_APPLESS_MAIN(ScriptLoaderTest)
#include "test_script_loader.moc"
