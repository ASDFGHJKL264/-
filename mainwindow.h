#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "qcustomplot.h"
#include "databasemanager.h"

#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QLineEdit>
#include <QMainWindow>
#include <QModbusRtuSerialClient>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>

struct SensorConfig
{
    bool enabled = false;
    QString name;
    int slaveId = 1;
    bool useInputRegisters = false;
    int temperatureRegister = 0;
    int humidityRegister = 1;
};

struct AppConfig
{
    QString port = "COM1";
    int baud = 9600;
    int dataBits = 8;
    QString parity = "无校验";
    int stopBits = 1;
    int pollIntervalMs = 500;
    bool simulatorEnabled = false;
    double temperatureMin = -20.0;
    double temperatureMax = 60.0;
    double humidityMin = 10.0;
    double humidityMax = 90.0;
    SensorConfig sensors[4];
};

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void initUI();
    void initSerialParameters();
    void refreshPortList();
    void loadConfig();
    void saveConfig();
    void applyConfigToUi();
    void toggleConnect();
    void pollNextSensor();
    void requestSensor(int index);
    void handleSample(int index, double temperature, double humidity);
    void updatePlot(int index, double temperature, double humidity);
    void scheduleNextPoll(int delayMs = -1);
    void printLog(const QString &message, bool warning = false);
    void rotateLogIfNeeded();
    void initializeDatabase();
    void initializeDataMenus();
    void showSensorConfigDialog();
    QString configFilePath() const;
    QString logDirectoryPath() const;
    int nextEnabledSensor(int after) const;
    bool validateConfig(QString *errorMessage) const;

    QComboBox *m_cbxPort = nullptr;
    QComboBox *m_cbxBaud = nullptr;
    QComboBox *m_cbxParity = nullptr;
    QComboBox *m_cbxDataBit = nullptr;
    QComboBox *m_cbxStopBit = nullptr;
    QPushButton *m_btnConnect = nullptr;
    QPushButton *m_btnDisconnect = nullptr;
    QPushButton *m_btnSaveCfg = nullptr;
    QLineEdit *m_edtHumidity[4]{};
    QLineEdit *m_edtTemperature[4]{};
    QTextEdit *m_txtLog = nullptr;
    QCustomPlot *m_plot = nullptr;

    QModbusRtuSerialClient *m_modbusClient = nullptr;
    QTimer *m_pollTimer = nullptr;
    QTimer *m_plotRefreshTimer = nullptr;
    AppConfig m_config;
    int m_currentSensor = -1;
    bool m_requestPending = false;
    bool m_plotPaused = false;
    qint64 m_sampleSequence = 0;
    QDateTime m_lastAlarm[4][2];
    DatabaseManager m_database;
    bool m_databaseAvailable = false;
};

#endif
