#include "historyquery.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QUuid>

HistoryQuery::HistoryQuery(const QString &path, QObject *parent)
    : QObject(parent), m_worker(new QObject), m_path(path)
{
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.start();
}
HistoryQuery::~HistoryQuery()
{
    QMetaObject::invokeMethod(m_worker, [] { QThread::currentThread()->quit(); });
    m_thread.wait();
}
bool HistoryQuery::run(Operation operation, const QString &from, const QString &to, int sensor, int offset)
{
    if (m_busy || sensor < -1 || sensor > 3 || offset < 0) return false;
    m_busy = true;
    QMetaObject::invokeMethod(m_worker, [this, path = m_path, operation, from, to, sensor, offset] {
        QVariantList rows;
        QString error;
        bool more = false;
        const auto name = "history-reader-" + QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(path);
            db.setConnectOptions(operation == ClearAlarms ? "QSQLITE_BUSY_TIMEOUT=250"
                : "QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=250");
            if (!db.open()) error = db.lastError().text();
            else {
                QSqlQuery q(db);
                QString sql;
                if (operation == ClearAlarms) sql = "DELETE FROM alarm_history";
                else {
                    const QString time = operation == Samples ? "sample_time" : "alarm_time";
                    sql = operation == Samples
                        ? "SELECT sample_time,sensor_name,slave_id,temperature,humidity,sensor_index FROM sensor_history"
                        : "SELECT alarm_time,sensor_name,slave_id,alarm_type,alarm_value,minimum_value,maximum_value FROM alarm_history";
                    sql += " WHERE " + time + ">=? AND " + time + "<=?";
                    if (sensor >= 0) sql += " AND sensor_index=?";
                    sql += " ORDER BY " + time + (operation == Samples ? " ASC" : " DESC")
                        + ",rowid " + (operation == Samples ? "ASC" : "DESC") + " LIMIT ? OFFSET ?";
                }
                if (!q.prepare(sql)) error = q.lastError().text();
                else {
                    if (operation != ClearAlarms) {
                        q.addBindValue(from); q.addBindValue(to);
                        if (sensor >= 0) q.addBindValue(sensor);
                        q.addBindValue(PageSize + 1); q.addBindValue(offset);
                    }
                    if (!q.exec()) error = q.lastError().text();
                    else if (operation != ClearAlarms) {
                        while (q.next()) {
                            if (rows.size() == PageSize) { more = true; break; }
                            QVariantList fields;
                            for (int i = 0; i < q.record().count(); ++i) fields.append(q.value(i));
                            rows.append(QVariant(fields));
                        }
                    }
                }
            }
        }
        QSqlDatabase::removeDatabase(name);
        QMetaObject::invokeMethod(this, [this, operation, rows, more, error] {
            m_busy = false;
            emit finished(operation, rows, more, error);
        });
    });
    return true;
}
