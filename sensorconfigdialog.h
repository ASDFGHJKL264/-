#ifndef SENSORCONFIGDIALOG_H
#define SENSORCONFIGDIALOG_H

#include <QDialog>

class AppConfig;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

class SensorConfigDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit SensorConfigDialog(const AppConfig &config, QWidget *parent = nullptr);
    void applyTo(AppConfig &config) const;

public slots:
    void accept() override;

private:
    bool validateInput(QString *errorMessage) const;

    static constexpr int SensorLimit = 4;
    QCheckBox *m_enabledEdits[SensorLimit]{};
    QLineEdit *m_nameEdits[SensorLimit]{};
    QSpinBox *m_slaveEdits[SensorLimit]{};
    QComboBox *m_typeEdits[SensorLimit]{};
    QSpinBox *m_temperatureEdits[SensorLimit]{};
    QSpinBox *m_humidityEdits[SensorLimit]{};
};

#endif
