#pragma once
#include <QObject>
#include <QThread>

class AsyncLogger final : public QObject {
    Q_OBJECT
public:
    explicit AsyncLogger(const QString &directory, QObject *parent = nullptr);
    ~AsyncLogger() override;
    void append(const QString &line);
    quint64 dropped() const { return m_dropped; }
signals:
    void error(const QString &message);
private:
    QThread m_thread;
    QObject *m_worker;
    QString m_directory;
    int m_pending = 0;
    quint64 m_dropped = 0;
};
