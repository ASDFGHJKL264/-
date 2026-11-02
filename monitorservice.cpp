#include "monitorservice.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>
#include <QSet>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QThread>
#include <chrono>
#include <cmath>

namespace
{
QString utcNow()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}
QString json(const QVariantMap &m)
{
    return QString::fromUtf8(QJsonDocument::fromVariant(m).toJson(QJsonDocument::Compact));
}
QString csvText(QString s)
{
    if (!s.isEmpty() && (QString("=+-@").contains(s.front()) || s.front().isSpace()))
        s.prepend('\'');
    s.replace('"', "\"\"");
    return '"' + s + '"';
}
} // namespace
qint64 MonitorService::monotonicMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
QVariantMap MonitorService::encodeRule(const ZoneRule &r)
{
    return {{"name", r.name},
            {"monitored", r.monitored},
            {"tempMin", r.tempMin},
            {"tempMax", r.tempMax},
            {"humMin", r.humMin},
            {"humMax", r.humMax},
            {"tempHysteresis", r.tempHysteresis},
            {"humHysteresis", r.humHysteresis},
            {"holdMs", r.holdMs},
            {"recoveryMs", r.recoveryMs},
            {"staleMs", r.staleMs}};
}
ZoneRule MonitorService::decodeRule(const QVariantMap &m)
{
    ZoneRule r;
    r.name = m.value("name").toString();
    r.monitored = m.value("monitored", true).toBool();
    r.tempMin = m.value("tempMin", -20).toDouble();
    r.tempMax = m.value("tempMax", 60).toDouble();
    r.humMin = m.value("humMin", 10).toDouble();
    r.humMax = m.value("humMax", 90).toDouble();
    r.tempHysteresis = m.value("tempHysteresis", 1).toDouble();
    r.humHysteresis = m.value("humHysteresis", 2).toDouble();
    r.holdMs = m.value("holdMs", 10000).toInt();
    r.recoveryMs = m.value("recoveryMs", 5000).toInt();
    r.staleMs = m.value("staleMs", 5000).toInt();
    return r;
}
bool MonitorService::execute(const QString &sql, const QVariantList &args)
{
    QSqlQuery q(m_db);
    if (!q.prepare(sql)) {
        m_error = q.lastError().text();
        return false;
    }
    for (const auto &a : args)
        q.addBindValue(a);
    if (!q.exec()) {
        m_error = q.lastError().text();
        return false;
    }
    return true;
}
QVariantList MonitorService::rows(const QString &sql, const QVariantList &args)
{
    QVariantList result;
    QSqlQuery q(m_db);
    q.prepare(sql);
    for (const auto &a : args)
        q.addBindValue(a);
    if (!q.exec()) {
        m_error = q.lastError().text();
        fail("查询失败");
        return result;
    }
    const auto record = q.record();
    while (q.next()) {
        QVariantMap row;
        for (int i = 0; i < record.count(); ++i)
            row[record.fieldName(i)] = q.value(i);
        result.append(row);
    }
    return result;
}
void MonitorService::fail(const QString &context)
{
    emit logMessage(context + "：" + m_error, true);
}
bool MonitorService::beginTransaction()
{
    if (m_db.transaction())
        return true;
    m_error = m_db.lastError().text();
    fail("数据库事务未开始");
    return false;
}
bool MonitorService::commitTransaction()
{
    if (m_db.commit())
        return true;
    m_error = m_db.lastError().text();
    return false;
}

