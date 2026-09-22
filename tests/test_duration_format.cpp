#include "core/DurationFormat.h"

#include <QtTest>

class DurationFormatTest : public QObject
{
    Q_OBJECT

private slots:
    void belowOneHourUsesMinutesOnly()
    {
        QCOMPARE(DurationFormat::clock(0), QStringLiteral("0:00"));
        QCOMPARE(DurationFormat::clock(1'000), QStringLiteral("0:01"));
        QCOMPARE(DurationFormat::clock(65'000), QStringLiteral("1:05"));
        QCOMPARE(DurationFormat::clock(59 * 60'000 + 59'000), QStringLiteral("59:59"));
    }

    // The player bar used QTime before, whose "mm" wraps at 60 - a 1 h 20 min
    // video showed "20:00" as its total length and a whole hour showed "00:00".
    void oneHourAndBeyondKeepsTheHours()
    {
        QCOMPARE(DurationFormat::clock(3'600'000), QStringLiteral("1:00:00"));
        QCOMPARE(DurationFormat::clock(3'900'000), QStringLiteral("1:05:00"));
        QCOMPARE(DurationFormat::clock(4'800'000), QStringLiteral("1:20:00"));
        QCOMPARE(DurationFormat::clock(11 * 3'600'000 + 59'000), QStringLiteral("11:00:59"));
    }

    void negativeInputIsClamped()
    {
        QCOMPARE(DurationFormat::clock(-1), QStringLiteral("0:00"));
        QCOMPARE(DurationFormat::clock(-5'000), QStringLiteral("0:00"));
    }

    void subSecondValuesTruncate()
    {
        QCOMPARE(DurationFormat::clock(999), QStringLiteral("0:00"));
        QCOMPARE(DurationFormat::clock(1'999), QStringLiteral("0:01"));
    }
};

QTEST_APPLESS_MAIN(DurationFormatTest)

#include "test_duration_format.moc"
