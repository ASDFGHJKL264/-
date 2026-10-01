#include "mainwindow.h"
#include "alarmdialog.h"
#include "historydialog.h"
#include "sensorconfigdialog.h"
#include "monitoringcontroller.h"
#include "asynclogger.h"
#include <QJsonDocument>
#include "monitorservice.h"
#include "monitordialog.h"
#include <QThread>
#include <QStatusBar>
#include <QSplitter>
#include <QSignalBlocker>
#include <QAbstractItemView>
#include "serialportpolicy.h"
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
constexpr int kMaximumPlotPoints = 1200;
} // namespace

MainWindow::MainWindow(QWidget *parent, const QString &dataDirectory)
    : QMainWindow(parent), m_plotRefreshTimer(new QTimer(this)),
      m_dataDirectory(dataDirectory.isEmpty() ? QApplication::applicationDirPath() : dataDirectory)
{
    initUI();
    m_logger = new AsyncLogger(m_dataDirectory + "/logs", this);
    connect(m_logger, &AsyncLogger::error, this, [this](const QString &error) {
        m_txtLog->append(error.toHtmlEscaped());
    });
    initSerialParameters();
    loadConfig();
    refreshPortList();
    applyConfigToUi();
    initializeDataMenus();
    initializeMonitoring();

    auto *portTimer = new QTimer(this);
    portTimer->setInterval(1000);
    connect(portTimer, &QTimer::timeout, this, &MainWindow::refreshPortList);
    portTimer->start();


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
}

