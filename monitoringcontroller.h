#pragma once
#include "acquisitioncontroller.h"
#include "monitorservice.h"
#include "monitorcontroller.h"
#include <QThread>

// Coordinates acquisition and persistence; owned by the GUI, contains no widgets.
class MonitoringController final : public QObject {
    Q_OBJECT
public:
    MonitoringController(const QString &databasePath, const AppConfig &defaults,
                         QObject *parent = nullptr, SensorTransport *transport = nullptr);
    ~MonitoringController() override;
    bool start(const AppConfig &config);
    void stop();
    void shutdown();
    MonitorService *service() const { return m_service; }
    MonitorController *monitor() const { return m_monitor; }
    QVariantMap metrics() const;
signals:
    void ready(bool ok);
    void sampleReady(int zone, double temperature, double humidity);
    void sessionChanged(bool active);
    void connectionStateChanged(AcquisitionController::ConnectionState state);
    void logMessage(const QString &message, bool warning);
private:
    AcquisitionController m_acquisition;
    QThread m_thread;
    MonitorService *m_service = nullptr;
    MonitorController *m_monitor = nullptr;
    AppConfig m_sessionConfig;
    bool m_ready = false;
    bool m_shuttingDown = false;
    quint64 m_saved = 0, m_failed = 0;
    qint64 m_maxStorageMs = 0;
};
