#include "sensorutils.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

class SensorCoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void crc16();
    void temperatureParsing_data();
    void temperatureParsing();
    void configReadWrite();
    void configDefaultsAndClamping();
    void alarmThreshold_data();
    void alarmThreshold();
};

void SensorCoreTest::crc16()
{
    const QByteArray request = QByteArray::fromHex("010300000002");
    QCOMPARE(SensorUtils::modbusCrc16(request), quint16(0x0BC4));
    const quint16 crc = SensorUtils::modbusCrc16(request);
    QCOMPARE(quint8(crc & 0xFF), quint8(0xC4));
    QCOMPARE(quint8(crc >> 8), quint8(0x0B));
}

void SensorCoreTest::temperatureParsing_data()
{
    QTest::addColumn<quint16>("raw");
    QTest::addColumn<double>("expected");
    QTest::newRow("zero") << quint16(0) << 0.0;
    QTest::newRow("positive") << quint16(253) << 25.3;
    QTest::newRow("negative") << quint16(10125) << -12.5;
    QTest::newRow("negative-zero") << quint16(10000) << 0.0;
}

void SensorCoreTest::temperatureParsing()
{
    QFETCH(quint16, raw);
    QFETCH(double, expected);
    QCOMPARE(SensorUtils::parseTemperature(raw), expected);
}

void SensorCoreTest::configReadWrite()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("config.ini");
    {
        QSettings writer(path, QSettings::IniFormat);
        writer.setValue("Serial/port", "COM8");
        writer.setValue("Serial/baud", 19200);
        writer.setValue("Sensor2/enabled", true);
        writer.setValue("Sensor2/slaveId", 7);
        writer.sync();
        QCOMPARE(writer.status(), QSettings::NoError);
    }
    QSettings reader(path, QSettings::IniFormat);
    QCOMPARE(reader.value("Serial/port").toString(), QString("COM8"));
    QCOMPARE(reader.value("Serial/baud").toInt(), 19200);
    QCOMPARE(reader.value("Sensor2/enabled").toBool(), true);
    QCOMPARE(reader.value("Sensor2/slaveId").toInt(), 7);
}

void SensorCoreTest::configDefaultsAndClamping()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath("missing.ini"), QSettings::IniFormat);
    QCOMPARE(settings.value("Serial/port", "COM1").toString(), QString("COM1"));
    QCOMPARE(qBound(100, settings.value("Acquisition/intervalMs", 500).toInt(), 60000), 500);
    settings.setValue("Acquisition/intervalMs", 10);
    QCOMPARE(qBound(100, settings.value("Acquisition/intervalMs", 500).toInt(), 60000), 100);
}

void SensorCoreTest::alarmThreshold_data()
{
    QTest::addColumn<double>("value");
    QTest::addColumn<double>("minimum");
    QTest::addColumn<double>("maximum");
    QTest::addColumn<bool>("alarm");
    QTest::newRow("below") << -20.1 << -20.0 << 60.0 << true;
    QTest::newRow("minimum-boundary") << -20.0 << -20.0 << 60.0 << false;
    QTest::newRow("normal") << 25.0 << -20.0 << 60.0 << false;
    QTest::newRow("maximum-boundary") << 60.0 << -20.0 << 60.0 << false;
    QTest::newRow("above") << 60.1 << -20.0 << 60.0 << true;
}

void SensorCoreTest::alarmThreshold()
{
    QFETCH(double, value);
    QFETCH(double, minimum);
    QFETCH(double, maximum);
    QFETCH(bool, alarm);
    QCOMPARE(SensorUtils::isOutsideThreshold(value, minimum, maximum), alarm);
}

QTEST_GUILESS_MAIN(SensorCoreTest)
#include "test_sensorcore.moc"