bool MonitorService::migrateAlarmHistory()
{
    if (!beginTransaction())
        return false;
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    // ALTER and the copy/drop share one transaction: a failed migration leaves the old
    // table and every record intact. Do not infer missing lifecycle/source information.
    QSet<QString> columns;
    {
        QSqlQuery q(m_db);
        if (!q.exec("PRAGMA table_info(monitor_alarms)")) {
            m_error = q.lastError().text();
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
        QSqlQuery q(m_db);
        if (!q.exec("SELECT 1 FROM sqlite_master WHERE type='table' AND name='alarm_history'")) {
            m_error = q.lastError().text();
            return false;
        }
        hasLegacy = q.next();
    }
    if (hasLegacy) {
        {
            QSqlQuery q(m_db);
            if (!q.exec("SELECT * FROM alarm_history ORDER BY rowid")) {
                m_error = q.lastError().text();
                return false;
            }
            const auto record = q.record();
            for (const auto &field : {"alarm_time", "sensor_index", "sensor_name", "slave_id",
                                      "alarm_type", "alarm_value", "minimum_value", "maximum_value"}) {
                if (record.indexOf(field) < 0) {
                    m_error = QString("旧版报警缺少字段：%1").arg(field);
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
                m_error = q.lastError().text();
                return false;
            }
        } // Finish the SELECT before changing the table schema.
        if (!execute("DROP TABLE alarm_history"))
            return false;
    }
    if (!commitTransaction())
        return false;
    rollback.dismiss();
    return true;
}

void MonitorService::initialize(const QString &path, const QVariantList &defaults)
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_lock = std::make_unique<QLockFile>(path + ".monitor.lock");
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock()) {
        emit logMessage("另一实例正在使用业务数据库，当前业务模块未启动", true);
        emit initialized(false);
        return;
    }
    m_connection = "monitor-business-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_db = QSqlDatabase::addDatabase("QSQLITE", m_connection);
    m_db.setDatabaseName(path);
    m_db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=250");
    if (!m_db.open()) {
        m_error = m_db.lastError().text();
        fail("业务数据库打开失败");
        emit initialized(false);
        return;
    }
    if (!execute("PRAGMA journal_mode=WAL") || !execute("PRAGMA foreign_keys=ON")) {
        fail("数据库参数初始化失败");
        emit initialized(false);
        return;
    }
    const QStringList schema = {
        "CREATE TABLE IF NOT EXISTS monitor_rules(zone INTEGER PRIMARY KEY,config TEXT NOT NULL)",
        "CREATE TABLE IF NOT EXISTS monitor_tasks(id INTEGER PRIMARY KEY,name TEXT NOT NULL,started_at TEXT NOT NULL,ended_at TEXT,status TEXT NOT NULL,simulator INTEGER NOT NULL,interval_ms INTEGER NOT NULL,elapsed_ms INTEGER NOT NULL DEFAULT 0)",
        "CREATE TABLE IF NOT EXISTS monitor_task_zones(task_id INTEGER NOT NULL REFERENCES monitor_tasks(id),zone INTEGER NOT NULL,config TEXT NOT NULL,samples INTEGER NOT NULL DEFAULT 0,slots INTEGER NOT NULL DEFAULT 0,temp_min REAL,temp_max REAL,hum_min REAL,hum_max REAL,temp_exceeded_ms INTEGER NOT NULL DEFAULT 0,hum_exceeded_ms INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(task_id,zone))",
        "CREATE TABLE IF NOT EXISTS monitor_alarms(id INTEGER PRIMARY KEY,zone INTEGER NOT NULL,name TEXT NOT NULL,metric INTEGER NOT NULL,direction INTEGER NOT NULL,value REAL NOT NULL,minimum REAL NOT NULL,maximum REAL NOT NULL,raised_at TEXT NOT NULL,ack_at TEXT,operator TEXT,note TEXT,recovered_at TEXT,task_id INTEGER REFERENCES monitor_tasks(id),simulator INTEGER NOT NULL,slave_id INTEGER,legacy_record TEXT)",
        "CREATE INDEX IF NOT EXISTS monitor_alarm_active ON monitor_alarms(zone,metric,recovered_at)",
        "CREATE INDEX IF NOT EXISTS monitor_alarm_unacknowledged ON monitor_alarms(id DESC) WHERE ack_at IS NULL",
        "CREATE TABLE IF NOT EXISTS monitor_actions(id INTEGER PRIMARY KEY,time TEXT NOT NULL,kind TEXT NOT NULL,detail TEXT NOT NULL)"};
    for (const auto &sql : schema)
        if (!execute(sql)) {
            fail("业务表初始化失败");
            emit initialized(false);
            return;
        }
    if (!migrateAlarmHistory()) {
        fail("旧版报警迁移失败，原记录已保留");
        emit initialized(false);
        return;
    }
    // A prior process cannot continue a monotonic task timeline. Preserve its last committed
    // statistics.
    if (!execute("UPDATE monitor_tasks SET status='interrupted',ended_at=? WHERE status='running'",
                 {utcNow()})) {
        fail("恢复任务失败");
        emit initialized(false);
        return;
    }
    for (int i = 0; i < 4; ++i) {
        auto stored = rows("SELECT config FROM monitor_rules WHERE zone=?", {i});
        auto map =
            stored.isEmpty()
                ? defaults.value(i).toMap()
                : QJsonDocument::fromJson(stored.first().toMap()["config"].toString().toUtf8())
                      .toVariant()
                      .toMap();
        auto rule = decodeRule(map);
        if (!rule.valid()) {
            rule = ZoneRule{};
            rule.name = QString("区域%1").arg(i + 1);
            emit logMessage("无效区域配置已使用默认值", true);
        }
        m_zones[i].rule = rule;
        if (!execute("INSERT OR REPLACE INTO monitor_rules(zone,config) VALUES(?,?)",
                     {i, json(encodeRule(rule))})) {
            fail("默认区域规则持久化失败");
            emit initialized(false);
            return;
        }
    }
    for (const auto &item :
         rows("SELECT id,zone,metric,direction FROM monitor_alarms WHERE recovered_at IS NULL AND legacy_record IS NULL")) {
        auto r = item.toMap();
        int z = r["zone"].toInt(), metric = r["metric"].toInt();
        if (z >= 0 && z < 4 && metric >= 0 && metric < 2) {
            m_alarmIds[z * 2 + metric] = r["id"].toLongLong();
            m_zones[z].channels[metric].active = true;
            m_zones[z].channels[metric].direction = r["direction"].toInt();
        }
    }
    m_ready = true;
    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &MonitorService::tick);
    m_timer->start();
    emit initialized(true);
    emit logMessage("区域监控业务与异步存储已启用", false);
    refresh();
}

