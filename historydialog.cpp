#include "historydialog.h"
#include "historyquery.h"

#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QFontMetricsF>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTimeZone>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>

namespace {
class HistoryTimeTicker final : public QCPAxisTickerDateTime
{
public:
    explicit HistoryTimeTicker(QCPAxis *axis) : m_axis(axis)
    {
        setTimeZone(QTimeZone::systemTimeZone());
    }

    void generate(const QCPRange &range, const QLocale &locale, QChar formatChar, int precision,
                  QVector<double> &ticks, QVector<double> *subTicks,
                  QVector<QString> *tickLabels) override
    {
        // Subsecond ticks need milliseconds; long histories need the year as well.
        setDateTimeFormat(range.size() < 10 ? "MM-dd\nHH:mm:ss.zzz"
                          : range.size() < 2 * 86400 ? "MM-dd\nHH:mm:ss"
                          : range.size() < 90 * 86400 ? "yyyy-MM-dd\nHH:mm"
                                                                   : "yyyy-MM-dd");
        if (!m_axis) {
            QCPAxisTickerDateTime::generate(range, locale, formatChar, precision, ticks,
                                            subTicks, tickLabels);
            return;
        }

        const QFontMetricsF metrics(m_axis->tickLabelFont());
        const auto axisMargins = m_axis->axisRect()->margins();
        const auto layoutMargins = m_axis->parentPlot()->plotLayout()->margins();
        // Use the current viewport, since the old axis rect can still have its pre-resize size.
        const double width = qMax(1.0, m_axis->parentPlot()->viewport().width()
                                          - axisMargins.left() - axisMargins.right()
                                          - layoutMargins.left() - layoutMargins.right()
                                          - metrics.horizontalAdvance("00"));
        QVector<QString> labels;
        for (int count = 7; count >= 1; --count) {
            setTickCount(count);
            QCPAxisTickerDateTime::generate(range, locale, formatChar, precision, ticks,
                                            subTicks, &labels);
            bool fits = true;
            double previousWidth = 0;
            for (qsizetype i = 0; i < labels.size(); ++i) {
                double labelWidth = 0;
                for (const auto &line : labels[i].split('\n'))
                    labelWidth = qMax(labelWidth, metrics.horizontalAdvance(line));
                if (i > 0 && (ticks[i] - ticks[i - 1]) * width / range.size()
                                 < (previousWidth + labelWidth) / 2 + 16) {
                    fits = false;
                    break;
                }
                previousWidth = labelWidth;
            }
            if (fits)
                break;
        }
        if (tickLabels)
            *tickLabels = labels;
    }

private:
    QPointer<QCPAxis> m_axis;
};

double historyGapThreshold(const QVector<double> &times)
{
    QVector<double> intervals;
    for (qsizetype i = 1; i < times.size(); ++i) {
        const double interval = times[i] - times[i - 1];
        // Polling is configurable up to 60 s. Longer gaps must not set the cadence.
        if (interval > 0 && interval <= 60.001)
            intervals.append(interval);
    }
    std::sort(intervals.begin(), intervals.end());
    const double cadence = intervals.isEmpty() ? 0.5 : intervals[intervals.size() / 2];
    return qMax(5.0, cadence * 3);
}

void insertHistoryGaps(QVector<double> &times, QVector<double> &temperatures,
                       QVector<double> &humidities)
{
    const double threshold = historyGapThreshold(times);
    QVector<double> separatedTimes, separatedTemperatures, separatedHumidities;
    for (qsizetype i = 0; i < times.size(); ++i) {
        if (i > 0 && times[i] - times[i - 1] > threshold) {
            separatedTimes.append(times[i - 1] + (times[i] - times[i - 1]) / 2);
            separatedTemperatures.append(std::numeric_limits<double>::quiet_NaN());
            separatedHumidities.append(std::numeric_limits<double>::quiet_NaN());
        }
        separatedTimes.append(times[i]);
        separatedTemperatures.append(temperatures[i]);
        separatedHumidities.append(humidities[i]);
    }
    times.swap(separatedTimes);
    temperatures.swap(separatedTemperatures);
    humidities.swap(separatedHumidities);
}
}

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
        m_plot->graph(i * 2)->removeFromLegend();
        m_plot->addGraph();
        m_plot->graph(i * 2 + 1)->setName(QString("模块%1 湿度").arg(i + 1));
        m_plot->graph(i * 2 + 1)->setPen(QPen(humidityColors[i], 1, Qt::DashLine));
        m_plot->graph(i * 2 + 1)->removeFromLegend();
    }
    // Keep isolated samples visible without thickening densely sampled lines.
    for (int i = 0; i < 8; ++i) {
        auto *points = m_plot->addGraph();
        points->setPen(QPen(m_plot->graph(i)->pen().color(), 1));
        points->setLineStyle(QCPGraph::lsNone);
        points->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDisc, 3));
        points->removeFromLegend();
        points->setVisible(false);
    }
    m_plot->axisRect()->insetLayout()->take(m_plot->legend);
    m_plot->legend->setFillOrder(QCPLayoutGrid::foColumnsFirst);
    m_plot->legend->setWrap(4);
    m_plot->legend->setBorderPen(QPen(Qt::NoPen));
    m_plot->legend->setVisible(false);
    m_plot->xAxis->setLabel("时间（本地）");
    m_plot->yAxis->setLabel("温度/湿度");
    auto dateTimeTicker = QSharedPointer<HistoryTimeTicker>::create(m_plot->xAxis);
    m_plot->xAxis->setTicker(dateTimeTicker);

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
            [this](int, const QVariantList &rows, bool more, const QString &error) {
        m_queryButton->setEnabled(true);
        m_startEdit->setEnabled(true); m_endEdit->setEnabled(true); m_sensorCombo->setEnabled(true);
        m_previous->setEnabled(m_offset > 0); m_next->setEnabled(more);
        if (!error.isEmpty()) {
            m_pageLabel->setText("查询失败");
            QMessageBox::warning(this, "数据库操作失败", error); return;
        }
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
        m_table->setItem(row, 0, new QTableWidgetItem(time.toLocalTime().toString("yyyy-MM-dd HH:mm:ss.zzz")));
        for (int column = 1; column < 3; ++column)
            m_table->setItem(row, column, new QTableWidgetItem(fields.at(column).toString()));
        for (int column = 3; column < 5; ++column)
            m_table->setItem(row, column, new QTableWidgetItem(QString::number(fields.at(column).toDouble(), 'f', 2)));
        const int rowSensorIndex = fields.at(5).toInt();
        if (rowSensorIndex < 0 || rowSensorIndex >= 4 || !time.isValid())
            continue;
        timeValues[rowSensorIndex].append(time.toMSecsSinceEpoch() / 1000.0);
        temperatures[rowSensorIndex].append(fields.at(3).toDouble());
        humidities[rowSensorIndex].append(fields.at(4).toDouble());
    }

    bool hasAnyData = false;
    for (int i = 0; i < 4; ++i) {
        const bool hasData = !timeValues[i].isEmpty();
        insertHistoryGaps(timeValues[i], temperatures[i], humidities[i]);
        QVector<double> isolatedTimes, isolatedTemperatures, isolatedHumidities;
        for (qsizetype j = 0; j < timeValues[i].size(); ++j) {
            const bool previousGap = j == 0 || qIsNaN(temperatures[i][j - 1]);
            const bool nextGap = j + 1 == timeValues[i].size() || qIsNaN(temperatures[i][j + 1]);
            if (!qIsNaN(temperatures[i][j]) && previousGap && nextGap) {
                isolatedTimes.append(timeValues[i][j]);
                isolatedTemperatures.append(temperatures[i][j]);
                isolatedHumidities.append(humidities[i][j]);
            }
        }
        auto *temperatureGraph = m_plot->graph(i * 2);
        auto *humidityGraph = m_plot->graph(i * 2 + 1);
        auto *temperaturePoints = m_plot->graph(8 + i * 2);
        auto *humidityPoints = m_plot->graph(8 + i * 2 + 1);
        temperatureGraph->setData(timeValues[i], temperatures[i]);
        humidityGraph->setData(timeValues[i], humidities[i]);
        temperaturePoints->setData(isolatedTimes, isolatedTemperatures);
        humidityPoints->setData(isolatedTimes, isolatedHumidities);
        const bool selected = sensorIndex < 0 || sensorIndex == i;
        temperatureGraph->setVisible(selected && hasData);
        humidityGraph->setVisible(selected && hasData);
        temperaturePoints->setVisible(selected && !isolatedTimes.isEmpty());
        humidityPoints->setVisible(selected && !isolatedTimes.isEmpty());
        temperatureGraph->removeFromLegend();
        humidityGraph->removeFromLegend();
        if (selected && hasData) {
            temperatureGraph->addToLegend();
            humidityGraph->addToLegend();
        }
        if (selected && hasData)
            hasAnyData = true;
    }
    m_plot->legend->setVisible(hasAnyData);
    // An empty legend's maximum width is almost zero. Leaving it in the same grid column
    // would also squeeze the axis rect to its minimum width, piling up the time labels.
    if (hasAnyData && !m_plot->legend->layout()) {
        m_plot->plotLayout()->addElement(1, 0, m_plot->legend);
        m_plot->plotLayout()->setRowStretchFactor(1, 0.001);
    } else if (!hasAnyData && m_plot->legend->layout() == m_plot->plotLayout()) {
        m_plot->plotLayout()->take(m_plot->legend);
        m_plot->plotLayout()->simplify();
    }
    if (hasAnyData) {
        m_plot->rescaleAxes(true);
        m_plot->xAxis->scaleRange(1.02, m_plot->xAxis->range().center());
        m_plot->yAxis->scaleRange(1.10, m_plot->yAxis->range().center());
    } else {
        const double now = QDateTime::currentMSecsSinceEpoch() / 1000.0;
        m_plot->xAxis->setRange(now - 60, now);
        m_plot->yAxis->setRange(0, 100);
    }
    m_plot->replot();
}
