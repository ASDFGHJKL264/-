#pragma once
#include "sensortransport.h"
#include <QTimer>
#include <QVariantMap>
#include <array>

class AcquisitionController final : public QObject {
    Q_OBJECT
public:
    explicit AcquisitionController(QObject *parent = nullptr, SensorTransport *transport = nullptr);
    static constexpr int MaximumPendingSamples = 64;
    bool start(const AppConfig &config);
    void stop();
    bool active() const { return m_active; }
    void sampleProcessed();
    QVariantMap metrics() const;
signals:
    void sampleReady(int zone, double temperature, double humidity, qint64 monotonic,
                     const QString &wallTime);
    void sampleLost(int zone);
    void sessionChanged(bool active, bool simulator);
    void logMessage(const QString &message, bool warning);
private:
    void poll();
    void complete(quint64 id, bool ok, double t, double h, const QString &error);
    void schedule();
    SensorTransport *m_transport;
    QTimer m_timer;
    AppConfig m_config;
    bool m_active = false, m_pending = false;
    quint64 m_requestId = 0, m_requests = 0, m_success = 0, m_failed = 0, m_late = 0;
    quint64 m_pauses = 0;
    int m_current = -1, m_inFlight = 0, m_peak = 0, m_slotMs = 500;
    qint64 m_nextDue = 0, m_connectDeadline = 0;
    std::array<qint64, 4> m_lastSample{{-1, -1, -1, -1}};
    std::array<qint64, 4> m_maxInterval{};
};
