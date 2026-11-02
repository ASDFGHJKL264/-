#include "databasemanager.h"
#include "monitorservice.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

namespace {
class Inspector {
public:
    explicit Inspector(const QString &path)
        : name("alarm-migration-" + QUuid::createUuid().toString()),
          db(QSqlDatabase::addDatabase("QSQLITE", name))
    {
        db.setDatabaseName(path);
        db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=250");
        db.open();
    }
    ~Inspector()
    {
        db.close();
        db = {};
        QSqlDatabase::removeDatabase(name);
    }
    bool exec(const QString &sql)
    {
        QSqlQuery q(db);
        const bool ok = q.exec(sql);
        if (!ok)
            qWarning() << q.lastError() << sql;
        return ok;
    }
    qint64 scalar(const QString &sql)
    {
        QSqlQuery q(db);
        if (!q.exec(sql) || !q.next()) {
            qWarning() << q.lastError() << sql;
            return -1;
        }
        return q.value(0).toLongLong();
    }
    QString name;
    QSqlDatabase db;
};

QVariantList defaults()
{
    QVariantList result;
    for (int i = 0; i < 4; ++i) {
        ZoneRule rule;
        rule.name = QString("区域%1").arg(i + 1);
        rule.holdMs = rule.recoveryMs = 0;
        result.append(MonitorService::encodeRule(rule));
    }
    return result;
}

const char *legacySchema =
    "CREATE TABLE alarm_history(id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "alarm_time TEXT NOT NULL,sensor_index INTEGER NOT NULL,sensor_name TEXT NOT NULL,"
    "slave_id INTEGER NOT NULL,alarm_type TEXT NOT NULL,alarm_value REAL NOT NULL,"
    "minimum_value REAL NOT NULL,maximum_value REAL NOT NULL)";

const char *businessSchema =
    "CREATE TABLE monitor_alarms(id INTEGER PRIMARY KEY,zone INTEGER NOT NULL,"
    "name TEXT NOT NULL,metric INTEGER NOT NULL,direction INTEGER NOT NULL,"
    "value REAL NOT NULL,minimum REAL NOT NULL,maximum REAL NOT NULL,"
    "raised_at TEXT NOT NULL,ack_at TEXT,operator TEXT,note TEXT,recovered_at TEXT,"
    "task_id INTEGER REFERENCES monitor_tasks(id),simulator INTEGER NOT NULL)";

bool addLegacy(Inspector &inspector, int id, const QString &type = QStringLiteral("温度"))
{
    QSqlQuery q(inspector.db);
    if (!q.prepare("INSERT INTO alarm_history VALUES(?,?,?,?,?,?,?,?,?)"))
        return false;
    q.addBindValue(id);
    // Deliberately older than the raw-history retention window. Migration must run intact.
    q.addBindValue(QString("2020-01-02T03:04:%1.123+08:00").arg(id % 60, 2, 10, QChar('0')));
    q.addBindValue(0);
    q.addBindValue(QString("旧区域\"%1\n备注").arg(id));
    q.addBindValue(17);
    q.addBindValue(type);
    q.addBindValue(80.25);
    q.addBindValue(-20.0);
    q.addBindValue(60.0);
    return q.exec();
}

QVariantMap view(const QSignalSpy &spy)
{
    return spy.isEmpty() ? QVariantMap{} : spy.last()[0].toMap();
}
}

class AlarmMigrationTests final : public QObject {
    Q_OBJECT
private slots:
    void freshDatabaseUsesOneAlarmTable()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("new.db");
        {
            DatabaseManager schema(path);
            QString error;
            QVERIFY2(schema.initialize(&error), qPrintable(error));
            QVERIFY2(schema.cleanupOldData(30, &error), qPrintable(error));
        }
        Inspector inspector(path);
        QVERIFY(inspector.db.isOpen());
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM sqlite_master WHERE name='alarm_history'"), 0);
        MonitorService service;
        QSignalSpy ready(&service, &MonitorService::initialized);
        service.initialize(path, defaults());
        QCOMPARE(ready.size(), 1);
        QVERIFY(ready.first()[0].toBool());
        service.session(true, true);
        service.sample(0, "新模块", 23, 80, 50, MonitorService::monotonicMs(),
                       QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM monitor_alarms"), 1);
        QCOMPARE(inspector.scalar("SELECT slave_id FROM monitor_alarms"), 23);
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM monitor_alarms WHERE legacy_record IS NULL"), 1);
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM sqlite_master WHERE name='alarm_history'"), 0);
        service.shutdown();
    }

