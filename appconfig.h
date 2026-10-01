#pragma once
#include <QString>

struct SensorConfig {
    bool enabled = false;
    QString name;
    int slaveId = 1;
    bool useInputRegisters = false;
    int temperatureRegister = 0;
    int humidityRegister = 1;
};

struct AppConfig {
    QString port = "COM1";
    int baud = 9600;
    int dataBits = 8;
    QString parity = "无校验";
    int stopBits = 1;
    int pollIntervalMs = 500;
    bool simulatorEnabled = false;
    double temperatureMin = -20.0, temperatureMax = 60.0;
    double humidityMin = 10.0, humidityMax = 90.0;
    SensorConfig sensors[4];
};

class ConfigStore {
public:
    static AppConfig load(const QString &path);
    static bool save(const QString &path, const AppConfig &config, QString *error);
    static bool validate(const AppConfig &config, QString *error);
};
