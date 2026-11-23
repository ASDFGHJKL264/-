#include "acquisitioncontroller.h"
#include "monitoringcontroller.h"
#include "historyquery.h"
#include "asynclogger.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QSqlQuery>
#include <QFile>
#include <QEvent>
#include <QPointer>
#include <QSerialPort>
#include <algorithm>
#include <limits>

class FakeTransport final : public SensorTransport {
public:
    bool opened = false, automatic = true;
    bool openSucceeds = true, connectImmediately = true, disconnectOnClose = false;
    int delayMs = 1, failSlave = -1;
    int openCalls = 0, closeCalls = 0;
    double temperature = 25, humidity = 50;
    QList<quint64> ids;
    QList<int> slaves;
    QStringList openedPorts;
    bool open(const AppConfig &c) override {
        ++openCalls;
        openedPorts.append(c.port);
        opened = openSucceeds && connectImmediately;
        if (opened) emit connected();
        return openSucceeds;
    }
    void close() override {
        ++closeCalls;
        opened = false;
        if (disconnectOnClose) emit disconnected("closed synchronously");
    }
    bool isReady() const override { return opened; }
    void connectNow() { opened = true; emit connected(); }
    void drop() { opened = false; emit disconnected("unplugged"); }
    void read(quint64 id, const SensorConfig &sensor) override {
        ids.append(id); slaves.append(sensor.slaveId);
        if (automatic) QTimer::singleShot(delayMs, this, [this, id, sensor, t = temperature, h = humidity] {
            emit completed(id, sensor.slaveId != failSlave, t, h, "injected timeout");
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
    void businessCommandsUseWorkerAndStopAfterShutdown() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto c = config();
        c.simulatorEnabled = true;
        MonitoringController runtime(dir.filePath("commands.db"), c);
        auto *commands = runtime.monitor();
        QSignalSpy ready(&runtime, &MonitoringController::ready);
        QSignalSpy views(commands, &MonitorController::viewReady);
        QSignalSpy tasks(commands, &MonitorController::taskChanged);
        QSignalSpy reports(commands, &MonitorController::reportReady);
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(ready.first()[0].toBool());
        QTRY_VERIFY(!views.isEmpty());
        auto rules = views.last()[0].toMap()["rules"].toList();
        auto rule = rules[0].toMap();
        rule["name"] = "configured through controller";
        rules[0] = rule;
        commands->configure(rules);
        QTRY_COMPARE(views.last()[0].toMap()["rules"].toList()[0].toMap()["name"].toString(),
                     QString("configured through controller"));
        QVERIFY(runtime.start(c));
        commands->startTask("=task,\"A\"\nB", {0}, c.pollIntervalMs);
        QTRY_COMPARE(tasks.size(), 1);
        QVERIFY(tasks.first()[0].toBool());
        QTRY_VERIFY(!views.last()[0].toMap()["tasks"].toList().isEmpty());
        const auto id = views.last()[0].toMap()["tasks"].toList()[0].toMap()["id"].toLongLong();
        commands->finishTask();
        QTRY_COMPARE(tasks.size(), 2);
        QVERIFY(!tasks.last()[0].toBool());
        commands->report(id);
        QTRY_COMPARE(reports.size(), 1);
        QVERIFY(reports.first()[0].toString().contains("\"'=task,\"\"A\"\"\nB\""));
        runtime.shutdown();
        QCoreApplication::processEvents();
        const auto count = views.size();
        commands->reloadRecords();
        commands->startTask("after shutdown", {0}, 100);
        commands->report(id);
        QCoreApplication::processEvents();
        QCOMPARE(views.size(), count);
        QCOMPARE(tasks.size(), 2);
        QCOMPARE(reports.size(), 1);
    }
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
    void realRtuTransportIgnoresRetiredClientCallbacks() {
        RtuTransport transport;
        QSignalSpy connectedNotifications(&transport, &SensorTransport::connected);
        QSignalSpy disconnectedNotifications(&transport, &SensorTransport::disconnected);
        auto settings = config();
        // An intentionally invalid name: never enumerate or open a user's physical port.
        settings.port = "CODEX_RECONNECT_TEST_PORT_DOES_NOT_EXIST_7F39C1";
        QVERIFY(!transport.open(settings));
        QPointer<QModbusRtuSerialClient> oldClient =
            transport.findChild<QModbusRtuSerialClient *>();
        QVERIFY(oldClient);
        auto *oldSerial = qobject_cast<QSerialPort *>(oldClient->device());
        QVERIFY(oldSerial);
        transport.close();
        QVERIFY(!transport.open(settings));
        QPointer<QModbusRtuSerialClient> currentClient;
        for (auto *client : transport.findChildren<QModbusRtuSerialClient *>())
            if (client != oldClient.data()) currentClient = client;
        QVERIFY(currentClient);
        QVERIFY(oldClient); // close() queued deletion; no event processing has run yet.
        connectedNotifications.clear();
        disconnectedNotifications.clear();

        emit oldClient->stateChanged(QModbusDevice::ConnectedState);
        emit oldClient->stateChanged(QModbusDevice::UnconnectedState);
        emit oldClient->errorOccurred(QModbusDevice::ConnectionError);
        // Unlike the client connections, the old serial's transport callback can still
        // exist until DeferredDelete: the generation guard must reject this too.
        emit oldSerial->errorOccurred(QSerialPort::ResourceError);
        QCOMPARE(connectedNotifications.size(), 0);
        QCOMPARE(disconnectedNotifications.size(), 0);
        QVERIFY(!transport.isReady());

        emit currentClient->stateChanged(QModbusDevice::UnconnectedState);
        QCOMPARE(disconnectedNotifications.size(), 1);
        emit currentClient->errorOccurred(QModbusDevice::ConnectionError);
        QCOMPARE(disconnectedNotifications.size(), 2);
        emit currentClient->errorOccurred(QModbusDevice::TimeoutError);
        QCOMPARE(disconnectedNotifications.size(), 2); // A slave timeout is not a bus drop.
        transport.close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(oldClient.isNull());
        QVERIFY(currentClient.isNull());
        QVERIFY(transport.findChildren<QModbusRtuSerialClient *>().isEmpty());
        QCOMPARE(connectedNotifications.size(), 0);
        QCOMPARE(disconnectedNotifications.size(), 2);
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
        auto settings = config();
        settings.autoReconnect = false; // Explicitly retain the manual-reconnect policy here.
        QVERIFY(c.start(settings)); QTRY_COMPARE(transport->ids.size(), 1);
        auto old = transport->ids.first();
        c.stop(); QVERIFY(c.start(settings)); QTRY_COMPARE(transport->ids.size(), 2);
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
    void reconnectKeepsSessionAndRejectsOldReply() {
        auto *transport = new FakeTransport;
        transport->automatic = false;
        transport->disconnectOnClose = true; // close() may synchronously report disconnection.
        AcquisitionController c(nullptr, transport);
        QSignalSpy sessions(&c, &AcquisitionController::sessionChanged);
        QSignalSpy samples(&c, &AcquisitionController::sampleReady);
        QSignalSpy lost(&c, &AcquisitionController::sampleLost);
        QVERIFY(c.start(config()));
        QTRY_COMPARE(transport->ids.size(), 1);
        const auto oldId = transport->ids.first();
        transport->drop();
        QVERIFY(c.active());
        QCOMPARE(c.connectionState(), AcquisitionController::Reconnecting);
        QCOMPARE(sessions.size(), 1);
        QCOMPARE(lost.size(), 4);
        QCOMPARE(c.m_reconnectTimer.interval(), 1000);
        QTest::qWait(20);
        emit transport->connected(); // Stale notification without a usable transport.
        QCOMPARE(c.connectionState(), AcquisitionController::Reconnecting);
        emit transport->completed(oldId, true, 99, 50, {});
        QCOMPARE(samples.size(), 0);
        c.m_reconnectTimer.start(0); // Advance the retry without sleeping for production backoff.
        QTRY_COMPARE(transport->openCalls, 2);
        QTRY_COMPARE(transport->ids.size(), 2);
        QVERIFY(transport->ids.last() != oldId);
        emit transport->completed(oldId, true, 99, 50, {});
        QCOMPARE(samples.size(), 0);
        emit transport->completed(transport->ids.last(), true, 26, 51, {});
        QCOMPARE(samples.size(), 1);
        QCOMPARE(sessions.size(), 1); // Reconnection is not another acquisition session.
        QCOMPARE(c.connectionState(), AcquisitionController::Connected);
        QCOMPARE(c.metrics()["reconnectAttempts"].toInt(), 1);
        QCOMPARE(c.metrics()["reconnectSuccesses"].toInt(), 1);
        QVERIFY(c.metrics()["offlineMs"].toLongLong() >= 20);
        QCOMPARE(transport->openedPorts, QStringList({"TEST", "TEST"}));
        c.stop();
        QCOMPARE(sessions.size(), 2);
        QVERIFY(!sessions.last()[0].toBool());
    }
    void retryBackoffCapsAndManualStopCancels() {
        auto *transport = new FakeTransport;
        transport->automatic = false;
        transport->disconnectOnClose = true;
        AcquisitionController c(nullptr, transport);
        QVERIFY(c.start(config()));
        transport->drop();
        transport->openSucceeds = false;
        const QList<int> delays{1000, 2000, 4000, 8000, 16000, 30000, 30000};
        for (int i = 0; i < delays.size(); ++i) {
            QCOMPARE(c.connectionState(), AcquisitionController::Reconnecting);
            QCOMPARE(c.m_reconnectTimer.interval(), delays[i]);
            QCOMPARE(c.metrics()["reconnectDelayMs"].toInt(), delays[i]);
            emit transport->disconnected("duplicate error");
            QCOMPARE(c.m_reconnectTimer.interval(), delays[i]);
            c.m_reconnectTimer.start(0);
            QTRY_COMPARE(transport->openCalls, i + 2);
        }
        QCOMPARE(c.metrics()["reconnectAttempts"].toInt(), delays.size());
        c.stop();
        QVERIFY(!c.active());
        QVERIFY(!c.m_reconnectTimer.isActive());
        QCOMPARE(c.connectionState(), AcquisitionController::Stopped);
        const auto attempts = transport->openCalls;
        const auto offline = c.metrics()["offlineMs"].toLongLong();
        transport->connectNow(); // Late connected after cancel must not resume polling.
        emit transport->disconnected("late disconnected");
        QTest::qWait(60);
        QCOMPARE(transport->openCalls, attempts);
        QCOMPARE(c.metrics()["offlineMs"].toLongLong(), offline);
        QVERIFY(!c.active());
    }
    void asyncConnectTimeoutAndStopDuringConnecting() {
        auto *transport = new FakeTransport;
        transport->connectImmediately = false;
        transport->disconnectOnClose = true;
        AcquisitionController c(nullptr, transport);
        QVERIFY(c.start(config()));
        QCOMPARE(c.connectionState(), AcquisitionController::Connecting);
        c.m_connectDeadline = 0;
        c.poll();
        QCOMPARE(c.connectionState(), AcquisitionController::Reconnecting);
        c.m_reconnectTimer.start(0);
        QTRY_COMPARE(transport->openCalls, 2);
        QCOMPARE(c.connectionState(), AcquisitionController::Connecting);
        c.stop();
        transport->connectNow();
        QTest::qWait(60);
        QVERIFY(transport->ids.isEmpty());
        QCOMPARE(transport->openCalls, 2);
        QVERIFY(!c.active());
        QVERIFY(!c.m_reconnectTimer.isActive());
    }
    void initialOpenFailureAndDisabledReconnectStayStopped() {
        auto *transport = new FakeTransport;
        transport->openSucceeds = false;
        transport->disconnectOnClose = true;
        AcquisitionController c(nullptr, transport);
        QSignalSpy sessions(&c, &AcquisitionController::sessionChanged);
        QVERIFY(!c.start(config()));
        QVERIFY(!c.active());
        QVERIFY(!c.m_reconnectTimer.isActive());
        QCOMPARE(sessions.size(), 0);
        transport->openSucceeds = true;
        auto settings = config(); settings.autoReconnect = false;
        QVERIFY(c.start(settings));
        transport->drop();
        QVERIFY(!c.active());
        QVERIFY(!c.m_reconnectTimer.isActive());
        QCOMPARE(sessions.size(), 2);
        QCOMPARE(c.metrics()["reconnectAttempts"].toInt(), 0);
    }
    void reconnectPreservesCreditsAndChecksDeadlineBeforeBackpressure() {
        auto *transport = new FakeTransport;
        AcquisitionController c(nullptr, transport);
        QSignalSpy samples(&c, &AcquisitionController::sampleReady);
        QVERIFY(c.start(config()));
        QTRY_COMPARE_WITH_TIMEOUT(samples.size(), 64, 5000);
        transport->drop();
        QCOMPARE(c.metrics()["pending"].toInt(), 64);
        transport->connectImmediately = false;
        c.m_reconnectTimer.start(0);
        QTRY_COMPARE(transport->openCalls, 2);
        c.m_connectDeadline = 0;
        c.poll(); // Full storage credits must not hide an expired connection attempt.
        QCOMPARE(c.connectionState(), AcquisitionController::Reconnecting);
        QCOMPARE(c.metrics()["pending"].toInt(), 64);
        transport->connectImmediately = true;
        c.m_reconnectTimer.start(0);
        QTRY_COMPARE(transport->openCalls, 3);
        QTest::qWait(60);
        QCOMPARE(samples.size(), 64);
        c.sampleProcessed();
        QTRY_COMPARE(samples.size(), 65);
        QCOMPARE(c.metrics()["pending"].toInt(), 64);
        c.stop();
    }
    void simulatorAndSlaveTimeoutDoNotReconnect() {
        auto *transport = new FakeTransport;
        transport->failSlave = 2;
        AcquisitionController c(nullptr, transport);
        connect(&c, &AcquisitionController::sampleReady, &c, &AcquisitionController::sampleProcessed);
        QVERIFY(c.start(config()));
        QTRY_VERIFY(c.metrics()["failedReads"].toInt() >= 2);
        QCOMPARE(transport->openCalls, 1);
        QCOMPARE(c.metrics()["reconnectAttempts"].toInt(), 0);
        c.stop();
        auto settings = config(); settings.simulatorEnabled = true;
        QVERIFY(c.start(settings));
        emit transport->disconnected("irrelevant hardware event");
        QTRY_VERIFY(c.metrics()["successfulReads"].toInt() > 0);
        QVERIFY(c.active());
        QCOMPARE(transport->openCalls, 1);
        QCOMPARE(c.metrics()["reconnectAttempts"].toInt(), 0);
        c.stop();
    }
    void shutdownDuringRetryCannotReopen() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        auto *transport = new FakeTransport;
        transport->disconnectOnClose = true;
        MonitoringController runtime(dir.filePath("cancel-retry.db"), config(), nullptr, transport);
        QSignalSpy ready(&runtime, &MonitoringController::ready);
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(runtime.start(config()));
        transport->drop();
        QVERIFY(runtime.metrics()["reconnectAttempts"].toInt() == 0);
        runtime.shutdown();
        const auto opens = transport->openCalls;
        transport->connectNow();
        QTest::qWait(1100); // One actual first-delay interval proves the timer was cancelled.
        QCOMPARE(transport->openCalls, opens);
        QVERIFY(!runtime.start(config()));
    }
    void reconnectKeepsTaskAndUnrecoveredAlarmInDatabase() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        auto settings = config();
        for (int i = 1; i < 4; ++i) settings.sensors[i].enabled = false;
        auto *transport = new FakeTransport;
        transport->temperature = 80;
        const auto path = dir.filePath("reconnect-business.db");
        MonitoringController runtime(path, settings, nullptr, transport);
        auto *commands = runtime.monitor();
        QSignalSpy ready(&runtime, &MonitoringController::ready);
        QSignalSpy sessions(&runtime, &MonitoringController::sessionChanged);
        QSignalSpy views(commands, &MonitorController::viewReady);
        QSignalSpy tasks(commands, &MonitorController::taskChanged);
        QSignalSpy statuses(commands, &MonitorController::statusReady);
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(ready.first()[0].toBool());
        QTRY_VERIFY(!views.isEmpty());
        auto rules = views.last()[0].toMap()["rules"].toList();
        auto first = rules[0].toMap(); first["holdMs"] = 0; rules[0] = first;
        commands->configure(rules);
        QVERIFY(runtime.start(settings));
        commands->startTask("reconnect continuity", {0}, settings.pollIntervalMs);
        QTRY_COMPARE(tasks.size(), 1);
        QTRY_VERIFY(!views.last()[0].toMap()["alarms"].toList().isEmpty());

        auto db = QSqlDatabase::addDatabase("QSQLITE", "reconnect-inspector");
        db.setDatabaseName(path); QVERIFY(db.open());
        const auto value = [&db](const QString &sql) {
            QSqlQuery query(db);
            if (!query.exec(sql) || !query.next()) return QVariant();
            return query.value(0);
        };
        const auto taskId = value("SELECT id FROM monitor_tasks WHERE status='running'").toLongLong();
        QVERIFY(taskId > 0);
        const auto before = value("SELECT samples FROM monitor_task_zones WHERE zone=0").toLongLong();
        QVERIFY(before > 0);
        statuses.clear();
        transport->drop();
        commands->reloadRecords();
        QTRY_VERIFY(std::any_of(statuses.cbegin(), statuses.cend(), [](const QList<QVariant> &row) {
            return row[0].toInt() == 0 && row[2].toString().contains("失效");
        }));
        QCOMPARE(sessions.size(), 1);
        QCOMPARE(tasks.size(), 1);
        QCOMPARE(value("SELECT COUNT(*) FROM monitor_alarms WHERE recovered_at IS NULL").toInt(), 1);
        QCOMPARE(value("SELECT id FROM monitor_tasks WHERE status='running'").toLongLong(), taskId);
        QTRY_COMPARE_WITH_TIMEOUT(runtime.metrics()["reconnectSuccesses"].toInt(), 1, 3000);
        QTRY_VERIFY(value("SELECT samples FROM monitor_task_zones WHERE zone=0").toLongLong() > before);
        QCOMPARE(sessions.size(), 1);
        QCOMPARE(tasks.size(), 1);
        QCOMPARE(value("SELECT COUNT(*) FROM monitor_alarms").toInt(), 1);
        QCOMPARE(value("SELECT COUNT(*) FROM monitor_alarms WHERE recovered_at IS NULL").toInt(), 1);
        QCOMPARE(value("SELECT id FROM monitor_tasks WHERE status='running'").toLongLong(), taskId);
        commands->finishTask();
        QTRY_COMPARE(tasks.size(), 2);
        const auto samples = value("SELECT samples FROM monitor_task_zones WHERE zone=0").toLongLong();
        const auto coveredSlots = value("SELECT slots FROM monitor_task_zones WHERE zone=0").toLongLong();
        const auto elapsed = value("SELECT elapsed_ms FROM monitor_tasks").toLongLong();
        QVERIFY(samples > before);
        QVERIFY(coveredSlots < (elapsed + settings.pollIntervalMs - 1) / settings.pollIntervalMs);
        runtime.shutdown();
        db.close(); db = {}; QSqlDatabase::removeDatabase("reconnect-inspector");
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
