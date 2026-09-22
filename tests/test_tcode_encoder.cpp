#include "device/TCodeEncoder.h"

#include <QtTest>

class TCodeEncoderTest : public QObject
{
    Q_OBJECT

private slots:
    void encodeSixAxis()
    {
        QMap<int, AxisConfig> configs;
        for (Track track : TrackInfo::sr6Tracks())
            configs.insert(static_cast<int>(track), AxisConfig{});

        QList<AxisTarget> targets = {
            {Track::Stroke, 50, 100},
            {Track::Surge, 25, 200},
            {Track::Sway, 75, 300},
            {Track::Twist, 10, 400},
            {Track::Roll, 90, 500},
            {Track::Pitch, 60, 600}
        };

        const QString line = TCodeEncoder::encodeLine(targets, configs);
        QVERIFY(line.contains(QStringLiteral("L05000I100")));
        QVERIFY(line.contains(QStringLiteral("L12500I200")));
        QVERIFY(line.contains(QStringLiteral("L27499I300")));
        QVERIFY(line.contains(QStringLiteral("R01000I400")));
        QVERIFY(line.contains(QStringLiteral("R18999I500")));
        QVERIFY(line.contains(QStringLiteral("R25999I600")));
        QVERIFY(!line.contains(QLatin1Char('G')));
    }

    void stopCommand()
    {
        QCOMPARE(TCodeEncoder::stopCommand(), QStringLiteral("DSTOP"));
    }

    // The reset / end-of-video home position follows the axis' calibrated home
    // instead of a hard coded 50 %, including on an inverted axis.
    void percentForHomeFollowsTheCalibratedValue()
    {
        AxisConfig config; // 0..9999, home 5000 by default
        QCOMPARE(TCodeEncoder::percentForHome(config), 50);

        config.home = 7500;
        QCOMPARE(TCodeEncoder::percentForHome(config), 75);
        // Round tripping through the encoder lands on the home value.
        QCOMPARE(TCodeEncoder::encodeLine({{Track::Stroke, TCodeEncoder::percentForHome(config), 100}},
                                          {{static_cast<int>(Track::Stroke), config}}),
                 QStringLiteral("L07499I100"));

        config.home = 2500;
        config.inverted = true;
        const int invertedPercent = TCodeEncoder::percentForHome(config);
        QCOMPARE(invertedPercent, 75);
        QCOMPARE(TCodeEncoder::encodeLine({{Track::Stroke, invertedPercent, 100}},
                                          {{static_cast<int>(Track::Stroke), config}}),
                 QStringLiteral("L02500I100"));
    }

    void percentForHomeFallsBackToTheMiddleOnADegenerateRange()
    {
        AxisConfig flat;
        flat.min = 4000;
        flat.max = 4000;
        QCOMPARE(TCodeEncoder::percentForHome(flat), 50);
    }
};

QTEST_APPLESS_MAIN(TCodeEncoderTest)
#include "test_tcode_encoder.moc"
