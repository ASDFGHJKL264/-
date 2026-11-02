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
    void reloadRecords();
    void browseAlarms(int direction, int filter = -1);
    void filterAlarmHistory(int zone, const QString &from = {}, const QString &to = {});
    void browseTasks(int direction);
    void report(qint64 taskId);
    void shutdown();
  signals:
    void viewReady(const QVariantMap &view);
    void statusReady(int zone, const QString &name, const QString &state);
    void taskChanged(bool active, const QString &name);
    void logMessage(const QString &text, bool warning);
    void sampleProcessed();
    void sampleStored(bool saved, qint64 elapsedMs);
    void sessionRejected();
    void reportReady(const QString &csv);
    void initialized(bool ok);
    void configurationLocked(bool locked);

  private:
    bool migrateAlarmHistory();
    void tick();
    bool execute(const QString &sql, const QVariantList &args = {});
    bool persistStats();
    bool beginTransaction();
    bool commitTransaction();
    QVariantList rows(const QString &sql, const QVariantList &args = {});
    void fail(const QString &context);
    struct RecordPage {
        QList<qint64> beforeIds{0};
        qint64 snapshotId = 0, lastId = 0;
        bool hasMore = false;
    };
    static constexpr int RecordPageSize = 100;
    void movePage(RecordPage &page, int direction);
    QVariantList alarmRecords();
    QVariantList taskRecords();
    QVariantMap pageInfo(const RecordPage &page) const;
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
    RecordPage m_alarmPage, m_taskPage;
    int m_alarmFilter = 0; // 0 business, 1 unrecovered, 2 unacknowledged, 3 completed, 4 legacy archive
    int m_pinnedAlarms = 0;
    int m_alarmZone = -1;
    QString m_alarmFrom, m_alarmTo;
    std::unique_ptr<QLockFile> m_lock;
};
