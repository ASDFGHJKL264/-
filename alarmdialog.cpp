#include "alarmdialog.h"

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

AlarmDialog::AlarmDialog(const QString &connectionName, QWidget *parent)
    : QDialog(parent), m_connectionName(connectionName)
{
    setWindowTitle("报警记录");
    resize(850, 520);
    auto *filters = new QHBoxLayout;
    m_startEdit = new QDateTimeEdit(QDateTime::currentDateTime().addDays(-7));
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
    auto *clearButton = new QPushButton("清空全部记录");
    filters->addWidget(new QLabel("开始："));
    filters->addWidget(m_startEdit);
    filters->addWidget(new QLabel("结束："));
    filters->addWidget(m_endEdit);
    filters->addWidget(m_sensorCombo);
    filters->addWidget(queryButton);
    filters->addWidget(clearButton);

    m_table = new QTableWidget;
    m_table->setColumnCount(8);
    m_table->setHorizontalHeaderLabels(
        {"时间", "模块", "地址", "类型", "数值", "下限", "上限", "状态"});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(filters);
    layout->addWidget(m_table);
    connect(queryButton, &QPushButton::clicked, this, &AlarmDialog::queryData);
    connect(clearButton, &QPushButton::clicked, this, &AlarmDialog::clearData);
    queryData();
}

void AlarmDialog::queryData()
{
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    QString sql = "SELECT alarm_time,sensor_name,slave_id,alarm_type,alarm_value,"
                  "minimum_value,maximum_value FROM alarm_history "
                  "WHERE alarm_time>=? AND alarm_time<=?";
    const int sensorIndex = m_sensorCombo->currentData().toInt();
    if (sensorIndex >= 0)
        sql += " AND sensor_index=?";
    sql += " ORDER BY alarm_time DESC LIMIT 10000";
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
    while (query.next()) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const QDateTime time = QDateTime::fromString(query.value(0).toString(), Qt::ISODateWithMs);
        m_table->setItem(row, 0, new QTableWidgetItem(time.toString("yyyy-MM-dd HH:mm:ss")));
        for (int column = 1; column < 7; ++column)
            m_table->setItem(row, column, new QTableWidgetItem(query.value(column).toString()));
        const double value = query.value(4).toDouble();
        const QString state = value < query.value(5).toDouble() ? "低于下限" : "高于上限";
        m_table->setItem(row, 7, new QTableWidgetItem(state));
        for (int column = 0; column < 8; ++column)
            m_table->item(row, column)->setForeground(QColor("#c62828"));
    }
}

void AlarmDialog::clearData()
{
    if (QMessageBox::question(this, "确认清空",
                              "确定清空全部报警记录吗？此操作无法撤销。")
        != QMessageBox::Yes)
        return;
    QSqlQuery query(QSqlDatabase::database(m_connectionName));
    if (!query.exec("DELETE FROM alarm_history")) {
        QMessageBox::critical(this, "清空失败", query.lastError().text());
        return;
    }
    queryData();
}
