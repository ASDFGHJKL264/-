#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QSqlDatabase>
#include <QString>

class QDateTime;

class DatabaseManager
{
public:
    explicit DatabaseManager(const QString &path = {});
    ~DatabaseManager();

    bool initialize(QString *errorMessage = nullptr);
    bool insertSample(const QDateTime &time, int sensorIndex, const QString &sensorName,
                      int slaveId, double temperature, double humidity,
                      QString *errorMessage = nullptr);
    bool insertAlarm(const QDateTime &time, int sensorIndex, const QString &sensorName,
                     int slaveId, const QString &type, double value,
                     double minimum, double maximum, QString *errorMessage = nullptr);
    bool cleanupOldData(int retentionDays, QString *errorMessage = nullptr);
    QString connectionName() const;
    QString databasePath() const;

private:
    bool executeSchema(QString *errorMessage);

    QString m_connectionName;
    QString m_path;
    QSqlDatabase m_database;
};

#endif