void MonitorService::configure(const QVariantList &rules)
{
    if (!m_ready || m_acquiring || m_taskId) {
        emit logMessage("请结束任务并断开采集后修改区域规则", true);
        return;
    }
    if (rules.size() != 4)
        return;
    std::array<ZoneRule, 4> proposed;
    QSet<QString> names;
    for (int i = 0; i < 4; ++i) {
        proposed[i] = decodeRule(rules[i].toMap());
        if (!proposed[i].valid() || names.contains(proposed[i].name)) {
            emit logMessage("区域名称不可重复，阈值/回差/时间参数必须有效", true);
            return;
        }
        names.insert(proposed[i].name);
        if (m_zones[i].channels[0].active || m_zones[i].channels[1].active) {
            if (encodeRule(proposed[i]) != encodeRule(m_zones[i].rule)) {
                emit logMessage("存在未恢复报警的区域不能修改规则", true);
                return;
            }
        }
    }
    if (!beginTransaction()) {
        m_error = m_db.lastError().text();
        fail("保存规则失败");
        return;
    }
    bool ok = true;
    for (int i = 0; i < 4; ++i)
        ok = ok && execute("INSERT OR REPLACE INTO monitor_rules(zone,config) VALUES(?,?)",
                           {i, json(encodeRule(proposed[i]))});
    ok = ok && execute("INSERT INTO monitor_actions(time,kind,detail) VALUES(?,'configure',?)",
                       {utcNow(), QString::fromUtf8(QJsonDocument::fromVariant(rules).toJson(
                                      QJsonDocument::Compact))});
    if (!ok || !commitTransaction()) {
        m_db.rollback();
        fail("保存区域规则失败");
        return;
    }
    for (int i = 0; i < 4; ++i) {
        m_zones[i].rule = proposed[i];
        m_zones[i].invalidate();
    }
    emit logMessage("区域规则已持久化", false);
    refresh();
    tick();
}

