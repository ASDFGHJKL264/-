#include "mainwindow.h"
#include "monitoringcontroller.h"
#include "historydialog.h"
#include "alarmdialog.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QPushButton>
#include <QSqlQuery>
#include <QFontDatabase>

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
                HistoryDialog history(dir.filePath("data/environment.db"));
                AlarmDialog alarms(dir.filePath("data/environment.db"));
                history.show(); alarms.show(); QTest::qWait(100);
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
