#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "qcustomplot.h"
#include "appconfig.h"

#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>

class MonitorService;
class MonitoringController;
class AsyncLogger;
class QGroupBox;
class MonitorDialog;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

  public:
    explicit MainWindow(QWidget *parent = nullptr, const QString &dataDirectory = {});
    ~MainWindow() override;

  private:
    void initUI();
    void initSerialParameters();
    void refreshPortList();
    void loadConfig();
    void saveConfig();
    void applyConfigToUi();
    void toggleConnect();
    void handleSample(int index, double temperature, double humidity);
    void updatePlot(int index, double temperature, double humidity);
    void printLog(const QString &message, bool warning = false);
    void initializeDataMenus();
    void showSensorConfigDialog();
    void initializeMonitoring();
    void showMonitoring();
    void stopAcquisition();
    QString configFilePath() const;
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

    QTimer *m_plotRefreshTimer = nullptr;
    AppConfig m_config;
    QString m_dataDirectory;
    MonitoringController *m_controller = nullptr;
    AsyncLogger *m_logger = nullptr;
    bool m_plotPaused = false;
    qint64 m_sampleSequence = 0;
    MonitorService *m_monitor = nullptr;
    QGroupBox *m_sensorBoxes[4]{};
    QPointer<MonitorDialog> m_monitorDialog;
    bool m_monitorReady = false, m_taskActive = false, m_collecting = false;
    bool m_businessLocked = false;
};

#endif
