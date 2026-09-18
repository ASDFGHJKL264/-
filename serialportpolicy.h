#pragma once

#include <QStringList>

namespace SerialPortPolicy {
// Preserve a manual choice; configuration is only a fallback.
inline QString preferred(const QStringList &available, const QString &current,
                         const QString &configured, bool simulator)
{
    if (simulator)
        return QStringLiteral("SIMULATOR");
    if (available.contains(current))
        return current;
    if (available.contains(configured))
        return configured;
    return available.isEmpty() ? QString() : available.first();
}

inline QString savedPort(const QStringList &available, const QString &selected,
                         const QString &previous, bool simulator)
{
    return !simulator && !selected.isEmpty() && available.contains(selected)
               ? selected : previous;
}
} // namespace SerialPortPolicy
