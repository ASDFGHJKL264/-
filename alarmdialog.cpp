#include "alarmdialog.h"
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

AlarmDialog::AlarmDialog(const QString &databasePath, QWidget *parent)
    : QDialog(parent), m_query(new HistoryQuery(databasePath, this))
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
    m_queryButton = new QPushButton("查询");
    auto *queryButton = m_queryButton;
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
    connect(queryButton, &QPushButton::clicked, this, [this] { m_offset = 0; queryData(); });
    connect(clearButton, &QPushButton::clicked, this, &AlarmDialog::clearData);
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

void AlarmDialog::queryData()
{
    if (m_startEdit->dateTime() > m_endEdit->dateTime()) {
        QMessageBox::warning(this, "查询条件", "开始时间不能晚于结束时间"); return;
    }
    m_requestedSensor = m_sensorCombo->currentData().toInt();
    if (!m_query->run(HistoryQuery::Alarms,
                     m_startEdit->dateTime().toString(Qt::ISODateWithMs),
                     m_endEdit->dateTime().toString(Qt::ISODateWithMs), m_requestedSensor, m_offset)) return;
    m_queryButton->setEnabled(false); m_previous->setEnabled(false); m_next->setEnabled(false);
    m_startEdit->setEnabled(false); m_endEdit->setEnabled(false); m_sensorCombo->setEnabled(false);
    m_pageLabel->setText("正在查询…");
}

void AlarmDialog::showRows(const QVariantList &rows)
{
    const int sensorIndex = m_requestedSensor;
    m_table->setRowCount(0);
    for (const auto &entry : rows) {
        const auto fields = entry.toList();
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const QDateTime time = QDateTime::fromString(fields.at(0).toString(), Qt::ISODateWithMs);
        m_table->setItem(row, 0, new QTableWidgetItem(time.toString("yyyy-MM-dd HH:mm:ss")));
        for (int column = 1; column < 7; ++column)
            m_table->setItem(row, column, new QTableWidgetItem(fields.at(column).toString()));
        const double value = fields.at(4).toDouble();
        const QString state = value < fields.at(5).toDouble() ? "低于下限" : "高于上限";
        m_table->setItem(row, 7, new QTableWidgetItem(state));
        for (int column = 0; column < 8; ++column)
            m_table->item(row, column)->setForeground(QColor(198, 40, 40));
    }
}

void AlarmDialog::clearData()
{
    if (!m_queryButton->isEnabled()) {
        QMessageBox::information(this, "操作进行中", "请等待当前查询或清空操作完成。");
        return;
    }
    if (QMessageBox::question(this, "确认清空",
                              "确定清空全部报警记录吗？此操作无法撤销。")
        != QMessageBox::Yes)
        return;
    if (m_query->run(HistoryQuery::ClearAlarms)) {
        m_queryButton->setEnabled(false); m_previous->setEnabled(false); m_next->setEnabled(false);
        m_pageLabel->setText("正在清空…");
    }
}
