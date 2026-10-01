#include "historydialog.h"
#include "historyquery.h"

#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

HistoryDialog::HistoryDialog(const QString &databasePath, QWidget *parent)
    : QDialog(parent), m_query(new HistoryQuery(databasePath, this))
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
    m_queryButton = new QPushButton("查询");
    auto *queryButton = m_queryButton;
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
    connect(queryButton, &QPushButton::clicked, this, [this] { m_offset = 0; queryData(); });
    auto *pages = new QHBoxLayout;
    m_previous = new QPushButton("上一页");
    m_next = new QPushButton("下一页");
    m_pageLabel = new QLabel;
    pages->addWidget(m_previous); pages->addWidget(m_pageLabel); pages->addWidget(m_next);
    layout->addLayout(pages);
    const auto resetPage = [this] {
        m_offset = 0;
        m_previous->setEnabled(false); m_next->setEnabled(false);
        m_pageLabel->setText("查询条件已变化，请重新查询");
    };
    connect(m_startEdit, &QDateTimeEdit::dateTimeChanged, this, resetPage);
    connect(m_endEdit, &QDateTimeEdit::dateTimeChanged, this, resetPage);
    connect(m_sensorCombo, &QComboBox::currentIndexChanged, this, resetPage);
    connect(m_previous, &QPushButton::clicked, this, [this] {
        m_offset = qMax(0, m_offset - HistoryQuery::PageSize); queryData();
    });
    connect(m_next, &QPushButton::clicked, this, [this] {
        m_offset += HistoryQuery::PageSize; queryData();
    });
    connect(m_query, &HistoryQuery::finished, this,
            [this](int operation, const QVariantList &rows, bool more, const QString &error) {
        m_queryButton->setEnabled(true);
        m_startEdit->setEnabled(true); m_endEdit->setEnabled(true); m_sensorCombo->setEnabled(true);
        m_previous->setEnabled(m_offset > 0); m_next->setEnabled(more);
        if (!error.isEmpty()) {
            m_pageLabel->setText("查询失败");
            QMessageBox::warning(this, "数据库操作失败", error); return;
        }
        if (operation == HistoryQuery::ClearAlarms) { m_offset = 0; queryData(); return; }
        showRows(rows);
        m_pageLabel->setText(QString("第 %1 页 · 本页 %2 条（每页最多500条）")
            .arg(m_offset / HistoryQuery::PageSize + 1).arg(rows.size()));
    });
    queryData();
}

void HistoryDialog::queryData()
{
    if (m_startEdit->dateTime() > m_endEdit->dateTime()) {
        QMessageBox::warning(this, "查询条件", "开始时间不能晚于结束时间"); return;
    }
    m_requestedSensor = m_sensorCombo->currentData().toInt();
    if (!m_query->run(HistoryQuery::Samples,
                     m_startEdit->dateTime().toString(Qt::ISODateWithMs),
                     m_endEdit->dateTime().toString(Qt::ISODateWithMs), m_requestedSensor, m_offset)) return;
    m_queryButton->setEnabled(false); m_previous->setEnabled(false); m_next->setEnabled(false);
    m_startEdit->setEnabled(false); m_endEdit->setEnabled(false); m_sensorCombo->setEnabled(false);
    m_pageLabel->setText("正在查询…");
}

void HistoryDialog::showRows(const QVariantList &rows)
{
    const int sensorIndex = m_requestedSensor;
    m_table->setRowCount(0);
    QVector<double> timeValues[4];
    QVector<double> temperatures[4];
    QVector<double> humidities[4];
    for (const auto &entry : rows) {
        const auto fields = entry.toList();
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const QDateTime time = QDateTime::fromString(fields.at(0).toString(), Qt::ISODateWithMs);
        m_table->setItem(row, 0, new QTableWidgetItem(time.toString("yyyy-MM-dd HH:mm:ss")));
        for (int column = 1; column < 5; ++column)
            m_table->setItem(row, column, new QTableWidgetItem(fields.at(column).toString()));
        const int rowSensorIndex = fields.at(5).toInt();
        if (rowSensorIndex < 0 || rowSensorIndex >= 4)
            continue;
        timeValues[rowSensorIndex].append(time.toMSecsSinceEpoch() / 1000.0);
        temperatures[rowSensorIndex].append(fields.at(3).toDouble());
        humidities[rowSensorIndex].append(fields.at(4).toDouble());
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
