#ifndef SENSORUTILS_H
#define SENSORUTILS_H

#include <QtGlobal>

class QByteArray;

namespace SensorUtils {

quint16 modbusCrc16(const QByteArray &data);
double parseTemperature(quint16 rawValue);
bool isOutsideThreshold(double value, double minimum, double maximum);

}

#endif
