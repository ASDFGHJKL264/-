#pragma once
#include <QObject>
#include <QPointer>
#include <QVariantMap>

class MonitorService;

// UI-facing business commands. The service owns all state and SQL on its thread.
// This adapter does not own the service or start/stop its thread.
class MonitorController final : public QObject
{
    Q_OBJECT
public:
    explicit MonitorController(MonitorService *service, QObject *parent = nullptr);
    void configure(const QVariantList &rules);
    void reloadRecords();
    void browseAlarms(int direction, int filter = -1);
    void filterAlarmHistory(int zone, const QString &from, const QString &to);
    void browseTasks(int direction);
    void acknowledge(qint64 id, const QString &operatorName, const QString &note);
    void startTask(const QString &name, const QList<int> &zones, int intervalMs);
    void finishTask();
    void report(qint64 taskId);
    void deactivate();
signals:
    void viewReady(const QVariantMap &view);
    void statusReady(int zone, const QString &name, const QString &state);
    void taskChanged(bool active, const QString &name);
    void configurationLocked(bool locked);
    void reportReady(const QString &csv);
    void logMessage(const QString &message, bool warning);
private:
    QPointer<MonitorService> m_service;
};