    void archivePreservesEveryOccurrenceAndDoesNotBecomeActive()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("archive.db");
        Inspector inspector(path);
        QVERIFY(inspector.db.isOpen());
        QVERIFY(inspector.exec(legacySchema));
        QVERIFY(inspector.db.transaction());
        for (int id = 1001; id <= 1207; ++id)
            QVERIFY(addLegacy(inspector, id));
        QVERIFY(addLegacy(inspector, 9999, "压力/未知类型"));
        QVERIFY(inspector.db.commit());
        {
            DatabaseManager schema(path);
            QString error;
            QVERIFY2(schema.initialize(&error), qPrintable(error));
            QVERIFY2(schema.cleanupOldData(30, &error), qPrintable(error));
        }
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM alarm_history"), 208);

        MonitorService service;
        QSignalSpy ready(&service, &MonitorService::initialized);
        QSignalSpy views(&service, &MonitorService::viewReady);
        QSignalSpy locks(&service, &MonitorService::configurationLocked);
        QSignalSpy rejected(&service, &MonitorService::sessionRejected);
        service.initialize(path, defaults());
        QCOMPARE(ready.size(), 1);
        QVERIFY(ready.first()[0].toBool());
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM sqlite_master WHERE name='alarm_history'"), 0);
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM monitor_alarms WHERE legacy_record IS NOT NULL"), 208);
        QCOMPARE(view(views)["alarms"].toList().size(), 0);
        QVERIFY(!locks.isEmpty());
        QVERIFY(!locks.last()[0].toBool());

        // Archived occurrences have no known recovery or source; they must not acquire a
        // fabricated active status, source, acknowledgement, or recovery timestamp.
        QSet<int> originalIds;
        qint64 anyArchivedId = 0;
        {
            QSqlQuery q(inspector.db);
            QVERIFY(q.exec("SELECT id,legacy_record,recovered_at,simulator,slave_id,metric "
                           "FROM monitor_alarms ORDER BY id"));
            while (q.next()) {
                const auto raw = QJsonDocument::fromJson(q.value(1).toString().toUtf8()).toVariant().toMap();
                const int originalId = raw["id"].toInt();
                QVERIFY(originalId == 9999 || (originalId >= 1001 && originalId <= 1207));
                QVERIFY(!originalIds.contains(originalId));
                originalIds.insert(originalId);
                QCOMPARE(raw["sensor_name"].toString(), QString("旧区域\"%1\n备注").arg(originalId));
                QCOMPARE(raw["alarm_time"].toString(),
                         QString("2020-01-02T03:04:%1.123+08:00").arg(originalId % 60, 2, 10, QChar('0')));
                QCOMPARE(raw["slave_id"].toInt(), 17);
                QCOMPARE(raw["sensor_index"].toInt(), 0);
                QCOMPARE(raw["alarm_value"].toDouble(), 80.25);
                QCOMPARE(raw["minimum_value"].toDouble(), -20.0);
                QCOMPARE(raw["maximum_value"].toDouble(), 60.0);
                QCOMPARE(raw["alarm_type"].toString(),
                         originalId == 9999 ? QString("压力/未知类型") : QString("温度"));
                QVERIFY(q.value(2).isNull());
                QCOMPARE(q.value(3).toInt(), -1);
                QCOMPARE(q.value(4).toInt(), 17);
                if (originalId == 9999)
                    QCOMPARE(q.value(5).toInt(), -1);
                anyArchivedId = q.value(0).toLongLong();
            }
        }
        QCOMPARE(originalIds.size(), 208);