void MonitorService::session(bool active, bool simulator)
{
    if (!m_ready) {
        if (active)
            emit sessionRejected();
        return;
    }
    if (active && m_taskId && simulator != m_simulator) {
        emit logMessage("运行中任务不允许切换数据来源", true);
        emit sessionRejected();
        return;
    }
    if (active) {
        // Do not let simulated samples recover hardware incidents (or vice versa).
        auto open = rows(
            "SELECT id FROM monitor_alarms WHERE recovered_at IS NULL AND legacy_record IS NULL AND simulator<>? LIMIT 1",
            {simulator});
        if (!open.isEmpty()) {
            emit logMessage(
                "存在另一数据源的未恢复报警，采集已拒绝。请切回原数据源。", true);
            m_acquiring = false;
            emit sessionRejected();
            return;
        }
    }
    m_acquiring = active;
    // A rejected start/stop must not relabel an existing task's data source.
    if (active)
        m_simulator = simulator;
    if (!active)
        for (int i = 0; i < 4; ++i)
            lost(i);
    tick();
    refresh();
}
void MonitorService::lost(int zone)
{
    if (zone < 0 || zone >= 4)
        return;
    m_zones[zone].invalidate();
    if (m_taskId && m_taskZones.contains(zone))
        m_stats[zone].invalidate(monotonicMs() - m_started, m_zones[zone].rule.staleMs);
    emit statusReady(zone, m_zones[zone].rule.name, m_zones[zone].status());
}

