#include <QtTest>

#include "core/MediaRelink.h"

namespace
{
RelinkMediaInfo info(const QString& path, const QString& name, qint64 size,
                     qint64 modifiedMs, qint64 durationMs)
{
    RelinkMediaInfo value;
    value.path = path;
    value.displayName = name;
    value.fileSize = size;
    value.modifiedMs = modifiedMs;
    value.durationMs = durationMs;
    return value;
}
}

class MediaRelinkTest : public QObject
{
    Q_OBJECT

private slots:
    void identicalFingerprintIsConfident()
    {
        const QList<RelinkMediaInfo> missing = {
            info(QStringLiteral("C:/inbox/clip.mp4"), QStringLiteral("clip.mp4"),
                 2048, 1000, 90000)
        };
        const QList<RelinkMediaInfo> available = {
            info(QStringLiteral("D:/sorted/2026/clip.mp4"), QStringLiteral("clip.mp4"),
                 2048, 1000, 90000)
        };

        const QList<RelinkSuggestion> result = MediaRelink::suggest(missing, available);
        QCOMPARE(result.size(), 1);
        QVERIFY(result.first().hasCandidate());
        QVERIFY(result.first().confident());
        QCOMPARE(result.first().bestScore, 5);
        QVERIFY(result.first().best()->sameSize);
        QVERIFY(result.first().best()->sameModified);
        QVERIFY(result.first().best()->sameDuration);
    }

    void nameOnlyMatchIsNotConfident()
    {
        const QList<RelinkMediaInfo> missing = {
            info(QStringLiteral("C:/inbox/clip.mp4"), QStringLiteral("clip.mp4"),
                 2048, 1000, 90000)
        };
        // Same name, completely different file: this is the case that made
        // XTPlayer mix up covers, so it must never be applied silently.
        const QList<RelinkMediaInfo> available = {
            info(QStringLiteral("D:/other/clip.mp4"), QStringLiteral("clip.mp4"),
                 4096, 2000, 30000)
        };

        const QList<RelinkSuggestion> result = MediaRelink::suggest(missing, available);
        QCOMPARE(result.size(), 1);
        QVERIFY(result.first().hasCandidate());
        QVERIFY(!result.first().confident());
        QCOMPARE(result.first().bestScore, 0);
    }

    void severalEqualCandidatesAreAmbiguous()
    {
        const QList<RelinkMediaInfo> missing = {
            info(QStringLiteral("C:/inbox/clip.mp4"), QStringLiteral("clip.mp4"),
                 2048, 1000, 90000)
        };
        const QList<RelinkMediaInfo> available = {
            info(QStringLiteral("D:/a/clip.mp4"), QStringLiteral("clip.mp4"), 2048, 1000,
                 90000),
            info(QStringLiteral("D:/b/clip.mp4"), QStringLiteral("clip.mp4"), 2048, 1000,
                 90000)
        };

        const QList<RelinkSuggestion> result = MediaRelink::suggest(missing, available);
        QCOMPARE(result.first().candidates.size(), 2);
        QVERIFY(result.first().ambiguous);
        QVERIFY(!result.first().confident());
    }

    void differentNameIsNeverSuggested()
    {
        const QList<RelinkMediaInfo> missing = {
            info(QStringLiteral("C:/inbox/clip.mp4"), QStringLiteral("clip.mp4"),
                 2048, 1000, 90000)
        };
        const QList<RelinkMediaInfo> available = {
            info(QStringLiteral("D:/other/other.mp4"), QStringLiteral("other.mp4"),
                 2048, 1000, 90000)
        };

        const QList<RelinkSuggestion> result = MediaRelink::suggest(missing, available);
        QVERIFY(!result.first().hasCandidate());
        QVERIFY(!result.first().confident());
    }

    void betterCandidateSortsFirst()
    {
        const QList<RelinkMediaInfo> missing = {
            info(QStringLiteral("C:/inbox/clip.mp4"), QStringLiteral("clip.mp4"),
                 2048, 1000, 90000)
        };
        const QList<RelinkMediaInfo> available = {
            // Same name, nothing else in common.
            info(QStringLiteral("D:/a/clip.mp4"), QStringLiteral("clip.mp4"), 999, 5,
                 1000),
            // Same name, size and timestamp.
            info(QStringLiteral("D:/b/clip.mp4"), QStringLiteral("clip.mp4"), 2048, 1000,
                 1000)
        };

        const QList<RelinkSuggestion> result = MediaRelink::suggest(missing, available);
        QCOMPARE(result.first().candidates.size(), 2);
        QCOMPARE(result.first().candidates.first().path,
                 QStringLiteral("D:/b/clip.mp4"));
        QVERIFY(result.first().confident());
    }

    void candidateListIsCapped()
    {
        QList<RelinkMediaInfo> available;
        for (int i = 0; i < 20; ++i)
        {
            available.append(info(QStringLiteral("D:/%1/clip.mp4").arg(i),
                                  QStringLiteral("clip.mp4"), 100 + i, 10, 20));
        }
        const QList<RelinkMediaInfo> missing = {
            info(QStringLiteral("C:/inbox/clip.mp4"), QStringLiteral("clip.mp4"), 1, 2, 3)
        };

        const QList<RelinkSuggestion> result = MediaRelink::suggest(missing, available);
        QCOMPARE(result.first().candidates.size(), MediaRelink::kMaxCandidates);
    }
};

QTEST_MAIN(MediaRelinkTest)
#include "test_media_relink.moc"