        // Pagination reaches all original occurrences without duplicates and without pins.
        service.browseAlarms(0, 4);
        QSet<qint64> archivedIds;
        int pages = 0;
        for (;;) {
            const auto current = view(views);
            const auto records = current["alarms"].toList();
            QVERIFY(records.size() <= 100);
            QCOMPARE(current["alarmPage"].toMap()["pinned"].toInt(), 0);
            for (const auto &record : records) {
                const auto id = record.toMap()["id"].toLongLong();
                QVERIFY(!archivedIds.contains(id));
                archivedIds.insert(id);
            }
            ++pages;
            if (!current["alarmPage"].toMap()["hasMore"].toBool())
                break;
            QVERIFY(pages < 4);
            service.browseAlarms(1);
        }
        QCOMPARE(pages, 3);
        QCOMPARE(archivedIds.size(), 208);
        service.browseAlarms(-1);
        QCOMPARE(view(views)["alarmPage"].toMap()["number"].toInt(), 2);
        service.acknowledge(anyArchivedId, "操作人", "归档不应被当成待确认报警");
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM monitor_alarms WHERE ack_at IS NOT NULL"), 0);

        auto changed = defaults();
        auto first = changed.first().toMap();
        first["name"] = "新区域名称";
        changed[0] = first;
        service.configure(changed);
        QCOMPARE(view(views)["rules"].toList().first().toMap()["name"].toString(), QString("新区域名称"));
        service.session(true, false);
        service.session(false, false);
        service.session(true, true);
        QCOMPARE(rejected.size(), 0);

        // A new event is written only once and carries its actual address.
        service.browseAlarms(0, 0);
        service.sample(0, "新模块", 23, 80, 50, MonitorService::monotonicMs(),
                       QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM monitor_alarms"), 209);
        QCOMPARE(inspector.scalar("SELECT slave_id FROM monitor_alarms WHERE legacy_record IS NULL"), 23);
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM monitor_alarms WHERE legacy_record IS NOT NULL AND recovered_at IS NOT NULL"), 0);
        service.shutdown();

        MonitorService resumed;
        QSignalSpy resumedReady(&resumed, &MonitorService::initialized);
        resumed.initialize(path, changed);
        QCOMPARE(resumedReady.size(), 1);
        QVERIFY(resumedReady.first()[0].toBool());
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM monitor_alarms"), 209);
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM sqlite_master WHERE name='alarm_history'"), 0);
        resumed.shutdown();
    }

    void existingActiveAlarmAndOldUniqueIndexRemainCorrect()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("active.db");
        {
            DatabaseManager schema(path);
            QVERIFY(schema.initialize());
        }
        Inspector inspector(path);
        QVERIFY(inspector.exec(businessSchema));
        QVERIFY(inspector.exec("CREATE UNIQUE INDEX monitor_alarm_one_active ON monitor_alarms(zone,metric) WHERE recovered_at IS NULL"));
        QVERIFY(inspector.exec("INSERT INTO monitor_alarms(id,zone,name,metric,direction,value,minimum,maximum,raised_at,simulator) "
                               "VALUES(7,0,'真实报警',0,1,80,-20,60,'2020-01-01T00:00:00.000Z',0)"));
        QVERIFY(inspector.exec(legacySchema));
        QVERIFY(addLegacy(inspector, 1));
        QVERIFY(addLegacy(inspector, 2));
        MonitorService service;
        QSignalSpy ready(&service, &MonitorService::initialized);
        QSignalSpy views(&service, &MonitorService::viewReady);
        QSignalSpy rejected(&service, &MonitorService::sessionRejected);
        service.initialize(path, defaults());
        QCOMPARE(ready.size(), 1);
        QVERIFY(ready.first()[0].toBool());
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM monitor_alarms"), 3);
        QCOMPARE(view(views)["alarmPage"].toMap()["pinned"].toInt(), 1);
        QCOMPARE(view(views)["alarms"].toList().first().toMap()["id"].toInt(), 7);
        service.session(true, true);
        QCOMPARE(rejected.size(), 1); // A real hardware alarm still blocks simulator recovery.
        service.session(true, false);
        QCOMPARE(rejected.size(), 1);
        service.sample(0, "真实模块", 17, 25, 50, MonitorService::monotonicMs(),
                       QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM monitor_alarms WHERE id=7 AND recovered_at IS NOT NULL"), 1);
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM monitor_alarms WHERE legacy_record IS NOT NULL AND recovered_at IS NOT NULL"), 0);
        service.shutdown();
    }

