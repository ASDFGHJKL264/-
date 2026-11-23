#include "acquisitioncontroller.h"
#include <QDateTime>
#include <chrono>
#include <cmath>

namespace {
qint64 nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
AcquisitionController::AcquisitionController(QObject *parent, SensorTransport *transport)
    : QObject(parent), m_transport(transport ? transport : new RtuTransport(this))
{
    m_transport->setParent(this);
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer);
    m_reconnectTimer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &AcquisitionController::poll);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &AcquisitionController::reconnect);
    connect(m_transport, &SensorTransport::completed, this, &AcquisitionController::complete);
    connect(m_transport, &SensorTransport::connected, this,
            &AcquisitionController::connectionEstablished);
    connect(m_transport, &SensorTransport::disconnected, this,
            &AcquisitionController::connectionLost);
}
bool AcquisitionController::start(const AppConfig &config)
{
    if (m_active || m_opening || m_stopping) return false;
    QString error;
    if (!ConfigStore::validate(config, &error)) {
        emit logMessage(error, true); return false;
    }
    m_config = config; // immutable for this session
    m_current = -1;
    m_lastSample.fill(-1);
    m_retryStep = m_reconnectDelayMs = 0;
    m_reconnectAttempts = m_reconnectSuccesses = 0;
    m_offlineSince = -1;
    m_offlineMs = 0;
    int count = 0;
    for (const auto &s : config.sensors) if (s.enabled) ++count;
    m_slotMs = qMax(1, config.pollIntervalMs / count);
    m_connectDeadline = nowMs() + 5000;
    m_active = true;
    const auto id = ++m_requestId;
    setConnectionState(config.simulatorEnabled ? Connected : Connecting);
    if (!m_active || id != m_requestId) return false;
    if (!config.simulatorEnabled) {
        m_opening = true;
        const bool opened = m_transport->open(config);
        m_opening = false;
        if (!m_active || id != m_requestId) return false;
        // A failed manual start is not consent to connect indefinitely later.
        if (!opened) {
            stop();
            emit logMessage("串口打开失败，请检查接线、占用及通信参数", true);
            return false;
        }
    }
    m_sessionAnnounced = true;
    emit sessionChanged(true, config.simulatorEnabled);
    if (m_active && id == m_requestId) m_timer.start(0);
    return m_active && id == m_requestId;
}
void AcquisitionController::stop()
{
    if (m_stopping) return;
    m_stopping = true;
    const bool announced = m_sessionAnnounced;
    m_sessionAnnounced = false;
    m_active = false;
    m_timer.stop();
    m_reconnectTimer.stop();
    m_reconnectDelayMs = 0;
    m_pending = false;
    ++m_requestId; // old callbacks cannot complete a newer session's request
    m_transport->close();
    if (m_offlineSince >= 0) {
        m_offlineMs += qMax<qint64>(0, nowMs() - m_offlineSince);
        m_offlineSince = -1;
    }
    setConnectionState(Stopped);
    if (announced) emit sessionChanged(false, m_config.simulatorEnabled);
    m_stopping = false;
    // In-flight storage credits deliberately survive restart.
}
void AcquisitionController::setConnectionState(ConnectionState state)
{
    if (m_connectionState == state) return;
    m_connectionState = state;
    emit connectionStateChanged(state);
}
void AcquisitionController::connectionEstablished()
{
    // Ignore notifications outside the current connection attempt (including stop).
    if (!m_active || m_config.simulatorEnabled || m_connectionState != Connecting
        || !m_transport->isReady()) return;
    const bool recovered = m_offlineSince >= 0;
    if (recovered) {
        m_offlineMs += qMax<qint64>(0, nowMs() - m_offlineSince);
        m_offlineSince = -1;
        ++m_reconnectSuccesses;
    }
    m_retryStep = m_reconnectDelayMs = 0;
    m_lastSample.fill(-1); // Never count a disconnection as a normal sample interval.
    setConnectionState(Connected);
    if (m_active && m_connectionState == Connected && !m_pending) m_timer.start(0);
    if (recovered && m_active && m_connectionState == Connected)
        emit logMessage("串口重连成功，等待有效采样；原有报警仍按实际数据判断", false);
}
void AcquisitionController::connectionLost(const QString &reason)
{
    if (!m_active || m_config.simulatorEnabled || m_stopping || m_opening
        || m_connectionState == Reconnecting || m_connectionState == Stopped) return;
    if (!m_config.autoReconnect) {
        stop();
        emit logMessage("串口断开，自动重连已关闭：" + reason, true);
        return;
    }
    m_timer.stop();
    m_pending = false;
    const auto id = ++m_requestId;
    if (m_offlineSince < 0) m_offlineSince = nowMs();
    // Change state before close(): close may synchronously emit disconnected again.
    setConnectionState(Reconnecting);
    if (!m_active || id != m_requestId) return;
    m_transport->close();
    for (int i = 0; i < 4; ++i) {
        emit sampleLost(i);
        if (!m_active || id != m_requestId) return;
    }
    static constexpr int delays[] = {1000, 2000, 4000, 8000, 16000, 30000};
    m_reconnectDelayMs = delays[qMin(m_retryStep, 5)];
    m_retryStep = qMin(m_retryStep + 1, 5);
    m_reconnectTimer.start(m_reconnectDelayMs);
    emit logMessage(QString("串口不可用：%1；%2秒后重连原端口 %3，可点击断开取消")
                    .arg(reason).arg(m_reconnectDelayMs / 1000).arg(m_config.port), true);
}
void AcquisitionController::reconnect()
{
    if (!m_active || m_connectionState != Reconnecting) return;
    m_reconnectDelayMs = 0;
    const auto id = ++m_requestId;
    m_connectDeadline = nowMs() + 5000;
    setConnectionState(Connecting);
    if (!m_active || id != m_requestId) return;
    ++m_reconnectAttempts;
    m_opening = true;
    const bool opened = m_transport->open(m_config); // Original port/config only.
    m_opening = false;
    if (!m_active || id != m_requestId) return;
    if (!opened) {
        connectionLost("重新打开串口失败");
        return;
    }
    if (m_connectionState == Connecting && m_transport->isReady()) connectionEstablished();
    if (m_active && m_connectionState != Reconnecting) m_timer.start(0);
}
void AcquisitionController::schedule()
{
    if (m_active && m_connectionState == Connected)
        m_timer.start(int(qMax<qint64>(0, m_nextDue - nowMs())));
}
void AcquisitionController::poll()
{
    if (!m_active || m_connectionState == Reconnecting || m_connectionState == Stopped) return;
    if (!m_config.simulatorEnabled && !m_transport->isReady()) {
        if (m_connectionState == Connected || nowMs() >= m_connectDeadline) {
            connectionLost("串口连接失效或连接超时");
        } else m_timer.start(50);
        return;
    }
    if (m_connectionState == Connecting) connectionEstablished();
    if (!m_active || m_connectionState != Connected || m_pending) return;
    if (m_inFlight >= MaximumPendingSamples) {
        ++m_pauses; m_timer.start(50); return;
    }
    do { m_current = (m_current + 1) % 4; } while (!m_config.sensors[m_current].enabled);
    m_pending = true;
    ++m_requestId; ++m_requests;
    // Pace from request START, avoiding an extra full interval after every reply.
    // Slow responses delay the round; no concurrent RTU requests or catch-up burst.
    m_nextDue = nowMs() + m_slotMs;
    if (m_config.simulatorEnabled) {
        const double phase = m_requests * 0.08 + m_current;
        complete(m_requestId, true, 23 + 3.5 * std::sin(phase),
                 52 + 8 * std::sin(phase * .7), {});
    } else m_transport->read(m_requestId, m_config.sensors[m_current]);
}
void AcquisitionController::complete(quint64 id, bool ok, double t, double h, const QString &error)
{
    if (!m_active || !m_pending || id != m_requestId) { ++m_late; return; }
    m_pending = false;
    ok = ok && std::isfinite(t) && std::isfinite(h) && t >= -100 && t <= 200 && h >= 0 && h <= 100;
    if (!ok) {
        ++m_failed;
        emit sampleLost(m_current);
        emit logMessage(QString("模块%1读取失败：%2").arg(m_current + 1)
            .arg(error.isEmpty() ? "数值无效" : error), true);
    } else {
        ++m_success; ++m_inFlight; m_peak = qMax(m_peak, m_inFlight);
        const auto now = nowMs();
        if (m_lastSample[m_current] >= 0)
            m_maxInterval[m_current] = qMax(m_maxInterval[m_current], now - m_lastSample[m_current]);
        m_lastSample[m_current] = now;
        emit sampleReady(m_current, t, h, now, QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    }
    schedule();
}
void AcquisitionController::sampleProcessed()
{
    if (m_inFlight > 0) --m_inFlight;
}
QVariantMap AcquisitionController::metrics() const
{
    QVariantList intervals;
    for (auto v : m_maxInterval) intervals.append(v);
    static const QString states[] = {"已停止", "连接中", "已连接", "等待重连"};
    const qint64 offline = m_offlineMs + (m_offlineSince >= 0
        ? qMax<qint64>(0, nowMs() - m_offlineSince) : 0);
    return {{"requests", m_requests}, {"successfulReads", m_success}, {"failedReads", m_failed},
            {"ignoredReplies", m_late}, {"pending", m_inFlight}, {"peakPending", m_peak},
            {"backpressureChecks", m_pauses}, {"maxIntervalsMs", intervals},
            {"connectionState", states[m_connectionState]},
            {"reconnectAttempts", m_reconnectAttempts}, {"reconnectSuccesses", m_reconnectSuccesses},
            {"offlineMs", offline}, {"reconnectDelayMs", m_reconnectDelayMs}};
}
