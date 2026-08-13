#include "databasemanager.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

DatabaseManager::DatabaseManager()
    : m_connectionName("environment-monitor-" + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

DatabaseManager::~DatabaseManager()
{
    if (m_database.isValid())
        m_database.close();
    m_database = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

QString DatabaseManager::databasePath() const
{
    return QApplication::applicationDirPath() + "/data/environment.db";
}

QString DatabaseManager::connectionName() const
{
    return m_connectionName;
}

bool DatabaseManager::initialize(QString *errorMessage)
{
    QDir().mkpath(QApplication::applicationDirPath() + "/data");
    m_database = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    m_database.setDatabaseName(databasePath());
    if (!m_database.open()) {
        if (errorMessage)
            *errorMessage = m_database.lastError().text();
        return false;
    }
    QSqlQuery pragma(m_database);
    pragma.exec("PRAGMA journal_mode=WAL");
    pragma.exec("PRAGMA synchronous=NORMAL");
    pragma.exec("PRAGMA busy_timeout=3000");
    return executeSchema(errorMessage);
}

bool DatabaseManager::executeSchema(QString *errorMessage)
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
        "ON sensor_history(sensor_index, sample_time)",
        "CREATE TABLE IF NOT EXISTS alarm_history ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "alarm_time TEXT NOT NULL,"
        "sensor_index INTEGER NOT NULL,"
        "sensor_name TEXT NOT NULL,"
        "slave_id INTEGER NOT NULL,"
        "alarm_type TEXT NOT NULL,"
        "alarm_value REAL NOT NULL,"
        "minimum_value REAL NOT NULL,"
        "maximum_value REAL NOT NULL)",
        "CREATE INDEX IF NOT EXISTS idx_alarm_time ON alarm_history(alarm_time)"
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

bool DatabaseManager::insertSample(const QDateTime &time, int sensorIndex,
                                   const QString &sensorName, int slaveId,
                                   double temperature, double humidity,
                                   QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare("INSERT INTO sensor_history "
                  "(sample_time,sensor_index,sensor_name,slave_id,temperature,humidity) "
                  "VALUES (?,?,?,?,?,?)");
    query.addBindValue(time.toString(Qt::ISODateWithMs));
    query.addBindValue(sensorIndex);
    query.addBindValue(sensorName);
    query.addBindValue(slaveId);
    query.addBindValue(temperature);
    query.addBindValue(humidity);
    if (!query.exec()) {
        if (errorMessage)
            *errorMessage = query.lastError().text();
        return false;
    }
    return true;
}

bool DatabaseManager::insertAlarm(const QDateTime &time, int sensorIndex,
                                  const QString &sensorName, int slaveId,
                                  const QString &type, double value,
                                  double minimum, double maximum,
                                  QString *errorMessage)
{
    QSqlQuery query(m_database);
    query.prepare("INSERT INTO alarm_history "
                  "(alarm_time,sensor_index,sensor_name,slave_id,alarm_type,"
                  "alarm_value,minimum_value,maximum_value) VALUES (?,?,?,?,?,?,?,?)");
    query.addBindValue(time.toString(Qt::ISODateWithMs));
    query.addBindValue(sensorIndex);
    query.addBindValue(sensorName);
    query.addBindValue(slaveId);
    query.addBindValue(type);
    query.addBindValue(value);
    query.addBindValue(minimum);
    query.addBindValue(maximum);
    if (!query.exec()) {
        if (errorMessage)
            *errorMessage = query.lastError().text();
        return false;
    }
    return true;
}

bool DatabaseManager::cleanupOldData(int retentionDays, QString *errorMessage)
{
    const QString cutoff = QDateTime::currentDateTime().addDays(-retentionDays)
                               .toString(Qt::ISODateWithMs);
    for (const QString &statement : {
             QString("DELETE FROM sensor_history WHERE sample_time < ?"),
             QString("DELETE FROM alarm_history WHERE alarm_time < ?")}) {
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
