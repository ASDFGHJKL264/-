#include "sensortransport.h"
#include "sensorutils.h"
#include <QModbusReply>
#include <QSerialPort>
#include <QVariant>

RtuTransport::RtuTransport(QObject *parent) : SensorTransport(parent)
{
}
bool RtuTransport::open(const AppConfig &c)
{
    close();
    // Each attempt owns a fresh client: old state/reply callbacks cannot identify
    // themselves as the new connection after a quick stop/start or reconnect.
    auto *client = new QModbusRtuSerialClient(this);
    m_client = client;
    const auto generation = m_generation;
    connect(client, &QModbusDevice::stateChanged, this,
            [this, client, generation](QModbusDevice::State state) {
        if (client != m_client || generation != m_generation) return;
        if (state == QModbusDevice::ConnectedState) emit connected();
        if (state == QModbusDevice::UnconnectedState) emit disconnected(client->errorString());
    });
    connect(client, &QModbusDevice::errorOccurred, this,
            [this, client, generation](QModbusDevice::Error error) {
        if (client == m_client && generation == m_generation
            && error == QModbusDevice::ConnectionError)
            emit disconnected(client->errorString());
    });
    client->setConnectionParameter(QModbusDevice::SerialPortNameParameter, c.port);
    client->setConnectionParameter(QModbusDevice::SerialBaudRateParameter, c.baud);
    client->setConnectionParameter(QModbusDevice::SerialDataBitsParameter, c.dataBits);
    const auto parity = c.parity == "奇校验" ? QSerialPort::OddParity
                      : c.parity == "偶校验" ? QSerialPort::EvenParity : QSerialPort::NoParity;
    client->setConnectionParameter(QModbusDevice::SerialParityParameter, parity);
    client->setConnectionParameter(QModbusDevice::SerialStopBitsParameter,
                                   c.stopBits == 2 ? QSerialPort::TwoStop : QSerialPort::OneStop);
    client->setTimeout(qMax(300, c.pollIntervalMs));
    client->setNumberOfRetries(1);
    const bool opened = client->connectDevice();
    // Inspect the I/O device after open; some backends create it lazily.
    if (client == m_client && generation == m_generation) {
        if (auto *serial = qobject_cast<QSerialPort *>(client->device())) {
            connect(serial, &QSerialPort::errorOccurred, this,
                    [this, client, generation, serial](QSerialPort::SerialPortError error) {
                // A single slave timeout/protocol exception must not reconnect the bus.
                if (client == m_client && generation == m_generation
                    && error == QSerialPort::ResourceError)
                    emit disconnected(serial->errorString());
            });
        }
    }
    return opened && client == m_client && generation == m_generation;
}
void RtuTransport::close()
{
    auto *client = m_client;
    m_client = nullptr;
    ++m_generation;
    if (!client) return;
    client->disconnect(this);
    client->disconnectDevice();
    client->deleteLater();
}
bool RtuTransport::isReady() const
{
    return m_client && m_client->state() == QModbusDevice::ConnectedState
        && m_client->device() && m_client->device()->isOpen();
}
void RtuTransport::read(quint64 request, const SensorConfig &sensor)
{
    auto *client = m_client;
    const auto generation = m_generation;
    if (!isReady()) {
        emit completed(request, false, 0, 0, "串口未连接");
        return;
    }
    const int first = qMin(sensor.temperatureRegister, sensor.humidityRegister);
    const int count = qAbs(sensor.temperatureRegister - sensor.humidityRegister) + 1;
    QModbusDataUnit unit(sensor.useInputRegisters ? QModbusDataUnit::InputRegisters
                                                : QModbusDataUnit::HoldingRegisters, first, count);
    auto *reply = client->sendReadRequest(unit, sensor.slaveId);
    if (!reply) {
        if (client == m_client && generation == m_generation)
            emit completed(request, false, 0, 0, client->errorString());
        return;
    }
    const auto finish = [this, client, generation, reply, request, sensor, first] {
        if (client != m_client || generation != m_generation) {
            reply->deleteLater();
            return;
        }
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