void MonitorService::sample(int zone, const QString &name, int slave, double t, double h,
                            qint64 mono, const QString &wall)
{
    Q_ASSERT(QThread::currentThread() == thread());
    QElapsedTimer elapsed;
    elapsed.start();
    bool saved = false;
    const auto completion = qScopeGuard([&] {
        emit sampleProcessed();
        emit sampleStored(saved, elapsed.elapsed());
    });
    if (!m_ready || zone < 0 || zone >= 4)
        return;
    if (!std::isfinite(t) || !std::isfinite(h) || t < -100 || t > 200 || h < 0 || h > 100) {
        lost(zone);
        emit logMessage("收到无效温湿度数据，未入库", true);
        return;
    }
    if (!m_acquiring || mono > monotonicMs() || mono < m_zones[zone].lastSample
        || monotonicMs() - mono > m_zones[zone].rule.staleMs) {
        lost(zone);
        return;
    }
    auto old = m_zones[zone];
    auto oldStats = m_stats;
    auto ids = m_alarmIds;
    auto transitions = m_zones[zone].sample(mono, t, h);
    if (m_taskId && m_taskZones.contains(zone) && mono >= m_started)
        m_stats[zone].observe(qMax<qint64>(0, mono - m_started), m_interval, m_zones[zone].rule, t,
                              h);
    if (!beginTransaction()) {
        m_zones[zone] = old;
        m_stats = oldStats;
        m_error = m_db.lastError().text();
        fail("存储繁忙，样本未保存");
        lost(zone); // failed persistence also breaks pending/recovery continuity
        return;
    }
    bool ok = execute(
        "INSERT INTO sensor_history(sample_time,sensor_index,sensor_name,slave_id,temperature,humidity) VALUES(?,?,?,?,?,?)",
        {wall, zone, name, slave, t, h});
    const auto &r = m_zones[zone].rule;
    for (const auto &change : transitions) {
        if (!ok)
            break;
        int key = zone * 2 + change.metric;
        if (change.kind == AlarmTransition::Raised) {
            ok = execute(
                "INSERT INTO monitor_alarms(zone,name,metric,direction,value,minimum,maximum,raised_at,task_id,simulator,slave_id) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                {zone, r.name, change.metric, change.direction, change.value,
                 change.metric ? r.humMin : r.tempMin, change.metric ? r.humMax : r.tempMax,
                 utcNow(), m_taskZones.contains(zone) && m_taskId ? QVariant(m_taskId) : QVariant(),
                 m_simulator, slave});
            auto result = rows("SELECT last_insert_rowid() AS id");
            if (!result.isEmpty())
                m_alarmIds[key] = result.first().toMap()["id"].toLongLong();
        } else {
            ok = execute(
                "UPDATE monitor_alarms SET recovered_at=? WHERE id=? AND recovered_at IS NULL",
                {utcNow(), m_alarmIds[key]});
            m_alarmIds[key] = 0;
        }
    }
    ok = ok && persistStats();
    if (!ok || !commitTransaction()) {
        m_db.rollback();
        m_zones[zone] = old;
        m_stats = oldStats;
        m_alarmIds = ids;
        fail("样本/业务事务未保存");
        lost(zone);
        return;
    }
    saved = true;
    emit statusReady(zone, m_zones[zone].rule.name, m_zones[zone].status());
    for (const auto &c : transitions)
        emit logMessage(QString("【%1】%2 %3")
                            .arg(c.kind == AlarmTransition::Raised ? "报警发生" : "报警恢复",
                                 r.name, c.metric ? "湿度" : "温度"),
                        c.kind == AlarmTransition::Raised);
    if (!transitions.isEmpty()) {
        if (m_alarmPage.beforeIds.size() == 1)
            m_alarmPage = {};
        refresh();
    }
}
bool MonitorService::persistStats()
{
    if (!m_taskId)
        return true;
    for (int z : m_taskZones) {
        const auto &s = m_stats[z];
        if (!execute(
                "UPDATE monitor_task_zones SET samples=?,slots=?,temp_min=?,temp_max=?,hum_min=?,hum_max=?,temp_exceeded_ms=?,hum_exceeded_ms=? WHERE task_id=? AND zone=?",
                {s.samples, s.occupiedSlots, s.samples ? QVariant(s.tempMin) : QVariant(),
                 s.samples ? QVariant(s.tempMax) : QVariant(),
                 s.samples ? QVariant(s.humMin) : QVariant(),
                 s.samples ? QVariant(s.humMax) : QVariant(), s.tempExceededMs, s.humExceededMs,
                 m_taskId, z}))
            return false;
    }
    return execute("UPDATE monitor_tasks SET elapsed_ms=? WHERE id=?",
                   {monotonicMs() - m_started, m_taskId});
}
void MonitorService::tick()
{
    for (int i = 0; i < 4; ++i) {
        m_zones[i].tick(monotonicMs());
        emit statusReady(i, m_zones[i].rule.name, m_zones[i].status());
    }
}

