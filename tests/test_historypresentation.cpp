#include "historydialog.h"

#include <QComboBox>
#include <QDateTime>
#include <QFontDatabase>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QUuid>
#include <QtTest>
#include <cmath>

namespace {
struct Reading {
    QDateTime time;
    int sensor = 0;
    double temperature = 20.580706299410057;
    double humidity = 46.7104590689921;
};

bool seedDatabase(const QString &path, const QList<Reading> &readings)
{
    const QString connection = "history-presentation-" + QUuid::createUuid().toString();
    bool ok = false;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery query(db);
            ok = query.exec("CREATE TABLE sensor_history(sample_time TEXT,sensor_index INTEGER,"
                            "sensor_name TEXT,slave_id INTEGER,temperature REAL,humidity REAL)");
            ok = ok && query.prepare("INSERT INTO sensor_history VALUES(?,?,?,?,?,?)");
            for (const auto &reading : readings) {
                if (!ok)
                    break;
                query.bindValue(0, reading.time.toString(Qt::ISODateWithMs));
                query.bindValue(1, reading.sensor);
                query.bindValue(2, QString("温湿度模块%1").arg(reading.sensor + 1));
                query.bindValue(3, reading.sensor + 1);
                query.bindValue(4, reading.temperature);
                query.bindValue(5, reading.humidity);
                ok = query.exec();
            }
            if (!ok)
                qWarning() << query.lastError();
            db.close();
        } else {
            qWarning() << db.lastError();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

QDateTime sampleBase()
{
    // Milliseconds are deliberate: two samples in one second must remain distinguishable.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    return QDateTime::fromMSecsSinceEpoch((now / 1000) * 1000 - 900000 + 123);
}

QPushButton *queryButton(HistoryDialog &dialog)
{
    for (auto *button : dialog.findChildren<QPushButton *>())
        if (button->text() == "查询")
            return button;
    return nullptr;
}

int gapCount(const QCPGraph *graph)
{
    int gaps = 0;
    for (auto it = graph->data()->constBegin(); it != graph->data()->constEnd(); ++it)
        if (std::isnan(it->value))
            ++gaps;
    return gaps;
}
}

class HistoryPresentationTests : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
#ifdef Q_OS_WIN
        const int id = QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
        const auto names = QFontDatabase::applicationFontFamilies(id);
        if (!names.isEmpty())
            QApplication::setFont(QFont(names.first(), 10));
#endif
    }

    void gapsLabelsPrecisionAndLegend()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto base = sampleBase();
        QList<Reading> readings;
        for (qint64 offset : {0, 500, 1000, 600000, 600500, 601000})
            readings.append({base.addMSecs(offset)});
        QVERIFY(seedDatabase(directory.filePath("history.db"), readings));

        HistoryDialog dialog(directory.filePath("history.db"));
        dialog.resize(1100, 780);
        dialog.show();
        auto *table = dialog.findChild<QTableWidget *>();
        auto *plot = dialog.findChild<QCustomPlot *>();
        auto *query = queryButton(dialog);
        QVERIFY(table && plot && query);
        QTRY_VERIFY(query->isEnabled());
        QCOMPARE(table->rowCount(), 6);

        // The table still contains only real samples; NaNs exist solely in the line data.
        QCOMPARE(table->item(0, 0)->text(), base.toString("yyyy-MM-dd HH:mm:ss.zzz"));
        QVERIFY(table->item(0, 0)->text() != table->item(1, 0)->text());
        QCOMPARE(table->item(0, 3)->text(), QString("20.58"));
        QCOMPARE(table->item(0, 4)->text(), QString("46.71"));
        QCOMPARE(plot->graph(0)->data()->size(), 7);
        QCOMPARE(plot->graph(1)->data()->size(), 7);
        QCOMPARE(gapCount(plot->graph(0)), 1);
        QCOMPARE(gapCount(plot->graph(1)), 1);
        const auto first = plot->graph(0)->data()->constBegin();
        QCOMPARE(first->value, readings.first().temperature);
        QVERIFY(plot->xAxis->range().lower < first->key);
        QVERIFY(plot->yAxis->range().lower < readings.first().temperature);
        QVERIFY(plot->yAxis->range().upper > readings.first().humidity);
        const auto breakPoint = first + 3;
        QVERIFY(std::isnan(breakPoint->value));
        QVERIFY(breakPoint->key > base.addMSecs(1000).toMSecsSinceEpoch() / 1000.0);
        QVERIFY(breakPoint->key < base.addMSecs(600000).toMSecsSinceEpoch() / 1000.0);

        // Only module 1 has data, although the combo is set to all modules.
        QCOMPARE(plot->legend->itemCount(), 2);
        QVERIFY(plot->legend->itemWithPlottable(plot->graph(0)));
        QVERIFY(plot->legend->itemWithPlottable(plot->graph(1)));
        QVERIFY(!plot->legend->itemWithPlottable(plot->graph(2)));
        QCOMPARE(plot->plotLayout()->element(1, 0), static_cast<QCPLayoutElement *>(plot->legend));
        QVERIFY(plot->legend->outerRect().top() >= plot->axisRect()->rect().bottom());

        const auto ticker = qSharedPointerDynamicCast<QCPAxisTickerDateTime>(plot->xAxis->ticker());
        QVERIFY(ticker);
        QVERIFY(ticker->timeZone().isValid());
        QVector<double> ticks, subTicks;
        QVector<QString> labels;
        ticker->generate(plot->xAxis->range(), QLocale(), 'g', 6, ticks, &subTicks, &labels);
        QVERIFY(!labels.isEmpty());
        QCOMPARE(ticks.size(), labels.size());
        for (qsizetype i = 0; i < labels.size(); ++i) {
            QVERIFY(!labels[i].isEmpty());
            const auto local = QDateTime::fromMSecsSinceEpoch(qint64(ticks[i] * 1000.0));
            QCOMPARE(labels[i], QLocale().toString(local, ticker->dateTimeFormat()));
        }
        QVERIFY(dialog.grab().save("history-presentation-fixed.png"));

        auto *sensor = dialog.findChild<QComboBox *>();
        QVERIFY(sensor);
        sensor->setCurrentIndex(sensor->findData(1)); // Module 2 has no samples.
        QTest::mouseClick(query, Qt::LeftButton);
        QTRY_VERIFY(query->isEnabled());
        QCOMPARE(table->rowCount(), 0);
        QCOMPARE(plot->legend->itemCount(), 0);
        QVERIFY(!plot->legend->visible());
        for (int i = 0; i < plot->graphCount(); ++i) {
            QVERIFY(plot->graph(i)->data()->isEmpty());
            QVERIFY(!plot->graph(i)->visible());
        }
    }

    void slowCadenceAndMultipleModules()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto base = sampleBase();
        QList<Reading> readings;
        for (int i = 0; i < 5; ++i) {
            readings.append({base.addSecs(i * 60), 0, 25.0 + i, 50.0 + i});
            readings.append({base.addSecs(i * 60 + 1), 2, 15.0 + i, 70.0 + i});
        }
        QVERIFY(seedDatabase(directory.filePath("history.db"), readings));
        HistoryDialog dialog(directory.filePath("history.db"));
        dialog.show();
        auto *plot = dialog.findChild<QCustomPlot *>();
        auto *table = dialog.findChild<QTableWidget *>();
        auto *query = queryButton(dialog);
        QVERIFY(plot && table && query);
        QTRY_VERIFY(query->isEnabled());
        QCOMPARE(table->rowCount(), 10);
        QCOMPARE(plot->legend->itemCount(), 4);
        for (int i : {0, 1, 4, 5}) {
            QCOMPARE(plot->graph(i)->data()->size(), 5);
            QCOMPARE(gapCount(plot->graph(i)), 0);
            QVERIFY(plot->graph(i)->visible());
        }
        for (int i : {2, 3, 6, 7})
            QVERIFY(!plot->graph(i)->visible());
    }

    void sparseSamplesDoNotCreateAContinuousLine()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto base = sampleBase();
        QVERIFY(seedDatabase(directory.filePath("history.db"),
                             {{base, 0, 25.0, 50.0}, {base.addSecs(600), 0, 25.0, 50.0}}));
        HistoryDialog dialog(directory.filePath("history.db"));
        dialog.show();
        auto *plot = dialog.findChild<QCustomPlot *>();
        auto *query = queryButton(dialog);
        QVERIFY(plot && query);
        QTRY_VERIFY(query->isEnabled());
        QCOMPARE(plot->graph(0)->data()->size(), 3);
        QCOMPARE(gapCount(plot->graph(0)), 1);
        QCOMPARE(gapCount(plot->graph(1)), 1);
        QVERIFY(plot->graph(0)->scatterStyle().shape() != QCPScatterStyle::ssNone);
    }
};

QTEST_MAIN(HistoryPresentationTests)
#include "test_historypresentation.moc"
