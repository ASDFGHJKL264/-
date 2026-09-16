#include "monitordialog.h"
#include "monitorservice.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QSqlQuery>
#include <QTabWidget>
#include <QTableWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QFontDatabase>

class MonitorUiTest : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
#ifdef Q_OS_WIN
        const int id = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
        const auto families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty())
            QApplication::setFont(QFont(families.first(), 10));
#endif
    }
    void workflowAndRender()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("ui.db");
        auto db = QSqlDatabase::addDatabase("QSQLITE", "ui-reader");
        db.setDatabaseName(path);
        QVERIFY(db.open());
        {
            QSqlQuery q(db);
            QVERIFY(q.exec(
                "CREATE TABLE sensor_history(sample_time TEXT,sensor_index INTEGER,sensor_name TEXT,slave_id INTEGER,temperature REAL,humidity REAL)"));
            QVERIFY(q.exec(
                "CREATE TABLE alarm_history(alarm_time TEXT,sensor_index INTEGER,sensor_name TEXT,slave_id INTEGER,alarm_type TEXT,alarm_value REAL,minimum_value REAL,maximum_value REAL)"));
        }
        QVariantList defaults;
        for (int i = 0; i < 4; ++i) {
            ZoneRule r;
            r.name = QString("实验区域%1").arg(i + 1);
            r.holdMs = 0;
            defaults.append(MonitorService::encodeRule(r));
        }
        MonitorService service;
        service.initialize(path, defaults);
        service.session(true, true);
        {
            MonitorDialog dialog(&service, {0, 1, 2, 3}, 500);
            dialog.show();
            QTest::qWait(100);
            auto *tabs = dialog.findChild<QTabWidget *>();
            QVERIFY(tabs);
            QCOMPARE(tabs->count(), 3);
            QVERIFY(dialog.grab().save("business-rules.png"));
            tabs->setCurrentIndex(2);
            auto edits = tabs->widget(2)->findChildren<QLineEdit *>();
            QCOMPARE(edits.size(), 1);
            edits.first()->setText("四区域演示任务");
            QPushButton *start = nullptr, *finish = nullptr;
            for (auto *b : dialog.findChildren<QPushButton *>()) {
                if (b->text() == "开始任务")
                    start = b;
                if (b->text() == "结束任务")
                    finish = b;
            }
            QVERIFY(start && finish);
            QVERIFY(start->isEnabled());
            QTest::mouseClick(start, Qt::LeftButton);
            QTRY_VERIFY(finish->isEnabled());
            service.sample(0, "模块1", 1, 80, 50, MonitorService::monotonicMs(),
                           "2026-09-16T10:00:00.000");
            tabs->setCurrentIndex(1);
            service.refresh();
            QTest::qWait(50);
            auto *alarmTable = tabs->widget(1)->findChild<QTableWidget *>();
            QCOMPARE(alarmTable->rowCount(), 1);
            QVERIFY(dialog.grab().save("business-alarms.png"));
            tabs->setCurrentIndex(2);
            QTest::mouseClick(finish, Qt::LeftButton);
            QTRY_VERIFY(!finish->isEnabled());
            QVERIFY(dialog.grab().save("business-tasks.png"));
        }
        service.shutdown();
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase("ui-reader");
    }
};
QTEST_MAIN(MonitorUiTest)
#include "test_monitorui.moc"
