#include "mainwindow.h"
#include "alarmdialog.h"
#include "historydialog.h"
#include "sensorconfigdialog.h"
#include "sensorutils.h"
#include "monitorservice.h"
#include "monitordialog.h"
#include <QThread>
#include <QStatusBar>
#include <cmath>

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMenuBar>
#include <QRandomGenerator>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QSettings>
#include <QSet>
#include <QTextStream>
#include <QtMath>
#include <QVBoxLayout>

namespace
{
constexpr int kSensorLimit = 4;
constexpr qint64 kMaxLogBytes = 5 * 1024 * 1024;
constexpr int kLogsToKeep = 10;
constexpr int kMaximumPlotPoints = 1200;
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), m_modbusClient(new QModbusRtuSerialClient(this)),
      m_pollTimer(new QTimer(this)), m_plotRefreshTimer(new QTimer(this))
{
    initUI();
    initSerialParameters();
    loadConfig();
    refreshPortList();
    applyConfigToUi();
    initializeDatabase();
    initializeDataMenus();
    initializeMonitoring();

    m_pollTimer->setSingleShot(true);
    connect(m_pollTimer, &QTimer::timeout, this, &MainWindow::pollNextSensor);

    m_plotRefreshTimer->setInterval(100);
    connect(m_plotRefreshTimer, &QTimer::timeout, this, [this] {
        if (!m_plotPaused && m_plot->plottableCount() > 0)
            m_plot->replot(QCustomPlot::rpQueuedReplot);
    });
    m_plotRefreshTimer->start();

    connect(m_btnConnect, &QPushButton::clicked, this, [this] {
        if (!m_collecting)
            toggleConnect();
    });
    connect(m_btnDisconnect, &QPushButton::clicked, this, &MainWindow::stopAcquisition);
    connect(m_btnSaveCfg, &QPushButton::clicked, this, &MainWindow::saveConfig);
    connect(m_modbusClient, &QModbusClient::errorOccurred, this,
            [this](QModbusDevice::Error error) {
                if (error != QModbusDevice::NoError)
                    printLog(QString("通信异常：%1").arg(m_modbusClient->errorString()), true);
            });
    connect(m_modbusClient, &QModbusDevice::stateChanged, this, [this](QModbusDevice::State state) {
        if (state == QModbusDevice::UnconnectedState && m_collecting && !m_config.simulatorEnabled)
            stopAcquisition();
        if (state == QModbusDevice::ConnectedState && m_collecting && !m_requestPending &&
            !m_pollTimer->isActive())
            scheduleNextPoll(0);
    });
}

void MainWindow::initializeDatabase()
{
    QString error;
    m_databaseAvailable = m_database.initialize(&error);
    if (!m_databaseAvailable) {
        printLog(QString("SQLite 初始化失败，采集仍可继续：%1").arg(error), true);
        return;
    }
    if (!m_database.cleanupOldData(30, &error))
        printLog(QString("历史数据自动清理失败：%1").arg(error), true);
    else
        printLog(QString("SQLite 已启用，数据文件：%1").arg(m_database.databasePath()));
}

