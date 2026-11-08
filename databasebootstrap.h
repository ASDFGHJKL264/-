#ifndef DATABASEBOOTSTRAP_H
#define DATABASEBOOTSTRAP_H

#include <QSqlDatabase>
#include <QString>

// Opens a temporary setup connection for sample schema/retention.
// Runtime writes belong to MonitorService, on its own worker connection.
class DatabaseBootstrap
{
public:
    explicit DatabaseBootstrap(const QString &path = {});
    ~DatabaseBootstrap();

    bool initialize(QString *errorMessage = nullptr);
    // Uses the caller's connection; migration owns one atomic copy/drop transaction.
    static bool initializeMonitoring(QSqlDatabase &db, QString *errorMessage = nullptr);
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