void MonitorService::startTask(const QString &name, const QList<int> &zones, int interval)
{
    if (!m_ready || !m_acquiring || m_taskId || zones.isEmpty() || name.trimmed().isEmpty() ||
        name.size() > 100 || interval < 100 || interval > 60000) {
        emit logMessage("任务未启动：请先采集、填写名称并选择区域，且只能运行一个任务", true);
        return;
    }
    QSet<int> unique;
    for (int z : zones)
        if (z < 0 || z >= 4 || unique.contains(z) || !m_zones[z].rule.monitored) {
            emit logMessage("任务区域无效或暂停监控", true);
            return;
        } else
            unique.insert(z);
    if (!beginTransaction())
        return;
    bool ok = execute(
        "INSERT INTO monitor_tasks(name,started_at,status,simulator,interval_ms) VALUES(?,?,'running',?,?)",
        {name.trimmed(), utcNow(), m_simulator, interval});
    auto result = rows("SELECT last_insert_rowid() AS id");
    auto id = result.isEmpty() ? 0 : result.first().toMap()["id"].toLongLong();
    for (int z : zones)
        ok = ok && execute("INSERT INTO monitor_task_zones(task_id,zone,config) VALUES(?,?,?)",
                           {id, z, json(encodeRule(m_zones[z].rule))});
    if (!ok || !commitTransaction()) {
        m_db.rollback();
        fail("启动任务失败");
        return;
    }
    m_taskId = id;
    m_started = monotonicMs();
    m_interval = interval;
    m_taskZones = zones;
    m_stats = {};
    emit taskChanged(true, name);
    emit logMessage("监测任务已启动：" + name, false);
    m_taskPage = {};
    refresh();
}
void MonitorService::finishTask(const QString &status)
{
    if (!m_taskId)
        return;
    auto saved = m_stats;
    for (int z : m_taskZones)
        m_stats[z].accrue(monotonicMs() - m_started, m_zones[z].rule.staleMs);
    if (!beginTransaction()) {
        m_stats = saved;
        return;
    }
    bool ok = persistStats() && execute("UPDATE monitor_tasks SET status=?,ended_at=? WHERE id=?",
                                        {status, utcNow(), m_taskId});
    if (!ok || !commitTransaction()) {
        m_db.rollback();
        m_stats = saved;
        fail("结束任务失败，请重试");
        return;
    }
    m_taskId = 0;
    m_taskZones.clear();
    emit taskChanged(false, {});
    emit logMessage("监测任务已结束", false);
    m_taskPage = {};
    refresh();
}
void MonitorService::acknowledge(qint64 id, const QString &person, const QString &note)
{
    if (!m_ready || person.trimmed().isEmpty() || person.size() > 64 || note.trimmed().isEmpty() ||
        note.size() > 1000) {
        emit logMessage("请填写操作人和处理备注（最长64/1000字）", true);
        return;
    }
    QSqlQuery q(m_db);
    q.prepare("UPDATE monitor_alarms SET ack_at=?,operator=?,note=? WHERE id=? AND ack_at IS NULL AND legacy_record IS NULL");
    q.addBindValue(utcNow());
    q.addBindValue(person.trimmed());
    q.addBindValue(note.trimmed());
    q.addBindValue(id);
    if (!q.exec()) {
        m_error = q.lastError().text();
        fail("确认报警失败");
        return;
    }
    emit logMessage(
        q.numRowsAffected() ? "报警已确认，确认不会解除异常状态" : "记录不存在或已经确认", false);
    refresh();
}
void MonitorService::movePage(RecordPage &page, int direction)
{
    if (direction == 0) {
        page = {};
    } else if (direction < 0 && page.beforeIds.size() > 1) {
        page.beforeIds.removeLast();
    } else if (direction > 0 && page.hasMore && page.lastId > 0) {
        page.beforeIds.append(page.lastId);
    }
}

void MonitorService::reloadRecords()
{
    m_alarmPage = {};
    m_taskPage = {};
    refresh();
}

void MonitorService::browseAlarms(int direction, int filter)
{
    if (filter >= 0 && filter <= 4 && filter != m_alarmFilter) {
        m_alarmFilter = filter;
        m_alarmPage = {};
    } else {
        movePage(m_alarmPage, direction);
    }
    refresh();
}

void MonitorService::browseTasks(int direction)
{
    movePage(m_taskPage, direction);
    refresh();
}

void MonitorService::filterAlarmHistory(int zone, const QString &from, const QString &to)
{
    if (zone < -1 || zone >= 4)
        return;
    const auto start = QDateTime::fromString(from, Qt::ISODateWithMs);
    const auto end = QDateTime::fromString(to, Qt::ISODateWithMs);
    if ((!from.isEmpty() || !to.isEmpty()) &&
        (!start.isValid() || !end.isValid() || start > end)) {
        emit logMessage("报警查询时间范围无效", true);
        return;
    }
    m_alarmZone = zone;
    m_alarmFrom = from.isEmpty() ? QString() : start.toUTC().toString(Qt::ISODateWithMs);
    m_alarmTo = to.isEmpty() ? QString() : end.toUTC().toString(Qt::ISODateWithMs);
    m_alarmPage = {};
    refresh();
}

