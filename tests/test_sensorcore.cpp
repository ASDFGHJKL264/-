#include "sensorutils.h"
#include "serialportpolicy.h"
#include "appconfig.h"
#include <limits>

#include <QSettings>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QScopeGuard>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include <QtTest>

class SensorCoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void crc16();
    void serialPortSelection();
    void serialPortPersistence();
    void temperatureParsing_data();
    void temperatureParsing();
    void configReadWrite();
    void configAtomicFailureAndUnknownKeys();
    void configDefaultsAndClamping();
    void alarmThreshold_data();
    void alarmThreshold();
};

void SensorCoreTest::serialPortSelection()
{
    using SerialPortPolicy::preferred;
    QCOMPARE(preferred({}, "", "COM1", false), QString());
    QCOMPARE(preferred({"COM3"}, "", "COM1", false), QString("COM3"));
    QCOMPARE(preferred({"COM3", "COM5"}, "COM5", "COM3", false), QString("COM5"));
    QCOMPARE(preferred({"COM3", "COM5"}, "COM9", "COM5", false), QString("COM5"));
    QCOMPARE(preferred({"COM3"}, "COM5", "COM5", false), QString("COM3"));
    QCOMPARE(preferred({}, "COM5", "COM5", true), QString("SIMULATOR"));
    QCOMPARE(preferred({"COM3"}, "SIMULATOR", "COM3", false), QString("COM3"));
}

void SensorCoreTest::serialPortPersistence()
{
    using SerialPortPolicy::savedPort;
    QCOMPARE(savedPort({}, "", "COM5", false), QString("COM5"));
    QCOMPARE(savedPort({}, "unavailable", "COM5", false), QString("COM5"));
    QCOMPARE(savedPort({"COM3"}, "SIMULATOR", "COM5", true), QString("COM5"));
    QCOMPARE(savedPort({"COM3"}, "COM3", "COM5", true), QString("COM5"));
    QCOMPARE(savedPort({"COM3"}, "COM9", "COM5", false), QString("COM5"));
    QCOMPARE(savedPort({"COM3"}, "COM3", "COM5", false), QString("COM3"));
}

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
    auto config = ConfigStore::load(path);
    config.port = "COM8"; config.baud = 19200;
    config.sensors[1].enabled = true; config.sensors[1].slaveId = 7;
    QString error;
    QVERIFY2(ConfigStore::save(path, config, &error), qPrintable(error));
    const auto read = ConfigStore::load(path);
    QCOMPARE(read.port, QString("COM8")); QCOMPARE(read.baud, 19200);
    QVERIFY(read.sensors[1].enabled); QCOMPARE(read.sensors[1].slaveId, 7);
    config.sensors[1].slaveId = config.sensors[0].slaveId;
    QVERIFY(!ConfigStore::save(path, config, &error));
    QCOMPARE(ConfigStore::load(path).sensors[1].slaveId, 7);
}

void SensorCoreTest::configAtomicFailureAndUnknownKeys()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("atomic.ini");
    auto config = ConfigStore::load(path);
    config.port = "COM8";
    QString error;
    QVERIFY2(ConfigStore::save(path, config, &error), qPrintable(error));
    QSettings reader(path, QSettings::IniFormat);
    reader.setValue("Extension/keep", "unknown-key");
    reader.sync();
    const auto readBytes = [&path] {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return QByteArray();
        return file.readAll();
    };
    const auto previous = readBytes();
    QVERIFY(!previous.isEmpty());
    auto rejected = config;
    rejected.sensors[1].enabled = true;
    rejected.sensors[1].slaveId = rejected.sensors[0].slaveId;
    QVERIFY(!ConfigStore::save(path, rejected, &error));
    QCOMPARE(readBytes(), previous);
    reader.sync();
    QCOMPARE(reader.value("Serial/port").toString(), QString("COM8"));

    auto candidate = config;
    candidate.port = "COM12";
    candidate.sensors[0].name = "原子提交新名称";
#ifdef Q_OS_WIN
    {
        // Deny replacement of the live file, but allow readers. This forces
        // the final atomic commit to fail even with elevated test privileges.
        const HANDLE locked = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                                           FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(locked != INVALID_HANDLE_VALUE);
        const auto unlock = qScopeGuard([locked] { CloseHandle(locked); });
        QVERIFY(!ConfigStore::save(path, candidate, &error));
        QCOMPARE(readBytes(), previous);
        reader.sync();
        QCOMPARE(reader.value("Serial/port").toString(), QString("COM8"));
        QCOMPARE(ConfigStore::load(path).port, QString("COM8"));
    }
#endif
    QVERIFY(ConfigStore::save(path, candidate, &error));
    reader.sync();
    QCOMPARE(reader.value("Serial/port").toString(), QString("COM12"));
    QCOMPARE(reader.value("Extension/keep").toString(), QString("unknown-key"));
    QCOMPARE(ConfigStore::load(path).sensors[0].name, candidate.sensors[0].name);
    const auto committed = readBytes();
    QVERIFY(QDir(directory.path()).mkdir("blocked.ini"));
    QVERIFY(!ConfigStore::save(directory.filePath("blocked.ini"), candidate, &error));
    QCOMPARE(readBytes(), committed);
}

void SensorCoreTest::configDefaultsAndClamping()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath("missing.ini"), QSettings::IniFormat);
    QCOMPARE(ConfigStore::load(settings.fileName()).port, QString("COM1"));
    QCOMPARE(ConfigStore::load(settings.fileName()).pollIntervalMs, 500);
    settings.setValue("Acquisition/intervalMs", 10);
    settings.sync();
    auto config = ConfigStore::load(settings.fileName());
    QCOMPARE(config.pollIntervalMs, 100);
    QString error;
    config.temperatureMin = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(!ConfigStore::validate(config, &error));
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
