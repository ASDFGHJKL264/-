#pragma once
#include <QObject>
#include <QThread>
#include <QVariantList>

class HistoryQuery final : public QObject {
    Q_OBJECT
public:
    enum Operation { Samples, Alarms, ClearAlarms };
    static constexpr int PageSize = 500;
    HistoryQuery(const QString &databasePath, QObject *parent = nullptr);
    ~HistoryQuery() override;
    bool run(Operation operation, const QString &from = {}, const QString &to = {},
             int sensor = -1, int offset = 0);
signals:
    void finished(int operation, const QVariantList &rows, bool hasMore, const QString &error);
private:
    QThread m_thread;
    QObject *m_worker;
    QString m_path;
    bool m_busy = false;
};
