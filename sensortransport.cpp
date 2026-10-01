#include "sensortransport.h"
#include "sensorutils.h"
#include <QModbusReply>
#include <QSerialPort>
#include <QVariant>

RtuTransport::RtuTransport(QObject *parent) : SensorTransport(parent)
{
    connect(&m_client, &QModbusDevice::stateChanged, this, [this](QModbusDevice::State s) {
        if (s == QModbusDevice::ConnectedState) emit connected();
        if (s == QModbusDevice::UnconnectedState) emit disconnected(m_client.errorString());
    });
}
bool RtuTransport::open(const AppConfig &c)
{
    m_client.setConnectionParameter(QModbusDevice::SerialPortNameParameter, c.port);
    m_client.setConnectionParameter(QModbusDevice::SerialBaudRateParameter, c.baud);
    m_client.setConnectionParameter(QModbusDevice::SerialDataBitsParameter, c.dataBits);
    const auto parity = c.parity == "奇校验" ? QSerialPort::OddParity
                      : c.parity == "偶校验" ? QSerialPort::EvenParity : QSerialPort::NoParity;
    m_client.setConnectionParameter(QModbusDevice::SerialParityParameter, parity);
    m_client.setConnectionParameter(QModbusDevice::SerialStopBitsParameter,
                                   c.stopBits == 2 ? QSerialPort::TwoStop : QSerialPort::OneStop);
    m_client.setTimeout(qMax(300, c.pollIntervalMs));
    m_client.setNumberOfRetries(1);
    return m_client.connectDevice();
}
void RtuTransport::close() { m_client.disconnectDevice(); }
bool RtuTransport::isReady() const { return m_client.state() == QModbusDevice::ConnectedState; }
void RtuTransport::read(quint64 request, const SensorConfig &sensor)
{
    const int first = qMin(sensor.temperatureRegister, sensor.humidityRegister);
    const int count = qAbs(sensor.temperatureRegister - sensor.humidityRegister) + 1;
    QModbusDataUnit unit(sensor.useInputRegisters ? QModbusDataUnit::InputRegisters
                                                : QModbusDataUnit::HoldingRegisters, first, count);
    auto *reply = m_client.sendReadRequest(unit, sensor.slaveId);
    if (!reply) {
        emit completed(request, false, 0, 0, m_client.errorString());
        return;
    }
    const auto finish = [this, reply, request, sensor, first] {
        const auto data = reply->result();
        const int ti = sensor.temperatureRegister - first, hi = sensor.humidityRegister - first;
        const bool ok = reply->error() == QModbusDevice::NoError
                     && ti < data.valueCount() && hi < data.valueCount();
        const QString error = reply->error() != QModbusDevice::NoError
            ? reply->errorString() : QStringLiteral("返回寄存器数量不足");
        emit completed(request, ok, ok ? SensorUtils::parseTemperature(data.value(ti)) : 0,
                       ok ? data.value(hi) * 0.1 : 0, ok ? QString() : error);
        reply->deleteLater();
    };
    if (reply->isFinished()) finish();
    else connect(reply, &QModbusReply::finished, this, finish);
}
