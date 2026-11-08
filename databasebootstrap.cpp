#include "databasebootstrap.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QDateTime>
#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QSqlRecord>
#include <QJsonDocument>
#include <QVariantMap>
#include <QSet>
#include <QScopeGuard>

DatabaseBootstrap::DatabaseBootstrap(const QString &path)
    : m_connectionName("environment-monitor-" + QUuid::createUuid().toString(QUuid::WithoutBraces)), m_path(path)
{
}

DatabaseBootstrap::~DatabaseBootstrap()
{
    if (m_database.isValid())
        m_database.close();
    m_database = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

QString DatabaseBootstrap::databasePath() const
{
    return m_path.isEmpty() ? QCoreApplication::applicationDirPath() + "/data/environment.db" : m_path;
}

QString DatabaseBootstrap::connectionName() const
{
    return m_connectionName;
}

bool DatabaseBootstrap::initialize(QString *errorMessage)
{
    QDir().mkpath(QFileInfo(databasePath()).absolutePath());
    m_database = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    m_database.setDatabaseName(databasePath());
    m_database.setConnectOptions("QSQLITE_BUSY_TIMEOUT=250");
    if (!m_database.open()) {
        if (errorMessage)
            *errorMessage = m_database.lastError().text();
        return false;
    }
    QSqlQuery pragma(m_database);
    pragma.exec("PRAGMA journal_mode=WAL");
    pragma.exec("PRAGMA synchronous=NORMAL");
    pragma.exec("PRAGMA busy_timeout=250");
    return executeSchema(errorMessage);
}

bool DatabaseBootstrap::executeSchema(QString *errorMessage)//数据库建表，历史样本表
{
    QSqlQuery query(m_database);
    const QStringList statements = {
        "CREATE TABLE IF NOT EXISTS sensor_history ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "sample_time TEXT NOT NULL,"
        "sensor_index INTEGER NOT NULL,"
        "sensor_name TEXT NOT NULL,"
        "slave_id INTEGER NOT NULL,"
        "temperature REAL NOT NULL,"
        "humidity REAL NOT NULL)",
        "CREATE INDEX IF NOT EXISTS idx_history_time ON sensor_history(sample_time)",
        "CREATE INDEX IF NOT EXISTS idx_history_sensor_time "
        "ON sensor_history(sensor_index, sample_time)"
    };
    for (const QString &statement : statements) {
        if (!query.exec(statement)) {
            if (errorMessage)
                *errorMessage = query.lastError().text();
            return false;
        }
    }
    return true;
}

bool DatabaseBootstrap::cleanupOldData(int retentionDays, QString *errorMessage)
{
    const QString cutoff = QDateTime::currentDateTime().addDays(-retentionDays)
                               .toString(Qt::ISODateWithMs);
    for (const QString &statement : {
             QString("DELETE FROM sensor_history WHERE sample_time < ?")}) {
        QSqlQuery query(m_database);
        query.prepare(statement);
        query.addBindValue(cutoff);
        if (!query.exec()) {
            if (errorMessage)
                *errorMessage = query.lastError().text();
            return false;
        }
    }
    return true;
}

namespace {
bool executeSql(QSqlDatabase &db, const QString &sql, const QVariantList &args, QString &error)
{
    QSqlQuery q(db);
    if (!q.prepare(sql)) { error = q.lastError().text(); return false; }
    for (const auto &value : args) q.addBindValue(value);
    if (!q.exec()) { error = q.lastError().text(); return false; }
    return true;
}
QString json(const QVariantMap &map)
{
    return QString::fromUtf8(QJsonDocument::fromVariant(map).toJson(QJsonDocument::Compact));
}
bool migrateAlarmHistory(QSqlDatabase &db, QString &error)
{
    const auto execute = [&](const QString &sql, const QVariantList &args = QVariantList{}) {
        return executeSql(db, sql, args, error);
    };
    if (!db.transaction()) {
        error = db.lastError().text();
        return false;
    }
    auto rollback = qScopeGuard([&db] { db.rollback(); });
    // ALTER and the copy/drop share one transaction: a failed migration leaves the old
    // table and every record intact. Do not infer missing lifecycle/source information.
    QSet<QString> columns;
    {
        QSqlQuery q(db);
        if (!q.exec("PRAGMA table_info(monitor_alarms)")) {
            error = q.lastError().text();
            return false;
        }
        while (q.next())
            columns.insert(q.value(1).toString());
    }
    if ((!columns.contains("slave_id") &&
         !execute("ALTER TABLE monitor_alarms ADD COLUMN slave_id INTEGER")) ||
        (!columns.contains("legacy_record") &&
         !execute("ALTER TABLE monitor_alarms ADD COLUMN legacy_record TEXT")) ||
        !execute("DROP INDEX IF EXISTS monitor_alarm_one_active") ||
        !execute("CREATE UNIQUE INDEX monitor_alarm_one_active ON monitor_alarms(zone,metric) WHERE recovered_at IS NULL AND legacy_record IS NULL"))
        return false;

    bool hasLegacy = false;
    {
        QSqlQuery q(db);
        if (!q.exec("SELECT 1 FROM sqlite_master WHERE type='table' AND name='alarm_history'")) {
            error = q.lastError().text();
            return false;
        }
        hasLegacy = q.next();
    }
    if (hasLegacy) {
        {
            QSqlQuery q(db);
            if (!q.exec("SELECT * FROM alarm_history ORDER BY rowid")) {
                error = q.lastError().text();
                return false;
            }
            const auto record = q.record();
            for (const auto &field : {"alarm_time", "sensor_index", "sensor_name", "slave_id",
                                      "alarm_type", "alarm_value", "minimum_value", "maximum_value"}) {
                if (record.indexOf(field) < 0) {
                    error = QString("旧版报警缺少字段：%1").arg(field);
                    return false;
                }
            }
            while (q.next()) {
                QVariantMap original;
                for (int i = 0; i < record.count(); ++i)
                    original.insert(record.fieldName(i), q.value(i));
                const auto type = original["alarm_type"].toString();
                const int metric = type == "温度" ? 0 : type == "湿度" ? 1 : -1;
                const double value = original["alarm_value"].toDouble();
                const double minimum = original["minimum_value"].toDouble();
                const double maximum = original["maximum_value"].toDouble();
                const int direction = value < minimum ? -1 : value > maximum ? 1 : 0;
                const auto time = QDateTime::fromString(original["alarm_time"].toString(), Qt::ISODateWithMs);
                const auto raised = time.isValid() ? time.toUTC().toString(Qt::ISODateWithMs)
                                                  : original["alarm_time"].toString();
                if (!execute(
                        "INSERT INTO monitor_alarms(zone,name,metric,direction,value,minimum,maximum,raised_at,simulator,slave_id,legacy_record) VALUES(?,?,?,?,?,?,?,?,-1,?,?)",
                        {original["sensor_index"], original["sensor_name"], metric, direction,
                         original["alarm_value"], original["minimum_value"], original["maximum_value"],
                         raised, original["slave_id"], json(original)}))
                    return false;
            }
            if (q.lastError().isValid()) {
                error = q.lastError().text();
                return false;
            }
        } // Finish the SELECT before changing the table schema.
        if (!execute("DROP TABLE alarm_history"))
            return false;
    }
    if (!db.commit()) {
        error = db.lastError().text();
        return false;
    }
    rollback.dismiss();
    return true;
}

} // namespace

bool DatabaseBootstrap::initializeMonitoring(QSqlDatabase &db, QString *errorMessage)
{
    QString error;
    const QStringList schema = {
        "CREATE TABLE IF NOT EXISTS monitor_rules(zone INTEGER PRIMARY KEY,config TEXT NOT NULL)",
        "CREATE TABLE IF NOT EXISTS monitor_tasks(id INTEGER PRIMARY KEY,name TEXT NOT NULL,started_at TEXT NOT NULL,ended_at TEXT,status TEXT NOT NULL,simulator INTEGER NOT NULL,interval_ms INTEGER NOT NULL,elapsed_ms INTEGER NOT NULL DEFAULT 0)",
        "CREATE TABLE IF NOT EXISTS monitor_task_zones(task_id INTEGER NOT NULL REFERENCES monitor_tasks(id),zone INTEGER NOT NULL,config TEXT NOT NULL,samples INTEGER NOT NULL DEFAULT 0,slots INTEGER NOT NULL DEFAULT 0,temp_min REAL,temp_max REAL,hum_min REAL,hum_max REAL,temp_exceeded_ms INTEGER NOT NULL DEFAULT 0,hum_exceeded_ms INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(task_id,zone))",
        "CREATE TABLE IF NOT EXISTS monitor_alarms(id INTEGER PRIMARY KEY,zone INTEGER NOT NULL,name TEXT NOT NULL,metric INTEGER NOT NULL,direction INTEGER NOT NULL,value REAL NOT NULL,minimum REAL NOT NULL,maximum REAL NOT NULL,raised_at TEXT NOT NULL,ack_at TEXT,operator TEXT,note TEXT,recovered_at TEXT,task_id INTEGER REFERENCES monitor_tasks(id),simulator INTEGER NOT NULL,slave_id INTEGER,legacy_record TEXT)",
        "CREATE INDEX IF NOT EXISTS monitor_alarm_active ON monitor_alarms(zone,metric,recovered_at)",
        "CREATE INDEX IF NOT EXISTS monitor_alarm_unacknowledged ON monitor_alarms(id DESC) WHERE ack_at IS NULL",
        "CREATE TABLE IF NOT EXISTS monitor_actions(id INTEGER PRIMARY KEY,time TEXT NOT NULL,kind TEXT NOT NULL,detail TEXT NOT NULL)"};
    for (const auto &sql : schema) {
        if (!executeSql(db, sql, {}, error)) {
            if (errorMessage) *errorMessage = error;
            return false;
        }
    }
    const bool ok = migrateAlarmHistory(db, error);
    if (!ok && errorMessage) *errorMessage = error;
    return ok;
}
