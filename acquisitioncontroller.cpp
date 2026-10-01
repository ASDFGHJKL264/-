#include "acquisitioncontroller.h"
#include <QDateTime>
#include <QRandomGenerator>
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
    connect(&m_timer, &QTimer::timeout, this, &AcquisitionController::poll);
    connect(m_transport, &SensorTransport::completed, this, &AcquisitionController::complete);
    connect(m_transport, &SensorTransport::connected, this, [this] {
        if (m_active && !m_pending) m_timer.start(0);
    });
    connect(m_transport, &SensorTransport::disconnected, this, [this](const QString &reason) {
        if (!m_active || m_config.simulatorEnabled) return;
        emit logMessage("串口断开：" + reason, true);
        stop();
    });
}
bool AcquisitionController::start(const AppConfig &config)
{
    if (m_active) return false;
    QString error;
    if (!ConfigStore::validate(config, &error)) {
        emit logMessage(error, true); return false;
    }
    m_config = config; // immutable for this session
    m_current = -1;
    m_lastSample.fill(-1);
    int count = 0;
    for (const auto &s : config.sensors) if (s.enabled) ++count;
    m_slotMs = qMax(1, config.pollIntervalMs / count);
    m_connectDeadline = nowMs() + 5000;
    m_active = true;
    if (!config.simulatorEnabled && !m_transport->open(config)) {
        stop(); emit logMessage("串口打开失败，请检查接线、占用及通信参数", true); return false;
    }
    emit sessionChanged(true, config.simulatorEnabled);
    m_timer.start(0);
    return true;
}
void AcquisitionController::stop()
{
    const bool wasActive = m_active;
    m_active = false;
    m_timer.stop();
    m_pending = false;
    ++m_requestId; // old callbacks cannot complete a newer session's request
    m_transport->close();
    if (wasActive) emit sessionChanged(false, m_config.simulatorEnabled);
    // In-flight storage credits deliberately survive restart.
}
void AcquisitionController::schedule()
{
    if (m_active) m_timer.start(int(qMax<qint64>(0, m_nextDue - nowMs())));
}
void AcquisitionController::poll()
{
    if (!m_active || m_pending) return;
    if (m_inFlight >= MaximumPendingSamples) {
        ++m_pauses; m_timer.start(50); return;
    }
    if (!m_config.simulatorEnabled && !m_transport->isReady()) {
        if (nowMs() >= m_connectDeadline) {
            emit logMessage("连接超时，采集已停止", true); stop();
        } else m_timer.start(50);
        return;
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
    return {{"requests", m_requests}, {"successfulReads", m_success}, {"failedReads", m_failed},
            {"ignoredReplies", m_late}, {"pending", m_inFlight}, {"peakPending", m_peak},
            {"backpressureChecks", m_pauses}, {"maxIntervalsMs", intervals}};
}
