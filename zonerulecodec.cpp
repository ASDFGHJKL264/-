#include "zonerulecodec.h"

QVariantMap ZoneRuleCodec::encode(const ZoneRule &r)
{
    return {{"name", r.name},
            {"monitored", r.monitored},
            {"tempMin", r.tempMin},
            {"tempMax", r.tempMax},
            {"humMin", r.humMin},
            {"humMax", r.humMax},
            {"tempHysteresis", r.tempHysteresis},
            {"humHysteresis", r.humHysteresis},
            {"holdMs", r.holdMs},
            {"recoveryMs", r.recoveryMs},
            {"staleMs", r.staleMs}};
}
ZoneRule ZoneRuleCodec::decode(const QVariantMap &m)
{
    ZoneRule r;
    r.name = m.value("name").toString();
    r.monitored = m.value("monitored", true).toBool();
    r.tempMin = m.value("tempMin", -20).toDouble();
    r.tempMax = m.value("tempMax", 60).toDouble();
    r.humMin = m.value("humMin", 10).toDouble();
    r.humMax = m.value("humMax", 90).toDouble();
    r.tempHysteresis = m.value("tempHysteresis", 1).toDouble();
    r.humHysteresis = m.value("humHysteresis", 2).toDouble();
    r.holdMs = m.value("holdMs", 10000).toInt();
    r.recoveryMs = m.value("recoveryMs", 5000).toInt();
    r.staleMs = m.value("staleMs", 5000).toInt();
    return r;
}
