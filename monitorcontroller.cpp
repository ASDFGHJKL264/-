#include "monitorcontroller.h"
#include "monitorservice.h"

MonitorController::MonitorController(MonitorService *service, QObject *parent)
    : QObject(parent), m_service(service)
{
    Q_ASSERT(service);
    connect(service, &MonitorService::viewReady, this, &MonitorController::viewReady);
    connect(service, &MonitorService::statusReady, this, &MonitorController::statusReady);
    connect(service, &MonitorService::taskChanged, this, &MonitorController::taskChanged);
    connect(service, &MonitorService::configurationLocked, this, &MonitorController::configurationLocked);
    connect(service, &MonitorService::reportReady, this, &MonitorController::reportReady);
    connect(service, &MonitorService::logMessage, this, &MonitorController::logMessage);
}

void MonitorController::configure(const QVariantList &rules)
{
    if (auto *s = m_service.data())
        QMetaObject::invokeMethod(s, [s, rules] { s->configure(rules); });
}
void MonitorController::reloadRecords()
{
    if (auto *s = m_service.data())
        QMetaObject::invokeMethod(s, [s] { s->reloadRecords(); });
}
void MonitorController::browseAlarms(int direction, int filter)
{
    if (auto *s = m_service.data())
        QMetaObject::invokeMethod(s, [s, direction, filter] { s->browseAlarms(direction, filter); });
}
void MonitorController::filterAlarmHistory(int zone, const QString &from, const QString &to)
{
    if (auto *s = m_service.data())
        QMetaObject::invokeMethod(s, [s, zone, from, to] { s->filterAlarmHistory(zone, from, to); });
}
void MonitorController::browseTasks(int direction)
{
    if (auto *s = m_service.data())
        QMetaObject::invokeMethod(s, [s, direction] { s->browseTasks(direction); });
}
void MonitorController::acknowledge(qint64 id, const QString &operatorName, const QString &note)
{
    if (auto *s = m_service.data())
        QMetaObject::invokeMethod(s, [s, id, operatorName, note] { s->acknowledge(id, operatorName, note); });
}
void MonitorController::startTask(const QString &name, const QList<int> &zones, int intervalMs)
{
    if (auto *s = m_service.data())
        QMetaObject::invokeMethod(s, [s, name, zones, intervalMs] { s->startTask(name, zones, intervalMs); });
}
void MonitorController::finishTask()
{
    if (auto *s = m_service.data())
        QMetaObject::invokeMethod(s, [s] { s->finishTask(); });
}
void MonitorController::report(qint64 taskId)
{
    if (auto *s = m_service.data())
        QMetaObject::invokeMethod(s, [s, taskId] { s->report(taskId); });
}
void MonitorController::deactivate()
{
    if (m_service) disconnect(m_service, nullptr, this, nullptr);
    m_service.clear();
}
