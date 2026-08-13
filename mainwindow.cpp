#include "mainwindow.h"
#include "alarmdialog.h"
#include "historydialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
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
#include <QSpinBox>
#include <QTextStream>
#include <QtMath>
#include <QVBoxLayout>

namespace {
constexpr int kSensorLimit = 4;
constexpr qint64 kMaxLogBytes = 5 * 1024 * 1024;
constexpr int kLogsToKeep = 10;
constexpr int kMaximumPlotPoints = 1200;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      m_modbusClient(new QModbusRtuSerialClient(this)),
      m_pollTimer(new QTimer(this)),
      m_plotRefreshTimer(new QTimer(this))
{
    initUI();
    initSerialParameters();
    loadConfig();
    refreshPortList();
    applyConfigToUi();
    initializeDatabase();
    initializeDataMenus();

    m_pollTimer->setSingleShot(true);
    connect(m_pollTimer, &QTimer::timeout, this, &MainWindow::pollNextSensor);

    m_plotRefreshTimer->setInterval(100);
    connect(m_plotRefreshTimer, &QTimer::timeout, this, [this] {
        if (!m_plotPaused && m_plot->plottableCount() > 0)
            m_plot->replot(QCustomPlot::rpQueuedReplot);
    });
    m_plotRefreshTimer->start();

    connect(m_btnConnect, &QPushButton::clicked, this, &MainWindow::toggleConnect);
    connect(m_btnDisconnect, &QPushButton::clicked, this, &MainWindow::toggleConnect);
    connect(m_btnSaveCfg, &QPushButton::clicked, this, &MainWindow::saveConfig);
    connect(m_modbusClient, &QModbusClient::errorOccurred, this,
            [this](QModbusDevice::Error error) {
        if (error != QModbusDevice::NoError)
            printLog(QString("通信异常：%1").arg(m_modbusClient->errorString()), true);
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
    connect(sensorConfigAction, &QAction::triggered,
            this, &MainWindow::showSensorConfigDialog);

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
    if (m_pollTimer->isActive() || m_requestPending ||
        m_modbusClient->state() != QModbusDevice::UnconnectedState) {
        QMessageBox::information(this, "传感器配置",
                                 "请先断开连接，再修改传感器配置。");
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("传感器配置");
    dialog.resize(820, 300);
    auto *layout = new QVBoxLayout(&dialog);
    auto *grid = new QGridLayout;
    const QStringList headers = {
        "模块", "启用", "名称", "站地址", "寄存器类型", "温度地址", "湿度地址"
    };
    for (int column = 0; column < headers.size(); ++column) {
        auto *label = new QLabel(headers.at(column));
        label->setAlignment(Qt::AlignCenter);
        grid->addWidget(label, 0, column);
    }

    QCheckBox *enabledEdits[kSensorLimit]{};
    QLineEdit *nameEdits[kSensorLimit]{};
    QSpinBox *slaveEdits[kSensorLimit]{};
    QComboBox *typeEdits[kSensorLimit]{};
    QSpinBox *temperatureEdits[kSensorLimit]{};
    QSpinBox *humidityEdits[kSensorLimit]{};
    for (int i = 0; i < kSensorLimit; ++i) {
        const auto &sensor = m_config.sensors[i];
        grid->addWidget(new QLabel(QString("模块%1").arg(i + 1)), i + 1, 0);
        enabledEdits[i] = new QCheckBox;
        enabledEdits[i]->setChecked(sensor.enabled);
        grid->addWidget(enabledEdits[i], i + 1, 1, Qt::AlignCenter);
        nameEdits[i] = new QLineEdit(sensor.name);
        grid->addWidget(nameEdits[i], i + 1, 2);
        slaveEdits[i] = new QSpinBox;
        slaveEdits[i]->setRange(1, 247);
        slaveEdits[i]->setValue(sensor.slaveId);
        grid->addWidget(slaveEdits[i], i + 1, 3);
        typeEdits[i] = new QComboBox;
        typeEdits[i]->addItem("保持寄存器（功能码03）", false);
        typeEdits[i]->addItem("输入寄存器（功能码04）", true);
        typeEdits[i]->setCurrentIndex(sensor.useInputRegisters ? 1 : 0);
        grid->addWidget(typeEdits[i], i + 1, 4);
        temperatureEdits[i] = new QSpinBox;
        temperatureEdits[i]->setRange(0, 65535);
        temperatureEdits[i]->setValue(sensor.temperatureRegister);
        grid->addWidget(temperatureEdits[i], i + 1, 5);
        humidityEdits[i] = new QSpinBox;
        humidityEdits[i]->setRange(0, 65535);
        humidityEdits[i]->setValue(sensor.humidityRegister);
        grid->addWidget(humidityEdits[i], i + 1, 6);
    }
    grid->setColumnStretch(2, 1);
    grid->setColumnStretch(4, 1);
    layout->addLayout(grid);
    auto *hint = new QLabel(
        "说明：每个启用模块必须使用不同站地址。保存后立即生效，下次启动自动加载。"
        "未连接的模块建议不要启用。");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText("保存");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        AppConfig candidate = m_config;
        for (int i = 0; i < kSensorLimit; ++i) {
            auto &sensor = candidate.sensors[i];
            sensor.enabled = enabledEdits[i]->isChecked();
            sensor.name = nameEdits[i]->text().trimmed();
            if (sensor.name.isEmpty())
                sensor.name = QString("温湿度模块%1").arg(i + 1);
            sensor.slaveId = slaveEdits[i]->value();
            sensor.useInputRegisters = typeEdits[i]->currentData().toBool();
            sensor.temperatureRegister = temperatureEdits[i]->value();
            sensor.humidityRegister = humidityEdits[i]->value();
        }
        const AppConfig original = m_config;
        m_config = candidate;
        QString error;
        if (!validateConfig(&error)) {
            m_config = original;
            QMessageBox::warning(&dialog, "配置无效", error);
            return;
        }
        saveConfig();
        applyConfigToUi();
        printLog("传感器配置已更新。重新建立连接后将按新配置轮询。");
        dialog.accept();
    });
    dialog.exec();
}

MainWindow::~MainWindow()
{
    m_pollTimer->stop();
    if (m_modbusClient->state() != QModbusDevice::UnconnectedState)
        m_modbusClient->disconnectDevice();
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
        const QString path = QFileDialog::getSaveFileName(
            this, "保存曲线图片", "温湿度曲线.png", "PNG 图片 (*.png);;JPG 图片 (*.jpg)");
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
    m_config.pollIntervalMs = qBound(100, settings.value("Acquisition/intervalMs", 500).toInt(), 60000);
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
        if (!enabled) {
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
    const int configuredIndex = m_cbxPort->findData(
        m_config.simulatorEnabled ? "SIMULATOR" : configuredPort);
    if (configuredIndex >= 0)
        m_cbxPort->setCurrentIndex(configuredIndex);
}

void MainWindow::toggleConnect()
{
    if (m_pollTimer->isActive() || m_requestPending ||
        m_modbusClient->state() != QModbusDevice::UnconnectedState) {
        m_pollTimer->stop();
        m_requestPending = false;
        if (m_modbusClient->state() != QModbusDevice::UnconnectedState)
            m_modbusClient->disconnectDevice();
        printLog("采集已停止，串口已断开。");
        return;
    }

    QString error;
    if (!validateConfig(&error)) {
        QMessageBox::warning(this, "配置无效", error);
        return;
    }
    if (m_config.simulatorEnabled) {
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
    m_modbusClient->setConnectionParameter(QModbusDevice::SerialStopBitsParameter,
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
    if (delayMs < 0)
        delayMs = qMax(20, m_config.pollIntervalMs / qMax(1, kSensorLimit));
    m_pollTimer->start(delayMs);
}

void MainWindow::pollNextSensor()
{
    if (m_requestPending)
        return;
    if (!m_config.simulatorEnabled &&
        m_modbusClient->state() != QModbusDevice::ConnectedState)
        return;
    m_currentSensor = nextEnabledSensor(m_currentSensor);
    if (m_currentSensor < 0)
        return;

    if (m_config.simulatorEnabled) {
        const double phase = m_sampleSequence * 0.08 + m_currentSensor;
        const double temperature = 23.0 + 3.5 * qSin(phase) +
            QRandomGenerator::global()->bounded(-30, 31) / 100.0;
        const double humidity = 52.0 + 8.0 * qSin(phase * 0.7) +
            QRandomGenerator::global()->bounded(-50, 51) / 100.0;
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
    const auto registerType = sensor.useInputRegisters
                                  ? QModbusDataUnit::InputRegisters
                                  : QModbusDataUnit::HoldingRegisters;
    QModbusDataUnit unit(registerType, firstRegister, lastRegister - firstRegister + 1);
    QModbusReply *reply = m_modbusClient->sendReadRequest(unit, sensor.slaveId);
    if (!reply) {
        printLog(QString("%1（地址 %2）请求发送失败：%3")
                     .arg(sensor.name).arg(sensor.slaveId).arg(m_modbusClient->errorString()), true);
        scheduleNextPoll();
        return;
    }
    m_requestPending = true;
    connect(reply, &QModbusReply::finished, this, [this, reply, index, firstRegister] {
        m_requestPending = false;
        const auto sensor = m_config.sensors[index];
        if (reply->error() == QModbusDevice::NoError) {
            const QModbusDataUnit result = reply->result();
            const int temperatureOffset = sensor.temperatureRegister - firstRegister;
            const int humidityOffset = sensor.humidityRegister - firstRegister;
            if (temperatureOffset < result.valueCount() && humidityOffset < result.valueCount()) {
                const quint16 rawTemperature = result.value(temperatureOffset);
                const quint16 rawHumidity = result.value(humidityOffset);
                const double temperature = rawTemperature >= 10000
                    ? -(rawTemperature - 10000) * 0.1 : rawTemperature * 0.1;
                handleSample(index, temperature, rawHumidity * 0.1);
            } else {
                printLog(QString("%1 返回的寄存器数量不足。").arg(sensor.name), true);
            }
        } else {
            printLog(QString("%1（地址 %2）读取失败：%3")
                         .arg(sensor.name).arg(sensor.slaveId).arg(reply->errorString()), true);
        }
        reply->deleteLater();
        scheduleNextPoll();
    });
}

void MainWindow::handleSample(int index, double temperature, double humidity)
{
    m_edtTemperature[index]->setText(QString::number(temperature, 'f', 1));
    m_edtHumidity[index]->setText(QString::number(humidity, 'f', 1));
    updatePlot(index, temperature, humidity);

    const QDateTime now = QDateTime::currentDateTime();
    if (m_databaseAvailable) {
        QString databaseError;
        const auto &sensor = m_config.sensors[index];
        if (!m_database.insertSample(now, index, sensor.name, sensor.slaveId,
                                     temperature, humidity, &databaseError)) {
            printLog(QString("历史数据写入失败：%1").arg(databaseError), true);
        }
    }
    const bool temperatureAlarm =
        temperature < m_config.temperatureMin || temperature > m_config.temperatureMax;
    const bool humidityAlarm =
        humidity < m_config.humidityMin || humidity > m_config.humidityMax;
    if (temperatureAlarm &&
        (!m_lastAlarm[index][0].isValid() || m_lastAlarm[index][0].secsTo(now) >= 60)) {
        printLog(QString("【报警】%1 温度 %2℃ 超出阈值。")
                     .arg(m_config.sensors[index].name).arg(temperature, 0, 'f', 1), true);
        if (m_databaseAvailable) {
            QString databaseError;
            const auto &sensor = m_config.sensors[index];
            if (!m_database.insertAlarm(now, index, sensor.name, sensor.slaveId, "温度",
                                        temperature, m_config.temperatureMin,
                                        m_config.temperatureMax, &databaseError))
                printLog(QString("报警记录写入失败：%1").arg(databaseError), true);
        }
        m_lastAlarm[index][0] = now;
    }
    if (humidityAlarm &&
        (!m_lastAlarm[index][1].isValid() || m_lastAlarm[index][1].secsTo(now) >= 60)) {
        printLog(QString("【报警】%1 湿度 %2% 超出阈值。")
                     .arg(m_config.sensors[index].name).arg(humidity, 0, 'f', 1), true);
        if (m_databaseAvailable) {
            QString databaseError;
            const auto &sensor = m_config.sensors[index];
            if (!m_database.insertAlarm(now, index, sensor.name, sensor.slaveId, "湿度",
                                        humidity, m_config.humidityMin,
                                        m_config.humidityMax, &databaseError))
                printLog(QString("报警记录写入失败：%1").arg(databaseError), true);
        }
        m_lastAlarm[index][1] = now;
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
    const QFileInfoList archives = directory.entryInfoList(
        {"application-*.log"}, QDir::Files, QDir::Time);
    for (int i = kLogsToKeep - 1; i < archives.size(); ++i)
        QFile::remove(archives.at(i).absoluteFilePath());
}

void MainWindow::printLog(const QString &message, bool warning)
{
    const QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    const QString line = QString("[%1] %2").arg(timestamp, message);
    if (m_txtLog) {
        const QString escaped = line.toHtmlEscaped();
        m_txtLog->append(warning
            ? QString("<span style=\"color:#c62828;\">%1</span>").arg(escaped)
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