QVariantMap MonitorService::pageInfo(const RecordPage &page) const
{
    return {{"number", page.beforeIds.size()},
            {"hasPrevious", page.beforeIds.size() > 1},
            {"hasMore", page.hasMore},
            {"pageSize", RecordPageSize},
            {"snapshotId", page.snapshotId}};
}

QVariantList MonitorService::alarmRecords()
{
    // The unique (zone, metric) index bounds normal unrecovered incidents to eight.
    // Pin them separately so an old incident remains visible on every history page.
    QVariantList pinned;
    if (m_alarmFilter != 3 && m_alarmFilter != 4) {
        QString sql = "SELECT * FROM monitor_alarms WHERE recovered_at IS NULL AND legacy_record IS NULL";
        QVariantList args;
        if (m_alarmZone >= 0) {
            sql += " AND zone=?";
            args.append(m_alarmZone);
        }
        if (m_alarmFilter == 2)
            sql += " AND ack_at IS NULL";
        pinned = rows(sql + " ORDER BY id DESC LIMIT 8", args);
    }
    m_pinnedAlarms = pinned.size();
    m_alarmPage.hasMore = false;
    m_alarmPage.lastId = 0;
    if (m_alarmFilter == 1)
        return pinned;
    if (m_alarmPage.snapshotId == 0) {
        const auto maximum = rows("SELECT COALESCE(MAX(id),0) AS id FROM monitor_alarms");
        if (!maximum.isEmpty())
            m_alarmPage.snapshotId = maximum.first().toMap()["id"].toLongLong();
    }
    QString sql = m_alarmFilter == 4
        ? "SELECT * FROM monitor_alarms WHERE legacy_record IS NOT NULL AND id<=?"
        : "SELECT * FROM monitor_alarms WHERE legacy_record IS NULL AND recovered_at IS NOT NULL AND id<=?";
    QVariantList args{m_alarmPage.snapshotId};
    if (m_alarmZone >= 0) {
        sql += " AND zone=?";
        args.append(m_alarmZone);
    }
    if (!m_alarmFrom.isEmpty()) {
        sql += " AND raised_at>=? AND raised_at<=?";
        args.append(m_alarmFrom);
        args.append(m_alarmTo);
    }
    if (m_alarmFilter == 2)
        sql += " AND ack_at IS NULL";
    else if (m_alarmFilter == 3)
        sql += " AND ack_at IS NOT NULL";
    if (m_alarmPage.beforeIds.last() > 0) {
        sql += " AND id<?";
        args.append(m_alarmPage.beforeIds.last());
    }
    sql += " ORDER BY id DESC LIMIT ?";
    args.append(RecordPageSize + 1);
    auto history = rows(sql, args);
    m_alarmPage.hasMore = history.size() > RecordPageSize;
    if (m_alarmPage.hasMore)
        history.removeLast();
    if (!history.isEmpty())
        m_alarmPage.lastId = history.last().toMap()["id"].toLongLong();
    pinned.append(history);
    return pinned;
}

QVariantList MonitorService::taskRecords()
{
    if (m_taskPage.snapshotId == 0) {
        const auto maximum = rows("SELECT COALESCE(MAX(id),0) AS id FROM monitor_tasks");
        if (!maximum.isEmpty())
            m_taskPage.snapshotId = maximum.first().toMap()["id"].toLongLong();
    }
    QString sql = "SELECT * FROM monitor_tasks WHERE id<=?";
    QVariantList args{m_taskPage.snapshotId};
    if (m_taskPage.beforeIds.last() > 0) {
        sql += " AND id<?";
        args.append(m_taskPage.beforeIds.last());
    }
    sql += " ORDER BY id DESC LIMIT ?";
    args.append(RecordPageSize + 1);
    auto result = rows(sql, args);
    m_taskPage.hasMore = result.size() > RecordPageSize;
    if (m_taskPage.hasMore)
        result.removeLast();
    m_taskPage.lastId = result.isEmpty() ? 0 : result.last().toMap()["id"].toLongLong();
    return result;
}

