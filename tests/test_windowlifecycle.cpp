#include "mainwindow.h"
#include "monitoringcontroller.h"
#include "historydialog.h"
#include "monitordialog.h"
#include <QMenuBar>
#include <QAction>
#include <QtTest>
#include <QTemporaryDir>
#include <QPushButton>
#include <QSqlQuery>
#include <QFontDatabase>
#include <QMessageBox>
#include <QDir>
#include <QGroupBox>
#include "sensorconfigdialog.h"
#include <limits>

class WindowLifecycleTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
#ifdef Q_OS_WIN
        const int id = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
        const auto names = QFontDatabase::applicationFontFamilies(id);
        if (!names.isEmpty()) QApplication::setFont(QFont(names.first(), 10));
#endif
    }
    void overviewLayoutUsesTwoEqualColumns() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MainWindow window(nullptr, dir.path());
        window.show();
        auto *controls = window.findChild<QWidget *>("monitorControls");
        auto *overview = window.findChild<QWidget *>("monitorOverview");
        QVERIFY(controls);
        QVERIFY(overview);
        auto *logGroup = qobject_cast<QGroupBox *>(window.m_txtLog->parentWidget());
        QVERIFY(logGroup);
        for (const QSize size : {QSize(1100, 860), QSize(1600, 1000)}) {
            window.resize(size);
            QTest::qWait(100);
            QVERIFY(qAbs(controls->width() - logGroup->width()) <= 2);
            QCOMPARE(controls->geometry().top(), logGroup->geometry().top());
            QCOMPARE(controls->height(), logGroup->height());
            QVERIFY(window.m_plot->mapTo(&window, QPoint()).y() >=
                    overview->mapTo(&window, QPoint()).y() + overview->height());
            for (auto *button : {window.m_btnConnect, window.m_btnDisconnect, window.m_btnSaveCfg})
                QVERIFY(button->width() >= button->minimumSizeHint().width());
        }
        QVERIFY(window.grab().save("main-layout-two-columns.png"));
    }
    void configurationCommitIsTransactional() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto original = ConfigStore::load(dir.filePath("config.ini"));
        original.port = "COM8";
        original.simulatorEnabled = true;
        QString error;
        QVERIFY(ConfigStore::save(dir.filePath("config.ini"), original, &error));
        MainWindow window(nullptr, dir.path());
        auto candidate = original;
        candidate.pollIntervalMs = 900;
        candidate.sensors[0].name = "已保存区域";
        QVERIFY(window.commitConfig(candidate));
        QCOMPARE(window.m_config.pollIntervalMs, 900);
        QCOMPARE(ConfigStore::load(dir.filePath("config.ini")).sensors[0].name, candidate.sensors[0].name);
        QCOMPARE(window.m_config.port, QString("COM8"));

        // A directory at the INI path fails even when tests run as administrator.
        QVERIFY(QDir(dir.path()).mkpath("blocked/config.ini"));
        window.m_dataDirectory = dir.filePath("blocked");
        auto rejected = candidate;
        rejected.pollIntervalMs = 1400;
        rejected.sensors[0].name = "不应生效";
        QTimer::singleShot(0, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->accept();
        });
        QVERIFY(!window.commitConfig(rejected));
        QCOMPARE(window.m_config.pollIntervalMs, 900);
        QCOMPARE(window.m_config.sensors[0].name, candidate.sensors[0].name);
        QCOMPARE(ConfigStore::load(dir.filePath("config.ini")).pollIntervalMs, 900);
        QVERIFY(!window.m_txtLog->toPlainText().contains("传感器配置已更新"));

        // Exercise the real modal dialog path, not only the helper.
        bool edited = false, sawFailure = false;
        QTimer dismiss;
        dismiss.setInterval(5);
        connect(&dismiss, &QTimer::timeout, &window, [&] {
            if (auto *dialog = qobject_cast<SensorConfigDialog *>(QApplication::activeModalWidget())) {
                if (!edited) {
                    const auto names = dialog->findChildren<QLineEdit *>();
                    if (!names.isEmpty()) names.first()->setText("对话框失败修改");
                    edited = true;
                    dialog->accept();
                }
            } else if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                sawFailure = box->windowTitle() == "保存失败";
                box->accept();
            }
        });
        dismiss.start();
        window.showSensorConfigDialog();
        dismiss.stop();
        QVERIFY(edited && sawFailure);
        QCOMPARE(window.m_config.sensors[0].name, candidate.sensors[0].name);
        QVERIFY(!window.m_txtLog->toPlainText().contains("传感器配置已更新"));
    }
    void ordinarySaveFailureDoesNotChangeMemory() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MainWindow window(nullptr, dir.path());
        const int originalBaud = window.m_config.baud;
        window.m_cbxBaud->setCurrentText("19200");
        QVERIFY(QDir(dir.path()).mkpath("config.ini"));
        QTimer::singleShot(0, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->accept();
        });
        QVERIFY(!window.saveConfig());
        QCOMPARE(window.m_config.baud, originalBaud);
        QVERIFY(!window.m_txtLog->toPlainText().contains("当前参数已保存"));
    }
    void realtimeRangeIncludesVisiblePhysicalExtremes() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MainWindow window(nullptr, dir.path());
        for (auto &sensor : window.m_config.sensors) sensor.enabled = true;
        window.applyConfigToUi();
        QCOMPARE(window.m_plot->graphCount(), 8);
        window.updatePlot(0, -100, 0);
        window.updatePlot(1, 200, 100);
        QVERIFY(window.m_plot->yAxis->range().lower < -100);
        QVERIFY(window.m_plot->yAxis->range().upper > 200);
        window.m_config.sensors[1].enabled = false;
        window.applyConfigToUi();
        QVERIFY(window.m_plot->yAxis->range().upper < 200);
        QVERIFY(window.m_plot->yAxis->range().lower < -100);
        window.m_plotPaused = true;
        window.updatePlot(0, 120, 50);
        QVERIFY(window.m_plot->yAxis->range().upper > 120);
        const auto sequence = window.m_sampleSequence;
        window.updatePlot(-1, 25, 50);
        window.updatePlot(0, std::numeric_limits<double>::quiet_NaN(), 50);
        window.updatePlot(0, 201, 50);
        QCOMPARE(window.m_sampleSequence, sequence);
        for (int i = 0; i < 1500; ++i) window.updatePlot(0, 25, 50);
        QCOMPARE(window.m_plot->graph(0)->data()->size(), 1200);
        QCOMPARE(window.m_plot->graph(1)->data()->size(), 1200);
        QVERIFY(window.m_plot->yAxis->range().lower > -100);
        QVERIFY(window.m_plot->yAxis->range().upper < 120);
        window.m_plot->replot();
        QVERIFY(!window.m_plot->toPixmap(900, 450).isNull());
    }
    void collectQueryAndClose() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        AppConfig config = ConfigStore::load(dir.filePath("config.ini"));
        config.simulatorEnabled = true; config.pollIntervalMs = 100;
        for (int i = 0; i < 4; ++i) config.sensors[i].enabled = true;
        QString error; QVERIFY(ConfigStore::save(dir.filePath("config.ini"), config, &error));
        const int cycles = qBound(3, qEnvironmentVariableIntValue("SENSOR_LIFECYCLE_CYCLES"), 1000);
        qint64 maxClose = 0;
        quint64 totalSaved = 0;
        for (int i = 0; i < cycles; ++i) {
            auto window = std::make_unique<MainWindow>(nullptr, dir.path());
            window->show();
            auto *controller = window->findChild<MonitoringController *>(); QVERIFY(controller);
            QPushButton *start = nullptr;
            for (auto *button : window->findChildren<QPushButton *>())
                if (button->text() == "建立连接") start = button;
            QVERIFY(start); QTRY_VERIFY(start->isEnabled());
            QTest::mouseClick(start, Qt::LeftButton);
            QTRY_VERIFY(controller->metrics()["saved"].toULongLong() >= 4);
            if (i == 0) {
                QTRY_VERIFY(controller->metrics()["saved"].toULongLong() >= 12);
                QVERIFY(window->grab().save("main-quality.png"));
                QAction *historyAction = nullptr;
                for (auto *action : window->menuBar()->actions()) {
                    QVERIFY(action->text() != "数据");
                    QVERIFY(action->text() != "报警记录");
                    if (action->objectName() == "openHistoryQuery") historyAction = action;
                }
                QVERIFY(historyAction && !historyAction->menu());
                QCOMPARE(historyAction->text(), QString("历史查询"));
                historyAction->trigger();
                auto *history = window->findChild<HistoryDialog *>();
                QVERIFY(history && history->isVisible());
                MonitorDialog alarms(controller->service(), {0, 1, 2, 3}, 100);
                alarms.show(); QTest::qWait(100);
                history->close();
            }
            QElapsedTimer elapsed; elapsed.start();
            controller->shutdown();
            totalSaved += controller->metrics()["saved"].toULongLong();
            window.reset();
            maxClose = qMax(maxClose, elapsed.elapsed());
        }
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "verify-windows");
            db.setDatabaseName(dir.filePath("data/environment.db")); QVERIFY(db.open());
            QSqlQuery q(db); QVERIFY(q.exec("SELECT COUNT(*) FROM sensor_history"));
            QVERIFY(q.next()); QCOMPARE(q.value(0).toULongLong(), totalSaved);
        }
        QSqlDatabase::removeDatabase("verify-windows");
        qInfo() << "cycles" << cycles << "saved" << totalSaved << "max close ms" << maxClose;
    }
};
QTEST_MAIN(WindowLifecycleTests)
#include "test_windowlifecycle.moc"