    void failedMigrationRollsBackAndCanBeRetried()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("retry.db");
        {
            DatabaseManager schema(path);
            QVERIFY(schema.initialize());
        }
        Inspector inspector(path);
        QVERIFY(inspector.exec(legacySchema));
        QVERIFY(addLegacy(inspector, 1));
        QVERIFY(addLegacy(inspector, 2));
        QVERIFY(inspector.exec(businessSchema));
        QVERIFY(inspector.exec("CREATE TRIGGER reject_archive BEFORE INSERT ON monitor_alarms "
                               "WHEN NEW.name LIKE '%2%' BEGIN SELECT RAISE(ABORT,'injected migration failure'); END"));
        MonitorService failed;
        QSignalSpy failedReady(&failed, &MonitorService::initialized);
        failed.initialize(path, defaults());
        QCOMPARE(failedReady.size(), 1);
        QVERIFY(!failedReady.first()[0].toBool());
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM alarm_history"), 2);
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM monitor_alarms"), 0);
        failed.shutdown();
        QVERIFY(inspector.exec("DROP TRIGGER reject_archive"));
        MonitorService retry;
        QSignalSpy retryReady(&retry, &MonitorService::initialized);
        retry.initialize(path, defaults());
        QCOMPARE(retryReady.size(), 1);
        QVERIFY(retryReady.first()[0].toBool());
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM sqlite_master WHERE name='alarm_history'"), 0);
        QCOMPARE(inspector.scalar("SELECT COUNT(*) FROM monitor_alarms WHERE legacy_record IS NOT NULL"), 2);
        retry.shutdown();
    }

    void legacySchemaWithoutExplicitIdIsStillPreserved()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("no-id.db");
        {
            DatabaseManager schema(path);
            QVERIFY(schema.initialize());
        }
        Inspector inspector(path);
        QVERIFY(inspector.exec("CREATE TABLE alarm_history(alarm_time TEXT,sensor_index INTEGER,"
                               "sensor_name TEXT,slave_id INTEGER,alarm_type TEXT,alarm_value REAL,"
                               "minimum_value REAL,maximum_value REAL)"));
        QVERIFY(inspector.exec("INSERT INTO alarm_history VALUES('2020-01-01T00:00:00',2,'无ID旧记录',8,'湿度',99,10,90)"));
        MonitorService service;
        QSignalSpy ready(&service, &MonitorService::initialized);
        service.initialize(path, defaults());
        QCOMPARE(ready.size(), 1);
        QVERIFY(ready.first()[0].toBool());
        {
            QSqlQuery q(inspector.db);
            QVERIFY(q.exec("SELECT legacy_record FROM monitor_alarms"));
            QVERIFY(q.next());
            const auto raw = QJsonDocument::fromJson(q.value(0).toString().toUtf8()).toVariant().toMap();
            QCOMPARE(raw["sensor_name"].toString(), QString("无ID旧记录"));
            QCOMPARE(raw["alarm_type"].toString(), QString("湿度"));
            QCOMPARE(raw["slave_id"].toInt(), 8);
            QCOMPARE(raw["alarm_value"].toDouble(), 99.0);
            QVERIFY(!q.next());
        }
        service.shutdown();
    }
};

QTEST_GUILESS_MAIN(AlarmMigrationTests)
#include "test_alarmmigration.moc"
