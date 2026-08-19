#include "historydialog.h"

#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QVBoxLayout>

HistoryDialog::HistoryDialog(const QString &connectionName, QWidget *parent)
    : QDialog(parent), m_connectionName(connectionName)
{
    setWindowTitle("温湿度历史查询");
    resize(900, 650);
    auto *filterLayout = new QHBoxLayout;
    m_startEdit = new QDateTimeEdit(QDateTime::currentDateTime().addDays(-1));
    m_endEdit = new QDateTimeEdit(QDateTime::currentDateTime());
    m_startEdit->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
    m_endEdit->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
    m_startEdit->setCalendarPopup(true);
    m_endEdit->setCalendarPopup(true);
    m_sensorCombo = new QComboBox;
    m_sensorCombo->addItem("全部模块", -1);
    for (int i = 0; i < 4; ++i)
        m_sensorCombo->addItem(QString("模块%1").arg(i + 1), i);
    auto *queryButton = new QPushButton("查询");
    filterLayout->addWidget(new QLabel("开始："));
    filterLayout->addWidget(m_startEdit);
    filterLayout->addWidget(new QLabel("结束："));
    filterLayout->addWidget(m_endEdit);
    filterLayout->addWidget(new QLabel("模块："));
    filterLayout->addWidget(m_sensorCombo);
    filterLayout->addWidget(queryButton);

    m_plot = new QCustomPlot;
    const QColor temperatureColors[4] = {
        QColor(211, 47, 47), QColor(245, 124, 0),
        QColor(123, 31, 162), QColor(0, 137, 123)
    };
    const QColor humidityColors[4] = {
        QColor(25, 118, 210), QColor(0, 151, 167),
        QColor(57, 73, 171), QColor(46, 125, 50)
    };
    for (int i = 0; i < 4; ++i) {
        m_plot->addGraph();
        m_plot->graph(i * 2)->setName(QString("模块%1 温度").arg(i + 1));
        m_plot->graph(i * 2)->setPen(QPen(temperatureColors[i], 2));
        m_plot->addGraph();
        m_plot->graph(i * 2 + 1)->setName(QString("模块%1 湿度").arg(i + 1));
        m_plot->graph(i * 2 + 1)->setPen(QPen(humidityColors[i], 2, Qt::DashLine));
    }
    m_plot->legend->setVisible(true);
    m_plot->xAxis->setLabel("时间");
    m_plot->yAxis->setLabel("温度/湿度");
    m_plot->xAxis->setTicker(QSharedPointer<QCPAxisTickerDateTime>(new QCPAxisTickerDateTime));
    qSharedPointerCast<QCPAxisTickerDateTime>(m_plot->xAxis->ticker())
        ->setDateTimeFormat("MM-dd\nHH:mm");

    m_table = new QTableWidget;
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({"时间", "模块", "从机地址", "温度(℃)", "湿度(%)"});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(filterLayout);
    layout->addWidget(m_plot, 3);
    layout->addWidget(m_table, 2);
    connect(queryButton, &QPushButton::clicked, this, &HistoryDialog::queryData);
    queryData();
}

void HistoryDialog::queryData()
{
    if (m_startEdit->dateTime() > m_endEdit->dateTime()) {
        QMessageBox::warning(this, "查询条件", "开始时间不能晚于结束时间。");
        return;
    }
    QSqlDatabase database = QSqlDatabase::database(m_connectionName);
    QSqlQuery query(database);
    QString sql = "SELECT sample_time,sensor_name,slave_id,temperature,humidity,sensor_index "
                  "FROM sensor_history WHERE sample_time>=? AND sample_time<=?";
    const int sensorIndex = m_sensorCombo->currentData().toInt();
    if (sensorIndex >= 0)
        sql += " AND sensor_index=?";
    sql += " ORDER BY sample_time ASC LIMIT 10000";
    query.prepare(sql);
    query.addBindValue(m_startEdit->dateTime().toString(Qt::ISODateWithMs));
    query.addBindValue(m_endEdit->dateTime().toString(Qt::ISODateWithMs));
    if (sensorIndex >= 0)
        query.addBindValue(sensorIndex);
    if (!query.exec()) {
        QMessageBox::critical(this, "查询失败", query.lastError().text());
        return;
    }
    m_table->setRowCount(0);
    QVector<double> timeValues[4];
    QVector<double> temperatures[4];
    QVector<double> humidities[4];
    while (query.next()) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const QDateTime time = QDateTime::fromString(query.value(0).toString(), Qt::ISODateWithMs);
        m_table->setItem(row, 0, new QTableWidgetItem(time.toString("yyyy-MM-dd HH:mm:ss")));
        for (int column = 1; column < 5; ++column)
            m_table->setItem(row, column, new QTableWidgetItem(query.value(column).toString()));
        const int rowSensorIndex = query.value(5).toInt();
        if (rowSensorIndex < 0 || rowSensorIndex >= 4)
            continue;
        timeValues[rowSensorIndex].append(time.toMSecsSinceEpoch() / 1000.0);
        temperatures[rowSensorIndex].append(query.value(3).toDouble());
        humidities[rowSensorIndex].append(query.value(4).toDouble());
    }

    bool hasAnyData = false;
    for (int i = 0; i < 4; ++i) {
        auto *temperatureGraph = m_plot->graph(i * 2);
        auto *humidityGraph = m_plot->graph(i * 2 + 1);
        temperatureGraph->setData(timeValues[i], temperatures[i]);
        humidityGraph->setData(timeValues[i], humidities[i]);
        const bool selected = sensorIndex < 0 || sensorIndex == i;
        const bool hasData = !timeValues[i].isEmpty();
        temperatureGraph->setVisible(selected && hasData);
        humidityGraph->setVisible(selected && hasData);
        temperatureGraph->removeFromLegend();
        humidityGraph->removeFromLegend();
        if (selected) {
            temperatureGraph->addToLegend();
            humidityGraph->addToLegend();
        }
        if (selected && hasData)
            hasAnyData = true;
    }
    if (hasAnyData)
        m_plot->rescaleAxes();
    m_plot->replot();
}
