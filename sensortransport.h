#pragma once
#include "appconfig.h"
#include <QObject>
#include <QModbusRtuSerialClient>

// Asynchronous transport boundary; production and injected test transports
// obey the same request-id contract. No QWidget or database dependency.
//这里的基类指的是通信接口类
class SensorTransport : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual bool open(const AppConfig &config) = 0;
    virtual void close() = 0;
    virtual bool isReady() const = 0;
    virtual void read(quint64 request, const SensorConfig &sensor) = 0;
signals:
    void connected();
    void disconnected(const QString &reason);
    void completed(quint64 request, bool ok, double temperature, double humidity,
                   const QString &error);
};
//这里的类是针对modbus通信专门造的类
class RtuTransport final : public SensorTransport {
    Q_OBJECT
public:
    explicit RtuTransport(QObject *parent = nullptr);
    bool open(const AppConfig &config) override;
    void close() override;
    bool isReady() const override;
    void read(quint64 request, const SensorConfig &sensor) override;
private:
    QModbusRtuSerialClient m_client;
};