void MainWindow::initializeDataMenus()
{
    auto *settingsMenu = menuBar()->addMenu("设置");
    auto *sensorConfigAction = settingsMenu->addAction("传感器配置");
    connect(sensorConfigAction, &QAction::triggered, this, &MainWindow::showSensorConfigDialog);

    auto *dataMenu = menuBar()->addMenu("数据");
    auto *historyAction = dataMenu->addAction("历史数据查询");
    auto *alarmAction = dataMenu->addAction("报警记录");
    connect(historyAction, &QAction::triggered, this, [this] {
        auto *dialog = new HistoryDialog(m_dataDirectory + "/data/environment.db", this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });
    connect(alarmAction, &QAction::triggered, this, [this] {
        auto *dialog = new AlarmDialog(m_dataDirectory + "/data/environment.db", this);
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
    if (m_collecting) {
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
    m_plotRefreshTimer->stop();
    if (m_monitorDialog) delete m_monitorDialog.data();
    if (m_controller) {
        m_controller->shutdown();
        printLog("采集统计：" + QString::fromUtf8(QJsonDocument::fromVariant(m_controller->metrics()).toJson(QJsonDocument::Compact)));
        delete m_controller;
        m_controller = nullptr;
    }
}

void MainWindow::initUI()
{
    setWindowTitle("基于 Modbus RTU 的温湿度数据采集");
    resize(1100, 860);

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
    sensorLayout->setContentsMargins(0, 0, 0, 0);
    sensorLayout->setVerticalSpacing(6);
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
    m_txtLog->setMinimumHeight(45);
    m_txtLog->setReadOnly(true);
    m_txtLog->document()->setMaximumBlockCount(1000);
    logLayout->addWidget(m_txtLog);

    m_plot = new QCustomPlot;
    m_plot->setMinimumHeight(280);
    m_plot->xAxis->setLabel("采样序号");
    m_plot->yAxis->setLabel("温度/湿度");
    m_plot->legend->setVisible(true);
    // Keep legends outside the axes, with one column per module.
    m_plot->axisRect()->insetLayout()->take(m_plot->legend);
    m_plot->plotLayout()->addElement(1, 0, m_plot->legend);
    m_plot->legend->setFillOrder(QCPLayoutGrid::foRowsFirst);
    m_plot->legend->setWrap(2);
    m_plot->legend->setBorderPen(Qt::NoPen);
    m_plot->plotLayout()->setRowStretchFactor(1, 0.001);
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
    auto *plotLogSplitter = new QSplitter(Qt::Vertical);
    plotLogSplitter->setObjectName("plotLogSplitter");
    plotLogSplitter->addWidget(groupPlot);
    plotLogSplitter->addWidget(groupLog);
    plotLogSplitter->setChildrenCollapsible(false);
    plotLogSplitter->setHandleWidth(7);
    plotLogSplitter->setStretchFactor(0, 5);
    plotLogSplitter->setStretchFactor(1, 1);
    plotLogSplitter->setSizes({450, 90});
    mainLayout->addWidget(plotLogSplitter, 1);
    groupCommunication->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    sensorWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    mainLayout->setSpacing(6);
    mainLayout->setContentsMargins(10, 8, 10, 8);
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
    return m_dataDirectory + "/config.ini";
}

void MainWindow::loadConfig()
{
    m_config = ConfigStore::load(configFilePath());
    printLog("配置已加载：" + configFilePath());
}

void MainWindow::applyConfigToUi()
{
    // refreshPortList owns port selection; never replace a manual choice here.
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
    return ConfigStore::validate(m_config, errorMessage);
}

void MainWindow::saveConfig()
{
    if (m_taskActive || m_collecting) {
        QMessageBox::information(this, "任务运行中", "请先结束任务并停止采集，再保存参数。");
        return;
    }
    QStringList availablePorts;
    for (const auto &port : QSerialPortInfo::availablePorts())
        availablePorts.append(port.portName());
    m_config.port = SerialPortPolicy::savedPort(
        availablePorts, m_cbxPort->currentData().toString(), m_config.port,
        m_config.simulatorEnabled);
    m_config.baud = m_cbxBaud->currentText().toInt();
    m_config.parity = m_cbxParity->currentText();
    m_config.dataBits = m_cbxDataBit->currentText().toInt();
    m_config.stopBits = m_cbxStopBit->currentText().toInt();
    QString error;
    if (!validateConfig(&error)) {
        QMessageBox::warning(this, "配置无效", error);
        return;
    }

    if (!ConfigStore::save(configFilePath(), m_config, &error)) {
        QMessageBox::warning(this, "保存失败", error); return;
    }
    printLog("当前参数已保存。");
}

void MainWindow::refreshPortList()
{
    // Qt reports unplug errors through the existing Modbus error/state signals.
    // Never rebuild a selector while connected, collecting, or choosing an item.
    if (m_collecting ||
        m_cbxPort->view()->isVisible())
        return;

    QStringList ports;
    for (const auto &port : QSerialPortInfo::availablePorts())
        ports.append(port.portName());
    ports.removeDuplicates();
    ports.sort(Qt::CaseInsensitive);

    QStringList expected = ports;
    if (m_config.simulatorEnabled)
        expected.append("SIMULATOR");
    else if (expected.isEmpty())
        expected.append(QString());

    QStringList displayed;
    for (int i = 0; i < m_cbxPort->count(); ++i)
        displayed.append(m_cbxPort->itemData(i).toString());
    const QString selected = SerialPortPolicy::preferred(
        ports, m_cbxPort->currentData().toString(), m_config.port,
        m_config.simulatorEnabled);
    if (displayed == expected && m_cbxPort->currentData().toString() == selected)
        return;

    const QSignalBlocker blocker(m_cbxPort);
    if (displayed != expected) {
        m_cbxPort->clear();
        for (const auto &port : ports)
            m_cbxPort->addItem(port, port);
        if (m_config.simulatorEnabled)
            m_cbxPort->addItem("模拟器", "SIMULATOR");
        else if (ports.isEmpty())
            m_cbxPort->addItem("未检测到串口", "");
    }
    m_cbxPort->setCurrentIndex(m_cbxPort->findData(selected));
}

void MainWindow::toggleConnect()
{
    if (m_collecting) { stopAcquisition(); return; }
    auto config = m_config;
    config.port = m_cbxPort->currentData().toString();
    config.baud = m_cbxBaud->currentText().toInt();
    config.parity = m_cbxParity->currentText();
    config.dataBits = m_cbxDataBit->currentText().toInt();
    config.stopBits = m_cbxStopBit->currentText().toInt();
    m_controller->start(config);
}

void MainWindow::handleSample(int index, double temperature, double humidity)
{
    m_edtTemperature[index]->setText(QString::number(temperature, 'f', 1));
    m_edtHumidity[index]->setText(QString::number(humidity, 'f', 1));
    updatePlot(index, temperature, humidity);
}

void MainWindow::initializeMonitoring()
{
    m_controller = new MonitoringController(m_dataDirectory + "/data/environment.db", m_config, this);
    m_monitor = m_controller->service();
    connect(m_controller, &MonitoringController::logMessage, this, &MainWindow::printLog);
    connect(m_controller, &MonitoringController::ready, this, [this](bool ok) {
        m_monitorReady = ok; m_btnConnect->setEnabled(ok);
    });
    m_btnConnect->setEnabled(false);
    connect(m_controller, &MonitoringController::sampleReady, this, &MainWindow::handleSample);
    connect(m_controller, &MonitoringController::sessionChanged, this, [this](bool active) {
        m_collecting = active;
        for (auto *box : {m_cbxPort, m_cbxBaud, m_cbxParity, m_cbxDataBit, m_cbxStopBit})
            box->setEnabled(!active);
        m_btnSaveCfg->setEnabled(!active);
        if (!active) for (int i = 0; i < 4; ++i) {
            m_edtTemperature[i]->setText("--"); m_edtHumidity[i]->setText("--");
        }
    });
    auto *metricsTimer = new QTimer(this);
    connect(metricsTimer, &QTimer::timeout, this, [this] {
        const auto m = m_controller->metrics();
        statusBar()->showMessage(QString("读取成功 %1 / 失败 %2 · 待存储 %3/64 · 保存 %4 / 失败 %5 · 日志丢弃 %6")
            .arg(m["successfulReads"].toULongLong()).arg(m["failedReads"].toULongLong())
            .arg(m["pending"].toInt()).arg(m["saved"].toULongLong())
            .arg(m["storageFailed"].toULongLong()).arg(m_logger->dropped()));
    });
    metricsTimer->start(1000);

    connect(m_monitor, &MonitorService::configurationLocked, this, [this](bool locked) {
        m_businessLocked = locked;
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
    if (m_controller) m_controller->stop();
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



void MainWindow::printLog(const QString &message, bool warning)
{
    const QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    const QString line = QString("[%1] %2").arg(timestamp, message);
    if (m_txtLog) {
        const QString escaped = line.toHtmlEscaped();
        m_txtLog->append(warning ? QString("<span style=\"color:#c62828;\">%1</span>").arg(escaped)
                                 : escaped);
    }
    if (m_logger) m_logger->append(line);
}
