#include "asynclogger.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

AsyncLogger::AsyncLogger(const QString &directory, QObject *parent)
    : QObject(parent), m_worker(new QObject), m_directory(directory)
{
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.start();
}
AsyncLogger::~AsyncLogger()
{
    QMetaObject::invokeMethod(m_worker, [] { QThread::currentThread()->quit(); });
    m_thread.wait();
}
void AsyncLogger::append(const QString &line)
{
    if (m_pending >= 256) { ++m_dropped; return; }
    ++m_pending;
    QMetaObject::invokeMethod(m_worker, [this, directory = m_directory, line] {
        QDir dir(directory);
        bool ok = dir.mkpath(".");
        const auto path = dir.filePath("application.log");
        if (ok && QFileInfo(path).size() >= 5 * 1024 * 1024) {
            ok = !QFile::exists(path + ".9") || QFile::remove(path + ".9");
            for (int i = 8; ok && i >= 1; --i)
                if (QFile::exists(path + "." + QString::number(i)))
                    ok = QFile::rename(path + "." + QString::number(i), path + "." + QString::number(i + 1));
            if (ok) ok = QFile::rename(path, path + ".1");
        }
        QFile file(path);
        if (ok) {
            const QByteArray bytes = (line + '\n').toUtf8();
            ok = file.open(QIODevice::WriteOnly | QIODevice::Append)
                && file.write(bytes) == bytes.size() && file.flush();
        }
        QMetaObject::invokeMethod(this, [this, ok] {
            --m_pending;
            if (!ok) emit error("文件日志写入或轮转失败，本条未保存");
        });
    });
}
