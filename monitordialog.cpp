#include "monitordialog.h"
#include "monitorservice.h"
#include <QTableWidget>
#include <QHeaderView>
#include <QLineEdit>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QFileDialog>
#include <QSaveFile>
#include <QSignalBlocker>

namespace
{
QTableWidget *table(const QStringList &headers)
{
    auto *t = new QTableWidget(0, headers.size());
    t->setHorizontalHeaderLabels(headers);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    return t;
}
QString selectedId(QTableWidget *table)
{
    const int row = table->currentRow();
    return row >= 0 && table->item(row, 0) ? table->item(row, 0)->text() : QString();
}
void restoreSelection(QTableWidget *table, const QString &id)
{
    if (id.isEmpty())
        return;
    for (int row = 0; row < table->rowCount(); ++row)
        if (table->item(row, 0)->text() == id) {
            table->selectRow(row);
            break;
        }
}
} // namespace
MonitorDialog::MonitorDialog(MonitorService *service, const QList<int> &enabled, int interval,
                             QWidget *parent)
    : QDialog(parent), m_service(service), m_enabled(enabled), m_interval(interval)
{
    setWindowTitle("实验室区域监控 / 报警处置 / 监测任务");
    resize(1160, 650);
    auto *root = new QVBoxLayout(this);
    auto *tabs = new QTabWidget;
    root->addWidget(tabs);
    auto *rulesPage = new QWidget;
    auto *ruleLayout = new QVBoxLayout(rulesPage);
    auto *ruleHint = new QLabel(
        "区域与模块一一对应。暂停监控仅暂停业务报警，不停止原始采集。修改规则须先结束任务、断开采集；未恢复报警的区域规则锁定。失效时间应大于实际轮询周期。");
    ruleHint->setWordWrap(true);
    ruleLayout->addWidget(ruleHint);
    m_rules = table({"模块", "区域名称", "监控", "温度下限", "温度上限", "湿度下限", "湿度上限",
                     "温度回差", "湿度回差", "持续超限秒", "恢复持续秒", "数据失效秒"});
    m_rules->setRowCount(4);
    ruleLayout->addWidget(m_rules);
    for (int i = 0; i < 4; ++i) {
        m_rules->setItem(i, 0, new QTableWidgetItem(QString::number(i + 1)));
        auto *name = new QLineEdit;
        name->setMaxLength(64);
        m_rules->setCellWidget(i, 1, name);
        m_rules->setCellWidget(i, 2, new QCheckBox);
        for (int c = 3; c <= 8; ++c) {
            auto *v = new QDoubleSpinBox;
            v->setDecimals(1);
            v->setRange(c <= 4 ? -100 : 0, c <= 4 ? 200 : 100);
            m_rules->setCellWidget(i, c, v);
        }
        for (int c = 9; c <= 11; ++c) {
            auto *v = new QSpinBox;
            v->setRange(c == 11 ? 1 : 0, c == 11 ? 600 : 3600);
            m_rules->setCellWidget(i, c, v);
        }
    }
    m_save = new QPushButton("保存区域规则");
    ruleLayout->addWidget(m_save);
    connect(m_save, &QPushButton::clicked, this, [this] {
        QVariantList rules;
        for (int i = 0; i < 4; ++i) {
            ZoneRule r;
            r.name = qobject_cast<QLineEdit *>(m_rules->cellWidget(i, 1))->text().trimmed();
            r.monitored = qobject_cast<QCheckBox *>(m_rules->cellWidget(i, 2))->isChecked();
            double *values[] = {&r.tempMin, &r.tempMax,        &r.humMin,
                                &r.humMax,  &r.tempHysteresis, &r.humHysteresis};
            for (int c = 3; c <= 8; ++c)
                *values[c - 3] = qobject_cast<QDoubleSpinBox *>(m_rules->cellWidget(i, c))->value();
            r.holdMs = qobject_cast<QSpinBox *>(m_rules->cellWidget(i, 9))->value() * 1000;
            r.recoveryMs = qobject_cast<QSpinBox *>(m_rules->cellWidget(i, 10))->value() * 1000;
            r.staleMs = qobject_cast<QSpinBox *>(m_rules->cellWidget(i, 11))->value() * 1000;
            if (!r.valid()) {
                QMessageBox::warning(this, "规则无效",
                                     "检查名称、上下限和回差：回差必须小于阈值区间的一半。");
                return;
            }
            rules.append(MonitorService::encodeRule(r));
        }
        QMetaObject::invokeMethod(m_service, [s = m_service, rules] {
            s->configure(rules);
        });
    });
    tabs->addTab(rulesPage, "区域规则");
    auto *alarmPage = new QWidget;
    auto *alarmLayout = new QVBoxLayout(alarmPage);
    auto *alarmHint = new QLabel(
        "未恢复报警固定置顶（翻页时仍显示）；历史每页最多100条。待确认筛选可查找旧记录，确认和恢复相互独立。新增历史请点首页或刷新。旧版报警仍在原“报警记录”窗口。");
    alarmHint->setWordWrap(true);
    alarmLayout->addWidget(alarmHint);
    auto *alarmNavigation = new QHBoxLayout;
    m_alarmFilter = new QComboBox;
    m_alarmFilter->setObjectName("alarmRecordFilter");
    m_alarmFilter->addItems({"全部报警", "未恢复报警", "待确认报警", "已完成报警"});
    auto *alarmFirst = new QPushButton("首页 / 最新");
    alarmFirst->setObjectName("alarmFirstPage");
    m_alarmPrevious = new QPushButton("上一页");
    m_alarmPrevious->setObjectName("alarmPreviousPage");
    m_alarmNext = new QPushButton("下一页");
    m_alarmNext->setObjectName("alarmNextPage");
    m_alarmPageLabel = new QLabel;
    m_alarmPageLabel->setObjectName("alarmPageLabel");
    alarmNavigation->addWidget(m_alarmFilter);
    alarmNavigation->addWidget(alarmFirst);
    alarmNavigation->addWidget(m_alarmPrevious);
    alarmNavigation->addWidget(m_alarmNext);
    alarmNavigation->addWidget(m_alarmPageLabel);
    alarmNavigation->addStretch();
    alarmLayout->addLayout(alarmNavigation);
    connect(m_alarmFilter, &QComboBox::currentIndexChanged, this, [this](int filter) {
        QMetaObject::invokeMethod(m_service, [s = m_service, filter] {
            s->browseAlarms(0, filter);
        });
    });
    for (auto *button : {alarmFirst, m_alarmPrevious, m_alarmNext}) {
        const int direction = button == alarmFirst ? 0 : button == m_alarmPrevious ? -1 : 1;
        connect(button, &QPushButton::clicked, this, [this, direction] {
            QMetaObject::invokeMethod(m_service, [s = m_service, direction] {
                s->browseAlarms(direction);
            });
        });
    }
    m_alarms = table({"ID", "区域", "类型", "方向", "发生值", "发生UTC", "确认UTC", "恢复UTC",
                      "操作人", "备注", "数据源"});
    m_alarms->setObjectName("businessAlarmTable");
    alarmLayout->addWidget(m_alarms);
    auto *ack = new QPushButton("确认选中报警并填写处理备注");
    alarmLayout->addWidget(ack);
    connect(ack, &QPushButton::clicked, this, [this] {
        int row = m_alarms->currentRow();
        if (row < 0)
            return;
        const auto id = m_alarms->item(row, 0)->text().toLongLong();
        bool ok = false;
        auto person = QInputDialog::getText(this, "报警确认", "操作人（非登录认证）：",
                                            QLineEdit::Normal, {}, &ok);
        if (!ok)
            return;
        auto note = QInputDialog::getMultiLineText(this, "报警确认", "处理备注：", {}, &ok);
        if (!ok)
            return;
        QMetaObject::invokeMethod(m_service, [s = m_service, id, person, note] {
            s->acknowledge(id, person, note);
        });
    });
    tabs->addTab(alarmPage, "报警处置");
    auto *taskPage = new QWidget;
    auto *taskLayout = new QVBoxLayout(taskPage);
    auto *controls = new QHBoxLayout;
    m_name = new QLineEdit;
    m_name->setMaxLength(100);
    m_name->setPlaceholderText("监测任务名称");
    controls->addWidget(m_name);
    for (int i = 0; i < 4; ++i) {
        m_participants[i] = new QCheckBox(QString("模块%1").arg(i + 1));
        m_participants[i]->setEnabled(enabled.contains(i));
        m_participants[i]->setChecked(enabled.contains(i));
        controls->addWidget(m_participants[i]);
    }
    m_start = new QPushButton("开始任务");
    m_finish = new QPushButton("结束任务");
    controls->addWidget(m_start);
    controls->addWidget(m_finish);
    taskLayout->addLayout(controls);
    taskLayout->addWidget(new QLabel(
        "任务期间锁定配置；掉线后任务继续，缺失时段不视为正常。导出前先结束任务。采样覆盖按配置间隔分时间槽统计。"));
    auto *taskNavigation = new QHBoxLayout;
    auto *taskFirst = new QPushButton("首页 / 最新");
    taskFirst->setObjectName("taskFirstPage");
    m_taskPrevious = new QPushButton("上一页");
    m_taskPrevious->setObjectName("taskPreviousPage");
    m_taskNext = new QPushButton("下一页");
    m_taskNext->setObjectName("taskNextPage");
    m_taskPageLabel = new QLabel;
    m_taskPageLabel->setObjectName("taskPageLabel");
    taskNavigation->addWidget(taskFirst);
    taskNavigation->addWidget(m_taskPrevious);
    taskNavigation->addWidget(m_taskNext);
    taskNavigation->addWidget(m_taskPageLabel);
    taskNavigation->addStretch();
    taskLayout->addLayout(taskNavigation);
    for (auto *button : {taskFirst, m_taskPrevious, m_taskNext}) {
        const int direction = button == taskFirst ? 0 : button == m_taskPrevious ? -1 : 1;
        connect(button, &QPushButton::clicked, this, [this, direction] {
            QMetaObject::invokeMethod(m_service, [s = m_service, direction] {
                s->browseTasks(direction);
            });
        });
    }
    m_tasks = table({"ID", "名称", "开始UTC", "结束UTC", "状态", "数据源", "间隔ms", "持续ms"});
    m_tasks->setObjectName("businessTaskTable");
    taskLayout->addWidget(m_tasks);
    auto *exportButton = new QPushButton("导出选中任务CSV报告");
    exportButton->setObjectName("exportTaskReport");
    taskLayout->addWidget(exportButton);
    connect(m_start, &QPushButton::clicked, this, [this] {
        QList<int> zones;
        for (int i = 0; i < 4; ++i)
            if (m_enabled.contains(i) && m_participants[i]->isChecked())
                zones.append(i);
        const auto name = m_name->text();
        const int interval = m_interval;
        QMetaObject::invokeMethod(m_service, [s = m_service, name, zones, interval] {
            s->startTask(name, zones, interval);
        });
    });
    connect(m_finish, &QPushButton::clicked, this, [this] {
        QMetaObject::invokeMethod(m_service, [s = m_service] {
            s->finishTask();
        });
    });
    connect(exportButton, &QPushButton::clicked, this, [this] {
        int row = m_tasks->currentRow();
        if (row < 0)
            return;
        auto id = m_tasks->item(row, 0)->text().toLongLong();
        QMetaObject::invokeMethod(m_service, [s = m_service, id] {
            s->report(id);
        });
    });
    connect(m_service, &MonitorService::reportReady, this, [this](const QString &csv) {
        auto path =
            QFileDialog::getSaveFileName(this, "导出任务报告", "监测任务报告.csv", "CSV (*.csv)");
        if (path.isEmpty())
            return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(csv.toUtf8()) < 0 || !file.commit())
            QMessageBox::warning(this, "导出失败", file.errorString());
        else
            QMessageBox::information(this, "导出完成",
                                     "任务报告已保存。超限时长为估算，缺失数据未计为正常。");
    });
    tabs->addTab(taskPage, "任务与报告");
    m_hint = new QLabel("正在加载…");
    m_hint->setWordWrap(true);
    root->addWidget(m_hint);
    m_hint->setTextFormat(Qt::PlainText);
    auto *reload = new QPushButton("刷新报警 / 任务");
    root->addWidget(reload);
    connect(reload, &QPushButton::clicked, this, &MonitorDialog::refresh);
    connect(m_service, &MonitorService::viewReady, this, &MonitorDialog::updateView);
    connect(m_service, &MonitorService::logMessage, this,
            [this](const QString &text, bool warning) {
                m_hint->setText(text);
                m_hint->setStyleSheet(warning ? "color:#c62828" : "");
            });
    refresh();
}
void MonitorDialog::refresh()
{
    QMetaObject::invokeMethod(m_service, [s = m_service] {
        s->reloadRecords();
    });
}
void MonitorDialog::updateView(const QVariantMap &v)
{
    const bool active = v["taskActive"].toBool();
    m_start->setEnabled(!active && v["acquiring"].toBool());
    m_finish->setEnabled(active);
    m_save->setEnabled(!active && !v["acquiring"].toBool());
    if (!m_loaded) {
        auto rules = v["rules"].toList();
        for (int i = 0; i < qMin(4, int(rules.size())); ++i) {
            auto r = MonitorService::decodeRule(rules[i].toMap());
            qobject_cast<QLineEdit *>(m_rules->cellWidget(i, 1))->setText(r.name);
            qobject_cast<QCheckBox *>(m_rules->cellWidget(i, 2))->setChecked(r.monitored);
            double values[] = {r.tempMin, r.tempMax,        r.humMin,
                               r.humMax,  r.tempHysteresis, r.humHysteresis};
            for (int c = 3; c <= 8; ++c)
                qobject_cast<QDoubleSpinBox *>(m_rules->cellWidget(i, c))->setValue(values[c - 3]);
            int seconds[] = {r.holdMs / 1000, r.recoveryMs / 1000, r.staleMs / 1000};
            for (int c = 9; c <= 11; ++c)
                qobject_cast<QSpinBox *>(m_rules->cellWidget(i, c))->setValue(seconds[c - 9]);
        }
        m_loaded = true;
    }
    auto alarms = v["alarms"].toList();
    const auto selectedAlarm = selectedId(m_alarms);
    m_alarms->setRowCount(0);
    for (const auto &item : alarms) {
        auto r = item.toMap();
        QStringList fields = {r["id"].toString(),
                              r["name"].toString(),
                              r["metric"].toInt() ? "湿度" : "温度",
                              r["direction"].toInt() < 0 ? "低于下限" : "高于上限",
                              r["value"].toString(),
                              r["raised_at"].toString(),
                              r["ack_at"].isNull() ? "未确认" : r["ack_at"].toString(),
                              r["recovered_at"].isNull() ? "未恢复" : r["recovered_at"].toString(),
                              r["operator"].toString(),
                              r["note"].toString(),
                              r["simulator"].toBool() ? "模拟" : "硬件"};
        int row = m_alarms->rowCount();
        m_alarms->insertRow(row);
        for (int c = 0; c < fields.size(); ++c)
            m_alarms->setItem(row, c, new QTableWidgetItem(fields[c]));
        if (r["recovered_at"].isNull())
            for (int c = 0; c < fields.size(); ++c)
                m_alarms->item(row, c)->setBackground(QColor(255, 238, 224));
    }
    restoreSelection(m_alarms, selectedAlarm);
    const auto alarmPage = v["alarmPage"].toMap();
    const QSignalBlocker filterBlocker(m_alarmFilter);
    m_alarmFilter->setCurrentIndex(alarmPage.value("filter", 0).toInt());
    m_alarmPrevious->setEnabled(alarmPage["hasPrevious"].toBool());
    m_alarmNext->setEnabled(alarmPage["hasMore"].toBool());
    m_alarmPageLabel->setText(QString("第%1页 · 置顶%2条 · 历史%3条")
                                .arg(alarmPage.value("number", 1).toInt())
                                .arg(alarmPage["pinned"].toInt())
                                .arg(alarms.size() - alarmPage["pinned"].toInt()));
    const auto selectedTask = selectedId(m_tasks);
    m_tasks->setRowCount(0);
    for (const auto &item : v["tasks"].toList()) {
        auto r = item.toMap();
        QStringList fields = {
            r["id"].toString(),          r["name"].toString(),
            r["started_at"].toString(),  r["ended_at"].toString(),
            r["status"].toString(),      r["simulator"].toBool() ? "模拟" : "硬件",
            r["interval_ms"].toString(), r["elapsed_ms"].toString()};
        int row = m_tasks->rowCount();
        m_tasks->insertRow(row);
        for (int c = 0; c < fields.size(); ++c)
            m_tasks->setItem(row, c, new QTableWidgetItem(fields[c]));
    }
    restoreSelection(m_tasks, selectedTask);
    const auto taskPage = v["taskPage"].toMap();
    m_taskPrevious->setEnabled(taskPage["hasPrevious"].toBool());
    m_taskNext->setEnabled(taskPage["hasMore"].toBool());
    m_taskPageLabel->setText(QString("第%1页 · %2条任务（每页最多100条）")
                               .arg(taskPage.value("number", 1).toInt())
                               .arg(m_tasks->rowCount()));
    m_hint->setText(active ? "当前有运行中的监测任务" : "业务数据已刷新");
}
