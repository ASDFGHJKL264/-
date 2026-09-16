#pragma once
#include <QString>
#include <QVector>
#include <array>

struct ZoneRule {
    QString name;
    bool monitored = true;
    double tempMin = -20, tempMax = 60, humMin = 10, humMax = 90;
    double tempHysteresis = 1, humHysteresis = 2;
    int holdMs = 10000, recoveryMs = 5000, staleMs = 5000;
    bool valid() const;
};

struct AlarmTransition {
    enum Kind { Raised, Recovered } kind;
    int metric;    // 0 temperature, 1 humidity
    int direction; // -1 low, +1 high
    double value;
};

// Pure business state: timestamps are monotonic milliseconds, not wall-clock time.
class ZoneMonitor
{
  public:
    ZoneRule rule;
    struct Channel {
        bool active = false;
        int direction = 0, pendingDirection = 0;
        qint64 pendingSince = -1, recoverySince = -1;
    };
    std::array<Channel, 2> channels;
    qint64 lastSample = -1;
    bool validData = false;
    QVector<AlarmTransition> sample(qint64 now, double temperature, double humidity);
    void invalidate();
    void tick(qint64 now);
    QString status() const;
};

struct TaskStats {
    qint64 samples = 0, occupiedSlots = 0, lastSlot = -1, lastTime = -1;
    qint64 tempExceededMs = 0, humExceededMs = 0;
    bool previousTempExceeded = false, previousHumExceeded = false;
    double tempMin = 0, tempMax = 0, humMin = 0, humMax = 0;
    void accrue(qint64 now, int staleMs);
    void observe(qint64 offsetMs, int intervalMs, const ZoneRule &rule, double t, double h);
    void invalidate(qint64 now, int staleMs);
    qint64 expected(qint64 durationMs, int intervalMs) const;
};
