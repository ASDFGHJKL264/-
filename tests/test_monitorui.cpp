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
#include <QComboBox>
#include <QLabel>
#include <QJsonDocument>
#include "databasemanager.h"

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
    void historicalPagingFilteringAndOldReport()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("ui-history.db");
        {
            DatabaseManager schema(path);
            QString error;
            QVERIFY2(schema.initialize(&error), qPrintable(error));
        }
        QVariantList defaults;
        for (int i = 0; i < 4; ++i) {
            ZoneRule rule;
            rule.name = QString("区域%1").arg(i + 1);
            defaults.append(MonitorService::encodeRule(rule));
        }
        MonitorService service;
        service.initialize(path, defaults);
        auto db = QSqlDatabase::addDatabase("QSQLITE", "ui-history-inspector");
        db.setDatabaseName(path);
        QVERIFY(db.open());
        {
            QSqlQuery q(db);
            QVERIFY(db.transaction());
            QVERIFY(q.prepare("INSERT INTO monitor_alarms(id,zone,name,metric,direction,value,minimum,maximum,raised_at,ack_at,recovered_at,simulator) VALUES(?,0,'old zone',0,1,80,-20,60,'raised',?,?,0)"));
            for (int id = 1; id <= 552; ++id) {
                q.bindValue(0, id);
                q.bindValue(1, id <= 2 ? QVariant() : QVariant("ack"));
                q.bindValue(2, id == 1 ? QVariant() : QVariant("recovered"));
                QVERIFY(q.exec());
            }
            QVERIFY(q.prepare("INSERT INTO monitor_tasks(id,name,started_at,ended_at,status,simulator,interval_ms,elapsed_ms) VALUES(?,?,'start','end','completed',0,500,1000)"));
            for (int id = 1; id <= 205; ++id) {
                q.bindValue(0, id);
                q.bindValue(1, QString("old-task-%1").arg(id));
                QVERIFY(q.exec());
            }
            QVERIFY(q.prepare("INSERT INTO monitor_task_zones(task_id,zone,config) VALUES(1,0,?)"));
            q.addBindValue(QString::fromUtf8(QJsonDocument::fromVariant(defaults.first()).toJson(
                QJsonDocument::Compact)));
            QVERIFY(q.exec());
            QVERIFY(db.commit());
        }
        {
            MonitorDialog dialog(&service, {0}, 500);
            dialog.show();
            auto *tabs = dialog.findChild<QTabWidget *>();
            auto *alarms = dialog.findChild<QTableWidget *>("businessAlarmTable");
            auto *tasks = dialog.findChild<QTableWidget *>("businessTaskTable");
            auto *alarmNext = dialog.findChild<QPushButton *>("alarmNextPage");
            auto *alarmPrevious = dialog.findChild<QPushButton *>("alarmPreviousPage");
            auto *filter = dialog.findChild<QComboBox *>("alarmRecordFilter");
            auto *taskNext = dialog.findChild<QPushButton *>("taskNextPage");
            auto *taskPrevious = dialog.findChild<QPushButton *>("taskPreviousPage");
            auto *taskPage = dialog.findChild<QLabel *>("taskPageLabel");
            QVERIFY(tabs && alarms && tasks && alarmNext && alarmPrevious && filter && taskNext &&
                    taskPrevious && taskPage);
            QTRY_COMPARE(alarms->rowCount(), 101);
            QCOMPARE(alarms->item(0, 0)->text(), QString("1"));
            QVERIFY(!alarmPrevious->isEnabled());
            tabs->setCurrentIndex(1);
            for (int page = 0; page < 5; ++page) {
                QVERIFY(alarmNext->isEnabled());
                QTest::mouseClick(alarmNext, Qt::LeftButton);
                QTRY_VERIFY(dialog.findChild<QLabel *>("alarmPageLabel")->text().startsWith(
                    QString("第%1页").arg(page + 2)));
                QCOMPARE(alarms->item(0, 0)->text(), QString("1"));
                QVERIFY(alarms->rowCount() <= 108);
            }
            QVERIFY(!alarmNext->isEnabled());
            QCOMPARE(alarms->item(alarms->rowCount() - 1, 0)->text(), QString("2"));
            filter->setCurrentIndex(2);
            QTRY_COMPARE(alarms->rowCount(), 2);
            alarms->selectRow(1);
            service.refresh();
            QCOMPARE(alarms->item(alarms->currentRow(), 0)->text(), QString("2"));
            service.acknowledge(2, "operator", "old incident reviewed");
            QTRY_COMPARE(alarms->rowCount(), 1);
            QCOMPARE(alarms->item(0, 0)->text(), QString("1"));

            tabs->setCurrentIndex(2);
            QTRY_COMPARE(tasks->rowCount(), 100);
            QTest::mouseClick(taskNext, Qt::LeftButton);
            QTRY_VERIFY(taskPage->text().startsWith("第2页"));
            QTest::mouseClick(taskNext, Qt::LeftButton);
            QTRY_COMPARE(tasks->rowCount(), 5);
            QVERIFY(!taskNext->isEnabled());
            QVERIFY(taskPrevious->isEnabled());
            tasks->selectRow(4);
            QCOMPARE(tasks->item(tasks->currentRow(), 0)->text(), QString("1"));
            service.refresh();
            QCOMPARE(tasks->item(tasks->currentRow(), 0)->text(), QString("1"));
            // Test the actual export click without opening a file dialog or writing user files.
            QObject::disconnect(&service, SIGNAL(reportReady(QString)), &dialog, nullptr);
            QSignalSpy reports(&service, &MonitorService::reportReady);
            QTest::mouseClick(dialog.findChild<QPushButton *>("exportTaskReport"), Qt::LeftButton);
            QTRY_COMPARE(reports.size(), 1);
            QVERIFY(reports.last()[0].toString().contains("old-task-1"));
            service.session(true, false);
            service.startTask("new running task", {0}, 500);
            QTRY_VERIFY(taskPage->text().startsWith("第1页"));
            QCOMPARE(tasks->item(0, 0)->text(), QString("206"));
            QCOMPARE(tasks->item(0, 4)->text(), QString("running"));
            service.finishTask();
            QTRY_COMPARE(tasks->item(0, 4)->text(), QString("completed"));
        }
        service.shutdown();
        db.close();
        db = {};
        QSqlDatabase::removeDatabase("ui-history-inspector");
    }
};
QTEST_MAIN(MonitorUiTest)
#include "test_monitorui.moc"
