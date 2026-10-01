#include "acquisitioncontroller.h"
#include "monitoringcontroller.h"
#include "historyquery.h"
#include "asynclogger.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QSqlQuery>
#include <QFile>
#include <limits>

class FakeTransport final : public SensorTransport {
public:
    bool opened = false, automatic = true;
    int delayMs = 1, failSlave = -1;
    QList<quint64> ids;
    QList<int> slaves;
    bool open(const AppConfig &) override { opened = true; emit connected(); return true; }
    void close() override { opened = false; }
    bool isReady() const override { return opened; }
    void read(quint64 id, const SensorConfig &sensor) override {
        ids.append(id); slaves.append(sensor.slaveId);
        if (automatic) QTimer::singleShot(delayMs, this, [this, id, sensor] {
            emit completed(id, sensor.slaveId != failSlave, 25, 50, "injected timeout");
        });
    }
};

static AppConfig config()
{
    AppConfig c; c.port = "TEST"; c.pollIntervalMs = 100;
    for (int i = 0; i < 4; ++i) {
        c.sensors[i].enabled = true; c.sensors[i].slaveId = i + 1;
        c.sensors[i].name = QString("区域%1").arg(i + 1);
    }
    return c;
}

class AcquisitionTests : public QObject {
    Q_OBJECT
private slots:
    void immediateShutdownCannotRestart() {
        QTemporaryDir dir;
        MonitoringController controller(dir.filePath("early.db"), config(), nullptr, new FakeTransport);
        QSignalSpy ready(&controller, &MonitoringController::ready);
        controller.shutdown();
        QCOMPARE(ready.size(), 0);
        QVERIFY(!controller.start(config()));
        QVERIFY(controller.service() == nullptr);
        controller.shutdown();
    }
    void roundRobinAndFailureIsolation() {
        auto *transport = new FakeTransport;
        transport->failSlave = 2;
        AcquisitionController c(nullptr, transport);
        connect(&c, &AcquisitionController::sampleReady, &c, &AcquisitionController::sampleProcessed);
        QSignalSpy lost(&c, &AcquisitionController::sampleLost);
        QVERIFY(c.start(config()));
        QTRY_VERIFY(transport->slaves.size() >= 12);
        c.stop();
        for (int i = 0; i < 12; ++i) QCOMPARE(transport->slaves[i], i % 4 + 1);
        QVERIFY(lost.size() >= 2);
        for (const auto &row : lost) QCOMPARE(row[0].toInt(), 1);
        QVERIFY(c.metrics()["successfulReads"].toULongLong() >= 8);
        qInfo() << "round-robin metrics" << c.metrics();
    }
    void storageBackpressureAndRestart() {
        auto *transport = new FakeTransport;
        AcquisitionController c(nullptr, transport);
        QSignalSpy samples(&c, &AcquisitionController::sampleReady);
        QVERIFY(c.start(config()));
        QTRY_COMPARE_WITH_TIMEOUT(samples.size(), 64, 5000);
        QTest::qWait(100);
        QCOMPARE(transport->ids.size(), 64);
        QCOMPARE(c.metrics()["peakPending"].toInt(), 64);
        QVERIFY(c.metrics()["backpressureChecks"].toULongLong() > 0);
        c.stop(); QVERIFY(c.start(config()));
        QTest::qWait(100); QCOMPARE(samples.size(), 64);
        c.sampleProcessed();
        QTRY_COMPARE(samples.size(), 65);
        c.stop();
        QCOMPARE(c.metrics()["pending"].toInt(), 64);
    }
    void lateInvalidAndDisconnect() {
        auto *transport = new FakeTransport; transport->automatic = false;
        AcquisitionController c(nullptr, transport);
        QSignalSpy samples(&c, &AcquisitionController::sampleReady);
        QSignalSpy lost(&c, &AcquisitionController::sampleLost);
        QVERIFY(c.start(config())); QTRY_COMPARE(transport->ids.size(), 1);
        auto old = transport->ids.first();
        c.stop(); QVERIFY(c.start(config())); QTRY_COMPARE(transport->ids.size(), 2);
        emit transport->completed(old, true, 80, 50, {});
        QCOMPARE(samples.size(), 0);
        emit transport->completed(transport->ids.last(), true,
                                  std::numeric_limits<double>::quiet_NaN(), 50, {});
        QCOMPARE(lost.size(), 1); QCOMPARE(samples.size(), 0);
        QTRY_COMPARE(transport->ids.size(), 3);
        emit transport->completed(transport->ids.last(), true, 25, 50, {});
        QCOMPARE(samples.size(), 1);
        emit transport->disconnected("unplugged");
        QVERIFY(!c.active());
        const auto count = transport->ids.size();
        QTest::qWait(100); QCOMPARE(transport->ids.size(), count);
    }
    void pipelineStorageFailureAndRecovery() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto path = dir.filePath("environment.db");
        MonitoringController controller(path, config(), nullptr, new FakeTransport);
        QSignalSpy ready(&controller, &MonitoringController::ready);
        QTRY_COMPARE(ready.size(), 1); QVERIFY(ready.first()[0].toBool());
        auto db = QSqlDatabase::addDatabase("QSQLITE", "pipeline-reader");
        db.setDatabaseName(path); QVERIFY(db.open());
        {
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TRIGGER fail_sample BEFORE INSERT ON sensor_history BEGIN SELECT RAISE(ABORT,'injected failure'); END"));
        }
        QVERIFY(controller.start(config()));
        QTRY_VERIFY(controller.metrics()["storageFailed"].toULongLong() >= 4);
        QCOMPARE(controller.metrics()["saved"].toULongLong(), quint64(0));
        {
            QSqlQuery q(db); QVERIFY(q.exec("SELECT COUNT(*) FROM sensor_history"));
            QVERIFY(q.next()); QCOMPARE(q.value(0).toInt(), 0); q.finish();
            QVERIFY(q.exec("DROP TRIGGER fail_sample"));
        }
        QTRY_VERIFY(controller.metrics()["saved"].toULongLong() >= 8);
        controller.shutdown();
        const auto m = controller.metrics();
        QCOMPARE(m["pending"].toInt(), 0);
        QCOMPARE(m["successfulReads"].toULongLong(),
                 m["saved"].toULongLong() + m["storageFailed"].toULongLong());
        {
            QSqlQuery q(db); QVERIFY(q.exec("SELECT COUNT(*) FROM sensor_history"));
            QVERIFY(q.next()); QCOMPARE(q.value(0).toULongLong(), m["saved"].toULongLong());
        }
        qInfo() << "pipeline metrics" << m;
        db.close(); db = {}; QSqlDatabase::removeDatabase("pipeline-reader");
    }
    void historyPagingAndLoggerBound() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto path = dir.filePath("history.db");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "seed-reader");
            db.setDatabaseName(path); QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TABLE sensor_history(sample_time TEXT,sensor_name TEXT,slave_id INTEGER,temperature REAL,humidity REAL,sensor_index INTEGER)"));
            QVERIFY(db.transaction());
            for (int i = 0; i < 501; ++i)
                QVERIFY(q.exec("INSERT INTO sensor_history VALUES('2026-09-30T00:00:00.000','sensor',1,25,50,0)"));
            QVERIFY(db.commit());
        }
        QSqlDatabase::removeDatabase("seed-reader");
        HistoryQuery query(path);
        QSignalSpy finished(&query, &HistoryQuery::finished);
        QVERIFY(query.run(HistoryQuery::Samples, "2026", "2027"));
        QVERIFY(!query.run(HistoryQuery::Samples, "2026", "2027"));
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished[0][3].toString().isEmpty());
        QCOMPARE(finished[0][1].toList().size(), 500); QVERIFY(finished[0][2].toBool());
        QVERIFY(query.run(HistoryQuery::Samples, "2026", "2027", -1, 500));
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(finished[1][1].toList().size(), 1); QVERIFY(!finished[1][2].toBool());
        {
            AsyncLogger logger(dir.filePath("logs"));
            for (int i = 0; i < 1000; ++i) logger.append(QString::number(i));
            QCOMPARE(logger.dropped(), quint64(744));
        }
        QFile log(dir.filePath("logs/application.log")); QVERIFY(log.open(QIODevice::ReadOnly));
        QCOMPARE(log.readAll().count('\n'), 256);
    }
};
QTEST_GUILESS_MAIN(AcquisitionTests)
#include "test_acquisition.moc"
