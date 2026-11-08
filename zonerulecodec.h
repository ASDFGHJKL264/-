#pragma once
#include "monitorcore.h"
#include <QVariantMap>

// Shared representation for persistence and controller/view messages.
namespace ZoneRuleCodec {
QVariantMap encode(const ZoneRule &rule);
ZoneRule decode(const QVariantMap &map);
}
