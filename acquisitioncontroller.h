#pragma once
#include "sensortransport.h"
#include <QTimer>
#include <QVariantMap>
#include <array>

class AcquisitionController final : public QObject {
    Q_OBJECT
public:
    enum ConnectionState { Stopped, Connecting, Connected, Reconnecting };
    Q_ENUM(ConnectionState)
    explicit AcquisitionController(QObject *parent = nullptr, SensorTransport *transport = nullptr);
    static constexpr int MaximumPendingSamples = 64;
    bool start(const AppConfig &config);
    void stop();
    bool active() const { return m_active; }
    ConnectionState connectionState() const { return m_connectionState; }
    void sampleProcessed();
    QVariantMap metrics() const;
signals:
    void sampleReady(int zone, double temperature, double humidity, qint64 monotonic,
                     const QString &wallTime);
    void sampleLost(int zone);
    void sessionChanged(bool active, bool simulator);
    void connectionStateChanged(AcquisitionController::ConnectionState state);
    void logMessage(const QString &message, bool warning);
private:
    friend class AcquisitionTests;
    void poll();
    void complete(quint64 id, bool ok, double t, double h, const QString &error);
    void schedule();
    void reconnect();
    void connectionLost(const QString &reason);
    void connectionEstablished();
    void setConnectionState(ConnectionState state);
    SensorTransport *m_transport;
    QTimer m_timer, m_reconnectTimer;
    AppConfig m_config;
    bool m_active = false, m_pending = false;
    bool m_opening = false, m_stopping = false, m_sessionAnnounced = false;
    ConnectionState m_connectionState = Stopped;
    quint64 m_requestId = 0, m_requests = 0, m_success = 0, m_failed = 0, m_late = 0;
    quint64 m_pauses = 0;
    int m_current = -1, m_inFlight = 0, m_peak = 0, m_slotMs = 500;
    qint64 m_nextDue = 0, m_connectDeadline = 0;
    int m_retryStep = 0, m_reconnectDelayMs = 0;
    quint64 m_reconnectAttempts = 0, m_reconnectSuccesses = 0;
    qint64 m_offlineSince = -1, m_offlineMs = 0;
    std::array<qint64, 4> m_lastSample{{-1, -1, -1, -1}};
    std::array<qint64, 4> m_maxInterval{};
};
