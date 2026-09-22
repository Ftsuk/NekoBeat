#include "TestLanguage.h"
#include "core/LoopClip.h"

#include <QtTest>

class LoopClipTest : public QObject
{
    Q_OBJECT

private slots:
    void normalizeRangeSwapsReversedPoints()
    {
        qint64 start = 5000;
        qint64 end = 2000;
        LoopClipRules::normalizeRange(&start, &end);
        QCOMPARE(start, 2000);
        QCOMPARE(end, 5000);

        start = 1000;
        end = 4000;
        LoopClipRules::normalizeRange(&start, &end);
        QCOMPARE(start, 1000);
        QCOMPARE(end, 4000);
    }

    void validateRangeRejectsBadSegments()
    {
        QVERIFY(LoopClipRules::validateRange(1000, 5000).isEmpty());

        QVERIFY(LoopClipRules::validateRange(-1, 5000).contains(QStringLiteral("负数")));
        QVERIFY(LoopClipRules::validateRange(5000, 5000).contains(QStringLiteral("晚于")));
        QVERIFY(LoopClipRules::validateRange(5000, 4000).contains(QStringLiteral("晚于")));

        const qint64 tiny = LoopClipRules::kMinClipDurationMs - 1;
        QVERIFY(LoopClipRules::validateRange(0, tiny).contains(QStringLiteral("太短")));
        QVERIFY(LoopClipRules::validateRange(0, LoopClipRules::kMinClipDurationMs).isEmpty());

        QVERIFY(LoopClipRules::validateRange(0, 6000, 5000).contains(QStringLiteral("超出")));
        QVERIFY(LoopClipRules::validateRange(0, 4000, 5000).isEmpty());
    }

    void validateClipChecksMediaPath()
    {
        LoopClip clip;
        QVERIFY(LoopClipRules::validateClip(clip).contains(QStringLiteral("没有关联视频")));

        clip.mediaPath = QStringLiteral("C:/media/a.mp4");
        clip.startMs = 0;
        clip.endMs = 1000;
        QVERIFY(LoopClipRules::validateClip(clip).isEmpty());
    }

    void shouldRewindHitsTheBPoint()
    {
        const qint64 start = 1000;
        const qint64 end = 5000;
        QVERIFY(!LoopClipRules::shouldRewind(4999, start, end));
        QVERIFY(LoopClipRules::shouldRewind(5000, start, end));
        QVERIFY(LoopClipRules::shouldRewind(5049, start, end));
        // Degenerate ranges never rewind.
        QVERIFY(!LoopClipRules::shouldRewind(5000, end, start));
        QVERIFY(!LoopClipRules::shouldRewind(5000, 5000, 5000));
    }

    void isOutsideRangeKeepsTolerance()
    {
        const qint64 start = 10000;
        const qint64 end = 20000;
        const qint64 tolerance = LoopClipRules::kExitToleranceMs;

        QVERIFY(!LoopClipRules::isOutsideRange(15000, start, end));
        QVERIFY(!LoopClipRules::isOutsideRange(start - tolerance + 1, start, end));
        QVERIFY(!LoopClipRules::isOutsideRange(end + tolerance - 1, start, end));
        QVERIFY(LoopClipRules::isOutsideRange(start - tolerance - 1, start, end));
        QVERIFY(LoopClipRules::isOutsideRange(end + tolerance + 1, start, end));
        QVERIFY(LoopClipRules::isOutsideRange(0, end, start));
    }

    void timecodeRoundTrip()
    {
        QCOMPARE(LoopClipRules::formatTimecode(0), QStringLiteral("00:00.000"));
        QCOMPARE(LoopClipRules::formatTimecode(65001), QStringLiteral("01:05.001"));
        QCOMPARE(LoopClipRules::formatTimecode(599999), QStringLiteral("09:59.999"));

        qint64 value = -1;
        QVERIFY(LoopClipRules::parseTimecode(QStringLiteral("01:05.001"), &value));
        QCOMPARE(value, 65001LL);
        QVERIFY(LoopClipRules::parseTimecode(QStringLiteral(" 12 "), &value));
        QCOMPARE(value, 12000LL);
        QVERIFY(LoopClipRules::parseTimecode(QStringLiteral("1:02,5"), &value));
        QCOMPARE(value, 62500LL);
        QVERIFY(!LoopClipRules::parseTimecode(QStringLiteral("abc"), &value));
        QVERIFY(!LoopClipRules::parseTimecode(QString(), &value));
    }

    void defaultTitleUsesVideoName()
    {
        const QString title = LoopClipRules::defaultTitle(
            QStringLiteral("D:/media/Example Clip.mp4"), 12000, 45000);
        QCOMPARE(title, QStringLiteral("Example Clip · 00:12.000–00:45.000"));
    }
};

QTEST_MAIN(LoopClipTest)
#include "test_loop_clip.moc"
