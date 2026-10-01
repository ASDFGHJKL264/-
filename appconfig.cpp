#include "appconfig.h"
#include <QSettings>
#include <QSet>
#include <QStringList>
#include <cmath>
AppConfig ConfigStore::load(const QString &path)
{
    AppConfig config;
    QSettings settings(path, QSettings::IniFormat);
    config.port = settings.value("Serial/port", "COM1").toString();
    config.baud = settings.value("Serial/baud", 9600).toInt();
    config.dataBits = settings.value("Serial/databits", 8).toInt();
    config.parity = settings.value("Serial/parity", "无校验").toString();
    config.stopBits = settings.value("Serial/stopbits", 1).toInt();
    config.pollIntervalMs =
        qBound(100, settings.value("Acquisition/intervalMs", 500).toInt(), 60000);
    config.simulatorEnabled = settings.value("Acquisition/simulator", false).toBool();
    config.temperatureMin = settings.value("Alarm/tempMin", -20.0).toDouble();
    config.temperatureMax = settings.value("Alarm/tempMax", 60.0).toDouble();
    config.humidityMin = settings.value("Alarm/humMin", 10.0).toDouble();
    config.humidityMax = settings.value("Alarm/humMax", 90.0).toDouble();

    for (int i = 0; i < 4; ++i) {
        const QString prefix = QString("Sensor%1/").arg(i + 1);
        auto &sensor = config.sensors[i];
        sensor.enabled = settings.value(prefix + "enabled", i == 0).toBool();
        sensor.name = settings.value(prefix + "name", QString("模块%1").arg(i + 1)).toString();
        sensor.slaveId = settings.value(prefix + "slaveId", i + 1).toInt();
        sensor.useInputRegisters = settings.value(prefix + "useInputReg", false).toBool();
        sensor.temperatureRegister = settings.value(prefix + "tempAddr", 0).toInt();
        sensor.humidityRegister = settings.value(prefix + "humAddr", 1).toInt();
    }
    return config;
}
bool ConfigStore::validate(const AppConfig &config, QString *errorMessage)
{
    QString ignored;
    if (!errorMessage) errorMessage = &ignored;
    if (config.pollIntervalMs < 100 || config.pollIntervalMs > 60000 || config.baud <= 0
        || config.dataBits < 5 || config.dataBits > 8 || (config.stopBits != 1 && config.stopBits != 2)
        || !QStringList({"无校验", "奇校验", "偶校验"}).contains(config.parity)
        || (!config.simulatorEnabled && (config.port.trimmed().isEmpty() || config.port == "SIMULATOR"))
        || !std::isfinite(config.temperatureMin) || !std::isfinite(config.temperatureMax)
        || !std::isfinite(config.humidityMin) || !std::isfinite(config.humidityMax)
        || config.temperatureMin < -100 || config.temperatureMax > 200
        || config.humidityMin < 0 || config.humidityMax > 100) {
        *errorMessage = "采集周期、串口或报警范围无效"; return false;
    }
    if (config.temperatureMin >= config.temperatureMax ||
        config.humidityMin >= config.humidityMax) {
        *errorMessage = "报警下限必须小于上限。";
        return false;
    }
    int enabledCount = 0;
    QSet<int> addresses;
    for (const auto &sensor : config.sensors) {
        if (!sensor.enabled)
            continue;
        ++enabledCount;
        if (sensor.slaveId < 1 || sensor.slaveId > 247) {
            *errorMessage = "从机地址必须在 1～247 之间。";
            return false;
        }
        if (addresses.contains(sensor.slaveId)) {
            *errorMessage = "启用的传感器不能使用重复从机地址。";
            return false;
        }
        addresses.insert(sensor.slaveId);
        if (sensor.temperatureRegister < 0 || sensor.humidityRegister < 0 ||
            sensor.temperatureRegister > 65535 || sensor.humidityRegister > 65535 ||
            qAbs(sensor.temperatureRegister - sensor.humidityRegister) >= 125) {
            *errorMessage = "寄存器地址无效，或两寄存器跨度超过 Modbus 单次读取上限。";
            return false;
        }
    }
    if (enabledCount == 0) {
        *errorMessage = "至少要启用一个传感器。";
        return false;
    }
    return true;
}

bool ConfigStore::save(const QString &path, const AppConfig &config, QString *error)
{
    if (!validate(config, error)) return false;
    QSettings settings(path, QSettings::IniFormat);
    settings.setValue("Serial/port", config.port);
    settings.setValue("Serial/baud", config.baud);
    settings.setValue("Serial/databits", config.dataBits);
    settings.setValue("Serial/parity", config.parity);
    settings.setValue("Serial/stopbits", config.stopBits);
    settings.setValue("Acquisition/intervalMs", config.pollIntervalMs);
    settings.setValue("Acquisition/simulator", config.simulatorEnabled);
    settings.setValue("Alarm/tempMin", config.temperatureMin);
    settings.setValue("Alarm/tempMax", config.temperatureMax);
    settings.setValue("Alarm/humMin", config.humidityMin);
    settings.setValue("Alarm/humMax", config.humidityMax);
    for (int i = 0; i < 4; ++i) {
        const QString prefix = QString("Sensor%1/").arg(i + 1);
        const auto &sensor = config.sensors[i];
        settings.setValue(prefix + "enabled", sensor.enabled);
        settings.setValue(prefix + "name", sensor.name);
        settings.setValue(prefix + "slaveId", sensor.slaveId);
        settings.setValue(prefix + "useInputReg", sensor.useInputRegisters);
        settings.setValue(prefix + "tempAddr", sensor.temperatureRegister);
        settings.setValue(prefix + "humAddr", sensor.humidityRegister);
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (error) *error = "配置写入失败";
        return false;
    }
    return true;
}
