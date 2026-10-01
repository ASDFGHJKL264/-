#include "monitorcore.h"
#include "monitorservice.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QSqlQuery>
#include <QThread>
#include <QPointer>
#include <limits>
#include "databasemanager.h"

class MonitorBusinessTest : public QObject
{
    Q_OBJECT
  private slots:
    void transactionRollbackAndLockRecovery()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath("rollback.db");
        {
            DatabaseManager schema(path); QString error;
            QVERIFY(schema.initialize(&error));
        }
        auto db = QSqlDatabase::addDatabase("QSQLITE", "rollback-inspector");
        db.setDatabaseName(path); QVERIFY(db.open());
        QVariantList defaults;
        for (int i = 0; i < 4; ++i) {
            ZoneRule r; r.name = QString::number(i); r.holdMs = 0;
            defaults.append(MonitorService::encodeRule(r));
        }
        MonitorService service;
        service.initialize(path, defaults);
        service.session(true, true);
        service.startTask("rollback", {0}, 100);
        QSignalSpy completed(&service, &MonitorService::sampleStored);
        const auto send = [&] {
            service.sample(0, "sensor", 1, 80, 50, MonitorService::monotonicMs(),
                           QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
        };
        {
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TRIGGER reject_alarm BEFORE INSERT ON monitor_alarms BEGIN SELECT RAISE(ABORT,'injected'); END"));
            send(); QVERIFY(!completed.last()[0].toBool());
            QVERIFY(q.exec("SELECT COUNT(*) FROM sensor_history")); QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 0); q.finish();
            QVERIFY(q.exec("SELECT samples FROM monitor_task_zones")); QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 0); q.finish();
            QVERIFY(q.exec("DROP TRIGGER reject_alarm"));
            QVERIFY(q.exec("BEGIN IMMEDIATE"));
            QElapsedTimer elapsed; elapsed.start();
            send(); QVERIFY(!completed.last()[0].toBool());
            QVERIFY(elapsed.elapsed() < 1500);
            QVERIFY(q.exec("ROLLBACK"));
            send(); QVERIFY(completed.last()[0].toBool());
            QVERIFY(q.exec("SELECT COUNT(*) FROM monitor_alarms")); QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 1); q.finish();
            QVERIFY(q.exec("SELECT samples FROM monitor_task_zones")); QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 1);
        }
        service.session(false, true);
        QSignalSpy rejected(&service, &MonitorService::sessionRejected);
        service.session(true, false);
        QCOMPARE(rejected.size(), 1);
        service.session(false, false); // rejected source must not change task identity
        service.session(true, false);
        QCOMPARE(rejected.size(), 2);
        service.session(true, true);
        QCOMPARE(rejected.size(), 2);
        service.shutdown();
        db.close(); db = {}; QSqlDatabase::removeDatabase("rollback-inspector");
    }
    void delayAndRecovery()
    {
        ZoneMonitor z;
        z.rule.name = "区域1";
        z.rule.tempMax = 30;
        z.rule.holdMs = 1000;
        z.rule.recoveryMs = 500;
        QVERIFY(z.sample(0, 31, 50).isEmpty());
        QVERIFY(z.sample(999, 31, 50).isEmpty());
        auto raised = z.sample(1000, 31, 50);
        QCOMPARE(raised.size(), 1);
        QCOMPARE(raised[0].kind, AlarmTransition::Raised);
        QVERIFY(z.sample(1100, 29.5, 50).isEmpty());
        QVERIFY(z.channels[0].active);
        QVERIFY(z.sample(1200, 29, 50).isEmpty());
        QVERIFY(z.sample(1699, 29, 50).isEmpty());
        auto recovered = z.sample(1700, 29, 50);
        QCOMPARE(recovered.size(), 1);
        QCOMPARE(recovered[0].kind, AlarmTransition::Recovered);
    }
    void missingDoesNotRecover()
    {
        ZoneMonitor z;
        z.rule.name = "A";
        z.rule.holdMs = 0;
        z.rule.recoveryMs = 1000;
        QCOMPARE(z.sample(0, 80, 50).size(), 1);
        z.invalidate();
        QVERIFY(z.channels[0].active);
        QVERIFY(z.sample(10000, 25, 50).isEmpty());
        z.tick(16000);
        QVERIFY(!z.validData);
        QVERIFY(z.sample(17000, 25, 50).isEmpty());
        QVERIFY(z.channels[0].active);
        QCOMPARE(z.sample(18000, 25, 50).size(), 1);
    }
    void interruptedPendingAndOppositeExcursion()
    {
        ZoneMonitor z;
        z.rule.name = "A";
        z.rule.holdMs = 1000;
        z.rule.staleMs = 1000;
        z.sample(0, 80, 50);
        QVERIFY(z.sample(2000, 80, 50).isEmpty());
        z.sample(2100, -30, 50);
        QVERIFY(z.sample(3000, -30, 50).isEmpty());
        QCOMPARE(z.sample(3100, -30, 50).size(), 1);
        QVERIFY(z.sample(3200, 80, 50).isEmpty());
        QVERIFY(z.channels[0].active);
    }
    void invalidRulesAndData()
    {
        ZoneRule r;
        r.name = "A";
        QVERIFY(r.valid());
        r.tempHysteresis = 100;
        QVERIFY(!r.valid());
        ZoneMonitor z;
        z.rule.name = "A";
        z.sample(0, 25, 50);
        z.sample(1, std::numeric_limits<double>::quiet_NaN(), 50);
        QVERIFY(!z.validData);
        z.sample(2, 25, 101);
        QVERIFY(!z.validData);
        z.rule.monitored = false;
        QVERIFY(z.sample(3, 100, 50).isEmpty());
    }
    void taskCoverageAndDuration()
    {
        TaskStats s;
        ZoneRule r;
        r.tempMax = 30;
        r.staleMs = 1000;
        s.observe(0, 500, r, 31, 50);
        s.observe(100, 500, r, 32, 50);
        s.observe(1000, 500, r, 25, 50);
        QCOMPARE(s.samples, 3);
        QCOMPARE(s.occupiedSlots, 2);
        QCOMPARE(s.expected(2000, 500), 4);
        QCOMPARE(s.tempExceededMs, 1000);
        s.invalidate(1100, 1000);
        s.observe(5000, 500, r, 25, 50);
        QCOMPARE(s.tempExceededMs, 1000);
        TaskStats empty;
        QCOMPARE(empty.expected(1000, 500), 2);
        QCOMPARE(empty.samples, 0);
    }
    void persistenceAndLifecycle()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path = temp.filePath("test.db");
        auto db = QSqlDatabase::addDatabase("QSQLITE", "test-reader");
        db.setDatabaseName(path);
        QVERIFY(db.open());
        {
            QSqlQuery q(db);
            QVERIFY(q.exec(
                "CREATE TABLE sensor_history(id INTEGER PRIMARY KEY,sample_time TEXT,sensor_index INTEGER,sensor_name TEXT,slave_id INTEGER,temperature REAL,humidity REAL)"));
            QVERIFY(q.exec(
                "CREATE TABLE alarm_history(id INTEGER PRIMARY KEY,alarm_time TEXT,sensor_index INTEGER,sensor_name TEXT,slave_id INTEGER,alarm_type TEXT,alarm_value REAL,minimum_value REAL,maximum_value REAL)"));
        }
        QVariantList defaults;
        for (int i = 0; i < 4; ++i) {
            ZoneRule r;
            r.name = QString("区域%1").arg(i + 1);
            r.holdMs = 0;
            r.recoveryMs = 0;
            defaults.append(MonitorService::encodeRule(r));
        }
        MonitorService service;
        QSignalSpy ready(&service, &MonitorService::initialized);
        service.initialize(path, defaults);
        QVERIFY(ready.first()[0].toBool());
        QSignalSpy views(&service, &MonitorService::viewReady);
        QSignalSpy reports(&service, &MonitorService::reportReady);
        service.configure(defaults);
        service.session(true, true);
        service.startTask("=报告测试", {0}, 500);
        service.sample(0, "sensor", 1, 80, 50, MonitorService::monotonicMs(),
                       "2026-09-16T10:00:00.000");
        service.refresh();
        auto v = views.last()[0].toMap();
        auto alarms = v["alarms"].toList();
        QCOMPARE(alarms.size(), 1);
        auto id = alarms.first().toMap()["id"].toLongLong();
        auto task = v["tasks"].toList().first().toMap()["id"].toLongLong();
        service.acknowledge(id, "操作人", "已检查现场");
        service.refresh();
        auto alarm = views.last()[0].toMap()["alarms"].toList().first().toMap();
        QVERIFY(!alarm["ack_at"].isNull());
        QVERIFY(alarm["recovered_at"].isNull());
        service.session(false, true);
        service.refresh();
        QVERIFY(
            views.last()[0].toMap()["alarms"].toList().first().toMap()["recovered_at"].isNull());
        service.finishTask();
        service.report(task);
        QCOMPARE(reports.size(), 1);
        QVERIFY(reports.first()[0].toString().contains("'=报告测试"));
        service.shutdown();
        MonitorService resumed;
        resumed.initialize(path, defaults);
        QSignalSpy resumedViews(&resumed, &MonitorService::viewReady);
        resumed.session(true, true);
        resumed.sample(0, "sensor", 1, 25, 50, MonitorService::monotonicMs(),
                       "2026-09-16T10:00:01.000");
        resumed.refresh();
        alarm = resumedViews.last()[0].toMap()["alarms"].toList().first().toMap();
        QVERIFY(!alarm["recovered_at"].isNull());
        QVERIFY(!alarm["ack_at"].isNull());
        resumed.startTask("中断测试", {0}, 500);
        resumed.shutdown();
        {
            QSqlQuery q(db);
            QVERIFY(q.exec("SELECT status FROM monitor_tasks ORDER BY id DESC LIMIT 1"));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toString(), QString("interrupted"));
        }
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase("test-reader");
    }
    void realWorkerShutdown()
    {
        QTemporaryDir dir;
        QThread thread;
        QPointer<MonitorService> worker = new MonitorService;
        worker->moveToThread(&thread);
        connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.start();
        QVariantList defaults;
        for (int i = 0; i < 4; ++i) {
            ZoneRule r;
            r.name = QString::number(i);
            defaults.append(MonitorService::encodeRule(r));
        }
        QMetaObject::invokeMethod(
            worker,
            [worker, path = dir.filePath("worker.db"), defaults] {
                worker->initialize(path, defaults);
            },
            Qt::BlockingQueuedConnection);
        QMetaObject::invokeMethod(worker, &MonitorService::shutdown, Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait();
        QVERIFY(worker.isNull());
    }
};
QTEST_GUILESS_MAIN(MonitorBusinessTest)
#include "test_monitorbusiness.moc"
