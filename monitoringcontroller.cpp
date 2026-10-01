#include "monitoringcontroller.h"
#include "databasemanager.h"
#include <QCoreApplication>
#include <QEvent>

MonitoringController::MonitoringController(const QString &path, const AppConfig &defaults,
                                         QObject *parent, SensorTransport *transport)
    : QObject(parent), m_acquisition(nullptr, transport), m_service(new MonitorService)
{
    m_service->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_service, &QObject::deleteLater);
    connect(m_service, &MonitorService::initialized, this, [this](bool ok) {
        if (m_shuttingDown) return;
        m_ready = ok; emit ready(ok);
    });
    connect(m_service, &MonitorService::logMessage, this, &MonitoringController::logMessage);
    connect(&m_acquisition, &AcquisitionController::logMessage, this, &MonitoringController::logMessage);
    connect(m_service, &MonitorService::sessionRejected, this, &MonitoringController::stop);
    connect(&m_acquisition, &AcquisitionController::sessionChanged, this,
            [this](bool active, bool simulator) {
        QMetaObject::invokeMethod(m_service, [s = m_service, active, simulator] { s->session(active, simulator); });
        emit sessionChanged(active);
    });
    connect(&m_acquisition, &AcquisitionController::sampleLost, this, [this](int zone) {
        QMetaObject::invokeMethod(m_service, [s = m_service, zone] { s->lost(zone); });
    });
    connect(&m_acquisition, &AcquisitionController::sampleReady, this,
            [this](int zone, double t, double h, qint64 mono, const QString &wall) {
        const auto sensor = m_sessionConfig.sensors[zone];
        QMetaObject::invokeMethod(m_service, [s = m_service, zone, sensor, t, h, mono, wall] {
            s->sample(zone, sensor.name, sensor.slaveId, t, h, mono, wall);
        });
        emit sampleReady(zone, t, h);
    });
    connect(m_service, &MonitorService::sampleStored, this, [this](bool saved, qint64 ms) {
        m_acquisition.sampleProcessed();
        saved ? ++m_saved : ++m_failed;
        m_maxStorageMs = qMax(m_maxStorageMs, ms);
    });
    QVariantList rules;
    for (const auto &sensor : defaults.sensors) {
        ZoneRule r;
        r.name = sensor.name;
        r.tempMin = defaults.temperatureMin; r.tempMax = defaults.temperatureMax;
        r.humMin = defaults.humidityMin; r.humMax = defaults.humidityMax;
        r.tempHysteresis = qMin(1.0, (r.tempMax - r.tempMin) / 4);
        r.humHysteresis = qMin(2.0, (r.humMax - r.humMin) / 4);
        r.staleMs = qBound(5000, defaults.pollIntervalMs * 3, 600000);
        rules.append(MonitorService::encodeRule(r));
    }
    m_thread.start();
    QMetaObject::invokeMethod(m_service, [s = m_service, path, rules] {
        QString error;
        {
            DatabaseManager schema(path);
            if (!schema.initialize(&error)) {
                emit s->logMessage("数据库初始化失败：" + error, true);
                emit s->initialized(false); return;
            }
            if (!schema.cleanupOldData(30, &error))
                emit s->logMessage("旧历史清理失败：" + error, true);
        }
        s->initialize(path, rules);
    });
}
MonitoringController::~MonitoringController() { shutdown(); }
bool MonitoringController::start(const AppConfig &config)
{
    if (m_shuttingDown || !m_ready) { emit logMessage("后台存储尚未就绪，不能开始采集", true); return false; }
    if (m_acquisition.active()) return false;
    m_sessionConfig = config;
    return m_acquisition.start(config);
}
void MonitoringController::stop() { m_acquisition.stop(); }
void MonitoringController::shutdown()
{
    if (m_shuttingDown) return;
    m_shuttingDown = true;
    if (!m_thread.isRunning()) return;
    stop();
    m_ready = false;
    QMetaObject::invokeMethod(m_service, [s = m_service] {
        s->shutdown(); QThread::currentThread()->quit();
    });
    m_thread.wait(); // producers stopped; bounded accepted work drains before shutdown
    m_service = nullptr;
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
}
QVariantMap MonitoringController::metrics() const
{
    auto result = m_acquisition.metrics();
    result.insert("saved", m_saved); result.insert("storageFailed", m_failed);
    result.insert("maxStorageMs", m_maxStorageMs);
    return result;
}
