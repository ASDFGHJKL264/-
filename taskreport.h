#pragma once
#include <QVariantMap>
#include <QString>

// Formats a completed task snapshot; does not query or modify the database.
namespace TaskReport {
QString toCsv(const QVariantMap &task, const QVariantList &zones);
}