void MonitorService::refresh()
{
    if (!m_ready)
        return;
    bool locked = m_taskId != 0;
    for (auto id : m_alarmIds)
        locked = locked || id != 0;
    emit configurationLocked(locked);
    QVariantList rules;
    for (const auto &z : m_zones)
        rules.append(encodeRule(z.rule));
    const auto alarms = alarmRecords();
    const auto tasks = taskRecords();
    auto alarmPage = pageInfo(m_alarmPage);
    alarmPage["filter"] = m_alarmFilter;
    alarmPage["pinned"] = m_pinnedAlarms;
    emit viewReady({{"rules", rules},
                    {"taskActive", m_taskId != 0},
                    {"acquiring", m_acquiring},
                    {"alarms", alarms},
                    {"tasks", tasks},
                    {"alarmPage", alarmPage},
                    {"taskPage", pageInfo(m_taskPage)}});
}
void MonitorService::report(qint64 id)
{
    if (!m_ready)
        return;
    auto task = rows("SELECT * FROM monitor_tasks WHERE id=?", {id});
    if (task.isEmpty())
        return;
    auto t = task.first().toMap();
    if (t["status"] == "running") {
        emit logMessage("请先结束任务再导出最终报告", true);
        return;
    }
    QString csv =
        QString(QChar(0xfeff)) +
        QStringLiteral(
            "任务ID,任务名称,状态,数据源,开始UTC,结束UTC,区域,有效样本数,计划时间槽,有效时间槽,缺失时间槽,覆盖率%,最低温度,最高温度,最低湿度,最高湿度,温度超限估算秒,湿度超限估算秒,规则快照\r\n");
    const auto duration = t["elapsed_ms"].toLongLong();
    const auto interval = qMax(1, t["interval_ms"].toInt());
    for (const auto &item :
         rows("SELECT * FROM monitor_task_zones WHERE task_id=? ORDER BY zone", {id})) {
        auto r = item.toMap();
        auto rule = QJsonDocument::fromJson(r["config"].toString().toUtf8()).toVariant().toMap();
        auto valid = r["slots"].toLongLong();
        auto expected = qMax(valid, qMax<qint64>(1, (duration + interval - 1) / interval));
        QStringList fields = {QString::number(id),
                              csvText(t["name"].toString()),
                              csvText(t["status"].toString()),
                              t["simulator"].toBool() ? "模拟" : "硬件",
                              csvText(t["started_at"].toString()),
                              csvText(t["ended_at"].toString()),
                              csvText(rule["name"].toString()),
                              r["samples"].toString(),
                              QString::number(expected),
                              QString::number(valid),
                              QString::number(expected - valid),
                              QString::number(valid * 100.0 / expected, 'f', 2),
                              r["temp_min"].toString(),
                              r["temp_max"].toString(),
                              r["hum_min"].toString(),
                              r["hum_max"].toString(),
                              QString::number(r["temp_exceeded_ms"].toLongLong() / 1000.0, 'f', 3),
                              QString::number(r["hum_exceeded_ms"].toLongLong() / 1000.0, 'f', 3),
                              csvText(r["config"].toString())};
        csv += fields.join(',') + "\r\n";
    }
    csv +=
        "\r\n说明,缺失时间槽不计为正常；超限时长采用末值保持且最多保持至失效阈值，仅为估算；中断任务统计截止最后持久化点。\r\n";
    emit reportReady(csv);
}
void MonitorService::shutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_timer)
        m_timer->stop();
    if (m_ready)
        finishTask("interrupted");
    m_ready = false;
    m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connection);
    m_lock.reset();
}
