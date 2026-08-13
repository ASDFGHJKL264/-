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
    m_plot->addGraph();
    m_plot->graph(0)->setName("温度");
    m_plot->graph(0)->setPen(QPen(Qt::red, 2));
    m_plot->addGraph();
    m_plot->graph(1)->setName("湿度");
    m_plot->graph(1)->setPen(QPen(Qt::blue, 2));
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
    QString sql = "SELECT sample_time,sensor_name,slave_id,temperature,humidity "
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
    QVector<double> timeValues, temperatures, humidities;
    while (query.next()) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const QDateTime time = QDateTime::fromString(query.value(0).toString(), Qt::ISODateWithMs);
        m_table->setItem(row, 0, new QTableWidgetItem(time.toString("yyyy-MM-dd HH:mm:ss")));
        for (int column = 1; column < 5; ++column)
            m_table->setItem(row, column, new QTableWidgetItem(query.value(column).toString()));
        timeValues.append(time.toMSecsSinceEpoch() / 1000.0);
        temperatures.append(query.value(3).toDouble());
        humidities.append(query.value(4).toDouble());
    }
    m_plot->graph(0)->setData(timeValues, temperatures);
    m_plot->graph(1)->setData(timeValues, humidities);
    if (!timeValues.isEmpty())
        m_plot->rescaleAxes();
    m_plot->replot();
}
