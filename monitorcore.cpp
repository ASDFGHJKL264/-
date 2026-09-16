#include "monitorcore.h"
#include <cmath>
#include <algorithm>

bool ZoneRule::valid() const
{
    return !name.trimmed().isEmpty() && name.size() <= 64 && std::isfinite(tempMin) &&
           std::isfinite(tempMax) && std::isfinite(humMin) && std::isfinite(humMax) &&
           std::isfinite(tempHysteresis) && std::isfinite(humHysteresis) && tempMin >= -100 &&
           tempMax <= 200 && tempMin < tempMax && humMin >= 0 && humMax <= 100 && humMin < humMax &&
           tempHysteresis >= 0 && tempHysteresis * 2 < tempMax - tempMin && humHysteresis >= 0 &&
           humHysteresis * 2 < humMax - humMin && holdMs >= 0 && holdMs <= 3600000 &&
           recoveryMs >= 0 && recoveryMs <= 3600000 && staleMs >= 1000 && staleMs <= 600000;
}

void ZoneMonitor::invalidate()
{
    validData = false;
    for (auto &c : channels) {
        c.pendingSince = c.recoverySince = -1;
        c.pendingDirection = 0;
    } // Active incidents remain active when data is missing.
}
void ZoneMonitor::tick(qint64 now)
{
    if (lastSample < 0 || now - lastSample > rule.staleMs)
        invalidate();
}

QVector<AlarmTransition> ZoneMonitor::sample(qint64 now, double t, double h)
{
    QVector<AlarmTransition> changes;
    if (!std::isfinite(t) || !std::isfinite(h) || t < -100 || t > 200 || h < 0 || h > 100) {
        invalidate();
        return changes;
    }
    tick(now);
    lastSample = now;
    validData = true;
    if (!rule.monitored)
        return changes;
    const double values[] = {t, h}, minimum[] = {rule.tempMin, rule.humMin};
    const double maximum[] = {rule.tempMax, rule.humMax};
    const double hysteresis[] = {rule.tempHysteresis, rule.humHysteresis};
    for (int i = 0; i < 2; ++i) {
        auto &c = channels[i];
        const int direction = values[i] < minimum[i] ? -1 : values[i] > maximum[i] ? 1 : 0;
        if (!c.active) {
            if (!direction) {
                c.pendingSince = -1;
                c.pendingDirection = 0;
                continue;
            }
            if (c.pendingSince < 0 || c.pendingDirection != direction) {
                c.pendingSince = now;
                c.pendingDirection = direction;
            }
            if (now - c.pendingSince >= rule.holdMs) {
                c.active = true;
                c.direction = direction;
                c.pendingSince = -1;
                changes.append({AlarmTransition::Raised, i, direction, values[i]});
            }
        } else {
            // Both sides of the normal band must be satisfied, even after a sudden opposite
            // excursion.
            const bool normal =
                values[i] >= minimum[i] + hysteresis[i] && values[i] <= maximum[i] - hysteresis[i];
            if (!normal) {
                c.recoverySince = -1;
                continue;
            }
            if (c.recoverySince < 0)
                c.recoverySince = now;
            if (now - c.recoverySince >= rule.recoveryMs) {
                changes.append({AlarmTransition::Recovered, i, c.direction, values[i]});
                c = Channel{};
            }
        }
    }
    return changes;
}

QString ZoneMonitor::status() const
{
    if (!rule.monitored)
        return QStringLiteral("暂停监控");
    if (!validData)
        return lastSample < 0 ? QStringLiteral("等待数据") : QStringLiteral("数据失效/离线");
    for (const auto &c : channels)
        if (c.active)
            return QStringLiteral("报警中");
    for (const auto &c : channels)
        if (c.pendingSince >= 0)
            return QStringLiteral("待确认超限");
    return QStringLiteral("正常");
}

void TaskStats::accrue(qint64 now, int staleMs)
{
    if (lastTime < 0)
        return;
    const auto duration = std::clamp<qint64>(now - lastTime, 0, staleMs);
    if (previousTempExceeded)
        tempExceededMs += duration;
    if (previousHumExceeded)
        humExceededMs += duration;
    lastTime = -1; // The next observation must explicitly start a new hold interval.
}
void TaskStats::observe(qint64 offset, int interval, const ZoneRule &r, double t, double h)
{
    accrue(offset, r.staleMs);
    const auto slot = offset / qMax(1, interval);
    if (slot != lastSlot) {
        ++occupiedSlots;
        lastSlot = slot;
    }
    if (samples++ == 0) {
        tempMin = tempMax = t;
        humMin = humMax = h;
    } else {
        tempMin = qMin(tempMin, t);
        tempMax = qMax(tempMax, t);
        humMin = qMin(humMin, h);
        humMax = qMax(humMax, h);
    }
    lastTime = offset;
    previousTempExceeded = t < r.tempMin || t > r.tempMax;
    previousHumExceeded = h < r.humMin || h > r.humMax;
}
void TaskStats::invalidate(qint64 now, int staleMs)
{
    accrue(now, staleMs);
}
qint64 TaskStats::expected(qint64 duration, int interval) const
{
    return qMax(occupiedSlots,
                qMax<qint64>(1, (duration + qMax(1, interval) - 1) / qMax(1, interval)));
}