void MainWindow::initializeDataMenus()
{
    auto *settingsMenu = menuBar()->addMenu("设置");
    auto *sensorConfigAction = settingsMenu->addAction("传感器配置");
    connect(sensorConfigAction, &QAction::triggered, this, &MainWindow::showSensorConfigDialog);

    auto *dataMenu = menuBar()->addMenu("数据");
    auto *historyAction = dataMenu->addAction("历史数据查询");
    auto *alarmAction = dataMenu->addAction("报警记录");
    historyAction->setEnabled(m_databaseAvailable);
    alarmAction->setEnabled(m_databaseAvailable);
    connect(historyAction, &QAction::triggered, this, [this] {
        auto *dialog = new HistoryDialog(m_database.connectionName(), this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });
    connect(alarmAction, &QAction::triggered, this, [this] {
        auto *dialog = new AlarmDialog(m_database.connectionName(), this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });
}

void MainWindow::showSensorConfigDialog()
{
    if (m_businessLocked) {
        QMessageBox::information(this, "配置已锁定",
                                 "请先结束任务并使未恢复报警恢复，再修改传感器映射。");
        return;
    }
    if (m_pollTimer->isActive() || m_requestPending ||
        m_modbusClient->state() != QModbusDevice::UnconnectedState) {
        QMessageBox::information(this, "传感器配置", "请先断开连接，再修改传感器配置。");
        return;
    }

    SensorConfigDialog dialog(m_config, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    dialog.applyTo(m_config);
    saveConfig();
    refreshPortList();
    if (m_monitorDialog)
        m_monitorDialog->close();
    applyConfigToUi();
    printLog("传感器配置已更新。重新建立连接后将按新配置轮询。");
}

MainWindow::~MainWindow()
{
    stopAcquisition();
    if (m_monitorThread && m_monitorThread->isRunning()) {
        QMetaObject::invokeMethod(m_monitor, &MonitorService::shutdown,
                                  Qt::BlockingQueuedConnection);
        m_monitorThread->quit();
        m_monitorThread->wait();
    }
}

void MainWindow::initUI()
{
    setWindowTitle("基于 Modbus RTU 的温湿度数据采集");
    resize(980, 820);

    auto *groupCommunication = new QGroupBox("通信参数");
    auto *communicationLayout = new QGridLayout(groupCommunication);
    communicationLayout->addWidget(new QLabel("端口号："), 0, 0);
    m_cbxPort = new QComboBox;
    communicationLayout->addWidget(m_cbxPort, 0, 1);
    communicationLayout->addWidget(new QLabel("波特率："), 0, 2);
    m_cbxBaud = new QComboBox;
    communicationLayout->addWidget(m_cbxBaud, 0, 3);
    communicationLayout->addWidget(new QLabel("校验位："), 0, 4);
    m_cbxParity = new QComboBox;
    communicationLayout->addWidget(m_cbxParity, 0, 5);
    communicationLayout->addWidget(new QLabel("数据位："), 1, 0);
    m_cbxDataBit = new QComboBox;
    communicationLayout->addWidget(m_cbxDataBit, 1, 1);
    communicationLayout->addWidget(new QLabel("停止位："), 1, 2);
    m_cbxStopBit = new QComboBox;
    communicationLayout->addWidget(m_cbxStopBit, 1, 3);
    m_btnConnect = new QPushButton("建立连接");
    m_btnDisconnect = new QPushButton("断开连接");
    m_btnSaveCfg = new QPushButton("保存参数配置");
    communicationLayout->addWidget(m_btnConnect, 1, 4);
    communicationLayout->addWidget(m_btnDisconnect, 1, 5);
    communicationLayout->addWidget(m_btnSaveCfg, 1, 6);

    auto *sensorWidget = new QWidget;
    auto *sensorLayout = new QGridLayout(sensorWidget);
    for (int i = 0; i < kSensorLimit; ++i) {
        auto *box = new QGroupBox(QString("温湿度模块%1").arg(i + 1));
        m_sensorBoxes[i] = box;
        auto *boxLayout = new QVBoxLayout(box);
        auto *humidityLayout = new QHBoxLayout;
        humidityLayout->addWidget(new QLabel("湿度值："));
        m_edtHumidity[i] = new QLineEdit("0.0");
        m_edtHumidity[i]->setReadOnly(true);
        m_edtHumidity[i]->setStyleSheet(
            "background-color:rgb(0,200,80);color:black;font-size:14px;padding:4px;");
        humidityLayout->addWidget(m_edtHumidity[i]);
        humidityLayout->addWidget(new QLabel("%"));
        boxLayout->addLayout(humidityLayout);

        auto *temperatureLayout = new QHBoxLayout;
        temperatureLayout->addWidget(new QLabel("温度值："));
        m_edtTemperature[i] = new QLineEdit("0.0");
        m_edtTemperature[i]->setReadOnly(true);
        m_edtTemperature[i]->setStyleSheet(
            "background-color:rgb(0,160,0);color:black;font-size:14px;padding:4px;");
        temperatureLayout->addWidget(m_edtTemperature[i]);
        temperatureLayout->addWidget(new QLabel("℃"));
        boxLayout->addLayout(temperatureLayout);
        sensorLayout->addWidget(box, i / 2, i % 2);
    }

    auto *groupLog = new QGroupBox("系统日志");
    auto *logLayout = new QVBoxLayout(groupLog);
    m_txtLog = new QTextEdit;
    m_txtLog->setReadOnly(true);
    logLayout->addWidget(m_txtLog);

    m_plot = new QCustomPlot;
    m_plot->xAxis->setLabel("采样序号");
    m_plot->yAxis->setLabel("温度/湿度");
    m_plot->legend->setVisible(true);
    const QColor temperatureColors[4] = {Qt::red, Qt::blue, Qt::green, Qt::magenta};
    const QColor humidityColors[4] = {Qt::darkRed, Qt::darkBlue, Qt::darkGreen, Qt::darkMagenta};
    for (int i = 0; i < kSensorLimit; ++i) {
        m_plot->addGraph();
        m_plot->graph(i * 2)->setName(QString("模块%1 温度").arg(i + 1));
        m_plot->graph(i * 2)->setPen(QPen(temperatureColors[i], 2));
        m_plot->addGraph();
        m_plot->graph(i * 2 + 1)->setName(QString("模块%1 湿度").arg(i + 1));
        m_plot->graph(i * 2 + 1)->setPen(QPen(humidityColors[i], 1, Qt::DashLine));
    }

    auto *pauseButton = new QPushButton("暂停曲线");
    auto *exportButton = new QPushButton("导出曲线图片");
    auto *plotButtonLayout = new QHBoxLayout;
    plotButtonLayout->addWidget(pauseButton);
    plotButtonLayout->addWidget(exportButton);
    connect(pauseButton, &QPushButton::clicked, this, [this, pauseButton] {
        m_plotPaused = !m_plotPaused;
        pauseButton->setText(m_plotPaused ? "恢复曲线" : "暂停曲线");
    });
    connect(exportButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, "保存曲线图片", "温湿度曲线.png",
                                                          "PNG 图片 (*.png);;JPG 图片 (*.jpg)");
        if (!path.isEmpty()) {
            const bool ok = path.endsWith(".jpg", Qt::CaseInsensitive)
                                ? m_plot->saveJpg(path, 1200, 600)
                                : m_plot->savePng(path, 1200, 600);
            printLog(ok ? QString("曲线图片已保存：%1").arg(path) : "曲线图片保存失败", !ok);
        }
    });

    auto *plotLayout = new QVBoxLayout;
    plotLayout->addLayout(plotButtonLayout);
    plotLayout->addWidget(m_plot);
    auto *groupPlot = new QGroupBox("实时温湿度曲线");
    groupPlot->setLayout(plotLayout);

    auto *mainLayout = new QVBoxLayout;
    mainLayout->addWidget(groupCommunication);
    mainLayout->addWidget(sensorWidget);
    mainLayout->addWidget(groupPlot);
    mainLayout->addWidget(groupLog);
    mainLayout->setStretchFactor(groupCommunication, 1);
    mainLayout->setStretchFactor(sensorWidget, 2);
    mainLayout->setStretchFactor(groupPlot, 4);
    mainLayout->setStretchFactor(groupLog, 3);
    mainLayout->setSpacing(12);
    mainLayout->setContentsMargins(14, 14, 14, 14);
    auto *central = new QWidget;
    central->setLayout(mainLayout);
    setCentralWidget(central);
}

void MainWindow::initSerialParameters()
{
    m_cbxBaud->addItems({"9600", "19200", "38400", "115200"});
    m_cbxParity->addItems({"无校验", "奇校验", "偶校验"});
    m_cbxDataBit->addItems({"5", "6", "7", "8"});
    m_cbxStopBit->addItems({"1", "2"});
}

QString MainWindow::configFilePath() const
{
    return QApplication::applicationDirPath() + "/config.ini";
}

void MainWindow::loadConfig()
{
    QSettings settings(configFilePath(), QSettings::IniFormat);
    m_config.port = settings.value("Serial/port", "COM1").toString();
    m_config.baud = settings.value("Serial/baud", 9600).toInt();
    m_config.dataBits = settings.value("Serial/databits", 8).toInt();
    m_config.parity = settings.value("Serial/parity", "无校验").toString();
    m_config.stopBits = settings.value("Serial/stopbits", 1).toInt();
    m_config.pollIntervalMs =
        qBound(100, settings.value("Acquisition/intervalMs", 500).toInt(), 60000);
    m_config.simulatorEnabled = settings.value("Acquisition/simulator", false).toBool();
    m_config.temperatureMin = settings.value("Alarm/tempMin", -20.0).toDouble();
    m_config.temperatureMax = settings.value("Alarm/tempMax", 60.0).toDouble();
    m_config.humidityMin = settings.value("Alarm/humMin", 10.0).toDouble();
    m_config.humidityMax = settings.value("Alarm/humMax", 90.0).toDouble();

    for (int i = 0; i < kSensorLimit; ++i) {
        const QString prefix = QString("Sensor%1/").arg(i + 1);
        auto &sensor = m_config.sensors[i];
        sensor.enabled = settings.value(prefix + "enabled", i == 0).toBool();
        sensor.name = settings.value(prefix + "name", QString("模块%1").arg(i + 1)).toString();
        sensor.slaveId = settings.value(prefix + "slaveId", i + 1).toInt();
        sensor.useInputRegisters = settings.value(prefix + "useInputReg", false).toBool();
        sensor.temperatureRegister = settings.value(prefix + "tempAddr", 0).toInt();
        sensor.humidityRegister = settings.value(prefix + "humAddr", 1).toInt();
    }
    printLog(QString("配置已加载：%1").arg(configFilePath()));
}

void MainWindow::applyConfigToUi()
{
    m_cbxPort->setCurrentText(m_config.port);
    m_cbxBaud->setCurrentText(QString::number(m_config.baud));
    m_cbxParity->setCurrentText(m_config.parity);
    m_cbxDataBit->setCurrentText(QString::number(m_config.dataBits));
    m_cbxStopBit->setCurrentText(QString::number(m_config.stopBits));
    for (int i = 0; i < kSensorLimit; ++i) {
        const bool enabled = m_config.sensors[i].enabled;
        m_plot->graph(i * 2)->setVisible(enabled);
        m_plot->graph(i * 2 + 1)->setVisible(enabled);
        if (!enabled || !m_collecting) {
            m_edtTemperature[i]->setText("--");
            m_edtHumidity[i]->setText("--");
        }
    }
}

bool MainWindow::validateConfig(QString *errorMessage) const
{
    if (m_config.temperatureMin >= m_config.temperatureMax ||
        m_config.humidityMin >= m_config.humidityMax) {
        *errorMessage = "报警下限必须小于上限。";
        return false;
    }
    int enabledCount = 0;
    QSet<int> addresses;
    for (const auto &sensor : m_config.sensors) {
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

void MainWindow::saveConfig()
{
    if (m_taskActive) {
        QMessageBox::information(this, "任务运行中", "请先结束任务再保存参数。");
        return;
    }
    m_config.port = m_cbxPort->currentData().toString();
    if (m_config.port.isEmpty())
        m_config.port = m_cbxPort->currentText();
    m_config.baud = m_cbxBaud->currentText().toInt();
    m_config.parity = m_cbxParity->currentText();
    m_config.dataBits = m_cbxDataBit->currentText().toInt();
    m_config.stopBits = m_cbxStopBit->currentText().toInt();
    QString error;
    if (!validateConfig(&error)) {
        QMessageBox::warning(this, "配置无效", error);
        return;
    }

    QSettings settings(configFilePath(), QSettings::IniFormat);
    settings.setValue("Serial/port", m_config.port);
    settings.setValue("Serial/baud", m_config.baud);
    settings.setValue("Serial/databits", m_config.dataBits);
    settings.setValue("Serial/parity", m_config.parity);
    settings.setValue("Serial/stopbits", m_config.stopBits);
    settings.setValue("Acquisition/intervalMs", m_config.pollIntervalMs);
    settings.setValue("Acquisition/simulator", m_config.simulatorEnabled);
    settings.setValue("Alarm/tempMin", m_config.temperatureMin);
    settings.setValue("Alarm/tempMax", m_config.temperatureMax);
    settings.setValue("Alarm/humMin", m_config.humidityMin);
    settings.setValue("Alarm/humMax", m_config.humidityMax);
    for (int i = 0; i < kSensorLimit; ++i) {
        const QString prefix = QString("Sensor%1/").arg(i + 1);
        const auto &sensor = m_config.sensors[i];
        settings.setValue(prefix + "enabled", sensor.enabled);
        settings.setValue(prefix + "name", sensor.name);
        settings.setValue(prefix + "slaveId", sensor.slaveId);
        settings.setValue(prefix + "useInputReg", sensor.useInputRegisters);
        settings.setValue(prefix + "tempAddr", sensor.temperatureRegister);
        settings.setValue(prefix + "humAddr", sensor.humidityRegister);
    }
    settings.sync();
    printLog("当前参数已保存；传感器配置可直接编辑 config.ini，无需修改代码。");
}

void MainWindow::refreshPortList()
{
    const QString configuredPort = m_config.port;
    m_cbxPort->clear();
    const auto ports = QSerialPortInfo::availablePorts();
    for (const auto &port : ports)
        m_cbxPort->addItem(port.portName(), port.portName());
    if (ports.isEmpty() && !m_config.simulatorEnabled)
        m_cbxPort->addItem("未检测到串口", "");
    if (m_config.simulatorEnabled)
        m_cbxPort->addItem("模拟器", "SIMULATOR");
    const int configuredIndex =
        m_cbxPort->findData(m_config.simulatorEnabled ? "SIMULATOR" : configuredPort);
    if (configuredIndex >= 0)
        m_cbxPort->setCurrentIndex(configuredIndex);
}

void MainWindow::toggleConnect()
{
    if (m_pollTimer->isActive() || m_requestPending ||
        m_modbusClient->state() != QModbusDevice::UnconnectedState) {
        stopAcquisition();
        return;
    }

    QString error;
    if (!validateConfig(&error)) {
        QMessageBox::warning(this, "配置无效", error);
        return;
    }
    if (m_config.simulatorEnabled) {
        m_collecting = true;
        ++m_acquisitionGeneration;
        if (m_monitor)
            QMetaObject::invokeMethod(m_monitor, [s = m_monitor] {
                s->session(true, true);
            });
        m_currentSensor = -1;
        printLog("模拟器模式已启动。");
        scheduleNextPoll(0);
        return;
    }

    const QString port = m_cbxPort->currentData().toString();
    if (port.isEmpty()) {
        QMessageBox::warning(this, "提示", "请选择有效串口。");
        return;
    }
    m_modbusClient->setConnectionParameter(QModbusDevice::SerialPortNameParameter, port);
    m_modbusClient->setConnectionParameter(QModbusDevice::SerialBaudRateParameter,
                                           m_cbxBaud->currentText().toInt());
    m_modbusClient->setConnectionParameter(QModbusDevice::SerialDataBitsParameter,
                                           m_cbxDataBit->currentText().toInt());
    QSerialPort::Parity parity = QSerialPort::NoParity;
    if (m_cbxParity->currentText() == "奇校验")
        parity = QSerialPort::OddParity;
    else if (m_cbxParity->currentText() == "偶校验")
        parity = QSerialPort::EvenParity;
    m_modbusClient->setConnectionParameter(QModbusDevice::SerialParityParameter, parity);
    m_modbusClient->setConnectionParameter(
        QModbusDevice::SerialStopBitsParameter,
        m_cbxStopBit->currentText() == "2" ? QSerialPort::TwoStop : QSerialPort::OneStop);
    m_modbusClient->setTimeout(qMax(300, m_config.pollIntervalMs));
    m_modbusClient->setNumberOfRetries(1);
    if (!m_modbusClient->connectDevice()) {
        QMessageBox::critical(this, "连接失败", m_modbusClient->errorString());
        refreshPortList();
        return;
    }
    m_currentSensor = -1;
    printLog(QString("串口 %1 已连接，开始轮询。").arg(port));
    m_collecting = true;
    ++m_acquisitionGeneration;
    if (m_monitor)
        QMetaObject::invokeMethod(m_monitor, [s = m_monitor] {
            s->session(true, false);
        });
    scheduleNextPoll(50);
}

int MainWindow::nextEnabledSensor(int after) const
{
    for (int step = 1; step <= kSensorLimit; ++step) {
        const int candidate = (after + step + kSensorLimit) % kSensorLimit;
        if (m_config.sensors[candidate].enabled)
            return candidate;
    }
    return -1;
}

void MainWindow::scheduleNextPoll(int delayMs)
{
    if (!m_collecting)
        return;
    if (delayMs < 0) {
        int enabled = 0;
        for (const auto &sensor : m_config.sensors)
            if (sensor.enabled)
                ++enabled;
        delayMs = qMax(20, m_config.pollIntervalMs / qMax(1, enabled));
    }
    m_pollTimer->start(delayMs);
}

void MainWindow::pollNextSensor()
{
    if (!m_collecting || m_requestPending)
        return;
    if (m_queuedSamples >= 64) {
        scheduleNextPoll(100);
        return;
    }
    if (!m_config.simulatorEnabled && m_modbusClient->state() != QModbusDevice::ConnectedState)
        return;
    m_currentSensor = nextEnabledSensor(m_currentSensor);
    if (m_currentSensor < 0)
        return;

    if (m_config.simulatorEnabled) {
        const double phase = m_sampleSequence * 0.08 + m_currentSensor;
        const double temperature =
            23.0 + 3.5 * qSin(phase) + QRandomGenerator::global()->bounded(-30, 31) / 100.0;
        const double humidity =
            52.0 + 8.0 * qSin(phase * 0.7) + QRandomGenerator::global()->bounded(-50, 51) / 100.0;
        handleSample(m_currentSensor, temperature, qBound(0.0, humidity, 100.0));
        scheduleNextPoll();
        return;
    }
    requestSensor(m_currentSensor);
}

void MainWindow::requestSensor(int index)
{
    const auto &sensor = m_config.sensors[index];
    const int firstRegister = qMin(sensor.temperatureRegister, sensor.humidityRegister);
    const int lastRegister = qMax(sensor.temperatureRegister, sensor.humidityRegister);
    const auto registerType = sensor.useInputRegisters ? QModbusDataUnit::InputRegisters
                                                       : QModbusDataUnit::HoldingRegisters;
    QModbusDataUnit unit(registerType, firstRegister, lastRegister - firstRegister + 1);
    QModbusReply *reply = m_modbusClient->sendReadRequest(unit, sensor.slaveId);
    if (!reply) {
        if (m_monitor)
            QMetaObject::invokeMethod(m_monitor, [s = m_monitor, index] {
                s->lost(index);
            });
        printLog(QString("%1（地址 %2）请求发送失败：%3")
                     .arg(sensor.name)
                     .arg(sensor.slaveId)
                     .arg(m_modbusClient->errorString()),
                 true);
        scheduleNextPoll();
        return;
    }
    m_requestPending = true;
    const auto generation = m_acquisitionGeneration;
    auto finished = [this, reply, index, firstRegister, generation] {
        if (generation != m_acquisitionGeneration || !m_collecting) {
            reply->deleteLater();
            return;
        }
        m_requestPending = false;
        const auto sensor = m_config.sensors[index];
        if (reply->error() == QModbusDevice::NoError) {
            const QModbusDataUnit result = reply->result();
            const int temperatureOffset = sensor.temperatureRegister - firstRegister;
            const int humidityOffset = sensor.humidityRegister - firstRegister;
            if (temperatureOffset < result.valueCount() && humidityOffset < result.valueCount()) {
                const quint16 rawTemperature = result.value(temperatureOffset);
                const quint16 rawHumidity = result.value(humidityOffset);
                const double temperature = SensorUtils::parseTemperature(rawTemperature);
                handleSample(index, temperature, rawHumidity * 0.1);
            } else {
                if (m_monitor)
                    QMetaObject::invokeMethod(m_monitor, [s = m_monitor, index] {
                        s->lost(index);
                    });
                printLog(QString("%1 返回的寄存器数量不足。").arg(sensor.name), true);
            }
        } else {
            if (m_monitor)
                QMetaObject::invokeMethod(m_monitor, [s = m_monitor, index] {
                    s->lost(index);
                });
            printLog(QString("%1（地址 %2）读取失败：%3")
                         .arg(sensor.name)
                         .arg(sensor.slaveId)
                         .arg(reply->errorString()),
                     true);
        }
        reply->deleteLater();
        scheduleNextPoll();
    };
    if (reply->isFinished())
        finished();
    else
        connect(reply, &QModbusReply::finished, this, finished);
}

void MainWindow::handleSample(int index, double temperature, double humidity)
{
    if (index < 0 || index >= 4 || !m_collecting)
        return;
    if (!std::isfinite(temperature) || !std::isfinite(humidity) || temperature < -100 ||
        temperature > 200 || humidity < 0 || humidity > 100) {
        if (m_monitor)
            QMetaObject::invokeMethod(m_monitor, [s = m_monitor, index] {
                s->lost(index);
            });
        printLog("无效温湿度数据，已丢弃", true);
        return;
    }
    m_edtTemperature[index]->setText(QString::number(temperature, 'f', 1));
    m_edtHumidity[index]->setText(QString::number(humidity, 'f', 1));
    updatePlot(index, temperature, humidity);
    if (!m_monitorReady)
        return;
    if (m_queuedSamples >= 64) {
        printLog("存储队列已满，本样本未保存；缺失数据不计为正常", true);
        return;
    }
    ++m_queuedSamples;
    const auto sensor = m_config.sensors[index];
    const auto mono = MonitorService::monotonicMs();
    const auto wall = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    QMetaObject::invokeMethod(
        m_monitor, [s = m_monitor, index, sensor, temperature, humidity, mono, wall] {
            s->sample(index, sensor.name, sensor.slaveId, temperature, humidity, mono, wall);
        });
}

void MainWindow::initializeMonitoring()
{
    if (!m_databaseAvailable)
        return;
    m_monitorThread = new QThread(this);
    m_monitor = new MonitorService;
    m_monitor->moveToThread(m_monitorThread);
    connect(m_monitorThread, &QThread::finished, m_monitor, &QObject::deleteLater);
    connect(m_monitor, &MonitorService::logMessage, this, &MainWindow::printLog);
    connect(m_monitor, &MonitorService::initialized, this, [this](bool ok) {
        m_monitorReady = ok;
    });
    connect(m_monitor, &MonitorService::configurationLocked, this, [this](bool locked) {
        m_businessLocked = locked;
    });
    connect(m_monitor, &MonitorService::sampleProcessed, this, [this] {
        m_queuedSamples = qMax(0, m_queuedSamples - 1);
    });
    connect(m_monitor, &MonitorService::taskChanged, this,
            [this](bool active, const QString &name) {
                m_taskActive = active;
                statusBar()->showMessage(active ? "监测任务运行中：" + name : "无运行中的监测任务");
            });
    connect(m_monitor, &MonitorService::statusReady, this,
            [this](int i, const QString &name, const QString &state) {
                const bool enabled = m_config.sensors[i].enabled;
                const QString shown = !enabled ? "未启用" : !m_collecting ? "采集已停止" : state;
                m_sensorBoxes[i]->setTitle(QString("模块%1 · %2 · %3").arg(i + 1).arg(name, shown));
                if (!enabled || !m_collecting || state.contains("失效") || state == "等待数据") {
                    m_edtTemperature[i]->setText("--");
                    m_edtHumidity[i]->setText("--");
                }
            });
    QVariantList defaults;
    for (const auto &sensor : m_config.sensors) {
        ZoneRule r;
        r.name = sensor.name;
        r.tempMin = m_config.temperatureMin;
        r.tempMax = m_config.temperatureMax;
        r.humMin = m_config.humidityMin;
        r.humMax = m_config.humidityMax;
        r.tempHysteresis = qMin(1.0, (r.tempMax - r.tempMin) / 4);
        r.humHysteresis = qMin(2.0, (r.humMax - r.humMin) / 4);
        r.staleMs = qBound(5000, m_config.pollIntervalMs * 3, 600000);
        defaults.append(MonitorService::encodeRule(r));
    }
    m_monitorThread->start();
    const auto path = m_database.databasePath();
    QMetaObject::invokeMethod(m_monitor, [s = m_monitor, path, defaults] {
        s->initialize(path, defaults);
    });
    auto *business = menuBar()->addMenu("区域监控");
    auto *action = business->addAction("区域规则 / 报警处置 / 监测任务");
    connect(action, &QAction::triggered, this, &MainWindow::showMonitoring);
}

void MainWindow::showMonitoring()
{
    if (!m_monitorReady) {
        QMessageBox::information(this, "业务模块", "后台数据库尚未就绪，请查看日志。");
        return;
    }
    if (m_monitorDialog) {
        m_monitorDialog->show();
        m_monitorDialog->raise();
        m_monitorDialog->activateWindow();
        return;
    }
    QList<int> enabled;
    for (int i = 0; i < 4; ++i)
        if (m_config.sensors[i].enabled)
            enabled.append(i);
    m_monitorDialog = new MonitorDialog(m_monitor, enabled, m_config.pollIntervalMs, this);
    m_monitorDialog->setAttribute(Qt::WA_DeleteOnClose);
    m_monitorDialog->show();
}

void MainWindow::stopAcquisition()
{
    m_collecting = false;
    ++m_acquisitionGeneration;
    m_pollTimer->stop();
    m_requestPending = false;
    if (m_modbusClient->state() != QModbusDevice::UnconnectedState)
        m_modbusClient->disconnectDevice();
    if (m_monitor && m_monitorThread->isRunning())
        QMetaObject::invokeMethod(m_monitor, [s = m_monitor, sim = m_config.simulatorEnabled] {
            s->session(false, sim);
        });
    for (int i = 0; i < 4; ++i) {
        m_edtTemperature[i]->setText("--");
        m_edtHumidity[i]->setText("--");
    }
}

void MainWindow::updatePlot(int index, double temperature, double humidity)
{
    const double key = ++m_sampleSequence;
    m_plot->graph(index * 2)->addData(key, temperature);
    m_plot->graph(index * 2 + 1)->addData(key, humidity);
    const double oldestKey = qMax(0.0, key - kMaximumPlotPoints);
    m_plot->graph(index * 2)->data()->removeBefore(oldestKey);
    m_plot->graph(index * 2 + 1)->data()->removeBefore(oldestKey);
    m_plot->xAxis->setRange(qMax(0.0, key - 200.0), key + 5.0);
    m_plot->yAxis->setRange(-30.0, 100.0);
}

QString MainWindow::logDirectoryPath() const
{
    return QApplication::applicationDirPath() + "/logs";
}

void MainWindow::rotateLogIfNeeded()
{
    QDir directory(logDirectoryPath());
    directory.mkpath(".");
    const QString activePath = directory.filePath("application.log");
    QFile active(activePath);
    if (!active.exists() || active.size() < kMaxLogBytes)
        return;
    const QString archiveName =
        QString("application-%1.log").arg(QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss"));
    active.rename(directory.filePath(archiveName));
    const QFileInfoList archives =
        directory.entryInfoList({"application-*.log"}, QDir::Files, QDir::Time);
    for (int i = kLogsToKeep - 1; i < archives.size(); ++i)
        QFile::remove(archives.at(i).absoluteFilePath());
}

void MainWindow::printLog(const QString &message, bool warning)
{
    const QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    const QString line = QString("[%1] %2").arg(timestamp, message);
    if (m_txtLog) {
        const QString escaped = line.toHtmlEscaped();
        m_txtLog->append(warning ? QString("<span style=\"color:#c62828;\">%1</span>").arg(escaped)
                                 : escaped);
        if (m_txtLog->document()->blockCount() > 1000)
            m_txtLog->document()->clear();
    }
    rotateLogIfNeeded();
    QFile file(QDir(logDirectoryPath()).filePath("application.log"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&file);
        stream << line << '\n';
    }
}
