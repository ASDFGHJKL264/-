#include "sensorutils.h"

#include <QByteArray>

quint16 SensorUtils::modbusCrc16(const QByteArray &data)
{
    quint16 crc = 0xFFFF;
    for (const char byte : data) {
        crc ^= static_cast<quint8>(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x0001) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

double SensorUtils::parseTemperature(quint16 rawValue)
{
    return rawValue >= 10000 ? -(rawValue - 10000) * 0.1 : rawValue * 0.1;
}

bool SensorUtils::isOutsideThreshold(double value, double minimum, double maximum)
{
    return value < minimum || value > maximum;
}
