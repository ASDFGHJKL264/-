#pragma once
#include "monitorcore.h"
#include <QObject>
#include <QSqlDatabase>
#include <QVariantMap>
#include <QTimer>
#include <QLockFile>
#include <memory>

class MonitorService final : public QObject
{
    Q_OBJECT
  public:
    explicit MonitorService(QObject *parent = nullptr) : QObject(parent)
    {
    }
    static qint64 monotonicMs();
    static QVariantMap encodeRule(const ZoneRule &r);
    static ZoneRule decodeRule(const QVariantMap &m);
    void initialize(const QString &path, const QVariantList &defaults);
    void configure(const QVariantList &rules);
    void session(bool active, bool simulator);
    void sample(int zone, const QString &sensorName, int slaveId, double t, double h,
                qint64 monotonic, const QString &wallTime);
    void lost(int zone);
    void startTask(const QString &name, const QList<int> &zones, int intervalMs);
    void finishTask(const QString &status = QStringLiteral("completed"));
    void acknowledge(qint64 alarmId, const QString &operatorName, const QString &note);
    void refresh();
    void report(qint64 taskId);
    void shutdown();
  signals:
    void viewReady(const QVariantMap &view);
    void statusReady(int zone, const QString &name, const QString &state);
    void taskChanged(bool active, const QString &name);
    void logMessage(const QString &text, bool warning);
    void sampleProcessed();
    void reportReady(const QString &csv);
    void initialized(bool ok);
    void configurationLocked(bool locked);

  private:
    void tick();
    bool execute(const QString &sql, const QVariantList &args = {});
    bool persistStats();
    bool beginTransaction();
    bool commitTransaction();
    QVariantList rows(const QString &sql, const QVariantList &args = {});
    void fail(const QString &context);
    QSqlDatabase m_db;
    QString m_connection;
    std::array<ZoneMonitor, 4> m_zones;
    std::array<qint64, 8> m_alarmIds{};
    std::array<TaskStats, 4> m_stats;
    QList<int> m_taskZones;
    qint64 m_taskId = 0, m_started = 0;
    int m_interval = 500;
    bool m_ready = false, m_acquiring = false, m_simulator = false;
    QTimer *m_timer = nullptr;
    QString m_error;
    std::unique_ptr<QLockFile> m_lock;
};
