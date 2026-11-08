#include "taskreport.h"
#include <QJsonDocument>

namespace {
QString csvText(QString s)
{
    if (!s.isEmpty() && (QString("=+-@").contains(s.front()) || s.front().isSpace()))
        s.prepend('\'');
    s.replace('"', "\"\"");
    return '"' + s + '"';
}
}

QString TaskReport::toCsv(const QVariantMap &t, const QVariantList &zones)
{
    const auto id = t["id"].toLongLong();
    QString csv =
        QString(QChar(0xfeff)) +
        QStringLiteral(
            "任务ID,任务名称,状态,数据源,开始UTC,结束UTC,区域,有效样本数,计划时间槽,有效时间槽,缺失时间槽,覆盖率%,最低温度,最高温度,最低湿度,最高湿度,温度超限估算秒,湿度超限估算秒,规则快照\r\n");
    const auto duration = t["elapsed_ms"].toLongLong();
    const auto interval = qMax(1, t["interval_ms"].toInt());
    for (const auto &item : zones) {
        auto r = item.toMap();
        auto rule = QJsonDocument::fromJson(r["config"].toString().toUtf8()).toVariant().toMap();
        auto valid = r["slots"].toLongLong();
        auto expected = qMax(valid, qMax<qint64>(1, (duration + interval - 1) / interval));
        QStringList fields = {QString::number(id),
                              csvText(t["name"].toString()),
                              csvText(t["status"].toString()),
                              t["simulator"].toBool() ? "模拟" : "硬件",
                              csvText(t["started_at"].toString()),
                              csvText(t["ended_at"].toString()),
                              csvText(rule["name"].toString()),
                              r["samples"].toString(),
                              QString::number(expected),
                              QString::number(valid),
                              QString::number(expected - valid),
                              QString::number(valid * 100.0 / expected, 'f', 2),
                              r["temp_min"].toString(),
                              r["temp_max"].toString(),
                              r["hum_min"].toString(),
                              r["hum_max"].toString(),
                              QString::number(r["temp_exceeded_ms"].toLongLong() / 1000.0, 'f', 3),
                              QString::number(r["hum_exceeded_ms"].toLongLong() / 1000.0, 'f', 3),
                              csvText(r["config"].toString())};
        csv += fields.join(',') + "\r\n";
    }
    csv +=
        "\r\n说明,缺失时间槽不计为正常；超限时长采用末值保持且最多保持至失效阈值，仅为估算；中断任务统计截止最后持久化点。\r\n";
    return csv;
}
